/*
 * Unit tests for the ionic emulator's QUERY_QP admin command.
 *
 * QUERY_QP is not answered by its completion. The driver DMA-maps two
 * zeroed side buffers, and reads the QP's state out of them once the
 * command completes. A handler that returns success without writing them
 * therefore reports a QP in RESET no matter what state it is really in --
 * which is exactly what ibv_query_qp saw while this opcode was a stub, on
 * a queue pair that had been driven all the way to RTS.
 *
 * These tests drive dispatch_wqe() directly against a fake guest memory,
 * so they pin the encoding of the two buffers: the state and MTU nibbles
 * the driver unpacks from state_pmtu, and the access flags it ORs across
 * both. The translation unit is #included to reach dispatch_wqe(), and its
 * external symbols are stubbed.
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
#include "ionic_adminq.c"

/* ---- Fake guest memory ------------------------------------------------- */

/*
 * Addresses are offsets into this buffer, so a "GPA" is an index. Only the
 * DMA entry points are stubbed; the handler's own address arithmetic runs
 * unmodified.
 */
#define GUEST_MEM_SIZE 4096
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
    if (max_nr_sgs < 1 || addr + len > GUEST_MEM_SIZE)
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

/* ---- Stubs for the TU's other external symbols ------------------------- */

#define QP_ID  7u
#define QP_QPN 42u
#define SQ_GPA 256u
#define RQ_GPA 512u

/* What the resource manager will claim about the queried QP. */
static uint8_t g_state;
static uint8_t g_path_mtu;
static uint32_t g_dest_qpn;
static uint32_t g_access;
static uint32_t g_rq_psn;
static uint32_t g_sq_psn;
static int g_query_rc;
static uint32_t g_queried_qpn;

int ionic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn)
{
    (void)handle;
    g_queried_qpn = qpn;
    if (g_query_rc)
        return g_query_rc;
    if (state)
        *state = g_state;
    if (path_mtu)
        *path_mtu = g_path_mtu;
    if (dest_qpn)
        *dest_qpn = g_dest_qpn;
    if (access_flags)
        *access_flags = g_access;
    if (rq_psn)
        *rq_psn = g_rq_psn;
    if (sq_psn)
        *sq_psn = g_sq_psn;
    return 0;
}

int ionic_rm_modify_qp(pvrdma_handle_t handle, uint32_t qpn, uint32_t attr_mask,
                       uint8_t type_state, uint32_t sq_psn, uint32_t rq_psn,
                       uint32_t qkey_dest_qpn, const uint8_t *dest_gid_16bytes)
{
    (void)handle;
    (void)qpn;
    (void)attr_mask;
    (void)type_state;
    (void)sq_psn;
    (void)rq_psn;
    (void)qkey_dest_qpn;
    (void)dest_gid_16bytes;
    return 0;
}

int ionic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle)
{
    (void)handle;
    *pd_handle = 1;
    return 0;
}
int ionic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe, uint32_t *cq_handle)
{
    (void)handle;
    (void)cqe;
    *cq_handle = 1;
    return 0;
}
void ionic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle)
{
    (void)handle;
    (void)cq_handle;
}
int ionic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn)
{
    (void)handle;
    (void)pd_handle;
    (void)qp_type;
    (void)max_send_wr;
    (void)max_recv_wr;
    (void)send_cq_handle;
    (void)recv_cq_handle;
    *qpn = QP_QPN;
    return 0;
}
void ionic_rm_dealloc_qp(pvrdma_handle_t handle, uint32_t qpn)
{
    (void)handle;
    (void)qpn;
}
int ionic_rm_alloc_mr(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint32_t access_flags, uint32_t *mr_handle)
{
    (void)handle;
    (void)pd_handle;
    (void)access_flags;
    *mr_handle = 1;
    return 0;
}
void ionic_rm_dealloc_mr(pvrdma_handle_t handle, uint32_t mr_handle)
{
    (void)handle;
    (void)mr_handle;
}

