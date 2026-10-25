/*
 *
 * Unit tests for tcp_broadcast_mesh_topology() in rdma_backend_tcp.c.
 *
 * Regression coverage for a stack-memory disclosure: the manager sends the
 * full fixed-size TcpMeshTopologyPayload (~17 KB, TOPO_MAXNODE node slots) on
 * every broadcast, but only filled in nodes[0..num_nodes). The struct was an
 * uninitialised stack local, so the unused tail slots put whatever the
 * thread's stack happened to hold -- including live pointers -- on the wire.
 *
 * Each case runs the broadcast on a thread whose stack the test allocated
 * and pre-filled with a poison byte, then reads the message back off a
 * socketpair and asserts every byte past num_nodes is zero. Without the
 * zero-fill the poison shows up on the wire and the test fails.
 *
 * The test links rdma_backend_tcp.c and reaches the broadcasts through
 * rdma_backend_tcp_priv.h; they only need the mesh, connection, and GID hash
 * tables, so the rest of the backend's externals are stubbed out.
 *
 * Table-driven: empty mesh (every slot unused), single node, a few nodes, a
 * full mesh (no tail at all), and a mesh table past the cap.
 *
 * Both mesh broadcasts, the topology and the GID table, snapshot the mesh
 * table into TCP_MAX_MESH_NODES slots. The same cases check that each fills
 * every slot it can and, given a table past the cap, stops at the cap and
 * logs an error instead of leaving nodes out silently.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_tcp_priv.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 * Apart from error_report(), which counts calls so a case can check whether
 * a broadcast reported an overflow, none of these are reachable from the
 * broadcast paths; they exist only to satisfy the linker
 */

/* Read only once the broadcast has returned or its thread has been joined */
static int error_reports;

void error_report(const char *fmt, ...)
{
    (void)fmt;
    error_reports++;
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

/*
 * Derived from the payload rather than hardcoded: if nodes[] is ever resized,
 * a literal here would silently check a short tail instead of failing
 */
#define TOPO_MAXNODE \
    (sizeof(((TcpMeshTopologyPayload *)0)->nodes) / sizeof(TcpMeshNodeInfo))

struct testcase {
    const char *name;
    uint32_t num_nodes;
};

/* The most nodes any case puts in the mesh table: one past the cap */
#define MAX_CASE_NODES (TCP_MAX_MESH_NODES + 1)

/* How many of @num_nodes a broadcast can carry */
static uint32_t carried_nodes(uint32_t num_nodes)
{
    return num_nodes < TCP_MAX_MESH_NODES ? num_nodes : TCP_MAX_MESH_NODES;
}

/* A broadcast reports one error when it has to leave nodes out */
static int expected_errors(uint32_t num_nodes)
{
    return num_nodes > TCP_MAX_MESH_NODES ? 1 : 0;
}

/* Read exactly len bytes, or return -1. */
static int read_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t ret = recv(fd, (char *)buf + got, len - got, 0);
        if (ret <= 0) {
            return -1;
        }
        got += (size_t)ret;
    }
    return 0;
}

/*
 * Run tcp_broadcast_mesh_topology() on a thread whose stack we allocate and
 * pre-fill with a poison pattern.
 *
 * Poisoning the caller's own frame and hoping the payload lands on it is not
 * reliable -- the compiler is free to place the ~17 KB struct anywhere, and
 * ASan's redzones shift frames further. Owning the whole stack removes the
 * guesswork: wherever the payload lands, it lands on POISON_BYTE, so any
 * unused slot that reaches the wire un-zeroed is unambiguous.
 */
#define POISON_BYTE     0xAB
#define POISON_STACK_SZ (1024 * 1024)

/*
 * Address of a local in the broadcast thread's frame, recorded so the caller
 * can prove the payload really landed on the stack we poisoned.
 *
 * ASan's use-after-return detection relocates large frames off the real
 * stack into freshly-zeroed heap "fake stacks", which would hide the poison
 * and make this test vacuously pass: the ~17 KB frame's tail would read as
 * zero no matter what the code under test does. tests/CMakeLists.txt builds
 * this binary without that instrumentation. Checking where the frame landed
 * keeps a build that has it anyway from passing silently.
 */
