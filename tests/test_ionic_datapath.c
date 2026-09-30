/*
 * Unit tests for the ionic data path's handling of guest work requests.
 *
 * These drive the doorbell path against a fake guest memory, so a WQE the
 * test writes is parsed, executed and completed exactly as one the guest
 * driver posts.
 *
 * UD and GSI routing.  A UD or GSI work request names its destination only
 * by an address handle and a remote QPN in the WQE.  The data path used to
 * ignore both: with no RC-style dest_qp_id on the QP it fell back to
 * delivering the SEND into the sender's own receive queue, and it copied the
 * payload to offset 0 of the receive buffer with no global route header in
 * front of it.  The guest's ib_mad reads the MAD at offset 40, so every CM
 * REQ an rdma_cm client sent came straight back to itself as "MAD received
 * with unsupported base version" -- which is why nvme connect -t rdma, rping
 * and every other rdma_cm user failed between two guests.  The tests pin that
 * the SEND lands on the QP the WQE names, on the node the AH's destination
 * GID names; that the receive carries a GRH the guest can build a reply
 * address from; and that the completion reports the sender.
 *
 * The TU is #included to reach the private state; its external symbols are
 * stubbed, with the mesh transmit captured so a message can be carried by
 * hand from one data path instance to another.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "ionic_datapath.c"

/* ---- Fake guest memory -------------------------------------------------
 *
 * A "GPA" is an offset into this buffer.  Only the DMA entry points are
 * stubbed; the data path's own address arithmetic runs unmodified.
 */
#define GUEST_MEM_SIZE (1u << 16)
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
    if (max_nr_sgs < 1 || addr > GUEST_MEM_SIZE || len > GUEST_MEM_SIZE - addr)
        return -1;
    sg->addr = addr;
    sg->len = len;
    return 1;
}

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    struct fake_sg *sg = (struct fake_sg *)sgl;

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

/* ---- Mesh stubs --------------------------------------------------------
 *
 * Resolution is an exact match, as in the TCP backend: node 0 advertised
 * 192.168.200.10 and node 1 advertised 192.168.200.20, and no other GID
 * names a node.
 */
static uint8_t g_tx[4096];
static size_t g_tx_len;
static uint32_t g_tx_node;
static unsigned g_tx_count;

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle)
{
    (void)handle;
    return UINT32_MAX;
}

uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle, const uint8_t *gid)
{
    static const uint8_t v4_prefix[14] = {0, 0, 0, 0,    0,    0,   0,
                                          0, 0, 0, 0xff, 0xff, 192, 168};
    (void)handle;
    if (!gid || memcmp(gid, v4_prefix, sizeof(v4_prefix)) || gid[14] != 200)
        return UINT32_MAX;
    if (gid[15] == 10)
        return 0;
    if (gid[15] == 20)
        return 1;
    return UINT32_MAX;
}

int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len)
{
    (void)handle;
    g_tx_count++;
    if (hdr_len + body_len > sizeof(g_tx))
        return -1;
    memcpy(g_tx, hdr, hdr_len);
    if (body_len)
        memcpy(g_tx + hdr_len, body, body_len);
    g_tx_len = hdr_len + body_len;
    g_tx_node = dst_node;
    return 0;
}

void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque)
{
    (void)handle;
    (void)fn;
    (void)opaque;
}

/* ---- Stubs for the TU's other external symbols -------------------------
 *
 * None of these are reachable from the UD path.
 */
int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    (void)emu;
    (void)vec;
    return 0;
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

static int g_failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        g_failures++;
}

/*
 * Guest memory layout.  Each data path instance gets its own 32 KiB half so
 * two of them can stand for the two guests of a mesh.  Rings are single
 * directly-addressed pages; lkey 0 makes every SGE a bus address.
 */
