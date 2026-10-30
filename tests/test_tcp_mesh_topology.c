/*
 *
 * Unit tests for the manager's broadcasts, the passes of its broadcaster,
 * tcp_broadcast_pass(), in tcp_manager.c.
 *
 * Regression coverage for a stack-memory disclosure: the manager sends the
 * full fixed-size topology payload (~17 KB, TOPO_MAXNODE node slots) on
 * every broadcast, but only filled in nodes[0..num_nodes). The struct was an
 * uninitialised stack local, so the unused tail slots put whatever the
 * thread's stack happened to hold -- including live pointers -- on the wire.
 *
 * Each case runs the pass on a thread whose stack the test allocated and
 * pre-filled with a poison byte, then reads the message back off a
 * socketpair and asserts every byte past num_nodes is zero. Without the
 * zero-fill the poison shows up on the wire and the test fails.
 *
 * The test links the TCP backend and reaches the broadcasts through
 * rdma_backend_tcp_priv.h; they only need the mesh, connection, and GID hash
 * tables, so the rest of the backend's externals are stubbed out.
 *
 * Table-driven: empty mesh (every slot unused), single node, a few nodes, a
 * full mesh (no tail at all), and a mesh table past the cap.
 *
 * The topology, and the whole GID table that a node that has just
 * registered is sent, snapshot the mesh table into TCP_MAX_MESH_NODES slots.
 * The same cases check that each fills every slot it can and, given a table
 * past the cap, stops at the cap and logs an error instead of leaving nodes
 * out silently.
 *
 * Every other change to the claims is sent as the changed node's set alone,
 * once to each connection.  The update case checks what a node's set says
 * when it claims something, is dead, or has left the table.
 *
 * A last case checks that the broadcaster coalesces, and misses nothing: what
 * is asked while a pass sends is sent by the next, once however often it was
 * asked, and after the first pass's.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

#include "byteorder.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_tcp_priv.h"
#include "rocm-ernic-warnings.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 * Apart from error_report(), which counts calls so a case can check whether
 * a pass reported an overflow, none of these are reachable from the passes;
 * they exist only to satisfy the linker
 */

/* Read only once the pass has returned or its thread has been joined */
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
int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg)
{
    (void)thread;
    (void)name;
    (void)start_routine;
    (void)arg;
    return 0;
}
void qemu_thread_set_name(const char *name)
{
    (void)name;
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}

/* ---- Test helpers ------------------------------------------------------ */

/*
 * Derived from the payload rather than hardcoded: if the node slots are ever
 * resized, a literal here would silently check a short tail instead of
 * failing
 */
#define TOPO_MAXNODE \
    ((TCP_TOPOLOGY_LEN - TCP_TOPOLOGY_NODE_OFF(0)) / TCP_MESH_NODE_LEN)

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

