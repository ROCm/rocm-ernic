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
 * Wire formats: ionic_fw.h (ionic_v1_wqe, ionic_v1_cqe, ionic_sge).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <syslog.h>
#include <endian.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <infiniband/verbs.h>
#include <vfio-user/libvfio-user.h>

#include "dma_range.h"
#include "ernic_ib.h"
#include "ernic_time.h"
#include "ionic_datapath.h"
#ifdef ERNIC_HAVE_P4
#include "p4_engine.h"
#include "p4_exec.h"
#include "p4_fp8.h"
#include "p4_fmt.h"
#endif
#include "ionic_datapath_priv.h"
#include "ionic_eth_emu.h"
#include "net/net_headers.h"
#include "nvmeof.h"
#include "nvmeof_cm.h"
#include "nvmeof_target.h"
#include "rocm_ernic_compat.h"
#include "s3_tcp.h"
#include "s3_target.h"

/* -------------------------------------------------------------------------
 * ionic_fw.h wire format constants (keep in sync with pinned kernel ref)
 * -------------------------------------------------------------------------
 */

/* enum ionic_v1_op */
#define IONIC_V1_OP_SEND           0
#define IONIC_V1_OP_SEND_INV       1
#define IONIC_V1_OP_SEND_IMM       2
#define IONIC_V1_OP_RDMA_READ      3
#define IONIC_V1_OP_RDMA_WRITE     4
#define IONIC_V1_OP_RDMA_WRITE_IMM 5
#define IONIC_V1_OP_ATOMIC_CS      6
#define IONIC_V1_OP_ATOMIC_FA      7
#define IONIC_V1_OP_REG_MR         8
#define IONIC_V1_OP_LOCAL_INV      9

/*
 * The per-QP wqes_by_opcode histogram is indexed by PVRDMA_WR_*, whose
 * numbering is a different permutation of the same operations than
 * enum ionic_v1_op above, so the two spaces need an explicit translation.
 * Every ionic opcode has exactly one PVRDMA counterpart; the reverse does
 * not hold (LSO and the indices above SEND_WITH_INV are unreachable here).
 */
static unsigned int ionic_op_to_pvrdma_wr(uint8_t op)
{
    static const uint8_t map[] = {
        [IONIC_V1_OP_SEND] = 2,           /* PVRDMA_WR_SEND */
        [IONIC_V1_OP_SEND_INV] = 8,       /* PVRDMA_WR_SEND_WITH_INV */
        [IONIC_V1_OP_SEND_IMM] = 3,       /* PVRDMA_WR_SEND_WITH_IMM */
        [IONIC_V1_OP_RDMA_READ] = 4,      /* PVRDMA_WR_RDMA_READ */
        [IONIC_V1_OP_RDMA_WRITE] = 0,     /* PVRDMA_WR_RDMA_WRITE */
        [IONIC_V1_OP_RDMA_WRITE_IMM] = 1, /* PVRDMA_WR_RDMA_WRITE_WITH_IMM */
        [IONIC_V1_OP_ATOMIC_CS] = 5,      /* PVRDMA_WR_ATOMIC_CMP_AND_SWP */
        [IONIC_V1_OP_ATOMIC_FA] = 6,      /* PVRDMA_WR_ATOMIC_FETCH_AND_ADD */
        [IONIC_V1_OP_REG_MR] = 11,        /* PVRDMA_WR_FAST_REG_MR */
        [IONIC_V1_OP_LOCAL_INV] = 10,     /* PVRDMA_WR_LOCAL_INV */
    };

    /* Out of range counts toward the QP total but no histogram bucket. */
    return op < sizeof(map) / sizeof(map[0]) ? map[op] : UINT_MAX;
}

/* enum ionic_v1_flag (be16 at WQE byte 10) */
#define IONIC_V1_FLAG_INL 0x0004u
#define IONIC_V1_FLAG_SIG 0x0008u

/*
 * struct ionic_v1_wqe:
 *   [0:7]   u64  wqe_id (native endian: SQ producer index / RQ meta index)
 *   [8]     u8   op
 *   [9]     u8   num_sge_key
 *   [10:11] be16 flags
 *   [12:15] be32 imm_data_key  (recv: total posted length)
 *   send common body:
 *     [16:19] be32 ah_id
 *     [20:23] be32 dest_qpn
 *     [24:27] be32 dest_qkey
 *     [28:31] be32 length
 *   recv body: [16:31] reserved
 *   [32...] payload: struct ionic_sge { be64 va; be32 len; be32 lkey; }
 */
#define WQE_PLD_OFF      32
#define WQE_SEND_LEN_OFF 28

/* The SRQ stride floor exists to guarantee a WQE header is actually present
 * before deliver_recv() reads one. Tie the two together rather than leaving
 * a 5 and a 32 to drift apart in separate headers. */
_Static_assert((1u << IONIC_SRQ_MIN_STRIDE_LOG2) >= WQE_PLD_OFF,
               "SRQ stride floor must cover the WQE header");
_Static_assert((1u << IONIC_WQE_MIN_STRIDE_LOG2) >= WQE_PLD_OFF,
               "WQE stride floor must cover the WQE header");
/* deliver_recv() reads an SRQ's WQEs into a buffer sized for a QP's. */
_Static_assert(IONIC_SRQ_MAX_STRIDE_LOG2 <= IONIC_WQE_MAX_STRIDE_LOG2,
               "SRQ stride ceiling must fit the receive WQE buffer");

/* The datagram half of the send body: which address handle, and which
 * remote QPN. Connected sends ignore both. */
#define WQE_SEND_AH_OFF       16
#define WQE_SEND_DEST_QPN_OFF 20
#define WQE_SEND_QKEY_OFF     24

/* struct ionic_v1_common_bdy.rdma overlays the send body: two be32 halves of
 * the remote va at 16/20, then the remote rkey, then the shared length. */
#define WQE_RDMA_VA_HI_OFF 16
#define WQE_RDMA_VA_LO_OFF 20
#define WQE_RDMA_RKEY_OFF  24

/* struct ionic_v1_atomic_bdy shares the first three fields with the rdma body
 * and then carries its operands and a single fixed 8-byte result SGE. */
/* ionic_v1_reg_mr_bdy, after the 16-byte base header: be64 va, be64 length,
 * be64 offset, be64 dma_addr, be32 map_count, be16 flags, u8 dir_size_log2,
 * u8 page_size_log2. */
#define WQE_REG_MR_VA_OFF         16
#define WQE_REG_MR_LENGTH_OFF     24
#define WQE_REG_MR_DMA_ADDR_OFF   40
#define WQE_REG_MR_MAP_COUNT_OFF  48
#define WQE_REG_MR_PAGE_SZ_L2_OFF 55
#define WQE_REG_MR_MIN_SZ         56

#define WQE_ATOMIC_SWAP_ADD_OFF 28
#define WQE_ATOMIC_COMPARE_OFF  36
#define WQE_ATOMIC_SGE_OFF      48

/* struct ionic_v1_cqe (32 bytes) */
#define CQE_SIZE          32
#define CQE_COLOR_BIT     0x01u
#define CQE_ERROR_BIT     0x02u
#define CQE_TYPE_RECV     (1u << 5)
#define CQE_TYPE_SEND_MSN (2u << 5)
#define CQE_TYPE_SEND_NPG (3u << 5)

_Static_assert((1u << IONIC_CQ_MIN_STRIDE_LOG2) == CQE_SIZE,
               "CQ stride floor must be one CQE");

/* enum ionic_v1_cqe_src_qpn_bits */
#define CQE_RECV_OP_SHIFT    24
#define CQE_RECV_OP_SEND     0
#define CQE_RECV_OP_SEND_INV 1
#define CQE_RECV_OP_SEND_IMM 2
#define CQE_RECV_OP_RDMA_IMM 3
#define CQE_RECV_IS_IPV4     (1u << (7 + CQE_RECV_OP_SHIFT))

/*
 * enum ionic_status from the guest driver's ionic_fw.h, which runs these
 * through ionic_to_ib_status() to reach an ib_wc_status.  LOCAL_LEN_ERR is a
 * receive buffer too small for the inbound message; LOCAL_PROT_ERR is an SGE
 * that does not lie inside the MR its own key names.
 */
#define IONIC_STS_OK                 0
#define IONIC_STS_LOCAL_LEN_ERR      1
#define IONIC_STS_LOCAL_QP_OPER_ERR  2
#define IONIC_STS_LOCAL_PROT_ERR     3
#define IONIC_STS_WQE_FLUSHED_ERR    4
#define IONIC_STS_LOCAL_ACC_ERR      7
#define IONIC_STS_REMOTE_ACC_ERR     9
#define IONIC_STS_RETRY_EXCEEDED     11
#define IONIC_STS_RNR_RETRY_EXCEEDED 12

/*
 * Largest payload one remote WQE may carry: whatever the mesh will accept in a
 * single message, less our own header.  perftest goes up to 8 MiB, so a cap
 * below that turns into FAIL rows rather than slow ones.
 */
#define IONIC_WIRE_MAX_PAYLOAD \
    ((uint32_t)(IONIC_MESH_MAX_MSG - sizeof(struct ionic_wire_hdr)))

/* A mesh message parked by the receive thread for the vfu thread to apply. */
struct dp_inmsg {
    struct dp_inmsg *next;
    uint32_t src_node;
    size_t len;
    uint64_t first_try_ms; /* 0 until the first delivery attempt */
    bool rnr_counted;      /* resp_rx_out_of_buffer already has it */
    uint8_t buf[];
};

/* -------------------------------------------------------------------------
 * Byte accounting
 * -------------------------------------------------------------------------
 */

void ionic_dp_rdma_bytes(struct ionic_datapath *dp, uint32_t qp_id,
                         uint64_t bytes, enum pvrdma_stat_op op,
                         unsigned int dirs)
{
    pvrdma_rdma_bytes_count(dp->pvrdma_handle, qp_id, bytes, op);
    if (dirs & DP_RDMA_TX)
        ionic_eth_emu_count_rdma(dp->eth_emu, bytes, true);
    if (dirs & DP_RDMA_RX)
        ionic_eth_emu_count_rdma(dp->eth_emu, bytes, false);
}

/* -------------------------------------------------------------------------
 * DMA helpers
 * -------------------------------------------------------------------------
 */

/*
 * How many host mappings one transfer may span.  A single guest run is usually
 * one mapping, but the guest's view is contiguous where the host's need not be,
 * so ask for room and split the transfer if even that is not enough.
 */
#define DP_DMA_SGL_MAX 128

static int dp_dma_rw(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len,
                     bool write)
{
    dma_sg_t *sg;
    struct iovec iov[DP_DMA_SGL_MAX];
    size_t done = 0;
    int cnt;

    if (!len)
        return 0;

    if (!ernic_dma_range_ok(gpa, len))
        return -EINVAL;

    sg = malloc(dma_sg_size() * DP_DMA_SGL_MAX);
    if (!sg)
        return -ENOMEM;

    cnt = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg,
                          DP_DMA_SGL_MAX, write ? PROT_WRITE : PROT_READ);
    if (cnt < 0) {
        free(sg);
        /*
         * Too fragmented to describe in one list.  Halving is enough to make
         * progress; the caller's loop is driven by what we report, not by a
         * fixed step.
         */
        if (errno == ENOSPC && len > 4096) {
            size_t half = len / 2;
            int rc = dp_dma_rw(vfu_ctx, gpa, buf, half, write);
            if (rc < 0)
                return rc;
            return dp_dma_rw(vfu_ctx, gpa + half, (char *)buf + half,
                             len - half, write);
        }
        return -EIO;
    }

    size_t nsg = (size_t)cnt;

    if (vfu_sgl_get(vfu_ctx, sg, iov, nsg, 0) < 0) {
        free(sg);
        return -EIO;
    }

    for (size_t i = 0; i < nsg && done < len; i++) {
        size_t n = iov[i].iov_len;

        if (n > len - done)
            n = len - done;
        if (write)
            memcpy(iov[i].iov_base, (const char *)buf + done, n);
        else
            memcpy((char *)buf + done, iov[i].iov_base, n);
        done += n;
    }

    if (write)
        vfu_sgl_mark_dirty(vfu_ctx, sg, nsg);
    vfu_sgl_put(vfu_ctx, sg, iov, nsg);
    free(sg);
    return done == len ? 0 : -EIO;
}

static int dp_dma_read(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len)
{
    return dp_dma_rw(vfu_ctx, gpa, buf, len, false);
}