#define HALF        0x8000u
#define CQ_OFF      0x0000u /* one CQ, shared by every QP's SQ and RQ */
#define SQ_OFF(qp)  (0x1000u + (qp) * 0x1000u)
#define RQ_OFF(qp)  (0x4000u + (qp) * 0x1000u)
#define PLD_OFF     0x7000u
#define RBUF_OFF(n) (0x7400u + (n) * 0x200u)

#define CQ_ID       1
#define STRIDE_LOG2 8 /* 256-byte WQEs: room for a SPEC16 list */
#define DEPTH_LOG2  4
#define CQ_DEPTH    64

#define QPT_GSI 1
#define QPT_UD  4
#define QP_GSI  1 /* the driver's GSI QP is always qpid 1 */
#define QP_UD_A 2
#define QP_UD_B 3

#define AH_ID    9
#define PAYLOAD  256 /* one MAD */
#define RBUF_LEN (IB_GRH_SIZE + PAYLOAD)

#define QKEY_GSI 0x80010000u

struct inst {
    struct ionic_datapath *dp;
    uint32_t base; /* this instance's half of g_mem */
    uint32_t cq_seen;
    uint16_t sq_prod[4];
    uint16_t rq_prod[4];
};

static struct inst g_a, g_b;

static void inst_init(struct inst *in, uint32_t base, uint32_t node)
{
    memset(in, 0, sizeof(*in));
    in->base = base;
    in->dp = ionic_datapath_create(NULL, NULL);
    in->dp->local_node = node;
    /* Only checked for non-NULL; every consumer of it is stubbed. */
    in->dp->pvrdma_handle = (pvrdma_handle_t)in;

    struct ionic_dp_ring_desc cq = {
        .buf = {.dma_addr = base + CQ_OFF, .map_count = 1},
        .depth_log2 = 6,
        .stride_log2 = 5,
    };
    ionic_datapath_register_cq(in->dp, CQ_ID, 0, &cq);

    for (uint32_t qp = QP_GSI; qp <= QP_UD_B; qp++) {
        struct ionic_dp_ring_desc sq = {
            .buf = {.dma_addr = base + SQ_OFF(qp - 1), .map_count = 1},
            .depth_log2 = DEPTH_LOG2,
            .stride_log2 = STRIDE_LOG2,
        };
        struct ionic_dp_ring_desc rq = {
            .buf = {.dma_addr = base + RQ_OFF(qp - 1), .map_count = 1},
            .depth_log2 = DEPTH_LOG2,
            .stride_log2 = STRIDE_LOG2,
        };
        ionic_datapath_register_qp(in->dp, qp, qp == QP_GSI ? QPT_GSI : QPT_UD,
                                   CQ_ID, &sq, CQ_ID, &rq);
    }
}

static void inst_fini(struct inst *in)
{
    ionic_datapath_destroy(in->dp);
    in->dp = NULL;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    v = htobe32(v);
    memcpy(p, &v, 4);
}

static void put_sge(uint8_t *p, uint64_t va, uint32_t len)
{
    uint64_t be_va = htobe64(va);
    memcpy(p, &be_va, 8);
    put_be32(p + 8, len);
    put_be32(p + 12, 0); /* IONIC_DMA_LKEY */
}

/*
 * Describe @total bytes at @va as @nsge SGEs, laid out the way the driver's
 * ionic_prep_pld() does for a kernel QP: past IONIC_V1_SPEC_FIRST_SGE (2)
 * SGEs it sets SPEC32 (or SPEC16 above 8), puts a table of lengths in the
 * first 32 bytes of the payload, and starts the SGEs after it.
 */
