/*
 * Compatibility Bridge: QEMU PVRDMA ↔ libvfio-user
 *
 * This header provides the translation layer between QEMU's device model
 * and the libvfio-user framework, allowing QEMU PVRDMA code to run in
 * a standalone vfio-user server.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VFU_COMPAT_BRIDGE_H
#define VFU_COMPAT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#include <vfio-user/libvfio-user.h>

/* Forward declarations */
typedef struct vfu_pvrdma_dev vfu_pvrdma_dev_t;
typedef struct PCIDevice PCIDevice;  /* Defined in QEMU stubs */
typedef uint64_t dma_addr_t;
typedef uint64_t hwaddr;

/*
 * QEMU MemoryRegion stub - not used in vfio-user (direct BAR access)
 */
typedef struct MemoryRegion {
    uint64_t size;
    void *opaque;
} MemoryRegion;

/*
 * Bridge functions: QEMU memory/DMA → libvfio-user
 */

/**
 * rdma_pci_dma_map - Map guest physical address to host virtual address
 * 
 * QEMU version uses AddressSpace and MemoryRegion.
 * vfio-user version uses DMA region mapping.
 *
 * @pci_dev: PCI device
 * @addr: Guest physical address (DMA address)
 * @len: Length of mapping
 * @return: Host virtual address, or NULL on error
 */
void *rdma_pci_dma_map(PCIDevice *pci_dev, dma_addr_t addr, size_t len);

/**
 * rdma_pci_dma_unmap - Unmap previously mapped DMA region
 *
 * @pci_dev: PCI device
 * @buffer: Host virtual address from rdma_pci_dma_map
 * @len: Length of mapping
 */
void rdma_pci_dma_unmap(PCIDevice *pci_dev, void *buffer, size_t len);

/*
 * Bridge functions: QEMU interrupts → libvfio-user
 */

/**
 * post_interrupt - Trigger MSI-X interrupt
 *
 * @dev: PVRDMA device
 * @vector: Interrupt vector (0=cmd, 1=async, 2=cq)
 */
void post_interrupt(void *dev, unsigned vector);

/*
 * Helper macros for QEMU compatibility
 */
#define PCI_DEVICE(obj) ((PCIDevice *)(obj))
#define PVRDMA_DEV(obj) ((PVRDMADev *)(obj))

/* Page size - must match guest and host */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#endif /* VFU_COMPAT_BRIDGE_H */

