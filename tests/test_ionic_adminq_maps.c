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
 * before anything is allocated. The MR map has a slot for every index the
 * driver hands out, so it can't fill; a key whose index has no slot is
 * refused instead.
 *
 * A CQ, QP or MR the data path then refuses to register was reported
 * created too, with no ring or page table behind it. That refusal must fail
 * the command, free the resource and leave the id unmapped.
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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "byteorder.h"
#include "ionic/ionic_adminq_priv.h"
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

/* The ids a test creates, and the ones it fills the maps with, which never
 * collide with them */
#define NEW_ID      3U
#define FILL_ID     1000U
#define HELD_HANDLE 77U /* the handle of the entry an id in use has */
#define CQ_ID       1U  /* the CQ the QPs name */

/*
 * An MR key as the driver builds it (ionic_mrid() in its ionic_fw.h): the
 * MR's index above a key byte that fast registration rotates. The driver
 * hands out indices 1 to MAX_MR_MAP, and the map keeps index i's MR at
 * i - 1. NEW_KEY names index NEW_ID.
 */
#define MR_KEY(index, byte) (((uint32_t)(index) << 8) | (uint32_t)(byte))
#define NEW_KEY             MR_KEY(NEW_ID, 0x5aU)
#define NEW_SLOT            (NEW_ID - 1U)

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
    ernic_put_le32(body + 40, 0xffffffffU); /* no SRQ */
    ernic_put_le32(body + 56, qp_id);
    body[62] = 0; /* ionic qp type 0 -> IB_QPT_RC */
}

static void build_mr_body(uint8_t body[45], uint32_t mrid)
{
    memset(body, 0, 45);
    ernic_put_le64(body + 8, PAGE_LEN); /* length */
    ernic_put_le32(body + 20, mrid);
    body[44] = 12; /* page_size_log2 */
}

/* ---- Filling the maps --------------------------------------------------- */

/* Fill all but the last @nfree entries of each map with ids no test uses. */
static void fill_cq_map(uint32_t nfree)
{
    for (uint32_t i = 0; i < MAX_CQ_MAP - nfree; i++) {
        g_ctx.cq_map[i] = (struct ionic_cq_map_entry){
            .valid = true, .cq_id = FILL_ID + i, .handle = FILL_ID + i};
    }
}

static void fill_qp_map(uint32_t nfree)
{
    for (uint32_t i = 0; i < MAX_QP_MAP - nfree; i++) {
        g_ctx.qp_map[i] = (struct ionic_qp_map_entry){
            .valid = true, .qp_id = FILL_ID + i, .qpn = FILL_ID + i};
    }
}

/*
 * How many valid entries of each map have @id. An id in use must keep
 * exactly one: the old maps put a second entry for it in the first free
 * slot, ahead of the first, so lookups found only the new one.
 */
static uint32_t cq_entries(uint32_t id)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_CQ_MAP; i++) {
        n += g_ctx.cq_map[i].valid && g_ctx.cq_map[i].cq_id == id;
    }
    return n;
}

static uint32_t qp_entries(uint32_t id)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_QP_MAP; i++) {
        n += g_ctx.qp_map[i].valid && g_ctx.qp_map[i].qp_id == id;
    }
    return n;
}

/* The MR map has no ids to match, only the slots the indices name */
static uint32_t mr_entries(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_MR_MAP; i++) {
        if (g_ctx.mr_map[i].valid) {
            n++;
        }
    }
    return n;
}

/* ---- CQs ---------------------------------------------------------------- */

static uint8_t create_cq(void)
{
    uint8_t body[34];

    build_cq_body(body, NEW_ID);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_CQ, body,
                                     sizeof(body));
}

