/*
 * Unit tests for the ionic emulator's RDMA error counters.
 *
 * The data path counts each RDMA error it hands the guest -- an error
 * completion, a NAK it sends as responder, a pass that finds no receive --
 * against the device and the QP involved, and the STATS and QP_STATS admin
 * commands serve those counters as hw_counters files.  Before them, the only
 * sign of a refused rkey or a receive-not-ready storm was a log line.
 *
 * The tests are one table.  Each row stands up a fresh data path over a fake
 * guest memory, runs one scenario, and compares all of the counters -- the
 * device's and each QP's -- against the row, so a counter that moves when it
 * should not fails just as one that stays still does.
 *
 * The cross-instance rows run both ends in the one data path: the requester's
 * peer is "node 1", and whatever the data path sends there is fed straight
 * back in, where it lands on the responder QP.  The clock is faked, so the
 * RNR and pending-timeout rows sit exactly on their boundaries.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_mesh.h"
#include "ernic_stats.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_datapath_priv.h"
#include "ionic/ionic_eth_emu.h"
#include "nvmeof/nvmeof_cm.h"
#include "nvmeof/nvmeof_target.h"
#include "qemu/thread.h"
#include "rocm_ernic_compat.h"
#include "s3/s3_tcp.h"
#include "s3/s3_target.h"

/*
 * The guest driver's side of the device ABI (enum ionic_v1_op, struct
 * ionic_v1_wqe and enum ionic_status in its ionic_fw.h), written out here
 * rather than taken from the data path, so a change on either side fails a
 * row instead of moving both at once.
 */
#define IONIC_V1_OP_SEND       0
#define IONIC_V1_OP_RDMA_READ  3
#define IONIC_V1_OP_RDMA_WRITE 4
#define IONIC_V1_OP_ATOMIC_CS  6
#define IONIC_V1_OP_ATOMIC_FA  7

#define WQE_RDMA_VA_HI_OFF      16
#define WQE_RDMA_VA_LO_OFF      20
#define WQE_RDMA_RKEY_OFF       24
#define WQE_SEND_LEN_OFF        28
#define WQE_ATOMIC_SWAP_ADD_OFF 28
#define WQE_PLD_OFF             32
#define WQE_ATOMIC_SGE_OFF      48

#define IONIC_STS_OK                0
#define IONIC_STS_LOCAL_QP_OPER_ERR 2
#define IONIC_STS_WQE_FLUSHED_ERR   4
#define IONIC_STS_REMOTE_ACC_ERR    9
#define IONIC_STS_RETRY_EXCEEDED    11

/* struct ionic_v1_cqe: its type and error bits, and where its fields lie */
#define CQE_ERROR_BIT     0x02u
#define CQE_TYPE_MASK     (7u << 5)
#define CQE_TYPE_RECV     (1u << 5)
#define CQE_TYPE_SEND_MSN (2u << 5)
#define CQE_TYPE_SEND_NPG (3u << 5)
#define CQE_MSN_OFF       4  /* be32 send.msg_msn */
#define CQE_NPG_WQE_OFF   16 /* native u64 send.npg_wqe_id */
#define CQE_STATUS_OFF    24 /* be32 status_length */
#define CQE_QTF_OFF       28 /* be32 qid, type and flags */

/* The data path's clock, which the rows set so the RNR and pending-timeout
 * rows sit exactly on their deadlines. */
static uint64_t g_now_ms;

static uint64_t fake_now_ms(void)
{
    return g_now_ms;
}

/* ---- Fake guest memory --------------------------------------------------
 *
 * GPA n is g_mem[n]; anything past the end fails, as an unmapped address
 * would, and so does anything that takes in g_hole_gpa, unless it is
 * UINT64_MAX.
 */

#define GUEST_MEM_SIZE 0x10000u
static uint8_t g_mem[GUEST_MEM_SIZE];
static uint64_t g_hole_gpa = UINT64_MAX;

struct fake_sg {
    uint64_t addr;
    size_t len;
};

size_t dma_sg_size(void)
{
    return sizeof(struct fake_sg);
}

int vfu_addr_to_sgl(vfu_ctx_t *vfu_ctx, vfu_dma_addr_t dma_addr, size_t len,
                    dma_sg_t *sgl, size_t max_nr_sgs, int prot)
{
    struct fake_sg *sg = (struct fake_sg *)sgl;
    uint64_t addr = (uint64_t)(uintptr_t)dma_addr;

    (void)vfu_ctx;
    (void)prot;
    /* Written so a huge addr cannot wrap the sum back into range. */
    if (max_nr_sgs < 1 || addr > GUEST_MEM_SIZE || len > GUEST_MEM_SIZE - addr)
        return -1;
    if (g_hole_gpa >= addr && g_hole_gpa - addr < len)
        return -1;
    sg->addr = addr;
    sg->len = len;
    return 1;
}

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    const struct fake_sg *sg = (const struct fake_sg *)sgl;

    (void)vfu_ctx;
    (void)flags;
    if (cnt < 1)
        return -1;
    iov->iov_base = g_mem + sg->addr;
    iov->iov_len = sg->len;
    return 0;
}

void vfu_sgl_put(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                 size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)iov;
    (void)cnt;
}

void vfu_sgl_mark_dirty(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)cnt;
}

void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)level;
    (void)fmt;
}

/* ---- The mesh: everything sent is kept, to be fed back in -------------- */

#define WIRE_MAX   8
#define WIRE_BYTES 512

static uint8_t g_wire[WIRE_MAX][WIRE_BYTES];
static size_t g_wire_len[WIRE_MAX];
static uint64_t g_wire_tag[WIRE_MAX]; /* what each message is called */
static bool g_wire_reply[WIRE_MAX];
static size_t g_nwire;

/*
 * What the mesh says: whether ernic_mesh_has_room() finds room to queue a
 * request, and, unless it is 0, what ernic_mesh_sendv() returns instead of
 * taking the message.
 */
static bool g_room;
static int g_send_rc;

/*
 * Called by the next ernic_mesh_sendv(), once, if set: how a row acts in the
 * middle of a pass, as a mesh receive thread can.
 */
static void (*g_sendv_hook)(void);

int ernic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply)
{
    (void)handle;
    (void)dst_node;
    if (g_sendv_hook) {
        void (*hook)(void) = g_sendv_hook;

        g_sendv_hook = NULL;
        hook();
    }
    if (g_send_rc != 0)
        return g_send_rc;
    if (g_nwire >= WIRE_MAX || hdr_len + body_len > WIRE_BYTES)
        return -1;
    memcpy(g_wire[g_nwire], hdr, hdr_len);
    if (body_len)
        memcpy(g_wire[g_nwire] + hdr_len, body, body_len);
    g_wire_tag[g_nwire] = tag;
    g_wire_reply[g_nwire] = reply;
    g_wire_len[g_nwire++] = hdr_len + body_len;
    return 0;
}

/* ---- Stubs for the TU's other external symbols -------------------------
 *
 * None of these are reachable from the paths under test; they exist only to
 * satisfy the linker.
 */

int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    (void)emu;
    (void)vec;
    return 0;
}

/* ---- The mesh backend ------------------------------------------------
 *
 * remote() attaches the data path to it through ionic_datapath_set_pvrdma(),
 * as the server does, and it hands back its receive callback, which the rows
 * call to deliver the peer's traffic.
 */

/* Stands in for the device the bridge hands out; only its address is used. */
struct PVRDMADev {
    int unused;
};
static struct PVRDMADev g_mesh;
static uint32_t g_local_node = UINT32_MAX;
static ernic_mesh_recv_fn g_recv_fn;
static void *g_recv_opaque;

uint32_t ernic_mesh_local_node(pvrdma_handle_t handle)
{
    (void)handle;
    return g_local_node;
}
uint32_t ernic_mesh_node_from_gid(pvrdma_handle_t handle, const uint8_t *gid)
{
    (void)handle;
    (void)gid;
    return UINT32_MAX;
}
bool ernic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len)
{
    (void)handle;
    (void)dst_node;
    (void)len;
    return g_room;
}
void ernic_mesh_set_recv_cb(pvrdma_handle_t handle, ernic_mesh_recv_fn fn,
                            void *opaque)
{
    (void)handle;
    g_recv_fn = fn;
    g_recv_opaque = opaque;
}

/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
/* NOLINTEND(readability-non-const-parameter) */
void nvmeof_target_destroy(struct nvmeof_target *t)
{
    (void)t;
}
const struct nvmeof_target_cfg *nvmeof_target_config(
    const struct nvmeof_target *t)
{
    (void)t;
    return NULL;
}
struct nvmeof_queue *nvmeof_target_find_queue(struct nvmeof_target *t,
                                              uint32_t handle)
{
    (void)t;
    (void)handle;
    return NULL;
}
int nvmeof_queue_exec(struct nvmeof_queue *q, const void *capsule, size_t len,
                      const struct nvmeof_dma_ops *dma, void *dma_ctx,
                      void *rsp)
{
    (void)q;
    (void)capsule;
    (void)len;
    (void)dma;
    (void)dma_ctx;
    (void)rsp;
    return -1;
}
struct nvmeof_cm *nvmeof_cm_create(struct nvmeof_target *target,
                                   uint32_t traddr, uint16_t trsvcid)
{
    (void)target;
    (void)traddr;
    (void)trsvcid;
    return NULL;
}
void nvmeof_cm_destroy(struct nvmeof_cm *cm)
{
    (void)cm;
}
void nvmeof_cm_drop_qp(struct nvmeof_cm *cm, uint32_t guest_qpn)
{
    (void)cm;
    (void)guest_qpn;
}
bool nvmeof_cm_handle_mad(struct nvmeof_cm *cm, const void *mad, size_t len,
                          void *rsp, struct nvmeof_cm_result *res)
{
    (void)cm;
    (void)mad;
    (void)len;
    (void)rsp;
    (void)res;
    return false;
}

/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
struct s3_target *s3_target_create(const struct s3_target_cfg *cfg, char *err,
                                   size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
/* NOLINTEND(readability-non-const-parameter) */
void s3_target_destroy(struct s3_target *t)
{
    (void)t;
}
void s3_tcp_cfg_from_target(struct s3_tcp_cfg *cfg,
                            const struct s3_target *target)
{
    (void)cfg;
    (void)target;
}
/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
struct s3_tcp *s3_tcp_create(const struct s3_tcp_cfg *cfg,
                             struct s3_target *target,
                             const struct s3_dma_ops *dma, void *dma_ctx,
                             s3_tcp_tx_fn tx, void *tx_ctx, char *err,
                             size_t errlen)
{
    (void)cfg;
    (void)target;
    (void)dma;
    (void)dma_ctx;
    (void)tx;
    (void)tx_ctx;
    (void)err;
    (void)errlen;
    return NULL;
}
/* NOLINTEND(readability-non-const-parameter) */
void s3_tcp_destroy(struct s3_tcp *s)
{
    (void)s;
}
bool s3_tcp_rx_frame(struct s3_tcp *s, const void *frame, size_t len,
                     uint64_t now_ms)
{
    (void)s;
    (void)frame;
    (void)len;
    (void)now_ms;
    return false;
}
bool s3_tcp_poll(struct s3_tcp *s, uint64_t now_ms)
{
    (void)s;
    (void)now_ms;
    return false;
}
bool s3_tcp_has_work(const struct s3_tcp *s)
{
    (void)s;
    return false;
}

void ionic_eth_emu_register_tx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_tx_filter_fn fn, void *ctx)
{
    (void)emu;
    (void)fn;
    (void)ctx;
}
int ionic_eth_emu_queue_rx_frame(struct ionic_eth_emu *emu, const void *frame,
                                 size_t len)
{
    (void)emu;
    (void)frame;
    (void)len;
    return -1;
}
/* The LIF rdma_* byte counters; this test only asserts the error counters. */
void ionic_eth_emu_count_rdma(struct ionic_eth_emu *emu, uint64_t bytes,
                              bool tx)
{
    (void)emu;
    (void)bytes;
    (void)tx;
}

void ernic_stats_qp_cqe(struct ernic_stats *st, uint32_t qp_id)
{
    (void)st;
    (void)qp_id;
}
void ernic_stats_qp_doorbell(struct ernic_stats *st, uint32_t qp_id,
                             bool is_send)
{
    (void)st;
    (void)qp_id;
    (void)is_send;
}
void ernic_stats_qp_wqe(struct ernic_stats *st, uint32_t qp_id, unsigned int wr)
{
    (void)st;
    (void)qp_id;
    (void)wr;
}
void ernic_stats_rdma_bytes(struct ernic_stats *st, uint32_t qp_id,
                            uint64_t bytes, enum ernic_stat_op op)
{
    (void)st;
    (void)qp_id;
    (void)bytes;
    (void)op;
}

#ifdef ERNIC_HAVE_P4
/* No P4 engine is attached here, so the pipeline never runs. The status
 * means nothing unless *done is set. */
uint32_t ionic_dp_p4_policy(struct ionic_datapath *dp, uint32_t qp_id,
                            uint8_t op, const uint8_t *wqe, uint32_t read_sz,
                            const struct dp_sge_list *src, uint32_t imm_be,
                            bool *done)
{
    (void)dp;
    (void)qp_id;
    (void)op;
    (void)wqe;
    (void)read_sz;
    (void)src;
    (void)imm_be;
    *done = false;
    return 0;
}
#endif

