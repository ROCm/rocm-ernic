/*
 * ionic_datapath.c — ionic RDMA data-path emulation
 *
 * Handles the data plane for the ionic RDMA emulator:
 *
 *   1. Doorbell writes from the guest (BAR2) trigger WQE processing.  The
 *      doorbell encodes qtype (BAR2 page offset), qid, ring (0 = work,
 *      1/2 = arm CQ) and p_index.
 *
 *   2. WQE processing: read ionic_v1_wqe entries from the guest SQ, match them
 *      against the destination QP's posted RQ WQEs, copy the payload directly
 *      between guest pages, and post ionic_v1_cqe completions to both CQs.
 *
 *   3. CQ notification: when a CQ is armed, raise an EQE on its EQ so the
 *      driver's interrupt handler polls it.
 *
 * SGEs name memory by (lkey, va, len) in the *client's* address space, so
 * every access goes through the MR table registered by CREATE_MR.  The
 * reserved lkey 0 (IONIC_DMA_LKEY) means the va is already a bus address.
 *
 * Wire formats: ionic_fw.h (ionic_v1_wqe, ionic_v1_cqe, ionic_sge), as
 * ionic_datapath_fw.h spells them out.
 *
 * The in-process targets and the P4 pipeline hook in from files of their
 * own: ionic_datapath_nvmeof.c, ionic_datapath_s3.c and
 * ionic_datapath_p4.c.  Everything they share with this file is declared in
 * ionic_datapath_priv.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <syslog.h>

#include <infiniband/verbs.h>
#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "dma_range.h"
#include "ernic_dma.h"
#include "ernic_ib.h"
#include "ernic_limits.h"
#include "ernic_log.h"
#include "ernic_mesh.h"
#include "ernic_stats.h"
#include "ernic_time.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_datapath_fw.h"
#include "ionic/ionic_datapath_priv.h"
#include "ionic/ionic_eth_emu.h"
#include "net/net_headers.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"

/*
 * The stats file's per-QP "WQEs by opcode" is indexed by enum ernic_stat_wr,
 * whose numbering is a different permutation of the same operations than
 * enum ionic_v1_op, so the two spaces need an explicit translation.
 * Every ionic opcode has exactly one counterpart; the reverse does not hold
 * (LSO and the opcodes after SEND_WITH_INV other than LOCAL_INV and
 * FAST_REG_MR are unreachable here).
 */
static unsigned int ionic_op_to_stat_wr(uint8_t op)
{
    static const uint8_t map[] = {
        [IONIC_V1_OP_SEND] = ERNIC_STAT_WR_SEND,
        [IONIC_V1_OP_SEND_INV] = ERNIC_STAT_WR_SEND_WITH_INV,
        [IONIC_V1_OP_SEND_IMM] = ERNIC_STAT_WR_SEND_WITH_IMM,
        [IONIC_V1_OP_RDMA_READ] = ERNIC_STAT_WR_RDMA_READ,
        [IONIC_V1_OP_RDMA_WRITE] = ERNIC_STAT_WR_RDMA_WRITE,
        [IONIC_V1_OP_RDMA_WRITE_IMM] = ERNIC_STAT_WR_RDMA_WRITE_WITH_IMM,
        [IONIC_V1_OP_ATOMIC_CS] = ERNIC_STAT_WR_ATOMIC_CMP_AND_SWP,
        [IONIC_V1_OP_ATOMIC_FA] = ERNIC_STAT_WR_ATOMIC_FETCH_AND_ADD,
        [IONIC_V1_OP_REG_MR] = ERNIC_STAT_WR_FAST_REG_MR,
        [IONIC_V1_OP_LOCAL_INV] = ERNIC_STAT_WR_LOCAL_INV,
    };

    /* Out of range counts toward the QP total but no histogram bucket. */
    return op < sizeof(map) / sizeof(map[0]) ? map[op] : UINT_MAX;
}

/* The SRQ stride floor exists to guarantee a WQE header is actually present
 * before ionic_dp_deliver_recv() reads one. Tie the two together rather than
 * leaving a 5 and a 32 to drift apart in separate headers. */
_Static_assert((1u << IONIC_SRQ_MIN_STRIDE_LOG2) >= WQE_PLD_OFF,
               "SRQ stride floor must cover the WQE header");
_Static_assert((1u << IONIC_WQE_MIN_STRIDE_LOG2) >= WQE_PLD_OFF,
               "WQE stride floor must cover the WQE header");
/* ionic_dp_deliver_recv() reads an SRQ's WQEs into a buffer sized for a
 * QP's. */
_Static_assert(IONIC_SRQ_MAX_STRIDE_LOG2 <= IONIC_WQE_MAX_STRIDE_LOG2,
               "SRQ stride ceiling must fit the receive WQE buffer");
_Static_assert((1u << IONIC_CQ_MIN_STRIDE_LOG2) == CQE_SIZE,
               "CQ stride floor must be one CQE");

/*
 * Largest payload one remote WQE may carry: whatever the mesh will accept in a
 * single message, less our own header.  perftest goes up to 8 MiB, so a cap
 * below that turns into FAIL rows rather than slow ones.
 */
#define IONIC_WIRE_MAX_PAYLOAD \
    ((uint32_t)(ERNIC_MESH_MAX_MSG - IONIC_WIRE_HDR_LEN))

/*
 * A mesh message parked by the receive thread for the vfu thread to apply.
 * buf is the mesh's own buffer, taken over rather than copied: a message can
 * be up to ERNIC_MESH_MAX_MSG bytes.  Free with dp_inmsg_free().
 */
struct dp_inmsg {
    struct dp_inmsg *next;
    uint32_t src_node;
    size_t len;
    uint64_t first_try_ms; /* 0 until the first delivery attempt */
    bool rnr_counted;      /* resp_rx_out_of_buffer already has it */
    uint8_t *buf;          /* from malloc(), owned */
};

static void dp_inmsg_free(struct dp_inmsg *m)
{
    free(m->buf);
    free(m);
}

/* -------------------------------------------------------------------------
 * Byte accounting
 * -------------------------------------------------------------------------
 */

void ionic_dp_rdma_bytes(const struct ionic_datapath *dp, uint32_t qp_id,
                         uint64_t bytes, enum ernic_stat_op op,
                         unsigned int dirs)
{
    ernic_stats_rdma_bytes(dp->stats, qp_id, bytes, op);
    if (dirs & DP_RDMA_TX)
        ionic_eth_emu_count_rdma(dp->eth_emu, bytes, true);
    if (dirs & DP_RDMA_RX)
        ionic_eth_emu_count_rdma(dp->eth_emu, bytes, false);
}

/* -------------------------------------------------------------------------
 * Page-table backed buffers
 * -------------------------------------------------------------------------
 */

void ionic_dp_buf_release(struct dp_buf *b)
{
    free(b->pages);
    memset(b, 0, sizeof(*b));
}

/*
 * 2^(depth_log2 + stride_log2), or 0 when that is not a size any ring could
 * have.
 *
 * The exponents are guest bytes. The SRQ pair is bounded at the adminq
 * before it gets here, but the QP pair is not yet, so this must not shift by
 * the width of the type or more. Returning 0 makes ionic_dp_buf_init() refuse,
 * which is the right answer for a geometry describing more memory than exists.
 */
uint64_t ionic_dp_ring_span_bytes(uint8_t depth_log2, uint8_t stride_log2)
{
    unsigned total = (unsigned)depth_log2 + (unsigned)stride_log2;

    if (total >= 64)
        return 0;
    return 1ull << total;
}

/*
 * @span is how many bytes the region actually covers -- a ring's depth times
 * its stride, or a memory region's length. map_count is a guest u32, and the
 * page table it describes is allocated here, so without a span to check it
 * against one CREATE command asks the host for up to 2^32 * 8 == 32 GiB.
 * Several hundred of them are reachable before any table fills, which makes
 * the device's memory use a function of what the guest asks for rather than
 * of what it was given.
 *
 * A page table never needs more entries than the region needs pages, plus
 * one for a region that does not start on a page boundary. Anything beyond
 * that is not a geometry the device could honour. Nor is a table of more
 * entries than the device's page tables hold between them: a memory region's
 * span is its length, which the guest chooses too, so only that cap bounds
 * the allocation by something the guest does not choose.
 */
int ionic_dp_buf_init(const struct ionic_datapath *dp, struct dp_buf *b,
                      const struct ionic_dp_buf_desc *desc, uint64_t va,
                      uint64_t span)
{
    /* Checked and used as a local: CodeQL's cpp/uncontrolled-allocation-size
     * does not see a bound checked through a pointer. */
    uint32_t map_count = desc->map_count;

    ionic_dp_buf_release(b);
    if (desc->page_size_log2 > 30)
        return -EINVAL;
    b->page_size_log2 = desc->page_size_log2 ? desc->page_size_log2 : 12;

    uint64_t page_size = 1ull << b->page_size_log2;
    /* The pages @span covers, rounded up, and one more, as above. Rounding
     * up by adding page_size - 1 first wrapped for a span within a page of
     * 2^64, and allowed one page however many the region needed. */
    uint64_t max_pages = (span >> b->page_size_log2) +
                         ((span & (page_size - 1)) != 0 ? 1u : 0u) + 1;

    if (max_pages > ERNIC_MAX_PAGE_TABLE_ENTRIES)
        max_pages = ERNIC_MAX_PAGE_TABLE_ENTRIES;
    if (span == 0 || map_count > max_pages) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: page table of %" PRIu32
                " entries for a %" PRIu64
                "-byte region at 2^%u pages (at most %" PRIu64 ")",
                map_count, span, b->page_size_log2, max_pages);
        return -EINVAL;
    }
    b->npages = map_count;

    if (map_count <= 1) {
        /* ionic_pgtbl_dma() already folded the intra-page offset in. */
        b->base = desc->dma_addr;
        b->npages = 1;
        return 0;
    }

    b->pages = calloc(map_count, sizeof(*b->pages));
    if (!b->pages) {
        b->npages = 0;
        return -ENOMEM;
    }
    if (ernic_dma_read(dp->dma, dp->vfu_ctx, desc->dma_addr, b->pages,
                       (size_t)map_count * 8) < 0) {
        ionic_dp_buf_release(b);
        return -EIO;
    }
    /* Each entry as the guest wrote it, le64, made a value in place */
    for (uint32_t i = 0; i < map_count; i++)
        b->pages[i] = ernic_get_le64((const uint8_t *)&b->pages[i]);

    b->first_off = (uint32_t)(va & ((1ull << b->page_size_log2) - 1));
    return 0;
}

/*
 * Resolve a byte offset within a buffer to a guest physical address, and
 * report how many bytes remain contiguous from there.
 */
static uint64_t buf_gpa(const struct dp_buf *b, uint64_t off, uint64_t *run)
{
    /*
     * An offset that would carry past 2^64 names nothing. Wrapped, it would
     * name an address near zero or, through a page table, an early page:
     * memory the offset was never meant to reach.
     */
    if (b->npages <= 1 || !b->pages) {
        if (off > UINT64_MAX - b->base) {
            *run = 0;
            return 0;
        }
        *run = UINT64_MAX;
        return b->base + off;
    }
    if (off > UINT64_MAX - b->first_off) {
        *run = 0;
        return 0;
    }

    uint64_t abs = off + b->first_off;
    uint64_t psz = 1ull << b->page_size_log2;
    uint64_t idx = abs >> b->page_size_log2;
    uint64_t poff = abs & (psz - 1);

    if (idx >= b->npages) {
        *run = 0;
        return 0;
    }

    /*
     * Report every physically adjacent page that follows, not just this one.
     * A large MR is usually backed by runs of contiguous pages, and stopping at
     * each page boundary turned an 8 MiB transfer into 2048 separate DMA calls.
     */
    uint64_t gpa = b->pages[idx] + poff;
    uint64_t len = psz - poff;

    while (idx + 1 < b->npages && b->pages[idx + 1] == b->pages[idx] + psz) {
        len += psz;
        idx++;
    }

    *run = len;
    return gpa;
}

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                             struct ionic_eth_emu *eth_emu)
{
    struct ionic_datapath *dp = calloc(1, sizeof(*dp));
    if (!dp)
        return NULL;

    dp->vfu_ctx = vfu_ctx;
    dp->eth_emu = eth_emu;

    dp->qp = calloc(ERNIC_MAX_QP, sizeof(*dp->qp));
    dp->cq = calloc(ERNIC_MAX_CQ, sizeof(*dp->cq));
    dp->srq = calloc(ERNIC_MAX_SRQ, sizeof(*dp->srq));
    dp->ah = calloc(ERNIC_MAX_AH, sizeof(*dp->ah));
    dp->dma = ernic_dma_create();
    if (!dp->qp || !dp->cq || !dp->srq || !dp->ah || !dp->dma) {
        free(dp->qp);
        free(dp->cq);
        free(dp->srq);
        free(dp->ah);
        ernic_dma_destroy(dp->dma);
        free(dp);
        return NULL;
    }
    dp->qp_count = ERNIC_MAX_QP;
    dp->cq_count = ERNIC_MAX_CQ;
    dp->srq_count = ERNIC_MAX_SRQ;
    dp->ah_count = ERNIC_MAX_AH;
    dp->local_node = UINT32_MAX;
    qemu_mutex_init(&dp->rx_lock);
    /* ionic_dp_now_ms(), as now_ms is NULL; no other thread can reach dp yet */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    dp->rx_polled_ms = ernic_monotonic_ms();
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    /* One init per slot, here rather than per registration: a reader that
     * samples srq->valid outside the lock must still find a lock it can
     * take, whatever the control path has done to that slot since. */
    for (uint32_t i = 0; i < dp->srq_count; i++)
        qemu_mutex_init(&dp->srq[i].lock);
    return dp;
}

static bool dp_mesh_recv(void *opaque, uint32_t src_node, void *buf,
                         size_t len);

void ionic_datapath_set_pvrdma(struct ionic_datapath *dp,
                               pvrdma_handle_t handle)
{
    if (!dp)
        return;

    dp->pvrdma_handle = handle;
    if (!handle)
        return;

    /* Fixed once the backend is realized: a manager is node 0 from the
     * start, and a worker's realize waits for its id, which registering
     * again never changes. */
    dp->local_node = ernic_mesh_local_node(dp->pvrdma_handle);
    if (dp->local_node == UINT32_MAX) {
        vfu_log(dp->vfu_ctx, LOG_INFO,
                "ionic_datapath: no mesh backend, remote QPs unreachable");
        return;
    }

    ernic_mesh_set_recv_cb(dp->pvrdma_handle, dp_mesh_recv, dp);
    vfu_log(dp->vfu_ctx, LOG_INFO, "ionic_datapath: mesh node %u",
            dp->local_node);
}

void ionic_datapath_set_stats(struct ionic_datapath *dp,
                              struct ernic_stats *stats)
{
    if (dp)
        dp->stats = stats;
}

void ionic_datapath_set_cq_event_cb(struct ionic_datapath *dp,
                                    ionic_dp_cq_event_fn_t fn, void *opaque)
{
    if (!dp)
        return;
    dp->cq_event_fn = fn;
    dp->cq_event_opaque = opaque;
}

void ionic_datapath_destroy(struct ionic_datapath *dp)
{
    if (!dp)
        return;

    /* Waits out a receive thread already in dp_mesh_recv(), so nothing can
     * reach dp once it has returned. */
    if (dp->pvrdma_handle)
        ernic_mesh_set_recv_cb(dp->pvrdma_handle, NULL, NULL);

    ionic_dp_nvmeof_detach(dp);
    ionic_dp_s3_detach(dp);

    qemu_mutex_lock(&dp->rx_lock);
    for (struct dp_inmsg *m = dp->rx_head; m;) {
        struct dp_inmsg *next = m->next;
        dp_inmsg_free(m);
        m = next;
    }
    dp->rx_head = dp->rx_tail = NULL;
    dp->rx_fresh = 0;
    qemu_mutex_unlock(&dp->rx_lock);
    qemu_mutex_destroy(&dp->rx_lock);

    for (uint32_t i = 0; i < dp->qp_count; i++) {
        ionic_dp_buf_release(&dp->qp[i].sq_buf);
        ionic_dp_buf_release(&dp->qp[i].rq_buf);
    }
    for (uint32_t i = 0; i < dp->cq_count; i++)
        ionic_dp_buf_release(&dp->cq[i].buf);
    for (uint32_t i = 0; i < ERNIC_MAX_MR; i++)
        ionic_dp_buf_release(&dp->mr[i].buf);
    free(dp->qp);
    free(dp->cq);
    for (uint32_t i = 0; i < dp->srq_count; i++) {
        /* Each live SRQ owns the page table ionic_dp_buf_init() built for it.
         * Destroying only the mutex leaked one allocation per SRQ on every
         * normal device shutdown. The mutex is destroyed for every slot,
         * live or not, because every slot's was initialised at create.
         * Like the frees below, this relies on nothing else reaching dp
         * now. */
        ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
        if (dp->srq[i].valid)
            ionic_dp_buf_release(&dp->srq[i].buf);
        ROCM_ERNIC_WARN_THREAD_SAFETY_ON
        qemu_mutex_destroy(&dp->srq[i].lock);
    }
    free(dp->srq);
    free(dp->ah);
    ernic_dma_destroy(dp->dma);
    free(dp);
}

/* -------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------
 */

int ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                               uint32_t eq_id,
                               const struct ionic_dp_ring_desc *ring)
{
    if (!dp || !ring || cq_id >= dp->cq_count)
        return -EINVAL;

    struct ionic_cq_ring *c = &dp->cq[cq_id];
    uint8_t stride_log2 = ionic_dp_cq_stride_log2(ring);

    /*
     * The guest picks this geometry, so it has to be checked before it is
     * used: a stride below CQE_SIZE would make each completion overwrite the
     * one after it, and the shifts are undefined once the exponent reaches the
     * width of the type. The admin queue refuses such a CQ before this is
     * called; this keeps the shifts below defined whoever the caller is.
     */
    if (!ionic_dp_ring_geometry_ok(ring->depth_log2, stride_log2,
                                   IONIC_CQ_MIN_STRIDE_LOG2,
                                   IONIC_RING_MAX_STRIDE_LOG2)) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQ %u bad geometry depth=2^%u stride=2^%u",
                cq_id, ring->depth_log2, stride_log2);
        ionic_dp_buf_release(&c->buf);
        c->valid = false;
        return -EINVAL;
    }
    /*
     * depth * stride is exactly what the ring occupies -- taken from the
     * descriptor, not from c->depth, which is not assigned until this call
     * has succeeded and is zero for a CQ being registered for the first
     * time. Reading it here made the span zero and rejected every CQ in
     * the device, so nothing ever completed while data still moved.
     */
    int rc = ionic_dp_buf_init(
        dp, &c->buf, &ring->buf, 0,
        ionic_dp_ring_span_bytes(ring->depth_log2, stride_log2));
    if (rc < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQ %u page table read failed", cq_id);
        c->valid = false;
        return rc;
    }
    c->depth = 1u << ring->depth_log2;
    c->stride_log2 = stride_log2;
    c->prod = 0;
    c->cons = 0;
    c->color = true; /* the driver's cq->color also starts true */
    c->armed = false;
    c->eq_id = eq_id;
    c->valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: CQ %u depth=%u stride=2^%u eq=%u pages=%u", cq_id,
            c->depth, c->stride_log2, eq_id, c->buf.npages);
    return 0;
}

void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id)
{
    if (!dp || cq_id >= dp->cq_count)
        return;
    ionic_dp_buf_release(&dp->cq[cq_id].buf);
    dp->cq[cq_id].valid = false;
}

/*
 * Lets @q's send queue move past the WQE that was waiting: for a receive, for
 * room in the mesh's send queue, or, one it couldn't read, for the requests
 * before it.
 */
