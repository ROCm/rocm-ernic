/*
 * Unit tests for stopping the TCP backend's threads at teardown.
 *
 * tcp_fini() used to stop each thread by clearing its flag and joining it,
 * and neither the accept nor the health-check thread looked at its flag
 * again soon:
 *
 *   - The listen socket is blocking, so the accept thread sat in accept()
 *     until a peer connected, and the join -- and with it a SIGTERM
 *     shutdown of a worker or manager -- hung until the process was killed.
 *     tcp_stop_accept_thread() now shuts the listen socket down before the
 *     join, which wakes accept() with EINVAL.
 *   - The manager's health-check thread slept out its whole interval
 *     (ERNIC_TCP_HEALTH_INTERVAL) before checking, so the join waited up to
 *     that long.  It now waits on a condition variable that
 *     tcp_stop_health_check_thread() signals.
 *
 * Once those returned, the receive threads were the problem: tcp_fini()
 * freed the mesh table while they could still read it, and never stopped
 * the threads of connections the manager had accepted but not registered.
 * tcp_stop_recv_threads() now stops every receive thread, registered,
 * pending or the worker's manager connection, before anything is freed.
 * A peer can't hold up the join either: a thread waiting for the rest of a
 * payload the peer is withholding gives up when its flag is cleared, and
 * one blocked sending to a peer that has stopped reading is released when
 * the stop shuts the socket down.
 *
 * That shutdown comes too late for a send that blocks while holding a lock
 * the stop needs first.  A broadcast sends under conn_table_lock, which
 * tcp_stop_recv_threads() takes before it shuts anything down, and the
 * health-check thread, joined before any socket is shut down, can be
 * blocked in a heartbeat or waiting for a connection's lock.
 * tcp_stop_threads() therefore sets priv->stopping before any join, and a
 * send that would have to wait gives up instead.  Later rows run
 * tcp_init() and tcp_fini() on a manager with a worker that stops reading.
 *
 * Every stop joins its thread only if the thread's flag is set, so a thread
 * that couldn't be created must leave its flag clear; a join of a thread
 * that doesn't exist is undefined behavior.  The last rows make
 * qemu_thread_create() fail in tcp_init(), in the accept thread, and in a
 * health-check reconnect, after which the retired connection must not be
 * stopped again.
 *
 * Each row starts (or deliberately does not start) the real threads, stops
 * them, and checks that the stop returned promptly, that the threads really
 * exited, and how many joins it took.  The health-check rows also check
 * that the thread still runs once per interval when nobody stops it.  A
 * per-row alarm() turns a hang into a named failure rather than a ctest
 * timeout.  The translation unit is #included to reach the static
 * functions, and its externals are stubbed; the thread entry points are
 * backed by real pthreads.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Pull in the code under test (including its static functions) */
#include "rdma/rdma_backend_tcp.c"

/* ---- Stubs ------------------------------------------------------------- */

RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    (void)dev_res;
    (void)mr_handle;
    return NULL;
}
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

/*
 * Real threads.  The trampoline counts the start routines that returned,
 * so "the join came back" and "the thread actually left its loop" are
 * checked separately.
 */
static atomic_int g_creates;
static atomic_int g_joins;
static atomic_int g_exits;

/*
 * How many more qemu_thread_create() calls succeed before the rest fail, as
 * pthread_create() does when the process is out of threads; -1 for no limit.
 */
static atomic_int g_creates_left = -1;
static atomic_int g_create_failures; /* calls failed by g_creates_left */

struct trampoline {
    void *(*fn)(void *);
    void *arg;
};

static void *trampoline_run(void *opaque)
{
    struct trampoline t = *(struct trampoline *)opaque;
    void *ret;

    free(opaque);
    ret = t.fn(t.arg);
    atomic_fetch_add(&g_exits, 1);
    return ret;
}

int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    struct trampoline *t;
    int left = atomic_load(&g_creates_left);

    (void)name;
    (void)mode;
    while (left > 0 &&
           !atomic_compare_exchange_weak(&g_creates_left, &left, left - 1))
        ;
    if (left == 0) {
        atomic_fetch_add(&g_create_failures, 1);
        return EAGAIN;
    }
    t = malloc(sizeof(*t));
    if (!t)
        return -1;
    t->fn = start_routine;
    t->arg = arg;
    if (pthread_create(&thread->thread, NULL, trampoline_run, t) != 0) {
        free(t);
        return -1;
    }
    atomic_fetch_add(&g_creates, 1);
    return 0;
}
void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}
void qemu_thread_join(QemuThread *thread)
{
    atomic_fetch_add(&g_joins, 1);
    pthread_join(thread->thread, NULL);
}

/* ---- Watchdog ---------------------------------------------------------- */

#define ROW_TIMEOUT_S 10

static const char *volatile g_current_row = "";

