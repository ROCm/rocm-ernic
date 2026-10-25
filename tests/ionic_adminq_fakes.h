/*
 * Fakes for driving ionic_adminq_dispatch_wqe() in-process.
 *
 * Include it in a program that links ionic_adminq.c.  It backs the DMA entry
 * points with a plain buffer -- a "GPA" is an offset into g_mem -- records
 * every region handed out for writing, and stubs the admin queue's other
 * external symbols.  The stats counters come from knobs (g_dev_rc, g_qp_rc,
 * g_max) so a caller can make either snapshot fail or saturate.
 *
 * Shared by tests/test_ionic_stats.c and the fuzz harness
 * nix/analysis/fuzz/fuzz_ionic_stats.c, so the table and the fuzzer run
 * the handler against the same fake device, and by
 * tests/test_ionic_adminq_poll.c and tests/test_ionic_adminq_maps.c.  It
 * defines functions, so include it once, in one translation unit.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ADMINQ_FAKES_H
#define IONIC_ADMINQ_FAKES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

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
    /* Written so a huge addr cannot wrap the sum back into range. */
    if (max_nr_sgs < 1 || addr > GUEST_MEM_SIZE || len > GUEST_MEM_SIZE - addr)
        return -1;
    sg->addr = addr;
    sg->len = len;
    return 1;
}

/*
 * Every region handed out for writing, in order, so a caller can check
 * where -- and how much -- the handler wrote, not just what the buffer
 * ended up holding.
 */
#define MAX_WRITES 8
static struct fake_sg g_writes[MAX_WRITES];
static size_t g_nwrites;

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    const struct fake_sg *sg = (const struct fake_sg *)sgl;

    (void)vfu_ctx;
    (void)flags;
    if (cnt < 1)
        return -1;
    if (g_nwrites < MAX_WRITES)
        g_writes[g_nwrites] = *sg;
    g_nwrites++;
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


/* ---- Stubs for the admin queue's other external symbols --------------- */

#define QP_ID    7u
#define QP_QPN   42u
#define QP2_ID   9u
#define QP2_QPN  43u
#define BUF_GPA  1024u
#define PAGE_LEN 4096u
#define POISON   0xa5u

/* Not exercised here; a variable so QUERY_QP's error path stays live. */
static int g_query_rc;

/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
int ionic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn)
{
    (void)handle;
    (void)qpn;
    (void)state;
    (void)path_mtu;
    (void)dest_qpn;
    (void)access_flags;
    (void)rq_psn;
    (void)sq_psn;
    return g_query_rc;
}
/* NOLINTEND(readability-non-const-parameter) */

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
/* How many CQs and MRs were allocated. */
static unsigned g_alloc_cq_calls;
static unsigned g_alloc_mr_calls;

int ionic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe, uint32_t *cq_handle)
{
    (void)handle;
    (void)cqe;
    g_alloc_cq_calls++;
    *cq_handle = 1;
    return 0;
}
void ionic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle)
{
    (void)handle;
    (void)cq_handle;
}
/* How many QPs were allocated, and the IB type of the last one. */
static unsigned g_alloc_qp_calls;
static uint8_t g_alloc_qp_type;

int ionic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn)
{
    (void)handle;
    (void)pd_handle;
    g_alloc_qp_calls++;
    g_alloc_qp_type = qp_type;
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
    g_alloc_mr_calls++;
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
void ionic_datapath_flush_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    (void)dp;
    (void)qp_id;
}
int ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                               uint64_t va, uint64_t length,
                               const struct ionic_dp_buf_desc *buf)
{
    (void)dp;
    (void)lkey;
    (void)va;
    (void)length;
    (void)buf;
    return 0;
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

uint32_t ionic_dp_local_node(const struct ionic_datapath *dp)
{
    (void)dp;
    return g_local_node;
}
uint32_t ionic_dp_node_from_gid(const struct ionic_datapath *dp,
                                const uint8_t *dgid)
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

/* SRQ and address-handle entry points: reached only by opcodes the
 * statistics tests never send, so they refuse or do nothing. */
/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
int ionic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle)
{
    (void)handle;
    (void)pd_handle;
    (void)max_wr;
    (void)max_sge;
    (void)srq_limit;
    (void)srq_handle;
    return -1;
}
/* NOLINTEND(readability-non-const-parameter) */
void ionic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle)
{
    (void)handle;
    (void)srq_handle;
}
int ionic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit)
{
    (void)handle;
    (void)srq_handle;
    (void)srq_limit;
    return -1;
}
/* The fake keeps the real function's signature, though it never
 * writes the outputs */