static void put_pld(uint8_t *w, uint64_t va, uint32_t total, uint32_t nsge)
{
    uint8_t *sgl = w + WQE_PLD_OFF;
    uint16_t flags;

    memcpy(&flags, w + 10, 2);
    flags = be16toh(flags);
    if (nsge > 2) {
        flags |= nsge > 8 ? IONIC_V1_FLAG_SPEC16 : IONIC_V1_FLAG_SPEC32;
        sgl += 32;
    }
    flags = htobe16(flags);
    memcpy(w + 10, &flags, 2);
    w[9] = (uint8_t)nsge;

    uint32_t chunk = total / nsge;
    for (uint32_t i = 0; i < nsge; i++) {
        uint32_t len = i + 1 < nsge ? chunk : total - chunk * (nsge - 1);
        put_sge(sgl + i * 16, va, len);
        if (nsge > 8) {
            uint16_t l16 = htobe16((uint16_t)len);
            memcpy(w + WQE_PLD_OFF + i * 2, &l16, 2);
        } else if (nsge > 2) {
            put_be32(w + WQE_PLD_OFF + i * 4, len);
        }
        va += len;
    }
}

/* Post one receive of RBUF_LEN bytes into buffer @n, split into @nsge SGEs. */
static void post_recv_sges(struct inst *in, uint32_t qp, uint32_t n,
                           uint32_t nsge)
{
    uint16_t slot = in->rq_prod[qp] % (1u << DEPTH_LOG2);
    uint8_t *w = g_mem + in->base + RQ_OFF(qp - 1) + slot * (1u << STRIDE_LOG2);

    memset(w, 0, 1u << STRIDE_LOG2);
    uint64_t wqe_id = slot;
    memcpy(w, &wqe_id, 8);
    put_be32(w + 12, RBUF_LEN);
    put_pld(w, in->base + RBUF_OFF(n), RBUF_LEN, nsge);
    memset(g_mem + in->base + RBUF_OFF(n), 0xee, RBUF_LEN);

    in->rq_prod[qp]++;
    ionic_datapath_doorbell(in->dp, DP_QTYPE_RQ,
                            (uint64_t)in->rq_prod[qp] |
                                ((uint64_t)(qp & 0xffu) << 24));
}

static void post_recv(struct inst *in, uint32_t qp, uint32_t n)
{
    post_recv_sges(in, qp, n, 1);
}

/*
 * Post a signalled UD SEND of PAYLOAD bytes, gathered from @nsge SGEs,
 * through @ah_id to @dest_qpn.
 */
static void post_ud_send_sges(struct inst *in, uint32_t qp, uint32_t ah_id,
                              uint32_t dest_qpn, uint32_t nsge)
{
    uint16_t slot = in->sq_prod[qp] % (1u << DEPTH_LOG2);
    uint8_t *w = g_mem + in->base + SQ_OFF(qp - 1) + slot * (1u << STRIDE_LOG2);

    memset(w, 0, 1u << STRIDE_LOG2);
    uint64_t wqe_id = slot;
    memcpy(w, &wqe_id, 8);
    w[8] = IONIC_V1_OP_SEND;
    uint16_t flags = htobe16(IONIC_V1_FLAG_SIG);
    memcpy(w + 10, &flags, 2);
    put_be32(w + 16, ah_id);
    put_be32(w + 20, dest_qpn);
    put_be32(w + 24, QKEY_GSI);
    put_be32(w + WQE_SEND_LEN_OFF, PAYLOAD);
    put_pld(w, in->base + PLD_OFF, PAYLOAD, nsge);

    /* A MAD: base version 1, then a recognisable body. */
    uint8_t *p = g_mem + in->base + PLD_OFF;
    for (uint32_t i = 0; i < PAYLOAD; i++)
        p[i] = (uint8_t)(i * 7 + 3);
    p[0] = 1;

    in->sq_prod[qp]++;
    ionic_datapath_doorbell(in->dp, DP_QTYPE_SQ,
                            (uint64_t)in->sq_prod[qp] |
                                ((uint64_t)(qp & 0xffu) << 24));
}

static void post_ud_send(struct inst *in, uint32_t qp, uint32_t ah_id,
                         uint32_t dest_qpn)
{
    post_ud_send_sges(in, qp, ah_id, dest_qpn, 1);
}

