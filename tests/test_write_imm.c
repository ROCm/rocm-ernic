/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Loopback WRITE_WITH_IMM responder-delivery test.
 *
 * Pairs two RC QPs on one rocm_ernic device, then runs several
 * WRITE_WITH_IMM iterations. Each iteration posts a fresh receive,
 * sends a payload with a randomly generated immediate value, and
 * verifies both completions: the sender WC must be a successful
 * RDMA_WRITE, and the responder WC must be RECV_RDMA_WITH_IMM with
 * the IBV_WC_WITH_IMM flag, the exact immediate value, the posted
 * receive wr_id, and the payload length. The payload itself is
 * compared against the data written into the remote buffer.
 *
 * Exits 0 when every iteration passes, 77 when no rocm_ernic device
 * is present (CTest skip), and 1 on any validation failure.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#define ITERATIONS    4
#define PAYLOAD_LEN   64
#define BUFFER_LEN    4096 /* page-sized so the MR chunk count divides cleanly */
#define POLL_TRIES    2000
#define POLL_DELAY_US 1000

static struct ibv_device *find_rocm_ernic(struct ibv_device **list, int n)
{
    for (int i = 0; i < n; i++) {
        const char *name;

        if (!list[i])
            continue;
        name = ibv_get_device_name(list[i]);
        /*
         * The kernel registers the device as rocm_ernic%d. Systems
         * with persistent RDMA naming see a PCI-topology name such
         * as rocep0s4, and the rocm-ernic udev rule renames it to
         * rocm-rdma-ernic0. Match all three.
         */
        if (strstr(name, "rocm_ernic") || strstr(name, "rocep") ||
            strstr(name, "rocm-rdma-ernic"))
            return list[i];
    }
    return NULL;
}

/* Bounded poll for a single completion. Returns 0 on success, -1 on
 * timeout or poll error. */
