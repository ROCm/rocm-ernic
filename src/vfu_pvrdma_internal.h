/*
 * Internal header for vfu_pvrdma server
 *
 * This header defines the main device structure that bridges libvfio-user
 * with the QEMU PVRDMA implementation.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VFU_PVRDMA_INTERNAL_H
#define VFU_PVRDMA_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

/* Forward declarations - avoid pulling in full QEMU headers */
typedef struct PVRDMADev PVRDMADev;
typedef struct PCIDevice PCIDevice;
typedef uint64_t hwaddr;

/* Only include full headers in .c files that need them */
#ifdef VFU_PVRDMA_INTERNAL_IMPL
#include "from-qemu/hw/rdma/vmw/pvrdma.h"
#include "from-qemu/hw/rdma/rdma_backend.h"
#include "from-qemu/hw/rdma/rdma_rm.h"
#endif

/* Forward declarations */
typedef struct vfu_pvrdma_dev vfu_pvrdma_dev_t;

/**
 * vfu_pvrdma_dev - Main device structure
 * 
 * This structure contains both the QEMU PVRDMA device state and the
 * libvfio-user context, providing the bridge between the two frameworks.
 */
struct vfu_pvrdma_dev {
    /* libvfio-user context */
    vfu_ctx_t *vfu_ctx;
    
    /* QEMU PVRDMA device structure */
    PVRDMADev pvrdma;
    
    /* PCI Device wrapper for compatibility bridge */
    PCIDevice pci_dev;
    
    /* BAR memory backing stores */
    void *bar0_mem;             /* MSI-X BAR (16KB) */
    void *bar1_mem;             /* Register BAR (256 bytes) */
    void *bar2_mem;             /* UAR BAR (variable size) */
    
    /* Configuration from command line */
    char *backend_device_name;  /* InfiniBand device name */
    char *backend_eth_device;   /* Ethernet device name */
    uint8_t backend_port_num;   /* IB port number */
    
    /* Runtime state */
    bool verbose;
    bool device_initialized;
    bool device_active;
    
    /* DMA mapping table - for tracking vfu_addr_to_sgl mappings */
    /* TODO: Implement proper mapping table */
};

/**
 * Device register operations (from QEMU PVRDMA)
 */

/* Register read/write (BAR1) */
uint64_t pvrdma_regs_read(void *opaque, hwaddr addr, unsigned size);
void pvrdma_regs_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);

/* UAR read/write (BAR2) */
uint64_t pvrdma_uar_read(void *opaque, hwaddr addr, unsigned size);
void pvrdma_uar_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);

/* Device control */
int pvrdma_exec_cmd(PVRDMADev *dev);

/**
 * Helper functions for device initialization
 */

/* Initialize the QEMU PVRDMA device structures within our context */
int vfu_pvrdma_init_device(vfu_pvrdma_dev_t *dev);

/* Cleanup device */
void vfu_pvrdma_cleanup_device(vfu_pvrdma_dev_t *dev);

#endif /* VFU_PVRDMA_INTERNAL_H */

