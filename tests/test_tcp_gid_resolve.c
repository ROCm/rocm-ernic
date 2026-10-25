/*
 *
 * Unit tests for GID -> node resolution in rdma_backend_tcp.c.
 *
 * Regression coverage for silent misrouting above two nodes: the resolver
 * used to derive a node id from one octet of the destination GID, and its
 * callers substituted a hardcoded node when that failed. Every peer then
 * resolved to the same node, so RDMA for nodes 2+ was delivered to the
 * wrong guest with no error anywhere.
 *
 * The replacement matches all 16 bytes against what each node advertised
 * about itself, and returns UINT32_MAX when nothing owns the GID so the
 * caller can fail. These cases pin both halves: distinct GIDs must resolve
 * distinctly, and an unknown GID must not resolve at all.
 *
 * Resolution is one of the entry points the ionic data path calls through
 * the device whatever its backend, so the last cases check that all of them
 * refuse a backend that isn't TCP.
 *
 * The test links rdma_backend_tcp.c and reaches its helpers through
 * rdma_backend_tcp_priv.h; resolution needs only the GID table, so the rest
 * of the backend's externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <infiniband/verbs.h>

#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_ops.h"
#include "rdma/rdma_backend_tcp_priv.h"
#include "rdma_backend.h"
#include "rdma_backend_defs.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 * None of these are reachable from the resolution path; they exist only to
 * satisfy the linker
 */
void error_report(const char *fmt, ...)
{
    (void)fmt;
}
void warn_report(const char *fmt, ...)
{
    (void)fmt;
}
void info_report(const char *fmt, ...)
{
    (void)fmt;
}
int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg)
{
    (void)thread;
    (void)start_routine;
    (void)arg;
    return 0;
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

#define CHECK(cond, ...)          \
    do {                          \
        if (!(cond)) {            \
            printf("    FAIL: "); \
            printf(__VA_ARGS__);  \
            printf("\n");         \
            failures++;           \
        }                         \
    } while (0)

static TcpBackendPrivate *priv_new(void)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);

    priv->node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->gid_owner = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            tcp_gid_key_free, NULL);
    qemu_mutex_init(&priv->gid_table_lock);
    return priv;
}

static void priv_free(TcpBackendPrivate *priv)
{
    g_hash_table_destroy(priv->node_gids);
    g_hash_table_destroy(priv->gid_owner);
    qemu_mutex_destroy(&priv->gid_table_lock);
    g_free(priv);
}

/* An IPv4-mapped GID, the form adminq_parse_roce_hdr() builds for a
 * RoCE-over-IPv4 header. */
static void gid_v4(uint8_t out[16], uint8_t last)
{
    memset(out, 0, 16);
    out[10] = 0xff;
    out[11] = 0xff;
    out[12] = 192;
    out[13] = 168;
    out[14] = 100;
    out[15] = last;
}

/* An IPv6 link-local GID, which is what the ionic driver actually reports
 * and what the original heuristic rejected outright. */
static void gid_ll(uint8_t out[16], uint8_t last)
{
    memset(out, 0, 16);
    out[0] = 0xfe;
    out[1] = 0x80;
    out[11] = 0xff;
    out[12] = 0xfe;
    out[15] = last;
}

static uint32_t resolve(TcpBackendPrivate *priv, const uint8_t raw[16])
{
    union ibv_gid gid;

    memcpy(gid.raw, raw, 16);
    return tcp_resolve_node_from_gid(priv, &gid);
}

/* ---- Cases ------------------------------------------------------------- */

/*
 * The actual regression: three nodes, three distinct GIDs, three distinct
 * answers. Under the old octet heuristic all three collapsed onto one node
 * because none of them matched the <subnet>.<(node+1)*10> convention.
 */
static void test_three_nodes_resolve_distinctly(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t gids[3][16];

    printf("  three nodes resolve distinctly\n");

    for (uint32_t i = 0; i < 3; i++) {
        gid_v4(gids[i], (uint8_t)(11 + i));
        tcp_gid_claims_set(priv, i, (const uint8_t(*)[16]) & gids[i], 1);
    }

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t got = resolve(priv, gids[i]);
        CHECK(got == i, "GID ending .%u resolved to node %u, expected %u",
              11 + i, got, i);
    }

    priv_free(priv);
}

