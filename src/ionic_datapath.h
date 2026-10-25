/*
 * ionic_datapath.h — ionic RDMA data-path emulation interface
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_H
#define IONIC_DATAPATH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"

struct ionic_eth_emu;
struct ionic_datapath;

/*
 * Memory-region budget.
 *
 * A stock nvme-rdma initiator runs with register_always, so it pre-allocates
 * a pool of IONIC_MR_PER_QUEUE regions for every queue it creates -- admin
 * queue included -- at connect time rather than per I/O.  An N-queue
 * controller therefore needs (N + 1) * IONIC_MR_PER_QUEUE regions to be
 * reachable before the first capsule moves.
 *
 * IONIC_MAX_MR sizes three tables that all have to hold that many: the
 * rdma_rm resource table (MAX_MR in src/qemu/rdma_rm_defs.h, which cannot
 * include this header), the data path's own dp_mr array, and the admin
 * queue's driver-id-to-handle map.  Whichever is smallest is the real
 * ceiling, so they are kept equal deliberately.
 *
 * It is not sized for the maximum 64 queues: dp_reg_mr() walks the whole
 * table on every fast registration -- that is, on every NVMe command -- so
 * the constant is also a per-I/O cost, and 2048 covers the default 8 queues
 * with headroom without doubling that walk again.  nvmeof_parse_backend()
 * rejects a queue count that would not fit.
 */
#define IONIC_MR_PER_QUEUE 128
#define IONIC_MAX_MR       2048

/* Largest max_queues that fits the budget, admin queue included. */
#define IONIC_MAX_MR_QUEUES ((IONIC_MAX_MR / IONIC_MR_PER_QUEUE) - 1)

/*
 * Description of a guest-resident ring or memory region as the driver hands
 * it to us: either a direct DMA address (map_count <= 1) or the address of a
 * page table of map_count le64 page addresses.
 */
struct ionic_dp_buf_desc {
    uint64_t dma_addr;
    uint32_t map_count;
    uint8_t page_size_log2;
};

struct ionic_dp_ring_desc {
    struct ionic_dp_buf_desc buf;
    uint8_t depth_log2;
    uint8_t stride_log2;
};

/*
 * Bounds on the ring geometry a guest may ask for.
 *
 * The driver sends each ring's depth and stride as log2 exponents, and the
 * device works with 1u << depth_log2 and 1u << stride_log2. Shifting a
 * 32-bit value by 32 or more is undefined, so every exponent from the guest
 * is checked against these before anything shifts by it, and a command
 * asking for more is refused rather than clamped: a ring of a size the
 * driver didn't ask for is one neither side agrees about. SRQs have tighter
 * bounds of their own (IONIC_SRQ_* in rocm_ernic_compat.h).
 *
 * DEPTH: 2^24 entries. The Linux driver's admin and event queues are 2^6
 * and 2^9 (IONIC_AQ_DEPTH and IONIC_EQ_DEPTH, rounded up to a power of
 * two), and the data path has always refused a deeper CQ.
 *
 * STRIDE: 2^16 bytes for the rings of fixed-size entries, the largest CQ
 * stride the data path has accepted. A send or receive queue's stride is
 * held to the max_stride the device advertises in its LIF identity
 * (IONIC_MAX_STRIDE in ionic_eth_emu_priv.h), 2^9, and the data path reads
 * a whole WQE of that size, so it never sees part of one.
 *
 * The floors are the size of what each ring holds, since entries any
 * smaller would overlap: a CQE is 32 bytes, and so is the part of a send or
 * receive WQE ahead of its payload (WQE_PLD_OFF in ionic_datapath.c), which
 * the data path reads from every WQE.
 */
#define IONIC_RING_MAX_DEPTH_LOG2  24u
#define IONIC_RING_MAX_STRIDE_LOG2 16u
#define IONIC_CQ_MIN_STRIDE_LOG2   5u
#define IONIC_WQE_MIN_STRIDE_LOG2  5u
#define IONIC_WQE_MAX_STRIDE_LOG2  9u

/* True if a ring's exponents are within the bounds above. */
static inline bool ionic_dp_ring_geometry_ok(uint8_t depth_log2,
                                             uint8_t stride_log2,
                                             uint8_t min_stride_log2,
                                             uint8_t max_stride_log2)
{
    return depth_log2 <= IONIC_RING_MAX_DEPTH_LOG2 &&
           stride_log2 >= min_stride_log2 && stride_log2 <= max_stride_log2;
}

