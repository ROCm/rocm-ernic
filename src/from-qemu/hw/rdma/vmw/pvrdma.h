/*
 * QEMU VMWARE paravirtual RDMA device definitions
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

#ifndef PVRDMA_PVRDMA_H
#define PVRDMA_PVRDMA_H

#include "qemu/units.h"
#include "qemu/notify.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci_device.h"
#include "chardev/char-fe.h"
#include "hw/net/vmxnet3_defs.h"

#include "../rdma_backend_defs.h"
#include "../rdma_rm_defs.h"

#include "standard-headers/drivers/infiniband/hw/vmw_pvrdma/pvrdma_dev_api.h"
#include "pvrdma_dev_ring.h"
#include "qom/object.h"

#define PAGE_SIZE 4096

/* BARs */
#define RDMA_MSIX_BAR_IDX 0
#define RDMA_REG_BAR_IDX  1
#define RDMA_UAR_BAR_IDX  2
#ifndef RDMA_BAR0_MSIX_SIZE
#define RDMA_BAR0_MSIX_SIZE (16 * KiB)
#endif
#ifndef RDMA_BAR1_REGS_SIZE
#define RDMA_BAR1_REGS_SIZE 64
#endif
#ifndef RDMA_BAR2_UAR_SIZE
#define RDMA_BAR2_UAR_SIZE (0x1000 * MAX_UCS) /* each uc gets page */
#endif

/* MSIX */
#define RDMA_MAX_INTRS  3
#define RDMA_MSIX_TABLE 0x0000
#define RDMA_MSIX_PBA   0x2000

/* Interrupts Vectors */
#define INTR_VEC_CMD_RING         0
#define INTR_VEC_CMD_ASYNC_EVENTS 1
#define INTR_VEC_CMD_COMPLETION_Q 2

/* HW attributes */
#define PVRDMA_HW_NAME    "pvrdma"
#define PVRDMA_HW_VERSION 17
#define PVRDMA_FW_VERSION 14

/* Some defaults */
#define PVRDMA_PKEY 0xFFFF

typedef struct DSRInfo {
    dma_addr_t dma;
    struct pvrdma_device_shared_region *dsr;

    union pvrdma_cmd_req *req;
    union pvrdma_cmd_resp *rsp;

    PvrdmaRingState *async_ring_state;
    PvrdmaRing async;

    PvrdmaRingState *cq_ring_state;
    PvrdmaRing cq;
} DSRInfo;

/* Per-QP statistics */
typedef struct PVRDMAQPStats {
    uint64_t doorbell_send;      /* Send doorbell rings */
    uint64_t doorbell_recv;      /* Receive doorbell rings */
    uint64_t doorbell_srq;       /* SRQ doorbell rings */
    uint64_t wqes_processed;     /* Total WQEs processed */
    uint64_t wqes_by_opcode[18]; /* WQEs by opcode type (see PVRDMA_WR_*) */
    uint64_t cqes_posted;        /* CQEs posted */
    uint64_t continuations;      /* Continuation callbacks scheduled */
    uint64_t bytes_sent;         /* Bytes sent via SEND operations */
    uint64_t bytes_received;     /* Bytes received via RECV operations */
    uint64_t bytes_rdma_read;    /* Bytes read via RDMA Read operations */
    uint64_t bytes_rdma_write;   /* Bytes written via RDMA Write operations */
} PVRDMAQPStats;

