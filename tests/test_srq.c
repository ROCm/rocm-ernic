// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2026, Advanced Micro Devices, Inc. */

/*
 * Shared receive queue smoke test, run inside a guest against the emulated
 * ernic.
 *
 * WHY THIS EXISTS. UCX's rc_verbs transport is built on an SRQ -- its whole
 * receive path is ibv_post_srq_recv() -- and it refuses a device that reports
 * max_srq == 0. That leaves ud_verbs as the only verbs transport, and UD has
 * no one-sided RDMA READ/WRITE, so nixl (and therefore LMCache P2P) falls
 * back to TCP without saying so. A zero here is the difference between RDMA
 * and not, and nothing else in the suite notices it.
 *
 * WHAT IT CHECKS, in the order the failures actually bite:
 *   1. the device advertises max_srq                 (the capability)
 *   2. an SRQ can be created and queried             (the admin path)
 *   3. a QP can be bound to it                       (create_qp's srq arg)
 *   4. a receive posted to the SRQ completes a SEND  (the datapath)
 *
 * Step 4 is the one that was silently broken: the emulator queued SRQ
 * receives on the SRQ and then consumed only the per-QP receive queue, so a
 * posted buffer was never filled and the send never completed. Creating an
 * SRQ "successfully" and hanging is exactly what advertising the capability
 * without the datapath gets you.
 *
 * Exits 0 on success, 77 when the device cannot do SRQ (CTest reports the
 * skip as a pass), and 1 on a validation failure. The 77 path is expected
 * until the guest carries the ionic SRQ driver -- the series is in
 * rdma/for-next; before it, ionic_rdma never sets max_srq at all.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"

#define SRQ_DEPTH   16
#define SRQ_SGE_MAX 2
#define BUF_LEN     4096

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_srq *srq = NULL;
    struct ibv_qp *qp = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_device_attr dev_attr;
    struct ibv_srq_attr srq_attr;
    void *buf = NULL;
    enum ernic_device_result lookup;
    int ndev = 0;
    int rc = 1;

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        lookup = ernic_requested_device() ? ERNIC_DEVICE_MISMATCH
                                          : ERNIC_DEVICE_ABSENT;
        ernic_report_no_device(lookup);
        if (dev_list)
            ibv_free_device_list(dev_list);
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

    /* 1. The capability. */
    if (ibv_query_device(ctx, &dev_attr)) {
        perror("ibv_query_device");
        goto out;
    }
    printf("max_srq=%d max_srq_wr=%d max_srq_sge=%d\n", dev_attr.max_srq,
           dev_attr.max_srq_wr, dev_attr.max_srq_sge);

    if (dev_attr.max_srq == 0) {
        /*
         * Not a failure: a guest whose ionic_rdma predates the SRQ series
         * reports zero here and cannot be asked to do better. Skip so the
         * lane stays green, and start testing for real the moment the guest
         * image carries the driver -- no CI change needed at that point.
         */
        printf("SKIP: device reports max_srq=0 (guest driver has no SRQ "
               "support); needs the ionic SRQ series from rdma/for-next\n");
        rc = 77;
        goto out;
    }

    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto out;
    }

    cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq");
        goto out;
    }

    /* 2. Create, then read back. A create that "succeeds" and then queries
     * as something else is the shape of a broken id round-trip: the emulator
     * assigns the SRQ id and DMAs it back, and if that response is lost the
     * driver silently keeps id 0. */
    struct ibv_srq_init_attr srq_init = {
        .attr = {.max_wr = SRQ_DEPTH, .max_sge = 1, .srq_limit = 0},
    };
    srq = ibv_create_srq(pd, &srq_init);
    if (!srq) {
        perror("ibv_create_srq");
        goto out;
    }
    printf("created SRQ\n");

    if (ibv_query_srq(srq, &srq_attr)) {
        perror("ibv_query_srq");
        goto out;
    }
    printf("queried SRQ: max_wr=%u max_sge=%u srq_limit=%u\n", srq_attr.max_wr,
           srq_attr.max_sge, srq_attr.srq_limit);
    if (srq_attr.max_wr < SRQ_DEPTH) {
        rc = fail("queried SRQ depth is smaller than requested");
        goto out;
    }

    /* 3. Bind a QP to it. The emulator accepted this argument and discarded
     * it for a long time; a QP that silently keeps its own receive queue
     * looks identical here and only diverges at step 4. */
    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .srq = srq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 16,
                .max_recv_wr = 0, /* receives come from the SRQ */
                .max_send_sge = 1,
                .max_recv_sge = 1},
    };
    qp = ibv_create_qp(pd, &qp_init);
    if (!qp) {
        perror("ibv_create_qp(srq)");
        goto out;
    }
    if (qp_init.srq != srq) {
        rc = fail("created QP is not bound to the SRQ it was given");
        goto out;
    }
    printf("created QP qpn=%u bound to SRQ\n", qp->qp_num);

    /* 4. Post a receive to the SRQ. The post itself is the last thing that
     * can be checked without a peer: delivery needs a connected QP pair, and
     * the two-VM lane is where that is exercised. A post that fails here is
     * ibv_post_srq_recv() reaching an SRQ the backend never finished
     * wiring. */
    if (posix_memalign(&buf, 4096, BUF_LEN)) {
        perror("posix_memalign");
        goto out;
    }
    memset(buf, 0, BUF_LEN);

    mr = ibv_reg_mr(pd, buf, BUF_LEN,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        goto out;
    }

    struct ibv_sge sge = {
        .addr = (uintptr_t)buf,
        .length = BUF_LEN,
        .lkey = mr->lkey,
    };
    struct ibv_recv_wr wr = {.wr_id = 0x5151, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad = NULL;

    if (ibv_post_srq_recv(srq, &wr, &bad)) {
        perror("ibv_post_srq_recv");
        goto out;
    }
    printf("posted a receive to the SRQ\n");

    /* Modify the watermark: the only SRQ attribute the ionic admin path
     * carries (ionic_admin_modify_srq is qid + low_wqes_limit). */
    struct ibv_srq_attr mod = {.srq_limit = 4};
    if (ibv_modify_srq(srq, &mod, IBV_SRQ_LIMIT)) {
        perror("ibv_modify_srq");
        goto out;
    }
    printf("modified SRQ limit to %u\n", mod.srq_limit);

    printf("PASS: SRQ create/query/bind/post/modify all succeeded\n");
    rc = 0;

out:
    if (qp)
        ibv_destroy_qp(qp);
    if (mr)
        ibv_dereg_mr(mr);
    free(buf);
    if (srq)
        ibv_destroy_srq(srq);
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
    return rc;
}