/* A CQ's stride; the driver may leave it 0 for the size of one CQE. */
static inline uint8_t ionic_dp_cq_stride_log2(
    const struct ionic_dp_ring_desc *ring)
{
    return ring->stride_log2 ? ring->stride_log2 : IONIC_CQ_MIN_STRIDE_LOG2;
}

/* Raised once completions have been written to a CQ. */
typedef void (*ionic_dp_cq_event_fn_t)(void *opaque, uint32_t eq_id,
                                       uint32_t cq_id);

/* Create / destroy */
struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                             struct ionic_eth_emu *eth_emu);
void ionic_datapath_destroy(struct ionic_datapath *dp);

void ionic_datapath_set_cq_event_cb(struct ionic_datapath *dp,
                                    ionic_dp_cq_event_fn_t fn, void *opaque);

/* Registration, driven from the admin queue handlers in ionic_adminq.c. */
void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint32_t eq_id,
                                const struct ionic_dp_ring_desc *ring);
void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id);

void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint8_t ib_qp_type, uint32_t sq_cq_id,
                                const struct ionic_dp_ring_desc *sq,
                                uint32_t rq_cq_id,
                                const struct ionic_dp_ring_desc *rq);
void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id);

/*
 * Shared receive queues.
 *
 * An SRQ is a receive ring like a QP's own, with two differences that matter
 * here: it is named by its own qid in its own qtype space, and several QPs
 * draw from it at once. The second is why it carries a lock where a per-QP
 * RQ does not -- a local send and a message off the mesh can be delivered to
 * two different QPs on two threads, and without the lock both would take the
 * same slot.
 *
 * A QP bound to an SRQ has no RQ ring of its own: CREATE_QP leaves the rq_*
 * fields zero and puts the SRQ's qid in rq_tbl_index_srq_id (~0 means "no
 * SRQ"). Its receive completions still go to its own rq_cq_id.
 */
/*
 * Returns 0, or negative on failure: -EINVAL for an unknown id or a ring
 * geometry outside IONIC_SRQ_MAX_{DEPTH,STRIDE}_LOG2, -EIO when the guest
 * page table cannot be read. The caller must fail CREATE_SRQ on a negative
 * return. Reporting success for an SRQ with no receive ring is the one
 * outcome worth avoiding: the guest then owns a queue that accepts posts and
 * never completes them, which surfaces as a hang in the datapath rather than
 * an error from the verb that caused it.
 */
int ionic_datapath_register_srq(struct ionic_datapath *dp, uint32_t srq_id,
                                const struct ionic_dp_ring_desc *rq);
void ionic_datapath_unregister_srq(struct ionic_datapath *dp, uint32_t srq_id);
void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id);

/*
 * Begin the flush on a QP that has entered the Error state, so that work
 * requests still outstanding are completed with IB_WC_WR_FLUSH_ERR instead
 * of left to a consumer that waits for them forever. One completion per
 * queue: it names no work request, and the driver completes what it has
 * posted from its own records. Receives are left alone for a QP bound to an
 * SRQ, because those WQEs belong to the SRQ and other QPs are still using
 * them.
 */
void ionic_datapath_flush_qp(struct ionic_datapath *dp, uint32_t qp_id);

/*
 * Address handles.
 *
 * A connected QP learns its peer once, at MODIFY_QP's RTR transition. An
 * unreliable datagram QP does not have one peer: every send names its
 * destination itself, by an AH id and a destination QPN in the WQE, and the
 * AH is where the RoCE header template -- and so the destination GID that
 * says which node the peer is on -- actually lives.
 *
 * Without this table a UD send had nowhere to resolve its ah_id, so the
 * datapath fell back to q->dest_qp_id, which a UD QP never sets, and
 * delivered every datagram back to the sender's own QP.
 */
void ionic_datapath_register_ah(struct ionic_datapath *dp, uint32_t ah_id,
                                const uint8_t dgid[16], const uint8_t sgid[16],
                                const uint8_t dmac[6], uint32_t dest_node_id);
void ionic_datapath_unregister_ah(struct ionic_datapath *dp, uint32_t ah_id);

/*
 * RC/UC peer, learned from MODIFY_QP's IB_QP_DEST_QPN and the destination
 * address in the RoCE header template.  @dest_node_id is a mesh node id;
 * when it names this instance the peer QP is local and never leaves the
 * emulator.  Pass UINT32_MAX when there is no mesh.
 */
