/*
 * Unit tests for how the ionic emulator reads admin WQEs off the ring.
 *
 * A WQE's header gives the length of its body, and a body longer than the
 * 60 bytes left in the first 64-byte stride runs on into the next ring
 * entries. ionic_adminq_poll() used to take that length on trust: it read
 * at most four strides and never past the producer index, but handed the
 * handler the guest's length anyway, so a CREATE_QP claiming two strides
 * with one produced had the end of its body taken from uninitialized
 * stack. It also read a WQE's later strides straight on from its first,
 * so one the driver wrapped to the start of the ring was finished with
 * whatever guest memory lies past the ring's end.
 *
 * Each row writes one WQE into a ring in fake guest memory that is
 * otherwise poison, rings the doorbell, polls once, and checks the
 * completion, how far the consumer index moved, and whether the command
 * ran. The test links ionic_adminq.c, reaches its context through
 * ionic_adminq_priv.h, and stubs its external symbols with
 * ionic_adminq_fakes.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <infiniband/verbs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "byteorder.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_adminq_priv.h"
#include "ionic/ionic_status.h"

#include "ionic_adminq_fakes.h"

/* ---- Layouts, from the driver's side (ionic_fw.h) ----------------------- */

#define STRIDE      64U /* ADMIN_WQE_STRIDE */
#define HDR_LEN     4U  /* ADMIN_WQE_HDR_LEN */
#define CQE_LEN     32U
#define QP_BODY_LEN 64U /* IONIC_ADMIN_CREATE_QP_IN_V1_LEN */

/* The admin queue and its CQ. The AQ is followed by poison, which is what
 * reading on past its end finds. */
#define AQ_GPA        256U
#define AQ_DEPTH_LOG2 3U
#define AQ_DEPTH      (1U << AQ_DEPTH_LOG2)
#define ACQ_GPA       2048U
#define CQ_ID         1U

_Static_assert(AQ_GPA + (AQ_DEPTH * STRIDE) < ACQ_GPA, "AQ runs into the ACQ");
_Static_assert(ACQ_GPA + (AQ_DEPTH * CQE_LEN) <= GUEST_MEM_SIZE,
               "ACQ runs off guest memory");

static int failures;

/* ---- Helpers ----------------------------------------------------------- */

/* A CREATE_QP for a UD QP on CQ_ID, valid in every field. type_state is the
 * last byte the handler reads, at [62], in the WQE's second stride. */
static void build_create_qp(uint8_t *body)
{
    memset(body, 0, QP_BODY_LEN);
    ernic_put_le32(body + 8, CQ_ID);
    body[12] = 4;  /* sq_depth_log2 */
    body[13] = 7;  /* sq_stride_log2 */
    body[15] = 12; /* sq_page_size_log2 */
    ernic_put_le32(body + 20, 1);
    ernic_put_le64(body + 24, 0x10000);
    ernic_put_le32(body + 32, CQ_ID);
    body[36] = 4;
    body[37] = 7;
    body[39] = 12;
    ernic_put_le32(body + 40, 0xffffffffU); /* no SRQ */
    ernic_put_le32(body + 44, 1);
    ernic_put_le64(body + 48, 0x20000);
    ernic_put_le32(body + 56, 3U | 1U << 24);
    body[62] = IONIC_QPT_UD;
}

/* A WQE for write_wqe() to put in the admin queue */
struct aq_wqe {
    uint32_t slot;     /* where it starts */
    uint8_t op;        /* NOOP or CREATE_QP */
    uint16_t len;      /* the body length its header claims */
    uint32_t nstrides; /* how many of its strides to write */
};

/*
 * Write @w into the first @w->nstrides of its strides, starting at
 * @w->slot of the @depth-entry ring at @gpa, the way
 * ionic_admin_poll_locked() reads it: one ring entry per stride, wrapping at
 * the end of the ring.
 */
static void write_wqe(uint64_t gpa, uint32_t depth, const struct aq_wqe *w)
{
    uint8_t wqe[AQ_DEPTH * STRIDE];

    /* A body that isn't a CREATE_QP is a pattern; nothing reads it. */
    for (size_t i = 0; i < sizeof(wqe); i++) {
        wqe[i] = (uint8_t)((i * 7) + 1);
    }
    wqe[0] = w->op;
    wqe[1] = 0;
    ernic_put_le16(wqe + 2, w->len);
    if (w->op == IONIC_V1_ADMIN_CREATE_QP) {
        build_create_qp(wqe + HDR_LEN);
    }

    if (w->nstrides > sizeof(wqe) / STRIDE) {
        abort();
    }
    for (size_t i = 0; i < w->nstrides; i++) {
        memcpy(g_mem + gpa + (((w->slot + i) % depth) * STRIDE),
               wqe + (i * STRIDE), STRIDE);
    }
}

/* A fresh context whose admin queue is at @gpa, 2^@depth_log2 deep, with
 * its consumer index at @slot, and with CQ_ID created. */
