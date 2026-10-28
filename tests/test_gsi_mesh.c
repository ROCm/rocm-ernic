/*
 * test_gsi_mesh -- a CM MAD on the GSI QP that has to cross the TCP mesh.
 *
 * rdma_cm between two guests never completes, and the reason is here rather
 * than in the guest: the kernel posts the CM REQ on QP1, the emulator decides
 * where it goes, and nothing forwards it to the node that owns the
 * destination GID.  test_ud_mesh covers the same journey for a UD datagram,
 * but it needs two VMs and two real devices, so the GSI case had no test that
 * CI could run.
 *
 * This is that test, in one process: two whole devices, one the mesh manager
 * and one a worker, joined over loopback TCP by the same backend the server
 * uses.  No VM, no RDMA hardware, no root.
 *
 * It checks the chain a CM REQ depends on, in the order it breaks:
 *
 *   1. the two nodes mesh and each is assigned a node id
 *   2. a GID claimed after the mesh formed resolves on the peer, which is
 *      what ERNIC_TCP_GUEST_GIDS cannot do -- it is read once at startup, and
 *      one environment variable cannot describe two devices anyway
 *   3. each node resolves the other's GID to a REMOTE node, not to itself
 *   4. a GSI send addressed to the peer's GID arrives on the peer's QP1
 *   5. and is NOT delivered back to the sender's own QP1, which is what
 *      happens today: the destination QPN in the WQE is 1, so an unresolved
 *      peer makes the datagram land on the sender's own GSI QP, silently.
 *
 * 4 and 5 are the same event seen from both ends. Checking only that the
 * sender got nothing would pass if the MAD vanished; checking only that the
 * peer got it would pass if it were also copied back.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "byteorder.h"
#include "ernic_mesh.h"
#include "ionic_harness.h"
#include "parse_int.h"
#include "pvrdma/pvrdma.h"
#include "rdma/rdma_backend_tcp_priv.h"

/* The mesh listens on loopback. Overridable so a CI runner that already has
 * something here can move it rather than fail. */
#define GM_PORT_DEFAULT 18640

/* Two addresses on a subnet nothing else in the tree uses, so a stray claim
 * from another test cannot make these contested. */
#define GM_IP_A 0xc0a8fa01u /* 192.168.250.1 */
#define GM_IP_B 0xc0a8fa02u /* 192.168.250.2 */

#define GM_CQ      1u
#define GM_AH      7u
#define GM_MR      0x0800000au
#define GM_VA      UINT64_C(0x7f6100000000)
#define GM_TX_OFF  0x0000u
#define GM_RX_OFF  0x0200u
#define GM_MAD_LEN 256u
/* The Q_Key ib_mad uses for GSI; the device checks it on a datagram. */
#define GM_QKEY 0x80010000u

static int failures;