static uintptr_t broadcast_frame;

static void *broadcast_thread(void *opaque)
{
    int frame_marker;

    broadcast_frame = (uintptr_t)&frame_marker;
    tcp_broadcast_mesh_topology((TcpBackendPrivate *)opaque);
    return NULL;
}

static int broadcast_on_poisoned_stack(TcpBackendPrivate *priv)
{
    pthread_attr_t attr;
    pthread_t tid;
    void *stack;
    int ret;
    long page_sz = sysconf(_SC_PAGESIZE);

    if (page_sz <= 0 ||
        posix_memalign(&stack, (size_t)page_sz, POISON_STACK_SZ) != 0) {
        return -1;
    }
    memset(stack, POISON_BYTE, POISON_STACK_SZ);

    /* Cleared per run so a value left by an earlier case can never stand in
     * for one this thread failed to record. pthread_join() below orders the
     * thread's write against our read.
     */
    broadcast_frame = 0;

    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, POISON_STACK_SZ);
    ret = pthread_create(&tid, &attr, broadcast_thread, priv);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        free(stack);
        return -1;
    }
    pthread_join(tid, NULL);

    /* Compared as uintptr_t: relational operators on pointers into different
     * objects are unspecified in C, and the frame is not part of `stack` as
     * far as the language is concerned. */
    uintptr_t base = (uintptr_t)stack;
    bool on_poisoned_stack =
        broadcast_frame >= base && broadcast_frame < base + POISON_STACK_SZ;

    free(stack);
    return on_poisoned_stack ? 0 : -1;
}

/* Populate the mesh table with `n` nodes whose fields encode their index */
static void fill_mesh(GHashTable *mesh_nodes, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        char hostname[64];
        snprintf(hostname, sizeof(hostname), "node-%u.example", i);
        MeshNodeInfo *node =
            tcp_mesh_node_info_new(i + 100, hostname, (uint16_t)(9000 + i));
        g_hash_table_insert(mesh_nodes, GUINT_TO_POINTER(i + 100), node);
    }
}

static void empty_mesh(GHashTable *mesh_nodes)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, mesh_nodes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        tcp_mesh_node_info_free((MeshNodeInfo *)value);
    }
    g_hash_table_remove_all(mesh_nodes);
}

/* A manager with one "worker" on the near end of a socketpair */
struct fixture {
    TcpBackendPrivate priv;
    TcpConnection conn;
    int sv[2];
};

static int fixture_init(struct fixture *f, uint32_t num_nodes)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, f->sv) != 0) {
        return -1;
    }

    memset(&f->priv, 0, sizeof(f->priv));
    f->priv.is_manager = true;
    atomic_store(&f->priv.local_node_id, 1);
    f->priv.mesh_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);
    f->priv.connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    /* No node has claims, so the GID broadcast sends each an empty set */
    f->priv.node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    qemu_mutex_init(&f->priv.mesh_table_lock);
    qemu_mutex_init(&f->priv.conn_table_lock);
    qemu_mutex_init(&f->priv.gid_table_lock);
    qemu_mutex_init(&f->priv.topology_broadcast_lock);
    qemu_mutex_init(&f->priv.gid_broadcast_lock);

    memset(&f->conn, 0, sizeof(f->conn));
    f->conn.node_id = 42;
    f->conn.sockfd = f->sv[0];
    /* The fixture's own reference: a lookup's ref and unref can't free it */
    atomic_init(&f->conn.refcount, 1);
    atomic_store(&f->conn.is_connected, true);
    f->conn.priv = &f->priv;
    qemu_mutex_init(&f->conn.lock);
    g_hash_table_insert(f->priv.connections, GUINT_TO_POINTER(42u), &f->conn);

    fill_mesh(f->priv.mesh_nodes, num_nodes);
    error_reports = 0;
    return 0;
}

