/*
 * Compatibility Bridge: QEMU PVRDMA ↔ libvfio-user
 *
 * This header provides a clean wrapper API that isolates QEMU header
 * dependencies. Only vfu_compat_bridge.c sees QEMU internals.
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
typedef uint64_t dma_addr_t;
typedef uint64_t hwaddr;

/*
 * Opaque handle to PVRDMA device
 * 
 * This hides the QEMU PVRDMADev structure from our code.
 * Only vfu_compat_bridge.c knows the real type.
 */
typedef void* pvrdma_handle_t;

/*
 * PVRDMA Device Management
 */

/**
 * pvrdma_device_create - Create and initialize PVRDMA device
 * @vfu_dev: Parent vfu_pvrdma device structure
 * @ib_dev_name: InfiniBand device name (e.g., "mlx5_0")
 * @eth_dev_name: Ethernet device name (e.g., "eth0")
 * @port_num: IB port number (typically 1)
 *
 * Returns: Opaque handle to PVRDMA device, or NULL on failure
 */
pvrdma_handle_t pvrdma_device_create(vfu_pvrdma_dev_t *vfu_dev,
                                     const char *ib_dev_name,
                                     const char *eth_dev_name,
                                     uint8_t port_num);

/**
 * pvrdma_device_destroy - Clean up and destroy PVRDMA device
 * @handle: Device handle from pvrdma_device_create()
 */
void pvrdma_device_destroy(pvrdma_handle_t handle);

/**
 * pvrdma_device_realize - Complete device initialization
 * @handle: Device handle
 *
 * Initializes RDMA backend, loads device attributes, etc.
 * Returns: 0 on success, -errno on failure
 */
int pvrdma_device_realize(pvrdma_handle_t handle);

/*
 * Register Access (BAR1)
 */

/**
 * pvrdma_regs_write - Write to PVRDMA register
 * @handle: Device handle
 * @offset: Register offset
 * @value: Value to write
 * @size: Access size (typically 4 for 32-bit)
 */
void pvrdma_regs_write(pvrdma_handle_t handle, hwaddr offset, 
                       uint32_t value, unsigned size);

/**
 * pvrdma_regs_read - Read from PVRDMA register  
 * @handle: Device handle
 * @offset: Register offset
 * @size: Access size (typically 4 for 32-bit)
 *
 * Returns: Register value
 */
uint32_t pvrdma_regs_read(pvrdma_handle_t handle, hwaddr offset, unsigned size);

/*
 * UAR Access (BAR2)
 */

/**
 * pvrdma_uar_write - Write to User Access Region (doorbell)
 * @handle: Device handle
 * @offset: UAR offset
 * @value: Value to write
 * @size: Access size
 */
void pvrdma_uar_write(pvrdma_handle_t handle, hwaddr offset,
                      uint32_t value, unsigned size);

/**
 * pvrdma_uar_read - Read from User Access Region
 * @handle: Device handle
 * @offset: UAR offset  
 * @size: Access size
 *
 * Returns: UAR value
 */
uint32_t pvrdma_uar_read(pvrdma_handle_t handle, hwaddr offset, unsigned size);

/*
 * Command Execution
 */

/**
 * pvrdma_exec_cmd - Execute command from command ring
 * @handle: Device handle
 *
 * Processes one command from the device shared region command ring.
 * Returns: 0 on success, -errno on failure
 */
int pvrdma_exec_cmd(pvrdma_handle_t handle);

/*
 * Statistics
 */

/**
 * pvrdma_get_stats - Get device statistics
 * @handle: Device handle
 * @commands: Pointer to receive command count
 * @regs_reads: Pointer to receive register read count
 * @regs_writes: Pointer to receive register write count
 * @uar_writes: Pointer to receive UAR write count
 * @interrupts: Pointer to receive interrupt count
 */
void pvrdma_get_stats(pvrdma_handle_t handle,
                     uint64_t *commands,
                     uint64_t *regs_reads,
                     uint64_t *regs_writes,
                     uint64_t *uar_writes,
                     uint64_t *interrupts);

/*
 * Bridge Functions: DMA and Interrupts
 * 
 * These are called FROM QEMU code TO interact with libvfio-user.
 * They need access to vfu_ctx which we get from the device.
 */

/**
 * rdma_pci_dma_map - Map guest physical address to host virtual address
 * @pci_dev: PCIDevice pointer (actually our wrapper)
 * @addr: Guest physical address
 * @plen: Pointer to length (updated with actual mapped length)
 * @dir: DMA direction (not used with vfio-user)
 *
 * Returns: Host virtual address, or NULL on failure
 */
void *rdma_pci_dma_map(void *pci_dev, dma_addr_t addr, dma_addr_t *plen, int dir);

/**
 * rdma_pci_dma_unmap - Unmap previously mapped DMA region
 * @pci_dev: PCIDevice pointer
 * @buffer: Host virtual address from rdma_pci_dma_map()
 * @len: Length that was mapped
 * @dir: DMA direction
 * @access_len: How much was actually accessed
 */
void rdma_pci_dma_unmap(void *pci_dev, void *buffer, dma_addr_t len,
                        int dir, dma_addr_t access_len);

/**
 * post_interrupt - Trigger MSI-X interrupt
 * @dev: PVRDMA device pointer (actually PVRDMADev)
 * @vector: Interrupt vector number (0-2)
 *
 * Called by QEMU code to trigger interrupts.
 */
void post_interrupt(void *dev, unsigned vector);

#endif /* VFU_COMPAT_BRIDGE_H */
