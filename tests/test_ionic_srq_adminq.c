/*
 * Unit tests for the ionic emulator's SRQ admin commands.
 *
 * CREATE / MODIFY / QUERY / DESTROY_SRQ had no in-process coverage at all,
 * although every other handler in this file is reachable through
 * test_ionic_query_qp. That gap hid a real defect: CREATE_SRQ DMA'd the
 * allocated id into the driver's response buffer but left ctx->resp_len at
 * zero, and the driver refuses to read a response shorter than
 * IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN -- so it kept the zero its buffer came
 * with and every later MODIFY or DESTROY named SRQ 0. A test that inspected
 * both the buffer AND the length would have caught it; one that only checked
 * the return code would not, which is why these assert on both.
 *
 * The other thing pinned here is capacity. max_srq is advertised from
 * IONIC_EMU_SRQ_COUNT, and the id->handle map is sized from the same
 * constant, so the two cannot drift. Creating one past the end must FAIL:
 * before, the map logged "full" and returned, while CREATE_SRQ still handed
 * the guest an id that no lookup would ever resolve again -- which surfaces
 * much later as posts vanishing in the datapath rather than as an error from
 * ibv_create_srq().
 *
 * Like the other adminq tests, the translation unit is #included to reach
 * dispatch_wqe() and its external symbols are stubbed.
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

/* Only the QP's backend number is referenced, by the query_qp stub below;
 * this test drives the SRQ opcodes. */
#define QP_QPN 42u

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
void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id)
{
    (void)dp;
    (void)qp_id;
    (void)srq_id;
}
void ionic_datapath_register_ah(struct ionic_datapath *dp, uint32_t ah_id,
                                const uint8_t dgid[16], const uint8_t dmac[6],
                                uint32_t dest_node_id)
{
    (void)dp;
    (void)ah_id;
    (void)dgid;
    (void)dmac;
    (void)dest_node_id;
}
void ionic_datapath_unregister_ah(struct ionic_datapath *dp, uint32_t ah_id)
{
    (void)dp;
    (void)ah_id;
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
/* UINT32_MAX = no mesh backend attached, which is the loopback case. */
static uint32_t g_local_node = UINT32_MAX;
static uint32_t g_node_from_gid;

uint32_t ionic_dp_local_node(struct ionic_datapath *dp)
{
    (void)dp;
    return g_local_node;
}
uint32_t ionic_dp_node_from_gid(struct ionic_datapath *dp, const uint8_t *dgid)
{
    (void)dp;
    (void)dgid;
    return g_node_from_gid;
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

/* ---- Recording SRQ stubs ----------------------------------------------- */

#define SRQ_HANDLE_BASE 100u

static uint32_t g_alloc_calls;
static uint32_t g_dealloc_calls;
static uint32_t g_last_dealloc_handle;
static int g_alloc_rc;
static uint32_t g_modify_handle;
static uint32_t g_modify_limit;
static uint32_t g_registered_srq_id;
static uint32_t g_unregistered_srq_id;
static uint8_t g_registered_depth_log2;

int ionic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle)
{
    (void)handle;
    (void)pd_handle;
    (void)max_wr;
    (void)max_sge;
    (void)srq_limit;
    if (g_alloc_rc)
        return g_alloc_rc;
    /* Distinct per call, so a test can tell which allocation a lookup found. */
    *srq_handle = SRQ_HANDLE_BASE + g_alloc_calls++;
    return 0;
}

void ionic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle)
{
    (void)handle;
    g_dealloc_calls++;
    g_last_dealloc_handle = srq_handle;
}

int ionic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit)
{
    (void)handle;
    g_modify_handle = srq_handle;
    g_modify_limit = srq_limit;
    return 0;
}

int ionic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge, uint32_t *srq_limit)
{
    (void)handle;
    (void)srq_handle;
    if (max_wr)
        *max_wr = 64;
    if (max_sge)
        *max_sge = IONIC_MAX_SRQ_SGES;
    if (srq_limit)
        *srq_limit = 8;
    return 0;
}

void ionic_datapath_register_srq(struct ionic_datapath *dp, uint32_t srq_id,
                                 const struct ionic_dp_ring_desc *ring)
{
    (void)dp;
    g_registered_srq_id = srq_id;
    g_registered_depth_log2 = ring ? ring->depth_log2 : 0;
}