/* NOLINTBEGIN(readability-non-const-parameter) */
int ionic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge, uint32_t *srq_limit)
{
    (void)handle;
    (void)srq_handle;
    (void)max_wr;
    (void)max_sge;
    (void)srq_limit;
    return -1;
}
/* NOLINTEND(readability-non-const-parameter) */
int ionic_datapath_register_srq(struct ionic_datapath *dp, uint32_t srq_id,
                                const struct ionic_dp_ring_desc *rq)
{
    (void)dp;
    (void)srq_id;
    (void)rq;
    return -1;
}
void ionic_datapath_unregister_srq(struct ionic_datapath *dp, uint32_t srq_id)
{
    (void)dp;
    (void)srq_id;
}
void ionic_datapath_bind_qp_srq(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t srq_id)
{
    (void)dp;
    (void)qp_id;
    (void)srq_id;
}
void ionic_datapath_set_qkey(struct ionic_datapath *dp, uint32_t qp_id,
                             uint32_t qkey)
{
    (void)dp;
    (void)qp_id;
    (void)qkey;
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

/*
 * The counter snapshots the handler reads.  Every value is distinct and
 * free of POISON bytes, so a value landing in the wrong slot, or a slot
 * left unwritten, shows up as a mismatch.  QP counters also encode the
 * qp_id they were asked for, which is how a row proves the handler picked
 * the right QP.
 */
static int g_dev_rc;
static int g_qp_rc;
static bool g_max;
static uint32_t g_counters_qp_id;

static uint64_t dev_value(size_t i)
{
    return g_max ? UINT64_MAX : 0x1000u + i + 1;
}

static uint64_t qp_value(uint32_t qp_id, size_t i)
{
    return g_max ? UINT64_MAX : ((uint64_t)qp_id << 8) + i + 1;
}

/*
 * Build the values in a local array and copy them in once.  Writing each
 * value at a byte offset into the caller's struct makes gcc 13's -O3
 * stringop-overflow analysis lose track of the destination size.
 */
#define FAKE_COUNTERS_MAX 64

static void fill_counters(void *out, size_t size, uint64_t (*value)(size_t))
{
    uint64_t vals[FAKE_COUNTERS_MAX] = {0};
    size_t n = size / sizeof(vals[0]);

    if (n > FAKE_COUNTERS_MAX)
        abort();
    for (size_t i = 0; i < n; i++)
        vals[i] = value(i);
    memcpy(out, vals, n * sizeof(vals[0]));
}

static uint64_t qp_value_for_counters(size_t i)
{
    return qp_value(g_counters_qp_id, i);
}

int pvrdma_get_dev_counters(pvrdma_handle_t handle,
                            struct pvrdma_dev_counters *out)
{
    (void)handle;
    memset(out, 0, sizeof(*out));
    if (g_dev_rc)
        return g_dev_rc;
    fill_counters(out, sizeof(*out), dev_value);
    return 0;
}

int pvrdma_get_qp_counters(pvrdma_handle_t handle, uint32_t qp_id,
                           struct pvrdma_qp_counters *out)
{
    (void)handle;
    memset(out, 0, sizeof(*out));
    if (g_qp_rc)
        return g_qp_rc;
    g_counters_qp_id = qp_id;
    fill_counters(out, sizeof(*out), qp_value_for_counters);
    return 0;
}

/*
 * The data path's RDMA counters follow the pvrdma ones in each table, so
 * they continue the same value sequence: entry i of a table always holds
 * value(i), whichever snapshot it came from.  The qp_id the handler asked
 * for is recorded, as for the pvrdma snapshot.
 */
static uint32_t g_rdma_qp_id;

void ionic_datapath_rdma_counters(const struct ionic_datapath *dp,
                                  uint32_t qp_id,
                                  uint64_t out[IONIC_DP_RDMA_COUNT])
{
    const size_t dev_first = sizeof(struct pvrdma_dev_counters) / 8;
    const size_t qp_first = sizeof(struct pvrdma_qp_counters) / 8;

    (void)dp;
    g_rdma_qp_id = qp_id;
    for (size_t i = 0; i < IONIC_DP_RDMA_COUNT; i++)
        out[i] = qp_id == IONIC_DP_DEVICE ? dev_value(dev_first + i)
                                          : qp_value(qp_id, qp_first + i);
}

/* ---- Fixture ----------------------------------------------------------- */

/*
 * The fakes stand in for the bridge, so they also supply the device it would
 * hand out. The code under test only compares the handle with NULL.
 */
struct PVRDMADev {
    int unused;
};
static struct PVRDMADev g_fake_pvrdma;

static struct ionic_adminq_ctx g_ctx;
/* Made by the first ctx_reset() and kept for the whole run: the fuzz
 * harness that shares this file has no main() of its own to make it. */
static struct ernic_dma *g_dma;

/* Two mapped QPs, a poisoned guest memory, and counters that succeed. */
static void ctx_reset(void)
{
    if (!g_dma) {
        g_dma = ernic_dma_create();
        if (!g_dma)
            abort();
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.dma = g_dma;
    g_ctx.pvrdma_handle = &g_fake_pvrdma;
    g_ctx.qp_map[0].valid = true;
    g_ctx.qp_map[0].qp_id = QP_ID;
    g_ctx.qp_map[0].qpn = QP_QPN;
    g_ctx.qp_map[1].valid = true;
    g_ctx.qp_map[1].qp_id = QP2_ID;
    g_ctx.qp_map[1].qpn = QP2_QPN;

    memset(g_mem, POISON, sizeof(g_mem));
    g_dev_rc = 0;
    g_qp_rc = 0;
    g_max = false;
    g_counters_qp_id = UINT32_MAX;
    g_rdma_qp_id = 0;
    g_nwrites = 0;
    g_alloc_qp_calls = 0;
    g_alloc_qp_type = 0;
    g_alloc_cq_calls = 0;
    g_alloc_mr_calls = 0;
}

#endif /* IONIC_ADMINQ_FAKES_H */
