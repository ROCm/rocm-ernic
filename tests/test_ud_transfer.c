// Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
//
// SPDX-License-Identifier: MIT
//
// test_ud_transfer -- what ibv_ud_pingpong cannot tell you.
//
// ud_pingpong is the obvious UD test and it is a weak one on loopback: it
// sends from buf + 40 and receives into buf, the same buffer, so the bytes it
// verifies are already where it expects whether or not the device prepended a
// GRH, and whether or not the datagram reached a different QP at all. It
// passed against an emulator that delivered every datagram straight back into
// the sending QP and prepended nothing.
//
// This uses two QPs and two separate buffers, and checks the three things
// that distinguishes:
//
//   1. the datagram reaches the OTHER QP, not the sender's
//   2. byte_len is 40 + payload, i.e. the GRH is counted
//   3. the payload starts at offset 40, i.e. the GRH is really there
//
// and then repeats the send inline, which is the path UCX's ud_verbs
// actually uses for small messages.
//
// Exits 0 on success, 77 when there is no usable device, 1 on a failure.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"

#define GRH_LEN      40
#define MSG_LEN      256
#define QKEY         0x11111111u
#define POLL_SECONDS 10

struct ud_ep {
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    unsigned char *buf; /* GRH_LEN + MSG_LEN */
};

static int ep_init(struct ibv_context *ctx, struct ibv_pd *pd, struct ud_ep *ep)
{
    ep->cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!ep->cq) {
        perror("ibv_create_cq");
        return -1;
    }

    struct ibv_qp_init_attr ia = {
        .send_cq = ep->cq,
        .recv_cq = ep->cq,
        .qp_type = IBV_QPT_UD,
        .cap = {.max_send_wr = 16,
                .max_recv_wr = 16,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 256},
    };
    ep->qp = ibv_create_qp(pd, &ia);
    if (!ep->qp) {
        perror("ibv_create_qp(UD)");
        return -1;
    }

    struct ibv_qp_attr a = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qkey = QKEY,
    };
    if (ibv_modify_qp(ep->qp, &a,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_QKEY)) {
        perror("modify_qp(INIT)");
        return -1;
    }

    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ep->qp, &a, IBV_QP_STATE)) {
        perror("modify_qp(RTR)");
        return -1;
    }

    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = 0;
    if (ibv_modify_qp(ep->qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        perror("modify_qp(RTS)");
        return -1;
    }

    ep->buf = calloc(1, GRH_LEN + MSG_LEN);
    if (!ep->buf) {
        perror("calloc");
        return -1;
    }
    ep->mr = ibv_reg_mr(pd, ep->buf, GRH_LEN + MSG_LEN, IBV_ACCESS_LOCAL_WRITE);
    if (!ep->mr) {
        perror("ibv_reg_mr");
        return -1;
    }
    return 0;
}

static int post_recv(struct ud_ep *ep, uint64_t wr_id)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)ep->buf,
        .length = GRH_LEN + MSG_LEN,
        .lkey = ep->mr->lkey,
    };
    struct ibv_recv_wr wr = {.wr_id = wr_id, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad = NULL;
    if (ibv_post_recv(ep->qp, &wr, &bad)) {
        perror("ibv_post_recv");
        return -1;
    }
    return 0;
}

/* Returns 1 when a completion of @opcode arrived, 0 on timeout. */
static int poll_for(struct ibv_cq *cq, enum ibv_wc_opcode opcode,
                    struct ibv_wc *out)
{
    time_t deadline = time(NULL) + POLL_SECONDS;
    while (time(NULL) < deadline) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0)
            return -1;
        if (n == 0)
            continue;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "completion failed: %s\n",
                    ibv_wc_status_str(wc.status));
            return -1;
        }
        if (wc.opcode == opcode) {
            *out = wc;
            return 1;
        }
    }
    return 0;
}

