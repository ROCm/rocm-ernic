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

#include "ionic_harness.h"
#include "rocm_ernic_compat.h"

/* The mesh listens on loopback. Overridable so a CI runner that already has
 * something here can move it rather than fail. */
#define GM_PORT_DEFAULT 18640

/* Two addresses on a subnet nothing else in the tree uses, so a stray claim
 * from another test cannot make these contested. */
#define GM_IP_A 0xc0a8fa01u /* 192.168.250.1 */
#define GM_IP_B 0xc0a8fa02u /* 192.168.250.2 */



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
 * Wait for @h to resolve @gid to some node, up to ~5s. Registration and the
 * advertisement that follows it are asynchronous, so a bare check races.
 */
static uint32_t gm_wait_resolve(struct ih *a, struct ih *b, struct ih *h,
                                const uint8_t gid[16])
{
    for (unsigned i = 0; i < 250; i++) {
        uint32_t node = ionic_mesh_node_from_gid(h->dev->pvrdma_handle, gid);

        if (node != UINT32_MAX)
            return node;
        gm_pump(a, b, 1);
    }
    return UINT32_MAX;
}

int main(void)
{
    const char *env = getenv("ERNIC_TEST_MESH_PORT");
    unsigned port = env ? (unsigned)strtoul(env, NULL, 10) : GM_PORT_DEFAULT;
    char mgr[64], wrk[64];

    snprintf(mgr, sizeof(mgr), "tcp:manager:listen:%u", port);
    snprintf(wrk, sizeof(wrk), "tcp:worker:127.0.0.1:%u", port);

    struct ih a, b;

    if (!ih_create(&a, mgr)) {
        fprintf(stderr, "SKIP: no mesh manager on port %u\n", port);
        return 77;
    }
    if (!ih_create(&b, wrk)) {
        ih_destroy(&a);
        fprintf(stderr, "SKIP: no mesh worker\n");
        return 77;
    }
    if (!gm_check(ih_bringup(&a) && ih_bringup(&b), "device bring-up failed"))
        goto out;

    /* 1. The mesh forms. The manager is always node 0 (tcp_init()); the
     * worker is assigned a non-zero id by REGISTER_RESP. */
    uint32_t node_a = UINT32_MAX, node_b = UINT32_MAX;

    for (unsigned i = 0; i < 250; i++) {
        node_a = ionic_mesh_local_node(a.dev->pvrdma_handle);
        node_b = ionic_mesh_local_node(b.dev->pvrdma_handle);
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
    gm_check(ionic_mesh_claim_gid(a.dev->pvrdma_handle, gid_a, true) == 0,
             "the manager could not claim its GID");
    gm_check(ionic_mesh_claim_gid(b.dev->pvrdma_handle, gid_b, true) == 0,
             "the worker could not claim its GID");

    /* 3. Each resolves the other's GID, and to the other's node. */
    uint32_t seen_b = gm_wait_resolve(&a, &b, &a, gid_b);
    uint32_t seen_a = gm_wait_resolve(&a, &b, &b, gid_a);

    gm_check(seen_b == node_b,
             "the manager resolved the worker's GID to node %u, want %u",
             seen_b, node_b);
    gm_check(seen_a == node_a,
             "the worker resolved the manager's GID to node %u, want %u",
             seen_a, node_a);
    /* A node must not claim its peer's address as its own. */
    gm_check(ionic_mesh_node_from_gid(a.dev->pvrdma_handle, gid_a) == node_a,
             "the manager does not own the GID it claimed");

out:
    ih_destroy(&b);
    ih_destroy(&a);
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        puts("PASS: the mesh resolves a GID claimed after it formed");
    return failures ? 1 : 0;
}