static void fixture_fini(struct fixture *f)
{
    close(f->sv[0]);
    close(f->sv[1]);
    qemu_mutex_destroy(&f->conn.lock);
    qemu_mutex_destroy(&f->priv.gid_broadcast_lock);
    qemu_mutex_destroy(&f->priv.topology_broadcast_lock);
    qemu_mutex_destroy(&f->priv.gid_table_lock);
    qemu_mutex_destroy(&f->priv.conn_table_lock);
    qemu_mutex_destroy(&f->priv.mesh_table_lock);
    g_hash_table_destroy(f->priv.node_gids);
    g_hash_table_destroy(f->priv.connections);
    empty_mesh(f->priv.mesh_nodes);
    g_hash_table_destroy(f->priv.mesh_nodes);
}

/*
 * Check that a broadcast that carried node @id in slot @slot names a node in
 * the mesh table, and one no earlier slot named; mark it in @seen.
 */
static int check_node_id(const struct testcase *tc, const char *what,
                         uint32_t slot, uint32_t id, bool seen[MAX_CASE_NODES])
{
    if (id < 100 || id >= 100 + tc->num_nodes) {
        printf("FAIL %-16s: %s slot %u has bogus node_id %u\n", tc->name, what,
               slot, id);
        return 1;
    }
    if (seen[id - 100]) {
        printf("FAIL %-16s: %s slot %u repeats node_id %u\n", tc->name, what,
               slot, id);
        return 1;
    }
    seen[id - 100] = true;
    return 0;
}

/* A table past the cap must be reported, and a table within it must not */
static int check_errors(const struct testcase *tc, const char *what)
{
    if (error_reports != expected_errors(tc->num_nodes)) {
        printf("FAIL %-16s: %s reported %d error(s), expected %d\n", tc->name,
               what, error_reports, expected_errors(tc->num_nodes));
        return 1;
    }
    return 0;
}

static int run_topology_case(const struct testcase *tc)
{
    struct fixture f;
    TcpMsgHeader hdr;
    TcpMeshTopologyPayload wire;
    bool seen[MAX_CASE_NODES] = {false};
    uint32_t carried = carried_nodes(tc->num_nodes);
    int fail = 0;

    if (fixture_init(&f, tc->num_nodes) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc->name);
        return 1;
    }

    if (broadcast_on_poisoned_stack(&f.priv) != 0) {
        printf("FAIL %-16s: broadcast did not run on the poisoned stack "
               "(thread setup failed, or ASan relocated the frame to a fake "
               "stack -- the tail check below would be vacuous)\n",
               tc->name);
        fail = 1;
        goto out;
    }

    if (read_exact(f.sv[1], &hdr, sizeof(hdr)) != 0 ||
        read_exact(f.sv[1], &wire, sizeof(wire)) != 0) {
        printf("FAIL %-16s: short read from socketpair\n", tc->name);
        fail = 1;
        goto out;
    }

    if (ntohl(hdr.msg_type) != TCP_MSG_MESH_TOPOLOGY ||
        ntohl(hdr.msg_len) != sizeof(wire)) {
        printf("FAIL %-16s: unexpected header type=%u len=%u\n", tc->name,
               ntohl(hdr.msg_type), ntohl(hdr.msg_len));
        fail = 1;
    }

    if (ntohl(wire.num_nodes) != carried) {
        printf("FAIL %-16s: num_nodes %u, expected %u\n", tc->name,
               ntohl(wire.num_nodes), carried);
        fail = 1;
    }

    /* Live entries: hash-table order is unspecified, so match by node_id */
    for (uint32_t i = 0; i < carried; i++) {
        uint32_t id = ntohl(wire.nodes[i].node_id);
        char expect_host[64];

        if (check_node_id(tc, "topology", i, id, seen) != 0) {
            fail = 1;
            continue;
        }
        snprintf(expect_host, sizeof(expect_host), "node-%u.example", id - 100);
        if (strcmp(wire.nodes[i].hostname, expect_host) != 0) {
            printf("FAIL %-16s: slot %u hostname '%s', expected '%s'\n",
                   tc->name, i, wire.nodes[i].hostname, expect_host);
            fail = 1;
        }
        if (ntohs(wire.nodes[i].port) != 9000 + (id - 100)) {
            printf("FAIL %-16s: slot %u port %u\n", tc->name, i,
                   ntohs(wire.nodes[i].port));
            fail = 1;
        }
    }

    /* The point of the test: every byte past the live entries is zero */
    const uint8_t *tail = (const uint8_t *)&wire.nodes[carried];
    size_t tail_len =
        (size_t)(TOPO_MAXNODE - carried) * sizeof(TcpMeshNodeInfo);
    for (size_t i = 0; i < tail_len; i++) {
        if (tail[i] != 0) {
            printf("FAIL %-16s: unused slot byte %zu of %zu is 0x%02x, "
                   "expected 0 (stack contents leaked on the wire)\n",
                   tc->name, i, tail_len, tail[i]);
            fail = 1;
            break;
        }
    }

    fail |= check_errors(tc, "topology broadcast");

    if (!fail) {
        printf("PASS %-16s: topology carried %u of %u node(s), %zu-byte tail "
               "all zero\n",
               tc->name, carried, tc->num_nodes, tail_len);
    }