static bool aq_reset(uint64_t gpa, uint8_t depth_log2, uint32_t slot)
{
    ctx_reset();
    g_ctx.cq_map[0].valid = true;
    g_ctx.cq_map[0].cq_id = CQ_ID;
    g_ctx.cq_map[0].handle = 1;
    if (!ionic_adminq_register_queue(&g_ctx, 0, gpa, depth_log2, ACQ_GPA,
                                     AQ_DEPTH_LOG2, 0, 0)) {
        return false;
    }
    g_ctx.aq[0].aq_cons = slot;
    return true;
}

/* The doorbell: the driver's producer index, @produced strides past
 * @slot. */
static void ring_doorbell(uint32_t depth, uint32_t slot, uint32_t produced)
{
    ionic_adminq_update_prod(&g_ctx, 0, (uint16_t)((slot + produced) % depth));
}

/* Completion @idx on the admin CQ is for the WQE at @slot, @op, with
 * @status (0 for success). */
static bool cqe_is(uint32_t idx, uint32_t slot, uint8_t op, uint8_t status)
{
    const uint8_t *cqe = g_mem + ACQ_GPA + ((size_t)idx * CQE_LEN);
    uint16_t cmd_idx = ernic_get_be16(cqe);
    uint32_t status_length = ernic_get_be32(cqe + 24);
    uint32_t qtf = ernic_get_be32(cqe + 28);

    bool error = (qtf & 2U) != 0;
    return cmd_idx == slot && cqe[2] == op && (qtf & 1U) != 0 &&
           error == (status != 0) && (!error || status_length == status);
}

/* Nothing was written to the admin CQ from completion @idx on. */
static bool cq_untouched_from(uint32_t idx)
{
    for (uint32_t i = idx * CQE_LEN; i < AQ_DEPTH * CQE_LEN; i++) {
        if (g_mem[ACQ_GPA + i] != POISON) {
            return false;
        }
    }
    return true;
}

/* ---- Tests ------------------------------------------------------------- */

struct poll_case {
    const char *category;
    const char *description;
    const char *expected;
    uint32_t slot;     /* where the WQE starts */
    uint8_t op;        /* NOOP or CREATE_QP */
    uint16_t len;      /* the body length its header claims */
    uint32_t written;  /* strides of it in the ring */
    uint32_t produced; /* strides the doorbell covers */
    uint8_t want_status;
    uint32_t want_consumed; /* strides the consumer index moves */
    bool want_qp;           /* the CREATE_QP ran, and made a UD QP */
};

#define NOOP      IONIC_V1_ADMIN_NOOP
#define CREATE_QP IONIC_V1_ADMIN_CREATE_QP

static const struct poll_case cases[] = {
    /* positive */
    {"positive", "NOOP with no body", "success, one stride", 0, NOOP, 0, 1, 1,
     0, 1, false},
    {"positive", "CREATE_QP, two strides", "success, a UD QP, two strides", 0,
     CREATE_QP, QP_BODY_LEN, 2, 2, 0, 2, true},

    /* boundary */
    {"boundary", "NOOP of 60 bytes, a full first stride", "success, one stride",
     0, NOOP, STRIDE - HDR_LEN, 1, 1, 0, 1, false},
    {"boundary", "NOOP of 61 bytes, one past the first stride",
     "success, two strides", 0, NOOP, STRIDE - HDR_LEN + 1, 2, 2, 0, 2, false},
    {"boundary", "NOOP of 252 bytes, four strides", "success, four strides", 0,
     NOOP, (4 * STRIDE) - HDR_LEN, 4, 4, 0, 4, false},
    {"boundary", "CREATE_QP ending at the last slot",
     "success, a UD QP, two strides", AQ_DEPTH - 2, CREATE_QP, QP_BODY_LEN, 2,
     2, 0, 2, true},

    /* corner */
    {"corner", "CREATE_QP whose second stride wraps to slot 0",
     "type_state from slot 0, not from past the ring: a UD QP", AQ_DEPTH - 1,
     CREATE_QP, QP_BODY_LEN, 2, 2, 0, 2, true},
    {"corner", "NOOP of 252 bytes wrapping after its second stride",
     "success, four strides", AQ_DEPTH - 2, NOOP, (4 * STRIDE) - HDR_LEN, 4, 4,
     0, 4, false},

    /* negative */
    {"negative", "CREATE_QP claiming two strides with one produced",
     "refused without running, one stride", 0, CREATE_QP, QP_BODY_LEN, 2, 1,
     IONIC_RC_EINVAL, 1, false},
    {"negative", "the same at the last slot",
     "refused without running, one stride", AQ_DEPTH - 1, CREATE_QP,
     QP_BODY_LEN, 2, 1, IONIC_RC_EINVAL, 1, false},
    {"negative", "NOOP of 253 bytes, five strides",
     "refused, the five strides it claims", 0, NOOP, (4 * STRIDE) - HDR_LEN + 1,
     5, 5, IONIC_RC_EINVAL, 5, false},
    {"negative", "NOOP of 65535 bytes with three strides produced",
     "refused, the three produced", 0, NOOP, UINT16_MAX, 3, 3, IONIC_RC_EINVAL,
     3, false},
};