static void sq_stall_clear(struct ionic_datapath *dp, struct ionic_qp_ring *q)
{
    q->sq_fault = false;
    if (!q->sq_rnr && !q->sq_busy)
        return;
    q->sq_rnr = false;
    q->sq_busy = false;
    dp->sq_stalled--;
}

/*
 * Stops @q's send queue on a WQE for another node until the mesh has room to
 * queue it; the stalled-queue retry in ionic_datapath_poll() tries it again.
 */
static void sq_busy_set(struct ionic_datapath *dp, struct ionic_qp_ring *q)
{
    if (!q->sq_rnr && !q->sq_busy)
        dp->sq_stalled++;
    q->sq_busy = true;
}

/* Defined with the other pending_* helpers, below the transmit path. */
static uint32_t pending_cancel_qp(struct ionic_datapath *dp, uint32_t qp_id);

int ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                               uint8_t ib_qp_type, uint32_t sq_cq_id,
                               const struct ionic_dp_ring_desc *sq,
                               uint32_t rq_cq_id,
                               const struct ionic_dp_ring_desc *rq)
{
    if (!dp || !sq || !rq || qp_id >= dp->qp_count)
        return -EINVAL;

    struct ionic_qp_ring *q = &dp->qp[qp_id];

    /*
     * The depths and strides are guest bytes, shifted below. The admin queue
     * refuses a QP whose rings are out of bounds before this is called; this
     * keeps the shifts defined whoever the caller is. The RQ has no stride
     * floor here because a QP bound to an SRQ has no ring of its own and
     * leaves its geometry zero; the admin queue checks the floor when it
     * does have one.
     */
    if (!ionic_dp_ring_geometry_ok(sq->depth_log2, sq->stride_log2,
                                   IONIC_WQE_MIN_STRIDE_LOG2,
                                   IONIC_WQE_MAX_STRIDE_LOG2) ||
        !ionic_dp_ring_geometry_ok(rq->depth_log2, rq->stride_log2, 0,
                                   IONIC_WQE_MAX_STRIDE_LOG2)) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: QP %u bad geometry sq(depth=2^%u stride=2^%u) "
                "rq(depth=2^%u stride=2^%u)",
                qp_id, sq->depth_log2, sq->stride_log2, rq->depth_log2,
                rq->stride_log2);
        return -EINVAL;
    }

    int rc = ionic_dp_buf_init(
        dp, &q->sq_buf, &sq->buf, 0,
        ionic_dp_ring_span_bytes(sq->depth_log2, sq->stride_log2));
    if (rc == 0)
        rc = ionic_dp_buf_init(
            dp, &q->rq_buf, &rq->buf, 0,
            ionic_dp_ring_span_bytes(rq->depth_log2, rq->stride_log2));
    if (rc < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: QP %u page table read failed", qp_id);
        /*
         * ionic_dp_buf_init() has already released the ring it failed on,
         * and the SQ may already be the new QP's. Left valid, a QP
         * registered under this id before would resolve the released ring to
         * guest physical address 0 onwards, so it goes, as DESTROY_QP would
         * take it.
         */
        ionic_datapath_unregister_qp(dp, qp_id);
        return rc;
    }
    q->ib_qp_type = ib_qp_type;
    q->sq_depth = 1u << sq->depth_log2;
    q->rq_depth = 1u << rq->depth_log2;
    q->sq_stride_log2 = sq->stride_log2;
    q->rq_stride_log2 = rq->stride_log2;
    q->sq_cq_id = sq_cq_id;
    q->rq_cq_id = rq_cq_id;
    sq_stall_clear(dp, q);
    q->sq_cons = q->sq_prod = 0;
    q->rq_prod = q->rq_cons = 0;
    q->msn = 0;
    /*
     * A new QP, even on a qp_id still in use: what an earlier one left in
     * the pending table isn't this one's to complete (see
     * ionic_datapath_unregister_qp()), and the requests it still has queued
     * in the mesh aren't this one's to take back.
     */
    pending_cancel_qp(dp, qp_id);
    q->mesh_id = ++dp->next_mesh_id;
    q->dest_valid = false;
    q->dest_node_id = UINT32_MAX;
    /* Cleared, not left alone: qp_ids are reused, and inheriting a previous
     * QP's SRQ binding would send this one's receives to a ring it was
     * never bound to. CREATE_QP's srq field is read after this call. */
    q->has_srq = false;
    q->srq_id = 0;
    memset(q->rdma, 0, sizeof(q->rdma));
    q->valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: QP %u type=%u sq(depth=%u stride=2^%u cq=%u) "
            "rq(depth=%u stride=2^%u cq=%u)",
            qp_id, ib_qp_type, q->sq_depth, q->sq_stride_log2, sq_cq_id,
            q->rq_depth, q->rq_stride_log2, rq_cq_id);
    return 0;
}

/*
 * What the mesh calls a request: its QP's mesh_id in the high half, so a
 * QP's requests can be taken back together, and its pending entry's req_id
 * in the low.
 */
static uint64_t dp_mesh_tag(uint32_t mesh_id, uint32_t req_id)
{
    return (uint64_t)mesh_id << 32 | req_id;
}

/* Take back every request q has queued in the mesh */
static void dp_mesh_cancel_qp(const struct ionic_datapath *dp,
                              const struct ionic_qp_ring *q)
{
    ernic_mesh_cancel(dp->pvrdma_handle, dp_mesh_tag(q->mesh_id, 0),
                      UINT64_C(0xffffffff00000000));
}

/* Take back p's request, if the mesh still has it queued */
static void dp_mesh_cancel_request(const struct ionic_datapath *dp,
                                   const struct dp_pending *p)
{
    ernic_mesh_cancel(dp->pvrdma_handle, p->tag, UINT64_MAX);
}

void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    if (!dp || qp_id >= dp->qp_count)
        return;
    ionic_dp_nvmeof_drop_qp(dp, qp_id);
    /*
     * The requests this QP still has queued in the mesh go out, unlike a
     * flushed QP's: nothing has told the consumer they didn't run, and a
     * NIC has usually transmitted what was rung on its doorbell long before
     * a destroy reaches it.  Consumers count on that, posting an unsignaled
     * WRITE and destroying the QP straight after (RDMA-Tutorial's write
     * example does).  Each request carries its own copy of the payload, so
     * sending it reads no guest memory.
     *
     * But nothing comes back to this QP: its pending entries are dropped,
     * without completions, which have no QP to go to.  A reply that arrives
     * later finds no entry, so a READ's or atomic's result doesn't land in
     * memory the consumer has had back since, and a QP that takes this
     * qp_id next isn't sent this one's completions.
     */
    pending_cancel_qp(dp, qp_id);
    sq_stall_clear(dp, &dp->qp[qp_id]);
    ionic_dp_buf_release(&dp->qp[qp_id].sq_buf);
    ionic_dp_buf_release(&dp->qp[qp_id].rq_buf);
    dp->qp[qp_id].valid = false;
}

/* Callers name an SRQ by the qid the guest sees, which is
 * IONIC_SRQ_QID_BASE + index; the table is indexed from zero. Converting at
 * the boundary keeps the offset in one place. */
static struct ionic_srq_ring *srq_by_qid(struct ionic_datapath *dp,
                                         uint32_t srq_qid)
{
    if (!dp || srq_qid < IONIC_SRQ_QID_BASE)
        return NULL;
    uint32_t idx = srq_qid - IONIC_SRQ_QID_BASE;
    if (idx >= dp->srq_count)
        return NULL;
    return &dp->srq[idx];
}

int ionic_datapath_register_srq(struct ionic_datapath *dp, uint32_t srq_id,
                                const struct ionic_dp_ring_desc *rq)
{
    struct ionic_srq_ring *sr = srq_by_qid(dp, srq_id);
    if (!sr || !rq)
        return -EINVAL;

    /*
     * Bound the exponents before anything shifts by them. The caller checks
     * these too, because it can fail the command cleanly; this is the second
     * line, so that a future caller cannot reintroduce the shift by 32 that
     * is undefined behaviour in C.
     */
    if (rq->depth_log2 > IONIC_SRQ_MAX_DEPTH_LOG2 ||
        rq->stride_log2 > IONIC_SRQ_MAX_STRIDE_LOG2 ||
        rq->stride_log2 < IONIC_SRQ_MIN_STRIDE_LOG2) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: SRQ %u rejected: depth_log2=%u stride_log2=%u "
                "(depth max %u, stride %u..%u)",
                srq_id, rq->depth_log2, rq->stride_log2,
                IONIC_SRQ_MAX_DEPTH_LOG2, IONIC_SRQ_MIN_STRIDE_LOG2,
                IONIC_SRQ_MAX_STRIDE_LOG2);
        return -EINVAL;
    }

    /*
     * Everything below runs under the lock, and the lock outlives the
     * registration -- it is initialised once with the table and destroyed
     * once with it. Tying its lifetime to the SRQ instead meant a reader
     * could sample valid, be descheduled, and then lock a mutex that this
     * path had destroyed in the meantime. Clearing valid under the lock
     * does not close that: the reader has already passed the check.
     */
    qemu_mutex_lock(&sr->lock);

    if (sr->valid) {
        /* Re-registering an id the guest reused: drop the old page table
         * rather than leaking it. */
        ionic_dp_buf_release(&sr->buf);
        sr->valid = false;
    }

    if (ionic_dp_buf_init(
            dp, &sr->buf, &rq->buf, 0,
            ionic_dp_ring_span_bytes(rq->depth_log2, rq->stride_log2)) < 0) {
        qemu_mutex_unlock(&sr->lock);
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: SRQ %u page table read failed", srq_id);
        return -EIO;
    }
    sr->depth = 1u << rq->depth_log2;
    sr->stride_log2 = rq->stride_log2;
    sr->prod = sr->cons = 0;
    sr->valid = true;
    qemu_mutex_unlock(&sr->lock);

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: SRQ %u registered depth=%u stride=%u", srq_id,
            1u << rq->depth_log2, 1u << rq->stride_log2);
    return 0;
}

void ionic_datapath_unregister_srq(struct ionic_datapath *dp, uint32_t srq_id)
{
    struct ionic_srq_ring *sr = srq_by_qid(dp, srq_id);
    if (!sr)
        return;
    /* Under the lock: a receive on another thread may be mid-take. Clearing
     * valid here makes every later take fail rather than read a freed page
     * table, and the lock itself survives -- see register_srq above. The
     * fields are reset by hand rather than memset, which would scribble over
     * the mutex this and every future registration share. */
    qemu_mutex_lock(&sr->lock);
    if (sr->valid) {
        sr->valid = false;
        ionic_dp_buf_release(&sr->buf);
        sr->depth = 0;
        sr->stride_log2 = 0;
        sr->prod = sr->cons = 0;
    }
    qemu_mutex_unlock(&sr->lock);
}

void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id)
{
    if (!dp || qp_id >= dp->qp_count || !srq_by_qid(dp, srq_id))
        return;

    /* Stored as a table index, not the guest-visible qid:
     * ionic_dp_deliver_recv() is on the hot path and should not repeat the
     * arithmetic. */
    dp->qp[qp_id].has_srq = true;
    dp->qp[qp_id].srq_id = srq_id - IONIC_SRQ_QID_BASE;
    vfu_log(dp->vfu_ctx, LOG_INFO, "ionic_datapath: QP %u bound to SRQ %u",
            qp_id, srq_id);
}

void ionic_datapath_register_ah(struct ionic_datapath *dp, uint32_t ah_id,
                                const uint8_t dgid[16], const uint8_t sgid[16],
                                const uint8_t dmac[6], uint32_t dest_node_id)
{
    if (!dp || ah_id >= dp->ah_count)
        return;

    struct ionic_ah *ah = &dp->ah[ah_id];
    if (dgid)
        memcpy(ah->dgid, dgid, 16);
    else
        memset(ah->dgid, 0, 16);
    if (sgid) {
        memcpy(ah->sgid, sgid, 16);
        ah->sgid_valid = true;
    } else {
        memset(ah->sgid, 0, 16);
        ah->sgid_valid = false;
    }
    if (dmac)
        memcpy(ah->dmac, dmac, 6);
    else
        memset(ah->dmac, 0, 6);
    ah->node_id = dest_node_id;
    ah->valid = true;

    vfu_log(
        dp->vfu_ctx, LOG_INFO,
        "ionic_datapath: AH %u -> node %u mac %02x:%02x:%02x:%02x:%02x:%02x",
        ah_id, dest_node_id, ah->dmac[0], ah->dmac[1], ah->dmac[2], ah->dmac[3],
        ah->dmac[4], ah->dmac[5]);
}

void ionic_datapath_unregister_ah(struct ionic_datapath *dp, uint32_t ah_id)
{
    if (!dp || ah_id >= dp->ah_count)
        return;
    memset(&dp->ah[ah_id], 0, sizeof(dp->ah[ah_id]));
}

/*
 * The Q_Key arrives on MODIFY_QP in the field a connected QP uses for its
 * destination QPN, which is why it is set separately rather than alongside
 * the peer: for UD the two never both apply.
 */
void ionic_datapath_set_qkey(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t qkey)
{
    if (!dp || qp_id >= dp->qp_count || !dp->qp[qp_id].valid)
        return;
    dp->qp[qp_id].qkey = qkey;
    dp->qp[qp_id].qkey_valid = true;
}

void ionic_datapath_set_dest(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t dest_qp_id, uint32_t dest_node_id)
{
    if (!dp || qp_id >= dp->qp_count || !dp->qp[qp_id].valid)
        return;
    dp->qp[qp_id].dest_qp_id = dest_qp_id;
    dp->qp[qp_id].dest_node_id = dest_node_id;
    dp->qp[qp_id].dest_valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: QP %u -> peer QP %u on node %u%s", qp_id,
            dest_qp_id, dest_node_id,
            dest_node_id == dp->local_node ? " (local)" : "");
}

/*
 * This node's mesh id, or UINT32_MAX when no mesh backend is attached --
 * the sentinel dp_is_remote() uses to keep every peer local on loopback,
 * nvmeof and s3 runs.  Callers use it to tell "there is no mesh here" from
 * "the mesh has no node for this GID", which are both UINT32_MAX out of
 * ionic_dp_node_from_gid().
 */
uint32_t ionic_dp_local_node(const struct ionic_datapath *dp)
{
    if (!dp || !dp->pvrdma_handle)
        return UINT32_MAX;
    return ernic_mesh_local_node(dp->pvrdma_handle);
}

uint32_t ionic_dp_node_from_gid(const struct ionic_datapath *dp,
                                const uint8_t *dgid)
{
    if (!dp || !dp->pvrdma_handle)
        return UINT32_MAX;
    return ernic_mesh_node_from_gid(dp->pvrdma_handle, dgid);
}

static struct dp_mr *mr_find(struct ionic_datapath *dp, uint32_t lkey)
{
    for (uint32_t i = 0; i < ERNIC_MAX_MR; i++)
        if (dp->mr[i].valid && dp->mr[i].lkey == lkey)
            return &dp->mr[i];
    return NULL;
}

int ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                               uint64_t va, uint64_t length,
                               const struct ionic_dp_buf_desc *buf)
{
    if (!dp)
        return -EINVAL;

    /*
     * A region past the top of the address space is no region: the bounds
     * checks against it in ionic_dp_mr_covers() and sge_gpa() assume va +
     * length fits. Refused like a registration that fails below, with any
     * earlier binding of the key dropped rather than left in place.
     */
    if (length > UINT64_MAX - va) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: MR %#x of %" PRIu64 " bytes at %#" PRIx64
                " runs past the end of the address space",
                lkey, length, va);
        ionic_datapath_unregister_mr(dp, lkey);
        return -EINVAL;
    }

    struct dp_mr *m = mr_find(dp, lkey);
    if (!m) {
        for (uint32_t i = 0; i < ERNIC_MAX_MR; i++) {
            if (!dp->mr[i].valid) {
                m = &dp->mr[i];
                break;
            }
        }
    }
    if (!m) {
        vfu_log(dp->vfu_ctx, LOG_WARNING, "ionic_datapath: MR table full");
        return -ENOSPC;
    }

    /* A memory region's span is its length; the guest said so in
     * CREATE_MR and the rkey checks already hold it to that. */
    int rc = ionic_dp_buf_init(dp, &m->buf, buf, va, length);
    if (rc < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: MR %#x page table read failed", lkey);
        /*
         * ionic_dp_buf_init() has already released the page table of an MR
         * this key was bound to, and left a zero base in its place. Still
         * valid, the old binding would resolve every address in it to guest
         * physical address 0 onwards.
         */
        m->valid = false;
        return rc;
    }
    m->lkey = lkey;
    m->va = va;
    m->length = length;
    m->valid = true;

    /* DEBUG, not INFO: fast registration re-binds an MR per I/O, so this
     * fires once per NVMe command on the nvmeof path. */
    vfu_log(dp->vfu_ctx, LOG_DEBUG,
            "ionic_datapath: MR lkey=%#x va=%#" PRIx64 " len=%" PRIu64
            " pages=%u pgsz=2^%u",
            lkey, va, length, m->buf.npages, m->buf.page_size_log2);
    return 0;
}

void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t lkey)
{
    struct dp_mr *m = dp ? mr_find(dp, lkey) : NULL;
    if (!m)
        return;
    ionic_dp_buf_release(&m->buf);
    m->valid = false;
}

/*
 * Fast registration (IB_WR_REG_MR).  CREATE_MR allocated the MR but left it
 * unbound; this work request carries the page table and a freshly rotated
 * key.  A stock nvme-rdma initiator posts one of these ahead of every command
 * -- register_always defaults on -- so without it the keyed SGLs the NVMe-oF
 * controller relies on never name a mapped region.
 *
 * ib_update_fast_reg_key() rotates only the top byte, so the previous key for
 * the same MR id has to be dropped or the table fills one rotation at a time.
 */
/*
 * Drop every binding of @key's MR id (its low 24 bits) except @key itself,
 * or every one if @keep_key is false: the earlier rotations of the key that
 * a REG_MR replaces.
 */
static void dp_mr_drop_id(struct ionic_datapath *dp, uint32_t key,
                          bool keep_key)
{
    for (uint32_t i = 0; i < ERNIC_MAX_MR; i++) {
        if (dp->mr[i].valid && (!keep_key || dp->mr[i].lkey != key) &&
            (dp->mr[i].lkey & 0x00ffffffu) == (key & 0x00ffffffu))
            ionic_datapath_unregister_mr(dp, dp->mr[i].lkey);
    }
}

static int dp_reg_mr(struct ionic_datapath *dp, const uint8_t *wqe,
                     uint32_t key)
{
    dp_mr_drop_id(dp, key, true);

    struct ionic_dp_buf_desc buf = {
        .dma_addr = ernic_get_be64(wqe + WQE_REG_MR_DMA_ADDR_OFF),
        .map_count = ernic_get_be32(wqe + WQE_REG_MR_MAP_COUNT_OFF),
        .page_size_log2 = wqe[WQE_REG_MR_PAGE_SZ_L2_OFF],
    };

    return ionic_datapath_register_mr(
        dp, key, ernic_get_be64(wqe + WQE_REG_MR_VA_OFF),
        ernic_get_be64(wqe + WQE_REG_MR_LENGTH_OFF), &buf);
}

/* -------------------------------------------------------------------------
 * SGE address translation
 * -------------------------------------------------------------------------
 */

static void dp_fault_clear(struct ionic_datapath *dp)
{
    dp->fault = IONIC_STS_OK;
}

/* First reason wins: it is the one that stopped the transfer. */
static void dp_fault_set(struct ionic_datapath *dp, uint32_t status)
{
    if (dp->fault == IONIC_STS_OK)
        dp->fault = status;
}

static uint32_t dp_fault_status(const struct ionic_datapath *dp,
                                uint32_t fallback)
{
    return dp->fault != IONIC_STS_OK ? dp->fault : fallback;
}

