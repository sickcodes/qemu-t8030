/*
 * QEMU NVM Express Virtual Namespace
 *
 * Copyright (c) 2019 CNEX Labs
 * Copyright (c) 2020 Samsung Electronics
 *
 * Authors:
 *  Klaus Jensen      <k.jensen@samsung.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/block/block.h"
#include "hw/pci/pci.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"

#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"

#include "trace.h"
#include "nvme.h"
#include "nvme-ns.h"

#define MIN_DISCARD_GRANULARITY (4 * KiB)

void nvme_ns_init_format(NvmeNamespace *ns)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    BlockDriverInfo bdi;
    int npdg, nlbas, ret;

    nlbas = nvme_ns_nlbas(ns);

    id_ns->nsze = cpu_to_le64(nlbas);

    /* no thin provisioning */
    id_ns->ncap = id_ns->nsze;
    id_ns->nuse = id_ns->ncap;
    id_ns->nstype = ns->params.nstype;

    ns->mdata_offset = nvme_l2b(ns, nlbas);

    npdg = ns->blkconf.discard_granularity / nvme_lsize(ns);

    ret = bdrv_get_info(blk_bs(ns->blkconf.blk), &bdi);
    if (ret >= 0 && bdi.cluster_size > ns->blkconf.discard_granularity) {
        npdg = bdi.cluster_size / nvme_lsize(ns);
    }

    id_ns->npda = id_ns->npdg = npdg - 1;
}

static int nvme_ns_init(NvmeNamespace *ns, Error **errp)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    uint8_t ds;
    uint16_t ms;
    int i;

    ns->csi = NVME_CSI_NVM;
    ns->status = 0x0;

    ns->id_ns.dlfeat = 0x1;

    /* support DULBE and I/O optimization fields */
    id_ns->nsfeat |= (0x4 | 0x10);

    if (ns->params.shared) {
        id_ns->nmic |= NVME_NMIC_NS_SHARED;
    }

    /* simple copy */
    id_ns->mssrl = cpu_to_le16(ns->params.mssrl);
    id_ns->mcl = cpu_to_le32(ns->params.mcl);
    id_ns->msrc = ns->params.msrc;

    ds = 31 - clz32(ns->blkconf.logical_block_size);
    ms = ns->params.ms;

    if (ns->params.ms) {
        id_ns->mc = 0x3;

        if (ns->params.mset) {
            id_ns->flbas |= 0x10;
        }

        id_ns->dpc = 0x1f;
        id_ns->dps = ((ns->params.pil & 0x1) << 3) | ns->params.pi;

        NvmeLBAF lbaf[16] = {
            [0] = { .ds =  9           },
            [1] = { .ds =  9, .ms =  8 },
            [2] = { .ds =  9, .ms = 16 },
            [3] = { .ds =  9, .ms = 64 },
            [4] = { .ds = 12           },
            [5] = { .ds = 12, .ms =  8 },
            [6] = { .ds = 12, .ms = 16 },
            [7] = { .ds = 12, .ms = 64 },
        };

        memcpy(&id_ns->lbaf, &lbaf, sizeof(lbaf));
        id_ns->nlbaf = 7;
    } else {
        NvmeLBAF lbaf[16] = {
            [0] = { .ds =  9 },
            [1] = { .ds = 12 },
        };

        memcpy(&id_ns->lbaf, &lbaf, sizeof(lbaf));
        id_ns->nlbaf = 1;
    }

    for (i = 0; i <= id_ns->nlbaf; i++) {
        NvmeLBAF *lbaf = &id_ns->lbaf[i];
        if (lbaf->ds == ds) {
            if (lbaf->ms == ms) {
                id_ns->flbas |= i;
                goto lbaf_found;
            }
        }
    }

    /* add non-standard lba format */
    id_ns->nlbaf++;
    id_ns->lbaf[id_ns->nlbaf].ds = ds;
    id_ns->lbaf[id_ns->nlbaf].ms = ms;
    id_ns->flbas |= id_ns->nlbaf;

