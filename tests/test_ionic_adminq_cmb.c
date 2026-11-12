/*
 * Unit tests for where CREATE_QP puts a ring in the CMB.
 *
 * A ring CREATE_QP flags as in the CMB (IONIC_QPF_SQ_CMB, IONIC_QPF_RQ_CMB)
 * is in BAR4, and the driver names it by its offset there, cmb_pgid <<
 * PAGE_SHIFT, not by a guest address. The handler used to take the first
 * CMB ring's address for the address of BAR4 and subtract it from every
 * later one. The first ring the driver places is usually at offset 0, which
 * left that base unset, so the next ring became the base and was put at
 * offset 0 too, on top of the first, and a ring below the base got an
 * offset just short of 2^64. Every send from one failed, which is how the
 * guest's RDMA tests failed once the device offered a CMB.
 *
 * The offset must now be passed to the data path unchanged, and a ring that
 * does not lie wholly inside the CMB must be refused before anything is
 * allocated, as must any CMB ring on a device without a CMB.
 *
 * The test links ionic_adminq.c, reaches its context through
 * ionic_adminq_priv.h, and stubs its external symbols with
 * ionic_adminq_fakes.h, whose ionic_datapath_set_qp_cmb() records what it
 * was given.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "byteorder.h"
#include "ionic/ionic_adminq_priv.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_status.h"

#include "ionic_adminq_fakes.h"

static int failures;

static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

/* The QPs a test creates, clear of the ones ctx_reset() maps, and the CQ
 * they name */
#define QP_C  3U
#define QP_D  4U
#define QP_E  5U
#define CQ_ID 1U

/* Each ring is 2^4 WQEs of 2^7 bytes */
#define DEPTH_LOG2  4U
#define STRIDE_LOG2 7U
#define RING_SPAN   (UINT64_C(1) << (DEPTH_LOG2 + STRIDE_LOG2))
#define CMB_PAGE    UINT64_C(4096)
#define CMB_SIZE    ((uint64_t)IONIC_BAR4_CMB_SIZE)

/* A guest address, well past the CMB's size, for a ring in guest memory */
#define RING_GPA UINT64_C(0x10000000)

/* Where CREATE_QP is told each ring is, and in how many pages */
struct ring_at {
    uint64_t dma_addr;
    uint32_t map_count;
};

/* 64 rather than 63: the handler reads type_state at body[62]. */
static void build_qp_body(uint8_t body[64], uint32_t qp_id, uint32_t flags,
                          struct ring_at sq, struct ring_at rq)
{
    memset(body, 0, 64);
    ernic_put_be32(body + 4, flags);         /* priv_flags */
    ernic_put_le32(body + 8, CQ_ID);         /* sq_cq_id */
    body[12] = DEPTH_LOG2;                   /* sq_depth_log2 */
    body[13] = STRIDE_LOG2;                  /* sq_stride_log2 */
    body[15] = 12;                           /* sq_page_size_log2 */
    ernic_put_le32(body + 20, sq.map_count); /* sq_map_count */
    ernic_put_le64(body + 24, sq.dma_addr);  /* sq_dma_addr */
    ernic_put_le32(body + 32, CQ_ID);        /* rq_cq_id */
    body[36] = DEPTH_LOG2;                   /* rq_depth_log2 */
    body[37] = STRIDE_LOG2;                  /* rq_stride_log2 */
    body[39] = 12;                           /* rq_page_size_log2 */
    ernic_put_le32(body + 40, 0xffffffffU);  /* no SRQ */
    ernic_put_le32(body + 44, rq.map_count); /* rq_map_count */
    ernic_put_le64(body + 48, rq.dma_addr);  /* rq_dma_addr */
    ernic_put_le32(body + 56, qp_id);
    body[62] = 0; /* ionic qp type 0 -> IB_QPT_RC */
}

static uint8_t create_qp(uint32_t qp_id, uint32_t flags, struct ring_at sq,
                         struct ring_at rq)
{
    uint8_t body[64];

    build_qp_body(body, qp_id, flags, sq, rq);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, body,
                                     sizeof(body));
}