/* The next unread CQE of this instance's CQ, or NULL if there is none. */
static const uint8_t *next_cqe(struct inst *in)
{
    struct ionic_cq_ring *c = &in->dp->cq[CQ_ID];
    if (in->cq_seen == c->prod)
        return NULL;
    return g_mem + in->base + CQ_OFF + (in->cq_seen++ % CQ_DEPTH) * CQE_SIZE;
}

static uint32_t cqe_type(const uint8_t *cqe)
{
    uint32_t qtf;
    memcpy(&qtf, cqe + 28, 4);
    return be32toh(qtf) & (7u << 5);
}

static uint32_t cqe_qid(const uint8_t *cqe)
{
    uint32_t qtf;
    memcpy(&qtf, cqe + 28, 4);
    return be32toh(qtf) >> 8;
}

static uint32_t cqe_src_qpn_op(const uint8_t *cqe)
{
    uint32_t v;
    memcpy(&v, cqe + 8, 4);
    return be32toh(v);
}

static uint32_t cqe_len(const uint8_t *cqe)
{
    uint32_t v;
    memcpy(&v, cqe + 24, 4);
    return be32toh(v);
}

static bool cqe_error(const uint8_t *cqe)
{
    return (cqe[31] & CQE_ERROR_BIT) != 0;
}

/* Find the receive completion for @qp among the pending CQEs. */
static const uint8_t *find_recv(struct inst *in, uint32_t qp)
{
    const uint8_t *cqe;
    while ((cqe = next_cqe(in)))
        if (cqe_type(cqe) == CQE_TYPE_RECV && cqe_qid(cqe) == qp)
            return cqe;
    return NULL;
}

static void v4_gid(uint8_t gid[16], uint8_t last)
{
    memset(gid, 0, 16);
    gid[10] = 0xff;
    gid[11] = 0xff;
    gid[12] = 192;
    gid[13] = 168;
    gid[14] = 200;
    gid[15] = last;
}

static const uint8_t SMAC_A[6] = {0x72, 0x6f, 0x63, 0x6d, 0x00, 0x01};
static const uint8_t SMAC_B[6] = {0x72, 0x6f, 0x63, 0x6d, 0x00, 0x02};

static struct ionic_dp_ah v4_ah(uint8_t src, uint8_t dst, const uint8_t *smac,
                                const uint8_t *dmac)
{
    struct ionic_dp_ah ah;
    memset(&ah, 0, sizeof(ah));
    v4_gid(ah.sgid, src);
    v4_gid(ah.dgid, dst);
    memcpy(ah.smac, smac, 6);
    memcpy(ah.dmac, dmac, 6);
    ah.ipv4 = true;
    return ah;
}

/*
 * The receive buffer as ib_mad sees it: a 40-byte GRH, of which a RoCEv2 IPv4
 * packet uses only the last 20 bytes (an IPv4 header whose saddr and daddr
 * ib_get_gids_from_rdma_hdr() turns back into GIDs), then the payload.
 */
static bool v4_grh_ok(const uint8_t *buf, uint8_t src, uint8_t dst)
{
    const uint8_t *ip = buf + 20;
    uint8_t want_s[4] = {192, 168, 200, src};
    uint8_t want_d[4] = {192, 168, 200, dst};

    return ip[0] == 0x45 && ip[9] == 17 /* UDP */ &&
           !memcmp(ip + 12, want_s, 4) && !memcmp(ip + 16, want_d, 4);
}

