/*
 *
 * Unit tests for TcpConnection lifetime in rdma_backend_tcp.c.
 *
 * A TcpConnection is shared: the data path and every receive thread look it
 * up by node ID and then use it, while the manager's health check can
 * replace it with a new one (tcp_mesh_reconnect_node()) at any moment. The
 * replacement must not end the old object's life while one of those users
 * still holds it, and a user caught mid-send on the old socket must get an
 * error back rather than hang.
 *
 * The two reconnect cases make the overlap deterministic instead of hoping a
 * race lands. Each runs two users of node A's connection:
 *
 *   - A parked sender. It sends a message much larger than the socket's send
 *     buffer to a far end the test never drains, so it blocks inside the
 *     send holding conn->lock. The first bytes arriving at the far end prove
 *     it is there.
 *
 *   - A waiting user, the one the case is about. It looks node A up and then
 *     blocks on conn->lock behind the parked sender. The fixture's
 *     connection table hashes through lookup_hook_hash(), which reports the
 *     waiting user's lookup; that report arrives with the waiting user still
 *     inside conn_table_lock, so the reconnect's table update cannot run
 *     until the waiting user already holds the old pointer.
 *
 * Only then does the test reconnect node A. When the parked sender's send
 * fails, the waiting user takes the lock and reads the connection. If the
 * reconnect ended the connection's life, that read is a heap-use-after-free
 * under ASan -- the waiting user matters because the parked sender's only
 * later access is the unlock inside libc, which ASan does not see. These
 * cases therefore matter most on the sanitizer builds; the assertions below
 * check what is observable without one.
 *
 * The health-check case runs a whole tcp_health_check_pass() against a node
 * that has stopped answering, with a loopback listener standing in for the
 * node, and checks the pass carries the reconnect through: death announced
 * first, new connection installed and announced, old one retired, node
 * marked alive.
 *
 * The last case covers ownership of the table itself: a peer that sends
 * REGISTER_NODE twice on one connection must not get that connection mapped
 * under a second node ID, which would leave the connection table holding
 * two owning references to one object.
 *
 * The static functions are reached by #including the translation unit. The
 * receive threads are real, so qemu_thread_create()/qemu_thread_join() are
 * implemented here on pthreads rather than stubbed out; the rest of the
 * backend's externals are stubs.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Pull in the code under test (including its static functions) */
#include "rdma/rdma_backend_tcp.c"

/* ---- Stubs for the TU's external symbols -------------------------------
 * The thread functions are real: the paths under test start and join
 * receive threads. The rest exist only to satisfy the linker.
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
    (void)name;
    (void)mode;
    return pthread_create(&thread->thread, NULL, start_routine, arg);
}
void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}
void qemu_thread_join(QemuThread *thread)
{
    pthread_join(thread->thread, NULL);
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

/* ---- Fixture ------------------------------------------------------------ */

#define MANAGER_NODE 0u
#define NODE_A       1u
#define NODE_B       2u

/* The node ID the manager's accept thread gives a connection until it
 * registers. */
#define UNASSIGNED_NODE 0xFFFFFFFFu

#define PEER_HOST "peer.invalid"
#define PEER_PORT 4791

/*
 * Far larger than any socket buffer involved, so a send of this size cannot
 * complete while the far end is not being read.
 */
#define BULK_LEN (1u << 20)

/* Small enough to fit any socket buffer in one piece. */
#define SMALL_LEN 64u

/* Chunk size for discarding message payloads the test does not inspect. */
#define SINK_LEN 4096

/* The smallest send buffer the kernel will grant; the request is a floor. */
#define SMALL_SNDBUF 4096

/* Bound on every wait, so a regression fails the case instead of hanging. */
#define WAIT_MS 10000

/*
 * Reports the next lookup of NODE_A in the connection table. GLib calls the
 * hash function inside every lookup, and the backend only looks the table up
 * with conn_table_lock held, so when the report arrives the looking-up
 * thread still holds that lock and nothing can replace the entry before it
 * has the pointer in hand.
 */
