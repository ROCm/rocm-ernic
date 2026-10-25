/*
 * Unit tests for the admin queue's id maps.
 *
 * CREATE_CQ, CREATE_QP and CREATE_MR each allocate a resource in the
 * resource manager and record the driver's id for it in a fixed-size map,
 * the only way DESTROY_CQ, DESTROY_QP and DESTROY_MR can find the resource
 * again. A CQ or QP created while its map was full used to be reported
 * created but left out of the map, and one created under an id already in
 * use replaced the entry of the one that had it, as an MR did too. Either
 * way a resource could never be freed, so a guest could fill the resource
 * manager's tables one command at a time. Each command must now be refused
 * before anything is allocated.
 *
 * The test links ionic_adminq.c, reaches its context through
 * ionic_adminq_priv.h, and stubs its external symbols with
 * ionic_adminq_fakes.h.
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
#include "ionic_adminq.h"
#include "ionic_adminq_priv.h"

#include "ionic_adminq_fakes.h"

static int failures;

static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* The ids a test creates, and the ones it fills the maps with, which never
 * collide with them */
#define NEW_ID      3u
#define FILL_ID     1000u
#define HELD_HANDLE 77u /* the handle of the entry an id in use has */
#define CQ_ID       1u  /* the CQ the QPs name */

/* ---- Bodies, from the driver's side (ionic_fw.h) ------------------------ */

static void build_cq_body(uint8_t body[34], uint32_t cq_id)
{
    memset(body, 0, 34);
    body[4] = 6;  /* depth_log2 */
    body[5] = 5;  /* stride_log2 */
    body[7] = 12; /* page_size_log2 */
    ernic_put_le32(body + 12, cq_id);
}

/* 64 rather than 63: the handler reads type_state at body[62]. */
static void build_qp_body(uint8_t body[64], uint32_t qp_id, uint32_t cq_id)
{
    memset(body, 0, 64);
    ernic_put_le32(body + 8, cq_id);        /* sq_cq_id */
    body[12] = 4;                           /* sq_depth_log2 */
    body[13] = 7;                           /* sq_stride_log2 */
    ernic_put_le32(body + 32, cq_id);       /* rq_cq_id */
    body[36] = 4;                           /* rq_depth_log2 */
    body[37] = 7;                           /* rq_stride_log2 */
    ernic_put_le32(body + 40, 0xffffffffu); /* no SRQ */
    ernic_put_le32(body + 56, qp_id);
    body[62] = 0; /* ionic qp type 0 -> IB_QPT_RC */
}

static void build_mr_body(uint8_t body[45], uint32_t mr_id)
{
    memset(body, 0, 45);
    ernic_put_le64(body + 8, PAGE_LEN); /* length */
    ernic_put_le32(body + 20, mr_id);
    body[44] = 12; /* page_size_log2 */
}

/* ---- Filling the maps --------------------------------------------------- */

/* Fill all but the last @nfree entries of each map with ids no test uses. */
static void fill_cq_map(uint32_t nfree)
{
    for (uint32_t i = 0; i < MAX_CQ_MAP - nfree; i++)
        g_ctx.cq_map[i] = (struct ionic_cq_map_entry){
            .valid = true, .cq_id = FILL_ID + i, .handle = FILL_ID + i};
}

static void fill_qp_map(uint32_t nfree)
{
    for (uint32_t i = 0; i < MAX_QP_MAP - nfree; i++)
        g_ctx.qp_map[i] = (struct ionic_qp_map_entry){
            .valid = true, .qp_id = FILL_ID + i, .qpn = FILL_ID + i};
}

static void fill_mr_map(uint32_t nfree)
{
    for (uint32_t i = 0; i < MAX_MR_MAP - nfree; i++)
        g_ctx.mr_map[i] = (struct ionic_mr_map_entry){
            .valid = true, .mr_id = FILL_ID + i, .handle = FILL_ID + i};
}

/*
 * How many valid entries of each map have @id. An id in use must keep
 * exactly one: the old maps put a second entry for it in the first free
 * slot, ahead of the first, so lookups found only the new one.
 */
static uint32_t cq_entries(uint32_t id)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_CQ_MAP; i++)
        n += g_ctx.cq_map[i].valid && g_ctx.cq_map[i].cq_id == id;
    return n;
}

static uint32_t qp_entries(uint32_t id)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_QP_MAP; i++)
        n += g_ctx.qp_map[i].valid && g_ctx.qp_map[i].qp_id == id;
    return n;
}

static uint32_t mr_entries(uint32_t id)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_MR_MAP; i++)
        n += g_ctx.mr_map[i].valid && g_ctx.mr_map[i].mr_id == id;
    return n;
}

/* ---- CQs ---------------------------------------------------------------- */

static bool create_cq(void)
{
    uint8_t body[34];

    build_cq_body(body, NEW_ID);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, body,
                                     sizeof(body)) == 0;
}

