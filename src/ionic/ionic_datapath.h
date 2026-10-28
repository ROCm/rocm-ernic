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

#include "ernic_limits.h"
#include "rocm_ernic_compat.h"

struct ionic_eth_emu;
struct ionic_datapath;

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
 * bounds of their own (IONIC_SRQ_*, below).
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
 *
 * The provider as proposed upstream (rdma-core #1799) rings the CQ qtype
 * for an SRQ instead, with ring 3, which no CQ doorbell uses. SRQ ids do
 * overlap CQ ids, so there the ring is what tells them apart.
 */
#define IONIC_SRQ_QID_BASE ERNIC_MAX_QP
_Static_assert(IONIC_SRQ_QID_BASE >= ERNIC_MAX_QP,
               "SRQ ids would overlap the guest's QP ids");
_Static_assert(IONIC_SRQ_QID_BASE + ERNIC_MAX_SRQ <= (1u << 24),
               "SRQ ids would not fit a 24-bit qid");

/*
 * Bounds on the ring geometry a guest may ask for in CREATE_SRQ, tighter
 * than the general ones above and checked the same way.
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
 * offset 9. A stride below the payload offset leaves those bytes unwritten,
 * so the receive is driven by whatever was on the stack. 2^5 is that offset
 * -- ionic_datapath.c static-asserts the two agree -- and the driver asks
 * for 2^6, so this rejects only geometries no real driver requests.
 */
#define IONIC_SRQ_MAX_DEPTH_LOG2  16u
#define IONIC_SRQ_MAX_STRIDE_LOG2 8u
#define IONIC_SRQ_MIN_STRIDE_LOG2 5u

/* Raised once completions have been written to a CQ. */
typedef void (*ionic_dp_cq_event_fn_t)(void *opaque, uint32_t eq_id,
                                       uint32_t cq_id);

/* Create / destroy */
struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                             struct ionic_eth_emu *eth_emu);
void ionic_datapath_destroy(struct ionic_datapath *dp);

void ionic_datapath_set_cq_event_cb(struct ionic_datapath *dp,
                                    ionic_dp_cq_event_fn_t fn, void *opaque);

/*
 * Registration, driven from the admin queue handlers in ionic_adminq.c.
 *
 * The CQ and QP registrations return 0, or a negative errno with nothing
 * registered under the id, an earlier registration of it included (except
 * that a QP refused for its geometry keeps the rings it had): -EINVAL for an
 * unknown id, a ring geometry outside the bounds the admin queue checks, or
 * a page table the device can't honour, -ENOMEM, or -EIO when the page table
 * can't be read. The caller must fail CREATE_CQ or CREATE_QP on a negative
 * return: a CQ with no ring never completes anything, and a QP with none
 * ignores its doorbells.
 */
int ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                               uint32_t eq_id,
                               const struct ionic_dp_ring_desc *ring);
void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id);

int ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
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
                                const uint8_t dgid[ERNIC_GID_LEN],
                                const uint8_t sgid[ERNIC_GID_LEN],
                                const uint8_t dmac[6], uint32_t dest_node_id);
void ionic_datapath_unregister_ah(struct ionic_datapath *dp, uint32_t ah_id);

/*
 * The Q_Key a datagram must present to be delivered into this QP.
 * MODIFY_QP carries it in the field a connected QP uses for its
 * destination QPN, so it is set separately: for UD the two never both
 * apply. Only UD and GSI consult it.
 */
void ionic_datapath_set_qkey(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t qkey);

/*
 * RC/UC peer, learned from MODIFY_QP's IB_QP_DEST_QPN and the destination
 * address in the RoCE header template.  @dest_node_id is a mesh node id;
 * when it names this instance the peer QP is local and never leaves the
 * emulator.  Pass UINT32_MAX when there is no mesh.
 */
void ionic_datapath_set_dest(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t dest_qp_id, uint32_t dest_node_id);

/*
 * This node's mesh id, and the node that owns a 16-byte destination GID,
 * or UINT32_MAX when this instance has no mesh backend.  On the TCP mesh
 * a NULL @dgid, or one no live node owns alone, also gives UINT32_MAX:
 * there is no default peer.
 */
uint32_t ionic_dp_local_node(const struct ionic_datapath *dp);
uint32_t ionic_dp_node_from_gid(const struct ionic_datapath *dp,
                                const uint8_t *dgid);

/*
 * An MR's key -- the driver's mrid, and every lkey and rkey that names the
 * MR -- is the MR's index above a key byte (ionic_mrid() in the driver's
 * ionic_fw.h). The byte is what fast registration rotates
 * (ib_update_fast_reg_key()), so every key an MR has had shares its index.
 */
#define IONIC_MRID_INDEX_SHIFT 8

/*
 * The slot the MR @key names has in the admin queue's map and the data
 * path's table: its index less one, since the driver hands out indices 1 to
 * ERNIC_MAX_MR and keeps 0 for the reserved lkey. False, with @slot
 * untouched, for a key whose index is outside that range, which names no MR
 * the device can hold.
 */
static inline bool ionic_mr_slot(uint32_t key, uint32_t *slot)
{
    uint32_t index = key >> IONIC_MRID_INDEX_SHIFT;

    if (index == 0 || index > ERNIC_MAX_MR)
        return false;
    *slot = index - 1;
    return true;
}

/*
 * MR registration.  @lkey is the driver's full mrid, key byte included, which
 * is exactly what userspace puts in an SGE.  @va/@length describe the region in
 * the client's address space; @buf resolves it to guest physical pages.  The
 * binding replaces whatever the MR's index was bound to, under any key, so
 * only @lkey reaches the region afterwards.
 *
 * Returns 0, or a negative errno with nothing registered under the MR's
 * index, under @lkey or any earlier key: -EINVAL for a key whose index the
 * device has no slot for, a region past the end of the address space or a
 * page table the device can't honour, -ENOMEM, or -EIO when the page table
 * can't be read.
 */
int ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                               uint64_t va, uint64_t length,
                               const struct ionic_dp_buf_desc *buf);

/*
 * Drop the region @key's MR is bound to, under @key or any other key byte:
 * DESTROY_MR names an MR by the key it was created with, which fast
 * registration has since rotated.
 */
void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t key);

/*
 * Set the pvrdma handle so the datapath can post sends via the backend, and
 * register for what peers send over the mesh.  Call this once, after
 * pvrdma_device_realize(), which is when this node's mesh id is known;
 * ionic_device_create() does.
 * @handle: from pvrdma_device_create().
 */
void ionic_datapath_set_pvrdma(struct ionic_datapath *dp,
                               pvrdma_handle_t handle);

/*
 * Set the statistics (ernic_stats.h) to count doorbells, WQEs, completions
 * and RDMA bytes into, or NULL for none.  They must outlive @dp.
 */
struct ernic_stats;
void ionic_datapath_set_stats(struct ionic_datapath *dp,
                              struct ernic_stats *stats);

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