/* Each part of a pass reports one error when it has to leave nodes out */
static int expected_errors(uint32_t num_nodes, int parts)
{
    return num_nodes > TCP_MAX_MESH_NODES ? parts : 0;
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

/* Read a message header into @hdr, in host byte order, or return -1. */
static int read_hdr(int fd, TcpMsgHeader *hdr)
{
    uint8_t wire[TCP_MSG_HDR_LEN];

    if (read_exact(fd, wire, sizeof(wire)) != 0) {
        return -1;
    }
    tcp_msg_hdr_get(hdr, wire);
    return 0;
}

/* A GID set's node id, its number of GIDs, and its GID slot @i */
static uint32_t gids_node(const TcpNodeGidsWire *wire)
{
    return ernic_get_be32(wire->bytes + TCP_NODE_GIDS_NODE_ID_OFF);
}

static uint32_t gids_num(const TcpNodeGidsWire *wire)
{
    return ernic_get_be32(wire->bytes + TCP_NODE_GIDS_NUM_OFF);
}

static const uint8_t *gids_slot(const TcpNodeGidsWire *wire, uint32_t i)
{
    return wire->bytes + TCP_NODE_GIDS_GIDS_OFF + ((size_t)i * 16);
}

/*
 * Run tcp_broadcast_pass() on a thread whose stack we allocate and pre-fill
 * with a poison pattern.
 *
 * Poisoning the caller's own frame and hoping the payload lands on it is not
 * reliable -- the compiler is free to place the ~17 KB struct anywhere, and
 * ASan's redzones shift frames further. Owning the whole stack removes the
 * guesswork: wherever the payload lands, it lands on POISON_BYTE, so any
 * unused slot that reaches the wire un-zeroed is unambiguous.
 */
#define POISON_BYTE     0xAB
#define POISON_STACK_SZ ((size_t)1024 * 1024)

/*
 * One run of the pass on the poisoned stack. The thread checks that a local
 * in its own frame lies within the stack we poisoned, proving the payload
 * really landed on it.
 *
 * ASan's use-after-return detection relocates large frames off the real
 * stack into freshly-zeroed heap "fake stacks", which would hide the poison
 * and make this test vacuously pass: the ~17 KB frame's tail would read as
 * zero no matter what the code under test does. tests/CMakeLists.txt builds
 * this binary without that instrumentation. Checking where the frame landed
 * keeps a build that has it anyway from passing silently.
 *
 * The thread does the check itself, so only its answer, never the address of
 * one of its locals, outlives its frame.
 */
struct poisoned_run {
    TcpBackendPrivate *priv;
    uintptr_t base;         /* Start of the stack we poisoned */
    bool on_poisoned_stack; /* Set by the thread, read after the join */
};

static void *broadcast_thread(void *opaque)
{
    struct poisoned_run *run = opaque;
    int frame_marker;
    /* Compared as uintptr_t: relational operators on pointers into different
     * objects are unspecified in C, and the frame is not part of the stack
     * buffer as far as the language is concerned. */
    uintptr_t frame = (uintptr_t)&frame_marker;

    run->on_poisoned_stack =
        frame >= run->base && frame < run->base + POISON_STACK_SZ;
    tcp_broadcast_pass(run->priv);
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

    /* pthread_join() below orders the thread's write against our read */
    struct poisoned_run run = {
        .priv = priv,
        .base = (uintptr_t)stack,
        .on_poisoned_stack = false,
    };

    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, POISON_STACK_SZ);
    ret = pthread_create(&tid, &attr, broadcast_thread, &run);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        free(stack);
        return -1;
    }
    pthread_join(tid, NULL);

    free(stack);
    return run.on_poisoned_stack ? 0 : -1;
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
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iter, mesh_nodes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        tcp_mesh_node_info_free((MeshNodeInfo *)value);
    }
    g_hash_table_remove_all(mesh_nodes);
}

/* A manager with one "worker", node 42, on the near end of a socketpair */
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
    /* The fixture runs no threads, and sets up before anything runs */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    f->priv.mesh_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);
    f->priv.connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    /* No node has claims, so the GID broadcast sends each an empty set */
    f->priv.node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    f->priv.gid_owner = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                              tcp_gid_key_free, NULL);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_mutex_init(&f->priv.mesh_table_lock);
    qemu_mutex_init(&f->priv.conn_table_lock);
    qemu_mutex_init(&f->priv.gid_table_lock);
    tcp_broadcaster_init(&f->priv);

    memset(&f->conn, 0, sizeof(f->conn));
    f->conn.node_id = 42;
    /* The fixture's own reference: a lookup's ref and unref can't free it */
    atomic_init(&f->conn.refcount, 1);
    atomic_store(&f->conn.is_connected, true);
    f->conn.priv = &f->priv;
    qemu_mutex_init(&f->conn.lock);
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    f->conn.sockfd = f->sv[0];
    g_hash_table_insert(f->priv.connections, GUINT_TO_POINTER(42U), &f->conn);
    fill_mesh(f->priv.mesh_nodes, num_nodes);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    error_reports = 0;
    return 0;
}