/* Link-local GIDs are the case seen in the field; they must resolve by
 * exact match like any other. */
static void test_link_local_resolves(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t a[16], b[16];

    printf("  link-local GIDs resolve by exact match\n");

    gid_ll(a, 0x03);
    gid_ll(b, 0x07);
    tcp_gid_claims_set(priv, 5, (const uint8_t(*)[16]) & a, 1);
    tcp_gid_claims_set(priv, 9, (const uint8_t(*)[16]) & b, 1);

    CHECK(resolve(priv, a) == 5, "first link-local resolved to %u, expected 5",
          resolve(priv, a));
    CHECK(resolve(priv, b) == 9, "second link-local resolved to %u, expected 9",
          resolve(priv, b));

    priv_free(priv);
}

/* An unknown GID must not resolve. Returning any node here is the bug. */
static void test_unknown_gid_does_not_resolve(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t known[16], unknown[16];

    printf("  unknown GID does not resolve\n");

    gid_v4(known, 11);
    gid_v4(unknown, 12);
    tcp_gid_claims_set(priv, 0, (const uint8_t(*)[16]) & known, 1);

    CHECK(resolve(priv, unknown) == UINT32_MAX,
          "unknown GID resolved to node %u, expected no match",
          resolve(priv, unknown));
    CHECK(tcp_resolve_node_from_gid(priv, NULL) == UINT32_MAX,
          "NULL GID resolved to a node");
    CHECK(tcp_resolve_node_from_gid(NULL, NULL) == UINT32_MAX,
          "NULL priv resolved to a node");

    priv_free(priv);
}

/* A node may own several addresses, and re-advertising replaces the
 * previous set rather than accumulating stale entries. */
static void test_multiple_and_replacement(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t pair[2][16], replacement[1][16];

    printf("  multiple GIDs per node, replaced on re-advertisement\n");

    gid_v4(pair[0], 11);
    gid_ll(pair[1], 0x11);
    tcp_gid_claims_set(priv, 4, (const uint8_t(*)[16])pair, 2);

    CHECK(resolve(priv, pair[0]) == 4, "IPv4 GID of node 4 did not resolve");
    CHECK(resolve(priv, pair[1]) == 4, "link-local of node 4 did not resolve");

    gid_v4(replacement[0], 77);
    tcp_gid_claims_set(priv, 4, (const uint8_t(*)[16])replacement, 1);

    CHECK(resolve(priv, replacement[0]) == 4,
          "replacement GID did not resolve");
    CHECK(resolve(priv, pair[0]) == UINT32_MAX,
          "superseded GID still resolves to node 4");
    CHECK(resolve(priv, pair[1]) == UINT32_MAX,
          "superseded link-local still resolves to node 4");

    priv_free(priv);
}

/* ERNIC_TCP_GUEST_GIDS parsing, including the IPv4 -> ::ffff:a.b.c.d
 * mapping that has to match what arrives on the wire. */
static void test_env_parsing(void)
{
    uint8_t out[TCP_MAX_NODE_GIDS][16];
    uint8_t expect[16];

    printf("  ERNIC_TCP_GUEST_GIDS parsing\n");

    CHECK(tcp_parse_guest_gids(NULL, out) == 0, "unset env yielded GIDs");
    CHECK(tcp_parse_guest_gids("", out) == 0, "empty env yielded GIDs");

    uint32_t n = tcp_parse_guest_gids("192.168.100.11, fe80::3 ,bogus", out);
    CHECK(n == 2, "expected 2 parsed GIDs, got %u", n);

    gid_v4(expect, 11);
    CHECK(memcmp(out[0], expect, 16) == 0,
          "IPv4 entry was not stored in ::ffff:a.b.c.d form");

    memset(expect, 0, 16);
    expect[0] = 0xfe;
    expect[1] = 0x80;
    expect[15] = 0x03;
    CHECK(memcmp(out[1], expect, 16) == 0, "IPv6 entry was not stored raw");
}

/* The advertisement goes on the wire whole, so the slots past num_gids must
 * be zero rather than whatever the stack held -- the same disclosure trap
 * test_tcp_mesh_topology.c guards for the topology payload. */