static int dp_dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa, const void *buf,
                        size_t len)
{
    /* dp_dma_rw does not modify buf on the write path. */
    return dp_dma_rw(vfu_ctx, gpa, (void *)(uintptr_t)buf, len, true);
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
 * that is not a geometry the device could honour.
 */
int ionic_dp_buf_init(struct ionic_datapath *dp, struct dp_buf *b,
                      const struct ionic_dp_buf_desc *desc, uint64_t va,
                      uint64_t span)
{
    ionic_dp_buf_release(b);
    if (desc->page_size_log2 > 30)
        return -EINVAL;
    b->page_size_log2 = desc->page_size_log2 ? desc->page_size_log2 : 12;

    uint64_t page_size = 1ull << b->page_size_log2;
    uint64_t max_pages = ((span + page_size - 1) >> b->page_size_log2) + 1;

    if (span == 0 || (uint64_t)desc->map_count > max_pages) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: page table of %u entries for a %" PRIu64
                "-byte region at 2^%u pages (at most %" PRIu64 ")",
                desc->map_count, span, b->page_size_log2, max_pages);
        return -EINVAL;
    }
    b->npages = desc->map_count;

    if (desc->map_count <= 1) {
        /* ionic_pgtbl_dma() already folded the intra-page offset in. */
        b->base = desc->dma_addr;
        b->npages = 1;
        return 0;
    }

    b->pages = calloc(desc->map_count, sizeof(*b->pages));
    if (!b->pages) {
        b->npages = 0;
        return -ENOMEM;
    }
    if (dp_dma_read(dp->vfu_ctx, desc->dma_addr, b->pages,
                    (size_t)desc->map_count * 8) < 0) {
        ionic_dp_buf_release(b);
        return -EIO;
    }
    for (uint32_t i = 0; i < desc->map_count; i++)
        b->pages[i] = le64toh(b->pages[i]);

    b->first_off = (uint32_t)(va & ((1ull << b->page_size_log2) - 1));
    return 0;
}

/*
 * Resolve a byte offset within a buffer to a guest physical address, and
 * report how many bytes remain contiguous from there.
 */
static uint64_t buf_gpa(const struct dp_buf *b, uint64_t off, uint64_t *run)
{
    if (b->npages <= 1 || !b->pages) {
        *run = UINT64_MAX;
        return b->base + off;
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

    dp->qp = calloc(MAX_QP, sizeof(*dp->qp));
    dp->cq = calloc(MAX_CQ, sizeof(*dp->cq));
    dp->srq = calloc(MAX_SRQ, sizeof(*dp->srq));
    dp->ah = calloc(MAX_AH, sizeof(*dp->ah));
    if (!dp->qp || !dp->cq || !dp->srq || !dp->ah) {
        free(dp->qp);
        free(dp->cq);
        free(dp->srq);
        free(dp->ah);
        free(dp);
        return NULL;
    }
    dp->qp_count = MAX_QP;
    dp->cq_count = MAX_CQ;
    dp->srq_count = MAX_SRQ;
    dp->ah_count = MAX_AH;
    dp->local_node = UINT32_MAX;
    pthread_mutex_init(&dp->rx_lock, NULL);
    /* One init per slot, here rather than per registration: a reader that
     * samples srq->valid outside the lock must still find a lock it can
     * take, whatever the control path has done to that slot since. */
    for (uint32_t i = 0; i < dp->srq_count; i++)
        pthread_mutex_init(&dp->srq[i].lock, NULL);
    return dp;
}

static void dp_mesh_recv(void *opaque, uint32_t src_node, const void *buf,
                         size_t len);

bool ionic_datapath_attach_nvmeof(struct ionic_datapath *dp,
                                  const struct nvmeof_target_cfg *cfg,
                                  char *err, size_t errlen)
{
    if (!dp || !cfg)
        return false;

    dp->nvmeof = nvmeof_target_create(cfg, err, errlen);
    if (!dp->nvmeof)
        return false;

    dp->nvmeof_cm = nvmeof_cm_create(dp->nvmeof, cfg->traddr, cfg->trsvcid);
    if (!dp->nvmeof_cm) {
        nvmeof_target_destroy(dp->nvmeof);
        dp->nvmeof = NULL;
        snprintf(err, errlen, "out of memory");
        return false;
    }

    /* The target's copy has the size an existing image file settled on. */
    cfg = nvmeof_target_config(dp->nvmeof);
    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: nvmeof target '%s' %" PRIu64
            " bytes bs=%u at %u.%u.%u.%u:%u",
            cfg->subnqn, cfg->size, cfg->block_size, cfg->traddr >> 24,
            (cfg->traddr >> 16) & 0xff, (cfg->traddr >> 8) & 0xff,
            cfg->traddr & 0xff, cfg->trsvcid);
    return true;
}

void ionic_datapath_set_pvrdma(struct ionic_datapath *dp,
                               pvrdma_handle_t handle)
{
    uint32_t node;
    bool changed;

    if (!dp)
        return;

    dp->pvrdma_handle = handle;
    if (!handle)
        return;

    node = ionic_mesh_local_node(dp->pvrdma_handle);
    changed = !dp->mesh_reported || node != dp->local_node;
    dp->local_node = node;
    dp->mesh_reported = true;

    if (node == UINT32_MAX) {
        if (changed)
            vfu_log(dp->vfu_ctx, LOG_INFO,
                    "ionic_datapath: no mesh backend, remote QPs unreachable");
        return;
    }

    ionic_mesh_set_recv_cb(dp->pvrdma_handle, dp_mesh_recv, dp);
    if (changed)
        vfu_log(dp->vfu_ctx, LOG_INFO, "ionic_datapath: mesh node %u", node);
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

    if (dp->pvrdma_handle)
        ionic_mesh_set_recv_cb(dp->pvrdma_handle, NULL, NULL);

    nvmeof_cm_destroy(dp->nvmeof_cm);
    nvmeof_target_destroy(dp->nvmeof);

    if (dp->eth_emu && dp->s3_tcp)
        ionic_eth_emu_register_tx_filter(dp->eth_emu, NULL, NULL);
    s3_tcp_destroy(dp->s3_tcp);
    s3_target_destroy(dp->s3);

    pthread_mutex_lock(&dp->rx_lock);
    for (struct dp_inmsg *m = dp->rx_head; m;) {
        struct dp_inmsg *next = m->next;
        free(m);
        m = next;
    }
    dp->rx_head = dp->rx_tail = NULL;
    dp->rx_fresh = 0;
    pthread_mutex_unlock(&dp->rx_lock);
    pthread_mutex_destroy(&dp->rx_lock);

    for (uint32_t i = 0; i < dp->qp_count; i++) {
        ionic_dp_buf_release(&dp->qp[i].sq_buf);
        ionic_dp_buf_release(&dp->qp[i].rq_buf);
    }
    for (uint32_t i = 0; i < dp->cq_count; i++)
        ionic_dp_buf_release(&dp->cq[i].buf);
    for (int i = 0; i < MAX_MR; i++)
        ionic_dp_buf_release(&dp->mr[i].buf);
    free(dp->qp);
    free(dp->cq);
    for (uint32_t i = 0; i < dp->srq_count; i++) {
        /* Each live SRQ owns the page table ionic_dp_buf_init() built for it.
         * Destroying only the mutex leaked one allocation per SRQ on every
         * normal device shutdown. The mutex is destroyed for every slot,
         * live or not, because every slot's was initialised at create. */
        if (dp->srq[i].valid)
            ionic_dp_buf_release(&dp->srq[i].buf);
        pthread_mutex_destroy(&dp->srq[i].lock);
    }
    free(dp->srq);
    free(dp->ah);
    free(dp);
}

/* -------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------
 */

void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint32_t eq_id,
                                const struct ionic_dp_ring_desc *ring)
{
    if (!dp || cq_id >= dp->cq_count)
        return;

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
        return;
    }
    /*
     * depth * stride is exactly what the ring occupies -- taken from the
     * descriptor, not from c->depth, which is not assigned until this call
     * has succeeded and is zero for a CQ being registered for the first
     * time. Reading it here made the span zero and rejected every CQ in
     * the device, so nothing ever completed while data still moved.
     */
    if (ionic_dp_buf_init(
            dp, &c->buf, &ring->buf, 0,
            ionic_dp_ring_span_bytes(ring->depth_log2, stride_log2)) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQ %u page table read failed", cq_id);
        c->valid = false;
        return;
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
}

void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id)
{
    if (!dp || cq_id >= dp->cq_count)
        return;
    ionic_dp_buf_release(&dp->cq[cq_id].buf);
    dp->cq[cq_id].valid = false;
}

/* Lets @q's send queue move past the WQE that was waiting for a receive. */
static void sq_rnr_clear(struct ionic_datapath *dp, struct ionic_qp_ring *q)
{
    if (!q->sq_rnr)
        return;
    q->sq_rnr = false;
    dp->sq_stalled--;
}

void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint8_t ib_qp_type, uint32_t sq_cq_id,
                                const struct ionic_dp_ring_desc *sq,
                                uint32_t rq_cq_id,
                                const struct ionic_dp_ring_desc *rq)
{
    if (!dp || qp_id >= dp->qp_count)
        return;

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
        return;
    }

    if (ionic_dp_buf_init(
            dp, &q->sq_buf, &sq->buf, 0,
            ionic_dp_ring_span_bytes(sq->depth_log2, sq->stride_log2)) < 0 ||
        ionic_dp_buf_init(
            dp, &q->rq_buf, &rq->buf, 0,
            ionic_dp_ring_span_bytes(rq->depth_log2, rq->stride_log2)) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: QP %u page table read failed", qp_id);
        return;
    }
    q->ib_qp_type = ib_qp_type;
    q->sq_depth = 1u << sq->depth_log2;
    q->rq_depth = 1u << rq->depth_log2;
    q->sq_stride_log2 = sq->stride_log2;
    q->rq_stride_log2 = rq->stride_log2;
    q->sq_cq_id = sq_cq_id;
    q->rq_cq_id = rq_cq_id;
    sq_rnr_clear(dp, q);
    q->sq_cons = q->sq_prod = 0;
    q->rq_prod = q->rq_cons = 0;
    q->msn = 0;
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
}

void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    if (!dp || qp_id >= dp->qp_count)
        return;
    if (dp->nvmeof_cm)
        nvmeof_cm_drop_qp(dp->nvmeof_cm, qp_id);
    sq_rnr_clear(dp, &dp->qp[qp_id]);
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
    pthread_mutex_lock(&sr->lock);

    if (sr->valid) {
        /* Re-registering an id the guest reused: drop the old page table
         * rather than leaking it. */
        ionic_dp_buf_release(&sr->buf);
        sr->valid = false;
    }

    if (ionic_dp_buf_init(
            dp, &sr->buf, &rq->buf, 0,
            ionic_dp_ring_span_bytes(rq->depth_log2, rq->stride_log2)) < 0) {
        pthread_mutex_unlock(&sr->lock);
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: SRQ %u page table read failed", srq_id);
        return -EIO;
    }
    sr->depth = 1u << rq->depth_log2;
    sr->stride_log2 = rq->stride_log2;
    sr->prod = sr->cons = 0;
    sr->valid = true;
    pthread_mutex_unlock(&sr->lock);

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
    pthread_mutex_lock(&sr->lock);
    if (sr->valid) {
        sr->valid = false;
        ionic_dp_buf_release(&sr->buf);
        sr->depth = 0;
        sr->stride_log2 = 0;
        sr->prod = sr->cons = 0;
    }
    pthread_mutex_unlock(&sr->lock);
}