static void test_cq_map(void)
{
    /* A full map refuses the CQ before allocating it */
    ctx_reset();
    fill_cq_map(0);
    check("cq-full-refused", create_cq() == IONIC_RC_ENOSPC);
    check("cq-full-allocated-nothing", g_alloc_cq_calls == 0);

    /* So does an id in use, which keeps its entry */
    ctx_reset();
    g_ctx.cq_map[5] = (struct ionic_cq_map_entry){
        .valid = true, .cq_id = NEW_ID, .handle = HELD_HANDLE};
    check("cq-in-use-refused", create_cq() == IONIC_RC_EEXIST);
    check("cq-in-use-allocated-nothing", g_alloc_cq_calls == 0);
    check("cq-in-use-entry-kept", g_ctx.cq_map[5].valid &&
                                      g_ctx.cq_map[5].cq_id == NEW_ID &&
                                      g_ctx.cq_map[5].handle == HELD_HANDLE);
    check("cq-in-use-one-entry", cq_entries(NEW_ID) == 1);

    /* The last free entry takes it */
    ctx_reset();
    fill_cq_map(1);
    check("cq-last-entry-accepted", create_cq() == IONIC_RC_SUCCESS);
    check("cq-last-entry-allocated-once", g_alloc_cq_calls == 1);
    check("cq-last-entry-mapped",
          g_ctx.cq_map[MAX_CQ_MAP - 1].valid &&
              g_ctx.cq_map[MAX_CQ_MAP - 1].cq_id == NEW_ID);
}

/* ---- QPs ---------------------------------------------------------------- */

static uint8_t create_qp(void)
{
    uint8_t body[64];

    build_qp_body(body, NEW_ID, CQ_ID);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, body,
                                     sizeof(body));
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
    check("qp-full-refused", create_qp() == IONIC_RC_ENOSPC);
    check("qp-full-allocated-nothing", g_alloc_qp_calls == 0);

    qp_ctx_reset();
    g_ctx.qp_map[5] = (struct ionic_qp_map_entry){
        .valid = true, .qp_id = NEW_ID, .qpn = HELD_HANDLE};
    check("qp-in-use-refused", create_qp() == IONIC_RC_EEXIST);
    check("qp-in-use-allocated-nothing", g_alloc_qp_calls == 0);
    check("qp-in-use-entry-kept", g_ctx.qp_map[5].valid &&
                                      g_ctx.qp_map[5].qp_id == NEW_ID &&
                                      g_ctx.qp_map[5].qpn == HELD_HANDLE);
    check("qp-in-use-one-entry", qp_entries(NEW_ID) == 1);

    qp_ctx_reset();
    fill_qp_map(1);
    check("qp-last-entry-accepted", create_qp() == IONIC_RC_SUCCESS);
    check("qp-last-entry-allocated-once", g_alloc_qp_calls == 1);
    check("qp-last-entry-mapped",
          g_ctx.qp_map[MAX_QP_MAP - 1].valid &&
              g_ctx.qp_map[MAX_QP_MAP - 1].qp_id == NEW_ID &&
              g_ctx.qp_map[MAX_QP_MAP - 1].qpn == QP_QPN);
}

/* ---- MRs ---------------------------------------------------------------- */

static uint8_t create_mr_key(uint32_t key)
{
    uint8_t body[45];

    build_mr_body(body, key);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_MR, body,
                                     sizeof(body));
}

static uint8_t create_mr(void)
{
    return create_mr_key(NEW_KEY);
}

static uint8_t destroy_mr(uint32_t key)
{
    uint8_t body[4];

    ernic_put_le32(body, key);
    return ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_DESTROY_MR, body,
                                     sizeof(body));
}

