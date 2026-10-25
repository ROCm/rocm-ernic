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
 * Like the other adminq tests, it links ionic_adminq.c, drives
 * ionic_adminq_dispatch_wqe() through ionic_adminq_priv.h, and stubs the
 * external symbols.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_dma.h"
#include "ionic_adminq_priv.h"
#include "ionic_datapath.h"
#include "rocm_ernic_compat.h"

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

int ionic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn)
{
    (void)handle;
    (void)qpn;
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
void ionic_rm_dealloc_pd(pvrdma_handle_t handle, uint32_t pd_handle)
{
    (void)handle;
    (void)pd_handle;
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
/* Counted so a refused CREATE_QP can be shown to allocate nothing: the
 * check that rejects an unknown SRQ has to run before this, or every
 * rejected command leaks a backend QP. */
static uint32_t g_qp_alloc_calls;

int ionic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn)
{
    g_qp_alloc_calls++;
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
void ionic_datapath_reset_rdma(struct ionic_datapath *dp)
{
    (void)dp;
}
/* This test drives the SRQ opcodes; the counters a VALS would read are all
 * zero, which is what a QP that has seen no error reports anyway. */
void ionic_datapath_rdma_counters(const struct ionic_datapath *dp,
                                  uint32_t qp_id,
                                  uint64_t out[IONIC_DP_RDMA_COUNT])
{
    (void)dp;
    (void)qp_id;
    memset(out, 0, sizeof(*out) * IONIC_DP_RDMA_COUNT);
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
/* Which SRQ a QP was bound to, and whether the bind happened at all:
 * CREATE_QP must refuse an id that names no SRQ rather than binding
 * to an empty slot. */
static uint32_t g_bind_calls;
static uint32_t g_bound_srq_id;

void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id)
{
    (void)dp;
    (void)qp_id;
    g_bind_calls++;
    g_bound_srq_id = srq_id;
}
void ionic_datapath_register_ah(struct ionic_datapath *dp, uint32_t ah_id,
                                const uint8_t dgid[16], const uint8_t sgid[16],
                                const uint8_t dmac[6], uint32_t dest_node_id)
{
    (void)dp;
    (void)ah_id;
    (void)dgid;
    (void)sgid;
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
void ionic_datapath_set_qkey(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t qkey)
{
    (void)dp;
    (void)qp_id;
    (void)qkey;
}

void ionic_datapath_flush_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    (void)dp;
    (void)qp_id;
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
/* The statistics commands are not under test here. */
int pvrdma_get_dev_counters(pvrdma_handle_t handle,
                            struct pvrdma_dev_counters *out)
{
    (void)handle;
    memset(out, 0, sizeof(*out));
    return 0;
}
int pvrdma_get_qp_counters(pvrdma_handle_t handle, uint32_t qp_id,
                           struct pvrdma_qp_counters *out)
{
    (void)handle;
    (void)qp_id;
    memset(out, 0, sizeof(*out));
    return 0;
}

/* ---- Recording SRQ stubs ----------------------------------------------- */

#define SRQ_HANDLE_BASE 100u

static uint32_t g_alloc_calls;
static uint32_t g_dealloc_calls;
static uint32_t g_last_dealloc_handle;
static int g_alloc_rc;
static uint32_t g_modify_handle;
static uint32_t g_modify_limit;
static uint32_t g_modify_calls;
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
    /* Mirrors rdma_rm_modify_srq(), which rejects a limit of 0 outright.
     * Reproducing that here is what makes the limit-0 case below a test:
     * a handler that forwards 0 to the resource manager fails, as it did. */
    if (srq_limit == 0)
        return -EINVAL;
    g_modify_handle = srq_handle;
    g_modify_limit = srq_limit;
    g_modify_calls++;
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

/* Set by a test that wants registration to fail, so the handler's unwind
 * path can be driven without a datapath. */
static int g_register_srq_rc;

int ionic_datapath_register_srq(struct ionic_datapath *dp, uint32_t srq_id,
                                const struct ionic_dp_ring_desc *ring)
{
    (void)dp;
    if (g_register_srq_rc)
        return g_register_srq_rc;
    g_registered_srq_id = srq_id;
    g_registered_depth_log2 = ring ? ring->depth_log2 : 0;
    return 0;
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

/*
 * The fakes stand in for the bridge, so they also supply the device it would
 * hand out. The code under test only compares the handle with NULL.
 */
struct PVRDMADev {
    int unused;
};
static struct PVRDMADev g_fake_pvrdma;

static struct ionic_adminq_ctx g_ctx;
/* Made in main(); outlives every ctx_reset(). */
static struct ernic_dma *g_dma;

#define RESP_GPA       256u
#define SRQ_DEPTH_LOG2 6
#define SRQ_LIMIT      12u

static void ctx_reset(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.dma = g_dma;
    g_ctx.pvrdma_handle = &g_fake_pvrdma;

    memset(g_mem, 0, sizeof(g_mem));
    g_alloc_calls = 0;
    g_dealloc_calls = 0;
    g_last_dealloc_handle = 0;
    g_alloc_rc = 0;
    g_modify_handle = 0;
    g_modify_limit = 0;
    g_modify_calls = 0;
    g_bind_calls = 0;
    g_qp_alloc_calls = 0;
    g_bound_srq_id = 0;
    g_registered_srq_id = 0;
    g_unregistered_srq_id = 0;
    g_registered_depth_log2 = 0;
}

/* The 42-byte ionic_admin_create_srq body the driver posts. */
static void build_create(uint8_t *body, uint64_t resp_gpa, uint32_t resp_len)
{
    memset(body, 0, IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN);
    ernic_put_le16(body + 22, SRQ_LIMIT);
    body[26] = SRQ_DEPTH_LOG2; /* depth_log2 */
    /* 6 is what the ionic driver actually asks for: a 64-byte SRQ WQE.
     * It also has to clear IONIC_SRQ_MIN_STRIDE_LOG2, which this used
     * to sit below. */
    body[27] = 6;  /* stride_log2 */
    body[28] = 12; /* page_size_log2 */
    ernic_put_le32(body + 30, resp_len);
    ernic_put_le64(body + 34, resp_gpa);
}

/* id-only bodies: MODIFY takes a 2-byte limit after it. */
static void build_id_body(uint8_t *body, size_t len, uint32_t srq_id)
{
    memset(body, 0, len);
    ernic_put_le32(body, srq_id);
}

static uint32_t resp_srq_id(void)
{
    return ernic_get_le32(g_mem + RESP_GPA);
}

/* Create one SRQ and return the id the handler reported by DMA. */
static uint32_t create_one(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    if (ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                  sizeof(body)))
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

    check("create-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) == 0);
    check("create-allocated-once", g_alloc_calls == 1);
    /* At or above the QP id space, or an RQ doorbell cannot be told apart.
     * Not strictly above: the first id is the base itself now that ids come
     * from the map slot. The allocator this replaced started at base + 1 and
     * never used slot 0, which is what the old ">" here encoded. */
    check("create-id-at-or-above-qp-space",
          resp_srq_id() >= IONIC_SRQ_QID_BASE);
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
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN - 1) != 0);
    check("create-short-body-allocated-nothing", g_alloc_calls == 0);
}

/* MODIFY and QUERY have to resolve the id back to the allocated handle. */
static void test_modify_and_query_resolve_the_id(void)
{
    uint8_t body[8];
    uint32_t srq_id;

    ctx_reset();
    srq_id = create_one();
    check("modify-precondition-created", srq_id != 0);

    build_id_body(body, sizeof(body), srq_id);
    ernic_put_le16(body + 4, 99);
    check("modify-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) == 0);
    check("modify-reached-the-allocated-handle",
          g_modify_handle == SRQ_HANDLE_BASE);
    check("modify-carried-the-limit", g_modify_limit == 99);

    build_id_body(body, sizeof(body), srq_id);
    check("query-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_SRQ, body,
                                    4) == 0);
}

/* An id the emulator never issued must not resolve to someone else's SRQ. */
static void test_unknown_id_is_refused(void)
{
    uint8_t body[8];

    ctx_reset();
    (void)create_one();

    build_id_body(body, sizeof(body), IONIC_SRQ_QID_BASE + 4242u);
    check("modify-unknown-id-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) != 0);
    check("query-unknown-id-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_SRQ, body,
                                    4) != 0);
}

/* DESTROY frees the resource, tells the datapath, and retires the id. */
static void test_destroy_releases_everything(void)
{
    uint8_t body[8];
    uint32_t srq_id;

    ctx_reset();
    srq_id = create_one();

    build_id_body(body, sizeof(body), srq_id);
    check("destroy-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_DESTROY_SRQ, body,
                                    4) == 0);
    check("destroy-freed-the-resource", g_dealloc_calls == 1);
    check("destroy-freed-the-right-handle",
          g_last_dealloc_handle == SRQ_HANDLE_BASE);
    check("destroy-unregistered-from-datapath",
          g_unregistered_srq_id == srq_id);

    build_id_body(body, sizeof(body), srq_id);
    check("destroyed-id-no-longer-resolves",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) != 0);
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

/*
 * Every id the allocator hands out has to index the datapath's SRQ table,
 * which srq_by_qid() addresses as srq_id - IONIC_SRQ_QID_BASE over
 * IONIC_EMU_SRQ_COUNT entries. An id past the end is not caught anywhere
 * downstream: srq_by_qid() returns NULL, registration does nothing, and the
 * guest is handed a queue that accepts posts and completes none of them.
 */
static void test_ids_stay_inside_the_datapath_table(void)
{
    uint32_t i, id, worst = 0;

    ctx_reset();
    for (i = 0; i < MAX_SRQ_MAP; i++) {
        id = create_one();
        if (id == 0)
            break;
        if (id - IONIC_SRQ_QID_BASE > worst)
            worst = id - IONIC_SRQ_QID_BASE;
    }
    check("range-filled-the-map", i == MAX_SRQ_MAP);
    check("range-every-id-indexes-the-table", worst < IONIC_EMU_SRQ_COUNT);
    /* Slot 0 is usable; the counter that preceded this skipped it. */
    check("range-uses-the-first-slot", g_registered_srq_id != 0);
}

/*
 * The id space must track live SRQs, not cumulative creates. This is the
 * regression test for the allocator that only ever counted up: with one SRQ
 * alive at a time, it drifted one id further out on every cycle and left the
 * table after 255 of them, while the map -- which recycles slots -- stayed
 * almost empty and reported nothing wrong.
 */
static void test_ids_are_recycled_across_churn(void)
{
    uint8_t body[8];
    uint32_t id = 0;
    bool in_range = true;

    ctx_reset();
    for (uint32_t i = 0; i < MAX_SRQ_MAP * 2; i++) {
        id = create_one();
        if (id == 0 || id - IONIC_SRQ_QID_BASE >= IONIC_EMU_SRQ_COUNT) {
            in_range = false;
            break;
        }
        build_id_body(body, sizeof(body), id);
        if (ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_DESTROY_SRQ, body,
                                      4) != 0) {
            in_range = false;
            break;
        }
    }
    check("churn-ids-stay-in-the-table", in_range);
}