static int do_send(struct ud_ep *src, struct ud_ep *dst, struct ibv_ah *ah,
                   int inlined)
{
    for (int i = 0; i < MSG_LEN; i++)
        src->buf[GRH_LEN + i] = (unsigned char)(0x5A ^ (i & 0xff));
    memset(dst->buf, 0, GRH_LEN + MSG_LEN);

    struct ibv_sge sge = {
        .addr = (uintptr_t)(src->buf + GRH_LEN),
        .length = MSG_LEN,
        .lkey = src->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 0xD1,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED | (inlined ? IBV_SEND_INLINE : 0),
        .wr = {.ud = {.ah = ah,
                      .remote_qpn = dst->qp->qp_num,
                      .remote_qkey = QKEY}},
    };
    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(src->qp, &wr, &bad)) {
        fprintf(stderr, "ibv_post_send(%s): %s\n", inlined ? "inline" : "sge",
                strerror(errno));
        return -1;
    }

    struct ibv_wc wc;
    if (poll_for(src->cq, IBV_WC_SEND, &wc) != 1) {
        fprintf(stderr, "FAIL: no send completion (%s)\n",
                inlined ? "inline" : "sge");
        return -1;
    }

    if (poll_for(dst->cq, IBV_WC_RECV, &wc) != 1) {
        fprintf(stderr,
                "FAIL: no receive completion on the PEER qp (%s).\n"
                "  The datagram did not reach QPN %u. A device that ignores\n"
                "  the WQE's dest_qpn delivers it to the sender instead,\n"
                "  which ibv_ud_pingpong cannot distinguish on loopback.\n",
                inlined ? "inline" : "sge", dst->qp->qp_num);
        return -1;
    }

    if (wc.byte_len != GRH_LEN + MSG_LEN) {
        fprintf(stderr,
                "FAIL: byte_len %u, expected %d (payload %d + GRH %d).\n"
                "  A UD receive counts the GRH.\n",
                wc.byte_len, GRH_LEN + MSG_LEN, MSG_LEN, GRH_LEN);
        return -1;
    }

    if (memcmp(dst->buf + GRH_LEN, src->buf + GRH_LEN, MSG_LEN) != 0) {
        /* Say which way it is wrong: landing at 0 is the missing-GRH case. */
        if (memcmp(dst->buf, src->buf + GRH_LEN, MSG_LEN) == 0)
            fprintf(stderr,
                    "FAIL: payload landed at offset 0, not %d -- no GRH was\n"
                    "  prepended. Consumers that parse at a fixed offset past\n"
                    "  the GRH read shifted data.\n",
                    GRH_LEN);
        else
            fprintf(stderr, "FAIL: payload mismatch (%s)\n",
                    inlined ? "inline" : "sge");
        return -1;
    }

    printf("  %-6s ok: %u bytes to QPN %u, payload at offset %d\n",
           inlined ? "inline" : "sge", wc.byte_len, dst->qp->qp_num, GRH_LEN);
    return 0;
}

/* do_send without the per-iteration printf. */
static int do_send_quiet(struct ud_ep *src, struct ud_ep *dst,
                         struct ibv_ah *ah, int inlined)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)(src->buf + GRH_LEN),
        .length = MSG_LEN,
        .lkey = src->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 0xD2,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED | (inlined ? IBV_SEND_INLINE : 0),
        .wr = {.ud = {.ah = ah,
                      .remote_qpn = dst->qp->qp_num,
                      .remote_qkey = QKEY}},
    };
    struct ibv_send_wr *bad = NULL;
    struct ibv_wc wc;

    if (ibv_post_send(src->qp, &wr, &bad))
        return -1;
    if (poll_for(src->cq, IBV_WC_SEND, &wc) != 1)
        return -1;
    if (poll_for(dst->cq, IBV_WC_RECV, &wc) != 1)
        return -1;
    return 0;
}

