/*
 * QEMU paravirtual RDMA
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* Minimal includes instead of qemu/osdep.h */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>
/* #include "qapi/error.h" - Not needed for standalone */
/* #include "qemu/module.h" - Not needed for standalone */
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h" /* For PVRDMA_DEV, OBJECT, PCI_SLOT, PCI_FUNC, etc. */
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
/* #include "hw/qdev-properties.h" - Not needed for standalone */
/* #include "hw/qdev-properties-system.h" - Not needed for standalone */
/* #include "cpu.h" - Not needed for PVRDMA */
/* #include "monitor/monitor.h" - Not needed for standalone */
#include "hw/rdma/rdma.h" /* Needed for rdma_pci_dma_map declaration */
#include "qom/object.h"   /* For object_get_typename */

#include "../rdma_rm.h"
#include "../rdma_backend.h"
#include "../rdma_utils.h"

#include <infiniband/verbs.h>
#include "pvrdma.h"
#include "standard-headers/rdma/vmw_pvrdma-abi.h"
/* #include "sysemu/runstate.h" - Not needed for standalone */
#include "standard-headers/drivers/infiniband/hw/vmw_pvrdma/pvrdma_dev_api.h"
#include "pvrdma_qp_ops.h"

/*
 * QEMU device properties - not used in standalone mode
 * Configuration is passed via command line to vfu_pvrdma instead
 */
#if 0
static Property pvrdma_dev_properties[] = {
    DEFINE_PROP_STRING("netdev", PVRDMADev, backend_eth_device_name),
    DEFINE_PROP_STRING("ibdev", PVRDMADev, backend_device_name),
    DEFINE_PROP_UINT8("ibport", PVRDMADev, backend_port_num, 1),
    DEFINE_PROP_UINT64("dev-caps-max-mr-size", PVRDMADev, dev_attr.max_mr_size,
                       MAX_MR_SIZE),
    DEFINE_PROP_INT32("dev-caps-max-qp", PVRDMADev, dev_attr.max_qp, MAX_QP),
    DEFINE_PROP_INT32("dev-caps-max-cq", PVRDMADev, dev_attr.max_cq, MAX_CQ),
    DEFINE_PROP_INT32("dev-caps-max-mr", PVRDMADev, dev_attr.max_mr, MAX_MR),
    DEFINE_PROP_INT32("dev-caps-max-pd", PVRDMADev, dev_attr.max_pd, MAX_PD),
    DEFINE_PROP_INT32("dev-caps-qp-rd-atom", PVRDMADev, dev_attr.max_qp_rd_atom,
                      MAX_QP_RD_ATOM),
    DEFINE_PROP_INT32("dev-caps-max-qp-init-rd-atom", PVRDMADev,
                      dev_attr.max_qp_init_rd_atom, MAX_QP_INIT_RD_ATOM),
    DEFINE_PROP_INT32("dev-caps-max-ah", PVRDMADev, dev_attr.max_ah, MAX_AH),
    DEFINE_PROP_INT32("dev-caps-max-srq", PVRDMADev, dev_attr.max_srq, MAX_SRQ),
    DEFINE_PROP_CHR("mad-chardev", PVRDMADev, mad_chr),
    DEFINE_PROP_END_OF_LIST(),
};
#endif /* Unused properties array */

/* Forward declaration */
typedef struct RdmaProvider RdmaProvider;

/* Get or create QP stats entry */
PVRDMAQPStats *pvrdma_get_qp_stats(PVRDMADev *dev, uint32_t qp_handle)
{
    PVRDMAQPStats *stats;

    if (!dev->stats.qp_stats) {
        return NULL;
    }

    stats = (PVRDMAQPStats *)g_hash_table_lookup(dev->stats.qp_stats,
                                                 GUINT_TO_POINTER(qp_handle));
    if (!stats) {
        stats = g_new0(PVRDMAQPStats, 1);
        g_hash_table_insert(dev->stats.qp_stats, GUINT_TO_POINTER(qp_handle),
                            stats);
    }

    return stats;
}