/*
 * The Q_Key a datagram must present to be delivered into this QP.
 * MODIFY_QP carries it in the field a connected QP uses for its
 * destination QPN, so it is set separately: for UD the two never both
 * apply. Only UD and GSI consult it.
 */
void ionic_datapath_set_qkey(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t qkey);
void ionic_datapath_set_dest(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t dest_qp_id, uint32_t dest_node_id);

/*
 * Resolve a 16-byte destination GID to a mesh node id, or UINT32_MAX when
 * this instance has no mesh backend.  @dgid may be NULL, which asks for the
 * default peer.
 */
uint32_t ionic_dp_local_node(const struct ionic_datapath *dp);
uint32_t ionic_dp_node_from_gid(const struct ionic_datapath *dp,
                                const uint8_t *dgid);

/*
 * MR registration.  @lkey is the driver's full mrid (index | key << 24), which
 * is exactly what userspace puts in an SGE.  @va/@length describe the region in
 * the client's address space; @buf resolves it to guest physical pages.
 */
void ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                                uint64_t va, uint64_t length,
                                const struct ionic_dp_buf_desc *buf);
void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t lkey);

/*
 * Attach the in-process NVMe-oF controller.  Once attached the data path
 * answers IB CM MADs on the guest's GSI QP and serves NVMe command capsules
 * sent to the QP numbers that handshake hands out, so a guest running stock
 * `nvme connect -t rdma` reaches a target without a second node.
 *
 * Returns false and fills @err on failure.  The data path owns the controller
 * from then on and tears it down in ionic_datapath_destroy().
 */
struct nvmeof_target_cfg;
bool ionic_datapath_attach_nvmeof(struct ionic_datapath *dp,
                                  const struct nvmeof_target_cfg *cfg,
                                  char *err, size_t errlen);

/*
 * Attach the in-process S3 object store.  Once attached the emulated NIC
 * answers ARP, ping and HTTP at the configured address, and an object
 * request that carries an x-amz-rdma-token moves its payload straight
 * between the store and the guest buffer the token describes.
 *
 * Requires the Ethernet emulator, because the control plane lives on the
 * emulated wire rather than on a host socket.  Returns false and fills
 * @err on failure; the data path owns the store from then on.
 */
struct s3_target_cfg;
bool ionic_datapath_attach_s3(struct ionic_datapath *dp,
                              const struct s3_target_cfg *cfg, char *err,
                              size_t errlen);

/*
 * Set the pvrdma handle so the datapath can post sends via the backend.
 * Call this once after ionic_device_init() and pvrdma_device_realize().
 * @handle: from pvrdma_device_create().
 */
void ionic_datapath_set_pvrdma(struct ionic_datapath *dp,
                               pvrdma_handle_t handle);

/*
 * Process a doorbell write from BAR2.
 * @qtype:        hardware queue type (decoded from BAR2 page offset)
 * @doorbell_val: 8-byte little-endian doorbell value
 */
void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val);

/*
 * Drain messages that arrived from peer instances and retire work requests
 * whose peer never answered.  Every guest DMA has to happen on the thread
 * that owns the vfio-user context, but mesh messages arrive on a backend
 * receive thread, so they are queued there and applied here.  The server's
 * main loop calls this on every iteration, like ionic_eth_emu_poll_rx().
 */
void ionic_datapath_poll(struct ionic_datapath *dp);

/* True when a peer message is queued, so the caller can skip its idle sleep. */
bool ionic_datapath_has_work(struct ionic_datapath *dp);

#ifdef ERNIC_HAVE_P4

/*
 * Apply an fp8 operation elementwise across two registered memory regions
 * and write the result to a third.
 *
 * This is in-network compute: the NIC reads both operand regions, combines
 * them pointwise, and writes a result region of the same length, instead
 * of the host reading two buffers and writing a third.  Combining two
 * partial buffers is what a collective does, so this is the primitive an
 * allreduce is built from.
 *
 * The regions are streamed in blocks rather than staged whole, so the
 * length is bounded by what a guest may ask the poll loop to do in one go
 * rather than by how much will fit on the stack.
 *
 * Note what this is NOT, yet: it calls the arithmetic directly and no P4
 * program is involved, so which operation happens is fixed by the caller
 * rather than decided by a table.  Wiring the pipeline into this path and
 * making the arithmetic an extern a program calls is what turns it into
 * something P4 governs; this function is then the extern's implementation
 * rather than the whole story.
 *
 * @op is an enum p4_fp8_op.  @len is the length of each region in bytes,
 * one fp8 value per byte, and the destination region is the same length.
 *
 * Returns 0, or a negative errno: -EINVAL for an argument out of range,
 * -EACCES for a key the guest may not name here, -EFAULT when a region
 * could not be read or written in full.
 */