typedef struct PVRDMADevStats {
    uint64_t commands;
    uint64_t regs_reads;
    uint64_t regs_writes;
    uint64_t uar_reads;
    uint64_t uar_writes;
    uint64_t bar0_reads;
    uint64_t bar0_writes;
    uint64_t interrupts;
    GHashTable *qp_stats;            /* Per-QP statistics (key: QP handle) */
    char *stats_file;                /* Stats output file path */
    FILE *stats_fp;                  /* Stats file handle */
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
    MemoryRegion msix;
    MemoryRegion regs;
    uint32_t regs_data[RDMA_BAR1_REGS_SIZE];
    MemoryRegion uar;
    uint32_t uar_data[RDMA_BAR2_UAR_SIZE / sizeof(uint32_t)];
    DSRInfo dsr_info;
    int interrupt_mask;
    struct ibv_device_attr dev_attr;
    uint64_t node_guid;
    char *backend_eth_device_name;
    char *backend_device_name;
    uint8_t backend_port_num;
    RdmaBackendDev backend_dev;
    RdmaDeviceResources rdma_dev_res;
    CharBackend mad_chr;
    VMXNET3State *func0;
    Notifier shutdown_notifier;
    PVRDMADevStats stats;
    /* Optional strings for stats file display (freed in cleanup) */
    char *stats_socket_path; /* Socket path for this instance */
    char *stats_backend_str; /* Full backend string (e.g. loopback:mode=...) */
    char *
        stats_connection_str; /* e.g. "connected", "disconnected (lost conn)" */
    uint16_t stats_pci_vid;   /* PCI Vendor ID for stats display */
    uint16_t stats_pci_did;   /* PCI Device ID for stats display */

    /* DHCP server (for loopback mode and TCP manager mode) */
    void *dhcp_server; /* DhcpServer* - forward declared to avoid include */
    /* DHCP proxy (for TCP worker mode) */
    void *dhcp_proxy; /* DhcpProxy* - forward declared to avoid include */
    /* TCP connections for rdma_cm (loopback mode) */
    GHashTable *tcp_connections; /* TcpConnection* - forward declared */

    /* MAC address */
    uint8_t mac_addr[6]; /* Device MAC address */
    bool mac_addr_set;   /* Whether MAC address was explicitly set */

    /*
     * Thread-safe completion interrupt delivery.
     * The TCP recv thread sets this flag instead of
     * calling vfu_irq_trigger directly; the main loop
     * drains it on every iteration.
     */
    volatile int pending_cq_interrupt;
};
typedef struct PVRDMADev PVRDMADev;
DECLARE_INSTANCE_CHECKER(PVRDMADev, PVRDMA_DEV, PVRDMA_HW_NAME)

static inline int get_reg_val(PVRDMADev *dev, hwaddr addr, uint32_t *val)
{
    int idx = addr >> 2;

    if (idx >= RDMA_BAR1_REGS_SIZE) {
        return -EINVAL;
    }

    *val = dev->regs_data[idx];

    return 0;
}

static inline int set_reg_val(PVRDMADev *dev, hwaddr addr, uint32_t val)
{
    int idx = addr >> 2;

    if (idx >= RDMA_BAR1_REGS_SIZE) {
        return -EINVAL;
    }

    dev->regs_data[idx] = val;

    return 0;
}

/* post_interrupt and pvrdma_exec_cmd are implemented in vfu_compat_bridge.c for
 * standalone mode */
void post_interrupt(PVRDMADev *dev, unsigned vector);
int pvrdma_exec_cmd(PVRDMADev *dev);

/* Statistics functions */
PVRDMAQPStats *pvrdma_get_qp_stats(PVRDMADev *dev, uint32_t qp_handle);
void pvrdma_write_stats_impl(PVRDMADev *dev);

/* Register/UAR handlers - implementations in pvrdma_main.c */
uint64_t pvrdma_regs_read_impl(void *opaque, hwaddr addr, unsigned size);
void pvrdma_regs_write_impl(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size);
uint64_t pvrdma_uar_read_impl(void *opaque, hwaddr addr, unsigned size);
void pvrdma_uar_write_impl(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size);

/* Ethernet register handlers */
uint64_t pvrdma_eth_regs_read(PVRDMADev *dev, hwaddr addr);
void pvrdma_eth_regs_write(PVRDMADev *dev, hwaddr addr, uint64_t val);
void pvrdma_eth_process_tx(PVRDMADev *dev);
void pvrdma_eth_rx_frame(PVRDMADev *dev, const void *frame_data, size_t len);

#endif