void ionic_datapath_unregister_srq(struct ionic_datapath *dp, uint32_t srq_id)
{
    (void)dp;
    g_unregistered_srq_id = srq_id;
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

#define RESP_GPA   256u
#define SRQ_DEPTH_LOG2 6
#define SRQ_LIMIT  12u

static void ctx_reset(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.pvrdma_handle = (pvrdma_handle_t)&g_ctx; /* only tested for NULL */

    memset(g_mem, 0, sizeof(g_mem));
    g_alloc_calls = 0;
    g_dealloc_calls = 0;
    g_last_dealloc_handle = 0;
    g_alloc_rc = 0;
    g_modify_handle = 0;
    g_modify_limit = 0;
    g_registered_srq_id = 0;
    g_unregistered_srq_id = 0;
    g_registered_depth_log2 = 0;
}

/* The 42-byte ionic_admin_create_srq body the driver posts. */
static void build_create(uint8_t *body, uint64_t resp_gpa, uint32_t resp_len)
{
    uint16_t limit_le = htole16(SRQ_LIMIT);
    uint32_t resp_len_le = htole32(resp_len);
    uint64_t resp_le = htole64(resp_gpa);

    memset(body, 0, IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN);
    memcpy(body + 22, &limit_le, 2);
    body[26] = SRQ_DEPTH_LOG2; /* depth_log2 */
    body[27] = 4;              /* stride_log2 */
    body[28] = 12;             /* page_size_log2 */
    memcpy(body + 30, &resp_len_le, 4);
    memcpy(body + 34, &resp_le, 8);
}

/* id-only bodies: MODIFY takes a 2-byte limit after it. */
static void build_id_body(uint8_t *body, size_t len, uint32_t srq_id)
{
    uint32_t id_le = htole32(srq_id);

    memset(body, 0, len);
    memcpy(body, &id_le, 4);
}

static uint32_t resp_srq_id(void)
{
    uint32_t le;

    memcpy(&le, g_mem + RESP_GPA, 4);
    return le32toh(le);
}

/* Create one SRQ and return the id the handler reported by DMA. */
static uint32_t create_one(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    if (dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body, sizeof(body)))
        return 0;
    return resp_srq_id();
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * The defect the missing coverage hid. The driver reads its response buffer
 * only when the reported length reaches IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN,
 * so writing the id without setting ctx->resp_len leaves it using SRQ id 0.
 * Both halves are asserted, because either alone passes while the pair is
 * broken.
 */
static void test_create_reports_id_and_length(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);

    check("create-accepted", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ,
                                          body, sizeof(body)) == 0);
    check("create-allocated-once", g_alloc_calls == 1);
    /* Above the QP id space, or an RQ doorbell cannot be told apart. */
    check("create-id-above-qp-space", resp_srq_id() > IONIC_SRQ_QID_BASE);
    check("create-resp-len-set",
          g_ctx.resp_len == IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    check("create-udma-idx-zero", g_mem[RESP_GPA + 4] == 0);
    /* The datapath needs the ring, not just the resource, or posted
     * receives land on an unknown queue. */
    check("create-registered-ring-with-datapath",
          g_registered_srq_id == resp_srq_id());
    check("create-ring-depth-passed-through",
          g_registered_depth_log2 == SRQ_DEPTH_LOG2);
}

/* A short body must be refused rather than read past its end. */
static void test_create_rejects_short_body(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);

    check("create-short-body-refused",
          dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                       IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN - 1) != 0);
    check("create-short-body-allocated-nothing", g_alloc_calls == 0);
}

/* MODIFY and QUERY have to resolve the id back to the allocated handle. */
static void test_modify_and_query_resolve_the_id(void)
{
    uint8_t body[8];
    uint32_t srq_id;
    uint16_t limit_le = htole16(99);

    ctx_reset();
    srq_id = create_one();
    check("modify-precondition-created", srq_id != 0);

    build_id_body(body, sizeof(body), srq_id);
    memcpy(body + 4, &limit_le, 2);
    check("modify-accepted", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ,
                                          body, 6) == 0);
    check("modify-reached-the-allocated-handle",
          g_modify_handle == SRQ_HANDLE_BASE);
    check("modify-carried-the-limit", g_modify_limit == 99);

    build_id_body(body, sizeof(body), srq_id);
    check("query-accepted", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_SRQ,
                                         body, 4) == 0);
}

/* An id the emulator never issued must not resolve to someone else's SRQ. */
static void test_unknown_id_is_refused(void)
{
    uint8_t body[8];

    ctx_reset();
    (void)create_one();

    build_id_body(body, sizeof(body), IONIC_SRQ_QID_BASE + 4242u);
    check("modify-unknown-id-refused",
          dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body, 6) != 0);
    check("query-unknown-id-refused",
          dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_SRQ, body, 4) != 0);
}

/* DESTROY frees the resource, tells the datapath, and retires the id. */
static void test_destroy_releases_everything(void)
{
    uint8_t body[8];
    uint32_t srq_id;

    ctx_reset();
    srq_id = create_one();

    build_id_body(body, sizeof(body), srq_id);
    check("destroy-accepted", dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_DESTROY_SRQ,
                                           body, 4) == 0);
    check("destroy-freed-the-resource", g_dealloc_calls == 1);
    check("destroy-freed-the-right-handle",
          g_last_dealloc_handle == SRQ_HANDLE_BASE);
    check("destroy-unregistered-from-datapath", g_unregistered_srq_id == srq_id);

    build_id_body(body, sizeof(body), srq_id);
    check("destroyed-id-no-longer-resolves",
          dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body, 6) != 0);
}

/*
 * Capacity, which is the whole point of tying MAX_SRQ_MAP to the advertised
 * IONIC_EMU_SRQ_COUNT. The map holds exactly what max_srq promises, and the
 * one past the end is refused -- not accepted with an id that resolves to
 * nothing, which is what a bare "map full" warning used to produce.
 */
static void test_map_capacity_is_enforced(void)
{
    uint32_t i, last = 0;

    ctx_reset();
    for (i = 0; i < MAX_SRQ_MAP; i++) {
        last = create_one();
        if (last == 0)
            break;
    }
    check("capacity-fills-to-advertised-max", i == MAX_SRQ_MAP && last != 0);

    g_dealloc_calls = 0;
    check("capacity-one-past-end-is-refused", create_one() == 0);
    /* Refused, and not leaked: the resource allocated for the attempt is
     * handed back rather than stranded. */
    check("capacity-refusal-frees-the-resource", g_dealloc_calls == 1);
}

int main(void)
{
    test_create_reports_id_and_length();
    test_create_rejects_short_body();
    test_modify_and_query_resolve_the_id();
    test_unknown_id_is_refused();
    test_destroy_releases_everything();
    test_map_capacity_is_enforced();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