/* Write stats to file */
void pvrdma_write_stats_impl(PVRDMADev *dev)
{
    GHashTableIter iter;
    gpointer key, value;
    FILE *fp;
    const char *opcode_names[] = {"RDMA_WRITE",
                                  "RDMA_WRITE_WITH_IMM",
                                  "SEND",
                                  "SEND_WITH_IMM",
                                  "RDMA_READ",
                                  "ATOMIC_CMP_AND_SWP",
                                  "ATOMIC_FETCH_AND_ADD",
                                  "LSO",
                                  "SEND_WITH_INV",
                                  "RDMA_READ_WITH_INV",
                                  "LOCAL_INV",
                                  "FAST_REG_MR",
                                  "MASKED_ATOMIC_CMP_SWP",
                                  "MASKED_ATOMIC_FETCH",
                                  "BIND_MW",
                                  "REG_SIG_MR",
                                  "ERROR"};

    if (!dev->stats.stats_file) {
        return;
    }

    fp = fopen(dev->stats.stats_file, "w");
    if (!fp) {
        rdma_error_report("Failed to open stats file %s: %s",
                          dev->stats.stats_file, strerror(errno));
        return;
    }

    fprintf(fp, "=== ROCm ERNIC Statistics ===\n");
    fprintf(fp, "Write count: %" PRIu64 "\n\n", dev->stats.stats_write_count);
    fprintf(fp, "Device Statistics:\n");
    fprintf(fp, "  commands         : %" PRIu64 "\n", dev->stats.commands);
    fprintf(fp, "  regs_reads       : %" PRIu64 "\n", dev->stats.regs_reads);
    fprintf(fp, "  regs_writes      : %" PRIu64 "\n", dev->stats.regs_writes);
    fprintf(fp, "  uar_writes       : %" PRIu64 "\n", dev->stats.uar_writes);
    fprintf(fp, "  interrupts       : %" PRIu64 "\n", dev->stats.interrupts);
    fprintf(fp, "  total_bytes_sent : %" PRIu64 "\n",
            dev->stats.total_bytes_sent);
    fprintf(fp, "  total_bytes_received : %" PRIu64 "\n",
            dev->stats.total_bytes_received);
    fprintf(fp, "  total_bytes_rdma_read : %" PRIu64 "\n",
            dev->stats.total_bytes_rdma_read);
    fprintf(fp, "  total_bytes_rdma_write : %" PRIu64 "\n",
            dev->stats.total_bytes_rdma_write);
    fprintf(fp, "\n");

    fprintf(fp, "Per-QP Statistics:\n");
    if (dev->stats.qp_stats) {
        g_hash_table_iter_init(&iter, dev->stats.qp_stats);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            uint32_t qp_handle = GPOINTER_TO_UINT(key);
            PVRDMAQPStats *qp_stats = (PVRDMAQPStats *)value;
            uint64_t total_wqes = 0;
            int i;

            fprintf(fp, "  QP %u:\n", qp_handle);
            fprintf(fp, "    doorbell_send  : %" PRIu64 "\n",
                    qp_stats->doorbell_send);
            fprintf(fp, "    doorbell_recv  : %" PRIu64 "\n",
                    qp_stats->doorbell_recv);
            fprintf(fp, "    doorbell_srq   : %" PRIu64 "\n",
                    qp_stats->doorbell_srq);
            fprintf(fp, "    wqes_processed : %" PRIu64 "\n",
                    qp_stats->wqes_processed);
            fprintf(fp, "    cqes_posted    : %" PRIu64 "\n",
                    qp_stats->cqes_posted);
            fprintf(fp, "    continuations  : %" PRIu64 "\n",
                    qp_stats->continuations);
            fprintf(fp, "    bytes_sent     : %" PRIu64 "\n",
                    qp_stats->bytes_sent);
            fprintf(fp, "    bytes_received : %" PRIu64 "\n",
                    qp_stats->bytes_received);
            fprintf(fp, "    bytes_rdma_read : %" PRIu64 "\n",
                    qp_stats->bytes_rdma_read);
            fprintf(fp, "    bytes_rdma_write : %" PRIu64 "\n",
                    qp_stats->bytes_rdma_write);

            fprintf(fp, "    WQEs by opcode:\n");
            for (i = 0; i < 16 && i < (int)G_N_ELEMENTS(opcode_names); i++) {
                if (qp_stats->wqes_by_opcode[i] > 0) {
                    fprintf(fp, "      %-20s: %" PRIu64 "\n", opcode_names[i],
                            qp_stats->wqes_by_opcode[i]);
                    total_wqes += qp_stats->wqes_by_opcode[i];
                }
            }
            if (total_wqes != qp_stats->wqes_processed) {
                fprintf(fp, "      (other opcodes)    : %" PRIu64 "\n",
                        qp_stats->wqes_processed - total_wqes);
            }
            fprintf(fp, "\n");
        }
    } else {
        fprintf(fp, "  (no QPs created yet)\n\n");
    }

    fclose(fp);
    dev->stats.stats_write_count++;
}

static void __attribute__((unused)) pvrdma_format_statistics(RdmaProvider *obj,
                                                             GString *buf)
{
    PVRDMADev *dev = PVRDMA_DEV(obj);
    PCIDevice *pdev = PCI_DEVICE(dev);

    g_string_append_printf(buf, "%s, %x.%x\n", pdev->name, PCI_SLOT(pdev),
                           PCI_FUNC(pdev));
    g_string_append_printf(buf, "\tcommands         : %" PRId64 "\n",
                           dev->stats.commands);
    g_string_append_printf(buf, "\tregs_reads       : %" PRId64 "\n",
                           dev->stats.regs_reads);
    g_string_append_printf(buf, "\tregs_writes      : %" PRId64 "\n",
                           dev->stats.regs_writes);
    g_string_append_printf(buf, "\tuar_writes       : %" PRId64 "\n",
                           dev->stats.uar_writes);
    g_string_append_printf(buf, "\tinterrupts       : %" PRId64 "\n",
                           dev->stats.interrupts);
    rdma_format_device_counters(&dev->rdma_dev_res, buf);
}

static void free_dev_ring(PCIDevice *pci_dev, PvrdmaRing *ring,
                          void *ring_state)
{
    pvrdma_ring_free(ring);
    rdma_pci_dma_unmap(pci_dev, ring_state, PAGE_SIZE);
}

static int init_dev_ring(PvrdmaRing *ring, PvrdmaRingState **ring_state,
                         const char *name, PCIDevice *pci_dev,
                         dma_addr_t dir_addr, uint32_t num_pages)
{
    uint64_t *dir, *tbl;
    int max_pages, rc = 0;

    rdma_info_report("init_dev_ring: ENTER ring=%s num_pages=%u", name,
                     num_pages);

    if (!num_pages) {
        rdma_error_report("Ring pages count must be strictly positive");
        return -EINVAL;
    }

    /*
     * Make sure we can satisfy the requested number of pages in a single
     * PAGE_SIZE sized page table (taking into account that first entry
     * is reserved for ring-state)
     */
    max_pages = PAGE_SIZE / sizeof(dma_addr_t) - 1;
    if (num_pages > max_pages) {
        rdma_error_report(
            "Maximum pages on a single directory must not exceed %d\n",
            max_pages);
        return -EINVAL;
    }

    dir = rdma_pci_dma_map(pci_dev, dir_addr, PAGE_SIZE);
    if (!dir) {
        rdma_error_report("Failed to map to page directory (ring %s)", name);
        rc = -ENOMEM;
        goto out;
    }

    /* We support only one page table for a ring */
    tbl = rdma_pci_dma_map(pci_dev, dir[0], PAGE_SIZE);
    if (!tbl) {
        rdma_error_report("Failed to map to page table (ring %s)", name);
        rc = -ENOMEM;
        goto out_free_dir;
    }

    *ring_state = rdma_pci_dma_map(pci_dev, tbl[0], PAGE_SIZE);
    if (!*ring_state) {
        rdma_error_report("Failed to map to ring state (ring %s)", name);
        rc = -ENOMEM;
        goto out_free_tbl;
    }
    /* RX ring is the second */
    (*ring_state)++;
    rc = pvrdma_ring_init(ring, name, pci_dev, (PvrdmaRingState *)*ring_state,
                          (num_pages - 1) * PAGE_SIZE /
                              sizeof(struct pvrdma_cqne),
                          sizeof(struct pvrdma_cqne), (dma_addr_t *)&tbl[1],
                          (dma_addr_t)num_pages - 1);
    if (rc) {
        rc = -ENOMEM;
        goto out_free_ring_state;
    }

    goto out_free_tbl;

out_free_ring_state:
    rdma_pci_dma_unmap(pci_dev, *ring_state, PAGE_SIZE);

out_free_tbl:
    rdma_pci_dma_unmap(pci_dev, tbl, PAGE_SIZE);

out_free_dir:
    rdma_pci_dma_unmap(pci_dev, dir, PAGE_SIZE);

out:
    return rc;
}