lbaf_found:
    nvme_ns_init_format(ns);

    return 0;
}

static int nvme_ns_init_blk(NvmeNamespace *ns, Error **errp)
{
    bool read_only;

    if (!blkconf_blocksizes(&ns->blkconf, errp)) {
        return -1;
    }

    read_only = !blk_supports_write_perm(ns->blkconf.blk);
    if (!blkconf_apply_backend_options(&ns->blkconf, read_only, false, errp)) {
        return -1;
    }

    if (ns->blkconf.discard_granularity == -1) {
        ns->blkconf.discard_granularity =
            MAX(ns->blkconf.logical_block_size, MIN_DISCARD_GRANULARITY);
    }

    ns->size = blk_getlength(ns->blkconf.blk);
    if (ns->size < 0) {
        error_setg_errno(errp, -ns->size, "could not get blockdev size");
        return -1;
    }

    return 0;
}

static int nvme_ns_zoned_check_calc_geometry(NvmeNamespace *ns, Error **errp)
{
    uint64_t zone_size, zone_cap;
    uint32_t lbasz = nvme_lsize(ns);

    /* Make sure that the values of ZNS properties are sane */
    if (ns->params.zone_size_bs) {
        zone_size = ns->params.zone_size_bs;
    } else {
        zone_size = NVME_DEFAULT_ZONE_SIZE;
    }
    if (ns->params.zone_cap_bs) {
        zone_cap = ns->params.zone_cap_bs;
    } else {
        zone_cap = zone_size;
    }
    if (zone_cap > zone_size) {
        error_setg(errp, "zone capacity %"PRIu64"B exceeds "
                   "zone size %"PRIu64"B", zone_cap, zone_size);
        return -1;
    }
    if (zone_size < lbasz) {
        error_setg(errp, "zone size %"PRIu64"B too small, "
                   "must be at least %"PRIu32"B", zone_size, lbasz);
        return -1;
    }
    if (zone_cap < lbasz) {
        error_setg(errp, "zone capacity %"PRIu64"B too small, "
                   "must be at least %"PRIu32"B", zone_cap, lbasz);
        return -1;
    }

    /*
     * Save the main zone geometry values to avoid
     * calculating them later again.
     */
    ns->zone_size = zone_size / lbasz;
    ns->zone_capacity = zone_cap / lbasz;
    ns->num_zones = nvme_ns_nlbas(ns) / ns->zone_size;

    /* Do a few more sanity checks of ZNS properties */
    if (!ns->num_zones) {
        error_setg(errp,
                   "insufficient drive capacity, must be at least the size "
                   "of one zone (%"PRIu64"B)", zone_size);
        return -1;
    }

    if (ns->params.max_open_zones > ns->num_zones) {
        error_setg(errp,
                   "max_open_zones value %u exceeds the number of zones %u",
                   ns->params.max_open_zones, ns->num_zones);
        return -1;
    }
    if (ns->params.max_active_zones > ns->num_zones) {
        error_setg(errp,
                   "max_active_zones value %u exceeds the number of zones %u",
                   ns->params.max_active_zones, ns->num_zones);
        return -1;
    }

    if (ns->params.max_active_zones) {
        if (ns->params.max_open_zones > ns->params.max_active_zones) {
            error_setg(errp, "max_open_zones (%u) exceeds max_active_zones (%u)",
                       ns->params.max_open_zones, ns->params.max_active_zones);
            return -1;
        }

        if (!ns->params.max_open_zones) {
            ns->params.max_open_zones = ns->params.max_active_zones;
        }
    }

    if (ns->params.zd_extension_size) {
        if (ns->params.zd_extension_size & 0x3f) {
            error_setg(errp,
                "zone descriptor extension size must be a multiple of 64B");
            return -1;
        }
        if ((ns->params.zd_extension_size >> 6) > 0xff) {
            error_setg(errp, "zone descriptor extension size is too large");
            return -1;
        }
    }

    return 0;
}

