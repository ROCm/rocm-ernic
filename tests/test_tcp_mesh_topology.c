/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
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
 * The static function is reached by #including the translation unit; the
 * broadcast path only needs the mesh/connection hash tables, so the rest of
 * the backend's externals are stubbed out.
 *
 * Table-driven: empty mesh (every slot unused), single node, a few nodes, and
 * a full mesh (no tail at all).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Pull in the code under test (including its static functions) */
#include "hw/rdma/rdma_backend_tcp.c"

/* ---- Stubs for the TU's external symbols -------------------------------
 * None of these are reachable from the topology-broadcast path; they exist
 * only to satisfy the linker
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
size_t dhcp_server_process(DhcpServer *server, const struct dhcp_packet *req,
                           size_t req_len, struct dhcp_packet *resp,
                           size_t max_resp_len)
{
    (void)server;
    (void)req;
    (void)req_len;
    (void)resp;
    (void)max_resp_len;
    return 0;
}
int eth_rx_inject_frame_mesh_blocking(PVRDMADev *dev, const void *frame_data,
                                      size_t len)
{
    (void)dev;
    (void)frame_data;
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
 * ASan's use-after-return detection relocates large frames off the real
 * stack into freshly-zeroed heap "fake stacks", which would hide the poison
 * and make this test vacuously pass. Turn it off for this binary; the
 * address/leak checks the project cares about here are unaffected.
 */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_stack_use_after_return=0";
}

/*
 * Address of a local in the broadcast thread's frame, recorded so the caller
 * can prove the payload really landed on the stack we poisoned.
 *
 * __asan_default_options() above is only a *default*: an ASAN_OPTIONS in the
 * environment overrides it, ASan moves the ~17 KB frame to a freshly-zeroed
 * fake stack, and the tail then reads as zero no matter what the code under
 * test does -- the test would pass vacuously. Checking where the frame landed
 * turns that silent hole into a loud failure.
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

    if (posix_memalign(&stack, sysconf(_SC_PAGESIZE), POISON_STACK_SZ) != 0) {
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
            mesh_node_info_new(i + 100, hostname, (uint16_t)(9000 + i));
        g_hash_table_insert(mesh_nodes, GUINT_TO_POINTER(i + 100), node);
    }
}

static void empty_mesh(GHashTable *mesh_nodes)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, mesh_nodes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        mesh_node_info_free((MeshNodeInfo *)value);
    }
    g_hash_table_remove_all(mesh_nodes);
}

static int run_case(const struct testcase *tc)
{
    TcpBackendPrivate priv;
    TcpConnection conn;
    TcpMsgHeader hdr;
    TcpMeshTopologyPayload wire;
    int sv[2];
    int fail = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL %-16s: socketpair failed\n", tc->name);
        return 1;
    }

    memset(&priv, 0, sizeof(priv));
    priv.is_manager = true;
    priv.local_node_id = 1;
    priv.mesh_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv.connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_mutex_init(&priv.mesh_table_lock);
    qemu_mutex_init(&priv.conn_table_lock);

    /* One "worker" on the near end of the socketpair */
    memset(&conn, 0, sizeof(conn));
    conn.node_id = 42;
    conn.sockfd = sv[0];
    conn.is_connected = true;
    conn.priv = &priv;
    qemu_mutex_init(&conn.lock);
    g_hash_table_insert(priv.connections, GUINT_TO_POINTER(42u), &conn);

    fill_mesh(priv.mesh_nodes, tc->num_nodes);

    if (broadcast_on_poisoned_stack(&priv) != 0) {
        printf("FAIL %-16s: broadcast did not run on the poisoned stack "
               "(thread setup failed, or ASan relocated the frame to a fake "
               "stack -- the tail check below would be vacuous)\n",
               tc->name);
        fail = 1;
        goto out;
    }

    if (read_exact(sv[1], &hdr, sizeof(hdr)) != 0 ||
        read_exact(sv[1], &wire, sizeof(wire)) != 0) {
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

    if (ntohl(wire.num_nodes) != tc->num_nodes) {
        printf("FAIL %-16s: num_nodes %u, expected %u\n", tc->name,
               ntohl(wire.num_nodes), tc->num_nodes);
        fail = 1;
    }

    /* Live entries: hash-table order is unspecified, so match by node_id */
    for (uint32_t i = 0; i < tc->num_nodes; i++) {
        uint32_t id = ntohl(wire.nodes[i].node_id);
        char expect_host[64];

        if (id < 100 || id >= 100 + tc->num_nodes) {
            printf("FAIL %-16s: slot %u has bogus node_id %u\n", tc->name, i,
                   id);
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
    const uint8_t *tail = (const uint8_t *)&wire.nodes[tc->num_nodes];
    size_t tail_len =
        (size_t)(TOPO_MAXNODE - tc->num_nodes) * sizeof(TcpMeshNodeInfo);
    for (size_t i = 0; i < tail_len; i++) {
        if (tail[i] != 0) {
            printf("FAIL %-16s: unused slot byte %zu of %zu is 0x%02x, "
                   "expected 0 (stack contents leaked on the wire)\n",
                   tc->name, i, tail_len, tail[i]);
            fail = 1;
            break;
        }
    }

    if (!fail) {
        printf("PASS %-16s: %u node(s), %zu-byte tail all zero\n", tc->name,
               tc->num_nodes, tail_len);
    }

out:
    close(sv[0]);
    close(sv[1]);
    qemu_mutex_destroy(&conn.lock);
    qemu_mutex_destroy(&priv.conn_table_lock);
    qemu_mutex_destroy(&priv.mesh_table_lock);
    g_hash_table_destroy(priv.connections);
    empty_mesh(priv.mesh_nodes);
    g_hash_table_destroy(priv.mesh_nodes);
    return fail;
}

int main(void)
{
    static const struct testcase cases[] = {
        {"empty-mesh", 0}, /* every slot unused */
        {"single-node", 1},
        {"few-nodes", 3},
        {"full-mesh", (uint32_t)TOPO_MAXNODE}, /* no tail at all */
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failures += run_case(&cases[i]);
    }

    if (failures) {
        printf("\n%d test case(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll %zu test cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
