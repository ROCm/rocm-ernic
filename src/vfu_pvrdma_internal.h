/*
 * Internal Device Structure for vfu_pvrdma
 *
 * This header defines our main device structure without including QEMU headers.
 * We use opaque handles to hide QEMU types.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VFU_PVRDMA_INTERNAL_H
#define VFU_PVRDMA_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

#include "vfu_compat_bridge.h"

/* Forward declarations */
typedef struct vfu_pvrdma_dev vfu_pvrdma_dev_t;

/* BARs - from QEMU PVRDMA definitions */
/* BAR sizes - Note: Also defined in pvrdma.h, so we use guards to avoid
 * redefinition warnings */
#ifndef RDMA_BAR0_MSIX_SIZE
#define RDMA_BAR0_MSIX_SIZE (16 * 1024) /* 16 KB for MSI-X */
#endif
#ifndef RDMA_BAR1_REGS_SIZE
#define RDMA_BAR1_REGS_SIZE 64 /* 64 DWORDs = 256 bytes */
#endif
#ifndef RDMA_BAR2_UAR_SIZE
#define RDMA_BAR2_UAR_SIZE (4096 * 168) /* 168 User Contexts */
#endif

/* MSI-X interrupt vectors */
#define RDMA_MAX_INTRS            3
#define INTR_VEC_CMD_RING         0
#define INTR_VEC_CMD_ASYNC_EVENTS 1
#define INTR_VEC_CMD_COMPLETION_Q 2

/**
 * vfu_pvrdma_dev - Main device structure
 *
 * This structure contains both the libvfio-user context and a handle to
 * the QEMU PVRDMA device implementation. The actual QEMU structures are
 * hidden behind the opaque pvrdma_handle_t.
 */
struct vfu_pvrdma_dev {
    /* libvfio-user context */
    vfu_ctx_t *vfu_ctx;

    /* Opaque handle to QEMU PVRDMA device */
    pvrdma_handle_t pvrdma_handle;

    /* BAR memory backing stores */
    void *bar0_mem; /* MSI-X table/PBA */
    void *bar1_mem; /* Registers */
    void *bar2_mem; /* UAR (User Access Region) */

    /* Backend device configuration */
    char *backend_device_name; /* IB device (e.g., "mlx5_0") */
    char *backend_eth_device;  /* Eth device (e.g., "eth0") */
    uint8_t backend_port_num;  /* IB port number */

    /* Device state flags */
    bool device_initialized; /* Device structure created */
    bool device_realized;    /* Backend initialized */
    bool device_active;      /* Client connected and device running */
    bool verbose;            /* Verbose logging enabled */
};

#endif /* VFU_PVRDMA_INTERNAL_H */