static struct {
    _Atomic bool armed;
    _Atomic bool seen;
} lookup_hook;

static guint lookup_hook_hash(gconstpointer key)
{
    if (GPOINTER_TO_UINT(key) == NODE_A &&
        atomic_exchange(&lookup_hook.armed, false)) {
        atomic_store(&lookup_hook.seen, true);
    }
    return g_direct_hash(key);
}

static void lookup_hook_arm(void)
{
    atomic_store(&lookup_hook.seen, false);
    atomic_store(&lookup_hook.armed, true);
}

/* True once the armed lookup has happened. */
static bool lookup_hook_wait(void)
{
    const struct timespec tick = {.tv_sec = 0, .tv_nsec = 1000000};

    for (int ms = 0; ms < WAIT_MS; ms++) {
        if (atomic_load(&lookup_hook.seen)) {
            return true;
        }
        nanosleep(&tick, NULL);
    }
    atomic_store(&lookup_hook.armed, false);
    return false;
}

/*
 * A manager-mode backend with nothing running but what each case starts.
 * The destroy notifiers are the production ones, so a replaced or destroyed
 * entry is released exactly as tcp_init() arranges; the connection table's
 * hash differs only by reporting lookups.
 */
struct mesh_fixture {
    TcpBackendPrivate priv;
    RdmaBackendDev backend_dev;
};

/* One peer: the backend's connection and the test's end of its socket. */
struct peer {
    TcpConnection *conn;
    int far;
};

static void fixture_init(struct mesh_fixture *f)
{
    memset(f, 0, sizeof(*f));

    f->backend_dev.backend_private = &f->priv;
    f->priv.backend_dev = &f->backend_dev;
    f->priv.mode = TCP_MODE_MANAGER;
    f->priv.is_manager = true;
    f->priv.local_node_id = MANAGER_NODE;
    f->priv.next_available_node_id = 1;
    atomic_init(&f->priv.next_seq, 1);
    atomic_init(&f->priv.tcp_stats.reconnect_attempts, 0);
    atomic_init(&f->priv.tcp_stats.reconnect_successes, 0);

    f->priv.connections = g_hash_table_new_full(
        lookup_hook_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    f->priv.mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, mesh_node_info_destroy_notify);
    qemu_mutex_init(&f->priv.conn_table_lock);
    qemu_mutex_init(&f->priv.mesh_table_lock);
    qemu_mutex_init(&f->priv.lock);
    tcp_bufpool_init(&f->priv.recv_pool);

    /* The manager lists itself, as tcp_init() does. */
    g_hash_table_insert(
        f->priv.mesh_nodes, GUINT_TO_POINTER(MANAGER_NODE),
        mesh_node_info_new(MANAGER_NODE, "manager.invalid", PEER_PORT));
}

/*
 * Stop every receive thread still attached to the table, then release the
 * tables. The threads are joined before anything is freed so none of them
 * can be inside a lookup when the table goes away.
 */
static void fixture_destroy(struct mesh_fixture *f)
{
    tcp_retire_all_connections(&f->priv);

    g_hash_table_destroy(f->priv.mesh_nodes);
    g_hash_table_destroy(f->priv.connections);
    tcp_bufpool_destroy(&f->priv.recv_pool);
    qemu_mutex_destroy(&f->priv.lock);
    qemu_mutex_destroy(&f->priv.mesh_table_lock);
    qemu_mutex_destroy(&f->priv.conn_table_lock);
}

/*
 * A connected socket pair. sv[0] is the backend's end and is non-blocking,
 * as the accept and connect paths leave it; sv[1] is the test's end and
 * times out rather than blocking forever.
 */