void free_dsr(PVRDMADev *dev)
{
    PCIDevice *pci_dev = PCI_DEVICE(dev);

    if (!dev->dsr_info.dsr) {
        return;
    }

    free_dev_ring(pci_dev, &dev->dsr_info.async,
                  dev->dsr_info.async_ring_state);

    free_dev_ring(pci_dev, &dev->dsr_info.cq, dev->dsr_info.cq_ring_state);

    rdma_pci_dma_unmap(pci_dev, dev->dsr_info.req,
                       sizeof(union pvrdma_cmd_req));

    rdma_pci_dma_unmap(pci_dev, dev->dsr_info.rsp,
                       sizeof(union pvrdma_cmd_resp));

    rdma_pci_dma_unmap(pci_dev, dev->dsr_info.dsr,
                       sizeof(struct pvrdma_device_shared_region));

    dev->dsr_info.dsr = NULL;
}

static int load_dsr(PVRDMADev *dev)
{
    int rc = 0;
    PCIDevice *pci_dev = PCI_DEVICE(dev);
    DSRInfo *dsr_info;
    struct pvrdma_device_shared_region *dsr;

    free_dsr(dev);

    /* Map to DSR */
    rdma_info_report("load_dsr: About to map DSR at guest addr %#lx",
                     (uint64_t)dev->dsr_info.dma);
    void *dsr_ptr = rdma_pci_dma_map(
        pci_dev, dev->dsr_info.dma, sizeof(struct pvrdma_device_shared_region));

    /* Check as uint64_t to see actual value */
    uint64_t dsr_as_int = (uint64_t)(uintptr_t)dsr_ptr;
    rdma_info_report(
        "load_dsr: rdma_pci_dma_map returned ptr=%p as_uint64=%#lx", dsr_ptr,
        dsr_as_int);

    dev->dsr_info.dsr = dsr_ptr;
    uint64_t stored_as_int = (uint64_t)(uintptr_t)dev->dsr_info.dsr;
    rdma_info_report(
        "load_dsr: Stored in dev->dsr_info.dsr = %p as_uint64=%#lx",
        dev->dsr_info.dsr, stored_as_int);

    if (!dev->dsr_info.dsr) {
        rdma_error_report("Failed to map to DSR");
        rc = -ENOMEM;
        goto out;
    }

    /* Shortcuts */
    dsr_info = &dev->dsr_info;
    dsr = dsr_info->dsr;

    rdma_info_report("load_dsr: After shortcuts, dsr = %p", dsr);
    rdma_info_report("load_dsr: Reading cmd_slot_dma from DSR...");

    /* Map to command slot */
    rdma_info_report("load_dsr: cmd_slot_dma = %#lx",
                     (uint64_t)dsr->cmd_slot_dma);
    dsr_info->req = rdma_pci_dma_map(pci_dev, dsr->cmd_slot_dma,
                                     sizeof(union pvrdma_cmd_req));
    rdma_info_report("load_dsr: mapped cmd_slot to host ptr=%p (dma=%#lx)",
                     dsr_info->req, (uint64_t)dsr->cmd_slot_dma);
    if (!dsr_info->req) {
        rdma_error_report("Failed to map to command slot address");
        rc = -ENOMEM;
        goto out_free_dsr;
    }

    /* Map to response slot */
    rdma_info_report("load_dsr: resp_slot_dma = %#lx",
                     (uint64_t)dsr->resp_slot_dma);
    dsr_info->rsp = rdma_pci_dma_map(pci_dev, dsr->resp_slot_dma,
                                     sizeof(union pvrdma_cmd_resp));
    rdma_info_report("load_dsr: mapped resp_slot to host ptr=%p (dma=%#lx)",
                     dsr_info->rsp, (uint64_t)dsr->resp_slot_dma);
    if (!dsr_info->rsp) {
        rdma_error_report("Failed to map to response slot address");
        rc = -ENOMEM;
        goto out_free_req;
    }

    /* Map to CQ notification ring */
    rc = init_dev_ring(&dsr_info->cq, &dsr_info->cq_ring_state, "dev_cq",
                       pci_dev, dsr->cq_ring_pages.pdir_dma,
                       dsr->cq_ring_pages.num_pages);
    if (rc) {
        rc = -ENOMEM;
        goto out_free_rsp;
    }

    /* Map to event notification ring */
    rc = init_dev_ring(&dsr_info->async, &dsr_info->async_ring_state,
                       "dev_async", pci_dev, dsr->async_ring_pages.pdir_dma,
                       dsr->async_ring_pages.num_pages);
    if (rc) {
        rc = -ENOMEM;
        goto out_free_rsp;
    }

    goto out;

out_free_rsp:
    rdma_pci_dma_unmap(pci_dev, dsr_info->rsp, sizeof(union pvrdma_cmd_resp));

out_free_req:
    rdma_pci_dma_unmap(pci_dev, dsr_info->req, sizeof(union pvrdma_cmd_req));

out_free_dsr:
    rdma_pci_dma_unmap(pci_dev, dsr_info->dsr,
                       sizeof(struct pvrdma_device_shared_region));
    dsr_info->dsr = NULL;

out:
    return rc;
}

