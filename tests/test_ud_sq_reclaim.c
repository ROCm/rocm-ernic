/*
 * test_ud_sq_reclaim -- does send queue space come back?
 *
 * Every other UD test here signals every send and uses a shallow queue, so
 * each WQE is completed and reclaimed immediately and the queue never fills.
 * That is not how a real consumer drives the device. UCX's ud_verbs posts
 * with a 256-entry send queue and leaves most sends UNSIGNALLED -- its trace
 * shows "wrid 0xffffffffffffffff" -- signalling only periodically, and
 * reclaims every WQE up to the one that completed.
 *
 * If a device never completes those periodic signalled sends, or completes
 * them in a way the driver cannot use to free the earlier ones, the queue
 * fills after exactly one depth and the consumer stops sending. Nothing is
 * dropped and nothing errors: the sender simply has nowhere to put the next
 * WQE. Seen from above it looks like a peer that went quiet, and seen from
 * the device it looks like a perfectly healthy run that just stopped.
 *
 * That is the shape of the UCX stall this exists to reproduce: full speed to
 * about 243 round trips against a 256-deep send queue, then both peers park
 * waiting for each other.
 *
 * Exits 0 on success, 77 when there is no usable device, 1 on a failure.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"
#include "ernic_time.h"
#include "parse_int.h"

#define GRH_LEN      40
#define MSG_LEN      64
#define SQ_DEPTH     256 /* UCX_UD_VERBS_TX_QUEUE_LEN */
#define RQ_DEPTH     128
#define SIGNAL_EVERY 64
#define QKEY         0x11111111U
#define ITERS        1000
#define POLL_SECONDS 10

