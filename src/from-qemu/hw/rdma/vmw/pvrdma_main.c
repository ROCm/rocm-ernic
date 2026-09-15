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
 * What remains of the original QEMU device model is the statistics
 * bookkeeping.  The guest-facing PVRDMA interface -- DSR command ring,
 * BAR1 registers, UAR doorbells, realize/reset -- went away with the
 * PVRDMA device mode; the ionic front end drives the resource
 * manager and backend directly.
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
                                  "ERROR",
                                  "SEND_DC"};

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
    fprintf(fp, "Instance:\n");
    fprintf(fp, "  %-11s : %s\n", "socket",
            dev->stats_socket_path ? dev->stats_socket_path : "(not set)");
    fprintf(fp, "  %-11s : %s\n", "backend",
            dev->stats_backend_str ? dev->stats_backend_str : "(not set)");
    if (dev->stats_pci_vid || dev->stats_pci_did) {
        fprintf(fp, "  %-11s : 0x%04x:0x%04x\n", "pci", dev->stats_pci_vid,
                dev->stats_pci_did);
    } else {
        fprintf(fp, "  %-11s : (not set)\n", "pci");
    }
    fprintf(fp, "  %-11s : %02x:%02x:%02x:%02x:%02x:%02x\n", "mac",
            dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
            dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);
    fprintf(fp, "  %-11s : %s\n", "connection",
            dev->stats_connection_str ? dev->stats_connection_str
                                      : "(not set)");
    /* The " : " separator is what ernic-exporter splits every stat line on;
     * without it this counter is invisible to the exporter. */
    fprintf(fp, "\nWrite count : %" PRIu64 "\n\n",
            dev->stats.stats_write_count);
    fprintf(fp, "Device Statistics:\n");
#define STATS_LABEL_WIDTH 23
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "commands",
            dev->stats.commands);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "bar0_reads",
            dev->stats.bar0_reads);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "bar0_writes",
            dev->stats.bar0_writes);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "uar_reads",
            dev->stats.uar_reads);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "uar_writes",
            dev->stats.uar_writes);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "mmio_reads_total",
            dev->stats.bar0_reads + dev->stats.uar_reads);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "mmio_writes_total",
            dev->stats.bar0_writes + dev->stats.uar_writes);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "interrupts",
            dev->stats.interrupts);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "reset_count",
            dev->stats.reset_count);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, "total_bytes_sent",
            dev->stats.total_bytes_sent);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "total_bytes_received", dev->stats.total_bytes_received);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "total_bytes_rdma_read", dev->stats.total_bytes_rdma_read);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "total_bytes_rdma_write", dev->stats.total_bytes_rdma_write);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "total_ip_bytes_tx", dev->stats.total_ip_bytes_tx);
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH,
            "total_ip_bytes_rx", dev->stats.total_ip_bytes_rx);
#undef STATS_LABEL_WIDTH
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
            fprintf(fp, "    wqes_processed : %" PRIu64 "\n",
                    qp_stats->wqes_processed);
            fprintf(fp, "    cqes_posted    : %" PRIu64 "\n",
                    qp_stats->cqes_posted);
            fprintf(fp, "    bytes_sent     : %" PRIu64 "\n",
                    qp_stats->bytes_sent);
            fprintf(fp, "    bytes_received : %" PRIu64 "\n",
                    qp_stats->bytes_received);
            fprintf(fp, "    bytes_rdma_read : %" PRIu64 "\n",
                    qp_stats->bytes_rdma_read);
            fprintf(fp, "    bytes_rdma_write : %" PRIu64 "\n",
                    qp_stats->bytes_rdma_write);

            fprintf(fp, "    WQEs by opcode:\n");
            for (i = 0; i < 18 && i < (int)G_N_ELEMENTS(opcode_names); i++) {
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