/*
 * Does the MR named by @key cover [@va, @va + @len)?  rkeys and lkeys share
 * one table here, so a failed transfer cannot be blamed on the remote end just
 * because a key was involved; this is what tells the two ends apart.
 */
bool ionic_dp_mr_covers(struct ionic_datapath *dp, uint32_t key, uint64_t va,
                        uint64_t len)
{
    /* No region to hold a bus address to, but the range must still fit the
     * address space rather than wrap back round to its start. */
    if (key == 0)
        return ernic_dma_range_ok(va, len);

    struct dp_mr *m = mr_find(dp, key);

    return m && va >= m->va && len <= m->length &&
           va - m->va <= m->length - len;
}

/*
 * Resolve one (lkey, va) pair to a guest physical address, reporting how many
 * bytes stay contiguous.  lkey 0 is IONIC_DMA_LKEY: the va is already a bus
 * address, which is what the kernel uses for its own DMA-mapped buffers.
 */
static uint64_t sge_gpa(struct ionic_datapath *dp, uint32_t lkey, uint64_t va,
                        uint64_t *run)
{
    if (lkey == 0) {
        *run = UINT64_MAX;
        return va;
    }

    /* Against the offset rather than m->va + m->length, which is written
     * so that it cannot wrap. */
    struct dp_mr *m = mr_find(dp, lkey);
    if (!m || va < m->va || va - m->va >= m->length) {
        dp_fault_set(dp, IONIC_STS_LOCAL_PROT_ERR);
        *run = 0;
        return 0;
    }

    uint64_t gpa = buf_gpa(&m->buf, va - m->va, run);
    /*
     * Checking the start address is not enough: a caller asks for a length of
     * its own choosing, and on the responder path that length comes off the
     * wire.  Without this, a peer naming a small region and a large length
     * would have the transfer run past the end of it into unrelated guest
     * pages.  A single-page region reports an unbounded run, so this is the
     * only bound such a region ever gets.
     */
    uint64_t left = m->length - (va - m->va);
    if (*run > left)
        *run = left;
    return gpa;
}

/*
 * Copy @len bytes between two SGE-described regions, walking both sides page
 * by page.  Returns the number of bytes copied.
 */
static uint32_t dp_copy_sge(struct ionic_datapath *dp, uint32_t dst_lkey,
                            uint64_t dst_va, uint32_t src_lkey, uint64_t src_va,
                            uint32_t len)
{
    uint8_t bounce[4096];
    uint32_t done = 0;

    while (done < len) {
        uint64_t src_run, dst_run;
        uint64_t src = sge_gpa(dp, src_lkey, src_va + done, &src_run);
        uint64_t dst = sge_gpa(dp, dst_lkey, dst_va + done, &dst_run);
        if (!src_run || !dst_run)
            break;

        uint64_t chunk = len - done;
        if (chunk > src_run)
            chunk = src_run;
        if (chunk > dst_run)
            chunk = dst_run;
        if (chunk > sizeof(bounce))
            chunk = sizeof(bounce);
        size_t n = (size_t)chunk;

        if (ernic_dma_read(dp->dma, dp->vfu_ctx, src, bounce, n) < 0 ||
            ernic_dma_write(dp->dma, dp->vfu_ctx, dst, bounce, n) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

/*
 * Move @len bytes between an SGE-described guest region and a host buffer:
 * a mesh message's payload, an in-process target's data, or the staging
 * buffers of a P4 operation.
 */
uint32_t ionic_dp_sge_to_host(struct ionic_datapath *dp, uint32_t lkey,
                              uint64_t va, uint8_t *dst, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        uint64_t run;
        uint64_t gpa = sge_gpa(dp, lkey, va + done, &run);
        if (!run)
            break;

        uint64_t chunk = len - done;
        if (chunk > run)
            chunk = run;
        if (ernic_dma_read(dp->dma, dp->vfu_ctx, gpa, dst + done,
                           (size_t)chunk) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

uint32_t ionic_dp_host_to_sge(struct ionic_datapath *dp, uint32_t lkey,
                              uint64_t va, const uint8_t *src, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        uint64_t run;
        uint64_t gpa = sge_gpa(dp, lkey, va + done, &run);
        if (!run)
            break;

        uint64_t chunk = len - done;
        if (chunk > run)
            chunk = run;
        if (ernic_dma_write(dp->dma, dp->vfu_ctx, gpa, src + done,
                            (size_t)chunk) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

/* -------------------------------------------------------------------------
 * CQ posting
 * -------------------------------------------------------------------------
 */

/*
 * Raise the CQ's event, if it is armed.  Arming has to behave as a level and
 * not an edge: the driver's ib_req_notify_cq() reports no missed events, so it
 * will not re-poll on its own, and a CQE that landed while the CQ was disarmed
 * would wake nobody.  That is reachable whenever the guest posts more work
 * from inside its own completion handler -- nvme-rdma does exactly that, with
 * the LOCAL_INV it issues on the Connect response.
 */
static void cq_fire_event(const struct ionic_datapath *dp,
                          struct ionic_cq_ring *c, uint32_t cq_id)
{
    if (!c->armed)
        return;
    c->armed = false;
    /* eq_id is the guest's u32: one past INT_MAX has no int to convert to,
     * and ionic_eth_emu_trigger_irq() refuses it whatever it became. */
    if (dp->cq_event_fn)
        dp->cq_event_fn(dp->cq_event_opaque, c->eq_id, cq_id);
    else if (dp->eth_emu && c->eq_id <= INT_MAX)
        ionic_eth_emu_trigger_irq(dp->eth_emu, (int)c->eq_id);
}

static int cq_write(const struct ionic_datapath *dp,
                    const struct ionic_cq_ring *c, uint64_t off,
                    const uint8_t *src, size_t len)
{
    size_t done = 0;

    while (done < len) {
        uint64_t run;
        uint64_t gpa = buf_gpa(&c->buf, off + done, &run);
        if (!run)
            return -1;

        size_t chunk = len - done;
        if (run < chunk)
            chunk = (size_t)run;
        if (ernic_dma_write(dp->dma, dp->vfu_ctx, gpa, src + done, chunk) < 0)
            return -1;
        done += chunk;
    }
    return 0;
}

/*
 * Count one RDMA error event against the device and, when @qp_id names a
 * registered QP, against that QP too.
 */
static void dp_count(struct ionic_datapath *dp, uint32_t qp_id,
                     enum ionic_dp_rdma_counter ctr)
{
    dp->rdma[ctr]++;
    if (qp_id < dp->qp_count && dp->qp[qp_id].valid)
        dp->qp[qp_id].rdma[ctr]++;
}

/*
 * An error completion the guest has been handed.  Every one counts as a CQE
 * error on its side of the connection; the status then says which requester
 * error it was.  Counting here, after the write, keeps the counters to what
 * the guest could actually have seen.
 */
static void dp_count_cqe_err(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t type, uint32_t status)
{
    if (type == CQE_TYPE_RECV) {
        dp_count(dp, qp_id, IONIC_DP_RESP_RX_CQE_ERR);
        if (status == IONIC_STS_LOCAL_LEN_ERR)
            dp_count(dp, qp_id, IONIC_DP_RESP_RX_LOCAL_LEN_ERR);
        return;
    }
    if (type != CQE_TYPE_SEND_MSN)
        return;

    dp_count(dp, qp_id, IONIC_DP_REQ_RX_CQE_ERR);
    switch (status) {
    case IONIC_STS_LOCAL_ACC_ERR:
        dp_count(dp, qp_id, IONIC_DP_REQ_TX_LOCAL_ACCESS_ERR);
        break;
    case IONIC_STS_LOCAL_QP_OPER_ERR:
        dp_count(dp, qp_id, IONIC_DP_REQ_TX_LOCAL_OPER_ERR);
        break;
    case IONIC_STS_LOCAL_PROT_ERR:
        dp_count(dp, qp_id, IONIC_DP_REQ_TX_MEMORY_MGMT_ERR);
        break;
    case IONIC_STS_REMOTE_ACC_ERR:
        dp_count(dp, qp_id, IONIC_DP_REQ_RX_REMOTE_ACCESS_ERR);
        break;
    case IONIC_STS_RETRY_EXCEEDED:
        dp_count(dp, qp_id, IONIC_DP_REQ_RX_IMPLIED_NAK_SEQ_ERR);
        break;
    case IONIC_STS_RNR_RETRY_EXCEEDED:
        dp_count(dp, qp_id, IONIC_DP_REQ_RX_RNR_RETRY_ERR);
        break;
    default:
        break;
    }
}

static void cq_post(struct ionic_datapath *dp, uint32_t cq_id,
                    const uint8_t cqe_body[CQE_SIZE - 8], uint32_t status_len,
                    uint32_t type, uint32_t qid, bool error)
{
    if (cq_id >= dp->cq_count || !dp->cq[cq_id].valid)
        return;

    struct ionic_cq_ring *c = &dp->cq[cq_id];
    uint8_t cqe[CQE_SIZE];

    memcpy(cqe, cqe_body, CQE_SIZE - 8);

    ernic_put_be32(cqe + 24, status_len);

    uint32_t qtf = (uint32_t)(c->color ? CQE_COLOR_BIT : 0) |
                   (error ? CQE_ERROR_BIT : 0) | type |
                   ((qid & 0xffffffu) << 8);
    ernic_put_be32(cqe + 28, qtf);

    /*
     * No error completion goes out unannounced. One of these ends the queue
     * pair for the consumer -- a provider that polls one stops polling
     * normally and flushes everything behind it, so every later request
     * comes back IB_WC_WR_FLUSH_ERR and the thing that actually went wrong
     * is no longer visible from outside. Saying it here is the difference
     * between a named fault and a consumer that reports only that its work
     * was flushed.
     *
     * A guest can have one of these logged per work request, though, so
     * the lines are limited, and in a flood only the first few are named.
     */
    if (error && status_len != IONIC_STS_WQE_FLUSHED_ERR &&
        ionic_dp_log_ok(dp, &dp->log_cq_error, "error completion(s)"))
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: CQ %u completing QP %u in error: status %u, "
                "cqe type %u",
                cq_id, qid, status_len, type >> 5);

    uint32_t stride = 1u << c->stride_log2;
    uint64_t base = (uint64_t)(c->prod % c->depth) * stride;

    /*
     * The guest polls the colour word in the last four bytes to decide a CQE
     * is complete, so those bytes go down after the body -- and the body
     * itself is written a page-run at a time, because a small page_size_log2
     * can put the run boundary inside the CQE.
     */
    if (cq_write(dp, c, base, cqe, CQE_SIZE - 4) < 0 ||
        cq_write(dp, c, base + CQE_SIZE - 4, cqe + CQE_SIZE - 4, 4) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQE write failed for cq_id=%u", cq_id);
        return;
    }

    ernic_stats_qp_cqe(dp->stats, qid);
    if (error)
        dp_count_cqe_err(dp, qid, type, status_len);

    c->prod++;
    if (c->prod % c->depth == 0)
        c->color = !c->color;

    cq_fire_event(dp, c, cq_id);
}

/* ::ffff:a.b.c.d -- the GID an IPv4 RoCEv2 port advertises for itself. */
static bool dp_gid_is_v4mapped(const uint8_t gid[16])
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0,    0,
                                       0, 0, 0, 0, 0xff, 0xff};
    return memcmp(gid, prefix, sizeof(prefix)) == 0;
}

/* The IPv6 form: a real GRH, with the GIDs where struct ib_grh keeps them. */
static void dp_grh_ipv6(uint8_t grh[IB_GRH_SIZE], uint32_t paylen,
                        const uint8_t *sgid, const uint8_t *dgid)
{
    memset(grh, 0, IB_GRH_SIZE);
    grh[0] = 0x60; /* IPv6 version nibble, as RoCEv2 GRHs carry */
    ernic_put_be16(grh + 4, (uint16_t)paylen);
    grh[6] = 0x1b; /* IB_GRH_NEXT_HDR */
    grh[7] = 64;
    if (sgid)
        memcpy(grh + 8, sgid, 16);
    if (dgid)
        memcpy(grh + 24, dgid, 16);
}

/*
 * The IPv4 form, which is not a GRH at all: RoCEv2 over IPv4 reports the
 * packet's IPv4 header, and the IB spec puts it in the *last* 20 bytes of
 * the 40-byte field. The kernel reads it through union rdma_network_hdr,
 * which is struct iphdr behind reserved[20], so the source address lands at
 * offset 32 and the destination at 36 -- nowhere near offsets 8 and 24,
 * where the IPv6 form keeps its GIDs. Real hardware delivers exactly this.
 *
 * Which form a receiver applies is decided by the completion, not by looking
 * at the bytes: the driver turns CQE_RECV_IS_IPV4 into wc->network_hdr_type
 * and the kernel parses by that alone. So the flag and the header have to be
 * built together, or a mismatch is read as a well-formed header at the wrong
 * offsets rather than rejected -- see dp_grh_is_ipv4().
 */
static void dp_grh_ipv4(uint8_t grh[IB_GRH_SIZE], uint32_t paylen,
                        const uint8_t *sgid, const uint8_t *dgid)
{
    uint8_t *ip = grh + IB_GRH_SIZE - IP_HDR_LEN;

    memset(grh, 0, IB_GRH_SIZE);
    ip[IP_VERSION_IHL_OFF] = 0x45; /* IPv4, a 5-word header with no options */
    ernic_put_be16(ip + IP_TOTAL_LEN_OFF, (uint16_t)(IP_HDR_LEN + paylen));
    ip[IP_TTL_OFF] = 64;
    ip[IP_PROTOCOL_OFF] = IP_PROTOCOL_UDP; /* RoCEv2 rides UDP port 4791 */
    memcpy(ip + IP_SRC_OFF, sgid + 12, 4);
    memcpy(ip + IP_DST_OFF, dgid + 12, 4);
    ernic_put_be16(ip + IP_CHECKSUM_OFF,
                   net_csum_fold(net_csum_add(0, ip, IP_HDR_LEN)));
}

/*
 * Build whichever form the addresses call for. A v4-mapped pair means the
 * port is RoCEv2 over IPv4 and the receiver will parse an IPv4 header;
 * anything else -- a link-local fe80:: GID, or no address handle at all --
 * stays IPv6.
 *
 * Which form was chosen is deliberately not returned: the completion reads
 * it back off the bytes with dp_grh_is_ipv4() instead, so that a header
 * built here and one that arrived from another mesh instance are flagged by
 * the same rule, with no second copy of the decision to drift.
 */
void ionic_dp_grh_build(uint8_t grh[IB_GRH_SIZE], uint32_t paylen,
                        const uint8_t *sgid, const uint8_t *dgid)
{
    if (sgid && dgid && dp_gid_is_v4mapped(sgid) && dp_gid_is_v4mapped(dgid)) {
        dp_grh_ipv4(grh, paylen, sgid, dgid);
        return;
    }
    dp_grh_ipv6(grh, paylen, sgid, dgid);
}

/*
 * Which form a 40-byte header already in hand is in.
 *
 * The completion flag is derived from the delivered bytes rather than
 * threaded down from whoever built them, because a datagram from another
 * mesh instance arrives with its header already on the front: it was built
 * by the sender, and the mesh protocol carries no room to say which form it
 * chose. Reading it here keeps the flag true for a locally built header and
 * a received one alike, with nothing to drift.
 *
 * Both halves are checked. The IPv4 form leaves bytes 0..19 zero and starts
 * its header with a 0x4 version nibble at offset 20; the IPv6 form has 0x6
 * at offset 0 and arbitrary GID bytes at 20. Neither test alone separates
 * them -- an IPv6 GRH whose sgid happens to hold 0x4x at offset 20 would
 * pass the second.
 */
static bool dp_grh_is_ipv4(const uint8_t *grh, uint32_t len)
{
    return grh && len >= IB_GRH_SIZE && (grh[0] & 0xf0) != 0x60 &&
           (grh[IB_GRH_SIZE - 20] & 0xf0) == 0x40;
}

static void cq_post_recv(struct ionic_datapath *dp, uint32_t cq_id,
                         uint32_t qid, uint64_t rq_wqe_id, uint32_t src_qpn,
                         uint8_t recv_op, uint32_t imm_be, uint32_t byte_len,
                         bool error, const uint8_t *src_mac, bool is_ipv4)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    /* recv.wqe_id is a native u64 the driver indexes rq_meta with. */
    memcpy(body + 0, &rq_wqe_id, 8);

    uint32_t qpn_op =
        ((uint32_t)recv_op << CQE_RECV_OP_SHIFT) | (src_qpn & 0xffffffu);
    /*
     * A UD or GSI completion is only usable if it also names the sender's
     * link layer: the driver turns src_mac into IB_WC_WITH_SMAC, and ib_cm
     * refuses a MAD whose work completion does not carry one.
     */
    if (src_mac)
        memcpy(body + 12, src_mac, 6);
    /*
     * Separate from the MAC, and deliberately so. This bit becomes
     * wc->network_hdr_type, which decides where the receiver reads the
     * datagram's addresses out of the 40-byte header; setting it alongside
     * the MAC claimed IPv4 for every UD receive while dp_ud_grh() was still
     * writing the IPv6 form, so the kernel read the middle of the
     * destination GID as the source address. It must follow the header.
     */
    if (is_ipv4)
        qpn_op |= CQE_RECV_IS_IPV4;
    ernic_put_be32(body + 8, qpn_op);
    memcpy(body + 20, &imm_be, 4); /* recv.imm_data_rkey, already be32 */

    cq_post(dp, cq_id, body, byte_len, CQE_TYPE_RECV, qid, error);
}

/*
 * Tell the driver a queue has gone into flush.
 *
 * This is one completion per queue, not one per outstanding work request.
 * The flush CQE consumes no WQE and carries no wqe_id -- the driver ignores
 * the field -- it only says "this queue is flushing now", and the driver
 * then completes whatever it still has posted with IB_WC_WR_FLUSH_ERR out
 * of its own records. The device cannot do that part itself: the wr_id a
 * consumer gets back lives in driver memory and was never handed down here.
 *
 * It goes through cq_post() rather than writing the ring directly because
 * the event matters as much as the CQE. A consumer blocked in
 * ibv_get_cq_event() is woken by nothing else, and that is exactly where a
 * process sits when it has disconnected and is waiting for its last receive
 * to come back.
 */
static void cq_post_flush(struct ionic_datapath *dp, uint32_t cq_id,
                          uint32_t qid, uint32_t type)
{
    uint8_t body[CQE_SIZE - 8];

    memset(body, 0, sizeof(body));
    cq_post(dp, cq_id, body, IONIC_STS_WQE_FLUSHED_ERR, type, qid, true);
}

/*
 * A QP has entered the Error state: start the flush on both its queues.
 *
 * Every work request still outstanding has to be completed -- with
 * IB_WC_WR_FLUSH_ERR -- or a consumer waits for it forever. That is not
 * pedantry about the spec: rping keeps a receive posted for the whole
 * connection, so when it disconnects its completion thread sits in
 * ibv_get_cq_event() and the process never exits. Anything that drains its
 * CQ on teardown behaves the same way.
 *
 * The send queue is flushed even when it looks idle. A send completes when
 * the peer acknowledges it, so "nothing left here" is not the same as
 * "nothing outstanding", and the driver is the side that knows; it treats
 * this as a hint and starts flushing once it has polled everything that can
 * still complete normally.
 *
 * A QP bound to a shared receive queue gets no receive flush. The WQEs in
 * an SRQ are the SRQ's, not this QP's -- other QPs are still drawing from
 * them -- and flushing a queue that is still live would complete another
 * QP's receives out from under it.
 */
void ionic_datapath_flush_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    if (!dp || qp_id >= dp->qp_count)
        return;

    struct ionic_qp_ring *q = &dp->qp[qp_id];
    if (!q->valid)
        return;

    /*
     * Before the flush is published, not after. A pending entry holds the
     * SGE list of a READ or atomic this QP posted, naming guest memory the
     * consumer is about to get back: the flush CQE is what lets it complete
     * those work requests and free or reuse the buffer. A response that
     * arrives after that -- the peer is a whole other instance, and nothing
     * here can stop it answering -- would find the entry still valid,
     * dp_scatter() into memory that is no longer the request's, and post a
     * second completion for a work request the driver has already flushed.
     */
    uint32_t cancelled = pending_cancel_qp(dp, qp_id);

    /*
     * Nor may a request the mesh still has queued for the peer go out after
     * the flush has completed it: a WRITE or atomic would land after the
     * consumer was told it hadn't run.  A NIC doesn't transmit a work request
     * it has flushed.
     */
    dp_mesh_cancel_qp(dp, q);

    /*
     * The flush completes every send outstanding, the one held for a
     * receive included, so none of them may run afterwards: a SEND the
     * stalled-queue retry delivered once the peer posted a receive would
     * land a message the driver has already reported flushed, and complete
     * its work request a second time.
     */
    sq_stall_clear(dp, q);
    q->sq_cons = q->sq_prod;

    cq_post_flush(dp, q->sq_cq_id, qp_id, CQE_TYPE_SEND_NPG);
    if (!q->has_srq)
        cq_post_flush(dp, q->rq_cq_id, qp_id, CQE_TYPE_RECV);

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: QP %u flushed%s, %u deferred request(s) cancelled",
            qp_id, q->has_srq ? " (send only, receives belong to its SRQ)" : "",
            cancelled);
}

static void cq_post_send_msn(struct ionic_datapath *dp, uint32_t cq_id,
                             uint32_t qid, uint32_t msn, uint32_t status)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    ernic_put_be32(body + 4, msn); /* send.msg_msn */

    cq_post(dp, cq_id, body, status, CQE_TYPE_SEND_MSN, qid,
            status != IONIC_STS_OK);
}

