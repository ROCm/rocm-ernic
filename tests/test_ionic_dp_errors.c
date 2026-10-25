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

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ionic_datapath.h"
#include "ionic_datapath_priv.h"
#include "ionic_eth_emu.h"
#include "nvmeof_cm.h"
#include "nvmeof_target.h"
#include "rocm_ernic_compat.h"
#include "s3_tcp.h"
#include "s3_target.h"

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

#define IONIC_STS_OK             0
#define IONIC_STS_REMOTE_ACC_ERR 9

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
 * would.
 */

#define GUEST_MEM_SIZE 0x10000u
static uint8_t g_mem[GUEST_MEM_SIZE];

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
 * What the mesh says: whether ionic_mesh_has_room() finds room to queue a
 * request, and, unless it is 0, what ionic_mesh_sendv() returns instead of
 * taking the message.
 */
static bool g_room;
static int g_send_rc;

int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply)
{
    (void)handle;
    (void)dst_node;
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
static ionic_mesh_recv_fn g_recv_fn;
static void *g_recv_opaque;

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle)
{
    (void)handle;
    return g_local_node;
}
uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle, const uint8_t *gid)
{
    (void)handle;
    (void)gid;
    return UINT32_MAX;
}
bool ionic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len)
{
    (void)handle;
    (void)dst_node;
    (void)len;
    return g_room;
}
void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque)
{
    (void)handle;
    g_recv_fn = fn;
    g_recv_opaque = opaque;
}

struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
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

struct s3_target *s3_target_create(const struct s3_target_cfg *cfg, char *err,
                                   size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
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

void pvrdma_qp_cqe_count(pvrdma_handle_t handle, uint32_t qp_id)
{
    (void)handle;
    (void)qp_id;
}
void pvrdma_qp_doorbell_count(pvrdma_handle_t handle, uint32_t qp_id,
                              bool is_send)
{
    (void)handle;
    (void)qp_id;
    (void)is_send;
}
void pvrdma_qp_wqe_count(pvrdma_handle_t handle, uint32_t qp_id,
                         unsigned int pvrdma_opcode)
{
    (void)handle;
    (void)qp_id;
    (void)pvrdma_opcode;
}
void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op op)
{
    (void)handle;
    (void)qp_id;
    (void)bytes;
    (void)op;
}


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
#define SQ_GPA(qp) (0x1000u * ((qp) - 1u))
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

void ionic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask)
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
    ionic_datapath_register_qp(dp, qp_id, type, cq_id, &sq, cq_id, &rq);
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
        .stride_log2 = 5,
    };
    struct ionic_dp_buf_desc mr = {.dma_addr = MR_GPA, .map_count = 1};

    if (!dp)
        return NULL;
    memset(g_mem, 0, sizeof(g_mem));
    for (size_t i = 0; i < MR_LEN; i++)
        g_mem[SRC_GPA + i] = (uint8_t)i;
    g_nwire = 0;
    g_room = true;
    g_send_rc = 0;
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

    ionic_datapath_register_cq(dp, CQ_ID, 0, &cq);
    register_qp(dp, QP_A, CQ_ID);
    register_qp(dp, QP_B, CQ_ID);
    register_qp(dp, QP_C, CQ_UNKNOWN);
    ionic_datapath_set_dest(dp, QP_A, QP_B, UINT32_MAX);
    ionic_datapath_set_dest(dp, QP_B, QP_A, UINT32_MAX);
    ionic_datapath_register_mr(dp, MR_KEY, MR_GPA, MR_LEN, &mr);
    return dp;
}

/* QP_A's peer becomes QP @dest_qp on node 1, so its work goes to the mesh. */
static void remote(struct ionic_datapath *dp, uint32_t dest_qp)
{
    g_local_node = LOCAL_NODE;
    ionic_datapath_set_pvrdma(dp, &g_mesh);
    ionic_datapath_set_dest(dp, QP_A, dest_qp, PEER_NODE);
}

/* Deliver @len bytes from the peer node, as the mesh's receive thread does. */
static void mesh_recv(const void *buf, size_t len)
{
    if (!g_recv_fn) {
        printf("FAIL: the data path registered no mesh receive callback\n");
        failures++;
        return;
    }
    g_recv_fn(g_recv_opaque, PEER_NODE, buf, len);
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
        uint64_t before[4][IONIC_DP_RDMA_COUNT], after[IONIC_DP_RDMA_COUNT];
        bool ok = dp != NULL;

        if (dp) {
            remote(dp, QP_B);
            post_send(dp, QP_A, &SEND64);
            ok = g_nwire == 1;
            if (ok) {
                memcpy(&request, g_wire[0], sizeof(request));
                reply.magic = request.magic;
                reply.op = IONIC_WIRE_ACK;
                reply.src_qp_id = request.dst_qp_id;
                reply.dst_qp_id = request.src_qp_id;
                reply.req_id = request.req_id;
                reply.status = htole32(rows[row].status);
                g_nwire = 0;
                if (rows[row].expire)
                    poll_at(dp, T0 + PENDING_MS + 1);
                else {
                    mesh_recv(&reply, sizeof(reply));
                    poll_at(dp, T0 + 1);
                }
                ok = dp->cq[CQ_ID].prod == 1;
                for (size_t v = 0; v < 4; v++)
                    ionic_datapath_rdma_counters(dp, views[v], before[v]);
                for (unsigned int repeat = 0; repeat < 2; repeat++) {
                    mesh_recv(&reply, sizeof(reply));
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
 * requests complete nothing, and still go out.
 */
enum mc_scenario {
    MC_TAGGED,     /* a remote WRITE and its ACK */
    MC_FLUSH,      /* QP_A flushed with its WRITE outstanding */
    MC_UNREGISTER, /* QP_A destroyed with its WRITE outstanding */
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
    memcpy(&h, g_wire[0], sizeof(h));
    tag = (uint64_t)QP_A << 32 | le32toh(h.req_id);
    if (g_wire_tag[0] != tag)
        return "the WRITE isn't called by QP_A and its request id";

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
         "the WRITE goes to the mesh as a request called by QP_A and its "
         "request id, the ACK as a reply, and nothing is taken back",
         MC_TAGGED},
        {"positive", "QP_A flushed while its WRITE is outstanding",
         "the mesh takes back QP_A's requests once, before the flush "
         "completions are posted",
         MC_FLUSH},
        {"corner", "QP_A destroyed while its WRITE is outstanding",
         "nothing is taken back: the WRITE still goes out, as a consumer "
         "that destroys a QP straight after an unsignaled WRITE counts on",
         MC_UNREGISTER},
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

int main(void)
{
    check_late_responses();
    check_mesh_cancel();
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
