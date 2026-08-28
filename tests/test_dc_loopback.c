/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Guest-side smoke: create SRQ, DCT, and DCI on rocm_ernic (loopback path).
 * Requires librocm_ernic and the rocm_ernic char device.  Intended to run
 * inside the VM used by system-tests after the custom provider is installed.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/rocm_ernic_dc.h>
#include <infiniband/verbs.h>

#define RECV_BYTES    64
#define SKEY          0xc001d00dULL
#define CQ_POLL_LOOPS 500000U

static struct ibv_device *find_rocm_ernic(struct ibv_device **list, int n)
{
    int i;
    const char *name;

    for (i = 0; i < n; i++) {
        if (!list[i])
            continue;
        name = ibv_get_device_name(list[i]);
        /* Accept rocm_ernic (legacy driver), rocep (PVRDMA device name),
         * or ionic_* (upstream ionic driver with AMD emulated DID patch). */
        if (strstr(name, "rocm_ernic") || strstr(name, "rocep") ||
            strstr(name, "ionic_"))
            return list[i];
    }
    return NULL;
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *send_cq = NULL;
    struct ibv_cq *recv_cq = NULL;
    struct ibv_srq *ibsrq = NULL;
    struct ibv_qp *dct = NULL;
    struct ibv_qp *dci = NULL;
    struct ibv_mr *recv_mr = NULL;
    void *recv_buf = NULL;
    struct ibv_srq_init_attr srq_attr = {};
    struct ibv_qp_init_attr qp_attr = {};
    struct rocm_ernic_dc_dct_init dct_init = {};
    struct rocm_ernic_dc_dci_init dci_init = {};
    struct ibv_recv_wr rwr = {};
    struct ibv_sge rsge = {};
    struct ibv_recv_wr *bad_rr = NULL;
    struct ibv_qp_attr mod_attr = {};
    int mod_mask;
    int ndev;
    int ret = 1;

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        fprintf(stderr, "No IB devices\n");
        goto out;
    }

    ibdev = find_rocm_ernic(dev_list, ndev);
    if (!ibdev) {
        fprintf(stderr, "No rocm_ernic device found - skipping test\n");
        ibv_free_device_list(dev_list);
        return 77; /* CTest skip code */
    }

    ctx = ibv_open_device(ibdev);
    if (!ctx) {
        perror("ibv_open_device");
        goto out;
    }

    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto out;
    }

    send_cq = ibv_create_cq(ctx, 8, NULL, NULL, 0);
    recv_cq = ibv_create_cq(ctx, 8, NULL, NULL, 0);
    if (!send_cq || !recv_cq) {
        fprintf(stderr, "ibv_create_cq failed\n");
        goto out;
    }

    srq_attr.attr.max_wr = 4;
    srq_attr.attr.max_sge = 1;

    ibsrq = ibv_create_srq(pd, &srq_attr);
    if (!ibsrq) {
        perror("ibv_create_srq");
        goto out;
    }

    recv_buf = calloc(1, RECV_BYTES);
    if (!recv_buf) {
        perror("calloc");
        goto out;
    }

    recv_mr = ibv_reg_mr(pd, recv_buf, RECV_BYTES, IBV_ACCESS_LOCAL_WRITE);
    if (!recv_mr) {
        perror("ibv_reg_mr");
        goto out;
    }

    rsge.addr = (uint64_t)(uintptr_t)recv_buf;
    rsge.length = RECV_BYTES;
    rsge.lkey = recv_mr->lkey;
    rwr.wr_id = 0xabc;
    rwr.sg_list = &rsge;
    rwr.num_sge = 1;
    if (ibv_post_srq_recv(ibsrq, &rwr, &bad_rr)) {
        perror("ibv_post_srq_recv");
        goto out;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = send_cq;
    qp_attr.recv_cq = recv_cq;
    qp_attr.qp_type = IBV_QPT_DRIVER;
    qp_attr.srq = ibsrq;
    qp_attr.cap.max_send_wr = 4;
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    dct_init.access_key = SKEY;
    dct_init.port_num = 1;

    dct = rocm_ernic_dc_create_dct(pd, &qp_attr, &dct_init);
    if (!dct) {
        fprintf(stderr, "rocm_ernic_dc_create_dct failed errno=%d\n", errno);
        goto out;
    }

    /* DCT: RESET → INIT */
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_INIT;
    mod_attr.port_num = 1;
    mod_attr.pkey_index = 0;
    mod_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    mod_mask =
        IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(dct, &mod_attr, mod_mask)) {
        fprintf(stderr, "DCT RESET->INIT failed errno=%d\n", errno);
        goto out;
    }

    /* DCT: INIT → RTR */
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTR;
    mod_attr.min_rnr_timer = 0;
    mod_mask = IBV_QP_STATE | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(dct, &mod_attr, mod_mask)) {
        fprintf(stderr, "DCT INIT->RTR failed errno=%d\n", errno);
        goto out;
    }

    /* DCT: RTR → RTS */
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTS;
    mod_attr.sq_psn = 0;
    mod_mask = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (ibv_modify_qp(dct, &mod_attr, mod_mask)) {
        fprintf(stderr, "DCT RTR->RTS failed errno=%d\n", errno);
        goto out;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = send_cq;
    qp_attr.recv_cq = recv_cq;
    qp_attr.qp_type = IBV_QPT_DRIVER;
    qp_attr.srq = NULL;
    qp_attr.cap.max_send_wr = 8;
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    memset(&dci_init, 0, sizeof(dci_init));
    dci = rocm_ernic_dc_create_dci(pd, &qp_attr, &dci_init);
    if (!dci) {
        fprintf(stderr, "rocm_ernic_dc_create_dci failed errno=%d\n", errno);
        goto out;
    }

    /* DCI: RESET → INIT */
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_INIT;
    mod_attr.port_num = 1;
    mod_attr.pkey_index = 0;
    mod_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    mod_mask =
        IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(dci, &mod_attr, mod_mask)) {
        fprintf(stderr, "DCI RESET->INIT failed errno=%d\n", errno);
        goto out;
    }

    /* DCI: INIT → RTS (no RTR for DCI) */
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTS;
    mod_attr.sq_psn = 0;
    mod_mask = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (ibv_modify_qp(dci, &mod_attr, mod_mask)) {
        fprintf(stderr, "DCI INIT->RTS failed errno=%d\n", errno);
        goto out;
    }

    {
        struct ibv_sge sge = {};
        struct ibv_mr *send_mr = NULL;
        uint32_t dctn = rocm_ernic_dc_get_dctn(dct);
        char payload[] = "dc-smoke";
        char *send_buf = NULL;

        send_buf = malloc(sizeof(payload));
        if (!send_buf) {
            perror("malloc");
            goto out;
        }
        memcpy(send_buf, payload, sizeof(payload));
        send_mr =
            ibv_reg_mr(pd, send_buf, sizeof(payload), IBV_ACCESS_LOCAL_WRITE);
        if (!send_mr) {
            perror("ibv_reg_mr send");
            free(send_buf);
            goto out;
        }

        sge.addr = (uint64_t)(uintptr_t)send_buf;
        sge.length = (uint32_t)sizeof(payload);
        sge.lkey = send_mr->lkey;

        if (rocm_ernic_dc_post_send(dci, 1, &sge, 1, dctn,
                                    (uint32_t)dct_init.access_key, 0)) {
            fprintf(stderr, "rocm_ernic_dc_post_send errno=%d\n", errno);
            ibv_dereg_mr(send_mr);
            free(send_buf);
            goto out;
        }

        /*
         * Do not free send memory until hardware (or emulation) has finished
         * reading the SGE for the posted send.  Drain send + recv CQs.
         */
        {
            struct ibv_wc wc;
            int got_send = 0;
            int got_recv = 0;
            unsigned poll_i;

            for (poll_i = 0; poll_i < CQ_POLL_LOOPS && (!got_send || !got_recv);
                 poll_i++) {
                int n;

                n = ibv_poll_cq(send_cq, 1, &wc);
                if (n < 0) {
                    fprintf(stderr, "ibv_poll_cq send_cq errno=%d\n", errno);
                    ibv_dereg_mr(send_mr);
                    free(send_buf);
                    goto out;
                }
                if (n == 1) {
                    if (wc.status != IBV_WC_SUCCESS) {
                        fprintf(stderr, "send_cq wc status %d\n", wc.status);
                        ibv_dereg_mr(send_mr);
                        free(send_buf);
                        goto out;
                    }
                    got_send = 1;
                }

                n = ibv_poll_cq(recv_cq, 1, &wc);
                if (n < 0) {
                    fprintf(stderr, "ibv_poll_cq recv_cq errno=%d\n", errno);
                    ibv_dereg_mr(send_mr);
                    free(send_buf);
                    goto out;
                }
                if (n == 1) {
                    if (wc.status != IBV_WC_SUCCESS) {
                        fprintf(stderr, "recv_cq wc status %d\n", wc.status);
                        ibv_dereg_mr(send_mr);
                        free(send_buf);
                        goto out;
                    }
                    got_recv = 1;
                }

                if (!got_send || !got_recv) {
                    struct timespec ts = {0, 1000};

                    (void)nanosleep(&ts, NULL);
                }
            }
            if (!got_send || !got_recv) {
                fprintf(stderr,
                        "timed out waiting for send/recv completions\n");
                ibv_dereg_mr(send_mr);
                free(send_buf);
                goto out;
            }
        }

        ibv_dereg_mr(send_mr);
        free(send_buf);
    }

    fprintf(stderr, "DC create paths OK (dctn=%u)\n",
            rocm_ernic_dc_get_dctn(dct));
    ret = 0;

out:
    if (dci)
        ibv_destroy_qp(dci);
    if (dct)
        ibv_destroy_qp(dct);
    if (ibsrq)
        ibv_destroy_srq(ibsrq);
    if (recv_mr)
        ibv_dereg_mr(recv_mr);
    free(recv_buf);
    if (send_cq)
        ibv_destroy_cq(send_cq);
    if (recv_cq)
        ibv_destroy_cq(recv_cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return ret;
}
