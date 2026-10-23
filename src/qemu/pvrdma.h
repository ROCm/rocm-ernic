/*
 * QEMU VMWARE paravirtual RDMA device definitions
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 *
 */

#ifndef PVRDMA_PVRDMA_H
#define PVRDMA_PVRDMA_H

#include <stdint.h>
#include <glib.h>
#include <infiniband/verbs.h>

#include "hw/pci/pci_device.h"

#include "rdma_backend_defs.h"
#include "rdma_rm_defs.h"

/* Per-QP statistics */
typedef struct PVRDMAQPStats {
    uint64_t doorbell_send;      /* Send doorbell rings */
    uint64_t doorbell_recv;      /* Receive doorbell rings */
    uint64_t wqes_processed;     /* Total WQEs processed */
    uint64_t wqes_by_opcode[18]; /* WQEs by opcode type (see PVRDMA_WR_*) */
    uint64_t cqes_posted;        /* CQEs posted */
    uint64_t bytes_sent;         /* Bytes sent via SEND operations */
    uint64_t bytes_received;     /* Bytes received via RECV operations */
    uint64_t bytes_rdma_read;    /* Bytes read via RDMA Read operations */
    uint64_t bytes_rdma_write;   /* Bytes written via RDMA Write operations */
} PVRDMAQPStats;

typedef struct PVRDMADevStats {
    uint64_t commands;
    uint64_t uar_reads;
    uint64_t uar_writes;
    uint64_t bar0_reads;
    uint64_t bar0_writes;
    uint64_t interrupts;
    GHashTable *qp_stats;            /* Per-QP statistics (key: QP handle) */
    char *stats_file;                /* Stats output file path */
    uint64_t stats_write_count;      /* Number of times stats written */
    uint64_t total_bytes_sent;       /* Total bytes sent across all QPs */
    uint64_t total_bytes_received;   /* Total bytes received across all QPs */
    uint64_t total_bytes_rdma_read;  /* Total bytes read via RDMA Read */
    uint64_t total_bytes_rdma_write; /* Total bytes written via RDMA Write */
    uint64_t total_ip_bytes_tx;      /* Total IP/Ethernet bytes transmitted */
    uint64_t total_ip_bytes_rx;      /* Total IP/Ethernet bytes received */
    uint64_t reset_count;            /* Device/FLR reset count */
} PVRDMADevStats;

struct PVRDMADev {
    PCIDevice parent_obj;
    struct ibv_device_attr dev_attr;
    char *backend_device_name;
    RdmaBackendDev backend_dev;
    RdmaDeviceResources rdma_dev_res;
    PVRDMADevStats stats;
    /* Optional strings for stats file display (freed in cleanup) */
    char *stats_socket_path; /* Socket path for this instance */
    char *stats_backend_str; /* Full backend string (e.g. loopback:mode=...) */
    char *
        stats_connection_str; /* e.g. "connected", "disconnected (lost conn)" */
    uint16_t stats_pci_vid;   /* PCI Vendor ID for stats display */
    uint16_t stats_pci_did;   /* PCI Device ID for stats display */

    uint8_t mac_addr[6]; /* Device MAC address */
};
typedef struct PVRDMADev PVRDMADev;

/* Statistics functions */
PVRDMAQPStats *pvrdma_get_qp_stats(PVRDMADev *dev, uint32_t qp_handle);
void pvrdma_write_stats_impl(PVRDMADev *dev);

#endif