/*
 * A send completion by SQ index, in error unless @status is IONIC_STS_OK.
 * The NPG form carries a status as the MSN form does: the driver's
 * ionic_comp_npg() reads status_length from an error CQE and reports it for
 * the work request at @sq_wqe_id. Posting it as success whatever @status
 * said told a datagram consumer that a refused send had gone.
 */
static void cq_post_send_npg(struct ionic_datapath *dp, uint32_t cq_id,
                             uint32_t qid, uint64_t sq_wqe_id, uint32_t status)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    /* send.npg_wqe_id is a native u64 masked with sq.mask by the driver. */
    memcpy(body + 16, &sq_wqe_id, 8);

    cq_post(dp, cq_id, body, status, CQE_TYPE_SEND_NPG, qid,
            status != IONIC_STS_OK);
}

/* -------------------------------------------------------------------------
 * WQE processing
 * -------------------------------------------------------------------------
 */

/* parse_sges() reads at most ERNIC_MAX_SGE, which must be every SGE the
 * largest WQE has room for. */
_Static_assert(WQE_PLD_OFF + ERNIC_MAX_SGE * 16u >=
                   1u << IONIC_WQE_MAX_STRIDE_LOG2,
               "the largest WQE holds SGEs that parse_sges() would not read");

/*
 * Read a WQE's SGE list into @out.
 *
 * Each length is the guest's own u32, and ERNIC_MAX_SGE of them can add up
 * to far more than a u32 holds. Summed in one, the total wrapped: a SEND of
 * SGEs of 0xfffffff0 and 0x20 bytes went out as a 16-byte message. Returns
 * false when the lengths add up to more than UINT32_MAX, with @out->total held
 * at UINT32_MAX: no message is that long, so a send must be refused, but as the
 * capacity of a receive it is still a bound no message exceeds.
 */
static bool parse_sges(const uint8_t *wqe, uint32_t stride, uint8_t num_sge,
                       struct dp_sge_list *out)
{
    uint32_t avail = stride > WQE_PLD_OFF ? stride - WQE_PLD_OFF : 0;
    uint32_t max_sge = avail / 16;
    uint64_t total = 0; /* ERNIC_MAX_SGE u32s, so it cannot wrap */

    out->count = 0;
    if (max_sge > ERNIC_MAX_SGE)
        max_sge = ERNIC_MAX_SGE;
    if (num_sge < max_sge)
        max_sge = num_sge;

    for (uint32_t i = 0; i < max_sge; i++) {
        const uint8_t *p = wqe + WQE_PLD_OFF + (size_t)i * 16;
        out->va[i] = ernic_get_be64(p + 0);
        out->len[i] = ernic_get_be32(p + 8);
        out->lkey[i] = ernic_get_be32(p + 12);
        total += out->len[i];
        out->count++;
    }
    out->total = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
    return total <= UINT32_MAX;
}

/*
 * Does every SGE of a work request name memory this guest registered?
 *
 * The SGEs of a send queue WQE are always local: the gather source of a
 * SEND or an RDMA WRITE, the scatter destination of an RDMA READ, the
 * result buffer of an atomic. So a key that resolves to nothing, or a
 * length that runs past the region it names, is a local protection error
 * and belongs on this QP's own send completion.
 *
 * A zero-length SGE is skipped rather than checked. It accesses nothing,
 * and the key in one is not required to be valid -- UCX's rc_verbs flush
 * posts exactly that.
 */
static bool dp_local_sges_ok(struct ionic_datapath *dp,
                             const struct dp_sge_list *l)
{
    for (uint32_t i = 0; i < l->count; i++) {
        if (l->len[i] == 0)
            continue;
        if (!ionic_dp_mr_covers(dp, l->lkey[i], l->va[i], l->len[i]))
            return false;
    }
    return true;
}

/* Walk an SGE list into or out of a linear host buffer. */
uint32_t ionic_dp_gather(struct ionic_datapath *dp, const struct dp_sge_list *l,
                         uint8_t *dst, uint32_t len)
{
    uint32_t done = 0;

    for (uint32_t i = 0; i < l->count && done < len; i++) {
        uint32_t chunk = l->len[i];
        if (chunk > len - done)
            chunk = len - done;
        if (!chunk)
            continue;

        uint32_t n =
            ionic_dp_sge_to_host(dp, l->lkey[i], l->va[i], dst + done, chunk);
        done += n;
        if (n != chunk)
            break;
    }
    return done;
}

static uint32_t dp_scatter(struct ionic_datapath *dp,
                           const struct dp_sge_list *l, const uint8_t *src,
                           uint32_t len)
{
    uint32_t done = 0;

    for (uint32_t i = 0; i < l->count && done < len; i++) {
        uint32_t chunk = l->len[i];
        if (chunk > len - done)
            chunk = len - done;
        if (!chunk)
            continue;

        uint32_t n =
            ionic_dp_host_to_sge(dp, l->lkey[i], l->va[i], src + done, chunk);
        done += n;
        if (n != chunk)
            break;
    }
    return done;
}

/* GSI is the one datagram QP type besides UD. */
static bool qp_is_datagram(const struct ionic_qp_ring *q)
{
    return q->ib_qp_type == ERNIC_IB_QPT_GSI || q->ib_qp_type == IBV_QPT_UD;
}

/*
 * Synthesize the source MAC reported with a UD/GSI completion.  There is no
 * Ethernet frame behind any of this, so the address only has to be stable and
 * locally administered; deriving it from the sending QP keeps peers apart in
 * a guest-side trace.
 */
static void dp_src_mac(const struct ionic_datapath *dp, uint32_t src_qp_id,
                       uint8_t mac[6])
{
    (void)dp;
    mac[0] = 0x02;
    mac[1] = 0x00;
    mac[2] = (uint8_t)(src_qp_id >> 24);
    mac[3] = (uint8_t)(src_qp_id >> 16);
    mac[4] = (uint8_t)(src_qp_id >> 8);
    mac[5] = (uint8_t)src_qp_id;
}

/*
 * Take the next posted receive off a receive ring, a QP's RQ or an SRQ: read
 * its WQE into *wqe_id and *dst, and advance *cons past it.  Returns false,
 * taking nothing, if the ring is empty or the WQE can't be read.  An SRQ's
 * caller holds its lock.
 */
static bool dp_ring_take(struct ionic_datapath *dp, uint32_t *cons,
                         uint32_t prod, uint32_t depth, uint8_t stride_log2,
                         const struct dp_buf *ring_buf, uint64_t *wqe_id,
                         struct dp_sge_list *dst)
{
    if (*cons == prod)
        return false;

    uint32_t stride = 1u << stride_log2;
    uint32_t slot = *cons % depth;
    uint64_t run;
    uint64_t gpa = buf_gpa(ring_buf, (uint64_t)slot * stride, &run);

    /* Big enough for the largest stride a QP's RQ or an SRQ can have, so
     * the whole WQE is read. */
    uint8_t rwqe[1u << IONIC_WQE_MAX_STRIDE_LOG2];
    uint32_t read_sz = stride < sizeof(rwqe) ? stride : (uint32_t)sizeof(rwqe);
    if (!run || run < read_sz ||
        ernic_dma_read(dp->dma, dp->vfu_ctx, gpa, rwqe, read_sz) < 0)
        return false;

    memcpy(wqe_id, rwqe + 0, 8);

    /*
     * read_sz, not stride: parse_sges() derives how many SGEs it may walk
     * from the size it is given, and only read_sz bytes were read. Passing
     * a stride larger than the buffer would authorise walking up to
     * ERNIC_MAX_SGE entries -- byte 543 -- because parse_sges() caps on
     * ERNIC_MAX_SGE and avail/16 but has no idea how much of the WQE actually
     * arrived. The ring geometry checks keep the two equal; read_sz is what
     * keeps the walk inside rwqe if they ever aren't.
     *
     * A receive whose lengths add up past UINT32_MAX can take any message,
     * which is what the total parse_sges() leaves for it says.
     */
    (void)parse_sges(rwqe, read_sz, rwqe[9], dst);

    (*cons)++;
    return true;
}

/*
 * Deliver a SEND payload into the destination QP's next posted receive.
 * Returns the number of bytes delivered, or -1 if no receive was available.
 *
 * @src describes the payload.  For a local send it names guest memory and
 * @src_host is NULL; for one that arrived from a peer instance @src_host
 * points at the received bytes and only @src->total is meaningful.
 */
int64_t ionic_dp_deliver_recv(struct ionic_datapath *dp,
                              struct ionic_qp_ring *dq, uint32_t dst_qp_id,
                              uint32_t src_qp_id, const struct dp_sge_list *src,
                              const uint8_t *src_host, uint8_t recv_op,
                              uint32_t imm_be)
{
    uint64_t rq_wqe_id = 0;
    struct dp_sge_list dst;
    bool taken;

    /*
     * Which ring the receive comes out of. A QP bound to an SRQ has no RQ
     * of its own -- CREATE_QP left its rq_* fields zero -- so reading
     * dq->rq_* for one would consume from an empty ring forever. The SRQ
     * is shared, so the take runs under its lock: two QPs being delivered
     * to on two threads would otherwise read the same slot and both
     * complete it. The take works on a copy of the WQE, so the SRQ is free
     * for the next taker before the scatter -- which can be long.
     */
    if (dq->has_srq) {
        if (dq->srq_id >= dp->srq_count)
            return -1;
        struct ionic_srq_ring *sr = &dp->srq[dq->srq_id];
        qemu_mutex_lock(&sr->lock);
        /* valid is read only under the lock. The bounds check above is the
         * one thing safe to do outside it, because srq_count never changes
         * after create. */
        taken = sr->valid &&
                dp_ring_take(dp, &sr->cons, sr->prod, sr->depth,
                             sr->stride_log2, &sr->buf, &rq_wqe_id, &dst);
        qemu_mutex_unlock(&sr->lock);
    } else {
        taken = dp_ring_take(dp, &dq->rq_cons, dq->rq_prod, dq->rq_depth,
                             dq->rq_stride_log2, &dq->rq_buf, &rq_wqe_id, &dst);
    }
    if (!taken)
        return -1;

    uint32_t copied = 0;

    dp_fault_clear(dp);

    if (recv_op == CQE_RECV_OP_RDMA_IMM) {
        /*
         * RDMA_WRITE_WITH_IMM consumes a receive but scatters nothing into
         * it: the payload already landed through the rkey.  The completion
         * still reports the length of that write, so take it from
         * @src->total rather than counting a copy that never happens.
         * Reporting zero here left the guest with a byte_len of 0 on an
         * otherwise successful completion.
         */
        copied = src->total;
    } else if (src_host) {
        /* The payload is already linear; only the receive side scatters. */
        copied = dp_scatter(dp, &dst, src_host,
                            src->total < dst.total ? src->total : dst.total);
    } else {
        /* Walk both SGE lists in lockstep, copying the overlap. */
        uint32_t si = 0, di = 0, soff = 0, doff = 0;
        while (si < src->count && di < dst.count) {
            uint32_t s_rem = src->len[si] - soff;
            uint32_t d_rem = dst.len[di] - doff;
            uint32_t chunk = s_rem < d_rem ? s_rem : d_rem;

            if (chunk) {
                uint32_t n =
                    dp_copy_sge(dp, dst.lkey[di], dst.va[di] + doff,
                                src->lkey[si], src->va[si] + soff, chunk);
                copied += n;
                if (n != chunk)
                    break;
            }
            soff += chunk;
            doff += chunk;
            if (soff == src->len[si]) {
                si++;
                soff = 0;
            }
            if (doff == dst.len[di]) {
                di++;
                doff = 0;
            }
        }
    }

    /*
     * A short delivery is only a length error when the receive buffer was the
     * thing that ran out.  If a scatter SGE would not resolve, the guest needs
     * to see that as a protection fault against its own memory, not as a
     * message it merely truncated.
     */
    bool truncated = copied < src->total;

    /*
     * A short receive completes in error, and that error is the end of the
     * queue pair as far as a consumer is concerned: a provider that sees one
     * stops polling normally and flushes the rest, so every request after it
     * comes back as IB_WC_WR_FLUSH_ERR and the original cause is nowhere in
     * sight. Saying so here is the difference between a diagnosis and a
     * consumer reporting only that its work requests were flushed, which is
     * what libfabric's rxm does and what made this expensive to find.
     *
     * The status is reported rather than named: this branch is not only the
     * buffer running out. dp_fault_status() returns the protection or access
     * fault instead when a scatter SGE would not resolve, and calling that
     * LOCAL_LEN_ERR would send whoever reads this looking for a receive that
     * was too small when the key was the problem. The capacity is dst.total,
     * what the guest posted; @copied is only what reached it, which is zero
     * when the first SGE already failed.
     */
    if (truncated) {
        uint32_t status = dp_fault_status(dp, IONIC_STS_LOCAL_LEN_ERR);

        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u receive short: %u bytes posted, %u "
                "arrived from QP %u, %u landed; completing status %u",
                dst_qp_id, dst.total, src->total, src_qp_id, copied, status);
    }

    ionic_dp_rdma_bytes(dp, dst_qp_id, copied, ERNIC_STAT_RECV, DP_RDMA_RX);

    /* GSI and UD receives carry a source MAC; nothing else does. */
    uint8_t smac[6];
    bool ud = qp_is_datagram(dq);
    if (ud)
        dp_src_mac(dp, src_qp_id, smac);

    cq_post_recv(
        dp, dq->rq_cq_id, dst_qp_id, rq_wqe_id, src_qp_id, recv_op, imm_be,
        truncated ? dp_fault_status(dp, IONIC_STS_LOCAL_LEN_ERR) : copied,
        truncated, ud ? smac : NULL,
        ud && dp_grh_is_ipv4(src_host, src->total));

    return copied;
}

/*
 * Move @len bytes between a local SGE list and a remote region named by
 * (rkey, remote_va).  The remote side is linear, so it is just an offset walk;
 * the local side is scattered.  Returns the number of bytes transferred.
 *
 * Both ends resolve through the same MR table: an rkey is an lkey here,
 * because the emulator holds every registration for the one guest it serves.
 */
static uint32_t rdma_xfer(struct ionic_datapath *dp,
                          const struct dp_sge_list *local, uint32_t rkey,
                          uint64_t remote_va, uint32_t len, bool to_remote)
{
    uint32_t done = 0;

    for (uint32_t i = 0; i < local->count && done < len; i++) {
        uint32_t chunk = local->len[i];
        if (chunk > len - done)
            chunk = len - done;
        if (!chunk)
            continue;

        uint32_t n = to_remote
                         ? dp_copy_sge(dp, rkey, remote_va + done,
                                       local->lkey[i], local->va[i], chunk)
                         : dp_copy_sge(dp, local->lkey[i], local->va[i], rkey,
                                       remote_va + done, chunk);
        done += n;
        if (n != chunk)
            break;
    }
    return done;
}

/*
 * Compare-and-swap or fetch-and-add on the 8 bytes of guest memory at @gpa,
 * setting *@old to what they held.  They hold a u64 in the guest's byte
 * order, which for the x86-64 guests the device runs under is little-endian,
 * and are read and written as one explicitly, so the result doesn't depend on
 * the host's byte order.  The operands are values: the driver converts the
 * caller's to big endian for the WQE, and undoing that yields exactly what
 * the application passed.  Returns false if either access fails.
 */
static bool dp_atomic_apply(struct ionic_datapath *dp, uint64_t gpa,
                            bool compare_swap, uint64_t swap_add,
                            uint64_t compare, uint64_t *old)
{
    uint8_t mem[8];
    uint64_t new_val;

    if (ernic_dma_read(dp->dma, dp->vfu_ctx, gpa, mem, sizeof(mem)) < 0)
        return false;
    *old = ernic_get_le64(mem);

    if (compare_swap)
        new_val = *old == compare ? swap_add : *old;
    else
        new_val = *old + swap_add;

    ernic_put_le64(mem, new_val);
    return ernic_dma_write(dp->dma, dp->vfu_ctx, gpa, mem, sizeof(mem)) >= 0;
}

/*
 * An atomic on 8 bytes of this guest's memory (dp_atomic_apply()), with the
 * original value returned to the local SGE, little-endian like the operand.
 */
static bool do_atomic(struct ionic_datapath *dp, const uint8_t *wqe,
                      bool compare_swap)
{
    uint64_t remote_va = ernic_get_be64(wqe + WQE_RDMA_VA_OFF);
    uint64_t swap_add = ernic_get_be64(wqe + WQE_ATOMIC_SWAP_ADD_OFF);
    uint64_t compare = ernic_get_be64(wqe + WQE_ATOMIC_COMPARE_OFF);
    uint32_t rkey = ernic_get_be32(wqe + WQE_RDMA_RKEY_OFF);

    uint64_t local_va = ernic_get_be64(wqe + WQE_ATOMIC_SGE_OFF);
    uint32_t local_lkey = ernic_get_be32(wqe + WQE_ATOMIC_SGE_OFF + 12);
    uint64_t run;

    /*
     * The two ends fail differently even though one table resolves both, so
     * each failure is attributed as it happens rather than inferred from the
     * false return.
     */
    if (!ionic_dp_mr_covers(dp, rkey, remote_va, 8)) {
        dp_fault_set(dp, IONIC_STS_REMOTE_ACC_ERR);
        return false;
    }

    uint64_t rgpa = sge_gpa(dp, rkey, remote_va, &run);
    if (run < 8) {
        dp_fault_set(dp, IONIC_STS_REMOTE_ACC_ERR);
        return false;
    }
    uint64_t lgpa = sge_gpa(dp, local_lkey, local_va, &run);
    if (run < 8) {
        dp_fault_set(dp, IONIC_STS_LOCAL_PROT_ERR);
        return false;
    }

    uint64_t old;
    if (!dp_atomic_apply(dp, rgpa, compare_swap, swap_add, compare, &old)) {
        dp_fault_set(dp, IONIC_STS_REMOTE_ACC_ERR);
        return false;
    }

    uint8_t old_le[8];
    ernic_put_le64(old_le, old);
    if (ernic_dma_write(dp->dma, dp->vfu_ctx, lgpa, old_le, sizeof(old_le)) <
        0) {
        dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Cross-instance transmit
 * -------------------------------------------------------------------------
 */

uint64_t ionic_dp_now_ms(const struct ionic_datapath *dp)
{
    if (dp->now_ms)
        return dp->now_ms();
    return ernic_monotonic_ms();
}

bool ionic_dp_log_ok(struct ionic_datapath *dp, ErnicLogLimit *lim,
                     const char *what)
{
    uint64_t held;

    if (!ernic_log_limit(lim, ionic_dp_now_ms(dp), &held))
        return false;
    if (held)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: %" PRIu64 " more %s not logged", held, what);
    return true;
}

static bool dp_is_remote(const struct ionic_datapath *dp,
                         const struct ionic_qp_ring *q)
{
    return dp->local_node != UINT32_MAX && q->dest_valid &&
           q->dest_node_id != UINT32_MAX && q->dest_node_id != dp->local_node;
}

static struct dp_pending *pending_alloc(struct ionic_datapath *dp)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!dp->pending[i].valid) {
            memset(&dp->pending[i], 0, sizeof(dp->pending[i]));
            dp->pending[i].valid = true;
            dp->pending[i].req_id = ++dp->next_req_id;
            dp->pending[i].deadline_ms = ionic_dp_now_ms(dp) + PENDING_MS;
            return &dp->pending[i];
        }
    }
    return NULL;
}