static void test_advertisement_tail_is_zeroed(void)
{
    TcpNodeGidsPayload payload;
    uint8_t gids[1][16];

    printf("  advertisement zero-fills unused GID slots\n");

    memset(&payload, 0xA5, sizeof(payload));
    gid_v4(gids[0], 11);
    tcp_fill_node_gids(&payload, 3, (const uint8_t(*)[16])gids, 1);

    CHECK(ntohl(payload.node_id) == 3, "node_id not set");
    CHECK(ntohl(payload.num_gids) == 1, "num_gids not set");

    for (size_t i = 1; i < TCP_MAX_NODE_GIDS; i++) {
        for (size_t j = 0; j < 16; j++) {
            CHECK(payload.gids[i][j] == 0,
                  "poison survived at unused slot %zu byte %zu", i, j);
        }
    }
}

/*
 * The log formatter must render all 16 bytes and stay inside its buffer.
 * It is written out by hand to dodge -Wformat-truncation=2, so the length
 * is maintained rather than computed -- which is exactly the kind of bound
 * worth pinning.
 */
static void test_gid_formatting(void)
{
    char buf[TCP_GID_STR_LEN];
    uint8_t g[16];

    printf("  GID log formatting\n");

    /* The exact GID a fleet guest with MAC 72:6f:63:6d:00:03 autoconfigures. */
    gid_ll(g, 0x03);
    g[8] = 0x70;
    g[9] = 0x6f;
    g[10] = 0x63;
    g[13] = 0x6d;
    CHECK(strcmp(tcp_gid_str(g, buf),
                 "fe80:0000:0000:0000:706f:63ff:fe6d:0003") == 0,
          "link-local rendered as '%s'", buf);

    /* Every byte 0xff: the longest output the formatter can produce. */
    memset(g, 0xff, sizeof(g));
    const char *all = tcp_gid_str(g, buf);
    CHECK(strlen(all) == TCP_GID_STR_LEN - 1,
          "widest GID rendered %zu chars, buffer holds %d incl. NUL",
          strlen(all), TCP_GID_STR_LEN);
    CHECK(strcmp(all, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff") == 0,
          "widest GID rendered as '%s'", all);

    /* All zeroes must still render every group, not an empty string. */
    memset(g, 0, sizeof(g));
    CHECK(strcmp(tcp_gid_str(g, buf),
                 "0000:0000:0000:0000:0000:0000:0000:0000") == 0,
          "zero GID rendered as '%s'", buf);
}

/*
 * An unreliable-datagram QP must reach RTR without a destination GID.
 *
 * UD names its peer per work request, so the INIT->RTR transition carries
 * a zero GID. Requiring one there rejected every ordinary UD QP with -EIO
 * once the resolver stopped guessing. A connected QP must still name a GID
 * some node owns.
 */
static void test_ud_rtr_needs_no_destination(void)
{
    TcpBackendPrivate *priv = priv_new();
    RdmaBackendDev dev;
    RdmaBackendQP bqp;
    union ibv_gid zero, known;
    uint8_t raw[16];
    int rc;

    printf("  UD reaches RTR without a destination GID\n");

    memset(&dev, 0, sizeof(dev));
    memset(&bqp, 0, sizeof(bqp));
    dev.backend_private = priv;
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    bqp.ibqp = (struct ibv_qp *)(uintptr_t)7u;

    gid_v4(raw, 11);
    tcp_gid_claims_set(priv, 2, (const uint8_t(*)[16]) & raw, 1);
    memset(&zero, 0, sizeof(zero));
    memcpy(known.raw, raw, 16);

    /* UD with no destination: accepted. */
    rc = rdma_backend_ops_tcp.qp_state_rtr(&dev, &bqp, IBV_QPT_UD, 0, &zero, 99,
                                           0, 0, false);
    CHECK(rc == 0, "UD RTR rejected with rc=%d", rc);

    /* A connected QP still must resolve, and still must fail when it cannot. */
    rc = rdma_backend_ops_tcp.qp_state_rtr(&dev, &bqp, IBV_QPT_RC, 0, &zero, 99,
                                           0, 0, false);
    CHECK(rc != 0, "RC RTR accepted an unresolvable GID");

    rc = rdma_backend_ops_tcp.qp_state_rtr(&dev, &bqp, IBV_QPT_RC, 0, &known,
                                           99, 0, 0, false);
    CHECK(rc == 0, "RC RTR rejected a resolvable GID (rc=%d)", rc);

    priv_free(priv);
}

/*
 * A contested GID must stay contested.
 *
 * The first attempt at this dropped the entry on conflict, which lost the
 * fact that it was contested: the next advertisement from either claimant
 * found an empty slot and took ownership, restoring arrival-order routing
 * by a longer path. Claims are now kept per node and the owner derived, so
 * a repeat, a third claimant, and a withdrawal all behave.
 */
static void test_contested_gid_stays_contested(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t shared[1][16], own_a[1][16];

    printf("  a contested GID stays contested\n");

    gid_v4(shared[0], 50);
    gid_v4(own_a[0], 51);

    /* Node 1 alone: resolvable. */
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])shared, 1);
    CHECK(resolve(priv, shared[0]) == 1, "sole claimant did not resolve");

    /* Node 2 claims it too: neither should resolve. */
    tcp_gid_claims_set(priv, 2, (const uint8_t(*)[16])shared, 1);
    CHECK(resolve(priv, shared[0]) == UINT32_MAX,
          "contested GID resolved to node %u", resolve(priv, shared[0]));

    /* Node 1 re-advertises: must not win it back. */
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])shared, 1);
    CHECK(resolve(priv, shared[0]) == UINT32_MAX,
          "re-advertisement took a contested GID (node %u)",
          resolve(priv, shared[0]));

    /* A third claimant changes nothing. */
    tcp_gid_claims_set(priv, 3, (const uint8_t(*)[16])shared, 1);
    CHECK(resolve(priv, shared[0]) == UINT32_MAX,
          "third claimant took a contested GID (node %u)",
          resolve(priv, shared[0]));

    /* Withdrawals leave exactly one claimant, which resolves again. */
    tcp_gid_claims_set(priv, 2, NULL, 0);
    tcp_gid_claims_set(priv, 3, NULL, 0);
    CHECK(resolve(priv, shared[0]) == 1,
          "GID did not recover once only node 1 claimed it (got %u)",
          resolve(priv, shared[0]));

    /* A conflict must not cost a node its uncontested addresses. */
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])own_a, 1);
    tcp_gid_claims_set(priv, 2, (const uint8_t(*)[16])shared, 1);
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])shared, 1);
    CHECK(resolve(priv, shared[0]) == UINT32_MAX, "contest not detected");

    priv_free(priv);
}

