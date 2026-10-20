/*
 * Receive-ring producer-index tests for the ionic datapath.
 *
 * The doorbell carries a producer index, and the only question these ask is
 * what that index means. ionic_queue_produce() in rdma-core masks it to the
 * ring -- "q->prod = (q->prod + 1) & q->mask" -- so it counts 0, 1, ...,
 * depth-1, 0, 1 and never grows past the ring. The datapath used to treat it
 * as a free-running 16-bit counter and add the difference, which agrees with
 * the masked index exactly until the first wrap and never again: posting one
 * more entry after a full lap subtracts instead of adding, and the device
 * concludes that tens of thousands of receives are outstanding.
 *
 * That has two consequences, and neither announces itself. "No receive
 * posted" stops being detectable, so RNR retry never triggers; and the
 * device reads ring entries the guest has already reclaimed, scattering into
 * buffers the application got back and completing work requests that are no
 * longer outstanding. A benchmark that reposts before data arrives never
 * sees it.
 *
 * The send queue already read the index the right way. These cover the
 * receive side, for a QP's own RQ and for a shared one.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_datapath.h"
#include "ionic_datapath_priv.h"
#include "ionic_eth_emu.h"
#include "nvmeof_cm.h"
#include "nvmeof_target.h"
#include "rocm_ernic_compat.h"
#include "s3_tcp.h"
#include "s3_target.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 *
 * None of these are reachable from the CQ doorbell path; they exist only to
 * satisfy the linker. libvfio-user itself is linked for vfu_log/vfu_sgl_*.
 */

/*
 * The real vfu_log() asserts on a NULL context, and the doorbell path logs on
 * every call, so this one has to be overridden rather than linked.
 */
void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)level;
    (void)fmt;
}

int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    (void)emu;
    (void)vec;
    return 0;
}

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle)
{
    (void)handle;
    return UINT32_MAX;
}
uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle, const uint8_t *gid)
{
    (void)handle;
    (void)gid;
    return UINT32_MAX;
}
int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len)
{
    (void)handle;
    (void)dst_node;
    (void)hdr;
    (void)hdr_len;
    (void)body;
    (void)body_len;
    return -1;
}
void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque)
{
    (void)handle;
    (void)fn;
    (void)opaque;
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

static struct ionic_datapath g_dp;
static int g_failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        g_failures++;
}

/* Ring 0 carries the index; the arm rings are a CQ concept. */
#define RING_CONS 0

/* The doorbell is one packed 64-bit write; see ionic_datapath_doorbell(). */
static uint64_t db(uint32_t qid, uint8_t ring, uint16_t p_index)
{
    return (uint64_t)p_index | ((uint64_t)ring << 16) |
           ((uint64_t)(qid & 0xffu) << 24) |
           ((uint64_t)((qid >> 8) & 0xffffu) << 32);
}


/* ---- Receive-ring fixtures --------------------------------------------- */

#define RQ_QP_ID 6
#define RQ_DEPTH 8
#define SRQ_SLOT 0
#define SRQ_QID  (IONIC_SRQ_QID_BASE + SRQ_SLOT)

static struct ionic_qp_ring g_qp_storage[RQ_QP_ID + 1];
static struct ionic_srq_ring g_srq_storage[SRQ_SLOT + 1];

/* A datapath holding one QP with its own RQ and one SRQ, both RQ_DEPTH
 * deep and both empty. Built by hand, as dp_with_cq() does: none of the
 * allocation paths are under test here. */
static struct ionic_datapath *dp_with_recv_rings(void)
{
    memset(&g_dp, 0, sizeof(g_dp));
    memset(g_qp_storage, 0, sizeof(g_qp_storage));
    memset(g_srq_storage, 0, sizeof(g_srq_storage));

    g_dp.qp = g_qp_storage;
    g_dp.qp_count = RQ_QP_ID + 1;
    g_dp.srq = g_srq_storage;
    g_dp.srq_count = SRQ_SLOT + 1;

    g_dp.qp[RQ_QP_ID].valid = true;
    g_dp.qp[RQ_QP_ID].rq_depth = RQ_DEPTH;
    g_dp.qp[RQ_QP_ID].rq_stride_log2 = 5;