static struct ring_at at(uint64_t dma_addr)
{
    return (struct ring_at){.dma_addr = dma_addr, .map_count = 1};
}

/* The fakes' context, with the CQ the QPs name */
static void qp_ctx_reset(void)
{
    ctx_reset();
    g_ctx.cq_map[0] =
        (struct ionic_cq_map_entry){.valid = true, .cq_id = CQ_ID, .handle = 1};
}

/* Whether the data path was last told @qp_id's send ring is at @offset in
 * the CMB, and its receive ring is not in it */
static bool sq_placed(uint32_t qp_id, uint64_t offset)
{
    return g_qp_cmb.qp_id == qp_id && g_qp_cmb.sq_cmb &&
           g_qp_cmb.sq_offset == offset && !g_qp_cmb.rq_cmb;
}

/* The QP was refused before anything was allocated or placed */
static bool refused_cleanly(uint8_t status)
{
    return status == IONIC_RC_ERANGE && g_alloc_qp_calls == 0 &&
           g_set_qp_cmb_calls == 0;
}

/* ---- Offsets ------------------------------------------------------------ */

static void test_offsets(void)
{
    /* Each send ring is where the driver said, whatever came before it:
     * one at offset 0, one above it, then one between the two */
    qp_ctx_reset();
    check("sq-at-0-accepted", create_qp(QP_C, IONIC_QPF_SQ_CMB, at(0),
                                        at(RING_GPA)) == IONIC_RC_SUCCESS);
    check("sq-at-0-placed", sq_placed(QP_C, 0));
    check("sq-above-0-accepted",
          create_qp(QP_D, IONIC_QPF_SQ_CMB, at(2 * CMB_PAGE), at(RING_GPA)) ==
              IONIC_RC_SUCCESS);
    check("sq-above-0-placed", sq_placed(QP_D, 2 * CMB_PAGE));
    check("sq-between-accepted", create_qp(QP_E, IONIC_QPF_SQ_CMB, at(CMB_PAGE),
                                           at(RING_GPA)) == IONIC_RC_SUCCESS);
    check("sq-between-placed", sq_placed(QP_E, CMB_PAGE));

    /* A first ring away from offset 0 is not where the CMB starts: one
     * below it is still at its own offset */
    qp_ctx_reset();
    check("sq-high-first-accepted",
          create_qp(QP_C, IONIC_QPF_SQ_CMB, at(2 * CMB_PAGE), at(RING_GPA)) ==
              IONIC_RC_SUCCESS);
    check("sq-high-first-placed", sq_placed(QP_C, 2 * CMB_PAGE));
    check("sq-below-first-accepted",
          create_qp(QP_D, IONIC_QPF_SQ_CMB, at(CMB_PAGE), at(RING_GPA)) ==
              IONIC_RC_SUCCESS);
    check("sq-below-first-placed", sq_placed(QP_D, CMB_PAGE));

    /* The receive ring, alone and with the send ring */
    qp_ctx_reset();
    check("rq-accepted", create_qp(QP_C, IONIC_QPF_RQ_CMB, at(RING_GPA),
                                   at(3 * CMB_PAGE)) == IONIC_RC_SUCCESS);
    check("rq-placed", g_qp_cmb.qp_id == QP_C && !g_qp_cmb.sq_cmb &&
                           g_qp_cmb.rq_cmb &&
                           g_qp_cmb.rq_offset == 3 * CMB_PAGE);
    check("both-accepted",
          create_qp(QP_D, IONIC_QPF_SQ_CMB | IONIC_QPF_RQ_CMB, at(CMB_PAGE),
                    at(4 * CMB_PAGE)) == IONIC_RC_SUCCESS);
    check("both-placed", g_qp_cmb.qp_id == QP_D && g_qp_cmb.sq_cmb &&
                             g_qp_cmb.sq_offset == CMB_PAGE &&
                             g_qp_cmb.rq_cmb &&
                             g_qp_cmb.rq_offset == 4 * CMB_PAGE);

    /* A QP with no ring in the CMB is not placed there, and its guest
     * addresses, far past the CMB's size, are not held to it */
    qp_ctx_reset();
    check("no-cmb-accepted",
          create_qp(QP_C, 0, at(RING_GPA), at(RING_GPA)) == IONIC_RC_SUCCESS);
    check("no-cmb-not-placed", g_set_qp_cmb_calls == 0);
}

