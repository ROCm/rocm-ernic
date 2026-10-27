/*
 * Statistics: the RDMA core's counters and the stats file
 *
 * The device front end counts what it does here, through the opaque handle
 * from rocm_ernic_compat.h: MMIO accesses, interrupts, Ethernet and RDMA
 * bytes, admin commands, and per-QP doorbells, WQEs and completions. The
 * admin queue reads the totals back for the guest's STATS_VALS and
 * QP_STATS_VALS commands, and the server writes them, with what it sets
 * here about the instance, to the stats file. rocm_ernic_compat.c
 * implements it.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_STATS_H
#define ERNIC_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h> /* FILE, for pvrdma_set_stats_section() */

#include "rocm_ernic_compat.h"

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
 * The ionic path triggers the vector itself and reports it here.
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
 * @stat_op: Operation class
 */
void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op stat_op);

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

/* Device totals, as reported to the guest by the ionic STATS_VALS command. */
struct pvrdma_dev_counters {
    uint64_t commands;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t total_bytes_rdma_read;
    uint64_t total_bytes_rdma_write;
};

/* One QP's counters, as reported by the ionic QP_STATS_VALS command. */
struct pvrdma_qp_counters {
    uint64_t doorbell_send;
    uint64_t doorbell_recv;
    uint64_t wqes_processed;
    uint64_t cqes_posted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_rdma_read;
    uint64_t bytes_rdma_write;
};

/**
 * pvrdma_get_dev_counters - Snapshot the device totals
 * @handle: Device handle
 * @out: Filled on success, zeroed on failure
 *
 * Returns 0, or -EINVAL for a NULL handle or @out.
 */
int pvrdma_get_dev_counters(pvrdma_handle_t handle,
                            struct pvrdma_dev_counters *out);

/**
 * pvrdma_get_qp_counters - Snapshot one QP's counters
 * @handle: Device handle
 * @qp_id: QP to read
 * @out: Filled on success, zeroed on failure
 *
 * A QP that has not counted anything yet reads as all zeros; this does not
 * create an entry for it.  Returns 0, or -EINVAL for a NULL handle or @out.
 */
int pvrdma_get_qp_counters(pvrdma_handle_t handle, uint32_t qp_id,
                           struct pvrdma_qp_counters *out);

/*
 * The stats file
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
 * pvrdma_set_stats_nic_profile - Set the NIC profile for stats display
 * @handle:  Device handle
 * @profile: the profile chain and hash, e.g. "compat (hash 0x...)" (may be
 *           NULL; shown as "(not set)" in stats file)
 */
void pvrdma_set_stats_nic_profile(pvrdma_handle_t handle, const char *profile);

/**
 * pvrdma_set_stats_section - Add a section to the stats file
 * @handle: Device handle
 * @fn:     writes the section, after "Device Statistics" (NULL removes it)
 * @opaque: passed to @fn
 */
void pvrdma_set_stats_section(pvrdma_handle_t handle,
                              void (*fn)(FILE *fp, void *opaque), void *opaque);

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

#endif /* ERNIC_STATS_H */