static struct dp_pending *pending_find(struct ionic_datapath *dp,
                                       uint32_t req_id)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (dp->pending[i].valid && dp->pending[i].req_id == req_id)
            return &dp->pending[i];
    return NULL;
}

/*
 * The reply bytes, headers included, that the requests still pending to
 * @node can bring back, which ERNIC_MESH_REPLY_BUDGET bounds.
 */
static uint64_t pending_reply_bytes(const struct ionic_datapath *dp,
                                    uint32_t node)
{
    uint64_t bytes = 0;

    for (int i = 0; i < MAX_PENDING; i++)
        if (dp->pending[i].valid && dp->pending[i].dest_node == node)
            bytes += dp->pending[i].reply_bytes;
    return bytes;
}

/*
 * Drop every deferred request of a QP that is flushing or going away, and
 * say how many.
 *
 * Dropped silently: the flush CQE the caller is about to post already tells
 * the driver to complete everything it still has outstanding on this queue,
 * so a completion here would be a second one for the same work request, and
 * a QP that is gone has nowhere for one to go. What matters is that the entry
 * stops being findable, since its SGE list names memory the consumer is free
 * to reuse the moment the flush lands or the QP is destroyed.
 */
static uint32_t pending_cancel_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    uint32_t n = 0;

    for (int i = 0; i < MAX_PENDING; i++) {
        if (dp->pending[i].valid && dp->pending[i].qp_id == qp_id) {
            dp->pending[i].valid = false;
            n++;
        }
    }
    return n;
}

/*
 * Whether @qp_id has a request pending whose completion is still to come. A
 * datagram's isn't one: its completion was posted when it was sent.
 */
static bool pending_completes_qp(const struct ionic_datapath *dp,
                                 uint32_t qp_id)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        const struct dp_pending *p = &dp->pending[i];

        if (p->valid && p->qp_id == qp_id && (p->use_msn || p->signalled))
            return true;
    }
    return false;
}

/*
 * Completes in error the WQE at @q's sq_cons, which the device couldn't read,
 * and flushes the QP, once nothing @q posted before it is still to complete.
 * Until then it does nothing, and the pending request's completion calls it
 * again.
 *
 * The drivers retire a send queue in order, so the error can't be reported
 * until every earlier work request has completed. Posted while an earlier
 * request is still waiting for its peer, the flush would have the driver
 * flush both, and report the error as a plain flush. It would also take the
 * earlier request back, so its answer would never come. A request waits at
 * most PENDING_MS, so the WQE does too.
 */
static void sq_fault_finish(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                            uint32_t qp_id)
{
    if (!q->sq_fault || pending_completes_qp(dp, qp_id))
        return;

    cq_post_send_npg(dp, q->sq_cq_id, qp_id, q->sq_cons % q->sq_depth,
                     IONIC_STS_LOCAL_QP_OPER_ERR);
    ionic_datapath_flush_qp(dp, qp_id);
}

/* Retire a deferred work request now that its peer has answered. */
static void pending_complete(struct ionic_datapath *dp, struct dp_pending *p,
                             uint32_t status)
{
    struct ionic_qp_ring *q = p->qp_id < dp->qp_count && dp->qp[p->qp_id].valid
                                  ? &dp->qp[p->qp_id]
                                  : NULL;

    if (q) {
        if (p->use_msn)
            cq_post_send_msn(dp, q->sq_cq_id, p->qp_id, p->msn, status);
        else if (p->signalled)
            cq_post_send_npg(dp, q->sq_cq_id, p->qp_id, p->wqe_id, status);
    }
    p->valid = false;

    /* That may have been the last request a WQE the device couldn't read
     * was waiting for. */
    if (q)
        sq_fault_finish(dp, q, p->qp_id);
}

/*
 * tag names a request to the mesh, for dp_mesh_cancel_qp() and
 * dp_mesh_cancel_request(); a reply's is ignored.
 */
static int dp_mesh_tx(const struct ionic_datapath *dp, uint32_t dst_node,
                      uint64_t tag, const struct ionic_wire_hdr *hdr,
                      const uint8_t *payload, uint32_t payload_len, bool reply)
{
    uint8_t wire[IONIC_WIRE_HDR_LEN];

    ionic_dp_wire_hdr_put(wire, hdr);
    return ernic_mesh_sendv(dp->pvrdma_handle, dst_node, wire, sizeof(wire),
                            payload, payload_len, tag, reply);
}

void ionic_dp_wire_hdr_put(uint8_t *wire, const struct ionic_wire_hdr *h)
{
    memset(wire, 0, IONIC_WIRE_HDR_LEN);
    ernic_put_le32(wire + IONIC_WIRE_MAGIC_OFF, h->magic);
    wire[IONIC_WIRE_OP_OFF] = h->op;
    wire[IONIC_WIRE_ATOMIC_CS_OFF] = h->atomic_cs;
    ernic_put_le32(wire + IONIC_WIRE_SRC_QP_OFF, h->src_qp_id);
    ernic_put_le32(wire + IONIC_WIRE_DST_QP_OFF, h->dst_qp_id);
    ernic_put_le32(wire + IONIC_WIRE_REQ_ID_OFF, h->req_id);
    ernic_put_le32(wire + IONIC_WIRE_RKEY_OFF, h->rkey);
    ernic_put_le32(wire + IONIC_WIRE_LENGTH_OFF, h->length);
    memcpy(wire + IONIC_WIRE_IMM_OFF, &h->imm_be, 4);
    ernic_put_le32(wire + IONIC_WIRE_STATUS_OFF, h->status);
    ernic_put_le64(wire + IONIC_WIRE_REMOTE_VA_OFF, h->remote_va);
    ernic_put_le64(wire + IONIC_WIRE_SWAP_ADD_OFF, h->swap_add);
    ernic_put_le64(wire + IONIC_WIRE_COMPARE_OFF, h->compare);
}

void ionic_dp_wire_hdr_get(struct ionic_wire_hdr *h, const uint8_t *wire)
{
    memset(h, 0, sizeof(*h));
    h->magic = ernic_get_le32(wire + IONIC_WIRE_MAGIC_OFF);
    h->op = wire[IONIC_WIRE_OP_OFF];
    h->atomic_cs = wire[IONIC_WIRE_ATOMIC_CS_OFF];
    h->src_qp_id = ernic_get_le32(wire + IONIC_WIRE_SRC_QP_OFF);
    h->dst_qp_id = ernic_get_le32(wire + IONIC_WIRE_DST_QP_OFF);
    h->req_id = ernic_get_le32(wire + IONIC_WIRE_REQ_ID_OFF);
    h->rkey = ernic_get_le32(wire + IONIC_WIRE_RKEY_OFF);
    h->length = ernic_get_le32(wire + IONIC_WIRE_LENGTH_OFF);
    memcpy(&h->imm_be, wire + IONIC_WIRE_IMM_OFF, 4);
    h->status = ernic_get_le32(wire + IONIC_WIRE_STATUS_OFF);
    h->remote_va = ernic_get_le64(wire + IONIC_WIRE_REMOTE_VA_OFF);
    h->swap_add = ernic_get_le64(wire + IONIC_WIRE_SWAP_ADD_OFF);
    h->compare = ernic_get_le64(wire + IONIC_WIRE_COMPARE_OFF);
}

static void wire_hdr_init(struct ionic_wire_hdr *h, uint8_t op,
                          uint32_t src_qp_id, uint32_t dst_qp_id,
                          uint32_t req_id)
{
    memset(h, 0, sizeof(*h));
    h->magic = IONIC_WIRE_MAGIC;
    h->op = op;
    h->src_qp_id = src_qp_id;
    h->dst_qp_id = dst_qp_id;
    h->req_id = req_id;
}

/* What remote_post() did with a work request */
enum dp_post {
    DP_POST_SENT,        /* queued for the peer; its answer completes it */
    DP_POST_BUSY,        /* no room for it, or its reply, yet: try it again */
    DP_POST_SEND_FAILED, /* the mesh refused it: no connection to the peer */
    DP_POST_FAILED,      /* refused here, before the mesh saw it */
};

/*
 * Transmit one WQE to the instance that owns the peer QP and park it until
 * the answer comes back.  Unless it returns DP_POST_SENT, nothing is parked:
 * DP_POST_BUSY leaves the caller to try the same WQE again on a later pass,
 * and the failures to post an immediate error completion.
 *
 * The mesh never waits for the peer: a request it has no room to queue comes
 * back busy, which stops the send queue the way a NIC out of transmit
 * credits does, until the peer has read what is queued.  So does one whose
 * reply the peer might have no room to queue, until earlier replies have
 * come back (ERNIC_MESH_REPLY_BUDGET).  A peer that has stopped reading has
 * its connection shut down by the send that times out on it, and the request
 * then fails.
 */
/*
 * @dest_qp_id and @dest_node_id are passed rather than read from @q because
 * a datagram QP has neither: it names its peer per send, from the WQE and
 * the address handle. Connected callers pass their own q->dest_*.
 *
 * @src_host, when set, is the payload already assembled in host memory --
 * the UD path uses it to send the GRH it built along with the data, so the
 * header is constructed in exactly one place for local and mesh delivery
 * alike. NULL means gather from @src, which names guest memory.
 */
static enum dp_post remote_post(struct ionic_datapath *dp,
                                struct ionic_qp_ring *q, uint32_t qp_id,
                                uint8_t op, uint64_t wqe_id, bool use_msn,
                                bool signalled, const struct dp_sge_list *src,
                                const uint8_t *wqe, uint32_t dest_qp_id,
                                uint32_t dest_node_id, const uint8_t *src_host)
{
    struct ionic_wire_hdr hdr;
    uint8_t *payload = NULL;
    uint32_t payload_len = 0;
    uint32_t reply_bytes;
    uint8_t wire_op;
    enum dp_post result = DP_POST_FAILED;
    int rc;

    struct dp_pending *p = pending_alloc(dp);
    if (!p) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u pending table full", qp_id);
        return DP_POST_FAILED;
    }

    p->qp_id = qp_id;
    p->wqe_id = wqe_id;
    p->use_msn = use_msn;
    p->signalled = signalled;

    switch (op) {
    case IONIC_V1_OP_SEND:
        wire_op = IONIC_WIRE_SEND;
        break;
    case IONIC_V1_OP_SEND_INV:
        wire_op = IONIC_WIRE_SEND_INV;
        break;
    case IONIC_V1_OP_SEND_IMM:
        wire_op = IONIC_WIRE_SEND_IMM;
        break;
    case IONIC_V1_OP_RDMA_WRITE:
        wire_op = IONIC_WIRE_WRITE;
        break;
    case IONIC_V1_OP_RDMA_WRITE_IMM:
        wire_op = IONIC_WIRE_WRITE_IMM;
        break;
    case IONIC_V1_OP_RDMA_READ:
        wire_op = IONIC_WIRE_READ_REQ;
        break;
    case IONIC_V1_OP_ATOMIC_CS:
    case IONIC_V1_OP_ATOMIC_FA:
        wire_op = IONIC_WIRE_ATOMIC_REQ;
        break;
    default:
        p->valid = false;
        return DP_POST_FAILED;
    }

    wire_hdr_init(&hdr, wire_op, qp_id, dest_qp_id, p->req_id);

    if (wire_op == IONIC_WIRE_SEND || wire_op == IONIC_WIRE_SEND_IMM ||
        wire_op == IONIC_WIRE_SEND_INV) {
        uint32_t imm_be;
        memcpy(&imm_be, wqe + 12, 4);
        hdr.imm_be = imm_be;
        hdr.length = src->total;
        payload_len = src->total;
    } else if (wire_op == IONIC_WIRE_WRITE || wire_op == IONIC_WIRE_WRITE_IMM ||
               wire_op == IONIC_WIRE_READ_REQ) {
        uint32_t length = ernic_get_be32(wqe + WQE_SEND_LEN_OFF);
        uint32_t imm_be;
        memcpy(&imm_be, wqe + 12, 4);

        hdr.remote_va = ernic_get_be64(wqe + WQE_RDMA_VA_OFF);
        hdr.rkey = ernic_get_be32(wqe + WQE_RDMA_RKEY_OFF);
        hdr.length = length;
        hdr.imm_be = imm_be;

        if (wire_op == IONIC_WIRE_READ_REQ) {
            /* The response scatters into these; keep them for later. */
            p->local = *src;
            p->length = length;
        } else {
            payload_len = length;
        }
    } else { /* ATOMIC_REQ */
        hdr.atomic_cs = op == IONIC_V1_OP_ATOMIC_CS;
        hdr.rkey = ernic_get_be32(wqe + WQE_RDMA_RKEY_OFF);
        hdr.remote_va = ernic_get_be64(wqe + WQE_RDMA_VA_OFF);
        hdr.swap_add = ernic_get_be64(wqe + WQE_ATOMIC_SWAP_ADD_OFF);
        hdr.compare = ernic_get_be64(wqe + WQE_ATOMIC_COMPARE_OFF);
        hdr.length = 8;

        /* The original value comes back into the WQE's single local SGE. */
        p->local.count = 1;
        p->local.va[0] = ernic_get_be64(wqe + WQE_ATOMIC_SGE_OFF);
        p->local.lkey[0] = ernic_get_be32(wqe + WQE_ATOMIC_SGE_OFF + 12);
        p->local.len[0] = 8;
        p->local.total = 8;
        p->length = 8;
    }

    if (payload_len > IONIC_WIRE_MAX_PAYLOAD) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u remote op %u payload %u too large",
                qp_id, op, payload_len);
        goto out;
    }

    /*
     * The peer drops a reply it has no room to queue, and the request then
     * waits out PENDING_MS, so ask it for no more replies at once than it is
     * sure to have room for.  That is mostly READ data, which the request's
     * own size says nothing about.  The peer refuses a READ longer than any
     * reply can be with a bare header.  p->length is 0 for the requests whose
     * reply is an ACK, and p itself counts nothing yet.
     *
     * The peer queues per connection and this counts per node, which agree
     * unless the peer is a worker with no connection of its own to this node:
     * its replies then go through the manager, on its one connection to it,
     * which every node it reaches that way shares.  A request a flush or
     * timeout has given up on stops counting, though its reply may still be
     * queued.
     *
     * Asked, like the mesh's room, before the gather, which a busy WQE would
     * otherwise repeat on every pass until there is room, for up to 16 MiB
     * each time.
     */
    reply_bytes = IONIC_WIRE_HDR_LEN +
                  (p->length <= IONIC_WIRE_MAX_PAYLOAD ? p->length : 0);
    if (pending_reply_bytes(dp, dest_node_id) >
        ERNIC_MESH_REPLY_BUDGET - reply_bytes) {
        result = DP_POST_BUSY;
        goto out;
    }
    if (!ernic_mesh_has_room(dp->pvrdma_handle, dest_node_id,
                             IONIC_WIRE_HDR_LEN + payload_len)) {
        result = DP_POST_BUSY;
        goto out;
    }

    if (payload_len) {
        payload = malloc(payload_len);
        if (!payload)
            goto out;
        if (src_host) {
            memcpy(payload, src_host, payload_len);
        } else if (ionic_dp_gather(dp, src, payload, payload_len) !=
                   payload_len) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u could not gather %u bytes", qp_id,
                    payload_len);
            goto out;
        }
    }

    p->tag = dp_mesh_tag(q->mesh_id, p->req_id);
    rc =
        dp_mesh_tx(dp, dest_node_id, p->tag, &hdr, payload, payload_len, false);
    if (rc == 0) {
        result = DP_POST_SENT;
        p->dest_node = dest_node_id;
        p->reply_bytes = reply_bytes;
        /* Claim the MSN only once the request is really on the wire, so a
         * failed send does not leave a hole the driver would wait on. */
        if (use_msn)
            p->msn = ++q->msn;

        /* A READ has moved nothing yet; it is counted when the response
         * scatters into p->local. */
        if (wire_op == IONIC_WIRE_WRITE || wire_op == IONIC_WIRE_WRITE_IMM)
            ionic_dp_rdma_bytes(dp, qp_id, payload_len, ERNIC_STAT_RDMA_WRITE,
                                DP_RDMA_TX);
        else if (wire_op != IONIC_WIRE_READ_REQ &&
                 wire_op != IONIC_WIRE_ATOMIC_REQ)
            ionic_dp_rdma_bytes(dp, qp_id, payload_len, ERNIC_STAT_SEND,
                                DP_RDMA_TX);
    } else if (rc == -EAGAIN) {
        /* The room checked above went to a route that changed since */
        result = DP_POST_BUSY;
    } else {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u send to node %u failed", qp_id,
                dest_node_id);
        result = DP_POST_SEND_FAILED;
    }

out:
    free(payload);
    if (result != DP_POST_SENT)
        p->valid = false;
    return result;
}

/* -------------------------------------------------------------------------
 * In-process targets: the NVMe-oF controller and the S3 store
 * -------------------------------------------------------------------------
 */

/*
 * ionic_datapath_nvmeof.c and ionic_datapath_s3.c attach the targets and
 * carry their traffic; this is what the two have in common.
 *
 * The targets reach guest memory the same way a peer's RDMA READ or WRITE
 * would: through the rkey the client put in its request, a keyed SGL for
 * NVMe-oF and the x-amz-rdma-token for S3.  Here that key resolves in the
 * same MR table as any lkey, because one guest owns every registration this
 * emulator holds.
 *
 * Except key 0.  sge_gpa() reads it as IONIC_DMA_LKEY and hands back the
 * address as a bus address with no MR lookup and no bound, which is for the
 * guest kernel's own buffers.  Any guest process that can reach a target
 * chooses the key, so honoring 0 would let an unprivileged one read or write
 * all of guest memory.  Refusing it moves nothing, which both targets report
 * as a failed transfer.  dp_handle_wire() refuses it from mesh peers for the
 * same reason.
 */
uint32_t ionic_dp_target_from_host(void *ctx, uint32_t key, uint64_t addr,
                                   void *dst, uint32_t len)
{
    if (key == 0)
        return 0;
    return ionic_dp_sge_to_host(ctx, key, addr, dst, len);
}

uint32_t ionic_dp_target_to_host(void *ctx, uint32_t key, uint64_t addr,
                                 const void *src, uint32_t len)
{
    if (key == 0)
        return 0;
    return ionic_dp_host_to_sge(ctx, key, addr, src, len);
}