/**
 * Convert git SHA hex string to uint64_t (first 8 bytes)
 * @sha: Git SHA hex string (at least 16 hex characters)
 * @return: uint64_t value from first 16 hex chars, or 0 on error
 */
static uint64_t git_sha_to_uint64(const char *sha)
{
    uint64_t result = 0;
    int i;

    if (!sha) {
        return 0;
    }

    /* Convert first 16 hex characters (8 bytes) to uint64_t */
    for (i = 0; i < 16 && sha[i] != '\0'; i++) {
        char c = sha[i];
        uint8_t nibble;

        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else {
            /* Invalid hex character */
            return 0;
        }

        result = (result << 4) | nibble;
    }

    return result;
}

static void init_dsr_dev_caps(PVRDMADev *dev)
{
    struct pvrdma_device_shared_region *dsr;
    uint64_t fw_ver_from_git;

    rdma_info_report("init_dsr_dev_caps: CALLED");

    if (!dev->dsr_info.dsr) {
        /* Buggy or malicious guest driver */
        rdma_error_report("Can't initialized DSR");
        return;
    }

    dsr = dev->dsr_info.dsr;
    rdma_info_report(
        "init_dsr_dev_caps: Setting caps.mode=ROCE, gid_types=ROCE_V1");
    rdma_info_report("init_dsr_dev_caps: DSR pointer = %p", dsr);
    rdma_info_report("init_dsr_dev_caps: Guest DSR PA = %#lx",
                     (unsigned long)dev->dsr_info.dma);

    /* Log the actual macro values */
    rdma_info_report("init_dsr_dev_caps: PVRDMA_FW_VERSION = %d",
                     PVRDMA_FW_VERSION);
    rdma_info_report("init_dsr_dev_caps: PVRDMA_DEVICE_MODE_ROCE = %d",
                     PVRDMA_DEVICE_MODE_ROCE);
    rdma_info_report("init_dsr_dev_caps: PVRDMA_GID_TYPE_FLAG_ROCE_V1 = 0x%x",
                     PVRDMA_GID_TYPE_FLAG_ROCE_V1);
    rdma_info_report("init_dsr_dev_caps: dsr->caps.gid_types BEFORE = 0x%x",
                     dsr->caps.gid_types);

    /* Convert git SHA to firmware version */
#ifdef GIT_SHA_FULL
    fw_ver_from_git = git_sha_to_uint64(GIT_SHA_FULL);
    if (fw_ver_from_git == 0) {
        /* Fallback to default if git SHA conversion failed */
        fw_ver_from_git = 1;
    }
#else
    fw_ver_from_git = 1; /* Default if git SHA not available */
#endif

    /* Write capabilities - these are the critical fields the driver checks */
    dsr->caps.fw_ver = fw_ver_from_git;
#ifdef GIT_SHA_SHORT
    rdma_info_report("init_dsr_dev_caps: fw_ver=0x%016" PRIx64
                     " (from git SHA: %s)",
                     fw_ver_from_git, GIT_SHA_SHORT);
#else
    rdma_info_report("init_dsr_dev_caps: fw_ver=0x%016" PRIx64
                     " (default, git SHA not available)",
                     fw_ver_from_git);
#endif
    dsr->caps.mode = PVRDMA_DEVICE_MODE_ROCE;
    dsr->caps.gid_types |= PVRDMA_GID_TYPE_FLAG_ROCE_V1;

    rdma_info_report("init_dsr_dev_caps: dsr->caps.gid_types AFTER = 0x%x",
                     dsr->caps.gid_types);

    /* Full memory barrier before continuing with other caps */
    __sync_synchronize();

    dsr->caps.max_uar = RDMA_BAR2_UAR_SIZE;
    dsr->caps.max_mr_size = dev->dev_attr.max_mr_size;
    dsr->caps.max_qp = dev->dev_attr.max_qp;
    dsr->caps.max_qp_wr = dev->dev_attr.max_qp_wr;
    dsr->caps.max_sge = dev->dev_attr.max_sge;
    dsr->caps.max_cq = dev->dev_attr.max_cq;
    dsr->caps.max_cqe = dev->dev_attr.max_cqe;
    dsr->caps.max_mr = dev->dev_attr.max_mr;
    dsr->caps.max_pd = dev->dev_attr.max_pd;
    dsr->caps.max_ah = dev->dev_attr.max_ah;
    dsr->caps.max_srq = dev->dev_attr.max_srq;
    dsr->caps.max_srq_wr = dev->dev_attr.max_srq_wr;
    dsr->caps.max_srq_sge = dev->dev_attr.max_srq_sge;
    dsr->caps.gid_tbl_len = MAX_GIDS;
    dsr->caps.sys_image_guid = 0;
    dsr->caps.node_guid = dev->node_guid;
    dsr->caps.phys_port_cnt = MAX_PORTS;
    dsr->caps.max_pkeys = MAX_PKEYS;
    /* Vendor and hardware version information */
    dsr->caps.vendor_id = 0x1022;      /* AMD vendor ID */
    dsr->caps.vendor_part_id = 0x1484; /* ROCm ERNIC device ID */
    dsr->caps.hw_ver = 1;              /* Hardware version 1 */
    /* Mesh metadata for TCP backend */
    dsr->caps.mesh_node_id =
        dev->backend_dev.mesh_enabled ? dev->backend_dev.mesh_node_id : 0xff;
    dsr->caps.mesh_num_nodes =
        dev->backend_dev.mesh_enabled ? dev->backend_dev.mesh_num_nodes : 0;
    dsr->caps.mesh_flags = dev->backend_dev.mesh_enabled ? 1 : 0;

    /* Per libvfio-user pattern from server.c:
     * Immediately call vfu_sgl_put() after writing to flush to guest.
     * This marks pages dirty and ensures guest sees the writes.
     */
    extern void pvrdma_dsr_flush(void *handle);
    rdma_info_report("init_dsr_dev_caps: Flushing DSR writes immediately "
                     "(matching server.c pattern)");
    pvrdma_dsr_flush(dev);

    /* Read back to verify the writes took effect */
    rdma_info_report("init_dsr_dev_caps: READBACK CHECK:");
    rdma_info_report("  fw_ver=%u (expected 1)", dsr->caps.fw_ver);
    rdma_info_report("  mode=%d (expected 0=ROCE)", dsr->caps.mode);
    rdma_info_report("  gid_types=0x%x (expected 0x1=ROCE_V1)",
                     dsr->caps.gid_types);

    /* Check actual offsets */
    rdma_info_report("  offsetof(fw_ver) = %zu",
                     offsetof(struct pvrdma_device_shared_region, caps.fw_ver));
    rdma_info_report("  offsetof(mode) = %zu",
                     offsetof(struct pvrdma_device_shared_region, caps.mode));
    rdma_info_report(
        "  offsetof(gid_types) = %zu",
        offsetof(struct pvrdma_device_shared_region, caps.gid_types));

    /* Calculate offset of gid_types within caps */
    size_t gid_offset_in_caps =
        offsetof(struct pvrdma_device_shared_region, caps.gid_types) -
        offsetof(struct pvrdma_device_shared_region, caps);
    rdma_info_report("  gid_types is at offset %zu within caps structure",
                     gid_offset_in_caps);

    /* Dump raw bytes around where fw_ver/mode/gid_types should be IN CAPS */
    unsigned char *caps_start = (unsigned char *)&dsr->caps;
    rdma_info_report(
        "  caps raw [0-7]: %02x %02x %02x %02x %02x %02x %02x %02x",
        caps_start[0], caps_start[1], caps_start[2], caps_start[3],
        caps_start[4], caps_start[5], caps_start[6], caps_start[7]);

    /* Check if there's struct packing/alignment issue */
    rdma_info_report("  sizeof(caps) = %zu", sizeof(dsr->caps));
    rdma_info_report("  Address of dsr->caps.fw_ver = %p", &dsr->caps.fw_ver);
    rdma_info_report("  Address of dsr->caps.mode = %p", &dsr->caps.mode);
    rdma_info_report("  Address of dsr->caps.gid_types = %p",
                     &dsr->caps.gid_types);
}