static bool payload_ok(const uint8_t *buf, const struct inst *sender)
{
    return !memcmp(buf + IB_GRH_SIZE, g_mem + sender->base + PLD_OFF, PAYLOAD);
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * The regression, on one instance.  UD QP A sends to UD QP B through an AH
 * that points at our own address.  B must get it, A must not, and the MAD
 * must start at offset 40 behind a GRH.
 */
static void test_local_ud_send(void)
{
    inst_init(&g_a, 0, UINT32_MAX);
    struct ionic_dp_ah ah = v4_ah(10, 10, SMAC_A, SMAC_A);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_UD_A, 0);
    post_recv(&g_a, QP_UD_B, 1);
    post_ud_send(&g_a, QP_UD_A, AH_ID, QP_UD_B);

    const uint8_t *rbuf = g_mem + g_a.base + RBUF_OFF(1);
    check(g_a.dp->qp[QP_UD_A].rq_cons == 0,
          "local: the sender's own receive is not consumed");
    check(g_a.dp->qp[QP_UD_B].rq_cons == 1,
          "local: the QP the WQE names receives the SEND");
    check(rbuf[IB_GRH_SIZE] == 1, "local: the MAD starts at offset 40");
    check(v4_grh_ok(rbuf, 10, 10),
          "local: the GRH carries an IPv4 header with both addresses");
    check(payload_ok(rbuf, &g_a), "local: the payload follows the GRH");

    const uint8_t *cqe = find_recv(&g_a, QP_UD_B);
    check(cqe != NULL, "local: a receive completion is posted for QP B");
    if (cqe) {
        uint32_t sq = cqe_src_qpn_op(cqe);
        check(!cqe_error(cqe), "local: the completion is not an error");
        check(cqe_len(cqe) == RBUF_LEN,
              "local: byte_len counts the GRH and the payload");
        check((sq & 0xffffffu) == QP_UD_A, "local: src_qpn names the sender");
        check((sq & CQE_RECV_IS_IPV4) != 0, "local: the IPv4 bit is set");
        check(!memcmp(cqe + 12, SMAC_A, 6),
              "local: src_mac is the sender's MAC from the AH");
    }
    inst_fini(&g_a);
}

/* The GSI QP is the case rdma_cm actually hits: QP1 to the peer's QP1. */
static void test_local_gsi_send(void)
{
    inst_init(&g_a, 0, UINT32_MAX);
    struct ionic_dp_ah ah = v4_ah(10, 10, SMAC_A, SMAC_A);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_GSI, 0);
    post_ud_send(&g_a, QP_GSI, AH_ID, QP_GSI);

    const uint8_t *rbuf = g_mem + g_a.base + RBUF_OFF(0);
    check(rbuf[IB_GRH_SIZE] == 1, "gsi: the MAD base version is at offset 40");
    check(find_recv(&g_a, QP_GSI) != NULL, "gsi: QP1 receives its own MAD");
    inst_fini(&g_a);
}

/*
 * Two instances.  A (node 0, .10) sends through an AH naming .20, which is
 * node 1.  Nothing may land locally; the message goes to node 1 and, carried
 * over by hand, lands on B's QP with a GRH describing the path from A.
 */
static void test_remote_gsi_send(void)
{
    inst_init(&g_a, 0, 0);
    inst_init(&g_b, HALF, 1);
    struct ionic_dp_ah ah = v4_ah(10, 20, SMAC_A, SMAC_B);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_GSI, 0);
    post_recv(&g_b, QP_GSI, 0);

    g_tx_count = 0;
    post_ud_send(&g_a, QP_GSI, AH_ID, QP_GSI);

    check(g_a.dp->qp[QP_GSI].rq_cons == 0,
          "remote: nothing is delivered on the sending instance");
    check(g_tx_count == 1 && g_tx_node == 1,
          "remote: exactly one message goes to the node the dgid names");

    /* UD is unacknowledged: the send completes as soon as it is on the wire. */
    const uint8_t *cqe = next_cqe(&g_a);
    check(cqe && cqe_type(cqe) == CQE_TYPE_SEND_NPG && !cqe_error(cqe),
          "remote: the signalled send completes without waiting for the peer");

    g_tx_count = 0;
    dp_handle_wire(g_b.dp, 0, g_tx, g_tx_len, true);
    check(g_tx_count == 0, "remote: the receiver sends nothing back");

    const uint8_t *rbuf = g_mem + g_b.base + RBUF_OFF(0);
    check(g_b.dp->qp[QP_GSI].rq_cons == 1, "remote: the peer's QP1 receives");
    check(rbuf[IB_GRH_SIZE] == 1,
          "remote: the MAD base version is at offset 40");
    check(v4_grh_ok(rbuf, 10, 20), "remote: the GRH says .10 -> .20");
    check(payload_ok(rbuf, &g_a), "remote: the payload arrives intact");

    cqe = find_recv(&g_b, QP_GSI);
    check(cqe != NULL, "remote: the peer gets a receive completion");
    if (cqe) {
        uint32_t sq = cqe_src_qpn_op(cqe);
        check((sq & 0xffffffu) == QP_GSI, "remote: src_qpn is the sender's");
        check((sq & CQE_RECV_IS_IPV4) != 0, "remote: the IPv4 bit is set");
        check(!memcmp(cqe + 12, SMAC_A, 6),
              "remote: src_mac is the sender's MAC");
        check(cqe_len(cqe) == RBUF_LEN, "remote: byte_len includes the GRH");
    }
    inst_fini(&g_a);
    inst_fini(&g_b);
}