static void fixture_fini(struct fixture *f)
{
    close(f->sv[0]);
    close(f->sv[1]);
    qemu_mutex_destroy(&f->conn.lock);
    tcp_broadcaster_destroy(&f->priv);
    qemu_mutex_destroy(&f->priv.gid_table_lock);
    qemu_mutex_destroy(&f->priv.conn_table_lock);
    qemu_mutex_destroy(&f->priv.mesh_table_lock);
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(f->priv.gid_owner);
    g_hash_table_destroy(f->priv.node_gids);
    g_hash_table_destroy(f->priv.connections);
    empty_mesh(f->priv.mesh_nodes);
    g_hash_table_destroy(f->priv.mesh_nodes);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
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

/*
 * A table past the cap must be reported, once by each of the broadcast's
 * @parts, and a table within it must not
 */
static int check_errors(const struct testcase *tc, const char *what, int parts)
{
    int expected = expected_errors(tc->num_nodes, parts);

    if (error_reports != expected) {
        printf("FAIL %-16s: %s reported %d error(s), expected %d\n", tc->name,
               what, error_reports, expected);
        return 1;
    }
    return 0;
}

/* The end of the stream: the sender has shut down its side */
static int check_eof(const struct testcase *tc, int fd, const char *what)
{
    char extra;

    if (recv(fd, &extra, 1, 0) != 0) {
        printf("FAIL %-16s: %s sent more than expected\n", tc->name, what);
        return 1;
    }
    return 0;
}

/*
 * Read the @carried messages of one whole GID table, in which no node has
 * claims, from @fd. Each must name a node in the mesh table, once, and send
 * every GID slot zeroed.
 */
static int read_empty_gid_table(const struct testcase *tc, int fd,
                                uint32_t carried)
{
    static const uint8_t zero[TCP_MAX_NODE_GIDS][16];
    bool seen[MAX_CASE_NODES] = {false};
    TcpMsgHeader hdr;
    TcpNodeGidsWire wire;
    int fail = 0;

    for (uint32_t i = 0; i < carried; i++) {
        if (read_hdr(fd, &hdr) != 0 ||
            read_exact(fd, wire.bytes, sizeof(wire.bytes)) != 0) {
            printf("FAIL %-16s: GID table ended after %u of %u message(s)\n",
                   tc->name, i, carried);
            return 1;
        }
        if (hdr.msg_type != TCP_MSG_NODE_GIDS ||
            hdr.msg_len != sizeof(wire.bytes)) {
            printf("FAIL %-16s: unexpected header type=%u len=%u\n", tc->name,
                   hdr.msg_type, hdr.msg_len);
            fail = 1;
        }
        fail |= check_node_id(tc, "GID", i, gids_node(&wire), seen);
        if (gids_num(&wire) != 0) {
            printf("FAIL %-16s: GID slot %u has %u GID(s), expected none\n",
                   tc->name, i, gids_num(&wire));
            fail = 1;
        }
        if (memcmp(gids_slot(&wire, 0), zero, sizeof(zero)) != 0) {
            printf("FAIL %-16s: GID slot %u has unused GIDs that aren't zero "
                   "(stack contents leaked on the wire)\n",
                   tc->name, i);
            fail = 1;
        }
    }
    return fail;
}

/*
 * A node that has just registered is sent the topology, then the whole GID
 * table, which a pass sends together.
 */
static int run_mesh_case(const struct testcase *tc)
{
    struct fixture f;
    TcpMsgHeader hdr;
    uint8_t wire[TCP_TOPOLOGY_LEN];
    bool seen[MAX_CASE_NODES] = {false};
    uint32_t carried = carried_nodes(tc->num_nodes);
    const uint8_t *tail;
    size_t tail_len;
    int fail = 0;

    if (fixture_init(&f, tc->num_nodes) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc->name);
        return 1;
    }

    tcp_broadcast_topology(&f.priv);
    tcp_broadcast_gid_table_to(&f.priv, f.conn.node_id);
    if (broadcast_on_poisoned_stack(&f.priv) != 0) {
        printf("FAIL %-16s: broadcast did not run on the poisoned stack "
               "(thread setup failed, or ASan relocated the frame to a fake "
               "stack -- the tail check below would be vacuous)\n",
               tc->name);
        fail = 1;
        goto out;
    }
    /* So reading past the last message sees end-of-file, not a hang */
    shutdown(f.sv[0], SHUT_WR);

    if (read_hdr(f.sv[1], &hdr) != 0 ||
        read_exact(f.sv[1], wire, sizeof(wire)) != 0) {
        printf("FAIL %-16s: short read from socketpair\n", tc->name);
        fail = 1;
        goto out;
    }

    if (hdr.msg_type != TCP_MSG_MESH_TOPOLOGY || hdr.msg_len != sizeof(wire)) {
        printf("FAIL %-16s: unexpected header type=%u len=%u\n", tc->name,
               hdr.msg_type, hdr.msg_len);
        fail = 1;
    }

    if (ernic_get_be32(wire + TCP_TOPOLOGY_NUM_NODES_OFF) != carried) {
        printf("FAIL %-16s: num_nodes %u, expected %u\n", tc->name,
               ernic_get_be32(wire + TCP_TOPOLOGY_NUM_NODES_OFF), carried);
        fail = 1;
    }

    /* Live entries: hash-table order is unspecified, so match by node_id */
    for (uint32_t i = 0; i < carried; i++) {
        const uint8_t *entry = wire + TCP_TOPOLOGY_NODE_OFF(i);
        const char *host = (const char *)entry + TCP_MESH_NODE_HOSTNAME_OFF;
        uint32_t id = ernic_get_be32(entry + TCP_MESH_NODE_ID_OFF);
        char expect_host[64];

        if (check_node_id(tc, "topology", i, id, seen) != 0) {
            fail = 1;
            continue;
        }
        snprintf(expect_host, sizeof(expect_host), "node-%u.example", id - 100);
        /* The terminator too, which the field needn't have */
        if (memcmp(host, expect_host, strlen(expect_host) + 1) != 0) {
            printf("FAIL %-16s: slot %u hostname '%.*s', expected '%s'\n",
                   tc->name, i, TCP_HOSTNAME_LEN, host, expect_host);
            fail = 1;
        }
        if (ernic_get_be16(entry + TCP_MESH_NODE_PORT_OFF) !=
            9000 + (id - 100)) {
            printf("FAIL %-16s: slot %u port %u\n", tc->name, i,
                   ernic_get_be16(entry + TCP_MESH_NODE_PORT_OFF));
            fail = 1;
        }
    }

    /* The point of the test: every byte past the live entries is zero */
    tail = wire + TCP_TOPOLOGY_NODE_OFF(carried);
    tail_len = (TOPO_MAXNODE - carried) * TCP_MESH_NODE_LEN;
    for (size_t i = 0; i < tail_len; i++) {
        if (tail[i] != 0) {
            printf("FAIL %-16s: unused slot byte %zu of %zu is 0x%02x, "
                   "expected 0 (stack contents leaked on the wire)\n",
                   tc->name, i, tail_len, tail[i]);
            fail = 1;
            break;
        }
    }

    /* The GID table follows the topology, on the same poisoned stack */
    fail |= read_empty_gid_table(tc, f.sv[1], carried);
    fail |= check_eof(tc, f.sv[1], "pass");
    fail |= check_errors(tc, "pass", 2);

    if (!fail) {
        printf("PASS %-16s: topology and GID table carried %u of %u "
               "node(s), %zu-byte tail all zero\n",
               tc->name, carried, tc->num_nodes, tail_len);
    }

out:
    fixture_fini(&f);
    return fail;
}