int main(void)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_ah *ah = NULL;
    struct ud_ep a = {0}, b = {0};
    union ibv_gid gid;
    unsigned char *src_two = NULL;
    struct ibv_mr *mr_two = NULL;
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

    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto out;
    }

    if (ep_init(ctx, pd, &a) || ep_init(ctx, pd, &b))
        goto out;
    printf("two UD QPs: sender=%u receiver=%u\n", a.qp->qp_num, b.qp->qp_num);

    /* Which GID index to build the handle from. Index 0 is the link-local
     * fe80:: one; UCX picks the IPv4-mapped RoCEv2 GID instead, so make it
     * selectable rather than assuming they behave the same. */
    const char *gidx_env = getenv("ERNIC_GID_INDEX");
    int gidx = gidx_env ? atoi(gidx_env) : 0;
    if (ibv_query_gid(ctx, 1, gidx, &gid)) {
        perror("ibv_query_gid");
        goto out;
    }
    printf("using GID index %d\n", gidx);

    struct ibv_ah_attr aa = {
        .is_global = 1,
        .port_num = 1,
        .grh = {.dgid = gid, .sgid_index = (uint8_t)gidx, .hop_limit = 1},
    };
    ah = ibv_create_ah(pd, &aa);
    if (!ah) {
        perror("ibv_create_ah");
        goto out;
    }
    printf("created AH\n");

    src_two = calloc(1, 128);
    if (!src_two) {
        perror("calloc");
        goto out;
    }
    mr_two = ibv_reg_mr(pd, src_two, 128, IBV_ACCESS_LOCAL_WRITE);
    if (!mr_two) {
        perror("ibv_reg_mr(two)");
        goto out;
    }

    if (post_recv(&b, 0xB1) || do_send(&a, &b, ah, 0))
        goto out;
    if (post_recv(&b, 0xB2) || do_send(&a, &b, ah, 1))
        goto out;

    /*
     * Sustained run. One send and one receive at a time is enough to prove
     * the plumbing; it is not enough to prove the queues keep being
     * replenished, which is where a device that stops consuming reposted
     * receives shows up -- as a stall after roughly one queue depth, long
     * after a two-message test has passed.
     */
    const char *iter_env = getenv("ERNIC_UD_ITERS");
    int iters = iter_env ? atoi(iter_env) : 0;
    for (int n = 0; n < iters; n++) {
        if (post_recv(&b, 0xC000 + n) || do_send_quiet(&a, &b, ah, n & 1)) {
            fprintf(stderr, "FAIL: stalled at iteration %d of %d\n", n, iters);
            goto out;
        }
    }
    if (iters)
        printf("sustained %d iterations ok\n", iters);

    /*
     * Two inline segments in one send, which is the shape UCX's ud_verbs
     * actually posts: a 16-byte header segment followed by the payload
     * ("[inl len 16] [inl len 64]" in its trace). A device that only honours
     * the first segment, or sizes the copy from one of them, delivers a
     * short or shifted message -- and a single-segment test never notices.
     */
    if (post_recv(&b, 0xD00D))
        goto out;
    {
        unsigned char hdr[16];
        for (int i = 0; i < 16; i++)
            hdr[i] = (unsigned char)(0xE0 + i);
        memcpy(src_two, hdr, 16);
        for (int i = 0; i < 64; i++)
            src_two[16 + i] = (unsigned char)(0x30 ^ i);

        struct ibv_sge sg[2] = {
            {.addr = (uintptr_t)src_two, .length = 16, .lkey = mr_two->lkey},
            {.addr = (uintptr_t)(src_two + 16),
             .length = 64,
             .lkey = mr_two->lkey},
        };
        struct ibv_send_wr wr = {
            .wr_id = 0xD00D,
            .sg_list = sg,
            .num_sge = 2,
            .opcode = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE,
            .wr = {.ud = {.ah = ah,
                          .remote_qpn = b.qp->qp_num,
                          .remote_qkey = QKEY}},
        };
        struct ibv_send_wr *bad = NULL;
        memset(b.buf, 0, GRH_LEN + MSG_LEN);
        if (ibv_post_send(a.qp, &wr, &bad)) {
            perror("ibv_post_send(2-seg inline)");
            goto out;
        }
        struct ibv_wc wc;
        if (poll_for(a.cq, IBV_WC_SEND, &wc) != 1) {
            fprintf(stderr, "FAIL: no send completion for 2-segment inline\n");
            goto out;
        }
        if (poll_for(b.cq, IBV_WC_RECV, &wc) != 1) {
            fprintf(stderr, "FAIL: 2-segment inline never arrived\n");
            goto out;
        }
        if (wc.byte_len != GRH_LEN + 80) {
            fprintf(stderr,
                    "FAIL: 2-segment inline delivered %u bytes, expected %d.\n"
                    "  Both segments must be concatenated: 16 + 64.\n",
                    wc.byte_len, GRH_LEN + 80);
            goto out;
        }
        if (memcmp(b.buf + GRH_LEN, src_two, 80) != 0) {
            fprintf(stderr, "FAIL: 2-segment inline payload mismatch\n");
            goto out;
        }
        printf("  2-seg inline ok: %u bytes (16+64 concatenated)\n",
               wc.byte_len);
    }

    printf("PASS: datagrams reached the peer QP with a GRH\n");
    rc = 0;

out:
    if (mr_two)
        ibv_dereg_mr(mr_two);
    free(src_two);
    if (ah)
        ibv_destroy_ah(ah);
    for (struct ud_ep *e = &a; e; e = (e == &a) ? &b : NULL) {
        if (e->mr)
            ibv_dereg_mr(e->mr);
        free(e->buf);
        if (e->qp)
            ibv_destroy_qp(e->qp);
        if (e->cq)
            ibv_destroy_cq(e->cq);
    }
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
    return rc;
}