static void uninit_msix(PCIDevice *pdev, int used_vectors)
{
    PVRDMADev *dev = PVRDMA_DEV(pdev);
    int i;

    for (i = 0; i < used_vectors; i++) {
        msix_vector_unuse(pdev, i);
    }

    msix_uninit(pdev, &dev->msix, &dev->msix);
}

static int __attribute__((unused)) init_msix(PCIDevice *pdev)
{
    PVRDMADev *dev = PVRDMA_DEV(pdev);
    int i;
    int rc;

    rc = msix_init(pdev, RDMA_MAX_INTRS, &dev->msix, RDMA_MSIX_BAR_IDX,
                   RDMA_MSIX_TABLE, &dev->msix, RDMA_MSIX_BAR_IDX,
                   RDMA_MSIX_PBA, 0, NULL);

    if (rc < 0) {
        rdma_error_report("Failed to initialize MSI-X");
        return rc;
    }

    for (i = 0; i < RDMA_MAX_INTRS; i++) {
        msix_vector_use(PCI_DEVICE(dev), i);
    }

    return 0;
}

static void __attribute__((unused)) pvrdma_fini(PCIDevice *pdev)
{
    PVRDMADev *dev = PVRDMA_DEV(pdev);

    notifier_remove(&dev->shutdown_notifier);

    pvrdma_qp_ops_fini();

    rdma_backend_stop(&dev->backend_dev);

    rdma_rm_fini(&dev->rdma_dev_res, &dev->backend_dev,
                 dev->backend_eth_device_name);

    rdma_backend_fini(&dev->backend_dev);

    free_dsr(dev);

    if (msix_enabled(pdev)) {
        uninit_msix(pdev, RDMA_MAX_INTRS);
    }

    rdma_info_report("Device %s %x.%x is down", pdev->name, PCI_SLOT(pdev),
                     PCI_FUNC(pdev));
}

static void pvrdma_stop(PVRDMADev *dev)
{
    /* Only stop backend if it was successfully initialized */
    if (dev->backend_dev.context) {
        rdma_backend_stop(&dev->backend_dev);
    }
}

static void pvrdma_start(PVRDMADev *dev)
{
    /* Only start backend if it was successfully initialized */
    if (dev->backend_dev.context) {
        rdma_backend_start(&dev->backend_dev);
    } else {
        rdma_info_report("Backend not available, running in PCI-only mode");
    }
}

static void activate_device(PVRDMADev *dev)
{
    union ibv_gid default_gid;
    int ret;

    rdma_info_report("activate_device: Activating device (backend=%s)",
                     dev->backend_dev.context ? "available" : "not available");
    pvrdma_start(dev);
    set_reg_val(dev, PVRDMA_REG_ERR, 0);
    rdma_info_report("activate_device: Device activated successfully");

    /* Initialize default GID (index 0) for TCP mesh backend */
    /* The backend will set node-specific GID if in mesh mode */
    memset(&default_gid, 0, sizeof(default_gid));
    default_gid.raw[0] = 0xfe;
    default_gid.raw[1] = 0x80;
    default_gid.raw[8] = 0x02;
    default_gid.raw[11] = 0xff;
    default_gid.raw[12] = 0xfe;
    /* Last bytes will be set by tcp_add_gid if in mesh mode */

    ret = rdma_rm_add_gid(&dev->rdma_dev_res, &dev->backend_dev,
                          dev->backend_eth_device_name, &default_gid, 0);
    if (ret == 0) {
        rdma_info_report("activate_device: Initialized default GID at index 0");
    } else {
        rdma_info_report("activate_device: Failed to add default GID (ret=%d)",
                         ret);
    }
}

static int unquiesce_device(PVRDMADev *dev)
{
    return 0;
}

static void reset_device(PVRDMADev *dev)
{
    pvrdma_stop(dev);
}