static int poll_one(struct ibv_cq *cq, struct ibv_wc *wc)
{
    for (int i = 0; i < POLL_TRIES; i++) {
        int n = ibv_poll_cq(cq, 1, wc);

        if (n > 0)
            return 0;
        if (n < 0) {
            fprintf(stderr, "ibv_poll_cq failed errno=%d\n", errno);
            return -1;
        }
        {
            struct timespec ts = {0, POLL_DELAY_US * 1000};

            nanosleep(&ts, NULL);
        }
    }
    return -1;
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cqa = NULL, *cqb = NULL;
    struct ibv_qp *qpa = NULL, *qpb = NULL;
    struct ibv_mr *dst_mr = NULL, *src_mr = NULL;
    unsigned char *dst_buf = NULL, *src_buf = NULL;
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr attr;
    union ibv_gid gid;
    int ret = 1;
    int ndev;
    int it;
    uint32_t seed;

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        fprintf(stderr, "No IB devices found - skipping test\n");
        if (dev_list)
            ibv_free_device_list(dev_list);
        return 77; /* CTest skip code */
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
    printf("device=%s\n", ibv_get_device_name(ibdev));

    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto out;
    }

    cqa = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    cqb = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cqa || !cqb) {
        fprintf(stderr, "ibv_create_cq failed\n");
        goto out;
    }

    /*
     * Both buffers are page-aligned and page-sized: the emulated
     * MR mapping rejects a region whose chunk count does not divide
     * the page-rounded length.
     */
    if (posix_memalign((void **)&dst_buf, BUFFER_LEN, BUFFER_LEN)) {
        fprintf(stderr, "posix_memalign failed for dst_buf\n");
        dst_buf = NULL;
        goto out;
    }
    if (posix_memalign((void **)&src_buf, BUFFER_LEN, BUFFER_LEN)) {
        fprintf(stderr, "posix_memalign failed for src_buf\n");
        src_buf = NULL;
        goto out;
    }

    dst_mr = ibv_reg_mr(pd, dst_buf, BUFFER_LEN,
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    src_mr = ibv_reg_mr(pd, src_buf, BUFFER_LEN, IBV_ACCESS_LOCAL_WRITE);
    if (!dst_mr || !src_mr) {
        fprintf(stderr, "ibv_reg_mr failed\n");
        goto out;
    }

    memset(&init, 0, sizeof(init));
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 8;
    init.cap.max_recv_wr = 8;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    init.send_cq = cqa;
    init.recv_cq = cqa;
    qpa = ibv_create_qp(pd, &init);
    if (!qpa) {
        perror("ibv_create_qp qpa");
        goto out;
    }
    init.send_cq = cqb;
    init.recv_cq = cqb;
    qpb = ibv_create_qp(pd, &init);
    if (!qpb) {
        perror("ibv_create_qp qpb");
        goto out;
    }
    printf("qpa=%u qpb=%u\n", qpa->qp_num, qpb->qp_num);

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_LOCAL_WRITE;
    {
        int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                   IBV_QP_ACCESS_FLAGS;

        if (ibv_modify_qp(qpa, &attr, mask) ||
            ibv_modify_qp(qpb, &attr, mask)) {
            fprintf(stderr, "INIT transition failed\n");
            goto out;
        }
    }

    /* The port reports LID 0 (RoCE-style); route via GID 0. */
    if (ibv_query_gid(ctx, 1, 0, &gid) != 0) {
        fprintf(stderr, "ibv_query_gid failed\n");
        goto out;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.rq_psn = 1;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.port_num = 1;
    {
        int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                   IBV_QP_MIN_RNR_TIMER;

        attr.dest_qp_num = qpb->qp_num;
        if (ibv_modify_qp(qpa, &attr, mask)) {
            fprintf(stderr, "RTR qpa failed\n");
            goto out;
        }
        attr.dest_qp_num = qpa->qp_num;
        if (ibv_modify_qp(qpb, &attr, mask)) {
            fprintf(stderr, "RTR qpb failed\n");
            goto out;
        }
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 1;
    attr.max_rd_atomic = 1;
    {
        int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                   IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

        if (ibv_modify_qp(qpa, &attr, mask) ||
            ibv_modify_qp(qpb, &attr, mask)) {
            fprintf(stderr, "RTS transition failed\n");
            goto out;
        }
    }

    seed = (uint32_t)getpid() ^ (uint32_t)time(NULL);
    srand(seed);
    printf("seed=%" PRIu32 "\n", seed);

    for (it = 0; it < ITERATIONS; it++) {
        uint64_t recv_wr_id = 0x5245435632494d4dULL + (uint64_t)it;
        uint32_t imm = (uint32_t)rand();
        struct ibv_sge rsge, wsge;
        struct ibv_recv_wr rwr;
        struct ibv_recv_wr *bad_r = NULL;
        struct ibv_send_wr swr;
        struct ibv_send_wr *bad_s = NULL;
        struct ibv_wc wc;

        /* Fresh receive per iteration: each WRITE_WITH_IMM consumes one. */
        memset(&rsge, 0, sizeof(rsge));
        rsge.addr = (uintptr_t)dst_buf;
        rsge.length = BUFFER_LEN;
        rsge.lkey = dst_mr->lkey;
        memset(&rwr, 0, sizeof(rwr));
        rwr.wr_id = recv_wr_id;
        rwr.sg_list = &rsge;
        rwr.num_sge = 1;
        if (ibv_post_recv(qpb, &rwr, &bad_r)) {
            fprintf(stderr, "iter %d: ibv_post_recv failed\n", it);
            goto out;
        }

        for (int i = 0; i < PAYLOAD_LEN; i++)
            src_buf[i] = (unsigned char)(rand() & 0xff);

        memset(&wsge, 0, sizeof(wsge));
        wsge.addr = (uintptr_t)src_buf;
        wsge.length = PAYLOAD_LEN;
        wsge.lkey = src_mr->lkey;
        memset(&swr, 0, sizeof(swr));
        swr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        swr.send_flags = IBV_SEND_SIGNALED;
        swr.imm_data = htonl(imm);
        swr.wr.rdma.remote_addr = (uintptr_t)dst_buf;
        swr.wr.rdma.rkey = dst_mr->rkey;
        swr.sg_list = &wsge;
        swr.num_sge = 1;
        if (ibv_post_send(qpa, &swr, &bad_s)) {
            fprintf(stderr, "iter %d: ibv_post_send failed\n", it);
            goto out;
        }

        if (poll_one(cqa, &wc)) {
            fprintf(stderr, "iter %d: sender CQE timeout\n", it);
            goto out;
        }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RDMA_WRITE) {
            fprintf(stderr, "iter %d: sender wc status=%d opcode=%d\n", it,
                    wc.status, wc.opcode);
            goto out;
        }

        if (poll_one(cqb, &wc)) {
            fprintf(stderr,
                    "iter %d: responder CQE timeout (immediate dropped?)\n",
                    it);
            goto out;
        }
        printf("iter %d: recv wc status=%d opcode=%d flags=%#x imm=%08" PRIx32
               " bytes=%u wr_id=%" PRIx64 "\n",
               it, wc.status, wc.opcode, wc.wc_flags, ntohl(wc.imm_data),
               wc.byte_len, wc.wr_id);
        if (wc.status != IBV_WC_SUCCESS ||
            wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
            !(wc.wc_flags & IBV_WC_WITH_IMM) || ntohl(wc.imm_data) != imm ||
            wc.wr_id != recv_wr_id || wc.byte_len != PAYLOAD_LEN) {
            fprintf(stderr, "iter %d: VALIDATION FAILED\n", it);
            goto out;
        }

        if (memcmp(dst_buf, src_buf, PAYLOAD_LEN) != 0) {
            fprintf(stderr, "iter %d: payload mismatch\n", it);
            goto out;
        }
    }

    printf("WRITE_WITH_IMM round trip verified over %d iterations\n",
           ITERATIONS);
    ret = 0;

out:
    if (qpa)
        ibv_destroy_qp(qpa);
    if (qpb)
        ibv_destroy_qp(qpb);
    if (dst_mr)
        ibv_dereg_mr(dst_mr);
    if (src_mr)
        ibv_dereg_mr(src_mr);
    free(dst_buf);
    free(src_buf);
    if (cqa)
        ibv_destroy_cq(cqa);
    if (cqb)
        ibv_destroy_cq(cqb);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}