out:
    fixture_fini(&f);
    return fail;
}

/*
 * The GID broadcast sends one message per node it carries, so a table past
 * the cap shows up as a missing message, not a short count.
 */
static int run_gid_case(const struct testcase *tc)
{
    struct fixture f;
    TcpMsgHeader hdr;
    TcpNodeGidsPayload wire;
    bool seen[MAX_CASE_NODES] = {false};
    uint32_t carried = carried_nodes(tc->num_nodes);
    char extra;
    int fail = 0;

    if (fixture_init(&f, tc->num_nodes) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc->name);
        return 1;
    }

    tcp_broadcast_gid_table(&f.priv);
    /* So reading past the last message sees end-of-file, not a hang */
    shutdown(f.sv[0], SHUT_WR);

    for (uint32_t i = 0; i < carried; i++) {
        if (read_exact(f.sv[1], &hdr, sizeof(hdr)) != 0 ||
            read_exact(f.sv[1], &wire, sizeof(wire)) != 0) {
            printf("FAIL %-16s: GID broadcast ended after %u of %u "
                   "message(s)\n",
                   tc->name, i, carried);
            fail = 1;
            goto out;
        }
        if (ntohl(hdr.msg_type) != TCP_MSG_NODE_GIDS ||
            ntohl(hdr.msg_len) != sizeof(wire)) {
            printf("FAIL %-16s: unexpected header type=%u len=%u\n", tc->name,
                   ntohl(hdr.msg_type), ntohl(hdr.msg_len));
            fail = 1;
        }
        fail |= check_node_id(tc, "GID", i, ntohl(wire.node_id), seen);
        if (ntohl(wire.num_gids) != 0) {
            printf("FAIL %-16s: GID slot %u has %u GID(s), expected none\n",
                   tc->name, i, ntohl(wire.num_gids));
            fail = 1;
        }
    }

    if (recv(f.sv[1], &extra, 1, 0) != 0) {
        printf("FAIL %-16s: GID broadcast sent more than %u message(s)\n",
               tc->name, carried);
        fail = 1;
    }

    fail |= check_errors(tc, "GID broadcast");

    if (!fail) {
        printf("PASS %-16s: GID table carried %u of %u node(s)\n", tc->name,
               carried, tc->num_nodes);
    }

out:
    fixture_fini(&f);
    return fail;
}

int main(void)
{
    static const struct testcase cases[] = {
        {"empty-mesh", 0}, /* every slot unused */
        {"single-node", 1},
        {"few-nodes", 3},
        {"full-mesh", (uint32_t)TOPO_MAXNODE}, /* no tail at all */
        {"over-cap", MAX_CASE_NODES},          /* one node left out */
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failures += run_topology_case(&cases[i]);
        failures += run_gid_case(&cases[i]);
    }

    if (failures) {
        printf("\n%d test case(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll %zu test cases passed\n",
           2 * (sizeof(cases) / sizeof(cases[0])));
    return 0;
}