void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id)
{
    if (!dp || qp_id >= dp->qp_count || !srq_by_qid(dp, srq_id))
        return;

    /* Stored as a table index, not the guest-visible qid: deliver_recv is
     * on the hot path and should not repeat the arithmetic. */
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
uint32_t ionic_dp_local_node(struct ionic_datapath *dp)
{
    if (!dp || !dp->pvrdma_handle)
        return UINT32_MAX;
    return ionic_mesh_local_node(dp->pvrdma_handle);
}

uint32_t ionic_dp_node_from_gid(struct ionic_datapath *dp, const uint8_t *dgid)
{
    if (!dp || !dp->pvrdma_handle)
        return UINT32_MAX;
    return ionic_mesh_node_from_gid(dp->pvrdma_handle, dgid);
}

static struct dp_mr *mr_find(struct ionic_datapath *dp, uint32_t lkey)
{
    for (int i = 0; i < MAX_MR; i++)
        if (dp->mr[i].valid && dp->mr[i].lkey == lkey)
            return &dp->mr[i];
    return NULL;
}

void ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                                uint64_t va, uint64_t length,
                                const struct ionic_dp_buf_desc *buf)
{
    if (!dp)
        return;

    struct dp_mr *m = mr_find(dp, lkey);
    if (!m) {
        for (int i = 0; i < MAX_MR; i++) {
            if (!dp->mr[i].valid) {
                m = &dp->mr[i];
                break;
            }
        }
    }
    if (!m) {
        vfu_log(dp->vfu_ctx, LOG_WARNING, "ionic_datapath: MR table full");
        return;
    }

    /* A memory region's span is its length; the guest said so in
     * CREATE_MR and the rkey checks already hold it to that. */
    if (ionic_dp_buf_init(dp, &m->buf, buf, va, length) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: MR %#x page table read failed", lkey);
        return;
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
static void dp_reg_mr(struct ionic_datapath *dp, const uint8_t *wqe,
                      uint32_t key)
{
    for (int i = 0; i < MAX_MR; i++) {
        if (dp->mr[i].valid && dp->mr[i].lkey != key &&
            (dp->mr[i].lkey & 0x00ffffffu) == (key & 0x00ffffffu))
            ionic_datapath_unregister_mr(dp, dp->mr[i].lkey);
    }

    uint64_t va, length, dma_addr;
    uint32_t map_count;
    memcpy(&va, wqe + WQE_REG_MR_VA_OFF, 8);
    memcpy(&length, wqe + WQE_REG_MR_LENGTH_OFF, 8);
    memcpy(&dma_addr, wqe + WQE_REG_MR_DMA_ADDR_OFF, 8);
    memcpy(&map_count, wqe + WQE_REG_MR_MAP_COUNT_OFF, 4);

    struct ionic_dp_buf_desc buf = {
        .dma_addr = be64toh(dma_addr),
        .map_count = be32toh(map_count),
        .page_size_log2 = wqe[WQE_REG_MR_PAGE_SZ_L2_OFF],
    };

    ionic_datapath_register_mr(dp, key, be64toh(va), be64toh(length), &buf);
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
static bool mr_covers(struct ionic_datapath *dp, uint32_t key, uint64_t va,
                      uint64_t len)
{
    if (key == 0)
        return true;

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

    struct dp_mr *m = mr_find(dp, lkey);
    if (!m || va < m->va || va >= m->va + m->length) {
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
    uint64_t left = m->va + m->length - va;
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

        if (dp_dma_read(dp->vfu_ctx, src, bounce, (size_t)chunk) < 0 ||
            dp_dma_write(dp->vfu_ctx, dst, bounce, (size_t)chunk) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

/*
 * Move @len bytes between an SGE-described guest region and a host buffer.
 * Used only by the cross-instance path: a mesh message is a linear host
 * payload, while both ends of it are scattered guest pages.
 */
static uint32_t dp_sge_to_host(struct ionic_datapath *dp, uint32_t lkey,
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
        if (dp_dma_read(dp->vfu_ctx, gpa, dst + done, (size_t)chunk) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

static uint32_t dp_host_to_sge(struct ionic_datapath *dp, uint32_t lkey,
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
        if (dp_dma_write(dp->vfu_ctx, gpa, src + done, (size_t)chunk) < 0) {
            dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
            break;
        }
        done += (uint32_t)chunk;
    }
    return done;
}

#ifdef ERNIC_HAVE_P4
/*
 * Blocks the operands are streamed through.  Small enough that three of
 * them on the stack of a function on the poll-loop thread is unremarkable,
 * large enough that a megabyte moves in a few dozen passes.
 */
#define FP8_MAP_CHUNK 2048u

/*
 * Is @key one the guest may name as an operand here?
 *
 * Key 0 is IONIC_DMA_LKEY, which means "the va is already a bus address"
 * and bypasses the memory region table entirely -- mr_covers() returns
 * true for it without looking anything up.  That is a deliberate internal
 * convention, and it is not one a guest-supplied compute request may use:
 * it would let the request name any memory the VMM exported rather than
 * only what the guest registered.
 */
static bool fp8_key_ok(struct ionic_datapath *dp, uint32_t key, uint64_t va,
                       uint32_t len)
{
    return key != 0 && mr_covers(dp, key, va, len);
}

int ionic_datapath_fp8_map(struct ionic_datapath *dp, uint8_t op,
                           uint32_t a_lkey, uint64_t a_va, uint32_t b_lkey,
                           uint64_t b_va, uint32_t len, uint32_t c_rkey,
                           uint64_t c_va)
{
    /*
     * Both operands are staged a block at a time rather than combined in
     * place.  They are independently scattered across guest pages and the
     * operation has to see matching elements of each, which a page-at-a-
     * time walk of two separate scatter lists does not give.
     */
    uint8_t a[FP8_MAP_CHUNK];
    uint8_t b[FP8_MAP_CHUNK];
    uint8_t c[FP8_MAP_CHUNK];
    uint32_t done = 0;

    if (!dp || len == 0 || len > IONIC_FP8_MAP_MAX)
        return -EINVAL;

    /*
     * Every region is checked in full before a byte moves.  Checking as we
     * go would leave a half-written destination behind when the tail of an
     * operand turned out not to be registered, which is the transfer-then-
     * report pattern the rest of this file was fixed away from.
     */
    if (!fp8_key_ok(dp, a_lkey, a_va, len) ||
        !fp8_key_ok(dp, b_lkey, b_va, len) ||
        !fp8_key_ok(dp, c_rkey, c_va, len))
        return -EACCES;

    while (done < len) {
        uint32_t n = len - done;

        if (n > FP8_MAP_CHUNK)
            n = FP8_MAP_CHUNK;

        if (dp_sge_to_host(dp, a_lkey, a_va + done, a, n) != n ||
            dp_sge_to_host(dp, b_lkey, b_va + done, b, n) != n)
            return -EFAULT;

        if (!p4_fp8_map((enum p4_fp8_op)op, a, b, n, c))
            return -EINVAL;

        /* Out through the same path an RDMA WRITE takes, so the result
         * reaches the destination the way any other payload would. */
        if (dp_host_to_sge(dp, c_rkey, c_va + done, c, n) != n)
            return -EFAULT;

        done += n;
    }

    return 0;
}

/*
 * Elements converted per pass.  Even, so a 4-bit format's chunk always
 * lands on a byte boundary at both ends, and small enough that the two
 * staging buffers -- at most four bytes an element each way -- are an
 * unremarkable amount of stack on the poll-loop thread.
 */
#define FMT_CVT_CHUNK_ELEMS 512u

int ionic_datapath_fmt_convert(struct ionic_datapath *dp, uint8_t src_fmt,
                               uint8_t dst_fmt, uint32_t a_lkey, uint64_t a_va,
                               uint32_t in_len, uint32_t c_rkey, uint64_t c_va)
{
    uint8_t in[FMT_CVT_CHUNK_ELEMS * 4u];
    uint8_t out[FMT_CVT_CHUNK_ELEMS * 4u];
    size_t n_elems, out_len;
    uint32_t done = 0;

    if (!dp || in_len == 0)
        return -EINVAL;
    if (!p4_fmt_valid(src_fmt) || !p4_fmt_valid(dst_fmt))
        return -EINVAL;

    /*
     * A source length that is not a whole number of elements, or an
     * element count the destination cannot hold in whole bytes, is
     * malformed rather than merely unsupported: a half element at either
     * end is not an array of either format.
     */
    n_elems = p4_fmt_elems((enum p4_fmt)src_fmt, in_len);
    if (n_elems == 0)
        return -EINVAL;
    if (n_elems > IONIC_FP8_MAP_MAX)
        return -EINVAL;
    out_len = p4_fmt_bytes((enum p4_fmt)dst_fmt, n_elems);
    if (out_len == 0)
        return -EINVAL;

    /* Each region against its own length; they differ here. */
    if (!fp8_key_ok(dp, a_lkey, a_va, in_len) ||
        !fp8_key_ok(dp, c_rkey, c_va, (uint32_t)out_len))
        return -EACCES;

    while (done < n_elems) {
        size_t n = n_elems - done;
        size_t in_bytes, out_bytes;

        if (n > FMT_CVT_CHUNK_ELEMS)
            n = FMT_CVT_CHUNK_ELEMS;
        in_bytes = p4_fmt_bytes((enum p4_fmt)src_fmt, n);
        out_bytes = p4_fmt_bytes((enum p4_fmt)dst_fmt, n);

        if (dp_sge_to_host(dp, a_lkey,
                           a_va + p4_fmt_bytes((enum p4_fmt)src_fmt, done), in,
                           (uint32_t)in_bytes) != in_bytes)
            return -EFAULT;

        if (!p4_fmt_convert((enum p4_fmt)src_fmt, (enum p4_fmt)dst_fmt, in, n,
                            out))
            return -EINVAL;

        if (dp_host_to_sge(dp, c_rkey,
                           c_va + p4_fmt_bytes((enum p4_fmt)dst_fmt, done), out,
                           (uint32_t)out_bytes) != out_bytes)
            return -EFAULT;

        done += (uint32_t)n;
    }

    return 0;
}

#endif /* ERNIC_HAVE_P4 */

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
static void cq_fire_event(struct ionic_datapath *dp, struct ionic_cq_ring *c,
                          uint32_t cq_id)
{
    if (!c->armed)
        return;
    c->armed = false;
    if (dp->cq_event_fn)
        dp->cq_event_fn(dp->cq_event_opaque, c->eq_id, cq_id);
    else if (dp->eth_emu)
        ionic_eth_emu_trigger_irq(dp->eth_emu, (int)c->eq_id);
}

static int cq_write(struct ionic_datapath *dp, struct ionic_cq_ring *c,
                    uint64_t off, const uint8_t *src, size_t len)
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
        if (dp_dma_write(dp->vfu_ctx, gpa, src + done, chunk) < 0)
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

    uint32_t sl = htobe32(status_len);
    memcpy(cqe + 24, &sl, 4);

    uint32_t qtf = (uint32_t)(c->color ? CQE_COLOR_BIT : 0) |
                   (error ? CQE_ERROR_BIT : 0) | type |
                   ((qid & 0xffffffu) << 8);
    qtf = htobe32(qtf);
    memcpy(cqe + 28, &qtf, 4);

    /*
     * No error completion goes out unannounced. One of these ends the queue
     * pair for the consumer -- a provider that polls one stops polling
     * normally and flushes everything behind it, so every later request
     * comes back IB_WC_WR_FLUSH_ERR and the thing that actually went wrong
     * is no longer visible from outside. Saying it here is the difference
     * between a named fault and a consumer that reports only that its work
     * was flushed.
     */
    if (error && status_len != IONIC_STS_WQE_FLUSHED_ERR)
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

    pvrdma_qp_cqe_count(dp->pvrdma_handle, qid);
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
    uint16_t pl = htobe16((uint16_t)paylen);
    memcpy(grh + 4, &pl, 2);
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
    struct ip_header ip;

    memset(grh, 0, IB_GRH_SIZE);
    memset(&ip, 0, sizeof(ip));
    ip.version_ihl = 0x45; /* IPv4, a 5-word header with no options */
    ip.total_len = htobe16((uint16_t)(sizeof(ip) + paylen));
    ip.ttl = 64;
    ip.protocol = IP_PROTOCOL_UDP; /* RoCEv2 rides UDP port 4791 */
    memcpy(&ip.src_ip, sgid + 12, 4);
    memcpy(&ip.dst_ip, dgid + 12, 4);
    ip.checksum = ip_checksum(&ip, sizeof(ip));

    memcpy(grh + IB_GRH_SIZE - sizeof(ip), &ip, sizeof(ip));
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
static void dp_grh_build(uint8_t grh[IB_GRH_SIZE], uint32_t paylen,
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
    qpn_op = htobe32(qpn_op);
    memcpy(body + 8, &qpn_op, 4);
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

/* Defined with the other pending_* helpers, below the transmit path. */
static uint32_t pending_cancel_qp(struct ionic_datapath *dp, uint32_t qp_id);

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
     * The flush completes every send outstanding, the one held for a
     * receive included, so none of them may run afterwards: a SEND the
     * stalled-queue retry delivered once the peer posted a receive would
     * land a message the driver has already reported flushed, and complete
     * its work request a second time.
     */
    sq_rnr_clear(dp, q);
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

    uint32_t m = htobe32(msn);
    memcpy(body + 4, &m, 4); /* send.msg_msn */

    cq_post(dp, cq_id, body, status, CQE_TYPE_SEND_MSN, qid,
            status != IONIC_STS_OK);
}

static void cq_post_send_npg(struct ionic_datapath *dp, uint32_t cq_id,
                             uint32_t qid, uint64_t sq_wqe_id)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    /* send.npg_wqe_id is a native u64 masked with sq.mask by the driver. */
    memcpy(body + 16, &sq_wqe_id, 8);

    cq_post(dp, cq_id, body, 0, CQE_TYPE_SEND_NPG, qid, false);
}

/* -------------------------------------------------------------------------
 * WQE processing
 * -------------------------------------------------------------------------
 */

static void parse_sges(const uint8_t *wqe, uint32_t stride, uint8_t num_sge,
                       struct dp_sge_list *out)
{
    uint32_t avail = stride > WQE_PLD_OFF ? stride - WQE_PLD_OFF : 0;
    uint32_t max_sge = avail / 16;

    out->count = 0;
    out->total = 0;
    if (max_sge > MAX_SGE)
        max_sge = MAX_SGE;
    if (num_sge < max_sge)
        max_sge = num_sge;

    for (uint32_t i = 0; i < max_sge; i++) {
        const uint8_t *p = wqe + WQE_PLD_OFF + i * 16;
        uint64_t va;
        uint32_t len, lkey;
        memcpy(&va, p + 0, 8);
        memcpy(&len, p + 8, 4);
        memcpy(&lkey, p + 12, 4);
        out->va[i] = be64toh(va);
        out->len[i] = be32toh(len);
        out->lkey[i] = be32toh(lkey);
        out->total += out->len[i];
        out->count++;
    }
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
        if (!mr_covers(dp, l->lkey[i], l->va[i], l->len[i]))
            return false;
    }
    return true;
}

/* Walk an SGE list into or out of a linear host buffer. */
static uint32_t dp_gather(struct ionic_datapath *dp,
                          const struct dp_sge_list *l, uint8_t *dst,
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
            dp_sge_to_host(dp, l->lkey[i], l->va[i], dst + done, chunk);
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
            dp_host_to_sge(dp, l->lkey[i], l->va[i], src + done, chunk);
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
static void dp_src_mac(struct ionic_datapath *dp, uint32_t src_qp_id,
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
 * Deliver a SEND payload into the destination QP's next posted receive.
 * Returns the number of bytes delivered, or -1 if no receive was available.
 *
 * @src describes the payload.  For a local send it names guest memory and
 * @src_host is NULL; for one that arrived from a peer instance @src_host
 * points at the received bytes and only @src->total is meaningful.
 */
static int64_t deliver_recv(struct ionic_datapath *dp, struct ionic_qp_ring *dq,
                            uint32_t dst_qp_id, uint32_t src_qp_id,
                            const struct dp_sge_list *src,
                            const uint8_t *src_host, uint8_t recv_op,
                            uint32_t imm_be)
{
    /*
     * Which ring the receive comes out of. A QP bound to an SRQ has no RQ
     * of its own -- CREATE_QP left its rq_* fields zero -- so reading
     * dq->rq_* for one would consume from an empty ring forever. The SRQ
     * is shared, so the take runs under its lock: two QPs being delivered
     * to on two threads would otherwise read the same slot and both
     * complete it.
     */
    struct ionic_srq_ring *sr = NULL;
    if (dq->has_srq) {
        if (dq->srq_id >= dp->srq_count)
            return -1;
        sr = &dp->srq[dq->srq_id];
        pthread_mutex_lock(&sr->lock);
        /* valid is read only under the lock. The bounds check above is the
         * one thing safe to do outside it, because srq_count never changes
         * after create. */
        if (!sr->valid) {
            pthread_mutex_unlock(&sr->lock);
            return -1;
        }
    }

    uint32_t *ring_cons = sr ? &sr->cons : &dq->rq_cons;
    uint32_t ring_prod = sr ? sr->prod : dq->rq_prod;
    uint32_t ring_depth = sr ? sr->depth : dq->rq_depth;
    uint8_t ring_stride_log2 = sr ? sr->stride_log2 : dq->rq_stride_log2;
    struct dp_buf *ring_buf = sr ? &sr->buf : &dq->rq_buf;

    if (*ring_cons == ring_prod) {
        if (sr)
            pthread_mutex_unlock(&sr->lock);
        return -1;
    }

    uint32_t stride = 1u << ring_stride_log2;
    uint32_t slot = *ring_cons % ring_depth;
    uint64_t run;
    uint64_t gpa = buf_gpa(ring_buf, (uint64_t)slot * stride, &run);

    /* Big enough for the largest stride a QP's RQ or an SRQ can have, so
     * the whole WQE is read. */
    uint8_t rwqe[1u << IONIC_WQE_MAX_STRIDE_LOG2];
    uint32_t read_sz = stride < sizeof(rwqe) ? stride : (uint32_t)sizeof(rwqe);
    if (!run || run < read_sz ||
        dp_dma_read(dp->vfu_ctx, gpa, rwqe, read_sz) < 0) {
        if (sr)
            pthread_mutex_unlock(&sr->lock);
        return -1;
    }

    uint64_t rq_wqe_id;
    memcpy(&rq_wqe_id, rwqe + 0, 8);

    struct dp_sge_list dst;
    /*
     * read_sz, not stride: parse_sges() derives how many SGEs it may walk
     * from the size it is given, and only read_sz bytes were read. Passing
     * a stride larger than the buffer would authorise walking up to
     * MAX_SGE entries -- byte 543 -- because parse_sges() caps on MAX_SGE
     * and avail/16 but has no idea how much of the WQE actually arrived.
     * The ring geometry checks keep the two equal; read_sz is what keeps
     * the walk inside rwqe if they ever aren't.
     */
    parse_sges(rwqe, read_sz, rwqe[9], &dst);

    (*ring_cons)++;

    /* Everything below works on the copy in rwqe, so the SRQ is free for
     * the next taker before the scatter -- which can be long. */
    if (sr)
        pthread_mutex_unlock(&sr->lock);

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

    ionic_dp_rdma_bytes(dp, dst_qp_id, copied, PVRDMA_STAT_RECV, DP_RDMA_RX);

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

/* Read the be32 pair at @off as one 64-bit value: the wire splits every
 * address and operand into high and low halves. */
static uint64_t wqe_be64_pair(const uint8_t *wqe, uint32_t off)
{
    uint32_t hi, lo;
    memcpy(&hi, wqe + off, 4);
    memcpy(&lo, wqe + off + 4, 4);
    return ((uint64_t)be32toh(hi) << 32) | be32toh(lo);
}

/*
 * Compare-and-swap or fetch-and-add on 8 bytes of remote memory, with the
 * original value returned to the local SGE.  Both operands and remote memory
 * are treated as host-order u64: the driver converts the caller's values to
 * big endian for the wire, so undoing that yields exactly what the
 * application passed, and the same guest owns both sides of the copy.
 */
static bool do_atomic(struct ionic_datapath *dp, const uint8_t *wqe,
                      bool compare_swap)
{
    uint64_t remote_va = wqe_be64_pair(wqe, WQE_RDMA_VA_HI_OFF);
    uint64_t swap_add = wqe_be64_pair(wqe, WQE_ATOMIC_SWAP_ADD_OFF);
    uint64_t compare = wqe_be64_pair(wqe, WQE_ATOMIC_COMPARE_OFF);
    uint32_t rkey;
    memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
    rkey = be32toh(rkey);

    uint64_t local_va, run;
    uint32_t local_lkey;
    memcpy(&local_va, wqe + WQE_ATOMIC_SGE_OFF, 8);
    memcpy(&local_lkey, wqe + WQE_ATOMIC_SGE_OFF + 12, 4);
    local_va = be64toh(local_va);
    local_lkey = be32toh(local_lkey);

    /*
     * The two ends fail differently even though one table resolves both, so
     * each failure is attributed as it happens rather than inferred from the
     * false return.
     */
    if (!mr_covers(dp, rkey, remote_va, 8)) {
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
    if (dp_dma_read(dp->vfu_ctx, rgpa, &old, 8) < 0) {
        dp_fault_set(dp, IONIC_STS_REMOTE_ACC_ERR);
        return false;
    }

    uint64_t new =
        compare_swap ? (old == compare ? swap_add : old) : old + swap_add;

    if (dp_dma_write(dp->vfu_ctx, rgpa, &new, 8) < 0) {
        dp_fault_set(dp, IONIC_STS_REMOTE_ACC_ERR);
        return false;
    }
    if (dp_dma_write(dp->vfu_ctx, lgpa, &old, 8) < 0) {
        dp_fault_set(dp, IONIC_STS_LOCAL_ACC_ERR);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Cross-instance transmit
 * -------------------------------------------------------------------------
 */

static uint64_t dp_now_ms(const struct ionic_datapath *dp)
{
    if (dp->now_ms)
        return dp->now_ms();
    return ernic_monotonic_ms();
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
            dp->pending[i].deadline_ms = dp_now_ms(dp) + PENDING_MS;
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
 * Drop every deferred request of a QP that is flushing, and say how many.
 *
 * Dropped silently: the flush CQE the caller is about to post already tells
 * the driver to complete everything it still has outstanding on this queue,
 * so a completion here would be a second one for the same work request. What
 * matters is that the entry stops being findable, since its SGE list names
 * memory the consumer is free to reuse the moment the flush lands.
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

/* Retire a deferred work request now that its peer has answered. */
static void pending_complete(struct ionic_datapath *dp, struct dp_pending *p,
                             uint32_t status)
{
    if (p->qp_id < dp->qp_count && dp->qp[p->qp_id].valid) {
        struct ionic_qp_ring *q = &dp->qp[p->qp_id];

        if (p->use_msn)
            cq_post_send_msn(dp, q->sq_cq_id, p->qp_id, p->msn, status);
        else if (p->signalled)
            cq_post_send_npg(dp, q->sq_cq_id, p->qp_id, p->wqe_id);
    }
    p->valid = false;
}

static int dp_mesh_tx(struct ionic_datapath *dp, uint32_t dst_node,
                      const struct ionic_wire_hdr *hdr, const uint8_t *payload,
                      uint32_t payload_len)
{
    return ionic_mesh_sendv(dp->pvrdma_handle, dst_node, hdr, sizeof(*hdr),
                            payload, payload_len);
}

static void wire_hdr_init(struct ionic_wire_hdr *h, uint8_t op,
                          uint32_t src_qp_id, uint32_t dst_qp_id,
                          uint32_t req_id)
{
    memset(h, 0, sizeof(*h));
    h->magic = htole32(IONIC_WIRE_MAGIC);
    h->op = op;
    h->src_qp_id = htole32(src_qp_id);
    h->dst_qp_id = htole32(dst_qp_id);
    h->req_id = htole32(req_id);
}

/*
 * Transmit one WQE to the instance that owns the peer QP and park it until
 * the answer comes back.  Returns false when the request could not be sent,
 * leaving the caller to post an immediate error completion.
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
static bool remote_post(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                        uint32_t qp_id, uint8_t op, uint64_t wqe_id,
                        bool use_msn, bool signalled,
                        const struct dp_sge_list *src, const uint8_t *wqe,
                        uint32_t dest_qp_id, uint32_t dest_node_id,
                        const uint8_t *src_host)
{
    struct ionic_wire_hdr hdr;
    uint8_t *payload = NULL;
    uint32_t payload_len = 0;
    uint8_t wire_op;
    bool ok = false;

    struct dp_pending *p = pending_alloc(dp);
    if (!p) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u pending table full", qp_id);
        return false;
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
        return false;
    }

    wire_hdr_init(&hdr, wire_op, qp_id, dest_qp_id, p->req_id);

    if (wire_op == IONIC_WIRE_SEND || wire_op == IONIC_WIRE_SEND_IMM ||
        wire_op == IONIC_WIRE_SEND_INV) {
        uint32_t imm_be;
        memcpy(&imm_be, wqe + 12, 4);
        hdr.imm_be = imm_be;
        hdr.length = htole32(src->total);
        payload_len = src->total;
    } else if (wire_op == IONIC_WIRE_WRITE || wire_op == IONIC_WIRE_WRITE_IMM ||
               wire_op == IONIC_WIRE_READ_REQ) {
        uint32_t va_hi, va_lo, rkey, length, imm_be;
        memcpy(&va_hi, wqe + WQE_RDMA_VA_HI_OFF, 4);
        memcpy(&va_lo, wqe + WQE_RDMA_VA_LO_OFF, 4);
        memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
        memcpy(&length, wqe + WQE_SEND_LEN_OFF, 4);
        memcpy(&imm_be, wqe + 12, 4);

        length = be32toh(length);
        hdr.remote_va =
            htole64(((uint64_t)be32toh(va_hi) << 32) | be32toh(va_lo));
        hdr.rkey = htole32(be32toh(rkey));
        hdr.length = htole32(length);
        hdr.imm_be = imm_be;

        if (wire_op == IONIC_WIRE_READ_REQ) {
            /* The response scatters into these; keep them for later. */
            p->local = *src;
            p->length = length;
        } else {
            payload_len = length;
        }
    } else { /* ATOMIC_REQ */
        uint32_t rkey;
        memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);

        hdr.atomic_cs = op == IONIC_V1_OP_ATOMIC_CS;
        hdr.rkey = htole32(be32toh(rkey));
        hdr.remote_va = htole64(wqe_be64_pair(wqe, WQE_RDMA_VA_HI_OFF));
        hdr.swap_add = htole64(wqe_be64_pair(wqe, WQE_ATOMIC_SWAP_ADD_OFF));
        hdr.compare = htole64(wqe_be64_pair(wqe, WQE_ATOMIC_COMPARE_OFF));
        hdr.length = htole32(8);

        /* The original value comes back into the WQE's single local SGE. */
        uint64_t local_va;
        uint32_t local_lkey;
        memcpy(&local_va, wqe + WQE_ATOMIC_SGE_OFF, 8);
        memcpy(&local_lkey, wqe + WQE_ATOMIC_SGE_OFF + 12, 4);
        p->local.count = 1;
        p->local.va[0] = be64toh(local_va);
        p->local.lkey[0] = be32toh(local_lkey);
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

    if (payload_len) {
        payload = malloc(payload_len);
        if (!payload)
            goto out;
        if (src_host) {
            memcpy(payload, src_host, payload_len);
        } else if (dp_gather(dp, src, payload, payload_len) != payload_len) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u could not gather %u bytes", qp_id,
                    payload_len);
            goto out;
        }
    }

    ok = dp_mesh_tx(dp, dest_node_id, &hdr, payload, payload_len) == 0;
    if (ok) {
        /* Claim the MSN only once the request is really on the wire, so a
         * failed send does not leave a hole the driver would wait on. */
        if (use_msn)
            p->msn = ++q->msn;

        /* A READ has moved nothing yet; it is counted when the response
         * scatters into p->local. */
        if (wire_op == IONIC_WIRE_WRITE || wire_op == IONIC_WIRE_WRITE_IMM)
            ionic_dp_rdma_bytes(dp, qp_id, payload_len, PVRDMA_STAT_RDMA_WRITE,
                                DP_RDMA_TX);
        else if (wire_op != IONIC_WIRE_READ_REQ &&
                 wire_op != IONIC_WIRE_ATOMIC_REQ)
            ionic_dp_rdma_bytes(dp, qp_id, payload_len, PVRDMA_STAT_SEND,
                                DP_RDMA_TX);
    } else {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u send to node %u failed", qp_id,
                dest_node_id);
    }

out:
    free(payload);
    if (!ok)
        p->valid = false;
    return ok;
}

/* -------------------------------------------------------------------------
 * In-process NVMe-oF controller
 * -------------------------------------------------------------------------
 */

/*
 * The controller reaches guest memory the same way a peer's RDMA READ or
 * WRITE would: through the rkey the initiator put in the command's keyed
 * SGL.  Here that key resolves in the same MR table as any lkey, because one
 * guest owns every registration this emulator holds.
 */
static uint32_t dp_nvmeof_from_host(void *ctx, uint32_t key, uint64_t addr,
                                    void *dst, uint32_t len)
{
    return dp_sge_to_host(ctx, key, addr, dst, len);
}

static uint32_t dp_nvmeof_to_host(void *ctx, uint32_t key, uint64_t addr,
                                  const void *src, uint32_t len)
{
    return dp_host_to_sge(ctx, key, addr, src, len);
}

static const struct nvmeof_dma_ops dp_nvmeof_dma_ops = {
    .from_host = dp_nvmeof_from_host,
    .to_host = dp_nvmeof_to_host,
};

/*
 * The S3 target reaches guest memory through the rkey in the client's
 * x-amz-rdma-token, which resolves in the same MR table as any lkey for the
 * same reason the NVMe-oF controller's does.
 */
static uint32_t dp_s3_from_host(void *ctx, uint32_t key, uint64_t addr,
                                void *dst, uint32_t len)
{
    return dp_sge_to_host(ctx, key, addr, dst, len);
}

static uint32_t dp_s3_to_host(void *ctx, uint32_t key, uint64_t addr,
                              const void *src, uint32_t len)
{
    return dp_host_to_sge(ctx, key, addr, src, len);
}

static const struct s3_dma_ops dp_s3_dma_ops = {
    .from_host = dp_s3_from_host,
    .to_host = dp_s3_to_host,
};

/* The control plane's replies go back to the guest as received frames. */
static void dp_s3_tx_frame(void *ctx, const void *frame, size_t len)
{
    struct ionic_datapath *dp = ctx;

    ionic_eth_emu_queue_rx_frame(dp->eth_emu, frame, len);
}

static bool dp_s3_tx_filter(void *ctx, const void *frame, size_t len)
{
    struct ionic_datapath *dp = ctx;

    return s3_tcp_rx_frame(dp->s3_tcp, frame, len, dp_now_ms(dp));
}

bool ionic_datapath_attach_s3(struct ionic_datapath *dp,
                              const struct s3_target_cfg *cfg, char *err,
                              size_t errlen)
{
    if (!dp || !cfg)
        return false;

    if (!dp->eth_emu) {
        snprintf(err, errlen,
                 "the s3 backend needs the Ethernet emulator for its "
                 "control plane");
        return false;
    }

    dp->s3 = s3_target_create(cfg, err, errlen);
    if (!dp->s3)
        return false;

    struct s3_tcp_cfg ncfg;
    s3_tcp_cfg_from_target(&ncfg, dp->s3);
    dp->s3_tcp = s3_tcp_create(&ncfg, dp->s3, &dp_s3_dma_ops, dp,
                               dp_s3_tx_frame, dp, err, errlen);
    if (!dp->s3_tcp) {
        s3_target_destroy(dp->s3);
        dp->s3 = NULL;
        return false;
    }

    ionic_eth_emu_register_tx_filter(dp->eth_emu, dp_s3_tx_filter, dp);

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: s3 bucket '%s' %" PRIu64
            " bytes at http://%u.%u.%u.%u:%u/ mac "
            "%02x:%02x:%02x:%02x:%02x:%02x",
            cfg->bucket, cfg->capacity, cfg->traddr >> 24,
            (cfg->traddr >> 16) & 0xff, (cfg->traddr >> 8) & 0xff,
            cfg->traddr & 0xff, cfg->trsvcid, ncfg.mac[0], ncfg.mac[1],
            ncfg.mac[2], ncfg.mac[3], ncfg.mac[4], ncfg.mac[5]);
    return true;
}

/* Build the global route header a UD receive is expected to carry. */
static void dp_nvmeof_grh(struct ionic_datapath *dp, uint8_t grh[IB_GRH_SIZE],
                          uint32_t paylen)
{
    /* sgid is the controller, dgid the guest. */
    dp_grh_build(grh, paylen, dp->nvmeof_sgid, dp->nvmeof_dgid);
}

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
static void dp_ud_grh(struct ionic_datapath *dp, uint8_t grh[IB_GRH_SIZE],
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
    dp_grh_build(grh, paylen,
                 ah ? (ah->sgid_valid ? ah->sgid : ah->dgid) : NULL,
                 ah ? ah->dgid : NULL);
    (void)dp;
}

/*
 * Remember which GIDs the guest is using.  A CM REQ names both ends of the
 * path it wants, and it is the only MAD that does, so the GRH on every reply
 * after it is built from what the REQ said.
 */
static void dp_nvmeof_learn_gids(struct ionic_datapath *dp, const uint8_t *mad)
{
    uint16_t attr;
    memcpy(&attr, mad + 16, 2);
    if (be16toh(attr) != 0x0010) /* CM_ATTR_REQ */
        return;
    memcpy(dp->nvmeof_dgid, mad + 80, 16); /* primary local  = guest */
    memcpy(dp->nvmeof_sgid, mad + 96, 16); /* primary remote = us    */
}

/*
 * A CM MAD the guest put on its GSI QP.  Returns true once the responder has
 * claimed it, whether or not a reply went back, so the caller does not also
 * try to deliver it to a peer QP that does not exist.
 */
static bool dp_nvmeof_gsi(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                          uint32_t qp_id, const struct dp_sge_list *src)
{
    if (!dp->nvmeof_cm || src->total < IB_MAD_SIZE)
        return false;

    uint8_t mad[IB_MAD_SIZE];
    if (dp_gather(dp, src, mad, IB_MAD_SIZE) != IB_MAD_SIZE)
        return false;

    uint8_t rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    if (!nvmeof_cm_handle_mad(dp->nvmeof_cm, mad, IB_MAD_SIZE, rsp, &res))
        return false;

    dp_nvmeof_learn_gids(dp, mad);

    switch (res.action) {
    case NVMEOF_CM_ACT_BIND:
        vfu_log(dp->vfu_ctx, LOG_INFO,
                "ionic_datapath: nvmeof queue %u bound to QP %u (ctrl QP %u)",
                res.nvme_qid, res.guest_qpn, res.local_qpn);
        break;
    case NVMEOF_CM_ACT_UNBIND:
        vfu_log(dp->vfu_ctx, LOG_INFO, "ionic_datapath: nvmeof released QP %u",
                res.guest_qpn);
        break;
    case NVMEOF_CM_ACT_NONE:
    case NVMEOF_CM_ACT_ESTABLISHED:
    default:
        break;
    }

    if (!res.rsp_len)
        return true;

    uint8_t buf[IB_GRH_SIZE + IB_MAD_SIZE];
    dp_nvmeof_grh(dp, buf, (uint32_t)res.rsp_len);
    memcpy(buf + IB_GRH_SIZE, rsp, res.rsp_len);

    struct dp_sge_list in = {.count = 0,
                             .total = (uint32_t)(IB_GRH_SIZE + res.rsp_len)};
    if (deliver_recv(dp, q, qp_id, 1 /* the peer's GSI QP is also QP1 */, &in,
                     buf, CQE_RECV_OP_SEND, 0) < 0)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: nvmeof CM reply dropped, no GSI receive");
    return true;
}

/*
 * An NVMe command capsule on a QP the CM handshake bound to a controller
 * queue.  The controller executes it synchronously -- pulling or pushing data
 * through the command's own keyed SGL -- and the response capsule goes back
 * as a SEND into the initiator's next posted receive.
 */
static bool dp_nvmeof_rc_send(struct ionic_datapath *dp,
                              struct ionic_qp_ring *q, uint32_t qp_id,
                              const struct dp_sge_list *src)
{
    if (!dp->nvmeof || !q->dest_valid ||
        !nvmeof_cm_is_target_qpn(q->dest_qp_id))
        return false;

    struct nvmeof_queue *nq = nvmeof_target_find_queue(dp->nvmeof, qp_id);
    if (!nq) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u has no nvmeof queue", qp_id);
        return true;
    }

    /* Only the admin Connect ever needs room past the SQE, and only if the
     * host ignores the ioccsz we advertise and sends its data in capsule. */
    uint8_t capsule[NVME_SQE_SIZE + 1024];
    uint32_t len =
        src->total < sizeof(capsule) ? src->total : (uint32_t)sizeof(capsule);
    if (dp_gather(dp, src, capsule, len) != len) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u capsule fetch failed", qp_id);
        return true;
    }

    uint8_t rsp[NVME_CQE_SIZE];
    int r = nvmeof_queue_exec(nq, capsule, len, &dp_nvmeof_dma_ops, dp, rsp);
    if (r == NVMEOF_EXEC_HELD)
        return true; /* an AER: answered only when an event occurs */
    if (r < 0) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u bad capsule (%u bytes)", qp_id, len);
        return true;
    }

    struct dp_sge_list in = {.count = 0, .total = NVME_CQE_SIZE};
    if (deliver_recv(dp, q, qp_id, q->dest_qp_id, &in, rsp, CQE_RECV_OP_SEND,
                     0) < 0)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u has no receive for a response capsule",
                qp_id);
    return true;
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
    if (dq->qkey_valid && sent_qkey != dq->qkey)
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
        cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id);
    }
}