static int socketpair_open(int sv[2], bool small_sndbuf)
{
    const struct timeval tmo = {.tv_sec = WAIT_MS / 1000, .tv_usec = 0};
    const int sndbuf = SMALL_SNDBUF;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return -1;
    }

    int flags = fcntl(sv[0], F_GETFL, 0);
    if (flags < 0 || fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) != 0 ||
        setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo)) != 0 ||
        (small_sndbuf && setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf,
                                    sizeof(sndbuf)) != 0)) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    return 0;
}

/* A connection to @node_id with its receive thread running, not yet in any
 * table. */
static int peer_open(struct mesh_fixture *f, uint32_t node_id,
                     bool small_sndbuf, struct peer *p)
{
    int sv[2];

    if (socketpair_open(sv, small_sndbuf) != 0) {
        return -1;
    }

    p->conn = tcp_connection_new(node_id, PEER_HOST, PEER_PORT);
    p->conn->priv = &f->priv;
    p->conn->sockfd = sv[0];
    atomic_store(&p->conn->is_connected, true);
    p->far = sv[1];

    atomic_store(&p->conn->recv_thread_running, true);
    qemu_thread_create(&p->conn->recv_thread, "test-recv",
                       tcp_recv_thread_per_conn, p->conn, QEMU_THREAD_JOINABLE);
    return 0;
}

/* Publish @p as a registered node listening at @host:@port, the way the
 * REGISTER_NODE handler does: in the mesh table and in the connection
 * table, which takes ownership. */
static int peer_register(struct mesh_fixture *f, const struct peer *p,
                         const char *host, uint16_t port)
{
    uint32_t node_id = p->conn->node_id;
    MeshNodeInfo *node = mesh_node_info_new(node_id, host, port);

    if (!node) {
        return -1;
    }
    node->is_alive = true;
    node->last_heartbeat = time(NULL);

    qemu_mutex_lock(&f->priv.mesh_table_lock);
    g_hash_table_insert(f->priv.mesh_nodes, GUINT_TO_POINTER(node_id), node);
    qemu_mutex_lock(&f->priv.conn_table_lock);
    g_hash_table_insert(f->priv.connections, GUINT_TO_POINTER(node_id),
                        p->conn);
    qemu_mutex_unlock(&f->priv.conn_table_lock);
    qemu_mutex_unlock(&f->priv.mesh_table_lock);
    return 0;
}

/*
 * Reconnect @node_id through the health check's own code, over a fresh
 * socket pair. The caller gets the test's end of the new pair in @far.
 */
static int reconnect(struct mesh_fixture *f, uint32_t node_id, int *far)
{
    int sv[2];

    if (socketpair_open(sv, false) != 0) {
        return -1;
    }

    /* Takes ownership of sv[0] whether or not it succeeds. */
    bool ok = tcp_mesh_reconnect_node(&f->priv, node_id, PEER_HOST, PEER_PORT,
                                      sv[0], time(NULL));

    *far = sv[1];
    return ok ? 0 : -1;
}

/*
 * Release senders still blocked on @fd's peer after a case has already
 * failed. Closing the far end fails their sends with EPIPE; SIGPIPE is
 * ignored first so that failure is reported rather than killing the run.
 * Only failure paths come here: on the passing path the backend itself must
 * fail the send without raising the signal.
 */
static void unblock_after_failure(int *fd)
{
    (void)signal(SIGPIPE, SIG_IGN);
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* ---- Wire helpers ------------------------------------------------------- */

/* Read exactly len bytes, or return -1 (including on timeout). */
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

/* Write exactly len bytes to a blocking socket, or return -1. */
static int write_exact(int fd, const void *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t ret = send(fd, (const char *)buf + sent, len - sent, 0);
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            return -1;
        }
        sent += (size_t)ret;
    }
    return 0;
}

/* Push one message in the wire format tcp_recv_message() expects:
 * byte-swapped header, then a raw payload. */