/* Implementation function - called from bridge wrapper */
uint64_t pvrdma_regs_read_impl(void *opaque, hwaddr addr, unsigned size)
{
    PVRDMADev *dev = opaque;
    uint32_t val;

    dev->stats.regs_reads++;

    /* Check if this is an Ethernet register (0x28-0x64) */
    if (addr >= 0x28 && addr <= 0x64) {
        uint64_t eth_val = pvrdma_eth_regs_read(dev, addr);
        /* Ensure we return a valid 32-bit value */
        return (uint32_t)eth_val;
    }

    if (get_reg_val(dev, addr, &val)) {
        rdma_error_report("Failed to read REG value from address 0x%x",
                          (uint32_t)addr);
        return -EINVAL;
    }

    /* Log error register reads specially */
    if (addr == PVRDMA_REG_ERR) {
        rdma_info_report(
            ">>> ERROR REGISTER READ: offset=0x%lx (PVRDMA_REG_ERR) value=%#x",
            addr, val);
    } else {
        rdma_info_report("BAR1 READ: offset=%#lx size=%u value=%#x", addr, size,
                         val);
    }

    return val;
}

/* Implementation function - called from bridge wrapper */
void pvrdma_regs_write_impl(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PVRDMADev *dev = opaque;

    rdma_info_report("BAR1 WRITE: offset=%#lx size=%u value=%#lx", addr, size,
                     val);

    dev->stats.regs_writes++;

    if (set_reg_val(dev, addr, val)) {
        rdma_error_report("Failed to set REG value, addr=0x%" PRIx64
                          ", val=0x%" PRIx64,
                          addr, val);
        return;
    }

    switch (addr) {
    case PVRDMA_REG_DSRLOW:
        dev->dsr_info.dma = val;
        break;
    case PVRDMA_REG_DSRHIGH:
        dev->dsr_info.dma |= val << 32;

        rdma_info_report("DSRHIGH write: Starting DSR initialization");
        load_dsr(dev);

        if (!dev->dsr_info.dsr) {
            rdma_error_report("DSRHIGH write: load_dsr() failed!");
            break;
        }

        /* init_dsr_dev_caps() now flushes immediately after writes (server.c
         * pattern) */
        init_dsr_dev_caps(dev);

        rdma_info_report("DSRHIGH write: Complete");
        break;
    case PVRDMA_REG_CTL:
        rdma_info_report(">>> CTL register write: val=%u", (unsigned)val);
        switch (val) {
        case PVRDMA_DEVICE_CTL_ACTIVATE:
            rdma_info_report(
                ">>> ACTIVATE command received, calling activate_device()");
            activate_device(dev);
            rdma_info_report(">>> activate_device() returned successfully");
            break;
        case PVRDMA_DEVICE_CTL_UNQUIESCE:
            rdma_info_report(">>> UNQUIESCE command received");
            unquiesce_device(dev);
            break;
        case PVRDMA_DEVICE_CTL_RESET:
            rdma_info_report(">>> RESET command received");
            reset_device(dev);
            break;
        default:
            rdma_info_report(">>> Unknown CTL command: %u", (unsigned)val);
            break;
        }
        rdma_info_report(">>> CTL register write complete");
        break;
    case PVRDMA_REG_IMR:
        dev->interrupt_mask = val;
        break;
    case PVRDMA_REG_REQUEST:
        if (val == 0) {
            pvrdma_exec_cmd(dev);
        }
        break;
    default:
        /* Check if this is an Ethernet register (0x28-0x64) */
        if (addr >= 0x28 && addr <= 0x64) {
            /* Handle Ethernet registers */
            pvrdma_eth_regs_write(dev, addr, val);
        }
        break;
    }
}

static const MemoryRegionOps regs_ops = {
    .read = pvrdma_regs_read_impl,
    .write = pvrdma_regs_write_impl,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl =
        {
            .min_access_size = sizeof(uint32_t),
            .max_access_size = sizeof(uint32_t),
        },
};

/* Implementation function - called from bridge wrapper */
uint64_t pvrdma_uar_read_impl(void *opaque, hwaddr addr, unsigned size)
{
    return 0xffffffff;
}

