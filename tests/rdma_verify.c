/*
 * rdma_verify.c — guest-side verbs payload check for the ionic emulator
 *
 * perftest and the ibv_*_pingpong tools report completions, not correctness:
 * an emulator that posts CQEs without moving bytes passes all of them.  This
 * runs SEND, RDMA WRITE, RDMA WRITE_IMM and RDMA READ over one RC QP pair and
 * checks the bytes that actually landed.
 *
 * Both QPs live in this process and are connected to each other, so no
 * out-of-band exchange (and no rdma_cm, hence no GSI traffic) is needed.
 *
 *   cc -o rdma_verify rdma_verify.c -libverbs
 *   ./rdma_verify <device>
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUF_SIZE  8192
#define GID_INDEX 0

static int failures;

struct ep {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint8_t *buf;
};

static void check(const char *what, const uint8_t *got, const uint8_t *want,
                  size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %-16s byte %zu: got 0x%02x want 0x%02x\n", what, i,
                   got[i], want[i]);
            failures++;
            return;
        }
    }
    printf("ok   %-16s %zu bytes\n", what, len);
}

static int ep_init(struct ep *e, struct ibv_context *ctx)
{
    e->pd = ibv_alloc_pd(ctx);
    e->cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    e->buf = aligned_alloc(4096, BUF_SIZE);
    if (!e->pd || !e->cq || !e->buf)
        return -1;
    memset(e->buf, 0, BUF_SIZE);

    e->mr = ibv_reg_mr(e->pd, e->buf, BUF_SIZE,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
    if (!e->mr)
        return -1;

    struct ibv_qp_init_attr ia = {
        .send_cq = e->cq,
        .recv_cq = e->cq,
        .cap = {.max_send_wr = 16,
                .max_recv_wr = 16,
                .max_send_sge = 1,
                .max_recv_sge = 1},
        .qp_type = IBV_QPT_RC,
    };
    e->qp = ibv_create_qp(e->pd, &ia);
    return e->qp ? 0 : -1;
}

static int ep_connect(struct ep *e, uint32_t peer_qpn, union ibv_gid peer_gid,
                      int port)
{
    struct ibv_qp_attr a = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = (uint8_t)port,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
    };
    if (ibv_modify_qp(e->qp, &a,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS))
        return -1;

    struct ibv_qp_attr r = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = peer_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {.is_global = 1,
                    .port_num = (uint8_t)port,
                    .grh = {.dgid = peer_gid,
                            .sgid_index = GID_INDEX,
                            .hop_limit = 1}},
    };
    if (ibv_modify_qp(e->qp, &r,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        return -1;

    struct ibv_qp_attr s = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = 0,
        .max_rd_atomic = 1,
    };
    return ibv_modify_qp(e->qp, &s,
                         IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                             IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                             IBV_QP_MAX_QP_RD_ATOMIC);
}

static int post_recv(struct ep *e, uint64_t off, uint32_t len)
{
    struct ibv_sge sge = {(uintptr_t)e->buf + off, len, e->mr->lkey};
    struct ibv_recv_wr wr = {.wr_id = 1, .sg_list = &sge, .num_sge = 1}, *bad;
    return ibv_post_recv(e->qp, &wr, &bad);
}

static int post_send(struct ep *e, enum ibv_wr_opcode op, uint64_t loff,
                     uint32_t len, uint64_t raddr, uint32_t rkey, uint32_t imm)
{
    struct ibv_sge sge = {(uintptr_t)e->buf + loff, len, e->mr->lkey};
    struct ibv_send_wr wr = {
        .wr_id = 2,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = op,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = imm,
        .wr = {.rdma = {.remote_addr = raddr, .rkey = rkey}},
    };
    struct ibv_send_wr *bad;
    return ibv_post_send(e->qp, &wr, &bad);
}

static int post_atomic(struct ep *e, enum ibv_wr_opcode op, uint64_t loff,
                       uint64_t raddr, uint32_t rkey, uint64_t swap,
                       uint64_t compare_add)
{
    struct ibv_sge sge = {(uintptr_t)e->buf + loff, 8, e->mr->lkey};
    struct ibv_send_wr wr = {
        .wr_id = 3,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = op,
        .send_flags = IBV_SEND_SIGNALED,
        .wr = {.atomic = {.remote_addr = raddr,
                          .compare_add = compare_add,
                          .swap = swap,
                          .rkey = rkey}},
    };
    struct ibv_send_wr *bad;

    /* Fetch-and-add takes its addend in compare_add, not swap. */
    if (op == IBV_WR_ATOMIC_FETCH_AND_ADD)
        wr.wr.atomic.compare_add = swap;

    return ibv_post_send(e->qp, &wr, &bad);
}

/* Spin for one completion; the emulator posts them from the doorbell write,
 * so this never has to wait long. */
static int wait_cq(struct ep *e, const char *what)
{
    struct ibv_wc wc;
    for (int i = 0; i < 5000000; i++) {
        int n = ibv_poll_cq(e->cq, 1, &wc);
        if (n < 0)
            break;
        if (n == 0)
            continue;
        if (wc.status != IBV_WC_SUCCESS) {
            printf("FAIL %-16s completion status %s\n", what,
                   ibv_wc_status_str(wc.status));
            failures++;
            return -1;
        }
        return 0;
    }
    printf("FAIL %-16s no completion\n", what);
    failures++;
    return -1;
}