#ifdef ERNIC_HAVE_P4
void ionic_datapath_set_p4_engine(struct ionic_datapath *dp,
                                  struct p4_engine *eng)
{
    if (dp)
        dp->p4_engine = eng;
}

/*
 * The work request did not ask for a compute operation at all.
 *
 * Distinct from a malformed one, and the distinction matters: a program's
 * table is keyed on things like the opcode, which cannot tell a reduction
 * apart from an ordinary RDMA WRITE, so the action fires for both.  A
 * request that is not the two-equal-operand shape has not asked for
 * arithmetic, and failing it would take ordinary RDMA WRITE away from
 * every QP on a device whose guest installed a reduce entry.
 *
 * ENOEXEC is used because nothing else in the engine or the datapath
 * returns it, so it cannot be confused with a real failure from the
 * pipeline.
 */
#define DP_P4_NOT_COMPUTE (-ENOEXEC)

/*
 * What an extern called from the pipeline is given.
 *
 * Everything an operation may touch is in here, and the pipeline cannot
 * see any of it: the program chooses the verb and the architecture
 * supplies the operands.  @ran records that an extern did work, which the
 * caller needs in order to refuse an operation that would then also
 * transfer over the result.
 */
struct dp_p4_ctx {
    struct ionic_datapath *dp;
    const struct dp_sge_list *src;
    const uint8_t *wqe;
    uint32_t read_sz;
    uint32_t qp_id;
    uint8_t op;
    bool ran;
};