/*
 * A worker that restarts comes back as a new node, and the manager has to
 * recognise it and withdraw what it claimed before.
 *
 * It asks for an id of 0xFFFFFFFF every time, so there is nothing in the
 * registration that says "I was node 1", and its listening port is
 * ephemeral so the address it registers from is no help either. What marks
 * the old node is that it can no longer be reached. Without the withdrawal
 * both ids claim the guest's
 * GIDs, a contested GID resolves to neither, and the guest becomes
 * unreachable at the moment it came back -- which is worse than it having
 * stayed away, and does not heal, because nothing retires a claim.
 */
static void test_returning_worker_reclaims_its_gids(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t g[1][16];

    printf("  a restarted worker's GIDs follow it to its new node id\n");

    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_mesh_node_info_destroy_notify);
    qemu_mutex_init(&priv->mesh_table_lock);

    /* The worker's first registration, and the GIDs it then advertised. */
    g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(1u),
                        tcp_mesh_node_info_new(1, "10.0.0.5", 6320));

    gid_v4(g[0], 70);
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == 1, "the first registration did not resolve");

    /* It restarts. Node 1's connection is gone, so its claims are
     * withdrawn -- which is what the manager does for every unreachable
     * node when any worker registers, and what the health check does when
     * it notices one has died. */
    tcp_gid_claims_set(priv, 1, NULL, 0);
    g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(2u),
                        tcp_mesh_node_info_new(2, "10.0.0.5", 6320));

    /* It advertises the same GIDs again under the new id. */
    tcp_gid_claims_set(priv, 2, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == 2,
          "the GID did not follow the worker to node 2 (got %u)",
          resolve(priv, g[0]));

    /* Without the withdrawal this is what would have happened instead. */
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == UINT32_MAX,
          "a stale claim left alongside the new one still resolved");

    g_hash_table_destroy(priv->mesh_nodes);
    qemu_mutex_destroy(&priv->mesh_table_lock);
    priv->mesh_nodes = NULL;
    priv_free(priv);
}

