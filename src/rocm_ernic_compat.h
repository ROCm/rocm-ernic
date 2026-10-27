/*
 * Compatibility Bridge: RDMA Device ↔ libvfio-user
 *
 * The interface through which the device front end (the ionic emulation and
 * the server) reaches the RDMA core in src/qemu/, so that the front end
 * doesn't include the core's headers. rocm_ernic_compat.c implements it.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_COMPAT_H
#define ROCM_ERNIC_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h> /* FILE, for pvrdma_set_stats_section() */

#include "ernic_limits.h"

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;

/*
 * Opaque handle to RDMA device
 *
 * This hides the QEMU PVRDMADev structure from our code: the struct is
 * incomplete here, and only the files that include pvrdma.h
 * (rocm_ernic_compat.c and src/rdma/pvrdma_stats.c) can see its members.
 * Unlike a void *, the compiler diagnoses any other pointer passed where a
 * handle belongs.
 *
 * The typedef alone would declare the struct, but without a separate
 * declaration IWYU asks this header to include pvrdma.h.
 */
struct PVRDMADev; // IWYU pragma: keep
typedef struct PVRDMADev *pvrdma_handle_t;

/*
 * RDMA Device Management
 */

/**
 * pvrdma_backend_name - Name the backend a --backend string selects
 * @backend_type_str: e.g. "loopback" or "nvmeof:size=1G" (may be NULL,
 *                    meaning "none")
 *
 * The name is everything before the first ':', and must match exactly.
 *
 * Returns: "none", "loopback", "tcp", "nvmeof" or "s3", or NULL if the name
 * is none of them
 */
const char *pvrdma_backend_name(const char *backend_type_str);

/**
 * pvrdma_device_create - Create and initialize RDMA device
 * @dev: Parent rocm_ernic device structure
 * @backend_type_str: Backend and its config, as pvrdma_backend_name() takes
 *
 * Returns: Opaque handle to RDMA device, or NULL on failure, including an
 * unknown backend
 */
pvrdma_handle_t pvrdma_device_create(const rocm_ernic_dev_t *dev,
                                     const char *backend_type_str);

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
 * ionic_rm_alloc_srq - Allocate a shared receive queue
 * @pd_handle: protection domain the SRQ belongs to
 * @srq_limit: low-watermark at which the device raises an SRQ_LIMIT event
 * @srq_handle: output: allocated SRQ handle
 */
int ionic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle);

/**
 * ionic_rm_dealloc_srq - Free a shared receive queue
 */
void ionic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle);

/**
 * ionic_rm_modify_srq - Change an SRQ's low-watermark
 */
int ionic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit);

/**
 * ionic_rm_query_srq - Read back an SRQ's attributes
 */
int ionic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge,
                       uint32_t *srq_limit);

/**
 * ionic_rm_alloc_pd - Allocate a protection domain
 */
int ionic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle);

/**
 * ionic_rm_dealloc_pd - Free a protection domain, once nothing uses it
 */
void ionic_rm_dealloc_pd(pvrdma_handle_t handle, uint32_t pd_handle);

/**
 * ionic_rm_alloc_qp - Allocate a queue pair
 * @handle:        pvrdma device handle
 * @pd_handle:     protection domain handle
 * @qp_type:       QP type (enum ibv_qp_type, or ERNIC_IB_QPT_GSI)
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
 *
 * An ionic_mesh_recv_fn that returns true has taken buf, which is from
 * malloc(), and must free() it on every path, rejections included.  One that
 * returns false has no room for the message yet, and leaves buf with the
 * mesh, which offers it again shortly and reads nothing more from that peer
 * meanwhile.  It must match tcp_ionic_recv_fn, which ionic_mesh_set_recv_cb()
 * casts it to.
 */
typedef bool (*ionic_mesh_recv_fn)(void *opaque, uint32_t src_node, void *buf,
                                   size_t len);

/*
 * Largest single message ionic_mesh_sendv() accepts, header included.  Mirrors
 * TCP_MAX_PAYLOAD_LEN in rdma_backend_tcp_priv.h, which static-asserts the
 * two agree.
 */
#define IONIC_MESH_MAX_MSG (16u << 20)

/*
 * Most reply bytes, headers included, that a node may have asked one peer
 * for at once.  The peer queues its replies to the node, and drops one it has
 * no room to queue, so a node that asked for more could lose them whenever it
 * falls behind reading the peer.  This is the room the peer's queue is sure
 * to have for replies, beside the most it queues of its own requests:
 * TCP_SENDQ_REPLY_BYTES - TCP_SENDQ_REQUEST_BYTES.  rdma_backend_tcp_priv.h
 * static-asserts that it fits.
 */