void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint32_t eq_id,
                                const struct ionic_dp_ring_desc *ring)
{
    (void)dp;
    (void)cq_id;
    (void)eq_id;
    (void)ring;
}
void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id)
{
    (void)dp;
    (void)cq_id;
}
void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint8_t ib_qp_type, uint32_t sq_cq_id,
                                const struct ionic_dp_ring_desc *sq,
                                uint32_t rq_cq_id,
                                const struct ionic_dp_ring_desc *rq)
{
    (void)dp;
    (void)qp_id;
    (void)ib_qp_type;
    (void)sq_cq_id;
    (void)sq;
    (void)rq_cq_id;
    (void)rq;
}
void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    (void)dp;
    (void)qp_id;
}
void ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                                uint64_t va, uint64_t length,
                                const struct ionic_dp_buf_desc *buf)
{
    (void)dp;
    (void)lkey;
    (void)va;
    (void)length;
    (void)buf;
}
void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t lkey)
{
    (void)dp;
    (void)lkey;
}
void ionic_datapath_set_dest(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t dest_qp_id, uint32_t dest_node_id)
{
    (void)dp;
    (void)qp_id;
    (void)dest_qp_id;
    (void)dest_node_id;
}
uint32_t ionic_dp_node_from_gid(struct ionic_datapath *dp, const uint8_t *dgid)
{
    (void)dp;
    (void)dgid;
    return 0;
}

void pvrdma_adminq_count(pvrdma_handle_t handle)
{
    (void)handle;
}
void pvrdma_qp_stats_forget(pvrdma_handle_t handle, uint32_t qp_id)
{
    (void)handle;
    (void)qp_id;
}

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static struct ionic_adminq_ctx g_ctx;

/*
 * A context holding one qp_id -> qpn mapping, the fake guest memory wiped,
 * and the resource manager reporting an RTS QP with 1024-byte MTU and both
 * remote access flags -- which is where the S3 client's QP ends up.
 */
static void ctx_reset(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.pvrdma_handle = (pvrdma_handle_t)&g_ctx; /* only tested for NULL */
    g_ctx.qp_map[0].valid = true;
    g_ctx.qp_map[0].qp_id = QP_ID;
    g_ctx.qp_map[0].qpn = QP_QPN;

    memset(g_mem, 0, sizeof(g_mem));
    g_state = 3;    /* IBV_QPS_RTS */
    g_path_mtu = 3; /* IBV_MTU_1024 */
    g_dest_qpn = 0xd00001;
    g_access = (1u << 1) | (1u << 2); /* REMOTE_WRITE | REMOTE_READ */
    g_rq_psn = 0x0abcde;
    g_sq_psn = 0x123456;
    g_query_rc = 0;
    g_queried_qpn = 0;
}

/* The 34-byte ionic_admin_query_qp body the driver posts. */
static void build_body(uint8_t *body, uint32_t qp_id, uint64_t sq_gpa,
                       uint64_t rq_gpa)
{
    uint64_t sq_le = htole64(sq_gpa), rq_le = htole64(rq_gpa);
    uint32_t id_ver = htole32(qp_id | (1u << 24));

    memset(body, 0, 34);
    memcpy(body + 8, &sq_le, 8);
    memcpy(body + 16, &rq_le, 8);
    memcpy(body + 28, &id_ver, 4);
}

static uint16_t rq_flags(void)
{
    uint16_t be;

    memcpy(&be, g_mem + RQ_GPA + 8, 2);
    return be16toh(be);
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * The regression the stub caused: the driver recovers the state from the
 * high nibble of rq->state_pmtu, so leaving the buffer zeroed reads back as
 * RESET on a QP that is in RTS.
 */
static void test_state_reaches_the_side_buffer(void)
{
    uint8_t body[34];

    ctx_reset();
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);

    check("query-accepted", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body,
                                         sizeof(body)) == 0);
    check("query-used-backend-qpn", g_queried_qpn == QP_QPN);
    check("state-nibble-is-rts", (g_mem[RQ_GPA] >> 4) == 3);
    /* The driver computes path_mtu as (state_pmtu & 0xf) - 7. */
    check("mtu-nibble-decodes-to-1024",
          (int)(g_mem[RQ_GPA] & 0x0f) - 7 == g_path_mtu);
}

/* Each state has to survive the round trip, not just the one RTS case. */
static void test_every_state_round_trips(void)
{
    bool ok = true;

    for (uint8_t s = 0; s <= 6; s++) {
        uint8_t body[34];

        ctx_reset();
        g_state = s;
        build_body(body, QP_ID, SQ_GPA, RQ_GPA);
        if (dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body)) !=
                0 ||
            (g_mem[RQ_GPA] >> 4) != s)
            ok = false;
    }
    check("all-states-round-trip", ok);
}