/* An empty advertisement withdraws a node's claims -- how the manager
 * revokes, and the case a broadcast of surviving entries could not express. */
static void test_empty_advertisement_withdraws(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t g[1][16];

    printf("  an empty advertisement withdraws a node's GIDs\n");

    gid_v4(g[0], 60);
    tcp_gid_claims_set(priv, 4, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == 4, "claim did not take effect");

    tcp_gid_claims_set(priv, 4, NULL, 0);
    CHECK(resolve(priv, g[0]) == UINT32_MAX,
          "withdrawn GID still resolves to node %u", resolve(priv, g[0]));

    priv_free(priv);
}

/*
 * A node the manager has declared dead keeps its claims but stops owning
 * them, and takes them up again when it answers.
 *
 * This is what makes a restart survivable. A restarted node registers
 * under a new id -- ids are allocated monotonically and a dead node stays
 * until a full mesh needs its slot -- so without this its guest's GID is
 * claimed by the id it had and the id it now has, the duplicate rule
 * resolves it to neither, and the address stays unreachable for the rest
 * of the mesh's life, or until a full mesh evicts the old id.
 */
static void test_dead_node_stops_owning_its_gids(void)
{
    TcpBackendPrivate *priv = priv_new();
    uint8_t g[1][16];

    printf("  a dead node stops owning its GIDs\n");

    gid_v4(g[0], 70);
    tcp_gid_claims_set(priv, 5, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == 5, "claim did not take effect");

    tcp_gid_claims_set_alive(priv, 5, false);
    CHECK(resolve(priv, g[0]) == UINT32_MAX,
          "a dead node still owns its GID (node %u)", resolve(priv, g[0]));

    /* The claims are kept, so answering again is enough to get them back:
     * a node that is merely late never re-advertises. */
    tcp_gid_claims_set_alive(priv, 5, true);
    CHECK(resolve(priv, g[0]) == 5,
          "a revived node did not get its GID back (got %u)",
          resolve(priv, g[0]));

    /* The same guest returning under a new id owns the address, rather
     * than deadlocking against the id it used to have. */
    tcp_gid_claims_set_alive(priv, 5, false);
    tcp_gid_claims_set(priv, 6, (const uint8_t(*)[16])g, 1);
    CHECK(resolve(priv, g[0]) == 6,
          "a restarted node did not take its own GID (got %u)",
          resolve(priv, g[0]));

    /* Two live claimants are still a contest; death is not a way to win
     * one. */
    tcp_gid_claims_set_alive(priv, 5, true);
    CHECK(resolve(priv, g[0]) == UINT32_MAX,
          "two live claimants resolved to node %u", resolve(priv, g[0]));

    /* Marking a node with no claims live or dead does nothing. */
    tcp_gid_claims_set_alive(priv, 99, false);
    tcp_gid_claims_set_alive(priv, 99, true);
    CHECK(resolve(priv, g[0]) == UINT32_MAX,
          "a node with no claims changed the table");

    priv_free(priv);
}

/* ---- Entry points on every backend ------------------------------------- */

/*
 * The ionic data path calls these through the device whatever its backend,
 * and each reads backend_private as a TcpBackendPrivate. On any other
 * backend that block is the other backend's own state, so each must check
 * the backend type before touching it. Without the check, shutting down a
 * loopback server that a guest had used locked a mutex past the end of the
 * loopback state and wrote the receive callback after it.
 */
enum hook {
    HOOK_LOCAL_NODE,
    HOOK_NODE_FROM_GID,
    HOOK_SET_CB,
    HOOK_CLEAR_CB,
    HOOK_SEND,
    HOOK_HAS_ROOM,
    HOOK_CANCEL,
};

/* refused is what a hook returns when it leaves the block alone. The
 * callback and cancel rows return nothing, so only the poison check catches
 * them. */
struct hook_case {
    enum hook hook;
    const char *name;
    int64_t refused;
};