/* ---- Harness ----------------------------------------------------------- */

static int failures;

static const char *const counter_name[IONIC_DP_RDMA_COUNT] = {
#define COUNTER_NAME(id, name) [IONIC_DP_##id] = (name),
    IONIC_DP_RDMA_COUNTERS(COUNTER_NAME)
#undef COUNTER_NAME
};

/*
 * Guest layout.  Every ring and buffer is one direct DMA region, so a key-0
 * SGE's va is simply a GPA.  QP_C completes onto a CQ that was never
 * registered.
 */
#define CQ_ID      1u
#define CQ_UNKNOWN 99u
#define QP_A       2u
#define QP_B       3u
#define QP_C       4u
#define QP_ABSENT  500u
#define QP_RC      2 /* IB_QPT_RC: the one type that waits out an RNR */
#define QP_UC      3 /* IB_QPT_UC */
#define LOCAL_NODE 0u
#define PEER_NODE  1u
#define CQ_GPA     0x0000u
#define SQ_GPA(qp) (UINT64_C(0x1000) * ((qp) - 1u))
#define RQ_GPA(qp) (SQ_GPA(qp) + 0x400u)
#define SRC_GPA    0x4000u /* the requester's payload, key 0 */
#define RESULT_GPA 0x4800u /* where an atomic's original value lands */
#define MR_GPA     0x5000u /* the one registered region */
#define MR_LEN     256u
#define MR_KEY     0x101u
#define BAD_KEY    0x999u
#define RECV_GPA   0x6000u
#define RING_DEPTH 4 /* log2 */
#define WQE_STRIDE 6 /* log2: 64 bytes, room for two SGEs */
#define CQE_STRIDE 5 /* log2: 32 bytes, one CQE */
#define T0         1000000u
#define OP_UNKNOWN 0x0fu

/*
 * The data path under test, and what it asked the mesh to cancel: how many
 * times, the last tag and mask, and how many completions CQ_ID had then.
 */
static struct ionic_datapath *g_dp;
static uint32_t g_ncancel;
static uint64_t g_cancel_tag;
static uint64_t g_cancel_mask;
static uint32_t g_cancel_cqes;

void ernic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask)
{
    (void)handle;
    g_ncancel++;
    g_cancel_tag = tag;
    g_cancel_mask = mask;
    g_cancel_cqes = g_dp ? g_dp->cq[CQ_ID].prod : 0;
}

static uint32_t g_sq_slot[QP_C + 1];

static void register_qp_as(struct ionic_datapath *dp, uint32_t qp_id,
                           uint32_t cq_id, uint8_t type)
{
    struct ionic_dp_ring_desc sq = {
        .buf = {.dma_addr = SQ_GPA(qp_id), .map_count = 1},
        .depth_log2 = RING_DEPTH,
        .stride_log2 = WQE_STRIDE,
    };
    struct ionic_dp_ring_desc rq = sq;

    rq.buf.dma_addr = RQ_GPA(qp_id);
    if (ionic_datapath_register_qp(dp, qp_id, type, cq_id, &sq, cq_id, &rq)) {
        printf("FAIL: QP %u did not register\n", qp_id);
        failures++;
    }
    g_sq_slot[qp_id] = 0;
}

static void register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                        uint32_t cq_id)
{
    register_qp_as(dp, qp_id, cq_id, QP_RC);
}

/*
 * A fresh data path: one CQ, three RC QPs, and one MR.  QP_A and QP_B are
 * each other's peer on this instance; remote() moves QP_A's peer to the mesh.
 */
static struct ionic_datapath *setup(void)
{
    struct ionic_datapath *dp = ionic_datapath_create(NULL, NULL);
    struct ionic_dp_ring_desc cq = {
        .buf = {.dma_addr = CQ_GPA, .map_count = 1},
        .depth_log2 = 6,
        .stride_log2 = CQE_STRIDE,
    };
    struct ionic_dp_buf_desc mr = {.dma_addr = MR_GPA, .map_count = 1};

    if (!dp)
        return NULL;
    memset(g_mem, 0, sizeof(g_mem));
    for (size_t i = 0; i < MR_LEN; i++)
        g_mem[SRC_GPA + i] = (uint8_t)i;
    g_hole_gpa = UINT64_MAX;
    g_nwire = 0;
    g_room = true;
    g_send_rc = 0;
    g_sendv_hook = NULL;
    g_dp = NULL;
    g_ncancel = 0;
    g_cancel_tag = 0;
    g_cancel_mask = 0;
    g_cancel_cqes = 0;
    g_now_ms = T0;
    g_local_node = UINT32_MAX;
    g_recv_fn = NULL;
    g_recv_opaque = NULL;
    dp->now_ms = fake_now_ms;
    /* Set on the real clock when the data path was created */
    qemu_mutex_lock(&dp->rx_lock);
    dp->rx_polled_ms = T0;
    qemu_mutex_unlock(&dp->rx_lock);

    if (ionic_datapath_register_cq(dp, CQ_ID, 0, &cq) ||
        ionic_datapath_register_mr(dp, MR_KEY, MR_GPA, MR_LEN, &mr)) {
        ionic_datapath_destroy(dp);
        return NULL;
    }
    register_qp(dp, QP_A, CQ_ID);
    register_qp(dp, QP_B, CQ_ID);
    register_qp(dp, QP_C, CQ_UNKNOWN);
    ionic_datapath_set_dest(dp, QP_A, QP_B, UINT32_MAX);
    ionic_datapath_set_dest(dp, QP_B, QP_A, UINT32_MAX);
    return dp;
}

/* QP_A's peer becomes QP @dest_qp on node 1, so its work goes to the mesh. */
static void remote(struct ionic_datapath *dp, uint32_t dest_qp)
{
    g_local_node = LOCAL_NODE;
    ionic_datapath_set_pvrdma(dp, &g_mesh);
    ionic_datapath_set_dest(dp, QP_A, dest_qp, PEER_NODE);
}

/*
 * Deliver @len bytes from the peer node, as the mesh's receive thread does: in
 * a buffer from malloc(), which the data path takes.  These rows never fill
 * its queue, so it always has room.
 */
static void mesh_recv(const void *buf, size_t len)
{
    void *copy;

    if (!g_recv_fn) {
        printf("FAIL: the data path registered no mesh receive callback\n");
        failures++;
        return;
    }
    copy = malloc(len);
    if (!copy) {
        printf("FAIL: no memory for a %zu-byte mesh message\n", len);
        failures++;
        return;
    }
    memcpy(copy, buf, len);
    if (!g_recv_fn(g_recv_opaque, PEER_NODE, copy, len)) {
        printf("FAIL: the data path had no room for a %zu-byte mesh message\n",
               len);
        failures++;
        free(copy);
    }
}

struct wqe {
    uint8_t op;
    uint64_t va; /* the one local SGE */
    uint32_t len;
    uint32_t lkey;
    uint64_t rva; /* RDMA and atomic target */
    uint32_t rkey;
};

/* Write @w into @qp_id's next SQ slot and ring the SQ doorbell for it. */
static void post_send(struct ionic_datapath *dp, uint32_t qp_id,
                      const struct wqe *w)
{
    struct ionic_qp_ring *q = &dp->qp[qp_id];
    uint32_t slot = g_sq_slot[qp_id]++ % q->sq_depth;
    uint8_t *e = g_mem + SQ_GPA(qp_id) + ((size_t)slot << WQE_STRIDE);
    bool atomic =
        w->op == IONIC_V1_OP_ATOMIC_CS || w->op == IONIC_V1_OP_ATOMIC_FA;

    memset(e, 0, 1u << WQE_STRIDE);
    ernic_put_be64(e, slot);
    e[8] = w->op;
    e[9] = 1;
    ernic_put_be32(e + WQE_RDMA_VA_HI_OFF, (uint32_t)(w->rva >> 32));
    ernic_put_be32(e + WQE_RDMA_VA_LO_OFF, (uint32_t)w->rva);
    ernic_put_be32(e + WQE_RDMA_RKEY_OFF, w->rkey);
    if (atomic) {
        ernic_put_be32(e + WQE_ATOMIC_SWAP_ADD_OFF + 4, 1); /* fetch-add 1 */
        ernic_put_be64(e + WQE_ATOMIC_SGE_OFF, w->va);
        ernic_put_be32(e + WQE_ATOMIC_SGE_OFF + 12, w->lkey);
    } else {
        ernic_put_be32(e + WQE_SEND_LEN_OFF, w->len);
        ernic_put_be64(e + WQE_PLD_OFF, w->va);
        ernic_put_be32(e + WQE_PLD_OFF + 8, w->len);
        ernic_put_be32(e + WQE_PLD_OFF + 12, w->lkey);
    }
    ionic_datapath_doorbell(dp, DP_QTYPE_SQ,
                            (uint64_t)(g_sq_slot[qp_id] & 0xffffu) |
                                (uint64_t)(qp_id & 0xffu) << 24 |
                                (uint64_t)(qp_id >> 8) << 32);
}

/* Post one key-0 receive of @len bytes on @qp_id. */
static void post_recv(struct ionic_datapath *dp, uint32_t qp_id, uint32_t len)
{
    struct ionic_qp_ring *q = &dp->qp[qp_id];
    uint32_t slot = q->rq_prod % q->rq_depth;
    uint8_t *e = g_mem + RQ_GPA(qp_id) + ((size_t)slot << WQE_STRIDE);

    memset(e, 0, 1u << WQE_STRIDE);
    ernic_put_be64(e, 0x100u + slot);
    e[9] = 1;
    ernic_put_be64(e + WQE_PLD_OFF, RECV_GPA);
    ernic_put_be32(e + WQE_PLD_OFF + 8, len);
    q->rq_prod++;
}

/* Hand everything sent so far back in, as the peer node's traffic. */
static void wire_feed(struct ionic_datapath *dp)
{
    size_t n = g_nwire;
    static uint8_t buf[WIRE_MAX][WIRE_BYTES];
    size_t len[WIRE_MAX];

    (void)dp;

    memcpy(buf, g_wire, sizeof(buf));
    memcpy(len, g_wire_len, sizeof(len));
    g_nwire = 0;
    for (size_t i = 0; i < n; i++)
        mesh_recv(buf[i], len[i]);
}

static void poll_at(struct ionic_datapath *dp, uint64_t now)
{
    g_now_ms = now;
    ionic_datapath_poll(dp);
}

/* Feed and poll until the conversation goes quiet: request, then answer. */
static void pump(struct ionic_datapath *dp)
{
    for (int i = 0; i < 4 && g_nwire; i++) {
        wire_feed(dp);
        poll_at(dp, g_now_ms);
    }
}

static const struct wqe SEND64 = {IONIC_V1_OP_SEND, SRC_GPA, 64, 0, 0, 0};
static const struct wqe WRITE_OK = {
    IONIC_V1_OP_RDMA_WRITE, SRC_GPA, 64, 0, MR_GPA, MR_KEY};
static const struct wqe READ_OK = {
    IONIC_V1_OP_RDMA_READ, SRC_GPA, 64, 0, MR_GPA, MR_KEY};
static const struct wqe FADD_OK = {
    IONIC_V1_OP_ATOMIC_FA, RESULT_GPA, 8, 0, MR_GPA, MR_KEY};

/* ---- Scenarios --------------------------------------------------------- */

static void run_write_bad_rkey(struct ionic_datapath *dp)
{
    struct wqe w = WRITE_OK;

    w.rkey = BAD_KEY;
    post_send(dp, QP_A, &w);
}

static void run_write_bad_lkey(struct ionic_datapath *dp)
{
    struct wqe w = WRITE_OK;

    w.lkey = BAD_KEY;
    post_send(dp, QP_A, &w);
}

static void run_recv_too_small(struct ionic_datapath *dp)
{
    post_recv(dp, QP_B, 16);
    post_send(dp, QP_A, &SEND64);
}

static void run_unsupported_op(struct ionic_datapath *dp)
{
    struct wqe w = SEND64;

    w.op = OP_UNKNOWN;
    post_send(dp, QP_A, &w);
}

static void run_atomic_bad_rkey(struct ionic_datapath *dp)
{
    struct wqe w = FADD_OK;

    w.rkey = BAD_KEY;
    post_send(dp, QP_A, &w);
}

static void run_send_no_recv(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
}

static void run_send_bad_source(struct ionic_datapath *dp)
{
    struct wqe w = SEND64;

    w.lkey = BAD_KEY;
    post_recv(dp, QP_B, 256);
    post_send(dp, QP_A, &w);
}

static void run_all_succeed(struct ionic_datapath *dp)
{
    post_recv(dp, QP_B, 256);
    post_send(dp, QP_A, &SEND64);
    post_send(dp, QP_A, &WRITE_OK);
    post_send(dp, QP_A, &READ_OK);
    post_send(dp, QP_A, &FADD_OK);
}