#define IONIC_MESH_REPLY_BUDGET (32u << 20)

/*
 * SRQ ids start above the QP id space, and they have to.
 *
 * There is no separate SRQ doorbell qtype: ionic_post_recv_common() rings
 * lif_cfg.rq_qtype for an SRQ exactly as it does for a QP's own RQ, so a
 * qtype-7 doorbell carries either a QP id or an SRQ id and the device has to
 * tell them apart by the number alone. The two come from different
 * allocators -- the driver allocates QP ids itself, while SRQ ids are
 * firmware-assigned (IONIC_LIF_RDMA_ALLOC_QID_SRQ; there is no equivalent
 * bit for QPs) -- so without a disjoint range they would collide, and an SRQ
 * doorbell would be credited to whichever QP shared its number.
 *
 * The guest's QP ids are bounded by the qp_count in the LIF identity, so
 * starting SRQ ids at that bound keeps the two apart with no table to
 * consult. The last one still has to fit the 24 bits a doorbell, and an
 * admin command's id_ver, carry a qid in.
 */
#define IONIC_SRQ_QID_BASE ERNIC_MAX_QP
_Static_assert(IONIC_SRQ_QID_BASE >= ERNIC_MAX_QP,
               "SRQ ids would overlap the guest's QP ids");
_Static_assert(IONIC_SRQ_QID_BASE + ERNIC_MAX_SRQ <= (1u << 24),
               "SRQ ids would not fit a 24-bit qid");

/*
 * Ceilings for the ring geometry exponents a guest may ask for.
 *
 * These are shift counts taken straight off the admin queue, so they are
 * attacker-controlled in the only sense that matters here: a malformed WQE
 * reaches them before anything has looked at the values. Shifting a 32-bit
 * type by 32 or more is undefined in C, so they must be bounded before use,
 * not merely clamped afterwards.
 *
 * DEPTH: 2^16 entries matches the max_srq_wr the device advertises (0xffff),
 * so no legitimate driver asks for more.
 *
 * STRIDE: 2^8. The driver's SRQ WQE is 64 bytes, so this is two doublings
 * of headroom above anything real, and it fits the WQE buffer the receive
 * path reads into (rwqe in ionic_dp_deliver_recv(), which ionic_datapath.c
 * static-asserts).
 *
 * The stride needs a floor as well as a ceiling, and for a different
 * reason. ionic_dp_deliver_recv() reads min(stride, sizeof rwqe) bytes and
 * then takes the work request id from offset 0 and the SGE count from
 * offset 9.
 * A stride below the payload offset leaves those bytes unwritten, so the
 * receive is driven by whatever was on the stack. 2^5 is that offset --
 * ionic_datapath.c static-asserts the two agree -- and the driver asks for
 * 2^6, so this rejects only geometries no real driver requests.
 */
#define IONIC_SRQ_MAX_DEPTH_LOG2  16u
#define IONIC_SRQ_MAX_STRIDE_LOG2 8u
#define IONIC_SRQ_MIN_STRIDE_LOG2 5u

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle);
uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle,
                                  const uint8_t *dest_gid_16bytes);
/*
 * Claim @gid_16bytes for this node, or withdraw it, and re-advertise to the
 * mesh.  Returns 0 on a non-mesh backend, which has no peers to tell.
 *
 * The mesh's only GID source in practice is still ERNIC_TCP_GUEST_GIDS, read
 * once at startup: nothing dispatches a guest GID-table change to here, so a
 * guest address that appears later resolves nowhere.  This is the entry
 * point that would fix that, and the gsi-mesh test is its only caller.
 */
int ionic_mesh_claim_gid(pvrdma_handle_t handle, const uint8_t *gid_16bytes,
                         bool add);
/*
 * Send a message to another node without waiting for it: 0 once queued, or a
 * negative errno.  -EAGAIN means a request (!reply) found the queue to
 * dst_node full, and is to be tried again later; a reply is queued past that
 * point.  tag is the caller's name for a request, for ionic_mesh_cancel(); a
 * reply's is ignored.  See tcp_backend_send_ionic_v().
 */
int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply);
/*
 * Whether a request of len bytes, header included, to dst_node would be
 * queued now rather than refused with -EAGAIN; true when there is no mesh.
 */
bool ionic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len);
/*
 * Take back the queued requests that haven't started to go out and whose tag
 * matches tag in the bits mask selects, so none of them reaches its peer.
 * Replies are left alone.  See tcp_backend_ionic_cancel().
 */
void ionic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask);
void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque);

#endif /* ROCM_ERNIC_COMPAT_H */
