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
 * ionic BAR layout.  Checked against upstream v7.2.4:
 * drivers/net/ethernet/pensando/ionic/{ionic_if.h,ionic_dev.c,ionic_bus_pci.c}
 *
 * ionic_map_bars() walks PCI BARs 0..5, skips any without IORESOURCE_MEM,
 * and compacts what is left into ionic->bars[].  So the driver's indices
 * are positional, not PCI BAR numbers:
 *   bars[0] = register BAR   -- ionic_dev_setup() requires len >= 0x8000
 *   bars[1] = doorbell BAR   (IONIC_PCI_BAR_DBELL)
 *   bars[2] = CMB, optional  (IONIC_PCI_BAR_CMB)
 *
 * Our BAR0 is 64-bit, so it consumes PCI BAR0+BAR1 and the guest sees our
 * PCI BAR2 as bars[1].  We expose no third MEM BAR, so the driver takes
 * the "no CMB" path.
 *
 * BAR0 map.  The first 32 KB is the ionic register window, laid out
 * exactly as ionic_dev_setup() expects.  MSI-X cannot live at offset 0
 * (that is the DEVI signature the driver probes) or at 0x2000 (intr_ctrl),
 * so the table and PBA sit above the register window:
 *
 *   0x0000  dev_info_regs   (signature, fw_status, fw_heartbeat, ...)
 *   0x0800  dev_cmd_regs    (doorbell, done, cmd, comp, data)
 *   0x1000  intr_status
 *   0x2000  intr_ctrl       (32 B per vector)
 *   0x8000  MSI-X table     <- above IONIC_BAR0_SIZE, invisible to ionic
 *   0xa000  MSI-X PBA
 *   0x10000 end
 *
 * BAR2: doorbell pages -- kernel page at index kern_pid, user pages at
 * higher indices via mmap.
 * ---------------------------------------------------------------------------
 */
#define IONIC_BAR0_REGS_SIZE  0x8000u  /* 32 KB ionic register window */
#define IONIC_BAR0_MSIX_TABLE 0x8000u  /* MSI-X table, above the regs */
#define IONIC_BAR0_MSIX_PBA   0xa000u  /* MSI-X PBA                   */
#define IONIC_BAR0_TOTAL_SIZE 0x10000u /* 64 KB total BAR0            */
#define IONIC_BAR2_DB_SIZE    (4 * 1024 * 1024) /* 4 MB doorbell pages */
#define IONIC_DB_PAGE_SIZE    4096              /* one 4K page per LIF */
#define IONIC_KERN_PID        0                 /* kernel doorbell page */

/* ionic MSI-X vectors.  ionic_lif_size() budgets 1 (adminq) + one per Tx/Rx
 * queue pair + one per RDMA EQ, and ionic_create_rdma_admin() hard-fails with
 * -EINVAL below IONIC_EQ_COUNT_MIN = 4 EQs, so a 4-vector table cannot carry
 * both halves of the driver.  Advertise the full table instead. */
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