_Noreturn static void watchdog(int sig)
{
    static const char msg[] = "FAIL hung stopping a thread: ";

    (void)sig;
    /* write() is async-signal-safe; the row text is a string literal. */
    if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0 ||
        write(STDOUT_FILENO, g_current_row, strlen(g_current_row)) < 0 ||
        write(STDOUT_FILENO, "\n", 1) < 0)
        _exit(1);
    _exit(1);
}

/* ---- Fixture ----------------------------------------------------------- */

/* A blocking listen socket on an ephemeral loopback port. */
static int listen_loopback(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* SO_ACCEPTCONN: still a listening socket, i.e. not shut down. */
static bool still_listening(int fd)
{
    int on = 0;
    socklen_t len = sizeof(on);

    return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &on, &len) == 0 && on;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
}

/* ---- Accept thread ----------------------------------------------------- */

enum setup {
    NOT_STARTED,      /* flag clear, no thread */
    BLOCKED,          /* thread started and given time to block */
    JUST_STARTED,     /* stop issued straight after the thread is created */
    NO_LISTEN_SOCKET, /* thread started with listen_fd -1 (accept fails) */
};

struct stop_case {
    const char *category;
    const char *description;
    const char *expected;
    enum setup setup;
    bool open_listen_fd; /* give priv a listen socket */
    int stops;           /* how many times to call the stop */
    int want_joins;
    bool want_exited;    /* the thread's start routine returned */
    bool want_listening; /* the socket is untouched afterwards */
};

static const struct stop_case cases[] = {
    {"positive", "stop a thread parked in accept()",
     "returns, the thread exits, one join", BLOCKED, true, 1, 1, true, false},
    {"negative", "stop when the thread was never started",
     "a no-op: no join and the listen socket left listening", NOT_STARTED, true,
     1, 0, false, true},
    {"negative", "stop with no thread and no listen socket",
     "a no-op: no join and no shutdown on fd -1", NOT_STARTED, false, 1, 0,
     false, false},
    {"boundary", "stop straight after the thread is created",
     "returns whether or not the thread reached accept() yet", JUST_STARTED,
     true, 1, 1, true, false},
    {"corner", "stop twice", "the second stop is a no-op: still one join",
     BLOCKED, true, 2, 1, true, false},
    {"corner", "stop a thread whose accept() fails (listen_fd -1)",
     "the cleared flag ends the error loop: returns, one join",
     NO_LISTEN_SOCKET, false, 1, 1, true, false},
};

static int run_case(const struct stop_case *c)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    int fd = -1;
    double start, took;
    bool ok;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_init(&priv->accept_thread_running, false);
    priv->listen_fd = -1;

    if (c->open_listen_fd) {
        fd = listen_loopback();
        if (fd < 0) {
            printf("FAIL %s: %s; could not open a loopback listen socket\n",
                   c->category, c->description);
            g_free(priv);
            return 1;
        }
        priv->listen_fd = fd;
    }

    if (c->setup != NOT_STARTED) {
        atomic_store(&priv->accept_thread_running, true);
        qemu_thread_create(&priv->accept_thread, "tcp-accept",
                           tcp_accept_thread, priv, QEMU_THREAD_JOINABLE);
        if (c->setup == BLOCKED)
            sleep_ms(100);
    }

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    start = now_s();
    for (int i = 0; i < c->stops; i++)
        tcp_stop_accept_thread(priv);
    took = now_s() - start;
    alarm(0);

    ok = atomic_load(&g_joins) == c->want_joins &&
         atomic_load(&g_exits) == (c->want_exited ? 1 : 0) &&
         (fd < 0 || still_listening(fd) == c->want_listening) &&
         !atomic_load(&priv->accept_thread_running) && took < 2.0;

    printf("%s accept %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     got %d join(s) (want %d), exited %d (want %d), "
               "listening %d (want %d), %.3f s\n",
               atomic_load(&g_joins), c->want_joins, atomic_load(&g_exits),
               (int)c->want_exited, fd < 0 ? -1 : (still_listening(fd) ? 1 : 0),
               (int)c->want_listening, took);

    if (fd >= 0)
        close(fd);
    g_free(priv);
    return ok ? 0 : 1;
}

/* ---- Health-check thread ----------------------------------------------- */

/*
 * An interval far longer than ROW_TIMEOUT_S: a stop that has to wait it out
 * trips the watchdog instead of passing slowly.
 */
#define LONG_INTERVAL_S 3600

struct health_case {
    const char *category;
    const char *description;
    const char *expected;
    enum setup setup;  /* NOT_STARTED, BLOCKED or JUST_STARTED */
    uint32_t interval; /* health_check_interval_sec */
    bool peer;         /* a connected node the thread sends heartbeats to */
    int stops;         /* how many times to call the stop */
    int want_joins;
    bool want_exited; /* the thread's start routine returned */
};