/* Implementation function - called from bridge wrapper */
void pvrdma_uar_write_impl(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    PVRDMADev *dev = opaque;
    hwaddr page_offset;

    rdma_info_report(">>> UAR WRITE: addr=0x%lx, val=0x%lx, size=%u",
                     (unsigned long)addr, (unsigned long)val, size);

    dev->stats.uar_writes++;

    /* Extract offset within the UAR page (each UC gets a 4KB page) */
    /* UAR is organized as: page_N = N * 0x1000, offsets within page: 0, 4, 8 */
    page_offset = addr & 0xFFF;

    /* Route by offset first - bit 31 means different things at different
     * offsets */
    switch (page_offset) {
    case PVRDMA_UAR_QP_OFFSET: {
        /* QP doorbell at offset 0 */
        uint32_t qp_handle = val & PVRDMA_UAR_HANDLE_MASK;
        PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);

        rdma_info_report(
            ">>> UAR: QP doorbell! val=0x%lx, flags: send=%d recv=%d",
            (unsigned long)val, !!(val & PVRDMA_UAR_QP_SEND),
            !!(val & PVRDMA_UAR_QP_RECV));
        if (val & PVRDMA_UAR_QP_SEND) {
            if (qp_stats) {
                qp_stats->doorbell_send++;
            }
            rdma_info_report(">>> UAR: Calling pvrdma_qp_send(handle=%u)",
                             qp_handle);
            pvrdma_qp_send(dev, qp_handle);
        }
        if (val & PVRDMA_UAR_QP_RECV) {
            if (qp_stats) {
                qp_stats->doorbell_recv++;
            }
            rdma_info_report(">>> UAR: Calling pvrdma_qp_recv(handle=%u)",
                             qp_handle);
            pvrdma_qp_recv(dev, qp_handle);
        }
        break;
    }
    case PVRDMA_UAR_CQ_OFFSET:
        /* CQ doorbell at offset 4 - bit 31 means CQ_POLL, not QP_RECV */
        if (val & PVRDMA_UAR_CQ_ARM) {
            rdma_rm_req_notify_cq(&dev->rdma_dev_res,
                                  val & PVRDMA_UAR_HANDLE_MASK,
                                  !!(val & PVRDMA_UAR_CQ_ARM_SOL));
        }
        if (val & PVRDMA_UAR_CQ_ARM_SOL) {
            rdma_warn_report("CQ ARM SOL not supported");
        }
        if (val & PVRDMA_UAR_CQ_POLL) {
            uint32_t cq_handle = val & PVRDMA_UAR_HANDLE_MASK;
            rdma_info_report(">>> UAR: CQ poll doorbell! cq_handle=%u",
                             cq_handle);
            pvrdma_cq_poll(&dev->rdma_dev_res, cq_handle);
        }
        break;
    case PVRDMA_UAR_SRQ_OFFSET: {
        uint32_t srq_handle = val & PVRDMA_UAR_HANDLE_MASK;
        PVRDMAQPStats *srq_stats = pvrdma_get_qp_stats(dev, srq_handle);

        if (val & PVRDMA_UAR_SRQ_RECV) {
            if (srq_stats) {
                srq_stats->doorbell_srq++;
            }
            pvrdma_srq_recv(dev, srq_handle);
        }
        break;
    }
    default:
        /* Check if this might be a QP doorbell at wrong offset */
        if ((val & (PVRDMA_UAR_QP_SEND | PVRDMA_UAR_QP_RECV)) &&
            page_offset != PVRDMA_UAR_CQ_OFFSET) {
            /* QP doorbell at wrong offset - handle it anyway */
            uint32_t qp_handle = val & PVRDMA_UAR_HANDLE_MASK;
            PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);

            rdma_warn_report(
                ">>> UAR: QP doorbell at wrong offset! addr=0x%lx "
                "(offset=0x%lx, expected 0x%x), val=0x%lx - handling anyway",
                (unsigned long)addr, (unsigned long)page_offset,
                PVRDMA_UAR_QP_OFFSET, (unsigned long)val);
            if (val & PVRDMA_UAR_QP_SEND) {
                if (qp_stats) {
                    qp_stats->doorbell_send++;
                }
                rdma_info_report(">>> UAR: Calling pvrdma_qp_send(handle=%u)",
                                 qp_handle);
                pvrdma_qp_send(dev, qp_handle);
            }
            if (val & PVRDMA_UAR_QP_RECV) {
                if (qp_stats) {
                    qp_stats->doorbell_recv++;
                }
                rdma_info_report(">>> UAR: Calling pvrdma_qp_recv(handle=%u)",
                                 qp_handle);
                pvrdma_qp_recv(dev, qp_handle);
            }
        } else {
            rdma_error_report("Unsupported command, addr=0x%" PRIx64
                              ", val=0x%" PRIx64,
                              addr, val);
        }
        break;
    }
}

static const MemoryRegionOps uar_ops = {
    .read = pvrdma_uar_read_impl,
    .write = pvrdma_uar_write_impl,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl =
        {
            .min_access_size = sizeof(uint32_t),
            .max_access_size = sizeof(uint32_t),
        },
};

static void __attribute__((unused)) init_pci_config(PCIDevice *pdev)
{
    pdev->config[PCI_INTERRUPT_PIN] = 1;
}

static void __attribute__((unused)) init_bars(PCIDevice *pdev)
{
    PVRDMADev *dev = PVRDMA_DEV(pdev);

    /* BAR 0 - MSI-X */
    memory_region_init(&dev->msix, OBJECT(dev), "pvrdma-msix",
                       RDMA_BAR0_MSIX_SIZE);
    pci_register_bar(pdev, RDMA_MSIX_BAR_IDX, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &dev->msix);

    /* BAR 1 - Registers */
    memset(&dev->regs_data, 0, sizeof(dev->regs_data));
    memory_region_init_io(&dev->regs, OBJECT(dev), &regs_ops, dev,
                          "pvrdma-regs", sizeof(dev->regs_data));
    pci_register_bar(pdev, RDMA_REG_BAR_IDX, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &dev->regs);

    /* BAR 2 - UAR */
    memset(&dev->uar_data, 0, sizeof(dev->uar_data));
    memory_region_init_io(&dev->uar, OBJECT(dev), &uar_ops, dev, "rdma-uar",
                          sizeof(dev->uar_data));
    pci_register_bar(pdev, RDMA_UAR_BAR_IDX, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &dev->uar);
}

static void __attribute__((unused)) init_regs(PCIDevice *pdev)
{
    PVRDMADev *dev = PVRDMA_DEV(pdev);

    set_reg_val(dev, PVRDMA_REG_VERSION, PVRDMA_HW_VERSION);
    set_reg_val(dev, PVRDMA_REG_ERR, 0xFFFF);
}

static void __attribute__((unused)) init_dev_caps(PVRDMADev *dev)
{
    size_t pg_tbl_bytes = PAGE_SIZE * (PAGE_SIZE / sizeof(uint64_t));
    size_t wr_sz =
        MAX(sizeof(struct pvrdma_sq_wqe_hdr), sizeof(struct pvrdma_rq_wqe_hdr));

    rdma_info_report("init_dev_caps: CALLED");
    rdma_info_report("  PAGE_SIZE=%zu, pg_tbl_bytes=%zu", (size_t)PAGE_SIZE,
                     pg_tbl_bytes);
    rdma_info_report("  sizeof(pvrdma_cqe)=%zu", sizeof(struct pvrdma_cqe));
    rdma_info_report("  max_sge=%d", dev->dev_attr.max_sge);

    dev->dev_attr.max_qp_wr =
        pg_tbl_bytes /
            (wr_sz + sizeof(struct pvrdma_sge) * dev->dev_attr.max_sge) -
        PAGE_SIZE;
    /* First page is ring state  ^^^^ */

    dev->dev_attr.max_cqe = pg_tbl_bytes / sizeof(struct pvrdma_cqe) -
                            PAGE_SIZE; /* First page is ring state */

    dev->dev_attr.max_srq_wr =
        pg_tbl_bytes /
            ((sizeof(struct pvrdma_rq_wqe_hdr) + sizeof(struct pvrdma_sge)) *
             dev->dev_attr.max_sge) -
        PAGE_SIZE;

    rdma_info_report("init_dev_caps: RESULTS:");
    rdma_info_report("  max_qp_wr=%d", dev->dev_attr.max_qp_wr);
    rdma_info_report("  max_cqe=%d", dev->dev_attr.max_cqe);
    rdma_info_report("  max_srq_wr=%d", dev->dev_attr.max_srq_wr);
}