static const struct hook_case hook_cases[] = {
    {HOOK_LOCAL_NODE, "tcp_backend_local_node_id()", UINT32_MAX},
    {HOOK_NODE_FROM_GID, "tcp_backend_node_from_gid()", UINT32_MAX},
    {HOOK_SET_CB, "tcp_backend_set_ionic_recv_cb(fn)", 0},
    {HOOK_CLEAR_CB, "tcp_backend_set_ionic_recv_cb(NULL)", 0},
    {HOOK_SEND, "tcp_backend_send_ionic_v()", -EINVAL},
    {HOOK_HAS_ROOM, "tcp_backend_ionic_has_room()", true},
    {HOOK_CANCEL, "tcp_backend_ionic_cancel()", 0},
};

struct backend_case {
    RdmaBackendType type;
    const char *name;
};

static const struct backend_case other_backends[] = {
    {RDMA_BACKEND_TYPE_NONE, "none"},
    {RDMA_BACKEND_TYPE_LOOPBACK, "loopback"},
    {RDMA_BACKEND_TYPE_VERBS, "verbs"},
    {RDMA_BACKEND_TYPE_NVMEOF, "nvmeof"},
    {RDMA_BACKEND_TYPE_S3, "s3"},
};

#define MESH_PEER   2u   /* the node the hooks address and the GID's owner */
#define POISON_BYTE 0xA5 /* fills another backend's private block */

static void recv_fn(void *opaque, uint32_t src_node, const void *buf,
                    size_t len)
{
    (void)opaque;
    (void)src_node;
    (void)buf;
    (void)len;
}

/* The TCP backend keeps this pointer, so it has to outlive the call. */
static int recv_opaque;

/* Call @hook on @dev and return its result, or 0 for the rows that return
 * nothing. */
static int64_t call_hook(RdmaBackendDev *dev, enum hook hook)
{
    static const uint8_t hdr[8] = {1};
    union ibv_gid gid;

    switch (hook) {
    case HOOK_LOCAL_NODE:
        return tcp_backend_local_node_id(dev);
    case HOOK_NODE_FROM_GID:
        gid_v4(gid.raw, 11);
        return tcp_backend_node_from_gid(dev, &gid);
    case HOOK_SET_CB:
        tcp_backend_set_ionic_recv_cb(dev, recv_fn, &recv_opaque);
        return 0;
    case HOOK_CLEAR_CB:
        tcp_backend_set_ionic_recv_cb(dev, NULL, NULL);
        return 0;
    case HOOK_SEND:
        return tcp_backend_send_ionic_v(dev, MESH_PEER, hdr, sizeof(hdr), NULL,
                                        0, 0, false);
    case HOOK_HAS_ROOM:
        return tcp_backend_ionic_has_room(dev, MESH_PEER, sizeof(hdr));
    case HOOK_CANCEL:
        tcp_backend_ionic_cancel(dev, 0, UINT64_MAX);
        return 0;
    default:
        abort(); /* every row names one of the cases above */
    }
}

/*
 * The other backend's block is as large as a TcpBackendPrivate and full of
 * poison, so an entry point that skips the check reads poison and changes
 * bytes compared here. With a smaller block it would write past the end,
 * where only ASan might notice, and only if the write landed in a redzone.
 */
static void test_hooks_refuse_other_backends(void)
{
    const size_t n_backends =
        sizeof(other_backends) / sizeof(other_backends[0]);
    const size_t n_hooks = sizeof(hook_cases) / sizeof(hook_cases[0]);

    printf("  mesh entry points leave other backends' state alone\n");

    for (size_t b = 0; b < n_backends; b++) {
        for (size_t h = 0; h < n_hooks; h++) {
            const struct backend_case *bc = &other_backends[b];
            const struct hook_case *hc = &hook_cases[h];
            uint8_t *block = g_malloc(sizeof(TcpBackendPrivate));
            RdmaBackendDev dev;
            int64_t got;
            size_t i;

            memset(block, POISON_BYTE, sizeof(TcpBackendPrivate));
            memset(&dev, 0, sizeof(dev));
            dev.backend_type = bc->type;
            dev.backend_private = block;

            got = call_hook(&dev, hc->hook);
            CHECK(got == hc->refused,
                  "%s on the %s backend returned %" PRId64
                  ", expected %" PRId64,
                  hc->name, bc->name, got, hc->refused);

            for (i = 0; i < sizeof(TcpBackendPrivate); i++) {
                if (block[i] != POISON_BYTE) {
                    break;
                }
            }
            CHECK(i == sizeof(TcpBackendPrivate),
                  "%s changed byte %zu of the %s backend's state", hc->name, i,
                  bc->name);

            g_free(block);
        }
    }
}