static int poll_one(struct ibv_cq *cq, struct ibv_wc *wc, int required)
{
    uint64_t deadline_ms =
        ernic_monotonic_ms() + (POLL_SECONDS * UINT64_C(1000));
    while (ernic_monotonic_ms() < deadline_ms) {
        int n = ibv_poll_cq(cq, 1, wc);
        if (n < 0) {
            return -1;
        }
        if (n > 0) {
            if (wc->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "completion failed: %s\n",
                        ibv_wc_status_str(wc->status));
                return -1;
            }
            return 1;
        }
        if (!required) {
            return 0;
        }
    }
    return required ? -1 : 0;
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *scq = NULL;
    struct ibv_cq *rcq = NULL;
    struct ibv_qp *sqp = NULL;
    struct ibv_qp *rqp = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_ah *ah = NULL;
    struct ibv_qp_init_attr sia;
    struct ibv_qp_init_attr ria;
    struct ibv_qp *qps[2];
    struct ibv_ah_attr aa;
    unsigned char *buf = NULL;
    union ibv_gid gid;
    enum ernic_device_result lookup;
    const char *se;
    int signal_every;
    int posted;
    int signalled_done;
    int recv_reaped;
    int ndev = 0;
    int rc = 1;

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        lookup = ernic_requested_device() ? ERNIC_DEVICE_MISMATCH
                                          : ERNIC_DEVICE_ABSENT;
        ernic_report_no_device(lookup);
        if (dev_list) {
            ibv_free_device_list(dev_list);
        }
        return lookup == ERNIC_DEVICE_MISMATCH ? 1 : 77;
    }
    ibdev = ernic_find_device(dev_list, ndev, &lookup);
    if (!ibdev) {
        ernic_report_no_device(lookup);
        ibv_free_device_list(dev_list);
        return lookup == ERNIC_DEVICE_MISMATCH ? 1 : 77;
    }

    ctx = ibv_open_device(ibdev);
    if (!ctx) {
        perror("ibv_open_device");
        goto out;
    }
    printf("device=%s\n", ibv_get_device_name(ibdev));

    pd = ibv_alloc_pd(ctx);
    scq = ibv_create_cq(ctx, SQ_DEPTH + 16, NULL, NULL, 0);
    rcq = ibv_create_cq(ctx, RQ_DEPTH + 16, NULL, NULL, 0);
    if (!pd || !scq || !rcq) {
        perror("alloc pd/cq");
        goto out;
    }

    sia = (struct ibv_qp_init_attr){
        .send_cq = scq,
        .recv_cq = scq,
        .qp_type = IBV_QPT_UD,
        .cap = {.max_send_wr = SQ_DEPTH,
                .max_recv_wr = 8,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 256},
    };
    ria = (struct ibv_qp_init_attr){
        .send_cq = rcq,
        .recv_cq = rcq,
        .qp_type = IBV_QPT_UD,
        .cap = {.max_send_wr = 8,
                .max_recv_wr = RQ_DEPTH,
                .max_send_sge = 1,
                .max_recv_sge = 1},
    };
    sqp = ibv_create_qp(pd, &sia);
    rqp = ibv_create_qp(pd, &ria);
    if (!sqp || !rqp) {
        perror("ibv_create_qp");
        goto out;
    }
    printf("sender qpn=%u (sq depth %d), receiver qpn=%u\n", sqp->qp_num,
           SQ_DEPTH, rqp->qp_num);

    qps[0] = sqp;
    qps[1] = rqp;
    for (int i = 0; i < 2; i++) {
        struct ibv_qp_attr a;
        memset(&a, 0, sizeof(a));
        a.qp_state = IBV_QPS_INIT;
        a.pkey_index = 0;
        a.port_num = 1;
        a.qkey = QKEY;
        if (ibv_modify_qp(qps[i], &a,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_QKEY)) {
            perror("modify(INIT)");
            goto out;
        }
        memset(&a, 0, sizeof(a));
        a.qp_state = IBV_QPS_RTR;
        if (ibv_modify_qp(qps[i], &a, IBV_QP_STATE)) {
            perror("modify(RTR)");
            goto out;
        }
        memset(&a, 0, sizeof(a));
        a.qp_state = IBV_QPS_RTS;
        a.sq_psn = 0;
        if (ibv_modify_qp(qps[i], &a, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
            perror("modify(RTS)");
            goto out;
        }
    }

    /* Check the allocation before registering it: ibv_reg_mr() was being
     * handed a null address on an allocation failure, which fails or
     * crashes inside libibverbs before the intended error path runs. */
    buf = calloc(1, GRH_LEN + MSG_LEN);
    if (!buf) {
        perror("calloc");
        goto out;
    }
    mr = ibv_reg_mr(pd, buf, GRH_LEN + MSG_LEN, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        goto out;
    }

    if (ibv_query_gid(ctx, 1, 0, &gid)) {
        perror("ibv_query_gid");
        goto out;
    }
    aa = (struct ibv_ah_attr){
        .is_global = 1,
        .port_num = 1,
        .grh = {.dgid = gid, .sgid_index = 0, .hop_limit = 1}};
    ah = ibv_create_ah(pd, &aa);
    if (!ah) {
        perror("ibv_create_ah");
        goto out;
    }

    /* Keep the receiver stocked; this test is about the SEND queue, and a
     * starved receiver would stall for an unrelated reason. */
    for (int i = 0; i < RQ_DEPTH; i++) {
        struct ibv_sge sge = {.addr = (uintptr_t)buf,
                              .length = GRH_LEN + MSG_LEN,
                              .lkey = mr->lkey};
        struct ibv_recv_wr wr = {
            .wr_id = (uint64_t)i, .sg_list = &sge, .num_sge = 1};
        struct ibv_recv_wr *bad = NULL;
        if (ibv_post_recv(rqp, &wr, &bad)) {
            perror("ibv_post_recv");
            goto out;
        }
    }

    /* Overridable so the unsignalled run length can be bisected. */
    se = getenv("ERNIC_SIGNAL_EVERY");
    signal_every = SIGNAL_EVERY;
    if (se && !ernic_parse_int(se, &signal_every)) {
        fprintf(stderr, "FAIL: ERNIC_SIGNAL_EVERY=\"%s\" is not a number\n",
                se);
        goto out;
    }
    if (signal_every < 1) {
        signal_every = 1;
    }
    printf("signalling every %d sends\n", signal_every);

    posted = 0;
    signalled_done = 0;
    recv_reaped = 0;
    for (int n = 0; n < ITERS; n++) {
        int sig = ((n + 1) % signal_every) == 0;
        struct ibv_sge sge = {.addr = (uintptr_t)buf + GRH_LEN,
                              .length = MSG_LEN,
                              .lkey = mr->lkey};
        struct ibv_send_wr wr = {
            .wr_id = (uint64_t)n,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_SEND,
            .send_flags = sig ? IBV_SEND_SIGNALED : 0,
            .wr = {.ud = {.ah = ah,
                          .remote_qpn = rqp->qp_num,
                          .remote_qkey = QKEY}},
        };
        struct ibv_send_wr *bad = NULL;
        int err = ibv_post_send(sqp, &wr, &bad);
        if (err) {
            fprintf(stderr,
                    "FAIL: ibv_post_send refused at iteration %d of %d: %s\n"
                    "  %d posted, %d signalled completions reaped.\n"
                    "  The send queue (%d deep) filled and never drained --\n"
                    "  unsignalled WQEs are not being reclaimed when a later\n"
                    "  signalled one completes. A consumer that posts mostly\n"
                    "  unsignalled sends, as UCX does, stops here with no\n"
                    "  error and no dropped traffic.\n",
                    n, ITERS, strerror(err), posted, signalled_done, SQ_DEPTH);
            goto out;
        }
        posted++;

        if (sig) {
            struct ibv_wc wc;
            if (poll_one(scq, &wc, 1) != 1) {
                fprintf(stderr,
                        "FAIL: no completion for the signalled send at "
                        "iteration %d.\n"
                        "  Without it the driver cannot free the %d "
                        "unsignalled WQEs before it. %d receives arrived.\n",
                        n, signal_every - 1, recv_reaped);
                goto out;
            }
            signalled_done++;
        }

        /* Drain receives opportunistically and repost, so the receiver is
         * never the thing that runs out. */
        struct ibv_wc rwc;
        while (poll_one(rcq, &rwc, 0) == 1) {
            recv_reaped++;
            struct ibv_sge rs = {.addr = (uintptr_t)buf,
                                 .length = GRH_LEN + MSG_LEN,
                                 .lkey = mr->lkey};
            struct ibv_recv_wr rw = {
                .wr_id = rwc.wr_id, .sg_list = &rs, .num_sge = 1};
            struct ibv_recv_wr *rbad = NULL;
            if (ibv_post_recv(rqp, &rw, &rbad)) {
                perror("repost recv");
                goto out;
            }
        }
    }

    printf("posted %d sends (%d signalled), reaped %d receives\n", posted,
           signalled_done, recv_reaped);
    printf("PASS: send queue space is reclaimed across %d unsignalled sends\n",
           ITERS);
    rc = 0;

    /* QPs first. This test deliberately leaves receives reposted and can
     * leave unsignalled sends outstanding, so destroying the QPs is what
     * flushes the work that still references the buffer, the MR and the
     * address handle. Releasing those first left live WQEs pointing at
     * them. */
out:
    if (sqp) {
        ibv_destroy_qp(sqp);
    }
    if (rqp) {
        ibv_destroy_qp(rqp);
    }
    if (ah) {
        ibv_destroy_ah(ah);
    }
    if (mr) {
        ibv_dereg_mr(mr);
    }
    free(buf);
    if (scq) {
        ibv_destroy_cq(scq);
    }
    if (rcq) {
        ibv_destroy_cq(rcq);
    }
    if (pd) {
        ibv_dealloc_pd(pd);
    }
    if (ctx) {
        ibv_close_device(ctx);
    }
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }
    return rc;
}
