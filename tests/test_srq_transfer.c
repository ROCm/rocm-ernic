// Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
//
// SPDX-License-Identifier: MIT
//
// test_srq_transfer -- move bytes through a shared receive queue.
//
// test_srq.c proves an SRQ can be created, queried, bound to a QP, posted to
// and modified. None of that touches the datapath: ibv_post_srq_recv() only
// writes a WQE into the SRQ's ring and rings a doorbell, and it returns
// success whether or not the device does anything with either. So the
// emulator passed that test for as long as ionic_datapath.c had no SRQ qtype
// at all and dropped every one of those doorbells on the floor.
//
// This is the test that notices. One RC QP in self-loopback, bound to an SRQ,
// with the receive posted to the SRQ rather than to the QP:
//
//   post_srq_recv -> SRQ ring + doorbell (qtype 10)
//   post_send     -> SQ ring + doorbell
//   emulator        deliver_recv() must take the WQE from the SRQ's ring,
//                   not from the QP's own (empty) RQ
//   poll_cq       -> one send and one recv completion, payload intact
//
// A device that ignores the SRQ doorbell hangs here rather than failing,
// which is why the poll is bounded and a timeout is a failure.
//
// Exits 0 on success, 77 when the device cannot do SRQ at all (the same skip
// test_srq.c uses), and 1 on a validation failure.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"

#define SRQ_DEPTH 16
#define MSG_LEN 256
#define POLL_SECONDS 10

/* The payload, chosen so a short or zeroed delivery is obvious. */
static void fill_pattern(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(0xA5 ^ (i & 0xff));
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
    struct ibv_qp_attr qp_attr;
    union ibv_gid my_gid;
    unsigned char *buf = NULL;
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

    if (ibv_query_device(ctx, &dev_attr)) {
        perror("ibv_query_device");
        goto out;
    }
    if (dev_attr.max_srq == 0) {
        printf("SKIP: device reports max_srq=0\n");
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

    struct ibv_srq_init_attr srq_init = {
        .attr = {.max_wr = SRQ_DEPTH, .max_sge = 1, .srq_limit = 0},
    };
    srq = ibv_create_srq(pd, &srq_init);
    if (!srq) {
        perror("ibv_create_srq");
        goto out;
    }
    printf("created SRQ\n");

    /* srq set and cap.max_recv_wr left 0: the QP has no receive ring of its
     * own, which is the case the datapath has to get right. */
    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .srq = srq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 8, .max_send_sge = 1, .max_recv_sge = 1},
    };
    qp = ibv_create_qp(pd, &qp_init);
    if (!qp) {
        perror("ibv_create_qp(srq)");
        goto out;
    }
    printf("created QP qpn=%u bound to SRQ\n", qp->qp_num);

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
        perror("modify_qp(INIT)");
        goto out;
    }

    if (ibv_query_gid(ctx, 1, 0, &my_gid)) {
        perror("ibv_query_gid");
        goto out;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.dest_qp_num = qp->qp_num; /* self-loopback */
    qp_attr.rq_psn = 0;
    qp_attr.max_dest_rd_atomic = 1;
    qp_attr.min_rnr_timer = 12;
    qp_attr.ah_attr.port_num = 1;
    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.grh.dgid = my_gid;
    qp_attr.ah_attr.grh.sgid_index = 0;
    qp_attr.ah_attr.grh.hop_limit = 1;
    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("modify_qp(RTR)");
        goto out;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    qp_attr.timeout = 14;
    qp_attr.retry_cnt = 7;
    qp_attr.rnr_retry = 7;
    qp_attr.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("modify_qp(RTS)");
        goto out;
    }
    printf("QP -> RTS (self-loopback)\n");

    /* One buffer, two halves: send out of the first, receive into the
     * second, so a delivery that never happened leaves the second zeroed
     * and the comparison fails rather than trivially passing. */
    buf = calloc(1, MSG_LEN * 2);
    if (!buf) {
        perror("calloc");
        goto out;
    }
    fill_pattern(buf, MSG_LEN);

    mr = ibv_reg_mr(pd, buf, MSG_LEN * 2,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        goto out;
    }

    struct ibv_sge rsge = {
        .addr = (uintptr_t)(buf + MSG_LEN),
        .length = MSG_LEN,
        .lkey = mr->lkey,
    };
    struct ibv_recv_wr rwr = {.wr_id = 0xBEEF, .sg_list = &rsge, .num_sge = 1};
    struct ibv_recv_wr *bad_rwr = NULL;
    if (ibv_post_srq_recv(srq, &rwr, &bad_rwr)) {
        perror("ibv_post_srq_recv");
        goto out;
    }
    printf("posted a receive to the SRQ\n");

    struct ibv_sge ssge = {
        .addr = (uintptr_t)buf,
        .length = MSG_LEN,
        .lkey = mr->lkey,
    };
    struct ibv_send_wr swr = {
        .wr_id = 0xCAFE,
        .sg_list = &ssge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_swr = NULL;
    if (ibv_post_send(qp, &swr, &bad_swr)) {
        perror("ibv_post_send");
        goto out;
    }
    printf("posted a SEND to the bound QP\n");

    /*
     * Bounded, because the failure this exists to catch is a hang: a device
     * that ignores the SRQ doorbell has no receive to satisfy the SEND, so
     * the recv completion simply never arrives.
     */
    int got_send = 0, got_recv = 0;
    unsigned recv_len = 0;
    time_t deadline = time(NULL) + POLL_SECONDS;
    while ((!got_send || !got_recv) && time(NULL) < deadline) {
        struct ibv_wc wc[4];
        int n = ibv_poll_cq(cq, 4, wc);
        if (n < 0) {
            fprintf(stderr, "ibv_poll_cq failed\n");
            goto out;
        }
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "completion %d: %s (wr_id=0x%llx)\n", i,
                        ibv_wc_status_str(wc[i].status),
                        (unsigned long long)wc[i].wr_id);
                goto out;
            }
            if (wc[i].opcode == IBV_WC_SEND) {
                got_send = 1;
            } else if (wc[i].opcode == IBV_WC_RECV) {
                got_recv = 1;
                recv_len = wc[i].byte_len;
                if (wc[i].wr_id != 0xBEEF) {
                    fprintf(stderr,
                            "recv completion carries wr_id 0x%llx, expected "
                            "0xBEEF -- the WQE did not come from the SRQ\n",
                            (unsigned long long)wc[i].wr_id);
                    goto out;
                }
            }
        }
    }

    if (!got_send || !got_recv) {
        fprintf(stderr,
                "FAIL: timed out after %ds with send=%d recv=%d.\n"
                "  A missing recv completion means the SEND found no receive "
                "buffer, i.e. the\n"
                "  receive posted to the SRQ never reached the device -- "
                "check that the\n"
                "  datapath handles the SRQ qtype's doorbell.\n",
                POLL_SECONDS, got_send, got_recv);
        goto out;
    }

    if (recv_len != MSG_LEN) {
        fprintf(stderr, "FAIL: received %u bytes, expected %d\n", recv_len,
                MSG_LEN);
        goto out;
    }
    if (memcmp(buf, buf + MSG_LEN, MSG_LEN) != 0) {
        fprintf(stderr, "FAIL: payload mismatch through the SRQ\n");
        goto out;
    }

    printf("received %u bytes through the SRQ, payload matches\n", recv_len);
    printf("PASS: data moved through a shared receive queue\n");
    rc = 0;

out:
    if (mr)
        ibv_dereg_mr(mr);
    free(buf);
    if (qp)
        ibv_destroy_qp(qp);
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