static void nvme_ns_zoned_init_state(NvmeNamespace *ns)
{
    uint64_t start = 0, zone_size = ns->zone_size;
    uint64_t capacity = ns->num_zones * zone_size;
    NvmeZone *zone;
    int i;

    ns->zone_array = g_new0(NvmeZone, ns->num_zones);
    if (ns->params.zd_extension_size) {
        ns->zd_extensions = g_malloc0(ns->params.zd_extension_size *
                                      ns->num_zones);
    }

    QTAILQ_INIT(&ns->exp_open_zones);
    QTAILQ_INIT(&ns->imp_open_zones);
    QTAILQ_INIT(&ns->closed_zones);
    QTAILQ_INIT(&ns->full_zones);

    zone = ns->zone_array;
    for (i = 0; i < ns->num_zones; i++, zone++) {
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        zone->d.zt = NVME_ZONE_TYPE_SEQ_WRITE;
        nvme_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
        zone->d.za = 0;
        zone->d.zcap = ns->zone_capacity;
        zone->d.zslba = start;
        zone->d.wp = start;
        zone->w_ptr = start;
        start += zone_size;
    }

    ns->zone_size_log2 = 0;
    if (is_power_of_2(ns->zone_size)) {
        ns->zone_size_log2 = 63 - clz64(ns->zone_size);
    }
}

static void nvme_ns_init_zoned(NvmeNamespace *ns)
{
    NvmeIdNsZoned *id_ns_z;
    int i;

    nvme_ns_zoned_init_state(ns);

    id_ns_z = g_malloc0(sizeof(NvmeIdNsZoned));

    /* MAR/MOR are zeroes-based, 0xffffffff means no limit */
    id_ns_z->mar = cpu_to_le32(ns->params.max_active_zones - 1);
    id_ns_z->mor = cpu_to_le32(ns->params.max_open_zones - 1);
    id_ns_z->zoc = 0;
    id_ns_z->ozcs = ns->params.cross_zone_read ? 0x01 : 0x00;

    for (i = 0; i <= ns->id_ns.nlbaf; i++) {
        id_ns_z->lbafe[i].zsze = cpu_to_le64(ns->zone_size);
        id_ns_z->lbafe[i].zdes =
            ns->params.zd_extension_size >> 6; /* Units of 64B */
    }

    ns->csi = NVME_CSI_ZONED;
    ns->id_ns.nsze = cpu_to_le64(ns->num_zones * ns->zone_size);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.ncap;

    /*
     * The device uses the BDRV_BLOCK_ZERO flag to determine the "deallocated"
     * status of logical blocks. Since the spec defines that logical blocks
     * SHALL be deallocated when then zone is in the Empty or Offline states,
     * we can only support DULBE if the zone size is a multiple of the
     * calculated NPDG.
     */
    if (ns->zone_size % (ns->id_ns.npdg + 1)) {
        warn_report("the zone size (%"PRIu64" blocks) is not a multiple of "
                    "the calculated deallocation granularity (%d blocks); "
                    "DULBE support disabled",
                    ns->zone_size, ns->id_ns.npdg + 1);

        ns->id_ns.nsfeat &= ~0x4;
    }

    ns->id_ns_zoned = id_ns_z;
}

static void nvme_clear_zone(NvmeNamespace *ns, NvmeZone *zone)
{
    uint8_t state;

    zone->w_ptr = zone->d.wp;
    state = nvme_get_zone_state(zone);
    if (zone->d.wp != zone->d.zslba ||
        (zone->d.za & NVME_ZA_ZD_EXT_VALID)) {
        if (state != NVME_ZONE_STATE_CLOSED) {
            trace_pci_nvme_clear_ns_close(state, zone->d.zslba);
            nvme_set_zone_state(zone, NVME_ZONE_STATE_CLOSED);
        }
        nvme_aor_inc_active(ns);
        QTAILQ_INSERT_HEAD(&ns->closed_zones, zone, entry);
    } else {
        trace_pci_nvme_clear_ns_reset(state, zone->d.zslba);
        nvme_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
    }
}

