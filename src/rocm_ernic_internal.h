/*
 * Internal Device Structure for ROCm ERNIC (Emulated RDMA NIC)
 *
 * This header defines our main device structure without including QEMU headers.
 * We use opaque handles to hide QEMU types.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_INTERNAL_H
#define ROCM_ERNIC_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"
#include "ionic_eth_emu.h"
#include "ionic_rdma_devcmd.h"
#include "ionic_datapath.h"

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;

/* ---------------------------------------------------------------------------
 * Legacy PVRDMA BAR layout (kept for reference during ionic migration).
 * These are used by the PVRDMA emulation layer in src/from-qemu/hw/rdma/vmw/.
 * ---------------------------------------------------------------------------
 */
#ifndef RDMA_BAR0_MSIX_SIZE
#define RDMA_BAR0_MSIX_SIZE (16 * 1024) /* 16 KB for MSI-X */
#endif
#ifndef RDMA_BAR1_REGS_SIZE
#define RDMA_BAR1_REGS_SIZE 64 /* 64 DWORDs = 256 bytes */
#endif
#ifndef MAX_UCS
#define MAX_UCS 512 /* Maximum number of user contexts */
#endif
#ifndef RDMA_BAR2_UAR_SIZE
#define RDMA_BAR2_UAR_SIZE (0x1000 * MAX_UCS) /* Each UC gets 4KB page */
#endif

/* Legacy PVRDMA MSI-X interrupt vectors */
#define RDMA_MAX_INTRS            3
#define INTR_VEC_CMD_RING         0
#define INTR_VEC_CMD_ASYNC_EVENTS 1
#define INTR_VEC_CMD_COMPLETION_Q 2

/* ---------------------------------------------------------------------------
 * ionic BAR layout (target layout after ionic migration).
 *
 * Real Pensando DSC BAR map (from ionic_if.h / ionic driver probe):
 *   BAR0 (64-bit): device registers + admin queue doorbell region (~4 MB)
 *   BAR2 (no BAR1): doorbell BAR — per-LIF doorbell pages for SQ/RQ/CQ/EQ
 *   BAR4:           device info page (read-only)
 *
 * For the emulated device we simplify to:
 *   BAR0: 4 MB — devcmd registers + MSI-X table/PBA
 *   BAR2: 4 MB — doorbell pages (per-LIF: kernel page at index kern_pid,
 *                user pages at higher indices via mmap)
 *
 * The ionic driver discovers these sizes from the identify response.
 * ---------------------------------------------------------------------------
 */
#define IONIC_BAR0_REGS_SIZE (4 * 1024 * 1024) /* 4 MB: regs + MSI-X   */
#define IONIC_BAR2_DB_SIZE   (4 * 1024 * 1024) /* 4 MB: doorbell pages  */
#define IONIC_DB_PAGE_SIZE   4096              /* one 4K page per LIF   */
#define IONIC_KERN_PID       0                 /* kernel doorbell page  */

/* ionic MSI-X vectors: EQ per vector (driver requests eq_count vectors).
 * We start with a fixed count matching IONIC_EQ_COUNT_MIN = 4. */
#define IONIC_MSIX_MIN_VECTORS 4
#define IONIC_MSIX_MAX_VECTORS 32

/**
 * rocm_ernic_dev - Main device structure
 *
 * This structure contains both the libvfio-user context and a handle to
 * the RDMA device implementation. The actual device structures are
 * hidden behind the opaque pvrdma_handle_t.
 */
struct rocm_ernic_dev {
    /* libvfio-user context */
    vfu_ctx_t *vfu_ctx;

    /* Opaque handle to legacy PVRDMA device (used during ionic migration) */
    pvrdma_handle_t pvrdma_handle;

    /* ionic emulation layer (replaces PVRDMA when ionic_mode is true) */
    struct ionic_eth_emu *ionic_emu;
    struct ionic_rdma_devcmd_state *ionic_rdma;
    struct ionic_datapath *ionic_dp;
    bool ionic_mode; /* true = use ionic path */

    /* BAR memory backing stores */
    void *bar0_mem; /* MSI-X table/PBA (legacy) or ionic BAR0 shadow */
    void *bar1_mem; /* Registers (legacy PVRDMA only) */
    void *bar2_mem; /* UAR (legacy) or ionic doorbell pages */

    /* Backend device configuration */
    char *backend_type_str;    /* Backend type: none, loopback, verbs:device */
    char *backend_device_name; /* IB device (e.g., "mlx5_0") */
    char *backend_eth_device;  /* Eth device (e.g., "eth0") */
    uint8_t backend_port_num;  /* IB port number */

    /* Device state flags */
    bool device_initialized; /* Device structure created */
    bool device_realized;    /* Backend initialized */
    bool device_active;      /* Client connected and device running */
    bool verbose;            /* Verbose logging enabled */

    /* Statistics */
    char *stats_file_path; /* Path to stats output file */

    /* MAC address */
    uint8_t mac_addr[6]; /* Device MAC address */
    bool mac_addr_set;   /* Whether MAC address was explicitly set */
};

#endif /* ROCM_ERNIC_INTERNAL_H */