static void run_case(const struct poll_case *c)
{
    bool ok = aq_reset(AQ_GPA, AQ_DEPTH_LOG2, c->slot);

    write_wqe(AQ_GPA, AQ_DEPTH,
              &(struct aq_wqe){.slot = c->slot,
                               .op = c->op,
                               .len = c->len,
                               .nstrides = c->written});
    ring_doorbell(AQ_DEPTH, c->slot, c->produced);
    ionic_adminq_poll(&g_ctx, NULL);

    uint32_t want_cons = (c->slot + c->want_consumed) % AQ_DEPTH;
    ok = ok && g_ctx.aq[0].aq_cons == want_cons &&
         cqe_is(0, c->slot, c->op, c->want_status) && cq_untouched_from(1);
    if (c->want_qp) {
        ok = ok && g_alloc_qp_calls == 1 && g_alloc_qp_type == IBV_QPT_UD;
    } else {
        ok = ok && g_alloc_qp_calls == 0;
    }

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got consumer index %u (want %u), %u QP(s) of type %u\n",
               g_ctx.aq[0].aq_cons, want_cons, g_alloc_qp_calls,
               g_alloc_qp_type);
        failures++;
    }
}

/*
 * Negative: a refused WQE moves the consumer index past every stride it
 * claims, as the driver does when it sees the completion, so the WQE after
 * it is read from where the driver put it and not from the middle of the
 * refused one.
 */
static void check_refused_then_next(void)
{
    const uint32_t big = 5;
    const uint32_t next = big;
    bool ok = aq_reset(AQ_GPA, AQ_DEPTH_LOG2, 0);

    write_wqe(AQ_GPA, AQ_DEPTH,
              &(struct aq_wqe){.slot = 0,
                               .op = NOOP,
                               .len = (4 * STRIDE) - HDR_LEN + 1,
                               .nstrides = big});
    write_wqe(AQ_GPA, AQ_DEPTH,
              &(struct aq_wqe){.slot = next, .op = NOOP, .nstrides = 1});
    ring_doorbell(AQ_DEPTH, 0, big + 1);
    ionic_adminq_poll(&g_ctx, NULL);

    ok = ok && cqe_is(0, 0, NOOP, IONIC_RC_EINVAL) &&
         cqe_is(1, next, NOOP, 0) && cq_untouched_from(2) &&
         g_ctx.aq[0].aq_cons == big + 1;
    printf("%s negative: a five-stride NOOP and a NOOP after it; expected: the "
           "first refused, the second run from slot %u\n",
           ok ? "ok  " : "FAIL", next);
    if (!ok) {
        failures++;
    }
}

/*
 * Negative: a WQE whose later strides can't be read stays queued for the
 * next poll, like one whose first stride can't be, rather than completing.
 * The ring is placed so that its last two slots are off guest memory.
 */
static void check_unreadable_tail(void)
{
    const uint8_t depth_log2 = 2;
    const uint64_t gpa = GUEST_MEM_SIZE - (2 * STRIDE);
    bool ok = aq_reset(gpa, depth_log2, 0);

    write_wqe(
        gpa, 1U << depth_log2,
        &(struct aq_wqe){
            .slot = 0, .op = NOOP, .len = (2 * STRIDE) + 1, .nstrides = 2});
    ring_doorbell(1U << depth_log2, 0, 3);
    ionic_adminq_poll(&g_ctx, NULL);

    ok = ok && cq_untouched_from(0) && g_ctx.aq[0].aq_cons == 0;
    printf("%s negative: a three-stride NOOP whose third stride is off guest "
           "memory; expected: no completion, nothing consumed\n",
           ok ? "ok  " : "FAIL");
    if (!ok) {
        failures++;
    }
}

/*
 * Boundary and negative: the deepest admin queue is 2^16 entries, the most
 * a completion's 16-bit cmd_idx can name, and one twice that is refused
 * rather than reported on with slot numbers cut down to 16 bits.
 */
static void check_depth_limit(void)
{
    ctx_reset();
    bool deepest = ionic_adminq_register_queue(&g_ctx, 0, AQ_GPA,
                                               IONIC_ADMINQ_MAX_DEPTH_LOG2,
                                               ACQ_GPA, AQ_DEPTH_LOG2, 0, 0);
    ctx_reset();
    bool deeper = ionic_adminq_register_queue(&g_ctx, 0, AQ_GPA,
                                              IONIC_ADMINQ_MAX_DEPTH_LOG2 + 1,
                                              ACQ_GPA, AQ_DEPTH_LOG2, 0, 0);
    bool ok = deepest && !deeper && !g_ctx.aq[0].valid;

    printf("%s boundary: admin queues 2^16 and 2^17 deep; expected: the first "
           "registered, the second refused\n",
           ok ? "ok  " : "FAIL");
    if (!ok) {
        failures++;
    }
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case(&cases[i]);
    }
    check_refused_then_next();
    check_unreadable_tail();
    check_depth_limit();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