static const struct health_case health_cases[] = {
    {"positive", "stop a thread waiting out a one-hour interval",
     "returns at once, the thread exits, one join", BLOCKED, LONG_INTERVAL_S,
     false, 1, 1, true},
    {"positive", "a 1 s interval with a connected node, then stop",
     "a heartbeat each pass, at least 0.5 s apart; the stop returns at once",
     BLOCKED, 1, true, 1, 1, true},
    {"negative", "stop when the thread was never started", "a no-op: no join",
     NOT_STARTED, LONG_INTERVAL_S, false, 1, 0, false},
    {"boundary", "stop straight after the thread is created",
     "the stop isn't lost if it lands before the thread starts waiting",
     JUST_STARTED, LONG_INTERVAL_S, false, 1, 1, true},
    {"corner", "stop twice", "the second stop is a no-op: still one join",
     BLOCKED, LONG_INTERVAL_S, false, 2, 1, true},
};

/* Wait up to timeout_ms for one message on fd, and check it's a heartbeat. */
static bool recv_heartbeat(int fd, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    TcpMsgHeader hdr;
    void *payload = NULL;

    if (poll(&pfd, 1, timeout_ms) != 1)
        return false;
    if (tcp_recv_message(fd, &hdr, &payload, NULL, NULL) < 0)
        return false;
    g_free(payload);
    return hdr.msg_type == TCP_MSG_HEARTBEAT;
}

/*
 * Check that the health-check thread waits between passes, sending a
 * heartbeat on each, by finding two heartbeats sent at least 0.5 s apart.
 *
 * The times the heartbeats are read can't show that: a reader scheduled late
 * finds two already queued however far apart they were sent.  Silence can.
 * A heartbeat arrives only after it is sent, so if nothing more arrives for
 * 0.5 s after one is read, the next was sent at least 0.5 s after it.  A late
 * reader that finds the next one already there tries again with that one,
 * within a 5 s budget; a thread that doesn't wait never leaves a gap.
 */
static bool heartbeats_spaced(int fd)
{
    double end = now_s() + 5.0;

    while (now_s() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};

        if (!recv_heartbeat(fd, 5000))
            return false;
        if (poll(&pfd, 1, 500) == 0)
            return recv_heartbeat(fd, 5000);
    }
    return false;
}

static int run_health_case(const struct health_case *c)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    TcpConnection *conn = NULL;
    int sv[2] = {-1, -1};
    double start, took;
    bool ok = true, spaced = false;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_init(&priv->health_check_running, false);
    qemu_mutex_init(&priv->health_check_lock);
    qemu_cond_init(&priv->health_check_cond);
    priv->health_check_interval_sec = c->interval;
    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, mesh_node_info_destroy_notify);
    qemu_mutex_init(&priv->mesh_table_lock);

    if (c->peer) {
        MeshNodeInfo *node = mesh_node_info_new(1, "peer", 0);

        if (!node || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            printf("FAIL health %s: %s; could not set up the peer\n",
                   c->category, c->description);
            mesh_node_info_free(node);
            ok = false;
            goto out;
        }
        conn = tcp_connection_new(1, "peer", 0);
        conn->sockfd = sv[0];
        conn->is_connected = true;
        node->conn = conn;
        node->is_alive = true;
        node->last_heartbeat = time(NULL);
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(1), node);
    }

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (c->setup != NOT_STARTED) {
        atomic_store(&priv->health_check_running, true);
        qemu_thread_create(&priv->health_check_thread, "tcp-health",
                           tcp_manager_health_check_thread, priv,
                           QEMU_THREAD_JOINABLE);
        if (c->setup == BLOCKED)
            sleep_ms(100);
    }

    /* A wait that returned early would send the heartbeats back to back */
    if (c->peer)
        spaced = heartbeats_spaced(sv[1]);

    start = now_s();
    for (int i = 0; i < c->stops; i++)
        tcp_stop_health_check_thread(priv);
    took = now_s() - start;
    alarm(0);

    ok = atomic_load(&g_joins) == c->want_joins &&
         atomic_load(&g_exits) == (c->want_exited ? 1 : 0) &&
         !atomic_load(&priv->health_check_running) && took < 2.0 &&
         (!c->peer || spaced);

    printf("%s health %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     got %d join(s) (want %d), exited %d (want %d), "
               "stop took %.3f s, heartbeats spaced %d\n",
               atomic_load(&g_joins), c->want_joins, atomic_load(&g_exits),
               (int)c->want_exited, took, (int)spaced);

out:
    g_hash_table_destroy(priv->mesh_nodes);
    tcp_connection_free(conn);
    if (sv[1] >= 0)
        close(sv[1]);
    qemu_mutex_destroy(&priv->mesh_table_lock);
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    g_free(priv);
    return ok ? 0 : 1;
}