/* -------------------------------------------------------------------------
 * Send processing
 * -------------------------------------------------------------------------
 */

/*
 * The GRH on an ordinary UD receive.
 *
 * The addresses are approximate and deliberately so: this emulator routes
 * datagrams by QPN, and the only GID it reliably has for a local peer is
 * the one in the address handle the sender used. Consumers that read the
 * GRH to identify a peer will see that; consumers that only need the
 * payload at the right offset -- which is the reason this exists -- are
 * served either way.
 */
static void dp_ud_grh(const struct ionic_datapath *dp, uint8_t grh[IB_GRH_SIZE],
                      uint32_t paylen, const struct ionic_ah *ah)
{
    /*
     * The addresses are not decoration. A receiver is entitled to check
     * them, and UCX's ud_verbs does: it compares the GRH's destination GID
     * against its own and silently drops anything that does not match, so a
     * zeroed GRH makes every datagram vanish on arrival with no error on
     * either side. It copes with either form -- it picks them apart the same
     * way dp_grh_is_ipv4() does, on the version nibble at offset 0, and for
     * the IPv4 one compares just the four address bytes at the end.
     *
     * The destination comes from the handle the sender used. So does the
     * source, but from the other address in the same RoCE header template:
     * that is the guest's own GID for the sgid_index the driver resolved.
     *
     * Using the destination for both -- which this did, on the belief that
     * cross-node datagrams never reached here -- is only harmless when
     * sender and receiver are the same device. They are not: the remote
     * branch of the UD send path calls this before handing the datagram to
     * the mesh, and there it told the receiver that its own GID was the
     * sender's. A consumer that replies to the GRH source then answers
     * itself.
     */
    ionic_dp_grh_build(grh, paylen,
                       ah ? (ah->sgid_valid ? ah->sgid : ah->dgid) : NULL,
                       ah ? ah->dgid : NULL);
    (void)dp;
}

/*
 * Why a datagram may not be delivered into @dq, or NULL if it may.
 *
 * Two rules, and the device had neither. The destination QPN in a work
 * request selected any valid QP in the device, so a guest could address
 * another process's RC QP -- which real hardware refuses on transport type
 * alone, a connected QP having no datagram receive path -- or QP1, the GSI
 * queue, and hand the kernel forged connection-management MADs. QP1's
 * Q_Key is privileged precisely so unprivileged consumers cannot.
 *
 * So a work request's Q_Key with the high-order bit set is not taken as a
 * key. The IB spec reserves those "controlled" keys for privileged
 * consumers and has the device send the QP's own Q_Key in their place, and
 * only a privileged consumer can give its QP one: the guest kernel refuses
 * MODIFY_QP with such a key from user space (ionic_check_modify_qp()
 * returns -EPERM). The WQE's key is whatever the sending process wrote, so
 * taken literally it would let any process send as QP1 by writing
 * 0x80010000. The kernel relies on the substitution itself: ib_mad sends
 * every MAD with IB_QP_SET_QKEY (0x80000000), meaning QP1's own key.
 *
 * The Q_Key is only checked once the destination has one. A QP that has not
 * been through MODIFY_QP with IB_QP_QKEY has no key to match, which is the
 * state QP1 is in before the kernel configures it; refusing there would
 * break GSI rather than protect it.
 */
const char *ionic_dp_ud_delivery_refused(const struct ionic_qp_ring *sq,
                                         const struct ionic_qp_ring *dq,
                                         uint32_t sent_qkey)
{
    /*
     * Only a datagram send is subject to these rules, and taking the
     * sender rather than assuming it is the whole reason this is a
     * function. Applied to every SEND it refuses each connected one --
     * an RC destination is not a datagram queue -- which is every RC and
     * UC transfer in the device.
     */
    if (!qp_is_datagram(sq))
        return NULL;
    if (!qp_is_datagram(dq))
        return "transport type accepts no datagrams";
    if (!dq->qkey_valid)
        return NULL;
    if (sent_qkey & DP_QKEY_CONTROLLED) {
        if (!sq->qkey_valid)
            return "sender has no Q_Key of its own to send";
        sent_qkey = sq->qkey;
    }
    if (sent_qkey != dq->qkey)
        return "Q_Key is not that queue's";
    return NULL;
}

/*
 * Send-side completion. RC/UC "remote" work requests are retired by a
 * running MSN sequence; everything else by the SQ index (NPG). Both are
 * needed for EVERY WQE, signalled or not.
 *
 * The signalled-only version of the NPG case was a deadlock. The driver
 * retires its send queue in ionic_poll_send() by walking sq_meta from
 * sq.cons and refusing to step over an entry that has no completion:
 *
 *     if (!meta->remote && !meta->local_comp)
 *             goto out_empty;
 *
 * local_comp is set by ionic_comp_npg() for the one index a CQE names, and
 * whether a user-visible work completion is produced is decided after
 * that, by meta->signal. So the signal flag selects what the driver
 * reports; it does not select what the device must report. Skip an
 * unsignalled WQE here and sq.cons stops at it forever: the queue fills,
 * post_send starts refusing, and the guest goes quiet with nothing dropped
 * and nothing logged.
 *
 * UCX's ud_verbs is the consumer that finds this -- it posts a 256-deep
 * send queue mostly unsignalled, so it ran to about 243 round trips at
 * full speed and then both peers parked waiting for each other. The REG_MR
 * and LOCAL_INV path in process_sq_wqe() completes unconditionally for the
 * same reason.
 */
static void dp_complete_send(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                             uint32_t qp_id, uint64_t wqe_id, bool remote,
                             uint32_t status)
{
    if (remote) {
        q->msn++;
        cq_post_send_msn(dp, q->sq_cq_id, qp_id, q->msn, status);
    } else {
        cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id, status);
    }
}

/*
 * A SEND, SEND_INV or SEND_IMM WQE on @q (@qp_id) that gathers @src, and
 * that process_sq_wqe() has not already handed to the mesh: it goes to a
 * target in this process, as a datagram to another node, or to a QP of
 * this device. @remote is process_sq_wqe()'s. Returns the send's completion
 * status, or sets *@stalled when the WQE is to be retried later instead.
 */
static uint32_t sq_send(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                        uint32_t qp_id, uint8_t op, uint64_t wqe_id,
                        const uint8_t *wqe, const struct dp_sge_list *src,
                        uint32_t imm_be, bool remote, bool *stalled)
{
    *stalled = false;

    uint8_t recv_op = op == IONIC_V1_OP_SEND_IMM   ? CQE_RECV_OP_SEND_IMM
                      : op == IONIC_V1_OP_SEND_INV ? CQE_RECV_OP_SEND_INV
                                                   : CQE_RECV_OP_SEND;

    /*
     * With a controller attached the guest is talking to something that
     * lives in this process, not to another QP: the CM handshake arrives
     * on GSI and every command capsule after it on the QP that handshake
     * bound.  Both are answered here and never reach the peer lookup.
     */
    if (q->ib_qp_type == ERNIC_IB_QPT_GSI &&
        ionic_dp_nvmeof_gsi(dp, q, qp_id, src))
        return IONIC_STS_OK;
    if (remote && ionic_dp_nvmeof_rc_send(dp, q, qp_id, src))
        return IONIC_STS_OK;

    /*
     * A datagram names its own destination. Connected QPs learned a
     * peer at RTR and q->dest_qp_id is right for them, but a UD or GSI
     * QP never sets dest_valid, so the old fallback to qp_id delivered
     * every datagram straight back to the sender -- which is why UCX's
     * ud_verbs transport, and so the rc_verbs wireup that depends on
     * it, never completed a handshake.
     *
     * ah_id is resolved for its node rather than its address: the
     * emulator routes by QP number, and the GID in the AH only decides
     * which instance the peer lives in.
     */
    uint32_t dst_id;
    const struct ionic_ah *ah = NULL;
    if (qp_is_datagram(q)) {
        dst_id = ernic_get_be32(wqe + WQE_SEND_DEST_QPN_OFF);

        uint32_t ah_id = ernic_get_be32(wqe + WQE_SEND_AH_OFF);
        ah =
            ah_id < dp->ah_count && dp->ah[ah_id].valid ? &dp->ah[ah_id] : NULL;
        if (!ah && ah_id) {
            /* Not fatal -- a local peer needs no address -- but it
             * means CREATE_AH never reached us for this handle. */
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u SEND names unknown AH %u", qp_id,
                    ah_id);
        }
        if (ah && dp->local_node != UINT32_MAX && ah->node_id != UINT32_MAX &&
            ah->node_id != dp->local_node) {
            /*
             * The datagram is for another instance. The GRH goes on
             * here, before it leaves, so the receiving side can deliver
             * what arrives unchanged -- the sender is the only one that
             * knows which handle was used.
             */
            uint8_t gbuf[IB_GRH_SIZE + 8192];
            uint32_t plen = src->total;
            if (plen > sizeof(gbuf) - IB_GRH_SIZE)
                plen = (uint32_t)(sizeof(gbuf) - IB_GRH_SIZE);
            if (ionic_dp_gather(dp, src, gbuf + IB_GRH_SIZE, plen) != plen) {
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u UD payload fetch failed", qp_id);
                return IONIC_STS_OK;
            }
            dp_ud_grh(dp, gbuf, plen, ah);

            struct dp_sge_list gsrc = {.count = 0, .total = IB_GRH_SIZE + plen};
            /*
             * use_msn AND signalled both false, which is not the same
             * as "unsignalled". The completion for this WQE is posted
             * by the common path below, as it is for every local op;
             * letting the pending entry post one too would complete the
             * same WQE twice and advance the driver's sq.cons twice for
             * one send. A datagram has no sequence to keep and nothing
             * to report from the peer's ack, so the ack is bookkeeping
             * here and nothing more.
             */
            enum dp_post posted =
                remote_post(dp, q, qp_id, op, wqe_id, false, false, &gsrc, wqe,
                            dst_id, ah->node_id, gbuf);
            if (posted == DP_POST_BUSY) {
                sq_busy_set(dp, q);
                *stalled = true;
                return IONIC_STS_OK;
            }
            if (posted != DP_POST_SENT) {
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u UD send to node %u failed",
                        qp_id, ah->node_id);
            }
            return IONIC_STS_OK;
        }
    } else {
        dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
    }

    struct ionic_qp_ring *dq =
        dst_id < dp->qp_count && dp->qp[dst_id].valid ? &dp->qp[dst_id] : NULL;
    if (!dq) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u SEND to unknown peer %u", qp_id, dst_id);
        return IONIC_STS_OK;
    }
    /*
     * Only a datagram send is checked. This sits after the branch above
     * because that is where the destination is resolved, but the rules
     * are a datagram's: a connected send's destination is its one
     * configured peer, it carries no Q_Key, and holding it to a
     * datagram's rules refuses every RC and UC SEND in the device.
     */
    {
        const char *refused = ionic_dp_ud_delivery_refused(
            q, dq, ernic_get_be32(wqe + WQE_SEND_QKEY_OFF));
        if (refused) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u datagram to QP %u refused: %s",
                    qp_id, dst_id, refused);
            return IONIC_STS_OK;
        }
    }
    /*
     * A UD receive begins with a 40-byte GRH: the payload starts at
     * offset 40 and the completion's byte_len counts it. Consumers rely
     * on that offset absolutely -- UCX parses its own header at a fixed
     * position past the GRH -- so delivering the payload at offset 0
     * does not truncate anything, it silently shifts every field a
     * consumer reads.
     *
     * ibv_ud_pingpong cannot catch this on loopback: it sends from
     * buf + 40 into the same buffer it receives at buf, so the bytes it
     * verifies are already in place whether or not a GRH was added.
     *
     * Built here rather than in ionic_dp_deliver_recv() because the GSI
     * path above already supplies its own GRH as payload, and a second one
     * would offset it twice.
     */
    uint8_t udbuf[IB_GRH_SIZE + 8192];
    struct dp_sge_list udsrc;
    const uint8_t *src_host = NULL;
    if (qp_is_datagram(dq)) {
        uint32_t plen = src->total;
        if (plen > sizeof(udbuf) - IB_GRH_SIZE)
            plen = (uint32_t)(sizeof(udbuf) - IB_GRH_SIZE);
        if (ionic_dp_gather(dp, src, udbuf + IB_GRH_SIZE, plen) != plen) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u UD payload fetch failed", qp_id);
            return IONIC_STS_OK;
        }
        dp_ud_grh(dp, udbuf, plen, ah);
        src_host = udbuf;
        udsrc.count = 0;
        udsrc.total = IB_GRH_SIZE + plen;
    }

    if (ionic_dp_deliver_recv(dp, dq, dst_id, qp_id, src_host ? &udsrc : src,
                              src_host, recv_op, imm_be) < 0) {
        /*
         * RNR.  A reliable requester retries rather than lose the
         * message, so an RC send queue waits here for the peer to post
         * a receive, for as long as the wire path waits.  UC, UD and
         * GSI have no retry: the responder drops the message and the
         * send completes.
         */
        uint64_t now = ionic_dp_now_ms(dp);

        if (q->ib_qp_type != IBV_QPT_RC) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u has no posted receive, "
                    "dropping %u bytes from QP %u",
                    dst_id, src->total, qp_id);
            dp_count(dp, dst_id, IONIC_DP_RESP_RX_OUT_OF_BUFFER);
            return IONIC_STS_OK;
        }
        if (!q->sq_rnr) {
            if (!q->sq_busy)
                dp->sq_stalled++;
            q->sq_rnr = true;
            q->sq_rnr_since_ms = now;
            dp_count(dp, dst_id, IONIC_DP_RESP_RX_OUT_OF_BUFFER);
        }
        if (now - q->sq_rnr_since_ms < RNR_RETRY_MS) {
            dp_count(dp, dst_id, IONIC_DP_RESP_TX_RNR_RETRY_ERR);
            *stalled = true;
            return IONIC_STS_OK;
        }
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u posted no receive within %u ms, "
                "dropping %u bytes from QP %u",
                dst_id, RNR_RETRY_MS, src->total, qp_id);
        return IONIC_STS_RNR_RETRY_EXCEEDED;
    }
    ionic_dp_rdma_bytes(dp, qp_id, src->total, ERNIC_STAT_SEND, DP_RDMA_TX);
    return IONIC_STS_OK;
}

/*
 * An RDMA READ, WRITE or WRITE_IMM WQE on @q (@qp_id), between @src and a
 * region of this device. Returns its completion status.
 */
static uint32_t sq_rdma(struct ionic_datapath *dp,
                        const struct ionic_qp_ring *q, uint32_t qp_id,
                        uint8_t op, const uint8_t *wqe,
                        const struct dp_sge_list *src, uint32_t imm_be)
{
    uint64_t remote_va = ernic_get_be64(wqe + WQE_RDMA_VA_OFF);
    uint32_t rkey = ernic_get_be32(wqe + WQE_RDMA_RKEY_OFF);
    uint32_t length = ernic_get_be32(wqe + WQE_SEND_LEN_OFF);

    bool to_remote = op != IONIC_V1_OP_RDMA_READ;

    /*
     * Check the far end's range before moving anything, not after.
     * rdma_xfer() walks until it runs out of region, so an
     * over-long length used to transfer everything that fitted and
     * only then report the overrun: a write left its first bytes in
     * the tail of the peer's region, and a read had already copied
     * them out. Both stay inside the registered region, so neither
     * reaches memory the key does not cover, but an operation the
     * device rejects should not have moved anything.
     *
     * Zero length is exempt, and has to be: it accesses nothing, the
     * spec does not require the R_Key to be validated for one, and
     * UCX's rc_verbs flush is a zero-length RDMA_WRITE with rkey 0.
     * The mesh responder path exempts it for the same reason.
     */
    if (length != 0 && !ionic_dp_mr_covers(dp, rkey, remote_va, length)) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u RDMA %s rkey=%#x va=%#" PRIx64
                " len=%u is outside the region",
                qp_id, to_remote ? "write" : "read", rkey, remote_va, length);
        return IONIC_STS_REMOTE_ACC_ERR;
    }

    uint32_t moved = rdma_xfer(dp, src, rkey, remote_va, length, to_remote);
    if (moved != length) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u RDMA %s rkey=%#x va=%#" PRIx64
                " moved %u of %u bytes",
                qp_id, to_remote ? "write" : "read", rkey, remote_va, moved,
                length);
        /*
         * Blame the end that was actually out of bounds.  Both ends
         * resolve through the same MR table, so without this an SGE of
         * the guest's own that overruns its MR is reported back to the
         * guest as the peer denying access.
         */
        return ionic_dp_mr_covers(dp, rkey, remote_va, length)
                   ? dp_fault_status(dp, IONIC_STS_LOCAL_PROT_ERR)
                   : IONIC_STS_REMOTE_ACC_ERR;
    }

    ionic_dp_rdma_bytes(dp, qp_id, moved,
                        to_remote ? ERNIC_STAT_RDMA_WRITE
                                  : ERNIC_STAT_RDMA_READ,
                        DP_RDMA_TX | DP_RDMA_RX);

    /* Only the _IMM form consumes a receive on the far side, and it does
     * so with no payload: the data already landed via the rkey.  The
     * length still rides along, because the responder's completion
     * reports how many bytes the write moved. */
    if (op == IONIC_V1_OP_RDMA_WRITE_IMM) {
        uint32_t dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
        struct ionic_qp_ring *dq = dst_id < dp->qp_count && dp->qp[dst_id].valid
                                       ? &dp->qp[dst_id]
                                       : NULL;
        struct dp_sge_list written = {.count = 0, .total = moved};
        if (dq)
            ionic_dp_deliver_recv(dp, dq, dst_id, qp_id, &written, NULL,
                                  CQE_RECV_OP_RDMA_IMM, imm_be);
    }
    return IONIC_STS_OK;
}

/*
 * Runs the WQE in @slot of @q's send queue. Returns false when sq_service()
 * is to stop at it: it is to be retried later, or the QP has been flushed.
 */
