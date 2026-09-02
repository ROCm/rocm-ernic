/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Guest-side regression test for the send error path accounting:
 * a work request that fails validation before the send_in_flight
 * increment must not decrement it when its error completion drains,
 * and the QP must stay usable afterwards.  Posts a plain SEND on a
 * DCI QP, which the device rejects with an unsupported-opcode error
 * completion, then posts a second plain SEND and requires it to be
 * answered as well.  Requires librocm_ernic and the rocm_ernic char
 * device.  Intended to run inside the VM used by system-tests
 * after the custom provider is installed.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/rocm_ernic_dc.h>
#include <infiniband/verbs.h>

#define SEND_BYTES    64
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
        /* PCI-topology (rocep0s4), driver (rocm-rdma-ernic0), or raw
         * (rocm_ernic0) naming depending on the guest udev rules. */
        if (strstr(name, "rocm_ernic") || strstr(name, "rocep") ||
            strstr(name, "ernic"))
            return list[i];
    }
    return NULL;
}

static int poll_one(struct ibv_cq *cq, struct ibv_wc *wc)
{
    unsigned poll_i;

    for (poll_i = 0; poll_i < CQ_POLL_LOOPS; poll_i++) {
        int n = ibv_poll_cq(cq, 1, wc);

        if (n < 0)
            return -1;
        if (n == 1)
            return 1;
        {
            struct timespec ts = {0, 1000};

            (void)nanosleep(&ts, NULL);
        }
    }
    return 0; /* timed out */
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *send_cq = NULL;
    struct ibv_cq *recv_cq = NULL;
    struct ibv_qp *dci = NULL;
    struct ibv_mr *mr = NULL;
    void *buf = NULL;
    struct ibv_qp_init_attr qp_attr = {};
    struct rocm_ernic_dc_dci_init dci_init = {};
    struct ibv_qp_attr mod_attr = {};
    struct ibv_wc wc;
    int mod_mask;
    int ndev;
    int ret = 1;

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        fprintf(stderr, "No IB devices\n");
        if (dev_list) {
            ibv_free_device_list(dev_list);
            return 77; /* CTest skip code */
        }
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

    buf = calloc(1, SEND_BYTES + RECV_BYTES);
    if (!buf) {
        perror("calloc");
        goto out;
    }

    mr = ibv_reg_mr(pd, buf, SEND_BYTES + RECV_BYTES, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
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

    /* DCI: RESET -> INIT -> RTS */
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
        struct ibv_send_wr swr = {};
        struct ibv_send_wr *bad = NULL;

        sge.addr = (uint64_t)(uintptr_t)buf;
        sge.length = SEND_BYTES;
        sge.lkey = mr->lkey;

        /* Phase 1: a plain SEND on a DCI QP.  The device rejects it
         * with an unsupported-opcode error completion; the error path
         * must not touch the in-flight accounting. */
        swr.wr_id = 0xdead;
        swr.opcode = IBV_WR_SEND;
        swr.sg_list = &sge;
        swr.num_sge = 1;
        if (ibv_post_send(dci, &swr, &bad)) {
            fprintf(stderr, "plain post_send errno=%d\n", errno);
            goto out;
        }

        ret = poll_one(send_cq, &wc);
        if (ret != 1) {
            fprintf(stderr, "no completion for plain SEND\n");
            ret = 1;
            goto out;
        }
        if (wc.status == IBV_WC_SUCCESS) {
            fprintf(stderr, "plain SEND on DCI unexpectedly succeeded\n");
            ret = 1;
            goto out;
        }
        if (wc.wr_id != 0xdead) {
            fprintf(stderr, "error completion wr_id=%llu != 0xdead\n",
                    (unsigned long long)wc.wr_id);
            ret = 1;
            goto out;
        }
        fprintf(stderr, "phase 1 OK: error completion status=%d\n", wc.status);

        /* Phase 2: another plain SEND on the same QP.  Before the fix
         * the error completion above decremented send_in_flight
         * without a matching increment, wrapping it to UINT32_MAX;
         * the send loop then refuses every further work request and
         * no completion ever arrives (stall).  After the fix the
         * second work request is processed and answered, even though
         * it fails for the same reason as the first. */
        swr.wr_id = 0xbeef;
        if (ibv_post_send(dci, &swr, &bad)) {
            fprintf(stderr, "second post_send errno=%d\n", errno);
            ret = 1;
            goto out;
        }

        ret = poll_one(send_cq, &wc);
        if (ret != 1) {
            fprintf(stderr, "no completion for second send - QP stalled after "
                            "error completion\n");
            ret = 1;
            goto out;
        }
        if (wc.wr_id != 0xbeef) {
            fprintf(stderr, "second completion wr_id=%llu != 0xbeef\n",
                    (unsigned long long)wc.wr_id);
            ret = 1;
            goto out;
        }
        fprintf(stderr, "phase 2 OK: QP processed a second work request "
                        "after the error completion\n");
    }

    fprintf(stderr, "DC error accounting OK\n");
    ret = 0;

out:
    if (dci)
        ibv_destroy_qp(dci);
    if (mr)
        ibv_dereg_mr(mr);
    free(buf);
    if (send_cq)
        ibv_destroy_cq(send_cq);
    if (recv_cq)
        ibv_destroy_cq(recv_cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}