    g_dp.srq[SRQ_SLOT].valid = true;
    g_dp.srq[SRQ_SLOT].depth = RQ_DEPTH;
    g_dp.srq[SRQ_SLOT].stride_log2 = 5;
    pthread_mutex_init(&g_dp.srq[SRQ_SLOT].lock, NULL);

    return &g_dp;
}

static void rq_doorbell(struct ionic_datapath *dp, uint32_t qid,
                        uint16_t p_index)
{
    ionic_datapath_doorbell(dp, DP_QTYPE_RQ, db(qid, RING_CONS, p_index));
}

/* What the device believes is posted and not yet consumed. */
static uint32_t qp_outstanding(const struct ionic_datapath *dp)
{
    return dp->qp[RQ_QP_ID].rq_prod - dp->qp[RQ_QP_ID].rq_cons;
}

static uint32_t srq_outstanding(const struct ionic_datapath *dp)
{
    return dp->srq[SRQ_SLOT].prod - dp->srq[SRQ_SLOT].cons;
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * Before the wrap the two readings agree, which is why this went unnoticed:
 * while prod stays below the depth, "difference from the last index" and
 * "masked index" are the same number.
 */
static void test_rq_counts_up_to_the_wrap(void)
{
    struct ionic_datapath *dp = dp_with_recv_rings();

    for (uint16_t i = 1; i < RQ_DEPTH; i++) {
        rq_doorbell(dp, RQ_QP_ID, i);
        rq_doorbell(dp, SRQ_QID, i);
    }
    check(qp_outstanding(dp) == RQ_DEPTH - 1,
          "QP RQ counts each post before the ring wraps");
    check(srq_outstanding(dp) == RQ_DEPTH - 1,
          "SRQ counts each post before the ring wraps");
}

/*
 * The regression. Seven posts into an eight-deep ring, all seven consumed,
 * then two more: the driver's index has wrapped to 1, and the device must
 * read that as two newly posted receives rather than as a backwards jump.
 */
static void test_rq_survives_the_wrap(void)
{
    struct ionic_datapath *dp = dp_with_recv_rings();

    for (uint16_t i = 1; i < RQ_DEPTH; i++) {
        rq_doorbell(dp, RQ_QP_ID, i);
        rq_doorbell(dp, SRQ_QID, i);
    }
    dp->qp[RQ_QP_ID].rq_cons = dp->qp[RQ_QP_ID].rq_prod;
    dp->srq[SRQ_SLOT].cons = dp->srq[SRQ_SLOT].prod;

    /* (7 + 1) & 7 == 0, then (0 + 1) & 7 == 1. */
    rq_doorbell(dp, RQ_QP_ID, 0);
    rq_doorbell(dp, SRQ_QID, 0);
    rq_doorbell(dp, RQ_QP_ID, 1);
    rq_doorbell(dp, SRQ_QID, 1);

    check(qp_outstanding(dp) == 2, "QP RQ reads two posts across the wrap");
    check(srq_outstanding(dp) == 2, "SRQ reads two posts across the wrap");
}

/* Several laps, to catch a fix that only works for the first one. */
static void test_rq_survives_many_wraps(void)
{
    struct ionic_datapath *dp = dp_with_recv_rings();
    uint32_t posted = 0;

    for (uint32_t lap = 0; lap < 5; lap++) {
        for (uint16_t i = 0; i < RQ_DEPTH; i++) {
            uint16_t idx = (uint16_t)((posted + 1) % RQ_DEPTH);

            rq_doorbell(dp, RQ_QP_ID, idx);
            rq_doorbell(dp, SRQ_QID, idx);
            posted++;
            if (qp_outstanding(dp) != 1 || srq_outstanding(dp) != 1)
                break;
            dp->qp[RQ_QP_ID].rq_cons = dp->qp[RQ_QP_ID].rq_prod;
            dp->srq[SRQ_SLOT].cons = dp->srq[SRQ_SLOT].prod;
        }
    }
    check(posted == 5 * RQ_DEPTH,
          "post-and-drain stays at one outstanding across five laps");
}

/* A doorbell cannot name a slot the ring does not have. Accepting one
 * would reintroduce the same impossible count by a different route. */
static void test_rq_rejects_an_index_past_the_depth(void)
{
    struct ionic_datapath *dp = dp_with_recv_rings();

    rq_doorbell(dp, RQ_QP_ID, RQ_DEPTH);
    rq_doorbell(dp, SRQ_QID, RQ_DEPTH);
    check(qp_outstanding(dp) == 0, "QP RQ ignores an out-of-range index");
    check(srq_outstanding(dp) == 0, "SRQ ignores an out-of-range index");

    rq_doorbell(dp, RQ_QP_ID, 0xffffu);
    rq_doorbell(dp, SRQ_QID, 0xffffu);
    check(qp_outstanding(dp) == 0, "QP RQ ignores 0xffff");
    check(srq_outstanding(dp) == 0, "SRQ ignores 0xffff");
}

/*
 * map_count is a guest u32 and the page table it describes is allocated
 * here, so without a bound one CREATE command asks the host for up to
 * 2^32 * 8 == 32 GiB, and several hundred are reachable before any table
 * fills. A page table never needs more entries than the region needs pages,
 * plus one for a region that does not start on a page boundary.
 *
 * The rejection happens before any allocation or DMA, so these need no
 * guest memory standing up.
 */
static void test_page_table_is_bounded_by_the_region(void)
{
    struct dp_buf b;
    struct ionic_dp_buf_desc desc = {
        .dma_addr = 0x1000,
        .page_size_log2 = 12,
    };

    memset(&b, 0, sizeof(b));

    /* One 4 KiB page, described by one entry: the single-page path. */
    desc.map_count = 1;
    check(ionic_dp_buf_init(&g_dp, &b, &desc, 0, 4096) == 0,
          "a one-page region with one table entry is accepted");

    /* The same region, claiming a million pages. */
    desc.map_count = 1000000;
    check(ionic_dp_buf_init(&g_dp, &b, &desc, 0, 4096) == -EINVAL,
          "a one-page region cannot have a million table entries");
    check(b.pages == NULL, "the oversized table was never allocated");

    /* Only the rejecting direction is checked here. A map_count the bound
     * accepts goes on to read the table out of guest memory, which needs a
     * vfu context; real rings of every size are registered by the system
     * tests, which is where "not too tight" is actually demonstrated. */

    /* A geometry describing no bytes at all describes no page table. */
    desc.map_count = 1;
    check(ionic_dp_buf_init(&g_dp, &b, &desc, 0, 0) == -EINVAL,
          "a zero-length region is refused");

    ionic_dp_buf_release(&b);
}

/*
 * ionic_dp_ring_span_bytes() exists so a guest geometry cannot shift by the
 * width of the type. The QP exponents are not bounded at the adminq yet, so
 * this is the thing standing between a malformed CREATE_QP and undefined
 * behaviour.
 */
static void test_ring_span_saturates(void)
{
    check(ionic_dp_ring_span_bytes(6, 6) == 4096,
          "2^6 entries of 2^6 bytes is 4 KiB");
    check(ionic_dp_ring_span_bytes(63, 0) != 0,
          "an exponent sum of 63 still computes");
    check(ionic_dp_ring_span_bytes(64, 0) == 0,
          "an exponent sum of 64 is refused");
    check(ionic_dp_ring_span_bytes(255, 255) == 0,
          "the largest guest bytes are refused");
}

/*
 * Who may receive a datagram. The destination QPN in a work request used to
 * select any valid QP in the device, with no check on transport type and no
 * Q_Key at all -- so a guest could address another process's RC QP, or QP1
 * and forge connection-management MADs at the kernel.
 */
static void test_ud_delivery_is_refused_to_the_wrong_queue(void)
{
    struct ionic_qp_ring rc = {.valid = true, .ib_qp_type = 2 /* RC */};
    struct ionic_qp_ring uc = {.valid = true, .ib_qp_type = 3 /* UC */};
    struct ionic_qp_ring ud = {.valid = true, .ib_qp_type = 4 /* UD */};
    struct ionic_qp_ring gsi = {.valid = true, .ib_qp_type = 1 /* GSI */};

    /*
     * A connected send is not subject to any of this, and saying so is the
     * first case because getting it wrong refuses every RC and UC transfer
     * in the device. An earlier revision applied the datagram rules to
     * every SEND and did exactly that: rdma_verify reported "no completion"
     * for send, send-recv, write-imm and read, and only the operations that
     * take another path still worked.
     */
    check(ionic_dp_ud_delivery_refused(&rc, &rc, 0) == NULL,
          "an RC send to its connected peer is not a datagram send");
    check(ionic_dp_ud_delivery_refused(&uc, &uc, 0) == NULL,
          "a UC send to its connected peer is not a datagram send");
    check(ionic_dp_ud_delivery_refused(&rc, &ud, 0) == NULL,
          "a connected sender is never held to the datagram rules");

    /* A datagram may only land in a queue that accepts datagrams. */
    check(ionic_dp_ud_delivery_refused(&ud, &rc, 0) != NULL,
          "a datagram to a connected RC queue is refused");
    check(ionic_dp_ud_delivery_refused(&ud, &uc, 0) != NULL,
          "a datagram to a connected UC queue is refused");
    check(ionic_dp_ud_delivery_refused(&ud, &ud, 0) == NULL,
          "a datagram to an unconfigured UD queue is delivered");
    check(ionic_dp_ud_delivery_refused(&ud, &gsi, 0) == NULL,
          "GSI before the kernel sets its Q_Key still accepts one");

    /* Once a queue has a Q_Key, a datagram must present it. */
    ud.qkey_valid = true;
    ud.qkey = 0x11111111u;
    check(ionic_dp_ud_delivery_refused(&ud, &ud, 0x11111111u) == NULL,
          "the right Q_Key is delivered");
    check(ionic_dp_ud_delivery_refused(&ud, &ud, 0x22222222u) != NULL,
          "a wrong Q_Key is refused");
    check(ionic_dp_ud_delivery_refused(&ud, &ud, 0) != NULL,
          "a zero Q_Key is refused");

    /* QP1's key is privileged; the point of checking it is that an
     * unprivileged guest guessing a QPN cannot reach the GSI queue. */
    gsi.qkey_valid = true;
    gsi.qkey = 0x80010000u;
    check(ionic_dp_ud_delivery_refused(&ud, &gsi, 0x80010000u) == NULL,
          "the privileged GSI Q_Key is delivered");
    check(ionic_dp_ud_delivery_refused(&ud, &gsi, 0x11111111u) != NULL,
          "GSI refuses a datagram that does not present its Q_Key");
}


/* Storage for the registration tests, which need a CQ table too. */
static struct ionic_cq_ring g_reg_cq[4];

/*
 * The registration entry points, not just the leaf they call.
 *
 * The page-table bound is computed from the ring's geometry, and computing
 * it from the wrong place rejects every legitimate ring. That is exactly
 * what happened: the CQ path read c->depth, which is not assigned until
 * after ionic_dp_buf_init() returns and is zero for a CQ being registered, so
 * the span was zero and no CQ in the device ever registered. The guest saw "no
 * completion" for every operation while the data still moved, because memory
 * regions take their span from a parameter rather than a field.
 *
 * Testing ionic_dp_buf_init() directly could not see this, and did not. These
 * go through the functions the adminq actually calls.
 */
static void test_registration_accepts_an_ordinary_ring(void)
{
    struct ionic_datapath *dp = dp_with_recv_rings();
    struct ionic_dp_ring_desc ring = {
        .buf = {.dma_addr = 0x100000, .map_count = 1, .page_size_log2 = 12},
        .depth_log2 = 6,
        .stride_log2 = 5,
    };

    memset(g_reg_cq, 0, sizeof(g_reg_cq));
    dp->cq = g_reg_cq;
    dp->cq_count = 4;

    ionic_datapath_register_cq(dp, 1, 2, &ring);
    check(dp->cq[1].valid, "a CQ with an ordinary geometry registers");
    check(dp->cq[1].depth == 64, "the CQ kept its depth");

    ionic_datapath_register_qp(dp, RQ_QP_ID, 2 /* RC */, 1, &ring, 1, &ring);
    check(dp->qp[RQ_QP_ID].valid, "a QP with ordinary rings registers");

    check(ionic_datapath_register_srq(dp, SRQ_QID, &ring) == 0,
          "an SRQ with an ordinary ring registers");
    check(dp->srq[SRQ_SLOT].valid, "and is left valid");
}

int main(void)
{
    test_registration_accepts_an_ordinary_ring();
    test_ud_delivery_is_refused_to_the_wrong_queue();
    test_page_table_is_bounded_by_the_region();
    test_ring_span_saturates();
    test_rq_counts_up_to_the_wrap();
    test_rq_survives_the_wrap();
    test_rq_survives_many_wraps();
    test_rq_rejects_an_index_past_the_depth();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