static void test_mr_map(void)
{
    /* Each MR goes in its index's slot, the last index's included */
    ctx_reset();
    check("mr-accepted", create_mr() == IONIC_RC_SUCCESS);
    check("mr-mapped-at-its-index",
          g_ctx.mr_map[NEW_SLOT].valid && mr_entries() == 1);

    ctx_reset();
    check("mr-last-index-accepted",
          create_mr_key(MR_KEY(MAX_MR_MAP, 0x01U)) == IONIC_RC_SUCCESS);
    check("mr-last-index-mapped",
          g_ctx.mr_map[MAX_MR_MAP - 1].valid && mr_entries() == 1);

    /* Index 0, the reserved lkey's, and those past the last have no slot */
    ctx_reset();
    check("mr-index-0-refused",
          create_mr_key(MR_KEY(0, 0x5aU)) == IONIC_RC_ERANGE);
    check("mr-index-past-last-refused",
          create_mr_key(MR_KEY(MAX_MR_MAP + 1, 0x5aU)) == IONIC_RC_ERANGE);
    check("mr-top-index-refused", create_mr_key(UINT32_MAX) == IONIC_RC_ERANGE);
    check("mr-no-slot-allocated-nothing", g_alloc_mr_calls == 0 &&
                                              g_register_mr_calls == 0 &&
                                              mr_entries() == 0);

    /* An index in use is refused, under any key byte: a rotated key names
     * the same MR, not another */
    ctx_reset();
    g_ctx.mr_map[NEW_SLOT] =
        (struct ionic_mr_map_entry){.valid = true, .handle = HELD_HANDLE};
    check("mr-in-use-refused", create_mr() == IONIC_RC_EEXIST);
    check("mr-in-use-other-byte-refused",
          create_mr_key(MR_KEY(NEW_ID, 0xa5U)) == IONIC_RC_EEXIST);
    check("mr-in-use-allocated-nothing", g_alloc_mr_calls == 0);
    check("mr-in-use-entry-kept",
          g_ctx.mr_map[NEW_SLOT].valid &&
              g_ctx.mr_map[NEW_SLOT].handle == HELD_HANDLE);
    check("mr-in-use-one-entry", mr_entries() == 1);

    /* DESTROY_MR finds the MR by its index, and has the data path drop it
     * under the key it names, which the data path resolves to the index */
    ctx_reset();
    g_ctx.mr_map[NEW_SLOT] =
        (struct ionic_mr_map_entry){.valid = true, .handle = HELD_HANDLE};
    check("mr-destroyed", destroy_mr(NEW_KEY) == IONIC_RC_SUCCESS);
    check("mr-destroyed-freed", g_dealloc_mr_calls == 1 && mr_entries() == 0);
    check("mr-destroyed-unregistered",
          g_unregister_mr_calls == 1 && g_unregister_mr_key == NEW_KEY);
    check("mr-destroyed-again-refused",
          destroy_mr(NEW_KEY) == IONIC_RC_ENOENT && g_dealloc_mr_calls == 1);
    check("mr-destroy-no-slot-refused",
          destroy_mr(MR_KEY(MAX_MR_MAP + 1, 0x5aU)) == IONIC_RC_ENOENT &&
              destroy_mr(MR_KEY(0, 0x5aU)) == IONIC_RC_ENOENT &&
              g_dealloc_mr_calls == 1);
}

/* ---- The data path refusing a registration ---------------------------- */

/* What the data path returns, and the status that must fail the command:
 * one the driver turns back into the same errno. */
static const struct {
    const char *what;
    int err;
    uint8_t status;
} dp_refusals[] = {
    {"EINVAL", -EINVAL, IONIC_RC_EINVAL},
    {"ENOMEM", -ENOMEM, IONIC_RC_ENOMEM},
    {"ENOSPC", -ENOSPC, IONIC_RC_ENOSPC},
    {"EIO", -EIO, IONIC_RC_EIO},
};

static void test_dp_refusal(void)
{
    for (size_t i = 0; i < sizeof(dp_refusals) / sizeof(dp_refusals[0]); i++) {
        printf("the data path refuses with %s:\n", dp_refusals[i].what);

        ctx_reset();
        g_register_rc = dp_refusals[i].err;
        check("cq-refused", create_cq() == dp_refusals[i].status);
        check("cq-refused-freed",
              g_alloc_cq_calls == 1 && g_dealloc_cq_calls == 1);
        check("cq-refused-unmapped", cq_entries(NEW_ID) == 0);

        qp_ctx_reset();
        g_register_rc = dp_refusals[i].err;
        check("qp-refused", create_qp() == dp_refusals[i].status);
        check("qp-refused-freed",
              g_alloc_qp_calls == 1 && g_dealloc_qp_calls == 1);
        check("qp-refused-unmapped", qp_entries(NEW_ID) == 0);

        ctx_reset();
        g_register_rc = dp_refusals[i].err;
        check("mr-refused", create_mr() == dp_refusals[i].status);
        check("mr-refused-freed",
              g_alloc_mr_calls == 1 && g_dealloc_mr_calls == 1);
        check("mr-refused-unmapped", mr_entries() == 0);
    }

    /* An MR with no region yet, as ib_alloc_mr() makes, has nothing to
     * register: REG_MR binds it later */
    uint8_t body[45];

    ctx_reset();
    g_register_rc = -EINVAL;
    build_mr_body(body, NEW_KEY);
    ernic_put_le64(body + 8, 0);
    check("mr-empty-accepted",
          ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_MR, body,
                                    sizeof(body)) == IONIC_RC_SUCCESS);
    check("mr-empty-not-registered", g_register_mr_calls == 0);
    check("mr-empty-mapped", g_ctx.mr_map[NEW_SLOT].valid && mr_entries() == 1);
}

int main(void)
{
    test_cq_map();
    test_qp_map();
    test_mr_map();
    test_dp_refusal();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
