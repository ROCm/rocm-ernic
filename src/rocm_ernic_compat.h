/*
 * Compatibility Bridge: RDMA Device ↔ libvfio-user
 *
 * This header provides a clean wrapper API that isolates QEMU header
 * dependencies. Only vfu_compat_bridge.c sees QEMU internals.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_COMPAT_H
#define ROCM_ERNIC_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#include <vfio-user/libvfio-user.h>

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;
typedef uint64_t dma_addr_t;
typedef uint64_t hwaddr;

/*
 * Opaque handle to RDMA device
 *
 * This hides the QEMU PVRDMADev structure from our code.
 * Only vfu_compat_bridge.c knows the real type.
 */
typedef void *pvrdma_handle_t;

/*
 * RDMA Device Management
 */

/**
 * pvrdma_device_create - Create and initialize RDMA device
 * @dev: Parent rocm_ernic device structure
 * @ib_dev_name: InfiniBand device name (e.g., "mlx5_0")
 * @eth_dev_name: Ethernet device name (e.g., "eth0")
 * @port_num: IB port number (typically 1)
 *
 * Returns: Opaque handle to RDMA device, or NULL on failure
 */
pvrdma_handle_t pvrdma_device_create(rocm_ernic_dev_t *dev,
                                     const char *backend_type_str,
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
void pvrdma_regs_write(pvrdma_handle_t handle, hwaddr offset, uint32_t value,
                       unsigned size);

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
void pvrdma_uar_write(pvrdma_handle_t handle, hwaddr offset, uint32_t value,
                      unsigned size);

/**
 * pvrdma_uar_read - Read from User Access Region
 * @handle: Device handle
 * @offset: UAR offset
 * @size: Access size
 *
 * Returns: UAR value
 */
uint32_t pvrdma_uar_read(pvrdma_handle_t handle, hwaddr offset, unsigned size);

/**
 * pvrdma_bar0_mmio_count - Record a BAR0 (MSI-X) MMIO access for statistics
 * @handle: Device handle
 * @is_write: true for write, false for read
 */
void pvrdma_bar0_mmio_count(pvrdma_handle_t handle, bool is_write);

/*
 * Command Execution - pvrdma_exec_cmd is declared in pvrdma.h
 */

/*
 * Statistics
 */

/**
 * pvrdma_get_stats - Get device statistics
 * @handle: Device handle
 * @commands: Pointer to receive command count (optional)
 * @regs_reads: Pointer to receive register read count (optional)
 * @regs_writes: Pointer to receive register write count (optional)
 * @uar_writes: Pointer to receive UAR write count (optional)
 * @interrupts: Pointer to receive interrupt count (optional)
 * @uar_reads: Pointer to receive UAR read count (optional)
 * @bar0_reads: Pointer to receive BAR0 read count (optional)
 * @bar0_writes: Pointer to receive BAR0 write count (optional)
 */
void pvrdma_get_stats(pvrdma_handle_t handle, uint64_t *commands,
                      uint64_t *regs_reads, uint64_t *regs_writes,
                      uint64_t *uar_writes, uint64_t *interrupts,
                      uint64_t *uar_reads, uint64_t *bar0_reads,
                      uint64_t *bar0_writes);

/**
 * pvrdma_set_stats_file - Set statistics output file path
 * @handle: Device handle
 * @stats_file: Path to stats output file (will be copied)
 */
void pvrdma_set_stats_file(pvrdma_handle_t handle, const char *stats_file);

/**
 * pvrdma_set_stats_instance_info - Set instance info for stats file display
 * @handle: Device handle
 * @socket_path: Socket path for this instance (may be NULL)
 * @backend_type_str: Full backend string e.g. loopback (may be NULL)
 */
void pvrdma_set_stats_instance_info(pvrdma_handle_t handle,
                                    const char *socket_path,
                                    const char *backend_type_str);

/**
 * pvrdma_set_stats_pci_ids - Set PCI VID:DID for stats file display
 * @handle: Device handle
 * @vid: PCI Vendor ID (e.g. 0x1022)
 * @did: PCI Device ID (e.g. 0x8000)
 */
void pvrdma_set_stats_pci_ids(pvrdma_handle_t handle, uint16_t vid,
                              uint16_t did);

/**
 * pvrdma_set_stats_connection_state - Set connection state for stats display
 * @handle: Device handle
 * @connection_str: e.g. "connected", "disconnected (lost connection)" (may be
 *                  NULL; shown as "(not set)" in stats file)
 */
void pvrdma_set_stats_connection_state(pvrdma_handle_t handle,
                                       const char *connection_str);

/**
 * pvrdma_inc_stats_flr_count - Increment PCI FLR reset count for stats
 * @handle: Device handle
 */
void pvrdma_inc_stats_flr_count(pvrdma_handle_t handle);

/**
 * pvrdma_write_stats - Write statistics to file
 * @handle: Device handle
 */
void pvrdma_write_stats(pvrdma_handle_t handle);

/*
 * Bridge Functions: DMA and Interrupts
 *
 * These are called FROM QEMU code TO interact with libvfio-user.
 * They need access to vfu_ctx which we get from the device.
 */

/* DMA functions are now pci_dma_map/unmap declared in hw/pci/pci.h */

/**
 * pvrdma_drain_pending_interrupts - deliver any CQ
 * completion interrupts queued by background threads.
 * Must be called from the main (vfio-user) thread.
 */
void pvrdma_drain_pending_interrupts(pvrdma_handle_t handle);

#endif /* ROCM_ERNIC_COMPAT_H */