static int __attribute__((unused)) pvrdma_check_ram_shared(Object *obj,
                                                           void *opaque)
{
    bool *shared = opaque;

    if (object_dynamic_cast(obj, "memory-backend-ram")) {
        *shared = object_property_get_bool(obj, "share", NULL);
    }

    return 0;
}

/*
 * Functions below (pvrdma_shutdown_notifier, pvrdma_check_ram_shared,
 * pvrdma_realize) are part of QEMU's device initialization flow. In our
 * standalone libvfio-user server, these are replaced by the wrapper API
 * functions in vfu_compat_bridge.c
 */
#if 0
static void pvrdma_shutdown_notifier(Notifier *n, void *opaque)
{
    PVRDMADev *dev = container_of(n, PVRDMADev, shutdown_notifier);
    PCIDevice *pci_dev = PCI_DEVICE(dev);

    pvrdma_fini(pci_dev);
}

static int pvrdma_check_ram_shared(Object *obj, void *opaque)
{
    bool *shared = opaque;

    if (object_dynamic_cast(obj, "memory-backend-ram")) {
        *shared = object_property_get_bool(obj, "share", NULL);
    }

    return 0;
}

static void pvrdma_realize(PCIDevice *pdev, Error **errp)
{
    int rc = 0;
    PVRDMADev *dev = PVRDMA_DEV(pdev);
    Object *memdev_root;
    bool ram_shared = false;
    PCIDevice *func0;

    rdma_info_report("Initializing device %s %x.%x", pdev->name,
                     PCI_SLOT(pdev), PCI_FUNC(pdev));

    if (PAGE_SIZE != qemu_real_host_page_size()) {
        error_setg(errp, "Target page size must be the same as host page size");
        return;
    }

    func0 = pci_get_function_0(pdev);
    /* Break if not vmxnet3 device in slot 0 */
    if (strcmp(object_get_typename(OBJECT(func0)), TYPE_VMXNET3)) {
        error_setg(errp, "Device on %x.0 must be %s", PCI_SLOT(pdev),
                   TYPE_VMXNET3);
        return;
    }
    dev->func0 = VMXNET3(func0);

    addrconf_addr_eui48((unsigned char *)&dev->node_guid,
                        (const char *)&dev->func0->conf.macaddr.a);

    memdev_root = object_resolve_path("/objects", NULL);
    if (memdev_root) {
        object_child_foreach(memdev_root, pvrdma_check_ram_shared, &ram_shared);
    }
    if (!ram_shared) {
        error_setg(errp, "Only shared memory backed ram is supported");
        return;
    }

    dev->dsr_info.dsr = NULL;

    init_pci_config(pdev);

    init_bars(pdev);

    init_regs(pdev);

    rc = init_msix(pdev);
    if (rc) {
        goto out;
    }

    rc = rdma_backend_init(&dev->backend_dev, pdev, &dev->rdma_dev_res,
                           dev->backend_device_name, dev->backend_port_num,
                           &dev->dev_attr, &dev->mad_chr);
    if (rc) {
        goto out;
    }
    /* Ensure backend holds the live rdma_dev_res pointer after any init reorders */
    dev->backend_dev.rdma_dev_res = &dev->rdma_dev_res;
    rdma_info_report("pvrdma_realize: backend_dev.rdma_dev_res=%p (expected %p)",
                     dev->backend_dev.rdma_dev_res, &dev->rdma_dev_res);

    init_dev_caps(dev);

    rc = rdma_rm_init(&dev->rdma_dev_res, &dev->dev_attr);
    if (rc) {
        goto out;
    }

    rc = pvrdma_qp_ops_init();
    if (rc) {
        goto out;
    }

    memset(&dev->stats, 0, sizeof(dev->stats));
    dev->stats.qp_stats = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                 NULL, g_free);
    dev->stats.stats_file = NULL;
    dev->stats.stats_fp = NULL;
    dev->stats.stats_write_count = 0;

    dev->shutdown_notifier.notify = pvrdma_shutdown_notifier;
    qemu_register_shutdown_notifier(&dev->shutdown_notifier);

#ifdef LEGACY_RDMA_REG_MR
    rdma_info_report("Using legacy reg_mr");
#else
    rdma_info_report("Using iova reg_mr");
#endif

out:
    if (rc) {
        pvrdma_fini(pdev);
        error_append_hint(errp, "Device failed to load\n");
    }
}
#endif /* Unused QEMU functions commented out */

/*
 * QEMU type registration - not needed for standalone libvfio-user server
 * The wrapper API in vfu_compat_bridge.c provides the glue
 */
#if 0
static void pvrdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    RdmaProviderClass *ir = RDMA_PROVIDER_CLASS(klass);

    k->realize = pvrdma_realize;
    k->vendor_id = PCI_VENDOR_ID_VMWARE;
    k->device_id = PCI_DEVICE_ID_VMWARE_PVRDMA;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_NETWORK_OTHER;

    dc->desc = "RDMA Device";
    device_class_set_props(dc, pvrdma_dev_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    ir->format_statistics = pvrdma_format_statistics;
}

static const TypeInfo pvrdma_info = {
    .name = PVRDMA_HW_NAME,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PVRDMADev),
    .class_init = pvrdma_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { INTERFACE_RDMA_PROVIDER },
        { }
    }
};

static void register_types(void)
{
    type_register_static(&pvrdma_info);
}

type_init(register_types)
#endif