/* ---- Receive threads --------------------------------------------------- */

/* What each running connection's peer does before the stop. */
enum peer_action {
    PEER_QUIET,         /* nothing */
    PEER_WITHHOLDS,     /* announces a payload and sends half of it */
    PEER_STOPS_READING, /* asks for a reply and never reads it */
};

struct recv_case {
    const char *category;
    const char *description;
    const char *expected;
    uint32_t table;       /* registered connections, threads running */
    uint32_t pending;     /* pending connections, threads running */
    bool manager;         /* a manager connection, thread running */
    bool first_exited;    /* the first table thread returns before the stop */
    bool one_not_started; /* plus a table connection with no thread */
    enum peer_action peer;
    int stops; /* how many times to call the stop */
    int want_joins;
};

static const struct recv_case recv_cases[] = {
    {"positive", "two registered, one pending and a manager connection",
     "every thread is stopped and joined once", 2, 1, true, false, false,
     PEER_QUIET, 1, 4},
    {"negative", "no connections at all", "a no-op: no join", 0, 0, false,
     false, false, PEER_QUIET, 1, 0},
    {"negative",
     "registered and pending peers that send half a payload and stop",
     "the stop ends the wait for the rest: both joined once", 1, 1, false,
     false, false, PEER_WITHHOLDS, 1, 2},
    {"negative",
     "registered and pending peers that never read the manager's replies",
     "the stop fails the blocked sends: both joined once", 1, 1, false, false,
     false, PEER_STOPS_READING, 1, 2},
    {"boundary", "a thread that already returned when its peer hung up",
     "still joined, once, along with the running one", 2, 0, false, true, false,
     PEER_QUIET, 1, 2},
    {"corner", "a connection whose thread was never started",
     "not joined; the running one is", 1, 0, false, false, true, PEER_QUIET, 1,
     1},
    {"corner", "stop twice", "the second stop is a no-op: no more joins", 1, 1,
     false, false, false, PEER_QUIET, 2, 2},
};

#define MAX_RECV_CONNS 8

/* A connection on one end of a socketpair; *peer gets the other end. */
static TcpConnection *recv_conn_new(TcpBackendPrivate *priv, uint32_t id,
                                    bool start, int *peer)
{
    TcpConnection *conn;
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return NULL;
    conn = tcp_connection_new(id, "peer", 0);
    conn->priv = priv;
    conn->sockfd = sv[0];
    conn->is_connected = true;
    /* Non-blocking, as the backend makes every connection's socket. */
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    *peer = sv[1];
    if (start) {
        atomic_store(&conn->recv_thread_running, true);
        qemu_thread_create(&conn->recv_thread, "tcp-recv",
                           tcp_recv_thread_per_conn, conn,
                           QEMU_THREAD_JOINABLE);
    }
    return conn;
}

/*
 * Send a header announcing a payload of len bytes, followed by the first
 * sent bytes of it, all zero.
 */
static bool send_message(int fd, TcpMsgType type, uint32_t len, uint32_t sent)
{
    struct {
        TcpMsgHeader hdr;
        uint8_t payload[TCP_REGISTER_NODE_LEN_V3];
    } msg;
    size_t size = sizeof(msg.hdr) + sent;

    if (sent > sizeof(msg.payload))
        return false;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(type);
    msg.hdr.msg_len = htonl(len);
    return send(fd, &msg, size, 0) == (ssize_t)size;
}

/*
 * Make the peer do its part.  PEER_WITHHOLDS leaves the thread in
 * tcp_recv_message() waiting for the rest of a payload.  PEER_STOPS_READING
 * fills the connection's send buffer, as a peer that stopped reading
 * eventually would, then sends a registration from a pre-v4 node; the
 * manager's refusal can't be sent, and the thread waits in the send loop.
 */
static bool peer_act(TcpConnection *conn, int peer, enum peer_action action)
{
    static const uint8_t fill[4096];

    switch (action) {
    case PEER_QUIET:
        return true;
    case PEER_WITHHOLDS:
        return send_message(peer, TCP_MSG_HEARTBEAT, 64, 32);
    case PEER_STOPS_READING:
        while (send(conn->sockfd, fill, sizeof(fill), 0) > 0)
            ;
        return errno == EAGAIN &&
               send_message(peer, TCP_MSG_REGISTER_NODE,
                            TCP_REGISTER_NODE_LEN_V3, TCP_REGISTER_NODE_LEN_V3);
    default:
        return false;
    }
}

static bool wait_for_exits(int n)
{
    for (int i = 0; i < 500 && atomic_load(&g_exits) < n; i++)
        sleep_ms(10);
    return atomic_load(&g_exits) >= n;
}

