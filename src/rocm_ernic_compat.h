/*
 * Compatibility Bridge: RDMA Device ↔ libvfio-user
 *
 * This header provides a clean wrapper API that isolates QEMU header
 * dependencies. Only vfu_compat_bridge.c sees QEMU internals.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
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

/**
 * pvrdma_bar0_mmio_count - Record a BAR0 (MSI-X) MMIO access for statistics
 * @handle: Device handle
 * @is_write: true for write, false for read
 */
void pvrdma_bar0_mmio_count(pvrdma_handle_t handle, bool is_write);

/**
 * pvrdma_uar_mmio_count - Record a doorbell-window MMIO access for statistics
 * @handle: Device handle
 * @is_write: true for write, false for read
 *
 * The ionic BAR2 doorbell page is what these counters see; the names
 * uar_reads/uar_writes are kept from the PVRDMA UAR they used to count.
 */
void pvrdma_uar_mmio_count(pvrdma_handle_t handle, bool is_write);

/**
 * pvrdma_irq_count - Record one delivered MSI-X interrupt
 * @handle: Device handle
 *
 * post_interrupt() counts its own; this is for the ionic path, which triggers
 * the vector itself.
 */
void pvrdma_irq_count(pvrdma_handle_t handle);

/**
 * pvrdma_eth_bytes_count - Record guest Ethernet bytes moved
 * @handle: Device handle
 * @bytes: Frame length
 * @is_tx: true for guest-to-host, false for host-to-guest
 */
void pvrdma_eth_bytes_count(pvrdma_handle_t handle, uint64_t bytes, bool is_tx);

/**
 * pvrdma_adminq_count - Record one executed admin-queue command
 * @handle: Device handle
 */
void pvrdma_adminq_count(pvrdma_handle_t handle);

/* Operation classes for pvrdma_rdma_bytes_count(). */
enum pvrdma_stat_op {
    PVRDMA_STAT_SEND,
    PVRDMA_STAT_RECV,
    PVRDMA_STAT_RDMA_READ,
    PVRDMA_STAT_RDMA_WRITE,
};

/* Pass as @qp_id to update only the device totals. */
#define PVRDMA_STAT_NO_QP 0xffffffffu

/**
 * pvrdma_rdma_bytes_count - Record RDMA bytes against the device and a QP
 * @handle: Device handle
 * @qp_id: QP to attribute the bytes to, or PVRDMA_STAT_NO_QP for totals only
 * @bytes: Number of bytes actually moved
 * @op: Operation class
 */
void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op op);

/**
 * pvrdma_qp_doorbell_count - Record one doorbell ring against a QP
 * @handle: Device handle
 * @qp_id: QP the doorbell targets
 * @is_send: true for the send queue, false for the receive queue
 *
 * One increment per ring, not per WQE the ring drains.
 */
void pvrdma_qp_doorbell_count(pvrdma_handle_t handle, uint32_t qp_id,
                              bool is_send);

/**
 * pvrdma_qp_wqe_count - Record one processed send WQE against a QP
 * @handle: Device handle
 * @qp_id: QP that owns the WQE
 * @pvrdma_opcode: PVRDMA_WR_* index, or >= 18 to count only the total
 *
 * Callers on the ionic path must translate their own opcode space first; see
 * ionic_op_to_pvrdma_wr() in ionic_datapath.c.
 */
void pvrdma_qp_wqe_count(pvrdma_handle_t handle, uint32_t qp_id,
                         unsigned int pvrdma_opcode);

/**
 * pvrdma_qp_cqe_count - Record one completion posted to a QP's CQ
 * @handle: Device handle
 * @qp_id: QP the completion belongs to
 */
void pvrdma_qp_cqe_count(pvrdma_handle_t handle, uint32_t qp_id);

/**
 * pvrdma_qp_stats_forget - Drop the per-QP counters for a destroyed QP
 * @handle: Device handle
 * @qp_id: QP being destroyed
 *
 * pvrdma_get_qp_stats() inserts lazily and never evicts, so without this the
 * exported QP count only ever grows and destroyed QPs keep reporting.
 */
void pvrdma_qp_stats_forget(pvrdma_handle_t handle, uint32_t qp_id);

/*
 * Command Execution - pvrdma_exec_cmd is declared in pvrdma.h
 */

/*
 * Statistics
 */

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

/**
 * ionic_rm_query_qp - Read a queue pair's attributes back out of rdma_rm
 * @qpn:          QP number (handle)
 * @state:        ibv_qp_state the device holds for the QP
 * @path_mtu:     ibv_mtu enum
 * @dest_qpn:     peer QPN, 0 when the backend tracks none
 * @access_flags: ibv_access_flags bitmask
 * @rq_psn:       receive PSN the guest last set, 0 if it never set one
 * @sq_psn:       send PSN the guest last set, 0 if it never set one
 *
 * Any out parameter may be NULL.  Returns 0 on success, -errno on failure.
 */
int ionic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn);

/**
 * Mesh access for the ionic data path.
 *
 * ionic resolves rkeys against its own guest-physical MR table rather than
 * through rdma_rm, so it cannot use the backend's RDMA messages.  It carries
 * its own protocol as an opaque mesh payload instead; these are thin
 * wrappers over the tcp_backend_* entry points so ionic_datapath.c does not
 * have to pull in the QEMU-derived headers.
 *
 * ionic_mesh_local_node() and ionic_mesh_node_from_gid() return UINT32_MAX
 * when the instance has no mesh backend (loopback or none), which the caller
 * reads as "every peer is local".
 */
typedef void (*ionic_mesh_recv_fn)(void *opaque, uint32_t src_node,
                                   const void *buf, size_t len);

/*
 * Largest single message ionic_mesh_sendv() accepts, header included.  Mirrors
 * TCP_MAX_PAYLOAD_LEN in rdma_backend_tcp.c, which static-asserts the two
 * agree.
 */
#define IONIC_MESH_MAX_MSG (16u << 20)

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle);
uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle,
                                  const uint8_t *dest_gid_16bytes);
/* Header and body stay separate all the way down to writev. */
int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len);
void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque);

#endif /* ROCM_ERNIC_COMPAT_H */