static void run_remote_all_succeed(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    post_recv(dp, QP_B, 256);
    post_send(dp, QP_A, &SEND64);
    post_send(dp, QP_A, &WRITE_OK);
    post_send(dp, QP_A, &READ_OK);
    post_send(dp, QP_A, &FADD_OK);
    pump(dp);
}

static void run_remote_write_bad_rkey(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    run_write_bad_rkey(dp);
    pump(dp);
}

static void run_remote_write_rkey0(struct ionic_datapath *dp)
{
    struct wqe w = WRITE_OK;

    remote(dp, QP_B);
    w.rkey = 0;
    post_send(dp, QP_A, &w);
    pump(dp);
}

static void run_remote_write_absent_qp(struct ionic_datapath *dp)
{
    remote(dp, QP_ABSENT);
    run_write_bad_rkey(dp);
    pump(dp);
}

/* Three passes find no receive; the fourth, after one is posted, delivers. */
static void run_rnr_three_passes(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    post_send(dp, QP_A, &SEND64);
    wire_feed(dp);
    poll_at(dp, T0);
    poll_at(dp, T0 + 1);
    poll_at(dp, T0 + 2);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + 3);
    pump(dp);
}

static void run_rnr_just_inside(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    post_send(dp, QP_A, &SEND64);
    wire_feed(dp);
    poll_at(dp, T0);
    poll_at(dp, T0 + RNR_RETRY_MS - 1);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + RNR_RETRY_MS - 1);
    pump(dp);
}

static void run_rnr_exceeded(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    post_send(dp, QP_A, &SEND64);
    wire_feed(dp);
    poll_at(dp, T0);
    poll_at(dp, T0 + RNR_RETRY_MS);
    pump(dp);
}

/* The second SEND waits behind the first, so only the first is retried. */
static void run_rnr_blocked(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    post_send(dp, QP_A, &SEND64);
    post_send(dp, QP_A, &SEND64);
    wire_feed(dp);
    poll_at(dp, T0);
}

/* The same RNR conversations with the peer on this instance. */
static void run_local_rnr_three_passes(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    poll_at(dp, T0 + 1);
    poll_at(dp, T0 + 2);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + 3);
}

static void run_local_rnr_just_inside(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    poll_at(dp, T0 + RNR_RETRY_MS - 1);
}

static void run_local_rnr_just_inside_recv(struct ionic_datapath *dp)
{
    run_local_rnr_just_inside(dp);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + RNR_RETRY_MS - 1);
}

static void run_local_rnr_exceeded(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    poll_at(dp, T0 + RNR_RETRY_MS);
}

/* Both SENDs wait; every pass retries only the first. */
static void run_local_rnr_blocked(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    post_send(dp, QP_A, &SEND64);
    poll_at(dp, T0 + 1);
}

static void run_local_rnr_blocked_recv(struct ionic_datapath *dp)
{
    run_local_rnr_blocked(dp);
    post_recv(dp, QP_B, 256);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + 2);
}

/*
 * QP_A moves to ERR while its SEND waits, and only then does QP_B post a
 * receive.  The flush has already completed that SEND, so the retry must not
 * deliver it, nor complete it a second time, then or at RNR_RETRY_MS.
 */
static void run_local_rnr_flush(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    ionic_datapath_flush_qp(dp, QP_A);
    post_recv(dp, QP_B, 256);
    poll_at(dp, T0 + 1);
    poll_at(dp, T0 + RNR_RETRY_MS);
}

/* QP_A and QP_B as UC: unreliable, so a SEND with no receive is not retried
 * but dropped by the responder, and the send completes. */
static void run_uc_no_recv(struct ionic_datapath *dp)
{
    register_qp_as(dp, QP_A, CQ_ID, QP_UC);
    register_qp_as(dp, QP_B, CQ_ID, QP_UC);
    ionic_datapath_set_dest(dp, QP_A, QP_B, UINT32_MAX);
    ionic_datapath_set_dest(dp, QP_B, QP_A, UINT32_MAX);
    post_send(dp, QP_A, &SEND64);
    poll_at(dp, T0 + 1);
}

static void run_local_rnr_reregister(struct ionic_datapath *dp)
{
    post_send(dp, QP_A, &SEND64);
    register_qp(dp, QP_A, CQ_ID);
    ionic_datapath_set_dest(dp, QP_A, QP_B, UINT32_MAX);
    poll_at(dp, T0 + RNR_RETRY_MS);
}

/* The peer never answers: the request is lost on the wire. */
static void run_pending_at(struct ionic_datapath *dp, uint64_t now)
{
    remote(dp, QP_B);
    post_send(dp, QP_A, &SEND64);
    g_nwire = 0;
    poll_at(dp, now);
}

static void run_pending_at_deadline(struct ionic_datapath *dp)
{
    run_pending_at(dp, T0 + PENDING_MS);
}

static void run_pending_past_deadline(struct ionic_datapath *dp)
{
    run_pending_at(dp, T0 + PENDING_MS + 1);
}

/* The mesh can't send the request at all: it has no connection to node 1 */
static void run_remote_send_failed(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    g_send_rc = -ENOTCONN;
    post_send(dp, QP_A, &WRITE_OK);
    poll_at(dp, T0 + 1);
}

/* The mesh's queue to node 1 is full: the WRITE waits, and isn't gathered */
static void run_remote_busy(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    g_room = false;
    post_send(dp, QP_A, &WRITE_OK);
    poll_at(dp, T0 + 1);
    poll_at(dp, T0 + PENDING_MS + 1);
    if (g_nwire != 0) {
        printf("FAIL: a WRITE was sent with no room to queue it\n");
        failures++;
    }
}

/* Then the queue drains, and the next pass sends it */
static void run_remote_busy_then_room(struct ionic_datapath *dp)
{
    run_remote_busy(dp);
    g_room = true;
    poll_at(dp, g_now_ms + 1);
    pump(dp);
}

/*
 * The mesh refuses with -EAGAIN after all, as it does when the route to the
 * node changed after it found room, and then takes it.
 */
static void run_remote_send_busy(struct ionic_datapath *dp)
{
    remote(dp, QP_B);
    g_send_rc = -EAGAIN;
    post_send(dp, QP_A, &WRITE_OK);
    poll_at(dp, T0 + 1);
    g_send_rc = 0;
    poll_at(dp, T0 + 2);
    pump(dp);
}

/* QP_A moves to ERR while its WRITE waits for room, and room comes after */
static void run_remote_busy_flush(struct ionic_datapath *dp)
{
    run_remote_busy(dp);
    ionic_datapath_flush_qp(dp, QP_A);
    g_room = true;
    poll_at(dp, g_now_ms + 1);
    if (g_nwire != 0) {
        printf("FAIL: a flushed WRITE was sent once there was room\n");
        failures++;
    }
}

static void run_reregister(struct ionic_datapath *dp)
{
    run_unsupported_op(dp);
    register_qp(dp, QP_A, CQ_ID);
}

static void run_unknown_cq(struct ionic_datapath *dp)
{
    struct wqe w = SEND64;

    w.op = OP_UNKNOWN;
    post_send(dp, QP_C, &w);
}

/* ---- Tests ------------------------------------------------------------- */

struct want {
    enum ionic_dp_rdma_counter ctr;
    uint64_t n; /* 0 ends the list */
};

#define W(id, n)         \
    {                    \
        IONIC_DP_##id, n \
    }

/*
 * After @run, QP_A and QP_B hold exactly @qa and @qb, every other counter
 * of theirs is 0, and the device holds their sum plus @dev_extra -- the
 * counts that belong to no QP this instance has.
 */
struct dp_case {
    const char *category;
    const char *description;
    const char *expected;
    void (*run)(struct ionic_datapath *dp);
    struct want qa[4];
    struct want qb[4];
    struct want dev_extra[3];
};