/*
 * The driver ORs the two access_perms_flags fields together, so it does not
 * matter which buffer carries them -- but they have to be big-endian, and
 * they have to be in the ionic bit order rather than the IB one.
 */
static void test_access_flags_are_translated(void)
{
    uint8_t body[34];
    uint16_t sq_be;

    ctx_reset();
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body));

    memcpy(&sq_be, g_mem + SQ_GPA + 2, 2);
    /* IONIC_QPF_REMOTE_WRITE | IONIC_QPF_REMOTE_READ */
    check("access-flags-in-ionic-order", be16toh(sq_be) == 0x3);
    check("access-flags-in-both-buffers", rq_flags() == be16toh(sq_be));

    ctx_reset();
    g_access = 1u << 3; /* REMOTE_ATOMIC alone */
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body));
    check("atomic-flag-translated", rq_flags() == 0x4);
}

/* dest_qp_num rides in the sq buffer's qkey_dest_qpn, big-endian. */
static void test_dest_qpn_is_reported(void)
{
    uint8_t body[34];
    uint32_t be;

    ctx_reset();
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body));

    memcpy(&be, g_mem + SQ_GPA + 8, 4);
    check("dest-qpn-reported", be32toh(be) == g_dest_qpn);
}

/*
 * The two PSNs land in different buffers and in the opposite one to the
 * name a reader expects: the receive PSN is the last field of the sq
 * buffer, the send PSN the second field of the rq buffer.  Both are 24-bit
 * on the wire, so a resource manager that hands back more has to be
 * truncated rather than allowed to spill into the neighbouring field.
 */
static void test_psns_are_reported(void)
{
    uint8_t body[34];
    uint32_t rq_be, sq_be;

    ctx_reset();
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body));

    memcpy(&rq_be, g_mem + SQ_GPA + 16, 4);
    memcpy(&sq_be, g_mem + RQ_GPA + 4, 4);
    check("rq-psn-in-sq-buffer", be32toh(rq_be) == g_rq_psn);
    check("sq-psn-in-rq-buffer", be32toh(sq_be) == g_sq_psn);

    ctx_reset();
    g_rq_psn = 0xff000001;
    g_sq_psn = 0xff000002;
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body));

    memcpy(&rq_be, g_mem + SQ_GPA + 16, 4);
    memcpy(&sq_be, g_mem + RQ_GPA + 4, 4);
    check("psns-masked-to-24-bits", be32toh(rq_be) == 1 && be32toh(sq_be) == 2);
}

/*
 * A qp_id with no mapping must fail the command rather than answer with a
 * zeroed buffer, which the driver would read as a perfectly good QP in
 * RESET.
 */
static void test_unknown_qp_fails(void)
{
    uint8_t body[34];

    ctx_reset();
    build_body(body, QP_ID + 1, SQ_GPA, RQ_GPA);
    check("unknown-qp-rejected", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP,
                                              body, sizeof(body)) != 0);
    check("unknown-qp-wrote-nothing", g_mem[RQ_GPA] == 0);
}

/* Same for a resource manager that refuses the query. */
static void test_query_failure_fails_the_command(void)
{
    uint8_t body[34];

    ctx_reset();
    g_query_rc = -22;
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    check("query-failure-rejected",
          dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, sizeof(body)) !=
              0);
}

/* A short body has no addresses in it to trust. */
static void test_short_body_writes_nothing(void)
{
    uint8_t body[34];

    ctx_reset();
    build_body(body, QP_ID, SQ_GPA, RQ_GPA);
    (void)dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_QP, body, 33);
    check("short-body-wrote-nothing", g_mem[RQ_GPA] == 0 && g_mem[SQ_GPA] == 0);
}

int main(void)
{
    test_state_reaches_the_side_buffer();
    test_every_state_round_trips();
    test_access_flags_are_translated();
    test_dest_qpn_is_reported();
    test_psns_are_reported();
    test_unknown_qp_fails();
    test_query_failure_fails_the_command();
    test_short_body_writes_nothing();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