/*
 * A GID the mesh cannot place is still our own when it matches the AH's
 * source, so a loopback MAD to an address this instance never advertised is
 * delivered rather than dropped as unroutable.
 */
static void test_unresolvable_self_stays_local(void)
{
    inst_init(&g_a, 0, 0);
    struct ionic_dp_ah ah = v4_ah(11, 11, SMAC_A, SMAC_A);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_UD_B, 0);
    g_tx_count = 0;
    post_ud_send(&g_a, QP_UD_A, AH_ID, QP_UD_B);

    check(g_tx_count == 0, "self: a send to our own GID stays off the mesh");
    check(g_a.dp->qp[QP_UD_B].rq_cons == 1, "self: and is delivered locally");
    inst_fini(&g_a);
}

/*
 * On a mesh, a destination no node advertised is unroutable.  It must be
 * dropped: neither sent to a guessed node nor delivered to the QP of that
 * number on this instance, which would hand a peer's MAD to our own guest.
 */
static void test_unowned_gid_dropped(void)
{
    inst_init(&g_a, 0, 0);
    struct ionic_dp_ah ah = v4_ah(10, 33, SMAC_A, SMAC_B);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_UD_B, 0);
    g_tx_count = 0;
    post_ud_send(&g_a, QP_UD_A, AH_ID, QP_UD_B);

    check(g_tx_count == 0, "unowned: nothing is sent on the mesh");
    check(g_a.dp->qp[QP_UD_B].rq_cons == 0,
          "unowned: and nothing is delivered locally");
    inst_fini(&g_a);
}

/* An IPv6 AH gets an IPv6 GRH, with the GIDs where ib_grh keeps them. */
static void test_ipv6_grh(void)
{
    inst_init(&g_a, 0, UINT32_MAX);
    struct ionic_dp_ah ah;
    memset(&ah, 0, sizeof(ah));
    ah.sgid[0] = 0xfe;
    ah.sgid[1] = 0x80;
    ah.sgid[15] = 0x01;
    memcpy(ah.dgid, ah.sgid, 16);
    memcpy(ah.smac, SMAC_A, 6);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_recv(&g_a, QP_UD_B, 0);
    post_ud_send(&g_a, QP_UD_A, AH_ID, QP_UD_B);

    const uint8_t *rbuf = g_mem + g_a.base + RBUF_OFF(0);
    check((rbuf[0] >> 4) == 6, "ipv6: the GRH has IP version 6");
    check(!memcmp(rbuf + 8, ah.sgid, 16) && !memcmp(rbuf + 24, ah.dgid, 16),
          "ipv6: sgid and dgid sit at GRH offsets 8 and 24");

    const uint8_t *cqe = find_recv(&g_a, QP_UD_B);
    check(cqe && !(cqe_src_qpn_op(cqe) & CQE_RECV_IS_IPV4),
          "ipv6: the IPv4 bit is clear");
    inst_fini(&g_a);
}