/*
 * The most a single request may move.  The work happens on the thread
 * that also services the vfio-user poll loop, so this is a bound on how
 * long a guest may stall that loop, not a bound on memory -- the streaming
 * below would take any length.  A megabyte of elements is a visible pause;
 * this is the point at which that was judged acceptable, and it is one
 * constant to revisit when it is not.
 */
#define IONIC_FP8_MAP_MAX 262144u

int ionic_datapath_fp8_map(struct ionic_datapath *dp, uint8_t op,
                           uint32_t a_lkey, uint64_t a_va, uint32_t b_lkey,
                           uint64_t b_va, uint32_t len, uint32_t c_rkey,
                           uint64_t c_va);

/*
 * Convert a region of floating point from one format to another,
 * elementwise, in either direction.
 *
 * @src_fmt and @dst_fmt are enum p4_fmt values.  @in_len bytes at
 * (@a_lkey, @a_va) hold however many elements of @src_fmt that is; the
 * result is the same number of @dst_fmt elements at (@c_rkey, @c_va),
 * which is longer, shorter or the same length depending on the pair.
 *
 * This is the only operation whose destination length is not its source
 * length, so each region is checked against its own, in full, before a
 * byte moves.  Returns 0, or a negative errno.
 *
 * The destination may not overlap the source, and nothing here checks
 * that it does not.  The conversion streams a chunk at a time -- read,
 * convert, write -- so a destination that overlaps the part of the
 * source still to be read is overwritten before it is used, and the
 * operation then completes successfully with wrong results from that
 * point on.  Widening makes it easy to reach: fp16 to fp32 into the same
 * address writes two bytes for every one it has read, so the second
 * chunk's input is gone before the first chunk is finished.
 *
 * Not checked because checking it properly is not a range comparison.
 * Two different keys may be registered over the same guest pages, so
 * overlap has to be decided on what the keys resolve to rather than on
 * the keys themselves, and the honest fix is to stage the whole source
 * before writing any of it.  Until then this is the guest corrupting its
 * own memory with its own keys -- it can reach nothing it has not
 * registered, and every region is still checked for that -- so it is
 * recorded here rather than guessed at.
 */
int ionic_datapath_fmt_convert(struct ionic_datapath *dp, uint8_t src_fmt,
                               uint8_t dst_fmt, uint32_t a_lkey, uint64_t a_va,
                               uint32_t in_len, uint32_t c_rkey, uint64_t c_va);

struct p4_engine;

/*
 * Give the data path the pipeline that governs RDMA operations, or NULL to
 * take it away.
 *
 * Borrowed, not owned: the engine outlives nothing here, and the caller
 * must clear it before destroying the engine.  Until one is set, and for a
 * program that declares no rdma_metadata, the data path behaves exactly as
 * it does without P4 built at all.
 *
 * What the pipeline may do to an operation is decide: it runs after the
 * operation's own memory has been checked, so a program never sees a
 * request that is already a protection error, and it cannot name memory --
 * an extern it calls is given the regions the work request itself named.
 */
void ionic_datapath_set_p4_engine(struct ionic_datapath *dp,
                                  struct p4_engine *eng);

#endif /* ERNIC_HAVE_P4 */

/*
 * RDMA error and congestion counters, served by the STATS and QP_STATS admin
 * commands after the device and QP totals, so the guest's hw_counters
 * directory carries them.
 *
 * The names are the rdma_* fields of struct ionic_lif_stats in the driver's
 * ionic_if.h, in that order and without the prefix: they are the only
 * Pensando names for these events that are published.  The driver keeps 27
 * characters of a stat name, so resp_rx_out_of_atomic_resource is cut to
 * resp_rx_out_of_atomic_res.  Counters with no emulator event behind them yet
 * (packet sequence, duplicates, flushes, CNP and ECN) are listed so the
 * directory has the real device's shape, and read 0.
 */