int main(int argc, char **argv)
{
    const char *want_dev = argc > 1 ? argv[1] : NULL;
    int num;
    struct ibv_device **list = ibv_get_device_list(&num);
    struct ibv_device *dev = NULL;

    for (int i = 0; i < num; i++)
        if (!want_dev || !strcmp(ibv_get_device_name(list[i]), want_dev))
            dev = list[i];
    if (!dev) {
        fprintf(stderr, "no such device\n");
        return 1;
    }

    struct ibv_context *ctx = ibv_open_device(dev);
    if (!ctx) {
        fprintf(stderr, "ibv_open_device failed\n");
        return 1;
    }

    union ibv_gid gid;
    if (ibv_query_gid(ctx, 1, GID_INDEX, &gid)) {
        fprintf(stderr, "ibv_query_gid failed\n");
        return 1;
    }

    struct ep a = {0}, b = {0};
    if (ep_init(&a, ctx) || ep_init(&b, ctx)) {
        fprintf(stderr, "endpoint setup failed\n");
        return 1;
    }
    if (ep_connect(&a, b.qp->qp_num, gid, 1) ||
        ep_connect(&b, a.qp->qp_num, gid, 1)) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }

    uint8_t pattern[BUF_SIZE];
    for (int i = 0; i < BUF_SIZE; i++)
        pattern[i] = (uint8_t)(i * 7 + 3);

    /* SEND: a[0..1023] -> b[4096..] */
    memcpy(a.buf, pattern, 1024);
    memset(b.buf + 4096, 0, 1024);
    post_recv(&b, 4096, 1024);
    post_send(&a, IBV_WR_SEND, 0, 1024, 0, 0, 0);
    wait_cq(&a, "send");
    wait_cq(&b, "send-recv");
    check("SEND", b.buf + 4096, pattern, 1024);

    /* RDMA WRITE: a[1024..3071] -> b[0..2047] */
    memcpy(a.buf + 1024, pattern + 1024, 2048);
    memset(b.buf, 0, 2048);
    post_send(&a, IBV_WR_RDMA_WRITE, 1024, 2048, (uintptr_t)b.buf, b.mr->rkey,
              0);
    wait_cq(&a, "rdma-write");
    check("RDMA WRITE", b.buf, pattern + 1024, 2048);

    /* RDMA WRITE with immediate: consumes a receive on the far side. */
    memcpy(a.buf + 3072, pattern + 3072, 512);
    memset(b.buf + 2048, 0, 512);
    post_recv(&b, 6144, 0);
    post_send(&a, IBV_WR_RDMA_WRITE_WITH_IMM, 3072, 512,
              (uintptr_t)b.buf + 2048, b.mr->rkey, htonl(0xfeedface));
    wait_cq(&a, "write-imm");
    wait_cq(&b, "write-imm-recv");
    check("RDMA WRITE_IMM", b.buf + 2048, pattern + 3072, 512);

    /* RDMA READ: b[3072..4095] -> a[5120..] */
    memcpy(b.buf + 3072, pattern + 5120, 1024);
    memset(a.buf + 5120, 0, 1024);
    post_send(&a, IBV_WR_RDMA_READ, 5120, 1024, (uintptr_t)b.buf + 3072,
              b.mr->rkey, 0);
    wait_cq(&a, "rdma-read");
    check("RDMA READ", a.buf + 5120, pattern + 5120, 1024);

    /* Atomics operate on one aligned 8-byte word and return the original. */
    uint64_t *remote_word = (uint64_t *)(b.buf + 4096);
    uint64_t *result = (uint64_t *)(a.buf + 6144);

    *remote_word = 100;
    *result = 0;
    post_atomic(&a, IBV_WR_ATOMIC_FETCH_AND_ADD, 6144, (uintptr_t)remote_word,
                b.mr->rkey, 23, 0);
    wait_cq(&a, "fetch-add");
    if (*result == 100 && *remote_word == 123) {
        printf("ok   %-16s 100 + 23 -> 123, returned 100\n", "ATOMIC FA");
    } else {
        printf("FAIL %-16s returned %lu, memory %lu (want 100 / 123)\n",
               "ATOMIC FA", (unsigned long)*result,
               (unsigned long)*remote_word);
        failures++;
    }

    /* Matching compare: swaps and returns the old value. */
    *result = 0;
    post_atomic(&a, IBV_WR_ATOMIC_CMP_AND_SWP, 6144, (uintptr_t)remote_word,
                b.mr->rkey, 777, 123);
    wait_cq(&a, "cmp-swap-hit");
    if (*result == 123 && *remote_word == 777) {
        printf("ok   %-16s 123 == 123 -> 777, returned 123\n", "ATOMIC CS hit");
    } else {
        printf("FAIL %-16s returned %lu, memory %lu (want 123 / 777)\n",
               "ATOMIC CS hit", (unsigned long)*result,
               (unsigned long)*remote_word);
        failures++;
    }

    /* Mismatching compare: memory must be left alone. */
    *result = 0;
    post_atomic(&a, IBV_WR_ATOMIC_CMP_AND_SWP, 6144, (uintptr_t)remote_word,
                b.mr->rkey, 999, 5);
    wait_cq(&a, "cmp-swap-miss");
    if (*result == 777 && *remote_word == 777) {
        printf("ok   %-16s 777 != 5 -> unchanged, returned 777\n",
               "ATOMIC CS miss");
    } else {
        printf("FAIL %-16s returned %lu, memory %lu (want 777 / 777)\n",
               "ATOMIC CS miss", (unsigned long)*result,
               (unsigned long)*remote_word);
        failures++;
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