/*
 * The operand convention for a compute operation.
 *
 * An RDMA WRITE whose gather list has exactly two equal-length entries:
 * those are the two operands, the WQE's rkey and virtual address are the
 * destination, and the WQE's length is how long each of the three is.
 * That is the three-region shape a collective wants, expressed entirely in
 * things the guest had to register and post for itself.
 *
 * It is fixed here rather than carried in the P4 program on purpose.  A
 * table entry that could name a key and an address would be an addressing
 * authority over guest memory; what a program may say is which arithmetic
 * to apply, and @sel is the whole of it.
 */
static int dp_p4_fp8_map(struct dp_p4_ctx *c, uint8_t sel)
{
    /*
     * Neither of these is an error: they say the request is an ordinary
     * transfer, not a compute request.  See DP_P4_NOT_COMPUTE.
     */
    if (c->op != IONIC_V1_OP_RDMA_WRITE && c->op != IONIC_V1_OP_RDMA_WRITE_IMM)
        return DP_P4_NOT_COMPUTE;
    if (c->src->count != 2 || c->src->len[0] != c->src->len[1])
        return DP_P4_NOT_COMPUTE;

    if (c->read_sz < WQE_SEND_LEN_OFF + 4u)
        return -EINVAL;

    uint32_t va_hi, va_lo, rkey, length;
    memcpy(&va_hi, c->wqe + WQE_RDMA_VA_HI_OFF, 4);
    memcpy(&va_lo, c->wqe + WQE_RDMA_VA_LO_OFF, 4);
    memcpy(&rkey, c->wqe + WQE_RDMA_RKEY_OFF, 4);
    memcpy(&length, c->wqe + WQE_SEND_LEN_OFF, 4);

    uint64_t remote_va = ((uint64_t)be32toh(va_hi) << 32) | be32toh(va_lo);

    /*
     * The work request's length is the whole gather list, because that
     * is what a driver puts there: ionic_prep_common() sums the scatter
     * list into wqe->common.length, so a two-operand request says 2N and
     * cannot be made to say anything else from a verbs caller.  The
     * result is elementwise, so the destination is N -- half of it.
     *
     * Checking the sum rather than the per-operand length is what makes
     * this reachable at all.  An earlier version demanded the WQE say N,
     * which the in-process harness could do, because it sets the field
     * directly, and which no real guest can.
     */
    if (be32toh(length) != c->src->total)
        return -EINVAL;

    return ionic_datapath_fp8_map(c->dp, sel, c->src->lkey[0], c->src->va[0],
                                  c->src->lkey[1], c->src->va[1],
                                  c->src->len[0], be32toh(rkey), remote_va);
}