/* A destroyed or never-created AH routes nowhere, and delivers nowhere. */
static void test_unknown_ah_dropped(void)
{
    inst_init(&g_a, 0, UINT32_MAX);
    struct ionic_dp_ah ah = v4_ah(10, 10, SMAC_A, SMAC_A);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);
    ionic_datapath_unregister_ah(g_a.dp, AH_ID);

    post_recv(&g_a, QP_UD_A, 0);
    post_recv(&g_a, QP_UD_B, 1);
    post_ud_send(&g_a, QP_UD_A, AH_ID, QP_UD_B);

    check(g_a.dp->qp[QP_UD_A].rq_cons == 0 && g_a.dp->qp[QP_UD_B].rq_cons == 0,
          "no-ah: nothing is delivered through a destroyed AH");

    /* Out of range: must be ignored, not written past the table (ASAN). */
    ionic_datapath_register_ah(g_a.dp, IONIC_MAX_AH, &ah);
    post_ud_send(&g_a, QP_UD_A, IONIC_MAX_AH, QP_UD_B);
    check(g_a.dp->qp[QP_UD_B].rq_cons == 0,
          "no-ah: an out-of-range ah_id routes nowhere");
    inst_fini(&g_a);
}

/*
 * A UD message that finds no posted receive is dropped once the RNR window
 * closes, silently: UD has no acknowledgement to carry an error back.
 */
static void test_remote_rnr_drop_is_silent(void)
{
    inst_init(&g_a, 0, 0);
    inst_init(&g_b, HALF, 1);
    struct ionic_dp_ah ah = v4_ah(10, 20, SMAC_A, SMAC_B);
    ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

    post_ud_send(&g_a, QP_GSI, AH_ID, QP_GSI);

    g_tx_count = 0;
    check(!dp_handle_wire(g_b.dp, 0, g_tx, g_tx_len, true),
          "rnr: inside the window the message is kept for a retry");
    check(dp_handle_wire(g_b.dp, 0, g_tx, g_tx_len, false),
          "rnr: past the window it is consumed");
    check(g_tx_count == 0, "rnr: and nothing is sent back");
    inst_fini(&g_a);
    inst_fini(&g_b);
}

/*
 * Kernel QPs ask for speculative SGE lists, so a WQE with more than two SGEs
 * carries a table of lengths ahead of them and flags SPEC32 or SPEC16.  Read
 * as plain SGEs, that table became a garbage first SGE, and nvmet-rdma's
 * multi-page RDMA WRITEs failed with "could not gather".  Both the send and
 * the receive side must skip it.
 */
static void test_spec_sge_lists(void)
{
    static const uint32_t counts[] = {2, 3, 8, 9};

    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++) {
        uint32_t n = counts[k];
        char what[96];

        inst_init(&g_a, 0, UINT32_MAX);
        struct ionic_dp_ah ah = v4_ah(10, 10, SMAC_A, SMAC_A);
        ionic_datapath_register_ah(g_a.dp, AH_ID, &ah);

        post_recv_sges(&g_a, QP_UD_B, 0, n);
        post_ud_send_sges(&g_a, QP_UD_A, AH_ID, QP_UD_B, n);

        const uint8_t *rbuf = g_mem + g_a.base + RBUF_OFF(0);
        const uint8_t *cqe = find_recv(&g_a, QP_UD_B);
        snprintf(what, sizeof(what),
                 "spec: %u SGEs each side deliver the payload intact", n);
        check(cqe && !cqe_error(cqe) && cqe_len(cqe) == RBUF_LEN &&
                  payload_ok(rbuf, &g_a) && v4_grh_ok(rbuf, 10, 10),
              what);
        inst_fini(&g_a);
    }
}

int main(void)
{
    test_local_ud_send();
    test_local_gsi_send();
    test_remote_gsi_send();
    test_unresolvable_self_stays_local();
    test_unowned_gid_dropped();
    test_ipv6_grh();
    test_unknown_ah_dropped();
    test_remote_rnr_drop_is_silent();
    test_spec_sge_lists();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