/*
 * The whole GID table alone, one message per node it carries, so a table
 * past the cap shows up as a missing message, not a short count.
 */
static int run_table_case(const struct testcase *tc)
{
    struct fixture f;
    uint32_t carried = carried_nodes(tc->num_nodes);
    int fail = 0;

    if (fixture_init(&f, tc->num_nodes) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc->name);
        return 1;
    }

    tcp_broadcast_gid_table_to(&f.priv, f.conn.node_id);
    tcp_broadcast_pass(&f.priv);
    /* So reading past the last message sees end-of-file, not a hang */
    shutdown(f.sv[0], SHUT_WR);

    fail |= read_empty_gid_table(tc, f.sv[1], carried);
    fail |= check_eof(tc, f.sv[1], "GID table");
    fail |= check_errors(tc, "GID table", 1);

    if (!fail) {
        printf("PASS %-16s: GID table carried %u of %u node(s)\n", tc->name,
               carried, tc->num_nodes);
    }

    fixture_fini(&f);
    return fail;
}

/* The one GID the update and order cases' node 100 claims */
static const uint8_t claimed[1][16] = {{0xfe, 0x80, [15] = 1}};

/*
 * Read one GID set from @fd into @wire; false, saying why, if it isn't one.
 * @what names the case.
 */
static bool read_gid_set(const char *what, int fd, TcpNodeGidsWire *wire)
{
    TcpMsgHeader hdr;

    if (read_hdr(fd, &hdr) != 0 || hdr.msg_type != TCP_MSG_NODE_GIDS ||
        hdr.msg_len != sizeof(wire->bytes) ||
        read_exact(fd, wire->bytes, sizeof(wire->bytes)) != 0) {
        printf("FAIL %-16s: a GID set didn't come\n", what);
        return false;
    }
    return true;
}