/* ---- Bounds ------------------------------------------------------------- */

static void test_bounds(void)
{
    /* The last place a ring fits, ending at the CMB's last byte */
    qp_ctx_reset();
    check("sq-at-end-accepted",
          create_qp(QP_C, IONIC_QPF_SQ_CMB, at(CMB_SIZE - RING_SPAN),
                    at(RING_GPA)) == IONIC_RC_SUCCESS);
    check("sq-at-end-placed", sq_placed(QP_C, CMB_SIZE - RING_SPAN));

    /* One byte further, and the last WQE runs off the end */
    qp_ctx_reset();
    check(
        "sq-past-end-refused",
        refused_cleanly(create_qp(QP_C, IONIC_QPF_SQ_CMB,
                                  at(CMB_SIZE - RING_SPAN + 1), at(RING_GPA))));

    /* Starting at or past the end */
    qp_ctx_reset();
    check("sq-at-size-refused",
          refused_cleanly(
              create_qp(QP_C, IONIC_QPF_SQ_CMB, at(CMB_SIZE), at(RING_GPA))));
    qp_ctx_reset();
    check("sq-guest-address-refused",
          refused_cleanly(
              create_qp(QP_C, IONIC_QPF_SQ_CMB, at(RING_GPA), at(RING_GPA))));

    /* An offset whose end wraps past 2^64 back into range */
    qp_ctx_reset();
    check("sq-wrapping-refused",
          refused_cleanly(create_qp(QP_C, IONIC_QPF_SQ_CMB,
                                    at(UINT64_MAX - RING_SPAN + 2),
                                    at(RING_GPA))));

    /* The receive ring is held to the same bounds, even with the send
     * ring in range */
    qp_ctx_reset();
    check("rq-past-end-refused",
          refused_cleanly(create_qp(QP_C, IONIC_QPF_SQ_CMB | IONIC_QPF_RQ_CMB,
                                    at(0), at(CMB_SIZE - RING_SPAN + 1))));

    /* A CMB ring is one span of BAR4, which the driver describes in a
     * single page table entry; more is not a CMB ring */
    qp_ctx_reset();
    check("sq-page-table-refused",
          refused_cleanly(create_qp(
              QP_C, IONIC_QPF_SQ_CMB,
              (struct ring_at){.dma_addr = 0, .map_count = 2}, at(RING_GPA))));
}

/* ---- No CMB ------------------------------------------------------------- */

/* The device has no CMB without --cmb, so a ring can't be in it, wherever
 * it says it is. The driver never asks then, so it is a guest that ignored
 * the missing BAR4, and is refused before anything is allocated. */
static void test_no_cmb(void)
{
    static const struct {
        const char *name;
        uint32_t flags;
    } rows[] = {
        {"without-cmb-sq-refused", IONIC_QPF_SQ_CMB},
        {"without-cmb-rq-refused", IONIC_QPF_RQ_CMB},
        {"without-cmb-both-refused", IONIC_QPF_SQ_CMB | IONIC_QPF_RQ_CMB},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        qp_ctx_reset();
        g_has_cmb = false;
        uint8_t status = create_qp(QP_C, rows[i].flags, at(0), at(CMB_PAGE));
        check(rows[i].name, status == IONIC_RC_ENOSUPP &&
                                g_alloc_qp_calls == 0 &&
                                g_set_qp_cmb_calls == 0);
    }

    /* A QP with its rings in guest memory doesn't need one */
    qp_ctx_reset();
    g_has_cmb = false;
    check("without-cmb-guest-rings-accepted",
          create_qp(QP_C, 0, at(RING_GPA), at(RING_GPA)) == IONIC_RC_SUCCESS);
}

int main(void)
{
    test_offsets();
    test_bounds();
    test_no_cmb();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