/* No device, or a TCP device whose private block is gone, is refused too. */
static void test_hooks_refuse_missing_backend(void)
{
    const size_t n_hooks = sizeof(hook_cases) / sizeof(hook_cases[0]);
    RdmaBackendDev dev;
    int64_t got;

    printf("  mesh entry points refuse a missing backend\n");

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;

    for (size_t h = 0; h < n_hooks; h++) {
        const struct hook_case *hc = &hook_cases[h];

        got = call_hook(NULL, hc->hook);
        CHECK(got == hc->refused,
              "%s with no device returned %" PRId64 ", expected %" PRId64,
              hc->name, got, hc->refused);

        got = call_hook(&dev, hc->hook);
        CHECK(got == hc->refused,
              "%s with no private block returned %" PRId64
              ", expected %" PRId64,
              hc->name, got, hc->refused);
    }
}

/*
 * On the TCP backend every entry point gets past the type check. The send
 * fails for want of a connection, which only the code after the check can
 * report; a refused backend would get -EINVAL.
 */
static void test_hooks_on_tcp(void)
{
    static const uint8_t hdr[8] = {1};
    TcpBackendPrivate *priv = priv_new();
    RdmaBackendDev dev;
    union ibv_gid gid;
    uint8_t raw[16];
    uint32_t node;
    int rc;

    printf("  mesh entry points work on the TCP backend\n");

    memset(&dev, 0, sizeof(dev));
    dev.backend_private = priv;
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;

    qemu_mutex_init(&priv->lock);
    qemu_mutex_init(&priv->conn_table_lock);
    priv->connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    atomic_store(&priv->local_node_id, 1);
    gid_v4(raw, 11);
    tcp_gid_claims_set(priv, MESH_PEER, (const uint8_t(*)[16]) & raw, 1);
    memcpy(gid.raw, raw, 16);

    node = tcp_backend_local_node_id(&dev);
    CHECK(node == 1, "local node id is %u, expected 1", node);

    node = tcp_backend_node_from_gid(&dev, &gid);
    CHECK(node == MESH_PEER, "GID resolved to node %u, expected %u", node,
          MESH_PEER);

    tcp_backend_set_ionic_recv_cb(&dev, recv_fn, &recv_opaque);
    CHECK(priv->ionic_recv_fn == recv_fn &&
              priv->ionic_recv_opaque == &recv_opaque,
          "receive callback not stored");

    tcp_backend_set_ionic_recv_cb(&dev, NULL, NULL);
    CHECK(!priv->ionic_recv_fn && !priv->ionic_recv_opaque,
          "receive callback not cleared");

    rc = tcp_backend_send_ionic_v(&dev, MESH_PEER, hdr, sizeof(hdr), NULL, 0, 0,
                                  false);
    CHECK(rc == -ENOTCONN,
          "send to an unconnected node returned %d, expected -ENOTCONN", rc);

    g_hash_table_destroy(priv->connections);
    priv->connections = NULL;
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->lock);
    priv_free(priv);
}

int main(void)
{
    printf("tcp GID resolution unit tests\n");

    test_three_nodes_resolve_distinctly();
    test_link_local_resolves();
    test_unknown_gid_does_not_resolve();
    test_multiple_and_replacement();
    test_env_parsing();
    test_advertisement_tail_is_zeroed();
    test_gid_formatting();
    test_ud_rtr_needs_no_destination();
    test_contested_gid_stays_contested();
    test_returning_worker_reclaims_its_gids();
    test_empty_advertisement_withdraws();
    test_dead_node_stops_owning_its_gids();
    test_hooks_refuse_other_backends();
    test_hooks_refuse_missing_backend();
    test_hooks_on_tcp();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
