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
 * pvrdma_inc_stats_reset_count - Increment device reset count for stats
 * @handle: Device handle
 */
void pvrdma_inc_stats_reset_count(pvrdma_handle_t handle);

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

/*
 * Thin wrappers for the ionic migration path.
 *
 * ionic_adminq.c cannot include rdma_rm.h directly (QEMU header conflict),
 * so these wrappers call rdma_rm_* on its behalf using the opaque handle.
 * Return 0 on success, -errno on failure.
 */

/**
 * ionic_rm_alloc_cq - Allocate a completion queue via rdma_rm
 * @handle: pvrdma device handle (must be realized)
 * @cqe:    number of CQ entries
 * @cq_handle: output: allocated CQ handle
 */
int ionic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe,
                      uint32_t *cq_handle);

/**
 * ionic_rm_dealloc_cq - Free a completion queue
 */
void ionic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle);

/**
 * ionic_rm_alloc_pd - Allocate a protection domain
 */
int ionic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle);

/**
 * ionic_rm_dealloc_pd - Free a protection domain
 */
void ionic_rm_dealloc_pd(pvrdma_handle_t handle, uint32_t pd_handle);

/**
 * ionic_rm_alloc_qp - Allocate a queue pair
 * @handle:        pvrdma device handle
 * @pd_handle:     protection domain handle
 * @qp_type:       QP type (enum ibv_qp_type)
 * @max_send_wr:   max send work requests
 * @max_recv_wr:   max recv work requests
 * @send_cq_handle: CQ handle for send completions
 * @recv_cq_handle: CQ handle for recv completions
 * @qpn:           output: QP number assigned
 */
int ionic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn);

/**
 * ionic_rm_dealloc_qp - Free a queue pair
 */
void ionic_rm_dealloc_qp(pvrdma_handle_t handle, uint32_t qpn);

/**
 * ionic_rm_alloc_mr - Allocate a memory region
 * @pd_handle:     protection domain
 * @access_flags:  MR access flags
 * @mr_handle:     output handle
 */
int ionic_rm_alloc_mr(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint32_t access_flags, uint32_t *mr_handle);

/**
 * ionic_rm_dealloc_mr - Free a memory region
 */
void ionic_rm_dealloc_mr(pvrdma_handle_t handle, uint32_t mr_handle);

/**
 * ionic_backend_post_send - Post a send WQE to the RDMA backend
 *
 * @qpn:         QP number (handle) in rdma_rm
 * @sge_va:      Array of SGE virtual addresses (guest VAs, host-order)
 * @sge_len:     Array of SGE lengths
 * @sge_lkey:    Array of SGE local keys
 * @num_sge:     Number of SGE entries
 * @opcode:      ionic v1 WQE opcode (IONIC_V1_OP_SEND etc.)
 *
 * Returns 0 on success, -errno on failure.
 */
int ionic_backend_post_send(pvrdma_handle_t handle, uint32_t qpn,
                            const uint64_t *sge_va, const uint32_t *sge_len,
                            const uint32_t *sge_lkey, uint32_t num_sge,
                            uint8_t opcode);

/**
 * ionic_rm_modify_qp - Modify a queue pair's state
 * @qpn:        QP number (handle)
 * @attr_mask:  IB attr mask (big-endian u32 from WQE, we convert)
 * @type_state: ionic type_state byte: bits[3:0]=to_state, bits[7:4]=from_state
 * @sq_psn:     SQ starting PSN
 * @rq_psn:     RQ starting PSN
 * @qkey:       Q-Key / destination QPN (combined le32)
 * @dest_gid:   16-byte destination GID (raw bytes, zeroed if not RC)
 */
int ionic_rm_modify_qp(pvrdma_handle_t handle, uint32_t qpn, uint32_t attr_mask,
                       uint8_t type_state, uint32_t sq_psn, uint32_t rq_psn,
                       uint32_t qkey_dest_qpn, const uint8_t *dest_gid_16bytes);

#endif /* ROCM_ERNIC_COMPAT_H */