static void test_cq_map(void)
{
    /* A full map refuses the CQ before allocating it */
    ctx_reset();
    fill_cq_map(0);
    check("cq-full-refused", !create_cq());
    check("cq-full-allocated-nothing", g_alloc_cq_calls == 0);

    /* So does an id in use, which keeps its entry */
    ctx_reset();
    g_ctx.cq_map[5] = (struct ionic_cq_map_entry){
        .valid = true, .cq_id = NEW_ID, .handle = HELD_HANDLE};
    check("cq-in-use-refused", !create_cq());
    check("cq-in-use-allocated-nothing", g_alloc_cq_calls == 0);
    check("cq-in-use-entry-kept", g_ctx.cq_map[5].valid &&
                                      g_ctx.cq_map[5].cq_id == NEW_ID &&
                                      g_ctx.cq_map[5].handle == HELD_HANDLE);
    check("cq-in-use-one-entry", cq_entries(NEW_ID) == 1);

    /* The last free entry takes it */
    ctx_reset();
    fill_cq_map(1);
    check("cq-last-entry-accepted", create_cq());
    check("cq-last-entry-allocated-once", g_alloc_cq_calls == 1);
    check("cq-last-entry-mapped",
          g_ctx.cq_map[MAX_CQ_MAP - 1].valid &&
              g_ctx.cq_map[MAX_CQ_MAP - 1].cq_id == NEW_ID);
}

/* ---- QPs ---------------------------------------------------------------- */

static bool create_qp(void)
{
    uint8_t body[64];

    build_qp_body(body, NEW_ID, CQ_ID);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, body,
                                     sizeof(body)) == 0;
}

/* The fakes' context, with the CQ the QP names */
static void qp_ctx_reset(void)
{
    ctx_reset();
    g_ctx.cq_map[0] =
        (struct ionic_cq_map_entry){.valid = true, .cq_id = CQ_ID, .handle = 1};
}

static void test_qp_map(void)
{
    qp_ctx_reset();
    fill_qp_map(0);
    check("qp-full-refused", !create_qp());
    check("qp-full-allocated-nothing", g_alloc_qp_calls == 0);

    qp_ctx_reset();
    g_ctx.qp_map[5] = (struct ionic_qp_map_entry){
        .valid = true, .qp_id = NEW_ID, .qpn = HELD_HANDLE};
    check("qp-in-use-refused", !create_qp());
    check("qp-in-use-allocated-nothing", g_alloc_qp_calls == 0);
    check("qp-in-use-entry-kept", g_ctx.qp_map[5].valid &&
                                      g_ctx.qp_map[5].qp_id == NEW_ID &&
                                      g_ctx.qp_map[5].qpn == HELD_HANDLE);
    check("qp-in-use-one-entry", qp_entries(NEW_ID) == 1);

    qp_ctx_reset();
    fill_qp_map(1);
    check("qp-last-entry-accepted", create_qp());
    check("qp-last-entry-allocated-once", g_alloc_qp_calls == 1);
    check("qp-last-entry-mapped",
          g_ctx.qp_map[MAX_QP_MAP - 1].valid &&
              g_ctx.qp_map[MAX_QP_MAP - 1].qp_id == NEW_ID &&
              g_ctx.qp_map[MAX_QP_MAP - 1].qpn == QP_QPN);
}

/* ---- MRs ---------------------------------------------------------------- */

static bool create_mr(void)
{
    uint8_t body[45];

    build_mr_body(body, NEW_ID);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_MR, body,
                                     sizeof(body)) == 0;
}

static void test_mr_map(void)
{
    ctx_reset();
    fill_mr_map(0);
    check("mr-full-refused", !create_mr());
    check("mr-full-allocated-nothing", g_alloc_mr_calls == 0);

    ctx_reset();
    g_ctx.mr_map[5] = (struct ionic_mr_map_entry){
        .valid = true, .mr_id = NEW_ID, .handle = HELD_HANDLE};
    check("mr-in-use-refused", !create_mr());
    check("mr-in-use-allocated-nothing", g_alloc_mr_calls == 0);
    check("mr-in-use-entry-kept", g_ctx.mr_map[5].valid &&
                                      g_ctx.mr_map[5].mr_id == NEW_ID &&
                                      g_ctx.mr_map[5].handle == HELD_HANDLE);
    check("mr-in-use-one-entry", mr_entries(NEW_ID) == 1);

    ctx_reset();
    fill_mr_map(1);
    check("mr-last-entry-accepted", create_mr());
    check("mr-last-entry-allocated-once", g_alloc_mr_calls == 1);
    check("mr-last-entry-mapped",
          g_ctx.mr_map[MAX_MR_MAP - 1].valid &&
              g_ctx.mr_map[MAX_MR_MAP - 1].mr_id == NEW_ID);
}

int main(void)
{
    test_cq_map();
    test_qp_map();
    test_mr_map();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