/*
 * The operand convention for a conversion.
 *
 * An RDMA WRITE whose gather list has exactly ONE entry: that is the
 * source, and the WQE's rkey and virtual address are the destination.
 * One entry rather than two is what tells a conversion from a reduction,
 * and it is the shape a driver produces for an ordinary one-buffer write,
 * so a program that selects a conversion gets it from a work request the
 * guest could already post.
 *
 * The destination may be longer than the source, shorter, or the same --
 * the program names both formats and either may be the wider.  That is
 * the one place the architecture's "three regions of length N" does not
 * hold, so the length is not checked against the destination here;
 * ionic_datapath_fmt_convert() checks each region against its own.
 */
static int dp_p4_fmt_convert(struct dp_p4_ctx *c, uint8_t src_fmt,
                             uint8_t dst_fmt)
{
    /* As in dp_p4_fp8_map(): neither of these is an error, they say the
     * request is an ordinary transfer rather than a conversion. */
    if (c->op != IONIC_V1_OP_RDMA_WRITE && c->op != IONIC_V1_OP_RDMA_WRITE_IMM)
        return DP_P4_NOT_COMPUTE;
    if (c->src->count != 1)
        return DP_P4_NOT_COMPUTE;

    if (c->read_sz < WQE_SEND_LEN_OFF + 4u)
        return -EINVAL;

    uint32_t va_hi, va_lo, rkey, length;
    memcpy(&va_hi, c->wqe + WQE_RDMA_VA_HI_OFF, 4);
    memcpy(&va_lo, c->wqe + WQE_RDMA_VA_LO_OFF, 4);
    memcpy(&rkey, c->wqe + WQE_RDMA_RKEY_OFF, 4);
    memcpy(&length, c->wqe + WQE_SEND_LEN_OFF, 4);

    uint64_t remote_va = ((uint64_t)be32toh(va_hi) << 32) | be32toh(va_lo);

    /* The driver sums the gather list into the length field; with one
     * entry that is the source length, and it has to agree. */
    if (be32toh(length) != c->src->total)
        return -EINVAL;

    return ionic_datapath_fmt_convert(c->dp, src_fmt, dst_fmt, c->src->lkey[0],
                                      c->src->va[0], c->src->len[0],
                                      be32toh(rkey), remote_va);
}

static int dp_p4_extern_call(void *ctx, uint8_t type, uint8_t method,
                             const struct p4_value *args, uint8_t nargs)
{
    struct dp_p4_ctx *c = ctx;

    if (type != P4_EXTERN_FP8)
        return -ENOSYS;
    /* map(op) names one arithmetic; cvt(src, dst) names two formats. */
    if (method == P4_EXTERN_FP8_MAP) {
        if (nargs != 1)
            return -ENOSYS;
    } else if (method == P4_EXTERN_FP8_CVT) {
        if (nargs != 2)
            return -ENOSYS;
    } else {
        return -ENOSYS;
    }

    /*
     * Each selector is one byte of a value that is wide enough for any
     * field the loader accepts.  A program that declared one wider and
     * put something in the top is not selecting an operation we have, so
     * it is refused rather than truncated.  Every argument is checked,
     * not just the first: cvt() has two.
     */
    for (uint8_t a = 0; a < nargs; a++)
        for (size_t i = 0; i + 1 < P4_VALUE_BYTES; i++)
            if (args[a].b[i])
                return -EINVAL;

    int rc;

    if (method == P4_EXTERN_FP8_MAP)
        rc = dp_p4_fp8_map(c, args[0].b[P4_VALUE_BYTES - 1]);
    else
        rc = dp_p4_fmt_convert(c, args[0].b[P4_VALUE_BYTES - 1],
                               args[1].b[P4_VALUE_BYTES - 1]);
    if (rc == 0)
        c->ran = true;
    return rc;
}

/*
 * Run the pipeline over one send WQE.
 *
 * Returns the completion status, and sets *@done when the ordinary
 * dispatch must not run: either because the program suppressed the
 * operation, or because the status is an error.
 */