static bool gm_check(bool cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool gm_check(bool cond, const char *fmt, ...)
{
    if (cond)
        return true;

    va_list ap;

    va_start(ap, fmt);
    fputs("FAIL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    failures++;
    return false;
}

/* The IPv4-mapped GID the RoCE header template for @ip produces. */
static void gm_gid_for_ip(uint8_t gid[16], uint32_t ip)
{
    memset(gid, 0, 16);
    gid[10] = 0xff;
    gid[11] = 0xff;
    gid[12] = (uint8_t)(ip >> 24);
    gid[13] = (uint8_t)(ip >> 16);
    gid[14] = (uint8_t)(ip >> 8);
    gid[15] = (uint8_t)ip;
}

/* Drive both devices' main loops. The mesh runs on its own threads, so this
 * is only about letting each device process what arrived. */
static void gm_pump(struct ih *a, struct ih *b, unsigned rounds)
{
    for (unsigned i = 0; i < rounds; i++) {
        ih_poll(a);
        ih_poll(b);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

/*
 * One device's GSI plumbing: a CQ, QP1 and an MR big enough for a MAD out
 * and a MAD plus its GRH back.
 */
struct gm_ep {
    struct ih_cq cq;
    struct ih_qp qp;
    struct ih_mr mr;
};

static bool gm_ep_open(struct ih *h, struct gm_ep *ep)
{
    return ih_cq_create(h, &ep->cq, GM_CQ, 5) &&
           ih_qp_create(h, &ep->qp, 1, IH_QPT_UD, &ep->cq, &ep->cq, NULL) &&
           ih_mr_create(h, &ep->mr, GM_MR, GM_VA, 2 * IH_PAGE_SIZE,
                        IH_ACCESS_ALL, false);
}

struct gm_counts {
    unsigned sends;
    unsigned recvs;
    bool recv_ipv4; /* the last receive's IONIC_V1_CQE_RECV_IS_IPV4 */
};

/* Drain a CQ, counting what came back. */
static void gm_drain(struct ih *h, struct gm_ep *ep, struct gm_counts *c)
{
    struct ih_cqe e;

    while (ih_cq_poll(h, &ep->cq, &e)) {
        if (e.type == IH_CQE_TYPE_RECV) {
            c->recvs++;
            c->recv_ipv4 = e.is_ipv4;
        } else {
            c->sends++;
        }
    }
}

/*
 * Wait for @h to resolve @gid to some node, up to ~5s. Registration and the
 * advertisement that follows it are asynchronous, so a bare check races.
 */
static uint32_t gm_wait_resolve(struct ih *a, struct ih *b, struct ih *h,
                                const uint8_t gid[16])
{
    for (unsigned i = 0; i < 250; i++) {
        uint32_t node = ernic_mesh_node_from_gid(h->dev->pvrdma_handle, gid);

        if (node != UINT32_MAX)
            return node;
        gm_pump(a, b, 1);
    }
    return UINT32_MAX;
}

/*
 * Wait, up to ~5s, for the manager @a to have seen node @node_id's
 * connection end, as registration checks it (tcp_collect_dead_claimants()):
 * its receive thread has read the hang-up, or the health check has retired
 * the connection.  Nothing public changes then -- the node owns its GIDs
 * until something declares it dead -- so this asks the backend.
 */
static bool gm_wait_hung_up(struct ih *a, uint32_t node_id)
{
    TcpBackendPrivate *priv =
        a->dev->pvrdma_handle->backend_dev.backend_private;

    for (unsigned i = 0; i < 2500; i++) {
        TcpConnection *conn = tcp_connection_lookup(priv, node_id);
        bool gone = !conn || tcp_conn_dead(conn);

        tcp_connection_unref(conn);
        if (gone)
            return true;
        ih_poll(a);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return false;
}

int main(void)
{
    const char *env = getenv("ERNIC_TEST_MESH_PORT");
    uint16_t port = GM_PORT_DEFAULT;

    if (env && !ernic_parse_port(env, &port)) {
        fprintf(stderr, "FAIL: ERNIC_TEST_MESH_PORT=\"%s\" is not a port\n",
                env);
        return 1;
    }

    char mgr[64], wrk[64];

    snprintf(mgr, sizeof(mgr), "tcp:manager:listen:%u", (unsigned)port);
    snprintf(wrk, sizeof(wrk), "tcp:worker:127.0.0.1:%u", (unsigned)port);

    struct ih a, b;
    uint32_t node_a, node_b, seen_a, seen_b, node_b2, seen_b2;
    struct ih_sge rx, rx_self, tx;
    struct ih_send s;
    struct gm_counts ca, cb;

    /*
     * Creating either device needs no VM, no RDMA hardware and no root, so a
     * failure here is a regression in device realization or the backend, not
     * a reason to skip: it must fail the test.
     */
    if (!ih_create(&a, mgr)) {
        fprintf(stderr, "FAIL: no mesh manager on port %u\n", (unsigned)port);
        return 1;
    }
    if (!ih_create(&b, wrk)) {
        ih_destroy(&a);
        fprintf(stderr, "FAIL: no mesh worker\n");
        return 1;
    }
    if (!gm_check(ih_bringup(&a) && ih_bringup(&b), "device bring-up failed"))
        goto out;

    /* 1. The mesh forms. The manager is always node 0 (tcp_init()); the
     * worker is assigned a non-zero id by REGISTER_RESP. */
    node_a = UINT32_MAX;
    node_b = UINT32_MAX;

    for (unsigned i = 0; i < 250; i++) {
        node_a = ernic_mesh_local_node(a.dev->pvrdma_handle);
        node_b = ernic_mesh_local_node(b.dev->pvrdma_handle);
        if (node_a != UINT32_MAX && node_b != UINT32_MAX && node_b != 0)
            break;
        gm_pump(&a, &b, 1);
    }
    if (!gm_check(node_a == 0, "the manager is node %u, want 0", node_a))
        goto out;
    if (!gm_check(node_b != UINT32_MAX && node_b != 0,
                  "worker never registered (node id %u)", node_b))
        goto out;
    gm_check(node_a != node_b, "both nodes were assigned id %u", node_a);

    /* 2. Claim a GID on each AFTER the mesh formed. */
    uint8_t gid_a[16], gid_b[16];

    gm_gid_for_ip(gid_a, GM_IP_A);
    gm_gid_for_ip(gid_b, GM_IP_B);
    gm_check(ernic_mesh_claim_gid(a.dev->pvrdma_handle, gid_a, true) == 0,
             "the manager could not claim its GID");
    gm_check(ernic_mesh_claim_gid(b.dev->pvrdma_handle, gid_b, true) == 0,
             "the worker could not claim its GID");

    /* 3. Each resolves the other's GID, and to the other's node. */
    seen_b = gm_wait_resolve(&a, &b, &a, gid_b);
    seen_a = gm_wait_resolve(&a, &b, &b, gid_a);

    gm_check(seen_b == node_b,
             "the manager resolved the worker's GID to node %u, want %u",
             seen_b, node_b);
    gm_check(seen_a == node_a,
             "the worker resolved the manager's GID to node %u, want %u",
             seen_a, node_a);
    /* A node must not claim its peer's address as its own. */
    gm_check(ernic_mesh_node_from_gid(a.dev->pvrdma_handle, gid_a) == node_a,
             "the manager does not own the GID it claimed");

    /*
     * 4/5. A CM-shaped MAD on QP1, addressed to the peer's GID, must arrive
     * on the PEER's QP1 and not come back round to the sender's. The
     * destination QPN in the WQE is 1 either way, so an unresolved peer
     * delivers it to the sender's own GSI QP -- silently, with no log.
     */
    struct gm_ep ea, eb;

    memset(&ea, 0, sizeof(ea));
    memset(&eb, 0, sizeof(eb));
    if (!gm_check(gm_ep_open(&a, &ea) && gm_ep_open(&b, &eb),
                  "GSI endpoint setup failed"))
        goto out_ep;
    if (!gm_check(ih_ah_create(&a, GM_AH, GM_IP_A, GM_IP_B) == 0,
                  "CREATE_AH for the peer failed"))
        goto out_ep;

    uint8_t mad[GM_MAD_LEN];

    memset(mad, 0, sizeof(mad));
    mad[0] = 0x01;  /* base version */
    mad[1] = 0x07;  /* mgmt class: CM */
    mad[2] = 0x02;  /* class version */
    mad[3] = 0x03;  /* method: SEND */
    mad[16] = 0x00; /* attribute id 0x0010: ConnectRequest */
    mad[17] = 0x10;

    rx = (struct ih_sge){.va = eb.mr.va + GM_RX_OFF,
                         .len = IH_GRH_SIZE + GM_MAD_LEN,
                         .lkey = eb.mr.lkey};
    /*
     * The sender posts a receive too, on its own QP1. Without one a MAD
     * that came back round would be dropped for want of a buffer and the
     * loopback check below could not tell that from a MAD that was
     * correctly forwarded -- both leave the sender's CQ empty.
     */
    rx_self = (struct ih_sge){.va = ea.mr.va + GM_RX_OFF,
                              .len = IH_GRH_SIZE + GM_MAD_LEN,
                              .lkey = ea.mr.lkey};
    tx = (struct ih_sge){
        .va = ea.mr.va + GM_TX_OFF, .len = GM_MAD_LEN, .lkey = ea.mr.lkey};
    s = (struct ih_send){.op = IH_OP_SEND,
                         .flags = IH_WQE_SIGNALLED,
                         .ah_id = GM_AH,
                         .dest_qpn = 1,
                         .qkey = GM_QKEY,
                         .sge = &tx,
                         .nsge = 1};

    ih_mr_write(&a, &ea.mr, tx.va, mad, sizeof(mad));
    ih_post_recv(&b, &eb.qp, &rx, 1);
    ih_post_recv(&a, &ea.qp, &rx_self, 1);
    ih_post_send(&a, &ea.qp, &s);

    memset(&ca, 0, sizeof(ca));
    memset(&cb, 0, sizeof(cb));

    for (unsigned i = 0; i < 250 && cb.recvs == 0; i++) {
        gm_pump(&a, &b, 1);
        gm_drain(&a, &ea, &ca);
        gm_drain(&b, &eb, &cb);
    }
    /* A few more rounds, so a copy that also came back round is counted. */
    gm_pump(&a, &b, 10);
    gm_drain(&a, &ea, &ca);
    gm_drain(&b, &eb, &cb);

    gm_check(ca.sends == 1, "the sender got %u send completions, want 1",
             ca.sends);
    gm_check(cb.recvs == 1, "the peer's QP1 got %u MADs, want 1", cb.recvs);
    gm_check(ca.recvs == 0,
             "the sender's own QP1 got %u MADs back: the MAD was looped back "
             "instead of forwarded",
             ca.recvs);

    /*
     * 6/7. What the MAD arrived behind, which is the half of this that
     * rdma_cm actually turns on. Both nodes' GIDs are v4-mapped, so a
     * forwarded datagram has to look like RoCEv2 over IPv4: the IPv4 header
     * in the last 20 bytes of the 40-byte field, the completion flagged to
     * say so, and the SENDER's address as its source.
     *
     * The source is what was wrong. The device flagged every datagram IPv4
     * while writing an IPv6 GRH, so a receiver read bytes 8..11 of the
     * destination GID as the source address -- for ::ffff:a.b.c.d that is
     * 0.0.255.255, plausible enough to ARP for and impossible to resolve.
     * ib_cm resolves that address to build the reply's address handle, so
     * cm_req_handler() blocked for a second, returned -ETIMEDOUT and dropped
     * the REQ without even a reject; rdma_connect() on the far side retried
     * forever with nothing logged at either end. Every check above passed
     * throughout: the MAD was forwarded correctly and still went nowhere.
     */
    if (cb.recvs == 1) {
        uint8_t rx_buf[IH_GRH_SIZE + GM_MAD_LEN];

        ih_mr_read(&b, &eb.mr, rx.va, rx_buf, sizeof(rx_buf));

        const uint8_t *ip = rx_buf + IH_GRH_SIZE - 20;

        gm_check(cb.recv_ipv4, "the forwarded MAD is not flagged IPv4");
        gm_check(ip[0] == 0x45, "the forwarded MAD carries no IPv4 header");
        gm_check(ernic_get_be32(ip + 12) == GM_IP_A,
                 "the forwarded MAD's source address is %u.%u.%u.%u, want the "
                 "sender's 192.168.250.1",
                 ip[12], ip[13], ip[14], ip[15]);
        gm_check(ernic_get_be32(ip + 16) == GM_IP_B,
                 "the forwarded MAD's destination address is %u.%u.%u.%u, want "
                 "the peer's 192.168.250.2",
                 ip[16], ip[17], ip[18], ip[19]);
        gm_check(memcmp(rx_buf + IH_GRH_SIZE, mad, sizeof(mad)) == 0,
                 "the MAD is not behind the 40-byte header");
    }

    /*
     * 8. The worker restarts, and its address has to come back with it.
     *
     * It returns as a brand new node -- it asks for an id of 0xFFFFFFFF
     * every time, and its listening port is ephemeral, so nothing in the
     * registration says which node it used to be. Its old registration is
     * still standing and still claiming the guest's GIDs, and a GID that
     * two nodes claim resolves to neither. So without the manager
     * withdrawing what an unreachable node claimed, the address stops
     * working at the moment the guest comes back, and never recovers:
     * nothing else retires a claim.
     */
    ih_mr_free(&eb.mr);
    ih_mr_free(&ea.mr);
    ih_destroy(&b);

    /*
     * The manager has to have seen the old worker hang up before the new one
     * registers.  Registration declares dead only the nodes whose connection
     * has ended; until the manager's receive thread reads the end of node_b's,
     * node_b is alive, as far as the manager can tell, and keeps its GID until
     * the health check's next pass.  Nothing orders that thread before the
     * new worker's registration, and on a loaded machine it can run after.
     */
    if (!gm_check(gm_wait_hung_up(&a, node_b),
                  "the manager never saw node %u hang up", node_b))
        goto out_a;

    struct ih b2;

    if (!gm_check(ih_create(&b2, wrk), "the worker could not be recreated"))
        goto out_a;
    if (!gm_check(ih_bringup(&b2), "the restarted worker did not come up")) {
        ih_destroy(&b2);
        goto out_a;
    }

    node_b2 = UINT32_MAX;

    for (unsigned i = 0; i < 400; i++) {
        node_b2 = ernic_mesh_local_node(b2.dev->pvrdma_handle);
        if (node_b2 != UINT32_MAX && node_b2 != 0)
            break;
        gm_pump(&a, &b2, 1);
    }
    gm_check(node_b2 != UINT32_MAX && node_b2 != 0,
             "the restarted worker never registered (node id %u)", node_b2);
    gm_check(node_b2 != node_b,
             "the restarted worker reused node %u, so this proves nothing",
             node_b2);

    gm_check(ernic_mesh_claim_gid(b2.dev->pvrdma_handle, gid_b, true) == 0,
             "the restarted worker could not claim its GID");

    seen_b2 = gm_wait_resolve(&a, &b2, &a, gid_b);

    gm_check(seen_b2 == node_b2,
             "after the restart the worker's GID resolves to node %u, want "
             "%u -- %s",
             seen_b2, node_b2,
             seen_b2 == UINT32_MAX
                 ? "the dead node's claim was never withdrawn, so the GID is "
                   "contested and routes nowhere"
                 : "it resolves to the wrong node");

    ih_destroy(&b2);
    goto out_a;

out_ep:
    ih_mr_free(&eb.mr);
    ih_mr_free(&ea.mr);

out:
    ih_destroy(&b);
out_a:
    ih_destroy(&a);
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        puts("PASS: a GSI MAD addressed to the peer reaches the peer's QP1");
    return failures ? 1 : 0;
}
