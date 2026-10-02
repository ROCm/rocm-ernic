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
 * The static functions are reached by #including the translation unit;
 * resolution needs only the GID table, so the rest of the backend's
 * externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rocm-ernic-warnings.h"

/* Pull in the code under test (including its static functions) */
#include "rdma/rdma_backend_tcp.c"

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
int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    (void)thread;
    (void)name;
    (void)start_routine;
    (void)arg;
    (void)mode;
    return 0;
}
void qemu_thread_exit(void *retval)
{
    (void)retval;
    abort();
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)dir;
    return NULL;
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;
}
int pci_dma_sync(PCIDevice *dev, uint64_t guest_addr, uint64_t len)
{
    (void)dev;
    (void)guest_addr;
    (void)len;
    return 0;
}
void rdma_backend_complete_work(enum ibv_wc_status status, uint32_t vendor_err,
                                uint32_t byte_len, uint32_t qp_num,
                                enum ibv_wc_opcode opcode, void *ctx)
{
    (void)status;
    (void)vendor_err;
    (void)byte_len;
    (void)qp_num;
    (void)opcode;
    (void)ctx;
}
RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    (void)dev_res;
    (void)mr_handle;
    return NULL;
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

    priv->gid_nodes = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            tcp_gid_key_free, NULL);
    qemu_mutex_init(&priv->gid_table_lock);
    return priv;
}

static void priv_free(TcpBackendPrivate *priv)
{
    g_hash_table_destroy(priv->gid_nodes);
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
        tcp_gid_table_set(priv, i, (const uint8_t(*)[16]) & gids[i], 1);
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
    tcp_gid_table_set(priv, 5, (const uint8_t(*)[16]) & a, 1);
    tcp_gid_table_set(priv, 9, (const uint8_t(*)[16]) & b, 1);

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
    tcp_gid_table_set(priv, 0, (const uint8_t(*)[16]) & known, 1);

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
    tcp_gid_table_set(priv, 4, (const uint8_t(*)[16])pair, 2);

    CHECK(resolve(priv, pair[0]) == 4, "IPv4 GID of node 4 did not resolve");
    CHECK(resolve(priv, pair[1]) == 4, "link-local of node 4 did not resolve");

    gid_v4(replacement[0], 77);
    tcp_gid_table_set(priv, 4, (const uint8_t(*)[16])replacement, 1);

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

    unsetenv("ERNIC_TCP_GUEST_GIDS");
    CHECK(tcp_parse_guest_gids(out) == 0, "unset env yielded GIDs");

    setenv("ERNIC_TCP_GUEST_GIDS", "192.168.100.11, fe80::3 ,bogus", 1);
    uint32_t n = tcp_parse_guest_gids(out);
    CHECK(n == 2, "expected 2 parsed GIDs, got %u", n);

    gid_v4(expect, 11);
    CHECK(memcmp(out[0], expect, 16) == 0,
          "IPv4 entry was not stored in ::ffff:a.b.c.d form");

    memset(expect, 0, 16);
    expect[0] = 0xfe;
    expect[1] = 0x80;
    expect[15] = 0x03;
    CHECK(memcmp(out[1], expect, 16) == 0, "IPv6 entry was not stored raw");

    unsetenv("ERNIC_TCP_GUEST_GIDS");
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

int main(void)
{
    printf("tcp GID resolution unit tests\n");

    test_three_nodes_resolve_distinctly();
    test_link_local_resolves();
    test_unknown_gid_does_not_resolve();
    test_multiple_and_replacement();
    test_env_parsing();
    test_advertisement_tail_is_zeroed();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
