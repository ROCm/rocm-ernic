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

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;

/* BARs - from RDMA device definitions */
/* BAR sizes - Note: Also defined in pvrdma.h, so we use guards to avoid
 * redefinition warnings */
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
/* Size in bytes (0x1000 * MAX_UCS) */
#define RDMA_BAR2_UAR_SIZE (0x1000 * MAX_UCS) /* Each UC gets 4KB page */
#endif

/* MSI-X interrupt vectors */
#define RDMA_MAX_INTRS            3
#define INTR_VEC_CMD_RING         0
#define INTR_VEC_CMD_ASYNC_EVENTS 1
#define INTR_VEC_CMD_COMPLETION_Q 2

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

    /* Opaque handle to RDMA device */
    pvrdma_handle_t pvrdma_handle;

    /* BAR memory backing stores */
    void *bar0_mem; /* MSI-X table/PBA */
    void *bar1_mem; /* Registers */
    void *bar2_mem; /* UAR (User Access Region) */

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