/*
 * Whether @wire, a set for node @id, carries @want GIDs, claimed if any,
 * and nothing in the slots past them
 */
static int check_gid_set(const char *what, const TcpNodeGidsWire *wire,
                         uint32_t id, uint32_t want)
{
    static const uint8_t zero[TCP_MAX_NODE_GIDS][16];

    if (gids_num(wire) != want ||
        (want && memcmp(gids_slot(wire, 0), claimed[0], 16) != 0) ||
        memcmp(gids_slot(wire, want), zero[0],
               sizeof(zero) - ((size_t)want * 16)) != 0) {
        printf("FAIL %-16s: node %u's set has %u GID(s), expected %u, or "
               "isn't zero past them\n",
               what, id, gids_num(wire), want);
        return 1;
    }
    return 0;
}

/*
 * A pass sends each node whose set it was asked for that set, alone, once,
 * however often it was asked: node 100, which claims a GID; node 101, which
 * claims one but is dead, so it owns nothing; and node 999, which claims one
 * but isn't in the table, as an evicted node isn't, so it owns nothing
 * either.  (They claim the same GID, which the sets don't care about.)
 */
static int run_update_case(void)
{
    static const char what[] = "update";
    struct fixture f;
    TcpNodeGidsWire wire;
    bool seen[3] = {false, false, false};
    MeshNodeInfo *node;
    int fail = 0;

    if (fixture_init(&f, 3) != 0) {
        printf("FAIL %-16s: socketpair failed\n", what);
        return 1;
    }
    tcp_gid_claims_set(&f.priv, 100, claimed, 1);
    tcp_gid_claims_set(&f.priv, 101, claimed, 1);
    tcp_gid_claims_set(&f.priv, 999, claimed, 1);
    qemu_mutex_lock(&f.priv.mesh_table_lock);
    node = g_hash_table_lookup(f.priv.mesh_nodes, GUINT_TO_POINTER(101U));
    tcp_node_set_alive(&f.priv, node, false, NULL);
    qemu_mutex_unlock(&f.priv.mesh_table_lock);

    tcp_broadcast_node_gids(&f.priv, 100);
    tcp_broadcast_node_gids(&f.priv, 101);
    tcp_broadcast_node_gids(&f.priv, 999);
    tcp_broadcast_node_gids(&f.priv, 100);
    tcp_broadcast_pass(&f.priv);
    shutdown(f.sv[0], SHUT_WR);

    /* In no particular order: the nodes are a set */
    for (int i = 0; !fail && i < 3; i++) {
        uint32_t id;

        if (!read_gid_set(what, f.sv[1], &wire)) {
            fail = 1;
            break;
        }
        id = gids_node(&wire);
        if (id == 100 && !seen[0]) {
            fail |= check_gid_set(what, &wire, id, 1);
            seen[0] = true;
        } else if (id == 101 && !seen[1]) {
            fail |= check_gid_set(what, &wire, id, 0);
            seen[1] = true;
        } else if (id == 999 && !seen[2]) {
            fail |= check_gid_set(what, &wire, id, 0);
            seen[2] = true;
        } else {
            printf("FAIL %-16s: a set for node %u, which wasn't asked for, "
                   "or came twice\n",
                   what, id);
            fail = 1;
        }
    }
    if (!fail) {
        static const struct testcase tc = {what, 3};

        fail |= check_eof(&tc, f.sv[1], "the pass");
    }

    if (!fail) {
        printf("PASS %-16s: each node's set came once, alone, and owns "
               "nothing when the node is dead or gone\n",
               what);
    }
    fixture_fini(&f);
    return fail;
}

/* A pass on a thread of its own, and whether it was started */
struct case_thread {
    pthread_t tid;
    bool started;
};

static void *pass_thread(void *opaque)
{
    tcp_broadcast_pass((TcpBackendPrivate *)opaque);
    return NULL;
}