/*
 * depth_log2 and stride_log2 are guest bytes that get shifted by. A value of
 * 32 or more makes "1u << n" undefined in C, and an oversized stride decides
 * how much of a WQE the receive path walks out of a fixed buffer. Both are
 * refused at the boundary, before anything shifts.
 */
static void test_oversized_exponents_are_refused(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[26] = 32; /* depth_log2: the shift-by-32 that is UB */
    check("depth-exponent-32-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("depth-exponent-32-allocated-nothing", g_alloc_calls == 0);

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[26] = IONIC_SRQ_MAX_DEPTH_LOG2 + 1;
    check("depth-exponent-past-advertised-max-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[27] = IONIC_SRQ_MAX_STRIDE_LOG2 + 1;
    check("stride-exponent-past-wqe-buffer-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("stride-exponent-refusal-allocated-nothing", g_alloc_calls == 0);

    /*
     * A stride too SMALL is the other half, and it is not a shift problem.
     * deliver_recv() reads min(stride, sizeof rwqe) bytes and then takes the
     * work request id from offset 0 and the SGE count from offset 9, so a
     * stride below the payload offset leaves both reads coming off whatever
     * happened to be on the stack.
     */
    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[27] = IONIC_SRQ_MIN_STRIDE_LOG2 - 1;
    check("stride-exponent-below-the-wqe-header-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("small-stride-refusal-allocated-nothing", g_alloc_calls == 0);

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[27] = 0;
    check("stride-exponent-zero-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);

    /* Both ends of the advertised range still work, so these are bounds and
     * not off-by-ones. */
    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[26] = IONIC_SRQ_MAX_DEPTH_LOG2;
    body[27] = IONIC_SRQ_MAX_STRIDE_LOG2;
    check("max-advertised-geometry-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) == 0);

    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    body[27] = IONIC_SRQ_MIN_STRIDE_LOG2;
    check("min-advertised-stride-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) == 0);
}

/*
 * Registration failing is the command failing. The handler used to ignore a
 * void return, DMA an id back and report success, leaving the guest owning
 * an SRQ with no receive ring -- a queue that accepts posts and completes
 * none of them, which is the silent hang this feature exists to remove.
 */
static void test_registration_failure_fails_the_command(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    ctx_reset();
    g_register_srq_rc = -EIO;
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);

    check("register-failure-refuses-create",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("register-failure-frees-the-resource", g_dealloc_calls == 1);
    check("register-failure-wrote-no-id", g_mem[RESP_GPA] == 0);
    g_register_srq_rc = 0;

    /* And the slot it took is handed back, not stranded. */
    check("register-failure-released-the-slot", create_one() != 0);
}

/*
 * srq_limit == 0 is the legal "no low-watermark" value, and the guest driver
 * accepts it. The resource manager does not -- rdma_rm_modify_srq() returns
 * -EINVAL for it -- so routing every limit through the resource manager
 * turned a valid ibv_modify_srq(srq_limit=0) into an error. The map holds
 * the authoritative value; only non-zero limits go any further.
 */
static void test_zero_limit_is_accepted(void)
{
    uint8_t body[8];
    uint32_t srq_id;

    ctx_reset();
    srq_id = create_one();
    check("zero-limit-precondition-created", srq_id != 0);

    build_id_body(body, sizeof(body), srq_id);
    ernic_put_le16(body + 4, 0);
    check("zero-limit-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) == 0);
    check("zero-limit-never-reached-the-resource-manager", g_modify_calls == 0);

    /* A real limit still goes through, so this is not a blanket bypass. */
    build_id_body(body, sizeof(body), srq_id);
    ernic_put_le16(body + 4, 4);
    check("nonzero-limit-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) == 0);
    check("nonzero-limit-forwarded",
          g_modify_calls == 1 && g_modify_limit == 4);

    /* And QUERY reports what was set, including back down to zero, which
     * the resource manager's copy could never show. */
    build_id_body(body, sizeof(body), srq_id);
    ernic_put_le16(body + 4, 0);
    check("zero-limit-accepted-again",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_MODIFY_SRQ, body,
                                    6) == 0);
    build_id_body(body, sizeof(body), srq_id);
    check("query-after-zero-limit-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_QUERY_SRQ, body,
                                    4) == 0);
}

/*
 * CREATE_QP carries the SRQ to bind to in rq_tbl_index_srq_id, straight
 * from the guest. ionic_datapath_bind_qp_srq() range-checks it, but
 * srq_by_qid() hands back the slot whether or not an SRQ lives there, so an
 * in-range id that was never created used to bind in silence. A QP bound
 * that way has no receive ring of its own and no shared one either: every
 * receive goes to a ring that is not there and nothing ever completes.
 *
 * Minimal valid bodies -- only the fields the handler reads.
 */
#define TEST_CQ_ID 3u
#define TEST_QP_ID 4u

static void build_cq_body(uint8_t body[34], uint32_t cq_id)
{
    memset(body, 0, 34);
    body[4] = 6;  /* depth_log2  */
    body[5] = 5;  /* stride_log2 */
    body[7] = 12; /* page_size_log2 */
    ernic_put_le32(body + 12, cq_id);
}

/* 64 rather than 60: the handler reads type_state at body[62]. */
static void build_qp_body(uint8_t body[64], uint32_t qp_id, uint32_t cq_id,
                          uint32_t srq_id)
{
    memset(body, 0, 64);
    ernic_put_le32(body + 8, cq_id);  /* sq_cq_id */
    body[12] = 4;                     /* sq_depth_log2 */
    body[13] = 7;                     /* sq_stride_log2 */
    ernic_put_le32(body + 32, cq_id); /* rq_cq_id */
    /* A QP bound to an SRQ has no RQ, and the driver leaves its geometry
     * zero. */
    if (srq_id == 0xffffffffu) {
        body[36] = 4; /* rq_depth_log2 */
        body[37] = 7; /* rq_stride_log2 */
    }
    ernic_put_le32(body + 40, srq_id);
    ernic_put_le32(body + 56, qp_id);
    body[62] = 0; /* ionic qp type 0 -> IB_QPT_RC */
}

static void test_qp_binding_checks_the_srq_id(void)
{
    uint8_t cqb[34];
    uint8_t qpb[64];
    uint32_t srq_id;

    ctx_reset();
    build_cq_body(cqb, TEST_CQ_ID);
    check("bind-precondition-cq",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, cqb,
                                    sizeof(cqb)) == 0);
    srq_id = create_one();
    check("bind-precondition-srq", srq_id != 0);

    /* The id CREATE_SRQ handed out binds, which is the path the driver
     * actually takes -- the guard must not break it. */
    build_qp_body(qpb, TEST_QP_ID, TEST_CQ_ID, srq_id);
    check("bind-known-srq-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    sizeof(qpb)) == 0);
    check("bind-known-srq-bound-once", g_bind_calls == 1);
    check("bind-known-srq-bound-to-it", g_bound_srq_id == srq_id);

    /* In range for the datapath table, but no SRQ was ever created there. */
    ctx_reset();
    build_cq_body(cqb, TEST_CQ_ID);
    (void)ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, cqb,
                                    sizeof(cqb));
    build_qp_body(qpb, TEST_QP_ID, TEST_CQ_ID, IONIC_SRQ_QID_BASE + 7);
    check("bind-unknown-srq-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    sizeof(qpb)) != 0);
    check("bind-unknown-srq-never-bound", g_bind_calls == 0);
    /* And refused before anything was allocated. Checking after
     * ionic_rm_alloc_qp() would turn each rejected command into a
     * leaked backend QP, so a guest could drain the table with
     * commands the device is busy refusing. */
    check("bind-unknown-srq-allocated-no-qp", g_qp_alloc_calls == 0);

    /* Out of the SRQ range entirely, which srq_by_qid() would reject but
     * the handler should refuse first. */
    ctx_reset();
    build_cq_body(cqb, TEST_CQ_ID);
    (void)ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, cqb,
                                    sizeof(cqb));
    build_qp_body(qpb, TEST_QP_ID, TEST_CQ_ID, 1u);
    check("bind-out-of-range-srq-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    sizeof(qpb)) != 0);
    check("bind-out-of-range-never-bound", g_bind_calls == 0);
    check("bind-out-of-range-allocated-no-qp", g_qp_alloc_calls == 0);

    /* ~0 is "no SRQ" and has to stay acceptable: every ordinary QP sends
     * it, so refusing it would break the device rather than harden it. */
    ctx_reset();
    build_cq_body(cqb, TEST_CQ_ID);
    (void)ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, cqb,
                                    sizeof(cqb));
    build_qp_body(qpb, TEST_QP_ID, TEST_CQ_ID, 0xffffffffu);
    check("no-srq-sentinel-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    sizeof(qpb)) == 0);
    check("no-srq-sentinel-never-bound", g_bind_calls == 0);
}

/*
 * The short-body guard has to cover every field the handler reads, and the
 * last one is type_state at [62]. It used to stop at 60, which covers
 * id_ver at [56:59] and nothing after, so a 60-byte body still had its QP
 * type -- and therefore whether the QP was RC, UC or UD -- taken from three
 * bytes the guest never sent.
 */
static void test_create_qp_body_must_reach_type_state(void)
{
    uint8_t cqb[34];
    uint8_t qpb[64];

    ctx_reset();
    build_cq_body(cqb, TEST_CQ_ID);
    (void)ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, cqb,
                                    sizeof(cqb));
    build_qp_body(qpb, TEST_QP_ID, TEST_CQ_ID, 0xffffffffu);

    /* One short of the last field. */
    check("create-qp-body-62-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    IONIC_ADMIN_CREATE_QP_IN_V1_LEN - 1) != 0);
    /* The old guard's value, which reads three bytes past what was sent. */
    check("create-qp-body-60-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    60) != 0);
    /* Exactly enough is enough. */
    check("create-qp-body-63-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, qpb,
                                    IONIC_ADMIN_CREATE_QP_IN_V1_LEN) == 0);
}

/*
 * An SRQ whose id never reaches the guest is one the guest can neither use
 * nor free: it holds a table slot until the device is reset, and the map has
 * only IONIC_EMU_SRQ_COUNT of them. The handler used to skip the response
 * write when the buffer was too short or its address zero, and then report
 * success -- leaving the driver holding the zero its buffer came with, which
 * names IONIC_SRQ_QID_BASE + 0, a real and probably someone else's queue.
 */
static void test_create_fails_when_the_id_cannot_be_returned(void)
{
    uint8_t body[IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN];

    /* Response buffer too short for the five bytes the driver expects. */
    ctx_reset();
    build_create(body, RESP_GPA, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN - 1);
    check("short-response-buffer-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("short-response-buffer-freed-the-resource", g_dealloc_calls == 1);
    check("short-response-buffer-unregistered",
          g_unregistered_srq_id != 0 || g_registered_srq_id == 0);

    /* The slot is handed back, so this does not leak one per attempt. */
    check("short-response-buffer-released-the-slot", create_one() != 0);

    /* No response address at all. */
    ctx_reset();
    build_create(body, 0, IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
    check("zero-response-address-refused",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_SRQ, body,
                                    sizeof(body)) != 0);
    check("zero-response-address-freed-the-resource", g_dealloc_calls == 1);
    check("zero-response-address-released-the-slot", create_one() != 0);
}

int main(void)
{
    g_dma = ernic_dma_create();
    if (!g_dma) {
        fprintf(stderr, "ernic_dma_create failed\n");
        return 1;
    }

    test_create_reports_id_and_length();
    test_create_rejects_short_body();
    test_modify_and_query_resolve_the_id();
    test_unknown_id_is_refused();
    test_destroy_releases_everything();
    test_map_capacity_is_enforced();
    test_ids_stay_inside_the_datapath_table();
    test_ids_are_recycled_across_churn();
    test_oversized_exponents_are_refused();
    test_registration_failure_fails_the_command();
    test_zero_limit_is_accepted();
    test_qp_binding_checks_the_srq_id();
    test_create_qp_body_must_reach_type_state();
    test_create_fails_when_the_id_cannot_be_returned();

    ernic_dma_destroy(g_dma);
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