static int send_msg(int fd, TcpMsgType type, uint32_t src_node,
                    uint32_t dst_node, const void *payload, uint32_t len)
{
    TcpMsgHeader hdr;

    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.msg_len = htonl(len);
    hdr.seq = htonl(1);
    hdr.src_node_id = htonl(src_node);
    hdr.dst_node_id = htonl(dst_node);
    hdr.src_qpn = 0;
    hdr.dst_qpn = 0;

    if (write_exact(fd, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    return len ? write_exact(fd, payload, len) : 0;
}

/*
 * Read messages off @fd, discarding them, until one of type @want arrives.
 * Returns 0 once it has, or -1 on error or timeout. @seen, if given, counts
 * the messages of type @count_type passed over on the way.
 */
static int read_until(int fd, TcpMsgType want, TcpMsgType count_type,
                      unsigned *seen)
{
    for (;;) {
        TcpMsgHeader hdr;
        uint8_t sink[SINK_LEN];

        if (read_exact(fd, &hdr, sizeof(hdr)) != 0) {
            return -1;
        }
        uint32_t type = ntohl(hdr.msg_type);
        uint32_t left = ntohl(hdr.msg_len);

        while (left > 0) {
            size_t chunk = left < sizeof(sink) ? left : sizeof(sink);
            if (read_exact(fd, sink, chunk) != 0) {
                return -1;
            }
            left -= (uint32_t)chunk;
        }

        if (type == (uint32_t)want) {
            return 0;
        }
        if (seen && type == (uint32_t)count_type) {
            (*seen)++;
        }
    }
}

/* True once @fd has data to read. */
static bool wait_readable(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    return poll(&pfd, 1, WAIT_MS) == 1 && (pfd.revents & POLLIN);
}

/* ---- Data-path senders -------------------------------------------------- */

struct ionic_send {
    RdmaBackendDev *dev;
    uint32_t dst_node;
    const uint8_t *body;
    size_t body_len;
    pthread_t tid;
    bool started;
    int rc;
};

static void *ionic_send_thread(void *opaque)
{
    struct ionic_send *s = opaque;
    const uint8_t hdr[16] = {0};

    s->rc = tcp_backend_send_ionic_v(s->dev, s->dst_node, hdr, sizeof(hdr),
                                     s->body, s->body_len);
    return NULL;
}

/* Send @len bytes of @body to @dst_node on a thread of its own. */
static int ionic_send_start(struct ionic_send *s, struct mesh_fixture *f,
                            uint32_t dst_node, const uint8_t *body, size_t len)
{
    s->dev = &f->backend_dev;
    s->dst_node = dst_node;
    s->body = body;
    s->body_len = len;
    s->rc = 0;
    s->started = pthread_create(&s->tid, NULL, ionic_send_thread, s) == 0;
    return s->started ? 0 : -1;
}

static void ionic_send_join(struct ionic_send *s)
{
    if (s->started) {
        pthread_join(s->tid, NULL);
        s->started = false;
    }
}

/*
 * Start the parked sender: a bulk send to @p that blocks inside the send,
 * holding the connection's lock, until the send fails.
 */
static int park_sender(struct ionic_send *s, struct mesh_fixture *f,
                       const struct peer *p, const uint8_t *bulk)
{
    if (ionic_send_start(s, f, p->conn->node_id, bulk, BULK_LEN) != 0) {
        return -1;
    }
    /* Bytes at the far end mean the sender is inside the send, with the rest
     * of the body stuck behind a full buffer. */
    return wait_readable(p->far) ? 0 : -1;
}

/* ---- Cases -------------------------------------------------------------- */

/*
 * The data path is using node A's connection when the health check
 * reconnects A.
 *
 * Both sends were addressed before the reconnect, so both are on the old
 * connection, whose socket is gone afterwards. Neither can complete, so both
 * must come back as -EIO -- and must come back at all: nothing but the
 * backend will wake the parked sender.
 */
static int test_send_vs_reconnect(const char *name)
{
    struct mesh_fixture f;
    struct peer a;
    struct ionic_send parked = {0};
    struct ionic_send waiting = {0};
    uint8_t small[SMALL_LEN] = {0};
    int new_far = -1;
    int fail = 0;

    fixture_init(&f);
    if (peer_open(&f, NODE_A, true, &a) != 0 ||
        peer_register(&f, &a, PEER_HOST, PEER_PORT) != 0) {
        printf("FAIL %-20s: fixture setup failed\n", name);
        fixture_destroy(&f);
        return 1;
    }
    uint8_t *bulk = g_malloc0(BULK_LEN);

    if (park_sender(&parked, &f, &a, bulk) != 0) {
        printf("FAIL %-20s: the bulk send never started\n", name);
        fail = 1;
    } else {
        lookup_hook_arm();
        if (ionic_send_start(&waiting, &f, NODE_A, small, sizeof(small)) != 0 ||
            !lookup_hook_wait()) {
            printf("FAIL %-20s: the second send never looked node %u up\n",
                   name, NODE_A);
            fail = 1;
        } else if (reconnect(&f, NODE_A, &new_far) != 0) {
            printf("FAIL %-20s: reconnect failed\n", name);
            fail = 1;
        }
    }

    if (fail) {
        unblock_after_failure(&a.far);
    }
    ionic_send_join(&parked);
    ionic_send_join(&waiting);

    if (!fail && parked.rc != -EIO) {
        printf("FAIL %-20s: interrupted bulk send returned %d, expected "
               "-EIO (%d)\n",
               name, parked.rc, -EIO);
        fail = 1;
    }
    if (!fail && waiting.rc != -EIO) {
        printf("FAIL %-20s: send addressed before the reconnect returned "
               "%d, expected -EIO (%d)\n",
               name, waiting.rc, -EIO);
        fail = 1;
    }

    g_free(bulk);
    if (a.far >= 0) {
        close(a.far);
    }
    if (new_far >= 0) {
        close(new_far);
    }
    fixture_destroy(&f);

    if (!fail) {
        printf("PASS %-20s: sends on the replaced connection failed "
               "cleanly\n",
               name);
    }
    return fail;
}

/*
 * A receive thread is relaying to node A when the health check reconnects
 * A. This is the network-facing form of the case above: the manager relays
 * worker-to-worker ionic traffic on the sending worker's receive thread, so
 * here node B's traffic alone drives the waiting user of A's connection.
 *
 * Afterwards B's receive thread must still be alive and serving B: a
 * heartbeat sent behind the relayed message must be answered.
 */
static int test_relay_vs_reconnect(const char *name)
{
    struct mesh_fixture f;
    struct peer a;
    struct peer b;
    struct ionic_send parked = {0};
    uint8_t small[SMALL_LEN] = {0};
    int new_far = -1;
    int fail = 0;

    fixture_init(&f);
    if (peer_open(&f, NODE_A, true, &a) != 0 ||
        peer_register(&f, &a, PEER_HOST, PEER_PORT) != 0) {
        printf("FAIL %-20s: fixture setup failed\n", name);
        fixture_destroy(&f);
        return 1;
    }
    if (peer_open(&f, NODE_B, false, &b) != 0 ||
        peer_register(&f, &b, PEER_HOST, PEER_PORT) != 0) {
        printf("FAIL %-20s: fixture setup failed\n", name);
        close(a.far);
        fixture_destroy(&f);
        return 1;
    }
    uint8_t *bulk = g_malloc0(BULK_LEN);

    if (park_sender(&parked, &f, &a, bulk) != 0) {
        printf("FAIL %-20s: the bulk send never started\n", name);
        fail = 1;
    } else {
        /* Addressed to A, so the manager relays it on B's receive thread. */
        lookup_hook_arm();
        if (send_msg(b.far, TCP_MSG_IONIC, NODE_B, NODE_A, small,
                     sizeof(small)) != 0 ||
            !lookup_hook_wait()) {
            printf("FAIL %-20s: the relay never looked node %u up\n", name,
                   NODE_A);
            fail = 1;
        } else if (reconnect(&f, NODE_A, &new_far) != 0) {
            printf("FAIL %-20s: reconnect failed\n", name);
            fail = 1;
        }
    }

    if (fail) {
        unblock_after_failure(&a.far);
    }
    ionic_send_join(&parked);

    if (!fail && parked.rc != -EIO) {
        printf("FAIL %-20s: interrupted bulk send returned %d, expected "
               "-EIO (%d)\n",
               name, parked.rc, -EIO);
        fail = 1;
    }

    /* B's receive thread drains its socket in order, so the reply to this
     * heartbeat means the relay ahead of it has returned. */
    if (!fail && (send_msg(b.far, TCP_MSG_HEARTBEAT, NODE_B, MANAGER_NODE, NULL,
                           0) != 0 ||
                  read_until(b.far, TCP_MSG_HEARTBEAT_RESP, TCP_MSG_HEARTBEAT,
                             NULL) != 0)) {
        printf("FAIL %-20s: node %u's receive thread stopped answering after "
               "the relay\n",
               name, NODE_B);
        fail = 1;
    }

    g_free(bulk);
    if (a.far >= 0) {
        close(a.far);
    }
    if (new_far >= 0) {
        close(new_far);
    }
    fixture_destroy(&f);
    close(b.far);

    if (!fail) {
        printf("PASS %-20s: relay on the replaced connection failed cleanly\n",
               name);
    }
    return fail;
}

/* ---- Health-check pass ------------------------------------------------- */

/*
 * How long node A has been silent when the pass runs. It must exceed three
 * health-check intervals (one second each here) and be a multiple of the
 * reconnect backoff the pass computes for it, which for 4 s is 4 s.
 */
#define SILENT_SEC 4

/* A loopback TCP listener on an ephemeral port, for the pass to dial. */
static int listener_open(uint16_t *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Accept the connection the pass dialed. Its reads time out rather than
 * block forever. */
static int listener_accept(int lfd)
{
    const struct timeval tmo = {.tv_sec = WAIT_MS / 1000, .tv_usec = 0};

    if (!wait_readable(lfd)) {
        return -1;
    }
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Wait for the wall clock to tick and return the new second. The pass reads
 * time(NULL) once; starting just after a tick leaves it most of a second in
 * which it still reads the value the test set the node up against.
 */
static time_t next_second(void)
{
    const struct timespec tick = {.tv_sec = 0, .tv_nsec = 1000000};
    time_t start = time(NULL);
    time_t now;

    while ((now = time(NULL)) == start) {
        nanosleep(&tick, NULL);
    }
    return now;
}

/*
 * A whole health-check pass over a node that has stopped answering. The pass
 * must declare the node dead and broadcast that before trying to reach it,
 * heartbeat it on the connection it has, dial it again, install the new
 * connection, retire the old one, mark the node alive again, and broadcast
 * the topology on the new connection.
 */
static int test_health_check_reconnect(const char *name)
{
    struct mesh_fixture f;
    struct peer a = {.conn = NULL, .far = -1};
    uint16_t port = 0;
    int acc = -1;
    unsigned handshakes = 0;
    unsigned death_broadcasts = 0;
    int fail = 0;

    fixture_init(&f);
    f.priv.health_check_interval_sec = 1;

    int lfd = listener_open(&port);
    if (lfd < 0 || peer_open(&f, NODE_A, false, &a) != 0 ||
        peer_register(&f, &a, "127.0.0.1", port) != 0) {
        printf("FAIL %-20s: fixture setup failed\n", name);
        if (lfd >= 0) {
            close(lfd);
        }
        if (a.far >= 0) {
            close(a.far);
        }
        fixture_destroy(&f);
        return 1;
    }

    /* Held across the pass so the old connection can still be inspected
     * after it has been replaced. */
    TcpConnection *old_conn = tcp_connection_lookup(&f.priv, NODE_A);

    time_t start = next_second();
    qemu_mutex_lock(&f.priv.mesh_table_lock);
    MeshNodeInfo *node =
        g_hash_table_lookup(f.priv.mesh_nodes, GUINT_TO_POINTER(NODE_A));
    node->last_heartbeat = start - SILENT_SEC;
    qemu_mutex_unlock(&f.priv.mesh_table_lock);

    tcp_health_check_pass(&f.priv);

    TcpConnection *new_conn = tcp_connection_lookup(&f.priv, NODE_A);
    if (!new_conn || new_conn == old_conn ||
        !atomic_load(&new_conn->is_connected)) {
        printf("FAIL %-20s: node %u's connection was not replaced\n", name,
               NODE_A);
        fail = 1;
    } else if (atomic_load(&old_conn->is_connected)) {
        printf("FAIL %-20s: the replaced connection was not retired\n", name);
        fail = 1;
    }

    /* The old connection carried the death broadcast, then the heartbeat,
     * and was then shut down. */
    uint8_t byte;
    if (!fail && read_until(a.far, TCP_MSG_HEARTBEAT, TCP_MSG_MESH_TOPOLOGY,
                            &death_broadcasts) != 0) {
        printf("FAIL %-20s: no heartbeat on the old connection\n", name);
        fail = 1;
    } else if (!fail && death_broadcasts != 1) {
        printf("FAIL %-20s: %u topology broadcasts ahead of the heartbeat on "
               "the old connection, expected 1 announcing the death\n",
               name, death_broadcasts);
        fail = 1;
    } else if (!fail && recv(a.far, &byte, 1, 0) != 0) {
        printf("FAIL %-20s: the old connection's socket was not shut down\n",
               name);
        fail = 1;
    }

    /* The new connection opened with a handshake, and the pass's topology
     * broadcast went out on it. */
    if (!fail) {
        acc = listener_accept(lfd);
        if (acc < 0 ||
            read_until(acc, TCP_MSG_MESH_TOPOLOGY, TCP_MSG_HANDSHAKE,
                       &handshakes) != 0 ||
            handshakes != 1) {
            printf("FAIL %-20s: the new connection did not get a handshake "
                   "and then the topology broadcast\n",
                   name);
            fail = 1;
        }
    }

    qemu_mutex_lock(&f.priv.mesh_table_lock);
    bool alive = node->is_alive && node->last_heartbeat >= start;
    qemu_mutex_unlock(&f.priv.mesh_table_lock);
    if (!fail && !alive) {
        printf("FAIL %-20s: node %u was not marked alive after reconnecting\n",
               name, NODE_A);
        fail = 1;
    }

    uint64_t successes = atomic_load(&f.priv.tcp_stats.reconnect_successes);
    if (!fail && successes != 1) {
        printf("FAIL %-20s: %" PRIu64 " successful reconnects counted, "
               "expected 1\n",
               name, successes);
        fail = 1;
    }

    tcp_connection_unref(new_conn);
    tcp_connection_unref(old_conn);
    fixture_destroy(&f);
    if (acc >= 0) {
        close(acc);
    }
    close(lfd);
    close(a.far);

    if (!fail) {
        printf("PASS %-20s: silent node redialed, old connection retired\n",
               name);
    }
    return fail;
}

/* Remove every table entry for @conn without releasing it; returns how many
 * there were. */
static unsigned steal_entries(struct mesh_fixture *f, TcpConnection *conn)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    unsigned n = 0;

    qemu_mutex_lock(&f->priv.conn_table_lock);
    g_hash_table_iter_init(&iter, f->priv.connections);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (value == conn) {
            g_hash_table_iter_steal(&iter);
            n++;
        }
    }
    qemu_mutex_unlock(&f->priv.conn_table_lock);
    return n;
}

/*
 * A new connection that sends REGISTER_NODE twice must end up under exactly
 * one node ID. Under two, the connection table holds two owning references
 * to one object, so whichever entry is released second -- by a reconnect or
 * by teardown -- releases freed memory.
 */
static int test_double_register(const char *name)
{
    struct mesh_fixture f;
    struct peer p;
    TcpRegisterNodePayload reg;
    unsigned resps = 0;
    int fail = 0;

    fixture_init(&f);

    /* Accepted but not yet registered: in no table, as the manager's accept
     * thread leaves it. */
    if (peer_open(&f, UNASSIGNED_NODE, false, &p) != 0) {
        printf("FAIL %-20s: fixture setup failed\n", name);
        fixture_destroy(&f);
        return 1;
    }

    memset(&reg, 0, sizeof(reg));
    g_strlcpy(reg.hostname, PEER_HOST, sizeof(reg.hostname));
    reg.port = htons(PEER_PORT);
    reg.requested_node_id = htonl(UNASSIGNED_NODE);

    for (int i = 0; i < 2 && !fail; i++) {
        if (send_msg(p.far, TCP_MSG_REGISTER_NODE, UNASSIGNED_NODE,
                     MANAGER_NODE, &reg, sizeof(reg)) != 0) {
            printf("FAIL %-20s: registration %d could not be sent\n", name,
                   i + 1);
            fail = 1;
        }
    }

    /* The heartbeat's reply is the barrier: it proves both registrations
     * have been handled. */
    if (!fail && send_msg(p.far, TCP_MSG_HEARTBEAT, NODE_A, MANAGER_NODE, NULL,
                          0) != 0) {
        printf("FAIL %-20s: heartbeat could not be sent\n", name);
        fail = 1;
    }
    if (!fail && read_until(p.far, TCP_MSG_HEARTBEAT_RESP,
                            TCP_MSG_REGISTER_RESP, &resps) != 0) {
        printf("FAIL %-20s: no reply to the heartbeat\n", name);
        fail = 1;
    }

    /* Stop the receive thread before touching the table it writes to. */
    tcp_connection_retire(p.conn);

    /*
     * Take the connection back out of the table and release it. It carries
     * one reference however many entries share it -- the creation reference,
     * which the first registration handed to the table -- so it is released
     * once, and a failure here is reported rather than turned into a double
     * free at teardown.
     */
    unsigned entries = steal_entries(&f, p.conn);
    if (!fail && entries != 1) {
        printf("FAIL %-20s: connection is in the table under %u node IDs "
               "after %u REGISTER_RESP(s), expected 1\n",
               name, entries, resps);
        fail = 1;
    }
    tcp_connection_unref(p.conn);

    close(p.far);
    fixture_destroy(&f);

    if (!fail) {
        printf("PASS %-20s: second registration did not add an owner\n", name);
    }
    return fail;
}

static const struct {
    const char *name;
    int (*run)(const char *name);
} cases[] = {
    {"send-vs-reconnect", test_send_vs_reconnect},
    {"relay-vs-reconnect", test_relay_vs_reconnect},
    {"health-check-reconnect", test_health_check_reconnect},
    {"double-register", test_double_register},
};

/*
 * With no arguments every case runs. Naming cases runs just those: under
 * ASan the first use-after-free aborts the process, so checking a single
 * case in isolation needs a run of its own.
 */
int main(int argc, char **argv)
{
    int failures = 0;

    for (int i = 1; i < argc; i++) {
        bool known = false;
        for (size_t c = 0; c < G_N_ELEMENTS(cases); c++) {
            known = known || strcmp(argv[i], cases[c].name) == 0;
        }
        if (!known) {
            (void)fprintf(stderr, "unknown case '%s'\n", argv[i]);
            return 2;
        }
    }

    for (size_t c = 0; c < G_N_ELEMENTS(cases); c++) {
        bool selected = argc < 2;
        for (int i = 1; i < argc && !selected; i++) {
            selected = strcmp(argv[i], cases[c].name) == 0;
        }
        if (selected) {
            failures += cases[c].run(cases[c].name);
        }
    }

    if (failures) {
        printf("\n%d test case(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll test cases passed\n");
    return 0;
}