/* Whether, within a few seconds, a pass has taken the topology it was asked */
static bool topology_taken(TcpBackendPrivate *priv)
{
    for (int i = 0; i < 5000; i++) {
        bool asked;

        qemu_mutex_lock(&priv->broadcast_lock);
        asked = priv->broadcast_topology;
        qemu_mutex_unlock(&priv->broadcast_lock);
        if (!asked) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

/* The broadcasts sent so far, as the broadcaster counts them */
static uint64_t sent_so_far(TcpBackendPrivate *priv)
{
    uint64_t sent;

    qemu_mutex_lock(&priv->broadcast_lock);
    sent = priv->broadcasts_sent;
    qemu_mutex_unlock(&priv->broadcast_lock);
    return sent;
}

/*
 * The first pass sends the topology.  While it does, node 100 claims a GID,
 * and the broadcaster is asked for the topology and node 100's set, each
 * twice.  The first pass took what it was asked before that, so it sends only
 * its topology, and the next sends one topology and one set for node 100,
 * which has the claim.
 */
static int run_order_case(void)
{
    static const struct testcase tc = {"order", 2};
    const uint32_t claimant = 100;
    struct fixture f;
    struct case_thread first = {.started = false};
    TcpMsgHeader hdr;
    uint8_t topo[TCP_TOPOLOGY_LEN];
    TcpNodeGidsWire wire;
    int fail = 0;

    if (fixture_init(&f, tc.num_nodes) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc.name);
        return 1;
    }

    /*
     * Hold the connections table, so the first pass stops between its
     * topology snapshot and its sends, once it has taken what it was asked.
     */
    tcp_broadcast_topology(&f.priv);
    qemu_mutex_lock(&f.priv.conn_table_lock);
    first.started = pthread_create(&first.tid, NULL, pass_thread, &f.priv) == 0;
    if (!first.started || !topology_taken(&f.priv)) {
        printf("FAIL %-16s: the first pass didn't start\n", tc.name);
        fail = 1;
    } else {
        tcp_gid_claims_set(&f.priv, claimant, claimed, 1);
        for (int i = 0; i < 2; i++) {
            tcp_broadcast_topology(&f.priv);
            tcp_broadcast_node_gids(&f.priv, claimant);
        }
    }
    qemu_mutex_unlock(&f.priv.conn_table_lock);
    if (first.started) {
        pthread_join(first.tid, NULL);
    }
    if (fail) {
        goto out;
    }
    if (sent_so_far(&f.priv) != 1) {
        printf("FAIL %-16s: the first pass sent %" PRIu64 " broadcast(s), "
               "expected the 1 asked before it\n",
               tc.name, sent_so_far(&f.priv));
        fail = 1;
        goto out;
    }
    tcp_broadcast_pass(&f.priv);
    shutdown(f.sv[0], SHUT_WR);

    for (int i = 0; i < 2; i++) {
        if (read_hdr(f.sv[1], &hdr) != 0 ||
            hdr.msg_type != TCP_MSG_MESH_TOPOLOGY ||
            read_exact(f.sv[1], topo, sizeof(topo)) != 0) {
            printf("FAIL %-16s: topology %d didn't come, or not before the "
                   "GID set\n",
                   tc.name, i + 1);
            fail = 1;
            goto out;
        }
    }
    if (!read_gid_set(tc.name, f.sv[1], &wire)) {
        fail = 1;
        goto out;
    }
    if (gids_node(&wire) != claimant) {
        printf("FAIL %-16s: a set for node %u, which wasn't asked for\n",
               tc.name, gids_node(&wire));
        fail = 1;
    }
    fail |= check_gid_set(tc.name, &wire, claimant, 1);
    fail |= check_eof(&tc, f.sv[1], "the second pass");
    if (!fail && sent_so_far(&f.priv) != 5) {
        printf("FAIL %-16s: the passes sent %" PRIu64 " broadcast(s), "
               "expected all 5 asked\n",
               tc.name, sent_so_far(&f.priv));
        fail = 1;
    }

    if (!fail) {
        printf("PASS %-16s: what was asked during a pass came with the next, "
               "once\n",
               tc.name);
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
    const size_t ncases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    for (size_t i = 0; i < ncases; i++) {
        failures += run_mesh_case(&cases[i]);
        failures += run_table_case(&cases[i]);
    }
    failures += run_update_case();
    failures += run_order_case();

    if (failures) {
        printf("\n%d test case(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll %zu test cases passed\n", (2 * ncases) + 2);
    return 0;
}