static uint32_t dp_p4_policy(struct ionic_datapath *dp, uint32_t qp_id,
                             uint8_t op, const uint8_t *wqe, uint32_t read_sz,
                             const struct dp_sge_list *src, uint32_t imm_be,
                             bool *done)
{
    struct dp_p4_ctx ctx = {
        .dp = dp,
        .src = src,
        .wqe = wqe,
        .read_sz = read_sz,
        .qp_id = qp_id,
        .op = op,
        .ran = false,
    };
    const struct p4_extern_ops ops = {
        .call = dp_p4_extern_call,
        .ctx = &ctx,
    };
    struct p4_rdma_meta meta;
    bool suppress = false;

    /* Goes nowhere near a wire, but it is the pipeline's whole view of the
     * operation, so no field of it is left to whatever was on the stack. */
    memset(&meta, 0, sizeof(meta));
    meta.opcode = op;
    meta.qp_num = qp_id;
    meta.length = src->total;
    meta.imm_data = be32toh(imm_be);
    if ((op == IONIC_V1_OP_RDMA_WRITE || op == IONIC_V1_OP_RDMA_WRITE_IMM ||
         op == IONIC_V1_OP_RDMA_READ) &&
        read_sz >= WQE_RDMA_RKEY_OFF + 4u) {
        uint32_t rkey;
        memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
        meta.rkey = be32toh(rkey);
    }

    int rc = p4_engine_process_rdma(dp->p4_engine, &ops, &meta, &suppress);

    /*
     * No program, or one that was never meant for this path, is not a
     * refusal: the device behaves as it does with no pipeline at all.
     * Every other error is, because the pipeline is the policy and
     * carrying on past a failed one is exactly bypassing it.
     */
    if (rc == -ENOENT)
        return IONIC_STS_OK;
    /*
     * The program selected an arithmetic for a request that is not a
     * compute request -- an ordinary RDMA WRITE, most of the time, because
     * a table keyed on the opcode cannot tell the two apart.  The extern
     * wrote nothing and the pipeline stopped at the call, so nothing has
     * been decided about this operation and nothing was suppressed: let it
     * dispatch as it would with no program at all.
     *
     * Failing here instead is what made a guest that installed a reduce
     * entry unable to do an ordinary RDMA WRITE on any QP of the device.
     */
    if (rc == DP_P4_NOT_COMPUTE)
        return IONIC_STS_OK;
    if (rc < 0) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u op %u P4 pipeline failed (%d)", qp_id,
                op, rc);
        *done = true;
        return IONIC_STS_LOCAL_QP_OPER_ERR;
    }

    /*
     * An extern that wrote the destination and an operation that then also
     * transfers would both write the same region, and the ordinary
     * transfer would land second: the guest would get its raw payload
     * where it asked for a result, with nothing to say so.  A program
     * whose action calls the extern has to mark_to_drop() as well, and one
     * that forgets is refused rather than quietly producing that.
     *
     * The extern has already run by this point, so the destination holds
     * the result: what the error prevents is the transfer landing on top
     * of it, and what it tells the guest is not to trust the region.
     */
    if (ctx.ran && !suppress) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u op %u P4 program called an extern but "
                "did not suppress the operation; the transfer would overwrite "
                "the result",
                qp_id, op);
        *done = true;
        return IONIC_STS_LOCAL_QP_OPER_ERR;
    }

    if (suppress)
        *done = true;
    return IONIC_STS_OK;
}
#endif /* ERNIC_HAVE_P4 */

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
        dp_dma_read(dp->vfu_ctx, gpa, wqe, read_sz) < 0)
        return true;

    uint64_t wqe_id;
    memcpy(&wqe_id, wqe + 0, 8);

    uint8_t op = wqe[8];
    if (!q->sq_rnr) /* a retry is the same WQE again */
        pvrdma_qp_wqe_count(dp->pvrdma_handle, qp_id,
                            ionic_op_to_pvrdma_wr(op));

    uint16_t flags;
    memcpy(&flags, wqe + 10, 2);
    flags = be16toh(flags);

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
        if (op != IONIC_V1_OP_REG_MR)
            ionic_datapath_unregister_mr(dp, be32toh(imm_be));
        else if (read_sz >= WQE_REG_MR_MIN_SZ)
            dp_reg_mr(dp, wqe, be32toh(imm_be));
        else
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u REG_MR WQE truncated (%u bytes)",
                    qp_id, read_sz);
        cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id);
        return true;
    }

    struct dp_sge_list src;
    if (flags & IONIC_V1_FLAG_INL) {
        /* Inline data lives in the payload area itself; describe it as a
         * single lkey-0 SGE pointing at the WQE's own guest pages. */
        uint32_t inl_len;
        memcpy(&inl_len, wqe + WQE_SEND_LEN_OFF, 4);
        inl_len = be32toh(inl_len);
        src.count = 1;
        src.va[0] = gpa + WQE_PLD_OFF;
        src.len[0] = inl_len;
        src.lkey[0] = 0;
        src.total = inl_len;
    } else {
        /* read_sz, not stride, for the reason given in deliver_recv(). */
        parse_sges(wqe, read_sz, wqe[9], &src);
    }

    bool remote = !qp_is_datagram(q);
    uint32_t status = IONIC_STS_OK;

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
        uint32_t p4_status =
            dp_p4_policy(dp, qp_id, op, wqe, read_sz, &src, imm_be, &done);
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
        if (remote_post(dp, q, qp_id, op, wqe_id, remote,
                        (flags & IONIC_V1_FLAG_SIG) != 0, &src, wqe,
                        q->dest_qp_id, q->dest_node_id, NULL))
            return true;

        if (remote) {
            q->msn++;
            cq_post_send_msn(dp, q->sq_cq_id, qp_id, q->msn,
                             IONIC_STS_REMOTE_ACC_ERR);
        } else if (flags & IONIC_V1_FLAG_SIG) {
            cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id);
        }
        return true;
    }

    dp_fault_clear(dp);

    switch (op) {
    case IONIC_V1_OP_SEND:
    case IONIC_V1_OP_SEND_INV:
    case IONIC_V1_OP_SEND_IMM: {
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
            dp_nvmeof_gsi(dp, q, qp_id, &src))
            break;
        if (remote && dp_nvmeof_rc_send(dp, q, qp_id, &src))
            break;

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
            uint32_t ah_id_be, dest_qpn_be;
            memcpy(&ah_id_be, wqe + WQE_SEND_AH_OFF, 4);
            memcpy(&dest_qpn_be, wqe + WQE_SEND_DEST_QPN_OFF, 4);
            dst_id = be32toh(dest_qpn_be);

            uint32_t ah_id = be32toh(ah_id_be);
            ah = ah_id < dp->ah_count && dp->ah[ah_id].valid ? &dp->ah[ah_id]
                                                             : NULL;
            if (!ah && ah_id) {
                /* Not fatal -- a local peer needs no address -- but it
                 * means CREATE_AH never reached us for this handle. */
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u SEND names unknown AH %u", qp_id,
                        ah_id);
            }
            if (ah && dp->local_node != UINT32_MAX &&
                ah->node_id != UINT32_MAX && ah->node_id != dp->local_node) {
                /*
                 * The datagram is for another instance. The GRH goes on
                 * here, before it leaves, so the receiving side can deliver
                 * what arrives unchanged -- the sender is the only one that
                 * knows which handle was used.
                 */
                uint8_t gbuf[IB_GRH_SIZE + 8192];
                uint32_t plen = src.total;
                if (plen > sizeof(gbuf) - IB_GRH_SIZE)
                    plen = (uint32_t)(sizeof(gbuf) - IB_GRH_SIZE);
                if (dp_gather(dp, &src, gbuf + IB_GRH_SIZE, plen) != plen) {
                    vfu_log(dp->vfu_ctx, LOG_WARNING,
                            "ionic_datapath: QP %u UD payload fetch failed",
                            qp_id);
                    break;
                }
                dp_ud_grh(dp, gbuf, plen, ah);

                struct dp_sge_list gsrc = {.count = 0,
                                           .total = IB_GRH_SIZE + plen};
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
                if (!remote_post(dp, q, qp_id, op, wqe_id, false, false, &gsrc,
                                 wqe, dst_id, ah->node_id, gbuf)) {
                    vfu_log(dp->vfu_ctx, LOG_WARNING,
                            "ionic_datapath: QP %u UD send to node %u failed",
                            qp_id, ah->node_id);
                }
                break;
            }
        } else {
            dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
        }

        struct ionic_qp_ring *dq = dst_id < dp->qp_count && dp->qp[dst_id].valid
                                       ? &dp->qp[dst_id]
                                       : NULL;
        if (!dq) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u SEND to unknown peer %u", qp_id,
                    dst_id);
            break;
        }
        /*
         * Only a datagram send is checked. This sits after the branch above
         * because that is where the destination is resolved, but the rules
         * are a datagram's: a connected send's destination is its one
         * configured peer, it carries no Q_Key, and holding it to a
         * datagram's rules refuses every RC and UC SEND in the device.
         */
        {
            uint32_t sent_qkey_be;
            memcpy(&sent_qkey_be, wqe + WQE_SEND_QKEY_OFF, 4);

            const char *refused =
                ionic_dp_ud_delivery_refused(q, dq, be32toh(sent_qkey_be));
            if (refused) {
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u datagram to QP %u refused: %s",
                        qp_id, dst_id, refused);
                break;
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
         * Built here rather than in deliver_recv() because the GSI path
         * above already supplies its own GRH as payload, and a second one
         * would offset it twice.
         */
        uint8_t udbuf[IB_GRH_SIZE + 8192];
        struct dp_sge_list udsrc;
        const uint8_t *src_host = NULL;
        if (qp_is_datagram(dq)) {
            uint32_t plen = src.total;
            if (plen > sizeof(udbuf) - IB_GRH_SIZE)
                plen = (uint32_t)(sizeof(udbuf) - IB_GRH_SIZE);
            if (dp_gather(dp, &src, udbuf + IB_GRH_SIZE, plen) != plen) {
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u UD payload fetch failed", qp_id);
                break;
            }
            dp_ud_grh(dp, udbuf, plen, ah);
            src_host = udbuf;
            udsrc.count = 0;
            udsrc.total = IB_GRH_SIZE + plen;
        }

        if (deliver_recv(dp, dq, dst_id, qp_id, src_host ? &udsrc : &src,
                         src_host, recv_op, imm_be) < 0) {
            /*
             * RNR.  A reliable requester retries rather than lose the
             * message, so an RC send queue waits here for the peer to post
             * a receive, for as long as the wire path waits.  UC, UD and
             * GSI have no retry: the responder drops the message and the
             * send completes.
             */
            uint64_t now = dp_now_ms(dp);

            if (q->ib_qp_type != 2 /* RC */) {
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: QP %u has no posted receive, "
                        "dropping %u bytes from QP %u",
                        dst_id, src.total, qp_id);
                dp_count(dp, dst_id, IONIC_DP_RESP_RX_OUT_OF_BUFFER);
                break;
            }
            if (!q->sq_rnr) {
                q->sq_rnr = true;
                q->sq_rnr_since_ms = now;
                dp->sq_stalled++;
                dp_count(dp, dst_id, IONIC_DP_RESP_RX_OUT_OF_BUFFER);
            }
            if (now - q->sq_rnr_since_ms < RNR_RETRY_MS) {
                dp_count(dp, dst_id, IONIC_DP_RESP_TX_RNR_RETRY_ERR);
                return false;
            }
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u posted no receive within %u ms, "
                    "dropping %u bytes from QP %u",
                    dst_id, RNR_RETRY_MS, src.total, qp_id);
            status = IONIC_STS_RNR_RETRY_EXCEEDED;
        } else
            ionic_dp_rdma_bytes(dp, qp_id, src.total, PVRDMA_STAT_SEND,
                                DP_RDMA_TX);
        break;
    }

    case IONIC_V1_OP_RDMA_READ:
    case IONIC_V1_OP_RDMA_WRITE:
    case IONIC_V1_OP_RDMA_WRITE_IMM: {
        uint32_t va_hi, va_lo, rkey, length;
        memcpy(&va_hi, wqe + WQE_RDMA_VA_HI_OFF, 4);
        memcpy(&va_lo, wqe + WQE_RDMA_VA_LO_OFF, 4);
        memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
        memcpy(&length, wqe + WQE_SEND_LEN_OFF, 4);

        uint64_t remote_va = ((uint64_t)be32toh(va_hi) << 32) | be32toh(va_lo);
        rkey = be32toh(rkey);
        length = be32toh(length);

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
        if (length != 0 && !mr_covers(dp, rkey, remote_va, length)) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u RDMA %s rkey=%#x va=%#" PRIx64
                    " len=%u is outside the region",
                    qp_id, to_remote ? "write" : "read", rkey, remote_va,
                    length);
            status = IONIC_STS_REMOTE_ACC_ERR;
            break;
        }

        uint32_t moved =
            rdma_xfer(dp, &src, rkey, remote_va, length, to_remote);
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
            status = mr_covers(dp, rkey, remote_va, length)
                         ? dp_fault_status(dp, IONIC_STS_LOCAL_PROT_ERR)
                         : IONIC_STS_REMOTE_ACC_ERR;
            break;
        }

        ionic_dp_rdma_bytes(dp, qp_id, moved,
                            to_remote ? PVRDMA_STAT_RDMA_WRITE
                                      : PVRDMA_STAT_RDMA_READ,
                            DP_RDMA_TX | DP_RDMA_RX);

        /* Only the _IMM form consumes a receive on the far side, and it does
         * so with no payload: the data already landed via the rkey.  The
         * length still rides along, because the responder's completion
         * reports how many bytes the write moved. */
        if (op == IONIC_V1_OP_RDMA_WRITE_IMM) {
            uint32_t dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
            struct ionic_qp_ring *dq =
                dst_id < dp->qp_count && dp->qp[dst_id].valid ? &dp->qp[dst_id]
                                                              : NULL;
            struct dp_sge_list written = {.count = 0, .total = moved};
            if (dq)
                deliver_recv(dp, dq, dst_id, qp_id, &written, NULL,
                             CQE_RECV_OP_RDMA_IMM, imm_be);
        }
        break;
    }

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
 * SEND that is waiting for a receive.
 */
static void sq_service(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                       uint32_t qp_id)
{
    for (uint32_t n = 0;
         q->sq_cons % q->sq_depth != q->sq_prod && n < q->sq_depth; n++) {
        if (!process_sq_wqe(dp, q, qp_id, q->sq_cons % q->sq_depth))
            return;
        sq_rnr_clear(dp, q);
        q->sq_cons++;
    }
}

/* -------------------------------------------------------------------------
 * Cross-instance receive
 *
 * Messages arrive on a backend receive thread, which must not touch guest
 * memory: every DMA belongs to the thread that owns the vfio-user context.
 * So dp_mesh_recv() only copies the bytes onto a queue, and the server's main
 * loop drains it here through ionic_datapath_poll().
 * -------------------------------------------------------------------------
 */

static void dp_mesh_recv(void *opaque, uint32_t src_node, const void *buf,
                         size_t len)
{
    struct ionic_datapath *dp = opaque;

    if (len < sizeof(struct ionic_wire_hdr) ||
        len > sizeof(struct ionic_wire_hdr) + IONIC_WIRE_MAX_PAYLOAD)
        return;

    struct dp_inmsg *m = malloc(sizeof(*m) + len);
    if (!m)
        return;

    m->next = NULL;
    m->src_node = src_node;
    m->len = len;
    m->first_try_ms = 0;
    m->rnr_counted = false;
    memcpy(m->buf, buf, len);

    pthread_mutex_lock(&dp->rx_lock);
    if (dp->rx_tail)
        dp->rx_tail->next = m;
    else
        dp->rx_head = m;
    dp->rx_tail = m;
    dp->rx_fresh++;
    pthread_mutex_unlock(&dp->rx_lock);
}

static void dp_wire_reply(struct ionic_datapath *dp, uint32_t dst_node,
                          uint8_t op, const struct ionic_wire_hdr *req,
                          uint32_t status, const uint8_t *payload,
                          uint32_t payload_len)
{
    struct ionic_wire_hdr r;

    wire_hdr_init(&r, op, le32toh(req->dst_qp_id), le32toh(req->src_qp_id),
                  le32toh(req->req_id));
    r.status = htole32(status);
    r.length = htole32(payload_len);
    dp_mesh_tx(dp, dst_node, &r, payload, payload_len);

    if (status == IONIC_STS_REMOTE_ACC_ERR)
        dp_count(dp, le32toh(req->dst_qp_id),
                 IONIC_DP_RESP_TX_REMOTE_ACCESS_ERR);
}