static bool process_sq_wqe(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                           uint32_t qp_id, uint32_t slot)
{
    uint32_t stride = 1u << q->sq_stride_log2;
    uint64_t run;
    uint64_t gpa = buf_gpa(&q->sq_buf, (uint64_t)slot * stride, &run);

    /* Big enough for the largest stride an SQ can have, so the whole WQE is
     * read. */
    uint8_t wqe[1u << IONIC_WQE_MAX_STRIDE_LOG2];
    uint32_t read_sz = stride < sizeof(wqe) ? stride : (uint32_t)sizeof(wqe);
    if (!run || run < read_sz ||
        ernic_dma_read(dp->dma, dp->vfu_ctx, gpa, wqe, read_sz) < 0) {
        /*
         * A WQE the device can't fetch is completed in error, and the QP
         * goes to Error, as a NIC's does on a fetch fault. Taken as done
         * without either, the work request vanished: nothing completed it,
         * and the driver never got its slot back. Retrying it, as the admin
         * queue does, would wedge the queue on memory the guest may have
         * unmapped for good.
         *
         * Its wqe_id is in the WQE, but both drivers set it to the SQ index
         * the WQE was posted at, and mask it with sq.mask, so the slot names
         * the same work request. It is an NPG completion whatever the op
         * was: an MSN one would complete whichever remote op holds the next
         * MSN, and the driver's ionic_comp_npg() takes an error for a WQE
         * of either kind. LOCAL_QP_OPER_ERR, IB_WC_LOC_QP_OP_ERR to the
         * consumer, is IB's error for a work request the QP itself couldn't
         * process; LOCAL_PROT_ERR is for a buffer an MR doesn't cover, and
         * the ring isn't one.
         *
         * The error goes first: had the driver seen the flush before it, it
         * could have flushed this work request and lost the cause. For the
         * same reason both wait for the requests posted before this one to
         * complete (sq_fault_finish()), and the queue stops here until then.
         * The flush empties it, so sq_service() has nothing left to run.
         */
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u SQ WQE %u at %#" PRIx64
                " can't be read; flushing the QP",
                qp_id, slot, gpa);
        sq_stall_clear(dp, q);
        q->sq_fault = true;
        sq_fault_finish(dp, q, qp_id);
        return false;
    }

    uint64_t wqe_id;
    memcpy(&wqe_id, wqe + 0, 8);

    uint8_t op = wqe[8];
    if (!q->sq_rnr && !q->sq_busy) /* a retry is the same WQE again */
        ernic_stats_qp_wqe(dp->stats, qp_id, ionic_op_to_stat_wr(op));

    uint16_t flags = ernic_get_be16(wqe + 10);

    uint32_t imm_be;
    memcpy(&imm_be, wqe + 12, 4);

    /*
     * REG_MR and LOCAL_INV are local operations: they never reach the wire, so
     * they are handled ahead of the remote-QP dispatch, and they complete by
     * SQ index whether or not the WQE asked to be signalled -- the driver's
     * poll_send() will not advance sq.cons past a local op until its NPG
     * completion lands.  num_sge_key is the key, not an SGE count, so this
     * must also come before parse_sges().
     */
    if (op == IONIC_V1_OP_REG_MR || op == IONIC_V1_OP_LOCAL_INV) {
        /*
         * A REG_MR that binds nothing completes in error, with the status
         * the driver turns into IB_WC_MW_BIND_ERR, the memory management
         * operation error IB gives a fast registration. Completed as
         * success, it told the guest that a key no transfer could use was
         * registered.
         */
        uint32_t reg_status = IONIC_STS_OK;

        if (op != IONIC_V1_OP_REG_MR) {
            ionic_datapath_unregister_mr(dp, ernic_get_be32(wqe + 12));
        } else if (read_sz < WQE_REG_MR_MIN_SZ) {
            /* Like any other REG_MR that fails, it leaves the MR with no
             * binding, under this key or an earlier rotation of it. */
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u REG_MR WQE truncated (%u bytes)",
                    qp_id, read_sz);
            dp_mr_drop_id(dp, ernic_get_be32(wqe + 12), false);
            reg_status = IONIC_STS_MEM_MGMT_OPER_ERR;
        } else if (dp_reg_mr(dp, wqe, ernic_get_be32(wqe + 12)) < 0) {
            reg_status = IONIC_STS_MEM_MGMT_OPER_ERR;
        }
        cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id, reg_status);
        return true;
    }

    bool remote = !qp_is_datagram(q);
    uint32_t status = IONIC_STS_OK;

    struct dp_sge_list src;
    if (op == IONIC_V1_OP_ATOMIC_CS || op == IONIC_V1_OP_ATOMIC_FA) {
        /*
         * An atomic has no SGE list. Its operands lie where one would
         * start, and parse_sges() read them as one: operands of all ones
         * made an SGE running past 2^64, and a compare-and-swap that
         * should run failed as a protection error. Its one local buffer is
         * the 8-byte result SGE at WQE_ATOMIC_SGE_OFF, which is what
         * do_atomic() and remote_post() use, so that is what is checked.
         *
         * A WQE too short to hold that SGE ran on stack bytes the guest
         * never wrote.
         */
        if (read_sz < WQE_ATOMIC_MIN_SZ) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u atomic WQE truncated (%u bytes)",
                    qp_id, read_sz);
            dp_complete_send(dp, q, qp_id, wqe_id, remote,
                             IONIC_STS_LOCAL_QP_OPER_ERR);
            return true; /* completed, in error: the WQE is done with */
        }
        src.count = 1;
        src.va[0] = ernic_get_be64(wqe + WQE_ATOMIC_SGE_OFF);
        src.len[0] = 8;
        src.lkey[0] = ernic_get_be32(wqe + WQE_ATOMIC_SGE_OFF + 12);
        src.total = 8;
    } else if (flags & IONIC_V1_FLAG_INL) {
        /* Inline data lives in the payload area itself; describe it as a
         * single lkey-0 SGE pointing at the WQE's own guest pages. */
        uint32_t inl_len = ernic_get_be32(wqe + WQE_SEND_LEN_OFF);

        /*
         * No longer than the payload area: that is all of the WQE that was
         * read, and all that @run says is contiguous. The length is the
         * guest's, and lkey 0 has no region to bound it, so a longer one
         * read on into whatever guest memory follows the slot, which for a
         * user QP needn't be the poster's.
         */
        if (inl_len > read_sz - WQE_PLD_OFF) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u op %u inline length %u is more "
                    "than the %u bytes its WQE holds",
                    qp_id, op, inl_len, read_sz - WQE_PLD_OFF);
            dp_complete_send(dp, q, qp_id, wqe_id, remote,
                             IONIC_STS_LOCAL_LEN_ERR);
            return true; /* completed, in error: the WQE is done with */
        }
        src.count = 1;
        src.va[0] = gpa + WQE_PLD_OFF;
        src.len[0] = inl_len;
        src.lkey[0] = 0;
        src.total = inl_len;
    } else if (!parse_sges(wqe, read_sz, wqe[9], &src)) {
        /* read_sz, not stride, for the reason given in
         * ionic_dp_deliver_recv(). */
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u op %u gathers more than %" PRIu32
                " bytes",
                qp_id, op, UINT32_MAX);
        dp_complete_send(dp, q, qp_id, wqe_id, remote, IONIC_STS_LOCAL_LEN_ERR);
        return true; /* completed, in error: the WQE is done with */
    }

    /*
     * Every SGE above names this guest's own memory, so a key it never
     * registered, or a length running past the region it names, is a local
     * protection error and belongs on this QP's send completion.
     *
     * Checked here, before the work request is dispatched either way,
     * because each operation's own failure path reports on whichever side
     * noticed. A local SEND whose gather failed completed successfully for
     * the sender and put the error on the peer's receive completion, which
     * told the sender nothing and blamed a QP that did nothing wrong; a
     * remote one sent the peer a request it could not satisfy. Nothing is
     * transferred in either case -- that part was always true -- but the
     * guest is now told whose fault it was, as IB requires.
     */
    if (!dp_local_sges_ok(dp, &src)) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u op %u names memory it has not "
                "registered",
                qp_id, op);
        dp_complete_send(dp, q, qp_id, wqe_id, remote,
                         IONIC_STS_LOCAL_PROT_ERR);
        return true; /* completed, in error: the WQE is done with */
    }

#ifdef ERNIC_HAVE_P4
    /*
     * The pipeline runs here: after the operation's own memory has been
     * checked and before it is dispatched anywhere.  A program therefore
     * never sees a request that is already a protection error, and an
     * extern it calls cannot be the first thing to touch a region the
     * guest did not register.
     */
    if (dp->p4_engine) {
        bool done = false;
        uint32_t p4_status = ionic_dp_p4_policy(dp, qp_id, op, wqe, read_sz,
                                                &src, imm_be, &done);
        if (done) {
            dp_complete_send(dp, q, qp_id, wqe_id, remote, p4_status);
            return true; /* completed by the pipeline */
        }
    }
#endif

    /*
     * The peer QP lives in another instance: hand the request to the mesh and
     * leave the completion parked until the peer answers.
     */
    if (dp_is_remote(dp, q)) {
        switch (remote_post(dp, q, qp_id, op, wqe_id, remote,
                            (flags & IONIC_V1_FLAG_SIG) != 0, &src, wqe,
                            q->dest_qp_id, q->dest_node_id, NULL)) {
        case DP_POST_SENT:
            return true;
        case DP_POST_BUSY:
            sq_busy_set(dp, q);
            return false;
        case DP_POST_SEND_FAILED:
            /*
             * The peer can't be reached, which is what a request that is
             * never answered reports too (see ionic_datapath_poll()).
             */
            status = IONIC_STS_RETRY_EXCEEDED;
            break;
        case DP_POST_FAILED:
        default:
            status = IONIC_STS_REMOTE_ACC_ERR;
            break;
        }

        if (remote) {
            q->msn++;
            cq_post_send_msn(dp, q->sq_cq_id, qp_id, q->msn, status);
        } else if (flags & IONIC_V1_FLAG_SIG) {
            cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id, status);
        }
        return true;
    }

    dp_fault_clear(dp);

    switch (op) {
    case IONIC_V1_OP_SEND:
    case IONIC_V1_OP_SEND_INV:
    case IONIC_V1_OP_SEND_IMM: {
        bool stalled;

        status = sq_send(dp, q, qp_id, op, wqe_id, wqe, &src, imm_be, remote,
                         &stalled);
        if (stalled)
            return false;
        break;
    }

    case IONIC_V1_OP_RDMA_READ:
    case IONIC_V1_OP_RDMA_WRITE:
    case IONIC_V1_OP_RDMA_WRITE_IMM:
        status = sq_rdma(dp, q, qp_id, op, wqe, &src, imm_be);
        break;

    case IONIC_V1_OP_ATOMIC_CS:
    case IONIC_V1_OP_ATOMIC_FA:
        if (!do_atomic(dp, wqe, op == IONIC_V1_OP_ATOMIC_CS)) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u atomic op=%u failed", qp_id, op);
            status = dp_fault_status(dp, IONIC_STS_REMOTE_ACC_ERR);
        }
        break;

    default:
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u unsupported op=%u", qp_id, op);
        status = IONIC_STS_LOCAL_QP_OPER_ERR;
        break;
    }

    /*
     * The responder is this same device, so a request it refused counts on
     * both ends: the requester's error through its completion, and the
     * responder's NAK here, against the QP it would have landed on.
     */
    if (status == IONIC_STS_REMOTE_ACC_ERR)
        dp_count(dp, q->dest_valid ? q->dest_qp_id : qp_id,
                 IONIC_DP_RESP_TX_REMOTE_ACCESS_ERR);

    dp_complete_send(dp, q, qp_id, wqe_id, remote, status);
    return true;
}

/*
 * Runs @q's send queue from sq_cons up to the last doorbell, stopping at a
 * SEND that is waiting for a receive, at a WQE for another node that the
 * mesh has no room to queue yet, or at a WQE it can't read, which flushes
 * the QP once the requests before it have completed.
 */
static void sq_service(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                       uint32_t qp_id)
{
    if (q->sq_fault)
        return;
    for (uint32_t n = 0;
         q->sq_cons % q->sq_depth != q->sq_prod && n < q->sq_depth; n++) {
        if (!process_sq_wqe(dp, q, qp_id, q->sq_cons % q->sq_depth))
            return;
        sq_stall_clear(dp, q);
        q->sq_cons++;
    }
}

/* -------------------------------------------------------------------------
 * Cross-instance receive
 *
 * Messages arrive on a backend receive thread, which must not touch guest
 * memory: every DMA belongs to the thread that owns the vfio-user context.
 * So dp_mesh_recv() only puts the message on a queue, and the server's main
 * loop drains it here through ionic_datapath_poll().
 * -------------------------------------------------------------------------
 */

/*
 * Whether a queue holding @msgs messages of @bytes bytes in all has room for
 * one more of @len bytes.  An empty one takes any message.
 */
static bool dp_rx_room(uint32_t msgs, size_t bytes, size_t len)
{
    return msgs == 0 || (msgs < DP_RX_MAX_MSGS && len <= DP_RX_MAX_BYTES &&
                         bytes <= DP_RX_MAX_BYTES - len);
}

/*
 * Takes buf (see ernic_mesh_recv_fn), freeing it at once if the message is
 * malformed, or queueing it, to be freed once the message is applied; or
 * returns false, leaving it with the mesh, if the queue has no room for it.
 *
 * The main thread empties the queue on every pass but for the SENDs waiting
 * for a receive, and gives up on those once they have kept a message out
 * for DP_RX_CROWDED_MS, so a message refused here is taken soon.  If the
 * main thread has stopped polling, nothing would take it, and it is dropped
 * instead.  That is DP_RX_STUCK_MS since a pass ended, with none under way:
 * a long pass is the main thread applying what is queued, and a message
 * dropped for it would cost its sender the 30-second request timeout.
 */
static bool dp_mesh_recv(void *opaque, uint32_t src_node, void *buf, size_t len)
{
    struct ionic_datapath *dp = opaque;
    bool room, stuck = false;

    if (len < IONIC_WIRE_HDR_LEN ||
        len > IONIC_WIRE_HDR_LEN + IONIC_WIRE_MAX_PAYLOAD) {
        free(buf);
        return true;
    }

    struct dp_inmsg *m = malloc(sizeof(*m));
    if (!m) {
        free(buf);
        return true;
    }

    m->next = NULL;
    m->src_node = src_node;
    m->len = len;
    m->first_try_ms = 0;
    m->rnr_counted = false;
    m->buf = buf;

    qemu_mutex_lock(&dp->rx_lock);
    room = dp_rx_room(dp->rx_msgs, dp->rx_bytes, len);
    if (room) {
        if (dp->rx_tail)
            dp->rx_tail->next = m;
        else
            dp->rx_head = m;
        dp->rx_tail = m;
        dp->rx_fresh++;
        dp->rx_msgs++;
        dp->rx_bytes += len;
    } else {
        /* A pass reads the clock before setting rx_polled_ms, so that is
         * later than now only if a unit test's clock went back */
        uint64_t now = ionic_dp_now_ms(dp);

        stuck = !dp->rx_in_pass && now > dp->rx_polled_ms &&
                now - dp->rx_polled_ms > DP_RX_STUCK_MS;
        if (stuck)
            dp->rx_dropped++;
        else if (len > dp->rx_waiting)
            dp->rx_waiting = len;
    }
    qemu_mutex_unlock(&dp->rx_lock);

    if (room)
        return true;
    if (stuck)
        free(buf);
    free(m);
    return stuck;
}

static void dp_wire_reply(struct ionic_datapath *dp, uint32_t dst_node,
                          uint8_t op, const struct ionic_wire_hdr *req,
                          uint32_t status, const uint8_t *payload,
                          uint32_t payload_len)
{
    struct ionic_wire_hdr r;

    wire_hdr_init(&r, op, req->dst_qp_id, req->src_qp_id, req->req_id);
    r.status = status;
    r.length = payload_len;
    /*
     * A reply the mesh can't queue is lost, and the requester's own timeout
     * for it completes the request; the mesh has logged why.
     */
    dp_mesh_tx(dp, dst_node, 0, &r, payload, payload_len, true);

    if (status == IONIC_STS_REMOTE_ACC_ERR)
        dp_count(dp, req->dst_qp_id, IONIC_DP_RESP_TX_REMOTE_ACCESS_ERR);
}

/* Apply a peer's atomic to a local MR, returning the pre-operation value. */
static bool dp_remote_atomic(struct ionic_datapath *dp, uint32_t rkey,
                             uint64_t va, bool compare_swap, uint64_t swap_add,
                             uint64_t compare, uint64_t *old)
{
    uint64_t run;
    uint64_t gpa = sge_gpa(dp, rkey, va, &run);

    return run >= 8 &&
           dp_atomic_apply(dp, gpa, compare_swap, swap_add, compare, old);
}

/*
 * Apply one inbound message.  Returns false only when the message is an
 * inbound SEND that found no posted receive and @allow_retry says to leave it
 * queued; the caller then hands it back on a later poll.  Every other outcome,
 * including a permanent failure, consumes the message.
 */
static bool dp_handle_wire(struct ionic_datapath *dp, uint32_t src_node,
                           const uint8_t *msg, size_t len, bool allow_retry)
{
    struct ionic_wire_hdr h;
    const uint8_t *payload = msg + IONIC_WIRE_HDR_LEN;
    uint32_t payload_len = (uint32_t)(len - IONIC_WIRE_HDR_LEN);

    ionic_dp_wire_hdr_get(&h, msg);
    if (h.magic != IONIC_WIRE_MAGIC) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: dropping mesh message with bad magic");
        return true;
    }

    uint32_t dst_qp_id = h.dst_qp_id;
    uint32_t src_qp_id = h.src_qp_id;
    uint32_t length = h.length;
    uint32_t rkey = h.rkey;
    uint64_t remote_va = h.remote_va;
    uint32_t status = IONIC_STS_OK;

    struct ionic_qp_ring *dq =
        dst_qp_id < dp->qp_count && dp->qp[dst_qp_id].valid ? &dp->qp[dst_qp_id]
                                                            : NULL;

    /* A plain RDMA op names only an rkey, so the QP it quotes may not exist
     * here; attribute those bytes to the device alone. */
    uint32_t stat_qp = dq ? dst_qp_id : ERNIC_STAT_NO_QP;

    /*
     * A peer may only name memory this instance has registered.  sge_gpa()
     * reads key 0 as IONIC_DMA_LKEY and hands back the va as a bus address with
     * no MR lookup and no bound, which is fine for a key our own guest put in a
     * WQE but would let another instance reach any guest physical page.
     *
     * Zero-length operations are exempt, and have to be. A zero-length RDMA
     * read or write performs no memory access at all, and the spec says the
     * R_Key is not validated for one -- so a sender is entitled to leave it
     * zero, and UCX does: its rc_verbs flush is a zero-length RDMA_WRITE with
     * rkey 0, va 0 and no payload. Rejecting that is not a tightening, it is
     * a wrong answer to a legal request, and it cost a two-VM rc_verbs hang
     * that looked like a transport failure. There is nothing to protect here
     * -- ionic_dp_host_to_sge() with len 0 touches no memory and returns 0
     * before it ever calls sge_gpa().
     *
     * Atomics are not exempt: remote_post() always gives them length 8, so
     * they still require a key.
     */
    if (!rkey && length &&
        (h.op == IONIC_WIRE_WRITE || h.op == IONIC_WIRE_WRITE_IMM ||
         h.op == IONIC_WIRE_READ_REQ || h.op == IONIC_WIRE_ATOMIC_REQ)) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: node %u sent op %u with rkey 0, rejecting",
                src_node, h.op);
        dp_wire_reply(dp, src_node,
                      h.op == IONIC_WIRE_READ_REQ     ? IONIC_WIRE_READ_RESP
                      : h.op == IONIC_WIRE_ATOMIC_REQ ? IONIC_WIRE_ATOMIC_RESP
                                                      : IONIC_WIRE_ACK,
                      &h, IONIC_STS_REMOTE_ACC_ERR, NULL, 0);
        return true;
    }

    switch (h.op) {
    case IONIC_WIRE_SEND:
    case IONIC_WIRE_SEND_IMM:
    case IONIC_WIRE_SEND_INV: {
        uint8_t recv_op = h.op == IONIC_WIRE_SEND_IMM   ? CQE_RECV_OP_SEND_IMM
                          : h.op == IONIC_WIRE_SEND_INV ? CQE_RECV_OP_SEND_INV
                                                        : CQE_RECV_OP_SEND;
        struct dp_sge_list src = {.count = 0, .total = length};

        if (length > payload_len)
            src.total = payload_len;

        if (!dq) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: SEND from node %u to unknown QP %u",
                    src_node, dst_qp_id);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else if (ionic_dp_deliver_recv(dp, dq, dst_qp_id, src_qp_id, &src,
                                         payload, recv_op, h.imm_be) < 0) {
            /* RNR: nothing has been consumed, so the message can simply wait
             * for the guest to post a receive. */
            if (allow_retry)
                return false;

            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u posted no receive in time, "
                    "dropping %u bytes from node %u",
                    dst_qp_id, src.total, src_node);
            status = IONIC_STS_RNR_RETRY_EXCEEDED;
        }
        dp_wire_reply(dp, src_node, IONIC_WIRE_ACK, &h, status, NULL, 0);
        break;
    }

    case IONIC_WIRE_WRITE:
    case IONIC_WIRE_WRITE_IMM: {
        uint32_t n = length > payload_len ? payload_len : length;

        if (ionic_dp_host_to_sge(dp, rkey, remote_va, payload, n) != length) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote write rkey=%#x va=%#" PRIx64
                    " of %u bytes failed",
                    rkey, remote_va, length);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else {
            /* The responder counts what landed in its own memory too, so each
             * instance's totals describe the traffic it actually moved. */
            ionic_dp_rdma_bytes(dp, stat_qp, n, ERNIC_STAT_RDMA_WRITE,
                                DP_RDMA_RX);

            if (h.op == IONIC_WIRE_WRITE_IMM && dq) {
                struct dp_sge_list written = {.count = 0, .total = n};
                ionic_dp_deliver_recv(dp, dq, dst_qp_id, src_qp_id, &written,
                                      NULL, CQE_RECV_OP_RDMA_IMM, h.imm_be);
            }
        }
        dp_wire_reply(dp, src_node, IONIC_WIRE_ACK, &h, status, NULL, 0);
        break;
    }

    case IONIC_WIRE_READ_REQ: {
        uint8_t *data = NULL;

        if (length > IONIC_WIRE_MAX_PAYLOAD)
            status = IONIC_STS_REMOTE_ACC_ERR;
        else if (length && !(data = malloc(length)))
            status = IONIC_STS_REMOTE_ACC_ERR;
        else if (ionic_dp_sge_to_host(dp, rkey, remote_va, data, length) !=
                 length) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote read rkey=%#x va=%#" PRIx64
                    " of %u bytes failed",
                    rkey, remote_va, length);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else {
            ionic_dp_rdma_bytes(dp, stat_qp, length, ERNIC_STAT_RDMA_READ,
                                DP_RDMA_TX);
        }

        dp_wire_reply(dp, src_node, IONIC_WIRE_READ_RESP, &h, status,
                      status == IONIC_STS_OK ? data : NULL,
                      status == IONIC_STS_OK ? length : 0);
        free(data);
        break;
    }

    case IONIC_WIRE_ATOMIC_REQ: {
        uint64_t old = 0;
        uint8_t old_le[8];

        if (!dp_remote_atomic(dp, rkey, remote_va, h.atomic_cs, h.swap_add,
                              h.compare, &old)) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote atomic rkey=%#x va=%#" PRIx64
                    " failed",
                    rkey, remote_va);
            status = IONIC_STS_REMOTE_ACC_ERR;
        }
        ernic_put_le64(old_le, old);
        dp_wire_reply(dp, src_node, IONIC_WIRE_ATOMIC_RESP, &h, status, old_le,
                      status == IONIC_STS_OK ? sizeof(old_le) : 0);
        break;
    }

    case IONIC_WIRE_ACK:
    case IONIC_WIRE_READ_RESP:
    case IONIC_WIRE_ATOMIC_RESP: {
        struct dp_pending *p = pending_find(dp, h.req_id);
        if (!p)
            break;

        status = h.status;
        if (status == IONIC_STS_OK && h.op != IONIC_WIRE_ACK) {
            uint32_t want = p->length < payload_len ? p->length : payload_len;

            /* The peer answered; anything that goes wrong from here is this
             * guest's own memory refusing the landing.  An ATOMIC_RESP's
             * le64 is a u64 as this guest's memory holds one, so it lands
             * as-is, like READ data (see dp_atomic_apply()). */
            dp_fault_clear(dp);
            uint32_t got = dp_scatter(dp, &p->local, payload, want);
            if (got != want)
                status = dp_fault_status(dp, IONIC_STS_LOCAL_PROT_ERR);

            /* The requester's READ bytes only exist once they have landed. */
            if (h.op == IONIC_WIRE_READ_RESP)
                ionic_dp_rdma_bytes(dp, p->qp_id, got, ERNIC_STAT_RDMA_READ,
                                    DP_RDMA_RX);
        }
        pending_complete(dp, p, status);
        break;
    }

    default:
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: unknown mesh op %u from node %u", h.op,
                src_node);
        break;
    }

    return true;
}