/*
 * Close all the zones that are currently open.
 */
static void nvme_zoned_ns_shutdown(NvmeNamespace *ns)
{
    NvmeZone *zone, *next;

    QTAILQ_FOREACH_SAFE(zone, &ns->closed_zones, entry, next) {
        QTAILQ_REMOVE(&ns->closed_zones, zone, entry);
        nvme_aor_dec_active(ns);
        nvme_clear_zone(ns, zone);
    }
    QTAILQ_FOREACH_SAFE(zone, &ns->imp_open_zones, entry, next) {
        QTAILQ_REMOVE(&ns->imp_open_zones, zone, entry);
        nvme_aor_dec_open(ns);
        nvme_aor_dec_active(ns);
        nvme_clear_zone(ns, zone);
    }
    QTAILQ_FOREACH_SAFE(zone, &ns->exp_open_zones, entry, next) {
        QTAILQ_REMOVE(&ns->exp_open_zones, zone, entry);
        nvme_aor_dec_open(ns);
        nvme_aor_dec_active(ns);
        nvme_clear_zone(ns, zone);
    }

    assert(ns->nr_open_zones == 0);
}

static int nvme_ns_check_constraints(NvmeCtrl *n, NvmeNamespace *ns,
                                     Error **errp)
{
    if (!ns->blkconf.blk) {
        error_setg(errp, "block backend not configured");
        return -1;
    }

    if (ns->params.pi && ns->params.ms < 8) {
        error_setg(errp, "at least 8 bytes of metadata required to enable "
                   "protection information");
        return -1;
    }

    if (ns->params.nsid > NVME_MAX_NAMESPACES) {
        error_setg(errp, "invalid namespace id (must be between 0 and %d)",
                   NVME_MAX_NAMESPACES);
        return -1;
    }

    if (!n->subsys) {
        if (ns->params.detached) {
            error_setg(errp, "detached requires that the nvme device is "
                       "linked to an nvme-subsys device");
            return -1;
        }

        if (ns->params.shared) {
            error_setg(errp, "shared requires that the nvme device is "
                       "linked to an nvme-subsys device");
            return -1;
        }
    }

    return 0;
}

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    if (nvme_ns_check_constraints(n, ns, errp)) {
        return -1;
    }

    if (nvme_ns_init_blk(ns, errp)) {
        return -1;
    }

    if (nvme_ns_init(ns, errp)) {
        return -1;
    }
    if (ns->params.zoned) {
        if (nvme_ns_zoned_check_calc_geometry(ns, errp) != 0) {
            return -1;
        }
        nvme_ns_init_zoned(ns);
    }

    return 0;
}

void nvme_ns_drain(NvmeNamespace *ns)
{
    blk_drain(ns->blkconf.blk);
}

void nvme_ns_shutdown(NvmeNamespace *ns)
{
    blk_flush(ns->blkconf.blk);
    if (ns->params.zoned) {
        nvme_zoned_ns_shutdown(ns);
    }
}

void nvme_ns_cleanup(NvmeNamespace *ns)
{
    if (ns->params.zoned) {
        g_free(ns->id_ns_zoned);
        g_free(ns->zone_array);
        g_free(ns->zd_extensions);
    }
}