static int run_recv_case(const struct recv_case *c)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    TcpConnection *all[MAX_RECV_CONNS];
    int peers[MAX_RECV_CONNS], n = 0, started = 0;
    /* The registered ones first, then a manager connection, then pending. */
    uint32_t in_table = c->table + (c->one_not_started ? 1u : 0u);
    uint32_t total = in_table + (c->manager ? 1u : 0u) + c->pending;
    double start, took;
    bool ok = true, flags_clear = true;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    qemu_mutex_init(&priv->conn_table_lock);
    tcp_bufpool_init(&priv->recv_pool);
    /* So a registration gets the manager's refusal */
    priv->is_manager = true;
    atomic_init(&priv->next_seq, 1);

    for (uint32_t i = 0; i < total && i < MAX_RECV_CONNS; i++) {
        bool not_started = c->one_not_started && i == 0;
        TcpConnection *conn =
            recv_conn_new(priv, i + 1, !not_started, &peers[n]);

        if (!conn) {
            printf("FAIL recv %s: %s; could not set up a connection\n",
                   c->category, c->description);
            ok = false;
            break;
        }
        all[n++] = conn;
        started += not_started ? 0 : 1;
        if (i < in_table)
            g_hash_table_insert(priv->connections, GUINT_TO_POINTER(i + 1),
                                conn);
        else if (c->manager && !priv->manager_conn)
            priv->manager_conn = conn;
        else
            g_ptr_array_add(priv->pending_conns, conn);
    }

    if (ok && c->peer != PEER_QUIET) {
        for (int i = 0; i < n; i++) {
            if (!peer_act(all[i], peers[i], c->peer)) {
                printf("FAIL recv %s: %s; the peer could not do its part\n",
                       c->category, c->description);
                ok = false;
                break;
            }
        }
        sleep_ms(200); /* let the threads block */
    }

    if (ok && c->first_exited) {
        /* The first running table connection's peer hangs up. */
        close(peers[0]);
        peers[0] = -1;
        if (!wait_for_exits(1)) {
            printf("FAIL recv %s: %s; the thread never saw the hang-up\n",
                   c->category, c->description);
            ok = false;
        }
    }

    if (ok) {
        g_current_row = c->description;
        alarm(ROW_TIMEOUT_S);
        start = now_s();
        for (int i = 0; i < c->stops; i++)
            tcp_stop_recv_threads(priv);
        took = now_s() - start;
        alarm(0);

        for (int i = 0; i < n; i++)
            if (atomic_load(&all[i]->recv_thread_running))
                flags_clear = false;
        ok = atomic_load(&g_joins) == c->want_joins &&
             atomic_load(&g_exits) == started && flags_clear && took < 2.0;

        printf("%s recv %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
               c->category, c->description, c->expected);
        if (!ok)
            printf("     got %d join(s) (want %d), %d exited (want %d), "
                   "flags clear %d, %.3f s\n",
                   atomic_load(&g_joins), c->want_joins, atomic_load(&g_exits),
                   started, (int)flags_clear, took);
    }

    /* tcp_connection_free() would join a thread still flagged running. */
    if (!ok)
        tcp_stop_recv_threads(priv);
    g_hash_table_destroy(priv->connections);
    tcp_free_pending_conns(priv);
    tcp_connection_free(priv->manager_conn);
    for (int i = 0; i < n; i++)
        if (peers[i] >= 0)
            close(peers[i]);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->conn_table_lock);
    g_free(priv);
    return ok ? 0 : 1;
}

/* ---- The whole backend ------------------------------------------------- */

/*
 * tcp_init() as a manager, with workers that are plain sockets in this
 * process, then tcp_fini().  A worker that stops reading leaves a manager
 * thread blocked sending to it, holding that connection's lock and, in a
 * broadcast, conn_table_lock as well.  tcp_fini() has to return anyway.
 *
 * The workers are attached over socketpairs rather than accepted over TCP.
 * A TCP socket whose peer has stopped reading doesn't refuse every send:
 * the kernel grows its buffer for a while, and appends a small send to a
 * segment already queued, so whether a given heartbeat blocks is up to the
 * kernel.  A Unix stream socket that is full stays full until the peer
 * reads.  The backend handles either the same way.
 */

/* Where the manager is left blocked when tcp_fini() is called. */
enum mgr_block {
    BLOCK_NONE,      /* nowhere: the workers' sockets have room */
    BLOCK_HEARTBEAT, /* the health check, on worker 1's connection */
    BLOCK_BROADCAST, /* worker 2's registration broadcast, to worker 1 */
};

struct fini_case {
    const char *category;
    const char *description;
    const char *expected;
    enum mgr_block block;
    const char *interval; /* ERNIC_TCP_HEALTH_INTERVAL */
};