/* Destination QP of a queued message, used to keep per-QP ordering. */
static uint32_t inmsg_dst_qp(const struct dp_inmsg *m)
{
    return ernic_get_le32(m->buf + IONIC_WIRE_DST_QP_OFF);
}

/*
 * dp_wire_reply() addresses a response to the QP that originated the request,
 * so on this side its dst_qp_id names a send queue rather than a receive queue
 * and collides with the ids inbound requests carry.  A response takes no part
 * in receive-side ordering: it retires a pending work request, and holding one
 * behind an RNR'd SEND stalls this QP's send queue until that SEND times out.
 */
static bool inmsg_is_response(const struct dp_inmsg *m)
{
    uint8_t op = m->buf[IONIC_WIRE_OP_OFF];

    return op == IONIC_WIRE_ACK || op == IONIC_WIRE_READ_RESP ||
           op == IONIC_WIRE_ATOMIC_RESP;
}

bool ionic_datapath_has_work(struct ionic_datapath *dp)
{
    bool work;

    if (!dp)
        return false;

    qemu_mutex_lock(&dp->rx_lock);
    work = dp->rx_fresh != 0;
    qemu_mutex_unlock(&dp->rx_lock);

    if (!work)
        work = ionic_dp_s3_has_work(dp);
    return work;
}

/*
 * Log the messages a full queue has dropped since this was last called,
 * which it did because nothing polled it (see dp_mesh_recv()).  On the main
 * thread, since a receive thread has no vfio-user context to log to.
 */
static void dp_rx_log_dropped(struct ionic_datapath *dp, uint64_t dropped)
{
    if (dropped == dp->rx_dropped_logged)
        return;
    vfu_log(dp->vfu_ctx, LOG_WARNING,
            "ionic_datapath: dropped %" PRIu64 " mesh message(s) that came "
            "while the queue was full and the device wasn't polling it",
            dropped - dp->rx_dropped_logged);
    dp->rx_dropped_logged = dropped;
}

/*
 * At the end of a pass that leaves @kept messages of @kept_bytes waiting for
 * a receive: whether they leave room for the largest message the queue has
 * refused.  If they do, the receive thread's next try takes it.  If not,
 * they are what keeps it out, and the passes give up on them once they have
 * done so for DP_RX_CROWDED_MS.
 */
static void dp_rx_note_crowding(struct ionic_datapath *dp, uint32_t kept,
                                size_t kept_bytes, uint64_t now)
    TSA_REQUIRES(dp->rx_lock)
{
    if (dp->rx_waiting != 0 && dp_rx_room(kept, kept_bytes, dp->rx_waiting))
        dp->rx_waiting = 0;

    if (dp->rx_waiting == 0) {
        dp->rx_crowded = false;
    } else if (!dp->rx_crowded) {
        dp->rx_crowded = true;
        dp->rx_crowded_ms = now;
    }
}

void ionic_datapath_poll(struct ionic_datapath *dp)
{
    struct dp_inmsg *list;
    uint64_t dropped, now;
    uint32_t queued;
    size_t queued_bytes;

    if (!dp)
        return;

    ionic_dp_s3_poll(dp);

    now = ionic_dp_now_ms(dp);
    qemu_mutex_lock(&dp->rx_lock);
    list = dp->rx_head;
    dp->rx_head = dp->rx_tail = NULL;
    dp->rx_fresh = 0;
    dp->rx_in_pass = true;
    dropped = dp->rx_dropped;
    queued = dp->rx_msgs;
    queued_bytes = dp->rx_bytes;
    qemu_mutex_unlock(&dp->rx_lock);

    dp_rx_log_dropped(dp, dropped);

    /*
     * Messages that cannot be delivered yet go on @defer and are put back for
     * the next poll.  Any further request for the same QP has to be deferred
     * with it, or a later SEND would overtake an earlier one; traffic for
     * other QPs, and every response, keeps flowing.
     *
     * What is put back stays in the queue's count, and a receive thread
     * waits for room in it.  Once what the passes leave has kept a message
     * out for DP_RX_CROWDED_MS, this pass gives up on every SEND still
     * waiting, so the queue empties.
     */
    struct dp_inmsg *defer_head = NULL, *defer_tail = NULL;
    bool give_up =
        dp->rx_crowded && now - dp->rx_crowded_ms >= DP_RX_CROWDED_MS;
    size_t done_bytes = 0, kept_bytes = 0;
    uint32_t done = 0, kept = 0;

    if (give_up)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: SENDs waiting for a receive have filled the "
                "mesh's inbound queue (%" PRIu32 " messages, %zu bytes) for "
                "%" PRIu64 " ms; those still without one fail now",
                queued, queued_bytes, now - dp->rx_crowded_ms);

    while (list) {
        struct dp_inmsg *m = list;
        list = m->next;
        m->next = NULL;

        bool blocked = false;
        if (!inmsg_is_response(m)) {
            for (struct dp_inmsg *d = defer_head; d; d = d->next) {
                if (inmsg_dst_qp(d) == inmsg_dst_qp(m)) {
                    blocked = true;
                    break;
                }
            }
        }

        if (!blocked) {
            if (!m->first_try_ms)
                m->first_try_ms = now;

            if (dp_handle_wire(dp, m->src_node, m->buf, m->len,
                               !give_up &&
                                   now - m->first_try_ms < RNR_RETRY_MS)) {
                done++;
                done_bytes += m->len;
                dp_inmsg_free(m);
                continue;
            }

            /*
             * Each pass that finds no receive is one RNR NAK on a real
             * responder; the message itself is short of a buffer once.
             */
            dp_count(dp, inmsg_dst_qp(m), IONIC_DP_RESP_TX_RNR_RETRY_ERR);
            if (!m->rnr_counted) {
                dp_count(dp, inmsg_dst_qp(m), IONIC_DP_RESP_RX_OUT_OF_BUFFER);
                m->rnr_counted = true;
            }
        }

        if (defer_tail)
            defer_tail->next = m;
        else
            defer_head = m;
        defer_tail = m;
        kept++;
        kept_bytes += m->len;
    }

    qemu_mutex_lock(&dp->rx_lock);
    if (defer_head) {
        defer_tail->next = dp->rx_head;
        dp->rx_head = defer_head;
        if (!dp->rx_tail)
            dp->rx_tail = defer_tail;
    }
    dp->rx_msgs -= done;
    dp->rx_bytes -= done_bytes;
    dp_rx_note_crowding(dp, kept, kept_bytes, now);
    dp->rx_in_pass = false;
    /* The pass can have been long */
    dp->rx_polled_ms = ionic_dp_now_ms(dp);
    qemu_mutex_unlock(&dp->rx_lock);

    /*
     * Retry the send queues that are waiting for a local peer's receive, or
     * for room to queue a request to another node.
     */
    for (uint32_t i = 0; dp->sq_stalled && i < dp->qp_count; i++)
        if (dp->qp[i].valid && (dp->qp[i].sq_rnr || dp->qp[i].sq_busy))
            sq_service(dp, &dp->qp[i], i);

    /* A peer that never answers must not wedge the guest's send queue. */
    for (int i = 0; i < MAX_PENDING; i++) {
        struct dp_pending *p = &dp->pending[i];
        if (p->valid && now > p->deadline_ms) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u request %u timed out", p->qp_id,
                    p->req_id);
            /*
             * A peer that is slow but still reading never trips the mesh's
             * send timeout, so the request can still be queued.  Sent after
             * this, a WRITE or atomic would run after its error completion.
             */
            dp_mesh_cancel_request(dp, p);
            pending_complete(dp, p, IONIC_STS_RETRY_EXCEEDED);
        }
    }
}

/* -------------------------------------------------------------------------
 * Doorbell handler
 *
 * Doorbell layout (struct ionic_doorbell, little-endian):
 *   le16 p_index   [0:1]
 *   u8   ring      [2]    — 0 = work, 1 = arm, 2 = arm solicited,
 *                           3 = SRQ (IONIC_DBELL_RING_SRQ)
 *   u8   qid_lo    [3]
 *   le16 qid_hi    [4:5]
 * -------------------------------------------------------------------------
 */

#define DP_DB_RING_SRQ 3u

/*
 * Advance a receive ring's producer from a doorbell's index.
 *
 * The index names a slot, not a running total. rdma-core's
 * ionic_queue_produce() is "q->prod = (q->prod + 1) & q->mask", so it counts
 * 0, 1, ..., depth-1, 0, 1 and never grows past the ring; the kernel driver
 * does the same.
 *
 * Reading it as a free-running 16-bit counter and adding the difference --
 * which is what this did -- agrees exactly until the ring first wraps, and
 * never again. Posting one more entry after a full lap subtracts rather than
 * adds: with an 8-deep ring at prod 7, an index of 1 yields (uint16_t)(1-7)
 * = 65530, so the device concludes 65530 receives are outstanding. From
 * there "no receive posted" is undetectable, so RNR retry never fires, and
 * the device reads ring entries the guest has already reclaimed -- scattering
 * into buffers the application got back and completing work requests that
 * are no longer outstanding. Traffic that always reposts before data arrives
 * never shows it.
 *
 * Depth is a power of two for both ring kinds (1u << depth_log2), so the
 * mask below is the modular difference.
 *
 * Returns false for an index the ring has no slot for, which is a malformed
 * doorbell rather than a wrap.
 */
static bool rq_prod_advance(uint32_t *prod, uint32_t depth, uint16_t p_index)
{
    if (depth == 0 || (uint32_t)p_index >= depth)
        return false;
    *prod += ((uint32_t)p_index - (*prod % depth)) & (depth - 1u);
    return true;
}

/*
 * A receive posted to the SRQ @qid. Its doorbell comes in two forms, both
 * with the SRQ's id: the guest kernel, and the provider with our
 * ansible/files/patches/rdma-core/0005, ring the RQ qtype, as for a QP's
 * own RQ; the provider without it (rdma-core #1799, as proposed upstream)
 * rings the CQ qtype with ring 3, IONIC_DBELL_RING_SRQ. Missing either is a
 * hang rather than an error: a QP on the SRQ completes its sends and never
 * a receive.
 */
static void srq_doorbell(struct ionic_datapath *dp, uint32_t qid,
                         uint16_t p_index)
{
    struct ionic_srq_ring *sr = srq_by_qid(dp, qid);
    bool posted = false;

    /* Lock first, then read valid: sampling it outside the lock is what let
     * this race unregister_srq. srq_by_qid() only does the bounds
     * arithmetic, which is stable for the datapath's life. */
    if (sr) {
        qemu_mutex_lock(&sr->lock);
        if (sr->valid)
            posted = rq_prod_advance(&sr->prod, sr->depth, p_index);
        qemu_mutex_unlock(&sr->lock);
    }
    if (!posted)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: doorbell for unregistered SRQ %u", qid);
}

void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val)
{
    uint16_t p_index = (uint16_t)(doorbell_val & 0xffffu);
    uint8_t ring = (uint8_t)((doorbell_val >> 16) & 0xffu);
    uint32_t qid = (uint32_t)(((doorbell_val >> 24) & 0xffu) |
                              (((doorbell_val >> 32) & 0xffffu) << 8));

    vfu_log(dp->vfu_ctx, LOG_DEBUG,
            "ionic_datapath: doorbell qtype=%d qid=%u ring=%u p_index=%u",
            qtype, qid, ring, p_index);

    switch (qtype) {
    case DP_QTYPE_CQ: {
        /* Ring 3 names an SRQ, not a CQ, and SRQ ids overlap CQ ids, so it
         * is told apart before the id is read as a CQ's. */
        if (ring == DP_DB_RING_SRQ) {
            srq_doorbell(dp, qid, p_index);
            return;
        }
        if (qid >= dp->cq_count || !dp->cq[qid].valid)
            return;
        struct ionic_cq_ring *c = &dp->cq[qid];
        /*
         * ring 0 carries the consumer index; rings 1 and 2 are arm-any and
         * arm-solicited.  The arm doorbell's own index is the driver's
         * arm_any_prod, not a consumer index, so only ring 0 may update cons.
         */
        if (ring == 0) {
            c->cons = p_index % c->depth;
        } else if (ring == 1 || ring == 2) {
            c->armed = true;
            if (c->prod % c->depth != c->cons)
                cq_fire_event(dp, c, qid);
        }
        return;
    }

    case DP_QTYPE_RQ:
        /*
         * Shared receive queues arrive here too. ionic_post_recv_common()
         * rings rq_qtype for an SRQ exactly as for a QP's own RQ, so this
         * one qtype carries both and the id range is what separates them --
         * see IONIC_SRQ_QID_BASE. Looking only at dp->qp below is what
         * silently dropped every SRQ receive: the doorbell landed on a
         * qp_id out of range and returned, so a SEND to an SRQ-bound QP
         * found no buffer and simply never completed.
         */
        if (qid >= IONIC_SRQ_QID_BASE) {
            srq_doorbell(dp, qid, p_index);
            return;
        }
        if (qid < dp->qp_count && dp->qp[qid].valid) {
            if (rq_prod_advance(&dp->qp[qid].rq_prod, dp->qp[qid].rq_depth,
                                p_index))
                ernic_stats_qp_doorbell(dp->stats, qid, false);
        }
        return;

    case DP_QTYPE_SQ:
        break;

    default:
        return;
    }

    if (qid >= dp->qp_count || !dp->qp[qid].valid)
        return;

    ernic_stats_qp_doorbell(dp->stats, qid, true);

    struct ionic_qp_ring *q = &dp->qp[qid];

    /* Producer indices are 16-bit and free-running modulo the ring depth. */
    q->sq_prod = (uint32_t)p_index % q->sq_depth;
    sq_service(dp, q, qid);
}

void ionic_datapath_rdma_counters(const struct ionic_datapath *dp,
                                  uint32_t qp_id,
                                  uint64_t out[IONIC_DP_RDMA_COUNT])
{
    const uint64_t *src = NULL;

    memset(out, 0, IONIC_DP_RDMA_COUNT * sizeof(out[0]));
    if (!dp)
        return;
    if (qp_id == IONIC_DP_DEVICE)
        src = dp->rdma;
    else if (qp_id < dp->qp_count && dp->qp[qp_id].valid)
        src = dp->qp[qp_id].rdma;
    if (src)
        memcpy(out, src, IONIC_DP_RDMA_COUNT * sizeof(out[0]));
}

void ionic_datapath_reset_rdma_counters(struct ionic_datapath *dp)
{
    if (!dp)
        return;
    memset(dp->rdma, 0, sizeof(dp->rdma));
    for (uint32_t i = 0; i < dp->qp_count; i++)
        memset(dp->qp[i].rdma, 0, sizeof(dp->qp[i].rdma));
}

/*
 * A driver that resets RDMA -- reloaded, or in a guest that rebooted -- has
 * already freed the memory behind everything it registered, without
 * destroying any of it.  A queue left registered would read and write pages
 * the guest now uses for something else, and an MR would let a peer's RDMA
 * WRITE do the same.  QPs go first, as the driver destroys them, so nothing
 * still names an SRQ or CQ when it goes.
 */
void ionic_datapath_reset_rdma(struct ionic_datapath *dp)
{
    if (!dp)
        return;
    for (uint32_t i = 0; i < dp->qp_count; i++)
        if (dp->qp[i].valid)
            ionic_datapath_unregister_qp(dp, i);
    /* Checks valid under the SRQ's lock itself */
    for (uint32_t i = 0; i < dp->srq_count; i++)
        ionic_datapath_unregister_srq(dp, IONIC_SRQ_QID_BASE + i);
    for (uint32_t i = 0; i < dp->cq_count; i++)
        if (dp->cq[i].valid)
            ionic_datapath_unregister_cq(dp, i);
    for (uint32_t i = 0; i < dp->ah_count; i++)
        ionic_datapath_unregister_ah(dp, i);
    for (uint32_t i = 0; i < ERNIC_MAX_MR; i++) {
        if (dp->mr[i].valid) {
            ionic_dp_buf_release(&dp->mr[i].buf);
            dp->mr[i].valid = false;
        }
    }
}