static const struct dp_case cases[] = {
    /* positive */
    {"positive",
     "RDMA WRITE with an unregistered rkey",
     "the requester counts a remote access error and a CQE error; the "
     "responder, the same device, counts the NAK it sent",
     run_write_bad_rkey,
     {W(REQ_RX_REMOTE_ACCESS_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_REMOTE_ACCESS_ERR, 1)},
     {{0}}},
    {"positive",
     "RDMA WRITE whose local SGE names an unregistered lkey",
     "the requester counts a memory management error and a CQE error; "
     "the responder counts nothing",
     run_write_bad_lkey,
     {W(REQ_TX_MEMORY_MGMT_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {{0}}},
    {"positive",
     "SEND of 64 bytes into a 16-byte receive",
     "the responder counts a local length error and a CQE error",
     run_recv_too_small,
     {{0}},
     {W(RESP_RX_LOCAL_LEN_ERR, 1), W(RESP_RX_CQE_ERR, 1)},
     {{0}}},
    {"positive",
     "unsupported opcode",
     "the failing QP counts a local operation error and a CQE error, and "
     "the other QP's counters do not move",
     run_unsupported_op,
     {W(REQ_TX_LOCAL_OPER_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {{0}}},
    {"positive",
     "fetch-and-add with an unregistered rkey",
     "a remote access error on both ends, as for RDMA WRITE",
     run_atomic_bad_rkey,
     {W(REQ_RX_REMOTE_ACCESS_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_REMOTE_ACCESS_ERR, 1)},
     {{0}}},
    {"positive",
     "local SEND with no receive posted",
     "one RNR pass and one out-of-buffer event on the responder",
     run_send_no_recv,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 1), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"positive",
     "local SEND finds no receive for three passes, then one",
     "three RNR passes, one out-of-buffer event, and no error completion",
     run_local_rnr_three_passes,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 3), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"positive",
     "remote RDMA WRITE with an unregistered rkey",
     "the responder counts the NAK it sends; the requester counts the "
     "remote access error it receives",
     run_remote_write_bad_rkey,
     {W(REQ_RX_REMOTE_ACCESS_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_REMOTE_ACCESS_ERR, 1)},
     {{0}}},
    {"positive",
     "remote SEND finds no receive for three passes, then one",
     "three RNR passes, one out-of-buffer event, and no error completion",
     run_rnr_three_passes,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 3), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},

    /* negative */
    {"negative",
     "local SEND, RDMA WRITE, RDMA READ and fetch-and-add that "
     "all succeed",
     "no counter moves",
     run_all_succeed,
     {{0}},
     {{0}},
     {{0}}},
    {"negative",
     "the same four operations over the mesh",
     "no counter moves",
     run_remote_all_succeed,
     {{0}},
     {{0}},
     {{0}}},
    {"negative",
     "an error completion on a CQ that was never registered",
     "no counter moves: the guest never saw the error",
     run_unknown_cq,
     {{0}},
     {{0}},
     {{0}}},
    {"negative",
     "remote RDMA WRITE the mesh can't send, having no connection to the "
     "node",
     "completes with retry-exceeded, as a request the peer never answers "
     "does: an implied NAK sequence error and a CQE error, not a remote "
     "access error",
     run_remote_send_failed,
     {W(REQ_RX_IMPLIED_NAK_SEQ_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {{0}}},
    {"negative",
     "remote RDMA WRITE while the mesh has no room to queue it, past "
     "PENDING_MS",
     "no counter moves: the WRITE waits, and nothing times out",
     run_remote_busy,
     {{0}},
     {{0}},
     {{0}}},

    /* boundary */
    {"boundary",
     "remote SEND still unanswered at RNR_RETRY_MS - 1",
     "two RNR passes and one out-of-buffer event; still delivered, so "
     "neither end counts an error",
     run_rnr_just_inside,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 2), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"boundary",
     "remote SEND still unanswered at exactly RNR_RETRY_MS",
     "one RNR pass, then the drop: the requester counts an RNR retry error "
     "and a CQE error",
     run_rnr_exceeded,
     {W(REQ_RX_RNR_RETRY_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_RNR_RETRY_ERR, 1), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"boundary",
     "local SEND still waiting at RNR_RETRY_MS - 1",
     "two RNR passes and one out-of-buffer event; still delivered, so "
     "neither end counts an error",
     run_local_rnr_just_inside_recv,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 2), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"boundary",
     "local SEND still waiting at exactly RNR_RETRY_MS",
     "one RNR pass, then the drop: the requester counts an RNR retry error "
     "and a CQE error",
     run_local_rnr_exceeded,
     {W(REQ_RX_RNR_RETRY_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_RNR_RETRY_ERR, 1), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"boundary",
     "peer silent until exactly PENDING_MS",
     "not yet timed out: no counter moves",
     run_pending_at_deadline,
     {{0}},
     {{0}},
     {{0}}},
    {"boundary",
     "peer silent until PENDING_MS + 1",
     "the request times out: an implied NAK sequence error and a CQE error",
     run_pending_past_deadline,
     {W(REQ_RX_IMPLIED_NAK_SEQ_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {{0}}},

    /* corner */
    {"corner",
     "remote RDMA WRITE with rkey 0",
     "refused before any lookup, and counted like any other remote access "
     "error",
     run_remote_write_rkey0,
     {W(REQ_RX_REMOTE_ACCESS_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {W(RESP_TX_REMOTE_ACCESS_ERR, 1)},
     {{0}}},
    {"corner",
     "remote NAK for a QP this instance does not have",
     "the responder's NAK counts on the device alone",
     run_remote_write_absent_qp,
     {W(REQ_RX_REMOTE_ACCESS_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {W(RESP_TX_REMOTE_ACCESS_ERR, 1)}},
    {"corner",
     "a second SEND held behind one that found no receive",
     "one RNR pass and one out-of-buffer event: the held SEND was never "
     "tried",
     run_rnr_blocked,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 1), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"corner",
     "a second local SEND held behind one that found no receive",
     "three RNR passes, all for the first SEND (its doorbell, the second "
     "SEND's, and one poll), and one out-of-buffer event",
     run_local_rnr_blocked,
     {{0}},
     {W(RESP_TX_RNR_RETRY_ERR, 3), W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"negative",
     "UC SEND with no receive posted",
     "the responder counts one out-of-buffer event and no RNR retry: UC has "
     "none, so the message is dropped at once",
     run_uc_no_recv,
     {{0}},
     {W(RESP_RX_OUT_OF_BUFFER, 1)},
     {{0}}},
    {"corner",
     "local SEND whose source SGE does not resolve",
     "the requester counts a memory management error and a CQE error; the "
     "send is refused before delivery, so the responder counts nothing",
     run_send_bad_source,
     {W(REQ_TX_MEMORY_MGMT_ERR, 1), W(REQ_RX_CQE_ERR, 1)},
     {{0}},
     {{0}}},
    {"corner",
     "QP re-registered after an error",
     "the QP's counters start again from 0; the device keeps its totals",
     run_reregister,
     {{0}},
     {{0}},
     {W(REQ_TX_LOCAL_OPER_ERR, 1), W(REQ_RX_CQE_ERR, 1)}},
};

static void add_want(uint64_t *v, const struct want *w, size_t n)
{
    for (size_t i = 0; i < n && w[i].n; i++)
        v[w[i].ctr] += w[i].n;
}

/* The first counter where @got and @want differ, or -1. */
static int first_diff(const uint64_t *got, const uint64_t *want)
{
    for (int i = 0; i < IONIC_DP_RDMA_COUNT; i++)
        if (got[i] != want[i])
            return i;
    return -1;
}

static void run_case(const struct dp_case *c)
{
    struct ionic_datapath *dp = setup();
    uint64_t want_a[IONIC_DP_RDMA_COUNT] = {0};
    uint64_t want_b[IONIC_DP_RDMA_COUNT] = {0};
    uint64_t want_c[IONIC_DP_RDMA_COUNT] = {0};
    uint64_t want_dev[IONIC_DP_RDMA_COUNT] = {0};
    uint64_t got[IONIC_DP_RDMA_COUNT];
    const char *where = NULL;
    int diff = -1;
    uint64_t g = 0, w = 0;

    if (!dp) {
        printf("FAIL %s: %s; expected: %s\n     could not create a data path\n",
               c->category, c->description, c->expected);
        failures++;
        return;
    }

    add_want(want_a, c->qa, 4);
    add_want(want_b, c->qb, 4);
    add_want(want_dev, c->qa, 4);
    add_want(want_dev, c->qb, 4);
    add_want(want_dev, c->dev_extra, 3);

    c->run(dp);

    const struct {
        const char *name;
        uint32_t qp_id;
        const uint64_t *want;
    } views[] = {
        {"QP_A", QP_A, want_a},
        {"QP_B", QP_B, want_b},
        {"QP_C", QP_C, want_c},
        {"device", IONIC_DP_DEVICE, want_dev},
    };
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]) && !where; i++) {
        ionic_datapath_rdma_counters(dp, views[i].qp_id, got);
        diff = first_diff(got, views[i].want);
        if (diff >= 0) {
            where = views[i].name;
            g = got[diff];
            w = views[i].want[diff];
        }
    }
    ionic_datapath_destroy(dp);

    printf("%s %s: %s; expected: %s\n", where ? "FAIL" : "ok  ", c->category,
           c->description, c->expected);
    if (where) {
        printf("     got %s %s = %" PRIu64 " (want %" PRIu64 ")\n", where,
               counter_name[diff], g, w);
        failures++;
    }
}

/* The getter reads zeros for anything it does not have counters for. */
struct getter_case {
    const char *category;
    const char *description;
    const char *expected;
    bool null_dp;
    uint32_t qp_id;
};

static const struct getter_case getter_cases[] = {
    {"negative", "counters for a NULL data path", "all zeros", true,
     IONIC_DP_DEVICE},
    {"negative", "counters for a qp_id that was never registered",
     "all zeros, not another QP's", false, QP_ABSENT},
    {"boundary", "counters for qp_id IONIC_DP_DEVICE - 1",
     "all zeros: past the QP table, and not the device", false,
     IONIC_DP_DEVICE - 1},
    {"corner", "counters for a QP unregistered after an error",
     "all zeros, though the device still counts the error", false, QP_A},
};

static void run_getter_case(const struct getter_case *c)
{
    struct ionic_datapath *dp = setup();
    uint64_t got[IONIC_DP_RDMA_COUNT];
    bool ok = dp != NULL;

    if (dp) {
        run_unsupported_op(dp);
        ionic_datapath_unregister_qp(dp, QP_A);
        memset(got, 0xa5, sizeof(got));
        ionic_datapath_rdma_counters(c->null_dp ? NULL : dp, c->qp_id, got);
        for (int i = 0; i < IONIC_DP_RDMA_COUNT; i++)
            if (got[i])
                ok = false;
        ionic_datapath_rdma_counters(dp, IONIC_DP_DEVICE, got);
        if (got[IONIC_DP_REQ_TX_LOCAL_OPER_ERR] != 1)
            ok = false;
        ionic_datapath_destroy(dp);
    }
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok)
        failures++;
}

/* Sum of every RDMA counter @qp_id has, so "all zero" is one comparison. */
static uint64_t rdma_sum(const struct ionic_datapath *dp, uint32_t qp_id)
{
    uint64_t v[IONIC_DP_RDMA_COUNT];
    uint64_t sum = 0;

    ionic_datapath_rdma_counters(dp, qp_id, v);
    for (int i = 0; i < IONIC_DP_RDMA_COUNT; i++)
        sum += v[i];
    return sum;
}

/*
 * A device reset zeroes the counters where they are kept, the device's and
 * each live QP's, so neither the LIF block nor hw_counters gets old values
 * back on its next read.
 */
static void check_reset_counters(void)
{
    struct ionic_datapath *dp = setup();
    uint64_t dev_before = 0, qp_before = 0, dev_after = 1, qp_after = 1;
    bool ok = dp != NULL;

    /* A NULL data path is no data path, and is not an error. */
    ionic_datapath_reset_rdma_counters(NULL);
    if (dp) {
        run_unsupported_op(dp);
        dev_before = rdma_sum(dp, IONIC_DP_DEVICE);
        qp_before = rdma_sum(dp, QP_A);
        ionic_datapath_reset_rdma_counters(dp);
        dev_after = rdma_sum(dp, IONIC_DP_DEVICE);
        qp_after = rdma_sum(dp, QP_A);
        ionic_datapath_destroy(dp);
    }
    ok = ok && dev_before && qp_before && !dev_after && !qp_after;
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", "positive",
           "an error counted, then the counters reset",
           "device and QP counters both nonzero, then both zero");
    if (!ok) {
        printf("     got device %" PRIu64 " -> %" PRIu64 ", QP %" PRIu64
               " -> %" PRIu64 "\n",
               dev_before, dev_after, qp_before, qp_after);
        failures++;
    }
}

/*
 * What an RNR does to the queues rather than the counters: an RC send queue
 * waits for the receive instead of dropping the message.
 */
struct sq_case {
    const char *category;
    const char *description;
    const char *expected;
    void (*run)(struct ionic_datapath *dp);
    uint32_t cqes;       /* completions on CQ_ID, sends and receives */
    bool delivered;      /* the SEND's bytes reached QP_B's receive buffer */
    uint32_t sq_stalled; /* send queues still waiting afterwards */
    uint32_t sq_cons;    /* QP_A's send queue position afterwards */
};

static const struct sq_case sq_cases[] = {
    {"positive", "local SEND finds no receive for three passes, then one",
     "delivered: the receive and the send complete, and the queue moves on",
     run_local_rnr_three_passes, 2, true, 0, 1},
    {"negative", "local SEND, RDMA WRITE, RDMA READ and fetch-and-add",
     "no queue ever waits", run_all_succeed, 5, true, 0, 4},
    {"boundary", "local SEND with no receive at RNR_RETRY_MS - 1",
     "nothing completes and the send queue still waits at the SEND",
     run_local_rnr_just_inside, 0, false, 1, 0},
    {"boundary", "a receive posted at RNR_RETRY_MS - 1",
     "delivered on that pass", run_local_rnr_just_inside_recv, 2, true, 0, 1},
    {"boundary", "local SEND with no receive at exactly RNR_RETRY_MS",
     "the send completes in error and the queue moves on, nothing delivered",
     run_local_rnr_exceeded, 1, false, 0, 1},
    {"corner", "a second local SEND behind one that waits",
     "neither completes: the second may not overtake the first",
     run_local_rnr_blocked, 0, false, 1, 0},
    {"corner", "two receives posted for two waiting SENDs",
     "both delivered, all four completions, the queue empty",
     run_local_rnr_blocked_recv, 4, true, 0, 2},
    {"negative", "UC SEND with no receive posted",
     "dropped at once: the send completes, nothing is delivered and no queue "
     "waits",
     run_uc_no_recv, 1, false, 0, 1},
    {"corner", "QP flushed while its SEND waits, then a receive posted",
     "only the two flush completions: the held SEND is neither delivered nor "
     "completed again, then or at RNR_RETRY_MS",
     run_local_rnr_flush, 2, false, 0, 1},
    {"corner", "QP re-registered while its send queue waits",
     "the wait is forgotten: nothing completes at RNR_RETRY_MS",
     run_local_rnr_reregister, 0, false, 0, 0},
    {"negative", "remote RDMA WRITE while the mesh has no room to queue it",
     "nothing is sent or completed, and the send queue waits at the WRITE, "
     "past PENDING_MS",
     run_remote_busy, 0, false, 1, 0},
    {"positive", "the mesh then has room",
     "the next pass sends the WRITE, which completes, and the queue moves on",
     run_remote_busy_then_room, 1, false, 0, 1},
    {"corner", "the mesh refuses the WRITE with -EAGAIN, then takes it",
     "it waits for a pass, then is sent and completes", run_remote_send_busy, 1,
     false, 0, 1},
    {"corner", "QP flushed while its WRITE waits for room, then room comes",
     "only the two flush completions: the WRITE is never sent",
     run_remote_busy_flush, 2, false, 0, 1},
};

static bool recv_delivered(void)
{
    for (uint32_t i = 0; i < SEND64.len; i++)
        if (g_mem[RECV_GPA + i] != g_mem[SRC_GPA + i])
            return false;
    return true;
}

static void run_sq_case(const struct sq_case *c)
{
    struct ionic_datapath *dp = setup();
    char got[160] = "could not create a data path";
    bool ok = false;

    if (dp) {
        c->run(dp);
        uint32_t cqes = dp->cq[CQ_ID].prod;
        bool delivered = recv_delivered();

        ok = cqes == c->cqes && delivered == c->delivered &&
             dp->sq_stalled == c->sq_stalled &&
             dp->qp[QP_A].sq_cons == c->sq_cons;
        snprintf(got, sizeof(got),
                 "%" PRIu32 " completions, %sdelivered, %" PRIu32
                 " waiting, sq_cons %" PRIu32,
                 cqes, delivered ? "" : "not ", dp->sq_stalled,
                 dp->qp[QP_A].sq_cons);
        ionic_datapath_destroy(dp);
    }
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got %s\n", got);
        failures++;
    }
}

/*
 * The wire header byte for byte, as every node in a mesh sends and expects
 * it, whatever version it runs (and as tests/py/ernic/mesh.py's
 * IONIC_WIRE_FMT has it).  Each field holds a different value, so a field
 * at the wrong offset or in the wrong byte order shows.
 */
static void check_wire_hdr_layout(void)
{
    static const uint8_t want[IONIC_WIRE_HDR_LEN] = {
        0x49,
        0x4f,
        0x4e,
        0x43, /* magic, "IONC" */
        IONIC_WIRE_ATOMIC_REQ,
        1,
        0,
        0, /* op, cs, reserved */
        0x04,
        0x03,
        0x02,
        0x01, /* src_qp_id */
        0x08,
        0x07,
        0x06,
        0x05, /* dst_qp_id */
        0x0c,
        0x0b,
        0x0a,
        0x09, /* req_id */
        0x10,
        0x0f,
        0x0e,
        0x0d, /* rkey */
        0x14,
        0x13,
        0x12,
        0x11, /* length */
        0xa1,
        0xa2,
        0xa3,
        0xa4, /* immediate */
        0x18,
        0x17,
        0x16,
        0x15, /* status */
        0x28,
        0x27,
        0x26,
        0x25,
        0x24,
        0x23,
        0x22,
        0x21, /* remote_va */
        0x38,
        0x37,
        0x36,
        0x35,
        0x34,
        0x33,
        0x32,
        0x31, /* swap_add */
        0x48,
        0x47,
        0x46,
        0x45,
        0x44,
        0x43,
        0x42,
        0x41, /* compare */
    };
    static const uint8_t imm[4] = {0xa1, 0xa2, 0xa3, 0xa4};
    struct ionic_wire_hdr h, back;
    uint8_t got[IONIC_WIRE_HDR_LEN];
    bool put_ok, get_ok;

    memset(&h, 0, sizeof(h));
    h.magic = IONIC_WIRE_MAGIC;
    h.op = IONIC_WIRE_ATOMIC_REQ;
    h.atomic_cs = 1;
    h.src_qp_id = 0x01020304u;
    h.dst_qp_id = 0x05060708u;
    h.req_id = 0x090a0b0cu;
    h.rkey = 0x0d0e0f10u;
    h.length = 0x11121314u;
    memcpy(&h.imm_be, imm, sizeof(imm));
    h.status = 0x15161718u;
    h.remote_va = UINT64_C(0x2122232425262728);
    h.swap_add = UINT64_C(0x3132333435363738);
    h.compare = UINT64_C(0x4142434445464748);

    /* The reserved bytes start out non-zero, to show the put clears them */
    memset(got, 0xee, sizeof(got));
    ionic_dp_wire_hdr_put(got, &h);
    put_ok = memcmp(got, want, sizeof(want)) == 0;

    ionic_dp_wire_hdr_get(&back, want);
    get_ok = back.magic == h.magic && back.op == h.op &&
             back.atomic_cs == h.atomic_cs && back.src_qp_id == h.src_qp_id &&
             back.dst_qp_id == h.dst_qp_id && back.req_id == h.req_id &&
             back.rkey == h.rkey && back.length == h.length &&
             back.imm_be == h.imm_be && back.status == h.status &&
             back.remote_va == h.remote_va && back.swap_add == h.swap_add &&
             back.compare == h.compare;

    printf("%s positive: the wire header written; expected: the documented "
           "bytes\n",
           put_ok ? "ok  " : "FAIL");
    printf("%s positive: the wire header read; expected: every field back\n",
           get_ok ? "ok  " : "FAIL");
    failures += !put_ok + !get_ok;
}

/*
 * An atomic works on the u64 a little-endian guest keeps in memory, whatever
 * the host's byte order: the target's bytes change as that u64 does, and
 * the original value comes back, to the requester's SGE and, from another
 * node, in the ATOMIC_RESP, in the same order.  The add carries out of the
 * low byte, which shows a value read in the wrong order.
 */
static void check_atomic_byte_order(void)
{
    static const uint8_t before[8] = {0xff, 0, 0, 0, 0, 0, 0, 0x80};
    static const uint8_t after[8] = {0x00, 0x01, 0, 0, 0, 0, 0, 0x80};
    static const struct {
        const char *description;
        bool remote;
    } rows[] = {
        {"a fetch-and-add on this node's memory", false},
        {"a fetch-and-add on another node's memory", true},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = NULL;

        if (!dp) {
            why = "no data path";
        } else {
            if (rows[row].remote)
                remote(dp, QP_B);
            memcpy(g_mem + MR_GPA, before, sizeof(before));
            post_send(dp, QP_A, &FADD_OK);
            poll_at(dp, g_now_ms);
            if (rows[row].remote) {
                struct ionic_wire_hdr h;

                /* The request, to the peer, which is this node */
                wire_feed(dp);
                poll_at(dp, g_now_ms);
                memset(&h, 0, sizeof(h));
                if (g_nwire == 1)
                    ionic_dp_wire_hdr_get(&h, g_wire[0]);
                if (g_nwire != 1 || h.op != IONIC_WIRE_ATOMIC_RESP ||
                    g_wire_len[0] != IONIC_WIRE_HDR_LEN + sizeof(before))
                    why = "the peer didn't answer with an 8-byte ATOMIC_RESP";
                else if (memcmp(g_wire[0] + IONIC_WIRE_HDR_LEN, before,
                                sizeof(before)) != 0)
                    why = "the ATOMIC_RESP's value isn't little-endian";
                /* The answer, back to the requester */
                wire_feed(dp);
                poll_at(dp, g_now_ms);
            }
            if (!why && dp->cq[CQ_ID].prod != 1)
                why = "the atomic didn't complete";
            if (!why && memcmp(g_mem + MR_GPA, after, sizeof(after)) != 0)
                why = "the target wasn't added to as a little-endian u64";
            if (!why && memcmp(g_mem + RESULT_GPA, before, sizeof(before)) != 0)
                why = "the original value didn't land little-endian";
            ionic_datapath_destroy(dp);
        }
        printf("%s positive: %s; expected: the u64 in guest byte order\n",
               why ? "FAIL" : "ok  ", rows[row].description);
        if (why) {
            printf("     got %s\n", why);
            failures++;
        }
    }
}

static void check_late_responses(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        bool expire;
        uint32_t status;
    } rows[] = {
        {"positive", "duplicate successful ACK",
         "one CQE, no second counter change", false, IONIC_STS_OK},
        {"negative", "duplicate error ACK",
         "one error CQE/count, no second counter change", false,
         IONIC_STS_REMOTE_ACC_ERR},
        {"boundary", "success arrives after timeout",
         "timeout CQE/count retained exactly once", true, IONIC_STS_OK},
        {"corner", "error arrives twice after timeout",
         "timeout CQE/count retained exactly once", true,
         IONIC_STS_REMOTE_ACC_ERR},
    };
    static const uint32_t views[] = {QP_A, QP_B, QP_C, IONIC_DP_DEVICE};

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        struct ionic_wire_hdr request, reply = {0};
        uint8_t reply_wire[IONIC_WIRE_HDR_LEN];
        uint64_t before[4][IONIC_DP_RDMA_COUNT], after[IONIC_DP_RDMA_COUNT];
        bool ok = dp != NULL;

        if (dp) {
            remote(dp, QP_B);
            post_send(dp, QP_A, &SEND64);
            ok = g_nwire == 1;
            if (ok) {
                ionic_dp_wire_hdr_get(&request, g_wire[0]);
                reply.magic = request.magic;
                reply.op = IONIC_WIRE_ACK;
                reply.src_qp_id = request.dst_qp_id;
                reply.dst_qp_id = request.src_qp_id;
                reply.req_id = request.req_id;
                reply.status = rows[row].status;
                ionic_dp_wire_hdr_put(reply_wire, &reply);
                g_nwire = 0;
                if (rows[row].expire)
                    poll_at(dp, T0 + PENDING_MS + 1);
                else {
                    mesh_recv(reply_wire, sizeof(reply_wire));
                    poll_at(dp, T0 + 1);
                }
                ok = dp->cq[CQ_ID].prod == 1;
                for (size_t v = 0; v < 4; v++)
                    ionic_datapath_rdma_counters(dp, views[v], before[v]);
                for (unsigned int repeat = 0; repeat < 2; repeat++) {
                    mesh_recv(reply_wire, sizeof(reply_wire));
                    poll_at(dp, g_now_ms + 1);
                }
                ok = ok && dp->cq[CQ_ID].prod == 1;
                for (size_t v = 0; v < 4; v++) {
                    ionic_datapath_rdma_counters(dp, views[v], after);
                    ok = ok && memcmp(before[v], after, sizeof(after)) == 0;
                }
            }
            ionic_datapath_destroy(dp);
        }
        printf("%s %s: %s; expected: %s\n", ok ? "ok" : "FAIL",
               rows[row].category, rows[row].description, rows[row].expected);
        if (!ok)
            failures++;
    }
}

/*
 * What the mesh is told about a QP's messages: what each request is called,
 * and which to take back.  A request the mesh still has queued must not
 * reach the peer once its work request has completed: when its QP is
 * flushed, or when it times out.  The completions tell the consumer it
 * didn't run, so it is taken back before they are posted.  A destroyed QP's
 * requests complete nothing, and still go out, and a QP given its qp_id next
 * takes back only its own: requests are called by the QP's mesh_id, new at
 * each registration, not by its qp_id.
 */
enum mc_scenario {
    MC_TAGGED,     /* a remote WRITE and its ACK */
    MC_FLUSH,      /* QP_A flushed with its WRITE outstanding */
    MC_UNREGISTER, /* QP_A destroyed with its WRITE outstanding */
    MC_REUSED,     /* and a new QP_A, flushed */
    MC_DEADLINE,   /* the WRITE unanswered at PENDING_MS */
    MC_EXPIRED,    /* and just past it */
};

#define MC_QP_MASK UINT64_C(0xffffffff00000000)

/* Whether the last cancel took back what is called tag, with exactly mask */
static bool mc_cancelled(uint64_t tag, uint64_t mask)
{
    return g_ncancel == 1 && g_cancel_mask == mask &&
           (g_cancel_tag & mask) == (tag & mask);
}

static const char *mc_run(struct ionic_datapath *dp, enum mc_scenario scenario)
{
    struct ionic_wire_hdr h;
    uint64_t tag;
    uint32_t cqes;

    remote(dp, QP_B);
    post_send(dp, QP_A, &WRITE_OK);
    if (g_nwire != 1 || g_wire_reply[0])
        return "the WRITE wasn't sent as a request";
    ionic_dp_wire_hdr_get(&h, g_wire[0]);
    tag = (uint64_t)dp->qp[QP_A].mesh_id << 32 | h.req_id;
    if (g_wire_tag[0] != tag)
        return "the WRITE isn't called by QP_A's mesh_id and its request id";

    cqes = dp->cq[CQ_ID].prod;
    g_dp = dp;
    switch (scenario) {
    case MC_TAGGED:
        wire_feed(dp);
        poll_at(dp, g_now_ms);
        if (g_nwire != 1 || !g_wire_reply[0])
            return "the ACK wasn't sent as a reply";
        return g_ncancel == 0 ? NULL : "a request was taken back";
    case MC_FLUSH:
        ionic_datapath_flush_qp(dp, QP_A);
        if (!mc_cancelled(tag, MC_QP_MASK))
            return "QP_A's requests weren't taken back once";
        if (g_cancel_cqes != cqes || dp->cq[CQ_ID].prod != cqes + 2)
            return "they weren't taken back before the flush completions";
        return NULL;
    case MC_UNREGISTER:
        ionic_datapath_unregister_qp(dp, QP_A);
        return g_ncancel == 0 ? NULL : "QP_A's requests were taken back";
    case MC_REUSED:
        ionic_datapath_unregister_qp(dp, QP_A);
        register_qp(dp, QP_A, CQ_ID);
        remote(dp, QP_B);
        ionic_datapath_flush_qp(dp, QP_A);
        if (g_ncancel != 1 || g_cancel_mask != MC_QP_MASK ||
            g_cancel_tag >> 32 != dp->qp[QP_A].mesh_id)
            return "the new QP_A's requests weren't taken back once";
        return (g_cancel_tag & MC_QP_MASK) != (tag & MC_QP_MASK)
                   ? NULL
                   : "the destroyed QP_A's WRITE was taken back with them";
    case MC_DEADLINE:
        poll_at(dp, T0 + PENDING_MS);
        return g_ncancel == 0 && dp->cq[CQ_ID].prod == cqes
                   ? NULL
                   : "the WRITE was given up on at its deadline";
    case MC_EXPIRED:
    default:
        poll_at(dp, T0 + PENDING_MS + 1);
        if (!mc_cancelled(tag, UINT64_MAX))
            return "the WRITE wasn't taken back once, alone";
        if (g_cancel_cqes != cqes || dp->cq[CQ_ID].prod != cqes + 1)
            return "it wasn't taken back before its error completion";
        return NULL;
    }
}

static void check_mesh_cancel(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        enum mc_scenario scenario;
    } rows[] = {
        {"positive", "a remote WRITE, and the ACK QP_B sends for it",
         "the WRITE goes to the mesh as a request called by QP_A's mesh_id "
         "and its request id, the ACK as a reply, and nothing is taken back",
         MC_TAGGED},
        {"positive", "QP_A flushed while its WRITE is outstanding",
         "the mesh takes back QP_A's requests once, before the flush "
         "completions are posted",
         MC_FLUSH},
        {"corner", "QP_A destroyed while its WRITE is outstanding",
         "nothing is taken back: the WRITE still goes out, as a consumer "
         "that destroys a QP straight after an unsignaled WRITE counts on",
         MC_UNREGISTER},
        {"corner",
         "QP_A destroyed with its WRITE outstanding, then a new "
         "QP_A flushed",
         "the mesh takes back the new QP_A's requests, and not the destroyed "
         "one's WRITE, which still goes out",
         MC_REUSED},
        {"boundary", "the WRITE still unanswered at PENDING_MS",
         "nothing is taken back or completed", MC_DEADLINE},
        {"positive", "the WRITE unanswered just past PENDING_MS",
         "the mesh takes back that request alone, once, before its "
         "retry-exceeded completion is posted",
         MC_EXPIRED},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? mc_run(dp, rows[row].scenario)
                             : "could not create a data path";

        g_dp = NULL;
        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * A reply to a QP that is gone.  Its requests still go out
 * (check_mesh_cancel()), so their replies still come back, but nothing may land
 * or complete for them: the consumer has its memory back, and a QP that took
 * the qp_id since isn't the one that asked.
 */
enum dr_when {
    DR_DESTROYED, /* QP_A destroyed before its request is answered */
    DR_REUSED,    /* and a new QP_A created */
    DR_RECREATED, /* a new QP_A created over the old, with no destroy */
};

static const char *dr_run(struct ionic_datapath *dp, const struct wqe *w,
                          enum dr_when when)
{
    /* From a READ's landing place through an atomic's */
    uint8_t before[RESULT_GPA + 8 - SRC_GPA];
    uint32_t cqes;

    remote(dp, QP_B);
    /* An atomic's original value is zero; let it show where it lands. */
    memset(g_mem + RESULT_GPA, 0xa5, 8);
    post_send(dp, QP_A, w);
    if (g_nwire != 1 || g_wire_reply[0])
        return "the request wasn't sent";
    wire_feed(dp);
    poll_at(dp, g_now_ms);
    if (g_nwire != 1 || !g_wire_reply[0])
        return "the peer didn't answer it";

    memcpy(before, g_mem + SRC_GPA, sizeof(before));
    cqes = dp->cq[CQ_ID].prod;
    if (when != DR_RECREATED)
        ionic_datapath_unregister_qp(dp, QP_A);
    if (when != DR_DESTROYED) {
        register_qp(dp, QP_A, CQ_ID);
        remote(dp, QP_B);
    }
    wire_feed(dp);
    poll_at(dp, g_now_ms);

    if (dp->cq[CQ_ID].prod != cqes)
        return "the answer was completed";
    if (memcmp(before, g_mem + SRC_GPA, sizeof(before)) != 0)
        return "the answer landed in the consumer's memory";
    return NULL;
}

static void check_destroyed_qp_replies(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        const struct wqe *w;
        enum dr_when when;
    } rows[] = {
        {"negative", "a READ answered after QP_A is destroyed",
         "the result lands nowhere, and nothing completes", &READ_OK,
         DR_DESTROYED},
        {"negative",
         "a READ answered after QP_A is destroyed and its qp_id reused",
         "the result lands nowhere, and the new QP_A completes nothing",
         &READ_OK, DR_REUSED},
        {"negative",
         "a fetch-and-add answered after QP_A is destroyed and its qp_id "
         "reused",
         "the original value lands nowhere, and the new QP_A completes "
         "nothing",
         &FADD_OK, DR_REUSED},
        {"negative",
         "a WRITE acknowledged after QP_A is destroyed and its qp_id reused",
         "the new QP_A completes nothing", &WRITE_OK, DR_REUSED},
        {"corner",
         "a READ answered after a new QP_A is created over the old one, "
         "which was never destroyed",
         "the result lands nowhere, and the new QP_A completes nothing",
         &READ_OK, DR_RECREATED},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? dr_run(dp, rows[row].w, rows[row].when)
                             : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * A WRITE from another node is checked before any of it lands: one whose
 * header length isn't its payload's, or whose range runs past the end of the
 * rkey's region, is refused with the region untouched, as sq_rdma() refuses
 * one from this node.  Each row is a WRITE from QP_A on the peer to QP_B.
 */
#define RW_MAX_PAYLOAD 64u

static const char *rw_run(struct ionic_datapath *dp, uint32_t rkey, uint64_t va,
                          uint32_t length, uint32_t payload_len,
                          uint32_t want_status)
{
    uint8_t msg[IONIC_WIRE_HDR_LEN + RW_MAX_PAYLOAD];
    uint8_t before[MR_LEN];
    struct ionic_wire_hdr h;

    if (payload_len > RW_MAX_PAYLOAD)
        return "the row's payload doesn't fit the message";
    remote(dp, QP_B);
    /* Not the payload's bytes, so a byte that lands shows */
    memset(g_mem + MR_GPA, 0x5a, MR_LEN);
    memcpy(before, g_mem + MR_GPA, sizeof(before));

    memset(&h, 0, sizeof(h));
    h.magic = IONIC_WIRE_MAGIC;
    h.op = IONIC_WIRE_WRITE;
    h.src_qp_id = QP_A;
    h.dst_qp_id = QP_B;
    h.req_id = 1;
    h.rkey = rkey;
    h.length = length;
    h.remote_va = va;
    ionic_dp_wire_hdr_put(msg, &h);
    for (uint32_t i = 0; i < payload_len; i++)
        msg[IONIC_WIRE_HDR_LEN + i] = (uint8_t)(0xa0u + i);
    mesh_recv(msg, IONIC_WIRE_HDR_LEN + payload_len);
    poll_at(dp, g_now_ms + 1);

    if (g_nwire != 1)
        return "the peer wasn't answered once";
    ionic_dp_wire_hdr_get(&h, g_wire[0]);
    if (h.op != IONIC_WIRE_ACK || h.status != want_status)
        return want_status == IONIC_STS_OK
                   ? "the WRITE wasn't acknowledged"
                   : "the WRITE wasn't refused with a remote access error";
    if (want_status != IONIC_STS_OK)
        return memcmp(before, g_mem + MR_GPA, sizeof(before)) == 0
                   ? NULL
                   : "the refused WRITE changed the region";
    return memcmp(g_mem + va, msg + IONIC_WIRE_HDR_LEN, length) == 0
               ? NULL
               : "the WRITE didn't land";
}

static void check_remote_write_bounds(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint32_t rkey;
        uint64_t va;
        uint32_t length;
        uint32_t payload_len;
        uint32_t status;
    } rows[] = {
        {"positive", "a 64-byte WRITE with 64 bytes of payload",
         "lands, and is acknowledged", MR_KEY, MR_GPA, 64, 64, IONIC_STS_OK},
        {"negative", "a 64-byte WRITE with 32 bytes of payload",
         "refused with a remote access error, and none of it lands", MR_KEY,
         MR_GPA, 64, 32, IONIC_STS_REMOTE_ACC_ERR},
        {"negative", "a 32-byte WRITE with 64 bytes of payload",
         "refused with a remote access error, and none of it lands", MR_KEY,
         MR_GPA, 32, 64, IONIC_STS_REMOTE_ACC_ERR},
        {"boundary", "a WRITE ending at the last byte of the region",
         "lands, and is acknowledged", MR_KEY, MR_GPA + MR_LEN - 64, 64, 64,
         IONIC_STS_OK},
        {"boundary", "a WRITE ending one byte past the region",
         "refused with a remote access error, and none of it lands", MR_KEY,
         MR_GPA + MR_LEN - 63, 64, 64, IONIC_STS_REMOTE_ACC_ERR},
        {"corner",
         "a zero-length WRITE with rkey 0, va 0 and no payload, as UCX's "
         "flush",
         "acknowledged", 0, 0, 0, 0, IONIC_STS_OK},
        {"corner", "a zero-length WRITE with 8 bytes of payload",
         "refused with a remote access error, and none of it lands", MR_KEY,
         MR_GPA, 0, 8, IONIC_STS_REMOTE_ACC_ERR},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why =
            dp ? rw_run(dp, rows[row].rkey, rows[row].va, rows[row].length,
                        rows[row].payload_len, rows[row].status)
               : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * Message sizes at the mesh's edge.  The data path takes the buffer the mesh
 * received a message into rather than copying it, so it has to free it
 * however the message ends: refused for its size, applied by a poll (here
 * dropped, for its zero magic), or still queued when the data path is
 * destroyed.  What the rows can see is whether the message was queued; a
 * buffer that is never freed, or freed twice, is for the sanitizers and
 * valgrind to report.
 */
enum mr_end {
    MR_REFUSED,   /* too short or too long to queue */
    MR_POLLED,    /* queued, then dropped by a poll */
    MR_DESTROYED, /* queued, then freed with the data path */
};

static const char *mr_run(struct ionic_datapath *dp, size_t len,
                          enum mr_end end)
{
    void *buf;

    remote(dp, QP_B);
    if (!g_recv_fn)
        return "the data path registered no mesh receive callback";
    buf = calloc(1, len);
    if (!buf)
        return "no memory for the message";
    if (!g_recv_fn(g_recv_opaque, PEER_NODE, buf, len)) {
        free(buf);
        return "the data path had no room for the message";
    }

    if (ionic_datapath_has_work(dp) != (end != MR_REFUSED))
        return end == MR_REFUSED ? "the message was queued"
                                 : "the message wasn't queued";
    if (end == MR_POLLED) {
        poll_at(dp, g_now_ms + 1);
        if (ionic_datapath_has_work(dp))
            return "the poll left the message queued";
    }
    return NULL;
}

static void check_mesh_recv_sizes(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        size_t len;
        enum mr_end end;
    } rows[] = {
        {"negative", "a message one byte shorter than the wire header",
         "refused, not queued", IONIC_WIRE_HDR_LEN - 1, MR_REFUSED},
        {"boundary", "a message that is just the wire header",
         "queued, then dropped by the next poll", IONIC_WIRE_HDR_LEN,
         MR_POLLED},
        {"boundary", "a message of ERNIC_MESH_MAX_MSG bytes",
         "queued, then dropped by the next poll", ERNIC_MESH_MAX_MSG,
         MR_POLLED},
        {"negative", "a message one byte over ERNIC_MESH_MAX_MSG",
         "refused, not queued", (size_t)ERNIC_MESH_MAX_MSG + 1, MR_REFUSED},
        {"corner", "a message still queued when the data path is destroyed",
         "freed with the data path", IONIC_WIRE_HDR_LEN, MR_DESTROYED},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? mr_run(dp, rows[row].len, rows[row].end)
                             : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * The queue of messages from other nodes holds DP_RX_MAX_MSGS messages and
 * DP_RX_MAX_BYTES bytes.  Past that, the data path refuses a message and
 * leaves it with the mesh, whose receive thread offers it again and reads
 * nothing more from that peer meanwhile.  A pass empties the queue but for
 * the SENDs waiting for a receive, and what is behind them for the same QP;
 * once those have kept a refused message out for DP_RX_CROWDED_MS, a pass
 * gives up on them, as at RNR_RETRY_MS.  A full queue that no pass has
 * ended for over DP_RX_STUCK_MS, with none under way, drops what comes
 * instead, since a receive thread would otherwise wait for it for good; a
 * long pass is still applying what is queued, and doesn't count.
 *
 * Junk (a zero magic) fills the queue where a row needs what a pass
 * consumes; SENDs to QP_B, which has no receive posted, where it needs what
 * a pass leaves.
 */
#define IONIC_STS_RNR_RETRY_EXCEEDED 12

enum rq_scenario {
    RQ_MSGS,         /* DP_RX_MAX_MSGS queued, then one more */
    RQ_BYTES,        /* DP_RX_MAX_BYTES queued, then one more */
    RQ_STUCK_EDGE,   /* full, with no pass for DP_RX_STUCK_MS */
    RQ_STUCK,        /* and for 1 ms more */
    RQ_LONG_PASS,    /* full, during a pass longer than DP_RX_STUCK_MS */
    RQ_CROWDED_EDGE, /* full of SENDs that keep one out, DP_RX_CROWDED_MS - 1 */
    RQ_CROWDED,      /* and for DP_RX_CROWDED_MS */
    RQ_ROOM_LEFT,    /* a SEND waits while junk fills the queue */
};

/*
 * Offer the data path a message of @len bytes, as the mesh's receive thread
 * does: junk, or a SEND from QP_A to QP_B.  1 if it took the message, 0 if it
 * refused it, which leaves the buffer here, or -1 if there was no memory.
 */
static int rq_offer(size_t len, bool send)
{
    uint8_t *buf = calloc(1, len);

    if (!buf)
        return -1;
    if (send) {
        struct ionic_wire_hdr h;

        memset(&h, 0, sizeof(h));
        h.magic = IONIC_WIRE_MAGIC;
        h.op = IONIC_WIRE_SEND;
        h.src_qp_id = QP_A;
        h.dst_qp_id = QP_B;
        h.length = (uint32_t)(len - IONIC_WIRE_HDR_LEN);
        ionic_dp_wire_hdr_put(buf, &h);
    }
    if (g_recv_fn(g_recv_opaque, PEER_NODE, buf, len))
        return 1;
    free(buf);
    return 0;
}

/* Whether the data path takes each of @n messages of @len bytes */
static bool rq_fill(uint32_t n, size_t len, bool send)
{
    for (uint32_t i = 0; i < n; i++)
        if (rq_offer(len, send) != 1)
            return false;
    return true;
}

/* What the queue holds, and how many messages it has dropped */
static void rq_counts(struct ionic_datapath *dp, uint32_t *msgs, size_t *bytes,
                      uint64_t *dropped)
{
    qemu_mutex_lock(&dp->rx_lock);
    *msgs = dp->rx_msgs;
    *bytes = dp->rx_bytes;
    *dropped = dp->rx_dropped;
    qemu_mutex_unlock(&dp->rx_lock);
}

/* Whether the queue is counted as empty */
static bool rq_empty(struct ionic_datapath *dp)
{
    uint32_t msgs;
    size_t bytes;
    uint64_t dropped;

    rq_counts(dp, &msgs, &bytes, &dropped);
    return msgs == 0 && bytes == 0;
}

/* Whether each message sent is an ACK of RNR-retry-exceeded, and there are n */
static bool rq_rnr_acks(size_t n)
{
    if (g_nwire != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        struct ionic_wire_hdr h;

        ionic_dp_wire_hdr_get(&h, g_wire[i]);
        if (h.op != IONIC_WIRE_ACK || h.status != IONIC_STS_RNR_RETRY_EXCEEDED)
            return false;
    }
    return true;
}

static const char *rq_stuck(struct ionic_datapath *dp, bool past)
{
    const size_t hdr = IONIC_WIRE_HDR_LEN;
    uint32_t msgs;
    size_t bytes;
    uint64_t dropped;

    if (!rq_fill(DP_RX_MAX_MSGS, hdr, false))
        return "the queue didn't take DP_RX_MAX_MSGS messages";
    /* setup() put the last pass at T0 */
    g_now_ms = T0 + DP_RX_STUCK_MS + (past ? 1 : 0);
    if (rq_offer(hdr, false) != (past ? 1 : 0))
        return past ? "the message was refused, not dropped"
                    : "the message was taken";
    rq_counts(dp, &msgs, &bytes, &dropped);
    if (msgs != DP_RX_MAX_MSGS || dropped != (past ? 1 : 0))
        return past ? "the message wasn't dropped, once"
                    : "the message was dropped";
    if (!past)
        return NULL;

    poll_at(dp, g_now_ms);
    if (dp->rx_dropped_logged != 1)
        return "the pass didn't log the drop, once";
    if (!rq_fill(DP_RX_MAX_MSGS, hdr, false))
        return "the queue didn't take DP_RX_MAX_MSGS messages after the pass";
    return rq_offer(hdr, false) == 0
               ? NULL
               : "once polled again, a full queue didn't refuse the message";
}

/* What rq_offer() returned for the offer rq_mid_pass() made, or -2 if none */
static int g_mid_pass_offer;

/* Past DP_RX_STUCK_MS into the pass, offer a message, as a receive thread */
static void rq_mid_pass(void)
{
    g_now_ms += DP_RX_STUCK_MS + 1;
    g_mid_pass_offer = rq_offer(IONIC_WIRE_HDR_LEN, false);
}

/*
 * A pass that runs past DP_RX_STUCK_MS.  The peer's WRITE comes first, so
 * the ACK for it is sent, and the offer made, as the pass begins.
 */
static const char *rq_long_pass(struct ionic_datapath *dp)
{
    const size_t hdr = IONIC_WIRE_HDR_LEN;
    uint8_t request[WIRE_BYTES];
    size_t len;
    uint32_t msgs;
    size_t bytes;
    uint64_t dropped;

    post_send(dp, QP_A, &WRITE_OK);
    if (g_nwire != 1)
        return "the WRITE wasn't sent";
    len = g_wire_len[0];
    memcpy(request, g_wire[0], len);
    g_nwire = 0;
    mesh_recv(request, len);
    if (!rq_fill(DP_RX_MAX_MSGS - 1, hdr, false))
        return "the queue didn't take DP_RX_MAX_MSGS messages";

    g_mid_pass_offer = -2;
    g_sendv_hook = rq_mid_pass;
    poll_at(dp, T0 + 1);
    rq_counts(dp, &msgs, &bytes, &dropped);
    if (g_mid_pass_offer == -2)
        return "the WRITE wasn't acknowledged";
    if (g_mid_pass_offer != 0 || dropped != 0)
        return "the message was taken during the pass";
    return rq_offer(hdr, false) == 1
               ? NULL
               : "the message wasn't taken after the pass";
}

static const char *rq_crowded(struct ionic_datapath *dp, bool past)
{
    const size_t hdr = IONIC_WIRE_HDR_LEN;

    /* Four of the largest SENDs: the first waits, the rest behind it */
    if (!rq_fill(DP_RX_MAX_BYTES / ERNIC_MESH_MAX_MSG, ERNIC_MESH_MAX_MSG,
                 true))
        return "the queue didn't take DP_RX_MAX_BYTES of SENDs";
    poll_at(dp, T0);
    if (rq_offer(hdr, false) != 0)
        return "the queue took a message past DP_RX_MAX_BYTES";
    /* The first pass to find the SENDs keeping it out */
    poll_at(dp, T0 + 1);
    poll_at(dp, T0 + DP_RX_CROWDED_MS);
    if (g_nwire != 0)
        return "a SEND was answered before DP_RX_CROWDED_MS";
    if (!past)
        return rq_offer(hdr, false) == 0 ? NULL : "the message was taken";

    poll_at(dp, T0 + 1 + DP_RX_CROWDED_MS);
    if (!rq_rnr_acks(DP_RX_MAX_BYTES / ERNIC_MESH_MAX_MSG))
        return "the SENDs weren't each answered with RNR-retry-exceeded";
    if (!rq_empty(dp))
        return "the queue wasn't left empty";
    return rq_offer(hdr, false) == 1 ? NULL : "the message wasn't taken";
}

static const char *rq_run(struct ionic_datapath *dp, enum rq_scenario scenario)
{
    const size_t hdr = IONIC_WIRE_HDR_LEN;

    remote(dp, QP_B);
    if (!g_recv_fn)
        return "the data path registered no mesh receive callback";

    switch (scenario) {
    case RQ_MSGS:
        if (!rq_fill(DP_RX_MAX_MSGS, hdr, false))
            return "the queue didn't take DP_RX_MAX_MSGS messages";
        if (rq_offer(hdr, false) != 0)
            return "the queue took a message past DP_RX_MAX_MSGS";
        poll_at(dp, g_now_ms + 1);
        if (!rq_empty(dp))
            return "the pass left the queue counted as holding something";
        return rq_offer(hdr, false) == 1 ? NULL : "a pass didn't make room";
    case RQ_BYTES:
        /* Three of the largest, and two that come to the fourth exactly */
        if (!rq_fill(3, ERNIC_MESH_MAX_MSG, false) ||
            rq_offer(ERNIC_MESH_MAX_MSG - hdr, false) != 1 ||
            rq_offer(hdr, false) != 1)
            return "the queue didn't take DP_RX_MAX_BYTES";
        if (rq_offer(hdr, false) != 0)
            return "the queue took a message past DP_RX_MAX_BYTES";
        poll_at(dp, g_now_ms + 1);
        if (!rq_empty(dp))
            return "the pass left the queue counted as holding something";
        return rq_offer(hdr, false) == 1 ? NULL : "a pass didn't make room";
    case RQ_STUCK_EDGE:
        return rq_stuck(dp, false);
    case RQ_STUCK:
        return rq_stuck(dp, true);
    case RQ_LONG_PASS:
        return rq_long_pass(dp);
    case RQ_CROWDED_EDGE:
        return rq_crowded(dp, false);
    case RQ_CROWDED:
        return rq_crowded(dp, true);
    case RQ_ROOM_LEFT:
    default:
        if (rq_offer(hdr, true) != 1)
            return "the queue didn't take the SEND";
        poll_at(dp, T0);
        if (!rq_fill(DP_RX_MAX_MSGS - 1, hdr, false))
            return "the queue didn't take the rest of DP_RX_MAX_MSGS";
        if (rq_offer(hdr, false) != 0)
            return "the queue took a message past DP_RX_MAX_MSGS";
        poll_at(dp, T0 + 1);
        poll_at(dp, T0 + 1 + DP_RX_CROWDED_MS);
        if (g_nwire != 0)
            return "the SEND was answered";
        return rq_offer(hdr, false) == 1 ? NULL : "the message wasn't taken";
    }
}

static void check_mesh_recv_room(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        enum rq_scenario scenario;
    } rows[] = {
        {"positive", "DP_RX_MAX_MSGS messages, then one more",
         "the queue takes them all, refuses the next, leaving it with the "
         "mesh, and takes it after a pass",
         RQ_MSGS},
        {"boundary",
         "messages that come to DP_RX_MAX_BYTES exactly, then one more",
         "the queue takes them all, refuses the next, and takes it after a "
         "pass",
         RQ_BYTES},
        {"boundary", "a full queue, with no pass for DP_RX_STUCK_MS",
         "the message is refused, not dropped", RQ_STUCK_EDGE},
        {"negative", "a full queue, with no pass for just over DP_RX_STUCK_MS",
         "the message is dropped and the next pass logs it; once polled, a "
         "full queue refuses again",
         RQ_STUCK},
        {"corner",
         "a full queue, while a pass runs for longer than DP_RX_STUCK_MS",
         "the message is refused, not dropped: the pass is applying what is "
         "queued, and the queue takes it once the pass is done",
         RQ_LONG_PASS},
        {"boundary",
         "SENDs waiting for a receive fill the queue, and keep a message out "
         "for DP_RX_CROWDED_MS - 1",
         "none is answered, and the message is still refused", RQ_CROWDED_EDGE},
        {"positive",
         "SENDs waiting for a receive fill the queue, and keep a message out "
         "for DP_RX_CROWDED_MS",
         "each fails with RNR-retry-exceeded, the queue empties, and the "
         "message is taken",
         RQ_CROWDED},
        {"corner",
         "a SEND waits for a receive while other messages fill the queue",
         "it keeps waiting past DP_RX_CROWDED_MS: a pass leaves room for the "
         "message refused",
         RQ_ROOM_LEFT},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? rq_run(dp, rows[row].scenario)
                             : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * The replies a node asks one peer for at once come to no more than
 * ERNIC_MESH_REPLY_BUDGET, headers included, since the peer drops a reply it
 * has no room to queue.  A request that would pass it waits, as one the mesh
 * has no room for does, until enough replies have come back, or the requests
 * have been given up on.  A READ's reply is as long as the READ, an atomic's
 * 8 bytes, and the rest bare headers; a READ longer than any reply is
 * answered with a bare header too.
 *
 * Each READ of BIG_READ bytes asks for a reply of ERNIC_MESH_MAX_MSG, so two
 * come to the budget exactly.  Nothing answers unless a row does, and the
 * READs are never gathered, so they needn't fit guest memory.
 */
#define WIRE_HDR   ((uint32_t)IONIC_WIRE_HDR_LEN)
#define BIG_READ   (ERNIC_MESH_MAX_MSG - WIRE_HDR)
#define OTHER_NODE 2u

_Static_assert(2 * ERNIC_MESH_MAX_MSG == ERNIC_MESH_REPLY_BUDGET,
               "two BIG_READs no longer fill the reply budget exactly");

enum rb_scenario {
    RB_FULL,       /* two READs whose replies fill the budget */
    RB_OVER,       /* and a WRITE behind them */
    RB_ANSWERED,   /* and one READ answered */
    RB_DEADLINE,   /* and the READs unanswered at PENDING_MS */
    RB_EXPIRED,    /* and just past it */
    RB_FLUSHED,    /* the READs' QP flushed, another QP's WRITE waiting */
    RB_DESTROYED,  /* the READs' QP destroyed, another QP's WRITE waiting */
    RB_OTHER_NODE, /* another QP's WRITE to another node */
    RB_ATOMIC,     /* READs and an atomic that fill it, then a WRITE */
    RB_OVERLONG,   /* a READ longer than any reply among them */
    RB_HUGE,       /* a READ of UINT32_MAX bytes, alone */
};

/* QP_A, whose peer is on node 1, posts a READ of @len bytes */
static void rb_read(struct ionic_datapath *dp, uint32_t len)
{
    struct wqe w = READ_OK;

    w.len = len;
    post_send(dp, QP_A, &w);
}

/* QP_A's two READs fill node 1's budget, then QP_B posts a WRITE to @node */
static void rb_other_qp(struct ionic_datapath *dp, uint32_t node)
{
    ionic_datapath_set_dest(dp, QP_B, QP_A, node);
    rb_read(dp, BIG_READ);
    rb_read(dp, BIG_READ);
    post_send(dp, QP_B, &WRITE_OK);
}

/* Node 1 answers the request at g_wire[@i], a READ, with a bare error */
static void rb_answer(struct ionic_datapath *dp, size_t i)
{
    struct ionic_wire_hdr request, reply;
    uint8_t reply_wire[IONIC_WIRE_HDR_LEN];

    ionic_dp_wire_hdr_get(&request, g_wire[i]);
    memset(&reply, 0, sizeof(reply));
    reply.magic = request.magic;
    reply.op = IONIC_WIRE_READ_RESP;
    reply.src_qp_id = request.dst_qp_id;
    reply.dst_qp_id = request.src_qp_id;
    reply.req_id = request.req_id;
    reply.status = IONIC_STS_REMOTE_ACC_ERR;
    ionic_dp_wire_hdr_put(reply_wire, &reply);
    mesh_recv(reply_wire, sizeof(reply_wire));
    poll_at(dp, g_now_ms + 1);
}

/* Whether one message has been sent since there were @before, a WRITE */
static bool rb_write_sent(size_t before)
{
    struct ionic_wire_hdr h;

    if (g_nwire != before + 1)
        return false;
    ionic_dp_wire_hdr_get(&h, g_wire[before]);
    return h.op == IONIC_WIRE_WRITE;
}

static const char *rb_run(struct ionic_datapath *dp, enum rb_scenario scenario)
{
    remote(dp, QP_B);
    switch (scenario) {
    case RB_FULL:
        rb_read(dp, BIG_READ);
        rb_read(dp, BIG_READ);
        return g_nwire == 2 && !dp->qp[QP_A].sq_busy
                   ? NULL
                   : "the READs weren't both sent";
    case RB_OVER:
    case RB_ANSWERED:
    case RB_DEADLINE:
    case RB_EXPIRED:
        rb_read(dp, BIG_READ);
        rb_read(dp, BIG_READ);
        post_send(dp, QP_A, &WRITE_OK);
        poll_at(dp, T0 + 1);
        if (g_nwire != 2 || !dp->qp[QP_A].sq_busy)
            return "the WRITE didn't wait";
        break;
    case RB_FLUSHED:
    case RB_DESTROYED:
        rb_other_qp(dp, PEER_NODE);
        if (g_nwire != 2 || !dp->qp[QP_B].sq_busy)
            return "QP_B's WRITE didn't wait behind QP_A's READs";
        if (scenario == RB_FLUSHED)
            ionic_datapath_flush_qp(dp, QP_A);
        else
            ionic_datapath_unregister_qp(dp, QP_A);
        poll_at(dp, T0 + 1);
        return rb_write_sent(2) ? NULL : "QP_B's WRITE wasn't sent";
    case RB_OTHER_NODE:
        rb_other_qp(dp, OTHER_NODE);
        return rb_write_sent(2) && !dp->qp[QP_B].sq_busy
                   ? NULL
                   : "QP_B's WRITE to node 2 waited for node 1's READs";
    case RB_ATOMIC:
        rb_read(dp, BIG_READ);
        rb_read(dp, BIG_READ - 8 - WIRE_HDR);
        post_send(dp, QP_A, &FADD_OK);
        if (g_nwire != 3)
            return "the READs and the atomic weren't all sent";
        post_send(dp, QP_A, &WRITE_OK);
        return g_nwire == 3 && dp->qp[QP_A].sq_busy ? NULL
                                                    : "the WRITE didn't wait";
    case RB_OVERLONG:
        rb_read(dp, BIG_READ);
        rb_read(dp, BIG_READ + 1);
        if (g_nwire != 2)
            return "the READs weren't both sent";
        rb_read(dp, BIG_READ);
        return g_nwire == 2 && dp->qp[QP_A].sq_busy
                   ? NULL
                   : "the third READ didn't wait";
    case RB_HUGE:
    default:
        rb_read(dp, UINT32_MAX);
        return g_nwire == 1 ? NULL : "the READ wasn't sent";
    }

    /* The WRITE is waiting behind the READs */
    if (scenario == RB_ANSWERED) {
        rb_answer(dp, 0);
        return rb_write_sent(2) ? NULL
                                : "the WRITE wasn't sent in the pass that "
                                  "applied the READ's answer";
    }
    if (scenario == RB_DEADLINE) {
        poll_at(dp, T0 + PENDING_MS);
        poll_at(dp, T0 + PENDING_MS);
        return g_nwire == 2 ? NULL : "the WRITE was sent";
    }
    if (scenario == RB_EXPIRED) {
        poll_at(dp, T0 + PENDING_MS + 1);
        poll_at(dp, T0 + PENDING_MS + 1);
        return rb_write_sent(2) ? NULL : "the WRITE wasn't sent";
    }
    poll_at(dp, T0 + 2);
    return g_nwire == 2 ? NULL : "the WRITE was sent on a later pass";
}

static void check_reply_budget(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        enum rb_scenario scenario;
    } rows[] = {
        {"boundary",
         "two READs whose replies come to ERNIC_MESH_REPLY_BUDGET exactly",
         "both are sent", RB_FULL},
        {"positive", "a WRITE behind them, whose reply is a bare ACK",
         "it waits, on every pass, and isn't sent", RB_OVER},
        {"positive", "one READ answered",
         "the WRITE is sent in the pass that applies the answer", RB_ANSWERED},
        {"boundary", "the READs still unanswered at PENDING_MS",
         "the WRITE still waits", RB_DEADLINE},
        {"positive", "the READs given up on just past PENDING_MS",
         "the WRITE is sent", RB_EXPIRED},
        {"positive",
         "the READs' QP flushed, with another QP's WRITE to the same node "
         "waiting",
         "the WRITE is sent", RB_FLUSHED},
        {"positive",
         "the READs' QP destroyed, with another QP's WRITE to the same node "
         "waiting",
         "the WRITE is sent", RB_DESTROYED},
        {"corner", "another QP's WRITE to another node",
         "it is sent at once: the budget is per node", RB_OTHER_NODE},
        {"boundary",
         "READs and a fetch-and-add whose replies, the atomic's 8 bytes "
         "included, come to the budget exactly, then a WRITE",
         "all but the WRITE are sent", RB_ATOMIC},
        {"corner",
         "a READ of BIG_READ, one a byte longer than any reply, then another "
         "of BIG_READ",
         "the first two are sent, the longer counted as a bare header, and "
         "the third waits",
         RB_OVERLONG},
        {"corner", "a READ of UINT32_MAX bytes, with nothing else pending",
         "it is sent: a READ alone never waits for good", RB_HUGE},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? rb_run(dp, rows[row].scenario)
                             : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

/*
 * A send WQE the device can't read completes in error, named by its slot,
 * and the QP is flushed, but only once the requests posted before it have
 * completed. The drivers retire a send queue in order, so while an earlier
 * READ waits for its answer, a flush would have them flush the READ and the
 * WQE both, and report no error; and the flush takes the READ back, so its
 * answer could never complete it. Each row's completions are compared, in
 * order, with what the drivers need to report the READ, then the error, then
 * a flush of whatever is left.
 */
enum uw_scenario {
    UW_ALONE,    /* nothing posted before it */
    UW_ANSWERED, /* a remote READ before it, which is answered */
    UW_DEADLINE, /* the READ unanswered at PENDING_MS */
    UW_EXPIRED,  /* and just past it */
    UW_FLUSHED,  /* QP_A flushed while it waits, then the READ answered */
};

/*
 * A completion on CQ_ID for QP_A. @id is a SEND_MSN's MSN, or the wqe_id of
 * an NPG completion that isn't a flush.
 */
struct uw_cqe {
    uint32_t type;
    uint32_t status;
    uint64_t id;
};

#define UW_MAX_CQES 4

/* Whether CQ_ID holds exactly @n completions, and they are @want */
static const char *uw_cqes(const struct ionic_datapath *dp,
                           const struct uw_cqe *want, uint32_t n)
{
    static char why[128];

    if (dp->cq[CQ_ID].prod != n) {
        snprintf(why, sizeof(why), "%" PRIu32 " completions, not %" PRIu32,
                 dp->cq[CQ_ID].prod, n);
        return why;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *c = g_mem + CQ_GPA + ((size_t)i << CQE_STRIDE);
        uint32_t qtf = ernic_get_be32(c + CQE_QTF_OFF);
        uint32_t status = ernic_get_be32(c + CQE_STATUS_OFF);
        bool error = (qtf & CQE_ERROR_BIT) != 0;
        uint64_t id = 0;

        if (want[i].type == CQE_TYPE_SEND_MSN)
            id = ernic_get_be32(c + CQE_MSN_OFF);
        else if (want[i].type == CQE_TYPE_SEND_NPG &&
                 want[i].status != IONIC_STS_WQE_FLUSHED_ERR)
            memcpy(&id, c + CQE_NPG_WQE_OFF, sizeof(id));

        if ((qtf & CQE_TYPE_MASK) != want[i].type || qtf >> 8 != QP_A ||
            status != want[i].status ||
            error != (want[i].status != IONIC_STS_OK) || id != want[i].id) {
            snprintf(why, sizeof(why),
                     "completion %" PRIu32 " is type %" PRIu32 " qid %" PRIu32
                     " status %" PRIu32 " (error %d) id %" PRIu64,
                     i, (qtf & CQE_TYPE_MASK) >> 5, qtf >> 8, status, error,
                     id);
            return why;
        }
    }
    return NULL;
}

static const char *uw_run(struct ionic_datapath *dp, enum uw_scenario scenario)
{
    struct uw_cqe want[UW_MAX_CQES];
    uint32_t n = 0;
    /* The slot of the WQE that can't be read: after the READ's, if any */
    uint32_t slot = scenario == UW_ALONE ? 0 : 1;
    const char *why;

    remote(dp, QP_B);
    if (scenario != UW_ALONE) {
        post_send(dp, QP_A, &READ_OK);
        if (g_nwire != 1)
            return "the READ wasn't sent";
    }
    g_hole_gpa = SQ_GPA(QP_A) + ((uint64_t)slot << WQE_STRIDE);
    post_send(dp, QP_A, &WRITE_OK);

    if (scenario != UW_ALONE) {
        /*
         * The guest maps the slot back and posts another WRITE. Neither
         * runs: the WQE that couldn't be read has ended the queue.
         */
        g_hole_gpa = UINT64_MAX;
        post_send(dp, QP_A, &WRITE_OK);
        if (dp->cq[CQ_ID].prod != 0)
            return "something completed while the READ waited";
        if (g_nwire != 1)
            return "a WRITE behind the READ was sent";
    }

    switch (scenario) {
    case UW_ALONE:
        break;
    case UW_ANSWERED:
        pump(dp);
        want[n++] = (struct uw_cqe){CQE_TYPE_SEND_MSN, IONIC_STS_OK, 1};
        break;
    case UW_DEADLINE:
        g_nwire = 0;
        poll_at(dp, T0 + PENDING_MS);
        return uw_cqes(dp, NULL, 0);
    case UW_EXPIRED:
        g_nwire = 0;
        poll_at(dp, T0 + PENDING_MS + 1);
        want[n++] =
            (struct uw_cqe){CQE_TYPE_SEND_MSN, IONIC_STS_RETRY_EXCEEDED, 1};
        break;
    case UW_FLUSHED:
    default:
        /* The flush takes the READ back, so nothing completes for it */
        ionic_datapath_flush_qp(dp, QP_A);
        pump(dp);
        want[n++] =
            (struct uw_cqe){CQE_TYPE_SEND_NPG, IONIC_STS_WQE_FLUSHED_ERR, 0};
        want[n++] =
            (struct uw_cqe){CQE_TYPE_RECV, IONIC_STS_WQE_FLUSHED_ERR, 0};
        return uw_cqes(dp, want, n);
    }

    want[n++] =
        (struct uw_cqe){CQE_TYPE_SEND_NPG, IONIC_STS_LOCAL_QP_OPER_ERR, slot};
    want[n++] =
        (struct uw_cqe){CQE_TYPE_SEND_NPG, IONIC_STS_WQE_FLUSHED_ERR, 0};
    want[n++] = (struct uw_cqe){CQE_TYPE_RECV, IONIC_STS_WQE_FLUSHED_ERR, 0};
    why = uw_cqes(dp, want, n);
    if (!why &&
        dp->qp[QP_A].sq_cons % dp->qp[QP_A].sq_depth != dp->qp[QP_A].sq_prod)
        why = "the flush didn't empty the send queue";
    return why;
}

static void check_unreadable_wqe(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        enum uw_scenario scenario;
    } rows[] = {
        {"positive", "a send WQE the device can't read, with nothing before it",
         "it completes at once with LOCAL_QP_OPER_ERR, by its slot, then "
         "both queues are flushed",
         UW_ALONE},
        {"positive",
         "a send WQE the device can't read, behind a remote READ that is "
         "then answered",
         "nothing completes or is sent until the answer; then the READ "
         "completes, then the WQE in error, then both flushes",
         UW_ANSWERED},
        {"boundary", "the READ before it still unanswered at PENDING_MS",
         "nothing completes", UW_DEADLINE},
        {"positive", "the READ before it given up on just past PENDING_MS",
         "the READ completes with RETRY_EXCEEDED, then the WQE in error, "
         "then both flushes",
         UW_EXPIRED},
        {"corner", "QP_A flushed while the WQE waits, then the READ answered",
         "only the two flushes: the READ was taken back, and the WQE is "
         "flushed with the rest",
         UW_FLUSHED},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_datapath *dp = setup();
        const char *why = dp ? uw_run(dp, rows[row].scenario)
                             : "could not create a data path";

        ionic_datapath_destroy(dp);
        printf("%s %s: %s; expected: %s\n", why ? "FAIL" : "ok  ",
               rows[row].category, rows[row].description, rows[row].expected);
        if (why) {
            printf("     %s\n", why);
            failures++;
        }
    }
}

int main(void)
{
    check_wire_hdr_layout();
    check_atomic_byte_order();
    check_late_responses();
    check_mesh_cancel();
    check_destroyed_qp_replies();
    check_remote_write_bounds();
    check_mesh_recv_sizes();
    check_mesh_recv_room();
    check_reply_budget();
    check_unreadable_wqe();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    for (size_t i = 0; i < sizeof(getter_cases) / sizeof(getter_cases[0]); i++)
        run_getter_case(&getter_cases[i]);
    check_reset_counters();
    for (size_t i = 0; i < sizeof(sq_cases) / sizeof(sq_cases[0]); i++)
        run_sq_case(&sq_cases[i]);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