/* Apply a peer's atomic to a local MR, returning the pre-operation value. */
static bool dp_remote_atomic(struct ionic_datapath *dp, uint32_t rkey,
                             uint64_t va, bool compare_swap, uint64_t swap_add,
                             uint64_t compare, uint64_t *old_out)
{
    uint64_t run;
    uint64_t gpa = sge_gpa(dp, rkey, va, &run);
    uint64_t old, new_val;

    if (run < 8 || dp_dma_read(dp->vfu_ctx, gpa, &old, 8) < 0)
        return false;

    if (compare_swap)
        new_val = old == compare ? swap_add : old;
    else
        new_val = old + swap_add;

    if (dp_dma_write(dp->vfu_ctx, gpa, &new_val, 8) < 0)
        return false;

    *old_out = old;
    return true;
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
    const struct ionic_wire_hdr *h = (const struct ionic_wire_hdr *)msg;
    const uint8_t *payload = msg + sizeof(*h);
    uint32_t payload_len = (uint32_t)(len - sizeof(*h));

    if (le32toh(h->magic) != IONIC_WIRE_MAGIC) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: dropping mesh message with bad magic");
        return true;
    }

    uint32_t dst_qp_id = le32toh(h->dst_qp_id);
    uint32_t src_qp_id = le32toh(h->src_qp_id);
    uint32_t length = le32toh(h->length);
    uint32_t rkey = le32toh(h->rkey);
    uint64_t remote_va = le64toh(h->remote_va);
    uint32_t status = IONIC_STS_OK;

    struct ionic_qp_ring *dq =
        dst_qp_id < dp->qp_count && dp->qp[dst_qp_id].valid ? &dp->qp[dst_qp_id]
                                                            : NULL;

    /* A plain RDMA op names only an rkey, so the QP it quotes may not exist
     * here; attribute those bytes to the device alone. */
    uint32_t stat_qp = dq ? dst_qp_id : PVRDMA_STAT_NO_QP;

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
     * -- dp_host_to_sge() with len 0 touches no memory and returns 0 before
     * it ever calls sge_gpa().
     *
     * Atomics are not exempt: remote_post() always gives them length 8, so
     * they still require a key.
     */
    if (!rkey && length &&
        (h->op == IONIC_WIRE_WRITE || h->op == IONIC_WIRE_WRITE_IMM ||
         h->op == IONIC_WIRE_READ_REQ || h->op == IONIC_WIRE_ATOMIC_REQ)) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: node %u sent op %u with rkey 0, rejecting",
                src_node, h->op);
        dp_wire_reply(dp, src_node,
                      h->op == IONIC_WIRE_READ_REQ     ? IONIC_WIRE_READ_RESP
                      : h->op == IONIC_WIRE_ATOMIC_REQ ? IONIC_WIRE_ATOMIC_RESP
                                                       : IONIC_WIRE_ACK,
                      h, IONIC_STS_REMOTE_ACC_ERR, NULL, 0);
        return true;
    }

    switch (h->op) {
    case IONIC_WIRE_SEND:
    case IONIC_WIRE_SEND_IMM:
    case IONIC_WIRE_SEND_INV: {
        uint8_t recv_op = h->op == IONIC_WIRE_SEND_IMM   ? CQE_RECV_OP_SEND_IMM
                          : h->op == IONIC_WIRE_SEND_INV ? CQE_RECV_OP_SEND_INV
                                                         : CQE_RECV_OP_SEND;
        struct dp_sge_list src = {.count = 0, .total = length};

        if (length > payload_len)
            src.total = payload_len;

        if (!dq) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: SEND from node %u to unknown QP %u",
                    src_node, dst_qp_id);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else if (deliver_recv(dp, dq, dst_qp_id, src_qp_id, &src, payload,
                                recv_op, h->imm_be) < 0) {
            /* RNR: nothing has been consumed, so the message can simply wait
             * for the guest to post a receive. */
            if (allow_retry)
                return false;

            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u posted no receive within %u ms, "
                    "dropping %u bytes from node %u",
                    dst_qp_id, RNR_RETRY_MS, src.total, src_node);
            status = IONIC_STS_RNR_RETRY_EXCEEDED;
        }
        dp_wire_reply(dp, src_node, IONIC_WIRE_ACK, h, status, NULL, 0);
        break;
    }

    case IONIC_WIRE_WRITE:
    case IONIC_WIRE_WRITE_IMM: {
        uint32_t n = length > payload_len ? payload_len : length;

        if (dp_host_to_sge(dp, rkey, remote_va, payload, n) != length) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote write rkey=%#x va=%#" PRIx64
                    " of %u bytes failed",
                    rkey, remote_va, length);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else {
            /* The responder counts what landed in its own memory too, so each
             * instance's totals describe the traffic it actually moved. */
            ionic_dp_rdma_bytes(dp, stat_qp, n, PVRDMA_STAT_RDMA_WRITE,
                                DP_RDMA_RX);

            if (h->op == IONIC_WIRE_WRITE_IMM && dq) {
                struct dp_sge_list written = {.count = 0, .total = n};
                deliver_recv(dp, dq, dst_qp_id, src_qp_id, &written, NULL,
                             CQE_RECV_OP_RDMA_IMM, h->imm_be);
            }
        }
        dp_wire_reply(dp, src_node, IONIC_WIRE_ACK, h, status, NULL, 0);
        break;
    }

    case IONIC_WIRE_READ_REQ: {
        uint8_t *data = NULL;

        if (length > IONIC_WIRE_MAX_PAYLOAD)
            status = IONIC_STS_REMOTE_ACC_ERR;
        else if (length && !(data = malloc(length)))
            status = IONIC_STS_REMOTE_ACC_ERR;
        else if (dp_sge_to_host(dp, rkey, remote_va, data, length) != length) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote read rkey=%#x va=%#" PRIx64
                    " of %u bytes failed",
                    rkey, remote_va, length);
            status = IONIC_STS_REMOTE_ACC_ERR;
        } else {
            ionic_dp_rdma_bytes(dp, stat_qp, length, PVRDMA_STAT_RDMA_READ,
                                DP_RDMA_TX);
        }

        dp_wire_reply(dp, src_node, IONIC_WIRE_READ_RESP, h, status,
                      status == IONIC_STS_OK ? data : NULL,
                      status == IONIC_STS_OK ? length : 0);
        free(data);
        break;
    }

    case IONIC_WIRE_ATOMIC_REQ: {
        uint64_t old = 0;

        if (!dp_remote_atomic(dp, rkey, remote_va, h->atomic_cs,
                              le64toh(h->swap_add), le64toh(h->compare),
                              &old)) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: remote atomic rkey=%#x va=%#" PRIx64
                    " failed",
                    rkey, remote_va);
            status = IONIC_STS_REMOTE_ACC_ERR;
        }
        dp_wire_reply(dp, src_node, IONIC_WIRE_ATOMIC_RESP, h, status,
                      (const uint8_t *)&old, status == IONIC_STS_OK ? 8 : 0);
        break;
    }

    case IONIC_WIRE_ACK:
    case IONIC_WIRE_READ_RESP:
    case IONIC_WIRE_ATOMIC_RESP: {
        struct dp_pending *p = pending_find(dp, le32toh(h->req_id));
        if (!p)
            break;

        status = le32toh(h->status);
        if (status == IONIC_STS_OK && h->op != IONIC_WIRE_ACK) {
            uint32_t want = p->length < payload_len ? p->length : payload_len;

            /* The peer answered; anything that goes wrong from here is this
             * guest's own memory refusing the landing. */
            dp_fault_clear(dp);
            uint32_t got = dp_scatter(dp, &p->local, payload, want);
            if (got != want)
                status = dp_fault_status(dp, IONIC_STS_LOCAL_PROT_ERR);

            /* The requester's READ bytes only exist once they have landed. */
            if (h->op == IONIC_WIRE_READ_RESP)
                ionic_dp_rdma_bytes(dp, p->qp_id, got, PVRDMA_STAT_RDMA_READ,
                                    DP_RDMA_RX);
        }
        pending_complete(dp, p, status);
        break;
    }

    default:
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: unknown mesh op %u from node %u", h->op,
                src_node);
        break;
    }

    return true;
}

/* Destination QP of a queued message, used to keep per-QP ordering. */
static uint32_t inmsg_dst_qp(const struct dp_inmsg *m)
{
    const struct ionic_wire_hdr *h = (const struct ionic_wire_hdr *)m->buf;

    return le32toh(h->dst_qp_id);
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
    const struct ionic_wire_hdr *h = (const struct ionic_wire_hdr *)m->buf;

    return h->op == IONIC_WIRE_ACK || h->op == IONIC_WIRE_READ_RESP ||
           h->op == IONIC_WIRE_ATOMIC_RESP;
}

bool ionic_datapath_has_work(struct ionic_datapath *dp)
{
    bool work;

    if (!dp)
        return false;

    pthread_mutex_lock(&dp->rx_lock);
    work = dp->rx_fresh != 0;
    pthread_mutex_unlock(&dp->rx_lock);

    if (!work && dp->s3_tcp)
        work = s3_tcp_has_work(dp->s3_tcp);
    return work;
}

void ionic_datapath_poll(struct ionic_datapath *dp)
{
    struct dp_inmsg *list;

    if (!dp)
        return;

    if (dp->s3_tcp)
        s3_tcp_poll(dp->s3_tcp, dp_now_ms(dp));

    pthread_mutex_lock(&dp->rx_lock);
    list = dp->rx_head;
    dp->rx_head = dp->rx_tail = NULL;
    dp->rx_fresh = 0;
    pthread_mutex_unlock(&dp->rx_lock);

    /*
     * Messages that cannot be delivered yet go on @defer and are put back for
     * the next poll.  Any further request for the same QP has to be deferred
     * with it, or a later SEND would overtake an earlier one; traffic for
     * other QPs, and every response, keeps flowing.
     */
    struct dp_inmsg *defer_head = NULL, *defer_tail = NULL;
    uint64_t now = dp_now_ms(dp);

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
                               now - m->first_try_ms < RNR_RETRY_MS)) {
                free(m);
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
    }

    if (defer_head) {
        pthread_mutex_lock(&dp->rx_lock);
        defer_tail->next = dp->rx_head;
        dp->rx_head = defer_head;
        if (!dp->rx_tail)
            dp->rx_tail = defer_tail;
        pthread_mutex_unlock(&dp->rx_lock);
    }

    /* Retry the send queues that are waiting for a local peer's receive. */
    for (uint32_t i = 0; dp->sq_stalled && i < dp->qp_count; i++)
        if (dp->qp[i].valid && dp->qp[i].sq_rnr)
            sq_service(dp, &dp->qp[i], i);

    /* A peer that never answers must not wedge the guest's send queue. */
    for (int i = 0; i < MAX_PENDING; i++) {
        struct dp_pending *p = &dp->pending[i];
        if (p->valid && now > p->deadline_ms) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u request %u timed out", p->qp_id,
                    p->req_id);
            pending_complete(dp, p, IONIC_STS_RETRY_EXCEEDED);
        }
    }
}

/* -------------------------------------------------------------------------
 * Doorbell handler
 *
 * Doorbell layout (struct ionic_doorbell, little-endian):
 *   le16 p_index   [0:1]
 *   u8   ring      [2]    — 0 = work, 1 = arm, 2 = arm solicited
 *   u8   qid_lo    [3]
 *   le16 qid_hi    [4:5]
 * -------------------------------------------------------------------------
 */

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
            struct ionic_srq_ring *sr = srq_by_qid(dp, qid);
            bool posted = false;

            /* Lock first, then read valid: sampling it outside the lock is
             * what let this race unregister_srq. srq_by_qid() only does the
             * bounds arithmetic, which is stable for the datapath's life. */
            if (sr) {
                pthread_mutex_lock(&sr->lock);
                if (sr->valid)
                    posted = rq_prod_advance(&sr->prod, sr->depth, p_index);
                pthread_mutex_unlock(&sr->lock);
            }
            if (!posted)
                vfu_log(dp->vfu_ctx, LOG_WARNING,
                        "ionic_datapath: doorbell for unregistered SRQ %u",
                        qid);
            return;
        }
        if (qid < dp->qp_count && dp->qp[qid].valid) {
            if (rq_prod_advance(&dp->qp[qid].rq_prod, dp->qp[qid].rq_depth,
                                p_index))
                pvrdma_qp_doorbell_count(dp->pvrdma_handle, qid, false);
        }
        return;

    case DP_QTYPE_SQ:
        break;

    default:
        return;
    }

    if (qid >= dp->qp_count || !dp->qp[qid].valid)
        return;

    pvrdma_qp_doorbell_count(dp->pvrdma_handle, qid, true);

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