#define IONIC_DP_RDMA_COUNTERS(X)                               \
    X(REQ_RX_PKT_SEQ_ERR, "req_rx_pkt_seq_err")                 \
    X(REQ_RX_RNR_RETRY_ERR, "req_rx_rnr_retry_err")             \
    X(REQ_RX_REMOTE_ACCESS_ERR, "req_rx_remote_access_err")     \
    X(REQ_RX_REMOTE_INV_REQ_ERR, "req_rx_remote_inv_req_err")   \
    X(REQ_RX_REMOTE_OPER_ERR, "req_rx_remote_oper_err")         \
    X(REQ_RX_IMPLIED_NAK_SEQ_ERR, "req_rx_implied_nak_seq_err") \
    X(REQ_RX_CQE_ERR, "req_rx_cqe_err")                         \
    X(REQ_RX_CQE_FLUSH_ERR, "req_rx_cqe_flush_err")             \
    X(REQ_RX_DUP_RESPONSES, "req_rx_dup_responses")             \
    X(REQ_RX_INVALID_PACKETS, "req_rx_invalid_packets")         \
    X(REQ_TX_LOCAL_ACCESS_ERR, "req_tx_local_access_err")       \
    X(REQ_TX_LOCAL_OPER_ERR, "req_tx_local_oper_err")           \
    X(REQ_TX_MEMORY_MGMT_ERR, "req_tx_memory_mgmt_err")         \
    X(RESP_RX_DUP_REQUESTS, "resp_rx_dup_requests")             \
    X(RESP_RX_OUT_OF_BUFFER, "resp_rx_out_of_buffer")           \
    X(RESP_RX_OUT_OF_SEQ_PKTS, "resp_rx_out_of_seq_pkts")       \
    X(RESP_RX_CQE_ERR, "resp_rx_cqe_err")                       \
    X(RESP_RX_CQE_FLUSH_ERR, "resp_rx_cqe_flush_err")           \
    X(RESP_RX_LOCAL_LEN_ERR, "resp_rx_local_len_err")           \
    X(RESP_RX_INV_REQUEST_ERR, "resp_rx_inv_request_err")       \
    X(RESP_RX_LOCAL_QP_OPER_ERR, "resp_rx_local_qp_oper_err")   \
    X(RESP_RX_OUT_OF_ATOMIC_RES, "resp_rx_out_of_atomic_res")   \
    X(RESP_TX_PKT_SEQ_ERR, "resp_tx_pkt_seq_err")               \
    X(RESP_TX_REMOTE_INV_REQ_ERR, "resp_tx_remote_inv_req_err") \
    X(RESP_TX_REMOTE_ACCESS_ERR, "resp_tx_remote_access_err")   \
    X(RESP_TX_REMOTE_OPER_ERR, "resp_tx_remote_oper_err")       \
    X(RESP_TX_RNR_RETRY_ERR, "resp_tx_rnr_retry_err")           \
    X(TX_CNP_PKTS, "tx_cnp_pkts")                               \
    X(RX_CNP_PKTS, "rx_cnp_pkts")                               \
    X(RX_ECN_PKTS, "rx_ecn_pkts")

#define IONIC_DP_RDMA_ENUM(id, name) IONIC_DP_##id,
enum ionic_dp_rdma_counter {
    IONIC_DP_RDMA_COUNTERS(IONIC_DP_RDMA_ENUM) IONIC_DP_RDMA_COUNT
};
#undef IONIC_DP_RDMA_ENUM

/* The qp_id that asks ionic_datapath_rdma_counters() for the device totals. */
#define IONIC_DP_DEVICE UINT32_MAX

/*
 * Snapshot the RDMA counters of one QP, by driver qp_id, or of the whole
 * device with IONIC_DP_DEVICE.  A QP that is not registered reads as zeros,
 * as does everything when @dp is NULL.  Call it on the thread that owns the
 * vfio-user context, which is the only one that moves them.
 */
void ionic_datapath_rdma_counters(const struct ionic_datapath *dp,
                                  uint32_t qp_id,
                                  uint64_t out[IONIC_DP_RDMA_COUNT]);

/*
 * Zero the RDMA counters, the device totals and every QP's, as firmware
 * does on a device reset.  @dp may be NULL.  Same thread as
 * ionic_datapath_rdma_counters().
 */
void ionic_datapath_reset_rdma_counters(struct ionic_datapath *dp);

/*
 * Forget every QP, SRQ, CQ, AH and MR the guest registered, each as its
 * unregister function does, as RDMA_RESET_LIF requires.  @dp may be NULL.
 * Same thread as the register functions.
 */
void ionic_datapath_reset_rdma(struct ionic_datapath *dp);

#endif /* IONIC_DATAPATH_H */