static const struct fini_case fini_cases[] = {
    {"positive", "a manager with two registered workers",
     "tcp_fini() returns and every thread exits", BLOCK_NONE, "3600"},
    {"negative", "a worker that stops reading the manager's heartbeats",
     "the blocked heartbeat gives up, so the health-check join returns",
     BLOCK_HEARTBEAT, "1"},
    {"negative", "a worker that stops reading while another registers",
     "the broadcast blocked under conn_table_lock gives up, so the receive "
     "threads can be stopped",
     BLOCK_BROADCAST, "3600"},
};

/* A TCP port that is free on every address, as the manager binds. */
static uint16_t free_port(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    uint16_t port = 0;

    if (fd < 0)
        return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
        port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

/*
 * Attach a worker to the manager, as tcp_accept_thread() does with a
 * connection it accepts, and register it.  Returns the worker's end of the
 * socketpair.  The worker never reads from it; what the manager sends during
 * these rows fits in the socket's buffer unless fill_conn() fills it first.
 */
static int worker_register(TcpBackendPrivate *priv)
{
    TcpConnection *conn;
    TcpMsgHeader hdr;
    TcpRegisterNodePayload reg;
    uint8_t buf[sizeof(hdr) + sizeof(reg)];
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    conn = tcp_connection_new(UINT32_MAX, "worker", 0);
    conn->sockfd = sv[0];
    conn->is_connected = true;
    conn->priv = priv;
    qemu_mutex_lock(&priv->conn_table_lock);
    g_ptr_array_add(priv->pending_conns, conn);
    atomic_store(&conn->recv_thread_running, true);
    qemu_thread_create(&conn->recv_thread, "tcp-recv-pending",
                       tcp_recv_thread_per_conn, conn, QEMU_THREAD_JOINABLE);
    qemu_mutex_unlock(&priv->conn_table_lock);

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(TCP_MSG_REGISTER_NODE);
    hdr.msg_len = htonl((uint32_t)sizeof(reg));
    memset(&reg, 0, sizeof(reg));
    /* Nothing listens on port 1, so a health-check reconnect fails at once */
    snprintf(reg.hostname, sizeof(reg.hostname), "127.0.0.1");
    reg.port = htons(1);
    reg.requested_node_id = htonl(UINT32_MAX);
    reg.version = htonl(TCP_PROTOCOL_VERSION);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &reg, sizeof(reg));

    /* On failure the connection stays pending, for tcp_fini() to free */
    if (send(sv[1], buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) {
        close(sv[1]);
        return -1;
    }
    return sv[1];
}

/* Wait up to 5 s for the manager to have n registered connections. */
static bool wait_for_registered(TcpBackendPrivate *priv, guint n)
{
    for (int i = 0; i < 500; i++) {
        guint have;

        qemu_mutex_lock(&priv->conn_table_lock);
        have = g_hash_table_size(priv->connections);
        qemu_mutex_unlock(&priv->conn_table_lock);
        if (have >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/*
 * Fill the manager's socket to a worker, as a worker that has stopped
 * reading eventually would.  Under conn->lock, like every other writer.
 */
static bool fill_conn(TcpBackendPrivate *priv, uint32_t node_id)
{
    static const uint8_t fill[4096];
    TcpConnection *conn = tcp_get_connection(priv, node_id);
    bool full;

    if (!conn)
        return false;
    qemu_mutex_lock(&conn->lock);
    while (send(conn->sockfd, fill, sizeof(fill), MSG_NOSIGNAL) > 0)
        ;
    full = errno == EAGAIN;
    qemu_mutex_unlock(&conn->lock);
    return full;
}

/*
 * Wait up to 5 s for another thread to hold lock for 100 ms straight, which
 * a send that has room never does.
 */
static bool held_elsewhere(QemuMutex *lock)
{
    uint32_t busy = 0;

    for (uint32_t i = 0; i < 500 && busy < 10; i++) {
        if (qemu_mutex_trylock(lock) == 0) {
            qemu_mutex_unlock(lock);
            busy = 0;
        } else {
            busy++;
        }
        sleep_ms(10);
    }
    return busy >= 10;
}

static int run_fini_case(const struct fini_case *c)
{
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    char config[64];
    const char *why = NULL;
    int w1 = -1, w2 = -1;
    uint16_t port = free_port();
    double start, took = 0.0;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", c->interval, 1);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port == 0 || tcp_init(&dev, config) != 0) {
        printf("FAIL fini %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;

    w1 = worker_register(priv);
    if (w1 < 0 || !wait_for_registered(priv, 1)) {
        why = "worker 1 did not register";
        goto stop;
    }
    /* Let worker 1's registration finish sending to it, while there's room */
    sleep_ms(200);

    if (c->block != BLOCK_NONE && !fill_conn(priv, 1)) {
        why = "could not fill worker 1's socket";
        goto stop;
    }
    if (c->block == BLOCK_HEARTBEAT) {
        TcpConnection *conn = tcp_get_connection(priv, 1);

        if (!conn)
            why = "worker 1's connection is gone";
        else if (!held_elsewhere(&conn->lock))
            why = "the health check never blocked on worker 1";
        goto stop;
    }

    w2 = worker_register(priv);
    if (w2 < 0) {
        why = "could not attach worker 2";
        goto stop;
    }
    if (c->block == BLOCK_BROADCAST) {
        /* Not wait_for_registered(): the blocked broadcast holds its lock */
        if (!held_elsewhere(&priv->conn_table_lock))
            why = "worker 2's broadcast never blocked on worker 1";
    } else if (!wait_for_registered(priv, 2)) {
        why = "worker 2 did not register";
    }

stop:
    start = now_s();
    tcp_fini(&dev);
    took = now_s() - start;
    alarm(0);

    ok = !why && took < 2.0 &&
         atomic_load(&g_exits) == atomic_load(&g_creates) &&
         !dev.backend_private;

    printf("%s fini %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok)
        printf("     %s%s%d of %d thread(s) exited, tcp_fini() took %.3f s\n",
               why ? why : "", why ? "; " : "", atomic_load(&g_exits),
               atomic_load(&g_creates), took);

    if (w1 >= 0)
        close(w1);
    if (w2 >= 0)
        close(w2);
    return ok ? 0 : 1;
}

/* ---- Threads that can't be created ------------------------------------- */

/*
 * A thread that couldn't be created must not be joined: its stop flag has to
 * be left clear, and a receive thread's connection freed rather than kept.
 */

enum create_fail {
    FAIL_IN_INIT,   /* tcp_init() starting the manager's threads */
    FAIL_ON_ACCEPT, /* the accept thread starting a connection's thread */
};

struct create_case {
    const char *category;
    const char *description;
    const char *expected;
    enum create_fail when;
    int creates_left; /* g_creates_left from then on */
    bool want_init;   /* tcp_init() succeeds */
};

static const struct create_case create_cases[] = {
    {"negative", "the manager's accept thread can't be created",
     "tcp_init() fails and joins nothing", FAIL_IN_INIT, 0, false},
    {"negative", "the manager's health-check thread can't be created",
     "tcp_init() fails and joins only the accept thread", FAIL_IN_INIT, 1,
     false},
    {"negative", "an accepted connection's receive thread can't be created",
     "the connection is closed, not left pending, and tcp_fini() joins only "
     "the threads that exist",
     FAIL_ON_ACCEPT, 0, true},
};

/* A blocking TCP connection to the loopback port, or -1. */
static int connect_loopback(uint16_t port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Wait up to 5 s for the other end of fd to close it, sending nothing. */
static bool closed_by_peer(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    char c;

    return poll(&pfd, 1, 5000) == 1 && recv(fd, &c, 1, 0) == 0;
}

static int run_create_case(const struct create_case *c)
{
    RdmaBackendDev dev;
    char config[64];
    const char *why = NULL;
    uint16_t port = free_port();
    int init_ret = -1, fd = -1;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", "3600", 1);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port == 0) {
        why = "no free TCP port";
        goto out;
    }
    if (c->when == FAIL_IN_INIT)
        atomic_store(&g_creates_left, c->creates_left);
    init_ret = tcp_init(&dev, config);

    if (c->when == FAIL_ON_ACCEPT && init_ret == 0) {
        TcpBackendPrivate *priv = dev.backend_private;
        guint pending;

        atomic_store(&g_creates_left, c->creates_left);
        fd = connect_loopback(port);
        if (fd < 0 || !closed_by_peer(fd)) {
            why = "the manager didn't close the connection";
        } else {
            qemu_mutex_lock(&priv->conn_table_lock);
            pending = priv->pending_conns->len;
            qemu_mutex_unlock(&priv->conn_table_lock);
            if (pending != 0)
                why = "the connection was left pending";
        }
    }
    atomic_store(&g_creates_left, -1);
    if (init_ret == 0)
        tcp_fini(&dev);

out:
    alarm(0);
    atomic_store(&g_creates_left, -1);
    ok = !why && (init_ret == 0) == c->want_init &&
         atomic_load(&g_joins) == atomic_load(&g_creates) &&
         atomic_load(&g_exits) == atomic_load(&g_creates);

    printf("%s create %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s%stcp_init() returned %d, %d thread(s) created, %d "
               "joined, %d exited\n",
               why ? why : "", why ? "; " : "", init_ret,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));

    if (fd >= 0)
        close(fd);
    return ok ? 0 : 1;
}

/* ---- Health-check reconnect -------------------------------------------- */

/*
 * The health check reconnects to a node that has stopped answering: it
 * retires the node's old connection, joining its receive thread, and opens a
 * new one.  When the new connection's receive thread can't be created, the
 * retired connection must not be left on the node, or the next attempt would
 * join its thread a second time.
 *
 * The node is made to look 4 s dead, so the thread's first pass, on a 1 s
 * interval, attempts a reconnect; attempts back off to whole powers of two
 * seconds, so the next would be at 8 s.  The target is a listening socket
 * here, where the kernel completes the connect.
 */
static int run_reconnect_case(void)
{
    static const char desc[] =
        "a reconnect whose receive thread can't be created, then a retry";
    static const char expected[] =
        "the old connection is retired, and its thread joined, once";
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    TcpConnection *old = NULL;
    MeshNodeInfo *node = NULL;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    int peer = -1, lfd = listen_loopback();
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_store(&g_create_failures, 0);
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->health_check_running, false);
    qemu_mutex_init(&priv->health_check_lock);
    qemu_cond_init(&priv->health_check_cond);
    priv->health_check_interval_sec = 1;
    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, mesh_node_info_destroy_notify);
    qemu_mutex_init(&priv->mesh_table_lock);
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    qemu_mutex_init(&priv->conn_table_lock);
    tcp_bufpool_init(&priv->recv_pool);

    g_current_row = desc;
    alarm(ROW_TIMEOUT_S);
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
        why = "could not open a listening socket";
    } else if (!(old = recv_conn_new(priv, 1, true, &peer))) {
        why = "could not set up the old connection";
    } else {
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(1), old);
        node = mesh_node_info_new(1, "127.0.0.1", ntohs(addr.sin_port));
        if (!node)
            why = "could not create the mesh node";
    }

    if (!why) {
        TcpConnection *on_node, *in_table;
        time_t t0 = time(NULL);
        int fd;

        node->conn = old;
        node->is_alive = false; /* already reported dead: no broadcast */
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(1), node);

        /* Start just after the second ticks over, so the first pass sees 4 s */
        while (time(NULL) == t0)
            sleep_ms(5);
        node->last_heartbeat = time(NULL) - 4;
        atomic_store(&g_creates_left, 1); /* the health thread; no more */
        atomic_store(&priv->health_check_running, true);
        qemu_thread_create(&priv->health_check_thread, "tcp-health",
                           tcp_manager_health_check_thread, priv,
                           QEMU_THREAD_JOINABLE);

        for (uint32_t i = 0; i < 600 && atomic_load(&g_create_failures) == 0;
             i++)
            sleep_ms(10);

        /* The attempt holds mesh_table_lock until it has given up */
        qemu_mutex_lock(&priv->mesh_table_lock);
        on_node = node->conn;
        qemu_mutex_unlock(&priv->mesh_table_lock);
        qemu_mutex_lock(&priv->conn_table_lock);
        in_table = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(1));
        qemu_mutex_unlock(&priv->conn_table_lock);
        qemu_mutex_lock(&old->lock);
        fd = old->sockfd;
        qemu_mutex_unlock(&old->lock);

        if (atomic_load(&g_create_failures) == 0)
            why = "no reconnect was attempted";
        else if (on_node)
            why = "the node still points at its retired connection";
        else if (in_table != old)
            why = "the retired connection left the connections table";
        else if (fd != -1 || atomic_load(&old->recv_thread_running))
            why = "the old connection wasn't retired";
        else if (atomic_load(&g_joins) != 1)
            why = "the old receive thread wasn't joined exactly once";

        tcp_stop_health_check_thread(priv);
        /* What a retry that still found the retired connection would do */
        tcp_retire_conn(old);
    }
    alarm(0);
    atomic_store(&g_creates_left, -1);

    /* The old receive thread and the health-check thread, each once */
    ok = !why && atomic_load(&g_joins) == 2 && atomic_load(&g_exits) == 2;

    printf("%s reconnect negative: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           desc, expected);
    if (!ok)
        printf("     %s%s%d join(s) (want 2), %d exited (want 2)\n",
               why ? why : "", why ? "; " : "", atomic_load(&g_joins),
               atomic_load(&g_exits));

    g_hash_table_destroy(priv->mesh_nodes);
    g_hash_table_destroy(priv->connections);
    if (peer >= 0)
        close(peer);
    if (lfd >= 0)
        close(lfd);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->mesh_table_lock);
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    g_free(priv);
    return ok ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    signal(SIGALRM, watchdog);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failures += run_case(&cases[i]);
    for (size_t i = 0; i < sizeof(health_cases) / sizeof(health_cases[0]); i++)
        failures += run_health_case(&health_cases[i]);
    for (size_t i = 0; i < sizeof(recv_cases) / sizeof(recv_cases[0]); i++)
        failures += run_recv_case(&recv_cases[i]);
    for (size_t i = 0; i < sizeof(fini_cases) / sizeof(fini_cases[0]); i++)
        failures += run_fini_case(&fini_cases[i]);
    for (size_t i = 0; i < sizeof(create_cases) / sizeof(create_cases[0]); i++)
        failures += run_create_case(&create_cases[i]);
    failures += run_reconnect_case();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
