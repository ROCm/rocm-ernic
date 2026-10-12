/*
 * Unit tests for the ionic emulator's hardware statistics admin commands.
 *
 * STATS_HDRS and QP_STATS_HDRS answer with a table of struct ionic_v1_stat
 * entries that the driver turns into its hw_counters files; STATS_VALS and
 * QP_STATS_VALS answer with the LE64 values at the offsets those headers
 * give.  While the four opcodes were stubs, the driver never saw a header
 * table at all, so a guest had no hw_counters directory to read.
 *
 * The tests are one table: each row posts one command through
 * ionic_adminq_dispatch_wqe() against a fake guest memory pre-filled with a
 * poison byte, and checks the completion status, how many bytes the handler
 * wrote, and what it wrote.  The test links ionic_adminq.c, reaches its context
 * and statistics tables through ionic_adminq_priv.h, and stubs its external
 * symbols.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

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

/* SRQ and address-handle entry points: reached only by opcodes the
 * statistics tests never send, so they refuse or do nothing. */
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

static void fill_counters(void *out, size_t size, uint64_t (*value)(size_t))
{
    uint8_t *p = out;

    for (size_t i = 0; i < size / sizeof(uint64_t); i++) {
        uint64_t v = value(i);

        memcpy(p + i * sizeof(v), &v, sizeof(v));
    }
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

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

static struct ionic_adminq_ctx g_ctx;

/* Two mapped QPs, a poisoned guest memory, and counters that succeed. */
static void ctx_reset(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.pvrdma_handle = (pvrdma_handle_t)&g_ctx; /* only tested for NULL */
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
}

/* The 17-byte ionic_admin_stats_hdr body, in a buffer of @len bytes. */
static void build_body(uint8_t *body, size_t len, uint64_t dma, uint32_t length,
                       uint32_t id_ver)
{
    uint64_t dma_le = htole64(dma);
    uint32_t length_le = htole32(length), id_ver_le = htole32(id_ver);

    memset(body, 0, len);
    memcpy(body, &dma_le, 8);
    memcpy(body + 8, &length_le, 4);
    memcpy(body + 12, &id_ver_le, 4);
}

/* Bytes from @gpa up to and including the last one that is not POISON. */
static size_t bytes_written(uint64_t gpa)
{
    size_t n = 0;

    for (size_t i = 0; gpa + i < GUEST_MEM_SIZE; i++)
        if (g_mem[gpa + i] != POISON)
            n = i + 1;
    return n;
}

/*
 * The first @entries headers at @gpa are @stats in order, each an LE64 at
 * the next 8-byte slot with a NUL-padded name; an entry past the table is
 * the all-zero NONE terminator.
 */
static bool hdrs_match(uint64_t gpa, const struct adminq_stat *stats,
                       size_t count, size_t entries)
{
    for (size_t i = 0; i < entries; i++) {
        const uint8_t *e = g_mem + gpa + i * STAT_HDR_SIZE;
        char name[STAT_NAME_LEN];
        uint32_t be;

        memcpy(&be, e, 4);
        memcpy(name, e + 4, sizeof(name));
        if (i >= count) {
            static const uint8_t zero[STAT_HDR_SIZE];

            if (memcmp(e, zero, sizeof(zero)) != 0)
                return false;
            continue;
        }
        if (be32toh(be) >> STAT_TYPE_SHIFT != STAT_TYPE_LE64 ||
            (be32toh(be) & 0x0fffffffu) != i * STAT_VAL_SIZE)
            return false;
        if (name[STAT_NAME_LEN - 1] != '\0' || strcmp(name, stats[i].name) != 0)
            return false;
        for (size_t j = strlen(name); j < sizeof(name); j++)
            if (name[j] != '\0')
                return false;
    }
    return true;
}

/* The first @n LE64 values at @gpa are value(0) .. value(n - 1). */
static bool vals_match(uint64_t gpa, size_t n, uint64_t (*value)(size_t))
{
    for (size_t i = 0; i < n; i++) {
        uint64_t le;

        memcpy(&le, g_mem + gpa + i * STAT_VAL_SIZE, 8);
        if (le64toh(le) != value(i))
            return false;
    }
    return true;
}

static uint64_t qp_value_for_qp(size_t i)
{
    return qp_value(QP_ID, i);
}

static uint64_t qp_value_for_qp2(size_t i)
{
    return qp_value(QP2_ID, i);
}

/* ---- Tests ------------------------------------------------------------- */

enum content {
    CONTENT_NONE,     /* nothing beyond rc and bytes written */
    CONTENT_DEV_HDRS, /* dev_stats headers, then the terminator if it fit */
    CONTENT_QP_HDRS,  /* qp_stats headers, then the terminator if it fit */
    CONTENT_DEV_VALS, /* the device counters */
    CONTENT_QP_VALS,  /* QP_ID's counters */
    CONTENT_QP2_VALS, /* QP2_ID's counters */
};

struct stats_case {
    const char *category;
    const char *description;
    const char *expected;
    uint8_t op;
    uint64_t dma;
    uint32_t length;
    uint32_t id_ver;
    uint16_t body_len;
    int dev_rc; /* pvrdma_get_dev_counters() result */
    int qp_rc;  /* pvrdma_get_qp_counters() result */
    bool max;   /* every counter is UINT64_MAX */
    uint8_t want_rc;
    size_t want_written; /* bytes from dma; 0 = guest memory untouched */
    enum content content;
};

#define HDRS    IONIC_V1_ADMIN_STATS_HDRS
#define QP_HDRS IONIC_V1_ADMIN_QP_STATS_HDRS
#define VALS    IONIC_V1_ADMIN_STATS_VALS
#define QP_VALS IONIC_V1_ADMIN_QP_STATS_VALS
#define VER1    (1u << 24)

static const struct stats_case cases[] = {
    /* positive */
    {"positive", "device headers into a page",
     "5 LE64 headers then a zeroed NONE terminator (192 bytes)", HDRS, BUF_GPA,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},
    {"positive", "QP headers into a page",
     "8 LE64 headers then a zeroed NONE terminator (288 bytes)", QP_HDRS,
     BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0,
     (QP_STATS_COUNT + 1) * 32, CONTENT_QP_HDRS},
    {"positive", "device values into a page",
     "the 5 device counters as LE64 at the header offsets (40 bytes)", VALS,
     BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0, DEV_STATS_COUNT * 8,
     CONTENT_DEV_VALS},
    {"positive", "QP values for the QP named by id_ver",
     "that QP's 8 counters as LE64 (64 bytes)", QP_VALS, BUF_GPA, PAGE_LEN,
     QP_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, QP_STATS_COUNT * 8,
     CONTENT_QP_VALS},
    {"positive", "QP values for a second mapped QP",
     "the second QP's counters, not the first's", QP_VALS, BUF_GPA, PAGE_LEN,
     QP2_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, QP_STATS_COUNT * 8,
     CONTENT_QP2_VALS},

    /* negative */
    {"negative", "QP values for an unmapped qp_id",
     "rc 1 and nothing written, not a page of zeros", QP_VALS, BUF_GPA,
     PAGE_LEN, (QP_ID + 100) | VER1, STATS_BODY_LEN, 0, 0, false, 1, 0,
     CONTENT_NONE},
    {"negative", "headers with dma_addr 0", "rc 1 and nothing written", HDRS, 0,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "headers with a 16-byte body (no type_state)",
     "rc 1 and nothing written", HDRS, BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN - 1,
     0, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "device values when the counter snapshot fails",
     "rc 1 and nothing written", VALS, BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN,
     -22, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "QP values when the counter snapshot fails",
     "rc 1 and nothing written", QP_VALS, BUF_GPA, PAGE_LEN, QP_ID | VER1,
     STATS_BODY_LEN, 0, -22, false, 1, 0, CONTENT_NONE},
    {"negative", "headers into a buffer that runs off guest memory",
     "rc 1 from the failed DMA and nothing written", HDRS, GUEST_MEM_SIZE - 64,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 1, 0, CONTENT_NONE},

    /* boundary */
    {"boundary", "headers with length 31, one byte short of an entry",
     "rc 1 and nothing written", HDRS, BUF_GPA, 31, 0, STATS_BODY_LEN, 0, 0,
     false, 1, 0, CONTENT_NONE},
    {"boundary", "headers with length 32, exactly one entry",
     "only the first header (32 bytes)", HDRS, BUF_GPA, 32, 0, STATS_BODY_LEN,
     0, 0, false, 0, 32, CONTENT_DEV_HDRS},
    {"boundary", "headers with room for the table but not the terminator",
     "5 headers and no terminator (160 bytes)", HDRS, BUF_GPA,
     DEV_STATS_COUNT * 32, 0, STATS_BODY_LEN, 0, 0, false, 0,
     DEV_STATS_COUNT * 32, CONTENT_DEV_HDRS},
    {"boundary", "headers with length one byte short of the terminator",
     "whole entries only: 5 headers (160 bytes)", HDRS, BUF_GPA,
     (DEV_STATS_COUNT + 1) * 32 - 1, 0, STATS_BODY_LEN, 0, 0, false, 0,
     DEV_STATS_COUNT * 32, CONTENT_DEV_HDRS},
    {"boundary", "values with length 7, one byte short of a value",
     "rc 1 and nothing written", VALS, BUF_GPA, 7, 0, STATS_BODY_LEN, 0, 0,
     false, 1, 0, CONTENT_NONE},
    {"boundary", "values with length 8, exactly one value",
     "only the first value (8 bytes)", VALS, BUF_GPA, 8, 0, STATS_BODY_LEN, 0,
     0, false, 0, 8, CONTENT_DEV_VALS},
    {"boundary", "values with length 20, not a multiple of 8",
     "whole values only: 2 values (16 bytes)", VALS, BUF_GPA, 20, 0,
     STATS_BODY_LEN, 0, 0, false, 0, 16, CONTENT_DEV_VALS},
    {"boundary", "headers with a body of exactly 17 bytes",
     "accepted: the full table (192 bytes)", HDRS, BUF_GPA, PAGE_LEN, 0,
     STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},

    /* corner */
    {"corner", "QP values with every id_ver version bit set",
     "the version byte is ignored and QP_ID is still found", QP_VALS, BUF_GPA,
     PAGE_LEN, QP_ID | 0xff000000u, STATS_BODY_LEN, 0, 0, false, 0,
     QP_STATS_COUNT * 8, CONTENT_QP_VALS},
    {"corner", "device values with every counter at UINT64_MAX",
     "every value round-trips as all ones (40 bytes)", VALS, BUF_GPA, PAGE_LEN,
     0, STATS_BODY_LEN, 0, 0, true, 0, DEV_STATS_COUNT * 8, CONTENT_DEV_VALS},
    {"corner", "headers with a body longer than 17 bytes",
     "the trailing bytes are ignored: the full table", HDRS, BUF_GPA, PAGE_LEN,
     0, 64, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32, CONTENT_DEV_HDRS},
    {"corner", "device headers ignore id_ver",
     "the full device table, as with id_ver 0", HDRS, BUF_GPA, PAGE_LEN,
     QP_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},
};

static bool content_matches(const struct stats_case *c)
{
    switch (c->content) {
    case CONTENT_NONE:
        return true;
    case CONTENT_DEV_HDRS:
        return hdrs_match(c->dma, dev_stats, DEV_STATS_COUNT,
                          c->want_written / STAT_HDR_SIZE);
    case CONTENT_QP_HDRS:
        return hdrs_match(c->dma, qp_stats, QP_STATS_COUNT,
                          c->want_written / STAT_HDR_SIZE);
    case CONTENT_DEV_VALS:
        return vals_match(c->dma, c->want_written / 8, dev_value);
    case CONTENT_QP_VALS:
        return g_counters_qp_id == QP_ID &&
               vals_match(c->dma, c->want_written / 8, qp_value_for_qp);
    case CONTENT_QP2_VALS:
        return g_counters_qp_id == QP2_ID &&
               vals_match(c->dma, c->want_written / 8, qp_value_for_qp2);
    default:
        return false;
    }
}

static void run_case(const struct stats_case *c)
{
    uint8_t body[64];
    uint8_t rc;
    size_t written;
    bool ok;

    ctx_reset();
    g_dev_rc = c->dev_rc;
    g_qp_rc = c->qp_rc;
    g_max = c->max;
    build_body(body, sizeof(body), c->dma, c->length, c->id_ver);

    rc = ionic_adminq_dispatch_wqe(&g_ctx, c->op, body, c->body_len);
    written = bytes_written(c->dma);
    ok = rc == c->want_rc && written == c->want_written && content_matches(c);

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got rc %u (want %u), %zu bytes written (want %zu)\n", rc,
               c->want_rc, written, c->want_written);
        failures++;
    }
}

/*
 * Corner: the driver forces name[27] to NUL, so a name of 28 or more bytes
 * would lose its tail and could collide with another.  Every name must fit
 * in 27 bytes and be unique within its table.
 */
static void check_names(const char *table, const struct adminq_stat *stats,
                        size_t count)
{
    bool ok = true;

    for (size_t i = 0; i < count; i++) {
        if (strlen(stats[i].name) > STAT_NAME_LEN - 1)
            ok = false;
        for (size_t j = 0; j < i; j++)
            if (strcmp(stats[i].name, stats[j].name) == 0)
                ok = false;
    }
    printf("%s corner: every %s name; expected: at most 27 bytes and unique\n",
           ok ? "ok  " : "FAIL", table);
    if (!ok)
        failures++;
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    check_names("device stat", dev_stats, DEV_STATS_COUNT);
    check_names("QP stat", qp_stats, QP_STATS_COUNT);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
