/*
 * test_ud_mesh -- a UD datagram that actually crosses the TCP mesh.
 *
 * Every other UD test in this tree puts both endpoints on one ibv_context and
 * addresses the AH with the device's own GID. That is deliberate and it tests
 * real things, but on the TCP backend it tests only one branch: a self-GID
 * resolves to the local node, so tcp_post_send() takes the
 * `dst_node == tcp_local_node(priv)` shortcut and the datagram never reaches a
 * socket. The wire path -- tcp_resolve_node_from_gid() picking a REMOTE node
 * from the send's dgid, and the datagram being framed, sent, received and
 * delivered on the far side -- had no test at all.
 *
 * This is that test. Two processes on two guests, each with its own device:
 *
 *   test_ud_mesh --server <device>
 *   test_ud_mesh --client <peer-ip> <device>
 *
 * The QPN, GID and Q_Key are exchanged over a plain TCP socket on the guests'
 * emulated Ethernet, because UD gives you no other way to learn them and the
 * mesh is exactly what is under test -- using RDMA to bootstrap RDMA would
 * make a failure unattributable.
 *
 * The server checks the three things the mesh path can get wrong, which are
 * the same three test_ud_transfer checks for a different reason:
 *
 *   1. the datagram arrives at all, i.e. it was routed to the right NODE
 *   2. byte_len is 40 + payload, i.e. the GRH is counted
 *   3. the payload starts at offset 40, i.e. the GRH is really there
 *
 * and additionally that wc.src_qp is the peer's QPN, which on a two-node run
 * is what distinguishes "came from the other guest" from "came back round".
 *
 * Exits 0 on success, 77 when there is no usable device, 1 on a failure.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"

#define GRH_LEN       40
#define MSG_LEN       256
#define QKEY          0x2B2B2B2Bu
#define OOB_PORT      18515
#define POLL_SECONDS  30
#define CONNECT_TRIES 120
#define SEND_WR_ID    0xD1D1u

/* qpn(4) + qkey(4) + gid(16), big-endian, no padding. */
#define OOB_WIRE_LEN 24

/*
 * What the two sides tell each other. Fixed width and explicitly big-endian on
 * the wire: the two guests are the same architecture today, and relying on
 * that is how a test starts passing for the wrong reason on the first mixed
 * run.
 */
struct oob_info {
    uint32_t qpn;
    uint32_t qkey;
    uint8_t gid[16];
};

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