static void nvme_ns_realize(DeviceState *dev, Error **errp)
{
    NvmeNamespace *ns = NVME_NS(dev);
    BusState *s = qdev_get_parent_bus(dev);
    NvmeCtrl *n = NVME(s->parent);
    NvmeSubsystem *subsys = n->subsys;
    uint32_t nsid = ns->params.nsid;
    int i;

    if (nvme_ns_setup(n, ns, errp)) {
        return;
    }

    if (!nsid) {
        for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
            if (nvme_ns(n, i) || nvme_subsys_ns(subsys, i)) {
                continue;
            }

            nsid = ns->params.nsid = i;
            break;
        }

        if (!nsid) {
            error_setg(errp, "no free namespace id");
            return;
        }
    } else {
        if (nvme_ns(n, nsid) || nvme_subsys_ns(subsys, nsid)) {
            error_setg(errp, "namespace id '%d' already allocated", nsid);
            return;
        }
    }

    if (subsys) {
        subsys->namespaces[nsid] = ns;

        if (ns->params.detached) {
            return;
        }

        if (ns->params.shared) {
            for (i = 0; i < ARRAY_SIZE(subsys->ctrls); i++) {
                NvmeCtrl *ctrl = subsys->ctrls[i];

                if (ctrl) {
                    nvme_attach_ns(ctrl, ns);
                }
            }

            return;
        }
    }

    nvme_attach_ns(n, ns);
}

static Property nvme_ns_props[] = {
    DEFINE_BLOCK_PROPERTIES(NvmeNamespace, blkconf),
    DEFINE_PROP_BOOL("detached", NvmeNamespace, params.detached, false),
    DEFINE_PROP_BOOL("shared", NvmeNamespace, params.shared, false),
    DEFINE_PROP_UINT32("nsid", NvmeNamespace, params.nsid, 0),
    DEFINE_PROP_UINT32("nstype", NvmeNamespace, params.nstype, 0),
    DEFINE_PROP_UUID("uuid", NvmeNamespace, params.uuid),
    DEFINE_PROP_UINT16("ms", NvmeNamespace, params.ms, 0),
    DEFINE_PROP_UINT8("mset", NvmeNamespace, params.mset, 0),
    DEFINE_PROP_UINT8("pi", NvmeNamespace, params.pi, 0),
    DEFINE_PROP_UINT8("pil", NvmeNamespace, params.pil, 0),
    DEFINE_PROP_UINT16("mssrl", NvmeNamespace, params.mssrl, 128),
    DEFINE_PROP_UINT32("mcl", NvmeNamespace, params.mcl, 128),
    DEFINE_PROP_UINT8("msrc", NvmeNamespace, params.msrc, 127),
    DEFINE_PROP_BOOL("zoned", NvmeNamespace, params.zoned, false),
    DEFINE_PROP_SIZE("zoned.zone_size", NvmeNamespace, params.zone_size_bs,
                     NVME_DEFAULT_ZONE_SIZE),
    DEFINE_PROP_SIZE("zoned.zone_capacity", NvmeNamespace, params.zone_cap_bs,
                     0),
    DEFINE_PROP_BOOL("zoned.cross_read", NvmeNamespace,
                     params.cross_zone_read, false),
    DEFINE_PROP_UINT32("zoned.max_active", NvmeNamespace,
                       params.max_active_zones, 0),
    DEFINE_PROP_UINT32("zoned.max_open", NvmeNamespace,
                       params.max_open_zones, 0),
    DEFINE_PROP_UINT32("zoned.descr_ext_size", NvmeNamespace,
                       params.zd_extension_size, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_ns_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->bus_type = TYPE_NVME_BUS;
    dc->realize = nvme_ns_realize;
    device_class_set_props(dc, nvme_ns_props);
    dc->desc = "Virtual NVMe namespace";
}

static void nvme_ns_instance_init(Object *obj)
{
    NvmeNamespace *ns = NVME_NS(obj);
    char *bootindex = g_strdup_printf("/namespace@%d,0", ns->params.nsid);

    device_add_bootindex_property(obj, &ns->bootindex, "bootindex",
                                  bootindex, DEVICE(obj));

    g_free(bootindex);
}

static const TypeInfo nvme_ns_info = {
    .name = TYPE_NVME_NS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_ns_class_init,
    .instance_size = sizeof(NvmeNamespace),
    .instance_init = nvme_ns_instance_init,
};

static void nvme_ns_register_types(void)
{
    type_register_static(&nvme_ns_info);
}

type_init(nvme_ns_register_types)