static void ep_destroy(struct ud_ep *ep)
{
    if (ep->mr)
        ibv_dereg_mr(ep->mr);
    free(ep->buf);
    if (ep->qp)
        ibv_destroy_qp(ep->qp);
    if (ep->cq)
        ibv_destroy_cq(ep->cq);
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

/* Returns 1 when a completion of @opcode arrived, 0 on timeout, -1 on error. */
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

/* Fill the payload with a pattern that is obviously not zeroes or a GRH. */
static void fill_payload(unsigned char *at)
{
    for (int i = 0; i < MSG_LEN; i++)
        at[i] = (unsigned char)(0xA5 ^ (i & 0xff));
}

static int send_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(fd, p + done, len - done, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = recv(fd, p + done, len - done, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL: peer closed the out-of-band connection\n");
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Swap our details for the peer's. Both sides write first and then read, which
 * cannot deadlock for a payload this small: it is well under any socket buffer.
 *
 * Serialised field by field rather than by writing the struct, so the wire
 * format is this layout and not whatever padding the ABI chose. The buffer is
 * cleared before use, as anything going to a wire must be.
 */
static int oob_exchange(int fd, const struct oob_info *mine,
                        struct oob_info *theirs)
{
    unsigned char out[OOB_WIRE_LEN];
    unsigned char in[OOB_WIRE_LEN];
    uint32_t be;

    memset(out, 0, sizeof(out));

    be = htonl(mine->qpn);
    memcpy(out, &be, sizeof(be));
    be = htonl(mine->qkey);
    memcpy(out + 4, &be, sizeof(be));
    memcpy(out + 8, mine->gid, sizeof(mine->gid));

    if (send_all(fd, out, sizeof(out)) < 0)
        return -1;

    memset(in, 0, sizeof(in));
    if (recv_all(fd, in, sizeof(in)) < 0)
        return -1;

    memcpy(&be, in, sizeof(be));
    theirs->qpn = ntohl(be);
    memcpy(&be, in + 4, sizeof(be));
    theirs->qkey = ntohl(be);
    memcpy(theirs->gid, in + 8, sizeof(theirs->gid));
    return 0;
}

static int oob_listen(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    int peer = -1;

    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(OOB_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    printf("waiting for the client on port %d\n", OOB_PORT);
    fflush(stdout);

    peer = accept(fd, NULL, NULL);
    if (peer < 0)
        perror("accept");
    close(fd);
    return peer;
}

/*
 * The client may well start before the server's listen(), because the two are
 * launched by separate ssh invocations. Retry rather than racing.
 */
static int oob_connect(const char *peer_ip)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(OOB_PORT);
    if (inet_pton(AF_INET, peer_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "FAIL: '%s' is not an IPv4 address\n", peer_ip);
        return -1;
    }

    for (int attempt = 0; attempt < CONNECT_TRIES; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            perror("socket");
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("connected to %s:%d\n", peer_ip, OOB_PORT);
            return fd;
        }
        close(fd);
        sleep(1);
    }

    fprintf(stderr, "FAIL: could not reach %s:%d -- is the server running?\n",
            peer_ip, OOB_PORT);
    return -1;
}

static void gid_str(const uint8_t *raw, char *out, size_t len)
{
    if (!inet_ntop(AF_INET6, raw, out, (socklen_t)len))
        snprintf(out, len, "<unprintable>");
}

/* The client half: address the peer's GID and send one datagram. */
static int run_client(struct ud_ep *ep, struct ibv_pd *pd,
                      const struct oob_info *peer)
{
    union ibv_gid dgid;
    struct ibv_ah *ah = NULL;
    struct ibv_wc wc;
    int rc = 1;

    memcpy(dgid.raw, peer->gid, sizeof(dgid.raw));

    /*
     * is_global with the PEER's GID. This is the whole point of the test: on
     * the TCP backend tcp_resolve_node_from_gid() matches this against the
     * GIDs each node advertised, so a remote GID is what selects the wire
     * path rather than the local-delivery shortcut.
     */
    struct ibv_ah_attr aa = {
        .is_global = 1,
        .port_num = 1,
        .grh = {.dgid = dgid, .sgid_index = 0, .hop_limit = 64},
    };
    ah = ibv_create_ah(pd, &aa);
    if (!ah) {
        fprintf(stderr, "ibv_create_ah(peer gid): %s\n", strerror(errno));
        fprintf(
            stderr,
            "  The peer's GID is not resolvable from this node. On the TCP\n"
            "  backend that means the mesh never learned it -- check that\n"
            "  both servers advertised their ERNIC_TCP_GUEST_GIDS.\n");
        return 1;
    }

    fill_payload(ep->buf + GRH_LEN);

    struct ibv_sge sge = {
        .addr = (uintptr_t)(ep->buf + GRH_LEN),
        .length = MSG_LEN,
        .lkey = ep->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = SEND_WR_ID,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .wr = {.ud = {.ah = ah,
                      .remote_qpn = peer->qpn,
                      .remote_qkey = peer->qkey}},
    };
    struct ibv_send_wr *bad = NULL;

    if (ibv_post_send(ep->qp, &wr, &bad)) {
        fprintf(stderr, "ibv_post_send: %s\n", strerror(errno));
        goto out;
    }

    if (poll_for(ep->cq, IBV_WC_SEND, &wc) != 1) {
        fprintf(stderr, "FAIL: no send completion within %ds\n", POLL_SECONDS);
        goto out;
    }

    printf("sent %d bytes to QPN %u\n", MSG_LEN, peer->qpn);
    rc = 0;

out:
    ibv_destroy_ah(ah);
    return rc;
}

/* The server half: receive one datagram and check what arrived. */
static int run_server(struct ud_ep *ep, const struct oob_info *peer)
{
    unsigned char expected[MSG_LEN];
    struct ibv_wc wc;

    fill_payload(expected);

    if (poll_for(ep->cq, IBV_WC_RECV, &wc) != 1) {
        fprintf(stderr,
                "FAIL: no datagram arrived within %ds.\n"
                "  The client reported a send completion, so the WQE was\n"
                "  accepted locally and the datagram was lost between the two\n"
                "  nodes -- the mesh wire path, not the verbs layer.\n",
                POLL_SECONDS);
        return 1;
    }

    if (wc.byte_len != GRH_LEN + MSG_LEN) {
        fprintf(stderr,
                "FAIL: byte_len %u, expected %d (payload %d + GRH %d).\n"
                "  A UD receive counts the GRH.\n",
                wc.byte_len, GRH_LEN + MSG_LEN, MSG_LEN, GRH_LEN);
        return 1;
    }

    if (memcmp(ep->buf + GRH_LEN, expected, MSG_LEN) != 0) {
        /* Say which way it is wrong: landing at 0 is the missing-GRH case. */
        if (memcmp(ep->buf, expected, MSG_LEN) == 0)
            fprintf(stderr,
                    "FAIL: payload landed at offset 0, not %d -- no GRH was\n"
                    "  prepended. Consumers that parse at a fixed offset past\n"
                    "  the GRH read shifted data.\n",
                    GRH_LEN);
        else
            fprintf(stderr, "FAIL: payload does not match what was sent\n");
        return 1;
    }

    /*
     * On one node this would be unremarkable. Across two it is the assertion
     * that the datagram came from the OTHER guest rather than looping back.
     */
    if (wc.src_qp != peer->qpn) {
        fprintf(stderr,
                "FAIL: src_qp %u, expected the peer's QPN %u.\n"
                "  The datagram did not come from the remote node.\n",
                wc.src_qp, peer->qpn);
        return 1;
    }

    printf("received %u bytes from QPN %u, payload at offset %d\n", wc.byte_len,
           wc.src_qp, GRH_LEN);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --server [device]\n"
            "       %s --client <peer-ip> [device]\n",
            argv0, argv0);
}

int main(int argc, char **argv)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ud_ep ep = {.cq = NULL};
    struct oob_info mine;
    struct oob_info peer;
    union ibv_gid my_gid;
    enum ernic_device_result lookup;
    const char *peer_ip = NULL;
    char gidbuf[INET6_ADDRSTRLEN];
    int is_server;
    int oob = -1;
    int ndev = 0;
    int rc = 1;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "--server") == 0) {
        is_server = 1;
    } else if (strcmp(argv[1], "--client") == 0) {
        is_server = 0;
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        peer_ip = argv[2];
    } else {
        usage(argv[0]);
        return 1;
    }

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
    printf("%s: device=%s\n", is_server ? "server" : "client",
           ibv_get_device_name(ibdev));

    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto out;
    }

    if (ep_init(ctx, pd, &ep))
        goto out;

    if (ibv_query_gid(ctx, 1, 0, &my_gid)) {
        perror("ibv_query_gid");
        goto out;
    }

    mine.qpn = ep.qp->qp_num;
    mine.qkey = QKEY;
    memcpy(mine.gid, my_gid.raw, sizeof(mine.gid));

    gid_str(mine.gid, gidbuf, sizeof(gidbuf));
    printf("local QPN %u, GID %s\n", mine.qpn, gidbuf);

    /*
     * Post the receive BEFORE the exchange tells the client where to send.
     * The other order is a race the emulator would report as a lost datagram.
     */
    if (is_server && post_recv(&ep, 0x1))
        goto out;

    oob = is_server ? oob_listen() : oob_connect(peer_ip);
    if (oob < 0)
        goto out;

    if (oob_exchange(oob, &mine, &peer) < 0)
        goto out;

    gid_str(peer.gid, gidbuf, sizeof(gidbuf));
    printf("peer QPN %u, GID %s\n", peer.qpn, gidbuf);

    if (memcmp(mine.gid, peer.gid, sizeof(mine.gid)) == 0) {
        fprintf(stderr,
                "FAIL: the peer reports this node's own GID %s.\n"
                "  Both halves are on the same node, so this run would test\n"
                "  the local-delivery shortcut and not the mesh wire.\n",
                gidbuf);
        goto out;
    }

    rc = is_server ? run_server(&ep, &peer) : run_client(&ep, pd, &peer);
    if (rc == 0)
        printf("PASS\n");

out:
    if (oob >= 0)
        close(oob);
    ep_destroy(&ep);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
    return rc;
}
