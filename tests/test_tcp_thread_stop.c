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
 * The health check must not hold the mesh table's lock while it reconnects:
 * the old connection's receive thread, which the reconnect joins, takes it
 * to record a heartbeat.  The reconnect rows connect to a node that doesn't
 * answer, and check that the lock is free and a heartbeat is answered
 * meanwhile, that a stop or the interval ends the wait, that a node that
 * answers meanwhile keeps its connection, and that a pass's reconnects
 * together take no more than an interval.  The connect rows check
 * tcp_connect_to_remote(), which waits for the connect, on its own,
 * including a deadline shared by several addresses.
 *
 * A worker's accept thread waits for a handshake from each peer that
 * connects, and has to stop waiting when it is stopped.  The handshake rows
 * check that, and also send it handshakes it has to refuse.
 *
 * A manager must register a connection only once, or teardown frees it
 * once for each time.  The registration rows send a second REGISTER_NODE
 * on the same connection.
 *
 * A worker must not hold the connection table's lock while it connects to
 * the peers in a topology update, since the data path takes it.  The
 * topology rows name peers that don't answer, and check that the lock is
 * free meanwhile, that a stop or the interval ends the wait, and that a
 * connection the peer opened meanwhile is kept.
 *
 * A message's header can arrive in parts, and tcp_recv_message() has to wait
 * for the rest rather than lose the part it has read.  The partial rows split
 * messages at several points and check what is read, and that a stop, a
 * deadline, or a hang-up ends the wait.
 *
 * A manager has to close the connections it is done with: a peer's that hung
 * up, was refused, or sent something else before registering, or never
 * registered, and a registered worker's that hung up.  The reap rows connect
 * such peers, check that the connections are closed and their threads
 * joined, that only so many may wait to register, and that a failed accept()
 * isn't retried in a tight loop.
 *
 * The node a message's header names as its sender is whatever the sender
 * wrote.  The sender rows name another node in a heartbeat and in data-path
 * requests, and check that the heartbeat is credited, and the answer sent,
 * by connection instead, and that data that waits for a receive is completed
 * on the connection it came on.  They also check that a dead node that
 * answers a heartbeat again is reported alive to the rest of the mesh.
 *
 * A connection is reference counted.  A thread that looked one up used to
 * get a bare pointer, and the health check's reconnect freed the old
 * connection while the thread could still be using it.  The lifetime rows
 * reconnect while the test holds a reference to the old connection, and
 * while another thread is blocked sending on it, and have a pending
 * connection register while teardown stops its receive thread.
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
#include <dirent.h>
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
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef ERNIC_HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

/* Pull in the code under test (including its static functions) */
#include "rdma/rdma_backend_tcp.c"

/* ---- Stubs ------------------------------------------------------------- */

/* The one memory region there is, while a row sets it; see the sender rows */
static _Atomic(RdmaRmMR *) g_mr;

RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    RdmaRmMR *mr = atomic_load(&g_mr);

    (void)dev_res;
    return mr && mr->rkey == mr_handle ? mr : NULL;
}

static atomic_int g_errors; /* error_report() calls */

void error_report(const char *fmt, ...)
{
    (void)fmt;
    atomic_fetch_add(&g_errors, 1);
}
void warn_report(const char *fmt, ...)
{
    (void)fmt;
}
void info_report(const char *fmt, ...)
{
    (void)fmt;
}

/*
 * DMA mappings, which only a work request through a DMA memory region makes
 * (see the sender rows): each pci_dma_map() hands out the next slot, and
 * pci_dma_unmap() counts the unmaps of each.  The rows map only from the
 * test's own thread, but a receive thread may unmap.
 */
#define DMA_SLOTS    4
#define DMA_SLOT_LEN 64u

static uint8_t g_dma_mem[DMA_SLOTS][DMA_SLOT_LEN];
static atomic_int g_dma_maps;
static atomic_int g_dma_unmaps[DMA_SLOTS];
static atomic_int g_dma_stray_unmaps; /* of an address that isn't a slot */

static void dma_reset(void)
{
    atomic_store(&g_dma_maps, 0);
    for (int i = 0; i < DMA_SLOTS; i++)
        atomic_store(&g_dma_unmaps[i], 0);
    atomic_store(&g_dma_stray_unmaps, 0);
}

/* Whether every mapping handed out has been unmapped once, and only once. */
static bool dma_unmapped_once(void)
{
    for (int i = 0; i < atomic_load(&g_dma_maps); i++)
        if (atomic_load(&g_dma_unmaps[i]) != 1)
            return false;
    return atomic_load(&g_dma_stray_unmaps) == 0;
}

void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    int slot = atomic_load(&g_dma_maps);

    (void)dev;
    (void)addr;
    (void)dir;
    if (slot >= DMA_SLOTS || *len > DMA_SLOT_LEN)
        return NULL;
    atomic_store(&g_dma_maps, slot + 1);
    return g_dma_mem[slot];
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)len;
    (void)dir;
    (void)access_len;
    for (int i = 0; i < DMA_SLOTS; i++) {
        if (buffer == g_dma_mem[i]) {
            atomic_fetch_add(&g_dma_unmaps[i], 1);
            return;
        }
    }
    atomic_fetch_add(&g_dma_stray_unmaps, 1);
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

/* Wait up to timeout_ms for one message on fd, and check its type. */
static bool recv_type(int fd, TcpMsgType type, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    TcpMsgHeader hdr;
    void *payload = NULL;

    if (poll(&pfd, 1, timeout_ms) != 1)
        return false;
    if (tcp_recv_message(fd, &hdr, &payload, NULL, NULL, 0) < 0)
        return false;
    g_free(payload);
    return hdr.msg_type == type;
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

        if (!recv_type(fd, TCP_MSG_HEARTBEAT, 5000))
            return false;
        if (poll(&pfd, 1, 500) == 0)
            return recv_type(fd, TCP_MSG_HEARTBEAT, 5000);
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
    /* Each pass reaps the pending connections, of which there are none */
    priv->pending_conns = g_ptr_array_new();
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    qemu_mutex_init(&priv->conn_table_lock);

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
        atomic_store(&conn->is_connected, true);
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(1), conn);
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
    g_hash_table_destroy(priv->connections);
    if (sv[1] >= 0)
        close(sv[1]);
    g_ptr_array_free(priv->pending_conns, TRUE);
    qemu_mutex_destroy(&priv->conn_table_lock);
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
    atomic_store(&conn->is_connected, true);
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

    /* Freeing would join a thread still flagged running, and complain. */
    if (!ok)
        tcp_stop_recv_threads(priv);
    g_hash_table_destroy(priv->connections);
    tcp_free_pending_conns(priv);
    tcp_connection_unref(priv->manager_conn);
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
 * connection it accepts, without registering it.  Returns the worker's end
 * of the socketpair, and the manager's connection in *connp if it isn't NULL.
 */
static int worker_attach(TcpBackendPrivate *priv, TcpConnection **connp)
{
    TcpConnection *conn;
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    conn = tcp_connection_new(UINT32_MAX, "worker", 0);
    conn->sockfd = sv[0];
    atomic_store(&conn->is_connected, true);
    conn->priv = priv;
    /* Or a reap would find it past the registration timeout */
    conn->accepted_ms = tcp_monotonic_ms();
    qemu_mutex_lock(&priv->conn_table_lock);
    g_ptr_array_add(priv->pending_conns, conn);
    atomic_store(&conn->recv_thread_running, true);
    qemu_thread_create(&conn->recv_thread, "tcp-recv-pending",
                       tcp_recv_thread_per_conn, conn, QEMU_THREAD_JOINABLE);
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (connp)
        *connp = conn;
    return sv[1];
}

/* Send a v4 REGISTER_NODE for hostname, asking for node id requested. */
static bool send_register(int fd, const char *hostname, uint32_t requested)
{
    TcpMsgHeader hdr;
    TcpRegisterNodePayload reg;
    uint8_t buf[sizeof(hdr) + sizeof(reg)];

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(TCP_MSG_REGISTER_NODE);
    hdr.msg_len = htonl((uint32_t)sizeof(reg));
    memset(&reg, 0, sizeof(reg));
    snprintf(reg.hostname, sizeof(reg.hostname), "%s", hostname);
    /* Nothing listens on port 1, so a health-check reconnect fails at once */
    reg.port = htons(1);
    reg.requested_node_id = htonl(requested);
    reg.version = htonl(TCP_PROTOCOL_VERSION);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &reg, sizeof(reg));
    return send(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf);
}

/*
 * Attach a worker and register it.  Returns the worker's end of the
 * socketpair.  The worker never reads from it; what the manager sends during
 * these rows fits in the socket's buffer unless fill_conn() fills it first.
 */
static int worker_register(TcpBackendPrivate *priv)
{
    int fd = worker_attach(priv, NULL);

    /* On failure the connection stays pending, for tcp_fini() to free */
    if (fd >= 0 && !send_register(fd, "127.0.0.1", UINT32_MAX)) {
        close(fd);
        return -1;
    }
    return fd;
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
    TcpConnection *conn = tcp_connection_lookup(priv, node_id);
    bool full;

    if (!conn)
        return false;
    qemu_mutex_lock(&conn->lock);
    while (send(conn->sockfd, fill, sizeof(fill), MSG_NOSIGNAL) > 0)
        ;
    full = errno == EAGAIN;
    qemu_mutex_unlock(&conn->lock);
    tcp_connection_unref(conn);
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
        TcpConnection *conn = tcp_connection_lookup(priv, 1);

        if (!conn)
            why = "worker 1's connection is gone";
        else if (!held_elsewhere(&conn->lock))
            why = "the health check never blocked on worker 1";
        tcp_connection_unref(conn);
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

/* Wait up to timeout_ms for fd's other end to close it, sending nothing. */
static bool closed_within(int fd, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    char c;

    return poll(&pfd, 1, timeout_ms) == 1 && recv(fd, &c, 1, 0) == 0;
}

static bool closed_by_peer(int fd)
{
    return closed_within(fd, 5000);
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

/* ---- A worker's handshake ---------------------------------------------- */

/*
 * A worker's accept thread reads a handshake from each peer that connects
 * before it gives the connection a receive thread.  It waits up to 5 s for
 * one, and used to wait all of it even when stopped, so a peer that
 * connected and sent nothing held up shutdown.  The other rows send what the
 * thread must refuse.  It read the node id and version from a handshake
 * whatever its length: through a NULL pointer when there was no payload,
 * past the end of a short one.  And after refusing a message of another
 * type, it kept the freed payload's pointer and freed it again when the next
 * peer hung up.  After each, the thread must still accept a good handshake.
 *
 * A handshake can also arrive in parts.  The thread used to give up on a
 * header that had only partly arrived, losing the bytes it had read, and
 * then read the rest as a header of its own and refuse it.  It now waits for
 * the rest, until it is stopped or the handshake's deadline passes.
 */

#define HS_NODE 7 /* the node id the test's peers claim */

/* How much of a header goes first when a row splits one, and the pause */
#define HS_SPLIT_AT 10
#define HS_SPLIT_MS 50

enum hs_peer {
    HS_GOOD,         /* only the good handshake */
    HS_SILENT,       /* connects and sends nothing */
    HS_PARTIAL,      /* sends part of a handshake's header, then nothing */
    HS_SPLIT,        /* a good handshake whose header comes in two parts */
    HS_EMPTY,        /* a handshake with no payload */
    HS_SHORT,        /* a handshake one byte short */
    HS_OTHER_HANGUP, /* another message type, then a peer that hangs up */
};

struct hs_case {
    const char *category;
    const char *description;
    const char *expected;
    enum hs_peer peer;
};

static const struct hs_case hs_cases[] = {
    {"positive", "a peer that sends a handshake",
     "answered and given a receive thread; both threads joined once", HS_GOOD},
    {"negative", "a peer that connects and sends nothing",
     "the stop doesn't wait out the handshake timeout: one join, at once",
     HS_SILENT},
    {"negative", "a peer that sends part of a handshake's header",
     "the stop ends the wait for the rest: one join, at once", HS_PARTIAL},
    {"boundary", "a handshake whose header comes in two parts, 50 ms apart",
     "answered and given a receive thread, as if it came whole", HS_SPLIT},
    {"negative", "a handshake with no payload",
     "refused; the next peer's handshake is accepted", HS_EMPTY},
    {"boundary", "a handshake one byte short",
     "refused without reading past it; the next is accepted", HS_SHORT},
    {"corner", "another message type, then a peer that hangs up",
     "both refused, the message freed once; the next is accepted",
     HS_OTHER_HANGUP},
};

/*
 * Send a message of type with the first len bytes of a handshake payload.
 * If split isn't zero, send only the first split bytes of the header, then
 * pause for HS_SPLIT_MS; if rest, send the rest of the message after that.
 */
static bool send_hs_split(int fd, TcpMsgType type, uint32_t len, size_t split,
                          bool rest)
{
    struct {
        TcpMsgHeader hdr;
        TcpHandshakePayload hs;
    } msg;
    size_t size = sizeof(msg.hdr) + len;

    if (len > sizeof(msg.hs) || split >= sizeof(msg.hdr))
        return false;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(type);
    msg.hdr.msg_len = htonl(len);
    msg.hdr.src_node_id = htonl(HS_NODE);
    msg.hs.node_id = htonl(HS_NODE);
    msg.hs.version = htonl(TCP_PROTOCOL_VERSION);
    if (split == 0)
        return send(fd, &msg, size, 0) == (ssize_t)size;
    if (send(fd, &msg, split, 0) != (ssize_t)split)
        return false;
    sleep_ms(HS_SPLIT_MS);
    return !rest || send(fd, (char *)&msg + split, size - split, 0) ==
                        (ssize_t)(size - split);
}

static bool send_hs(int fd, TcpMsgType type, uint32_t len)
{
    return send_hs_split(fd, type, len, 0, false);
}

/* The rows whose peer leaves the thread waiting, so no good peer follows */
static bool hs_waits(enum hs_peer peer)
{
    return peer == HS_SILENT || peer == HS_PARTIAL;
}

/* Connect a peer that sends what the row says, and check it's refused. */
static const char *hs_bad_peer(enum hs_peer peer, uint16_t port, int *fd)
{
    int hup;

    *fd = connect_loopback(port);
    if (*fd < 0)
        return "could not connect";
    switch (peer) {
    case HS_GOOD:
    case HS_SPLIT:
        return "not a bad peer";
    case HS_SILENT:
        sleep_ms(200); /* let the thread start waiting for the handshake */
        return NULL;
    case HS_PARTIAL:
        if (!send_hs_split(*fd, TCP_MSG_HANDSHAKE,
                           (uint32_t)sizeof(TcpHandshakePayload), HS_SPLIT_AT,
                           false))
            return "could not send part of the handshake";
        sleep_ms(150); /* the thread is waiting for the rest by now */
        return NULL;
    case HS_EMPTY:
        if (!send_hs(*fd, TCP_MSG_HANDSHAKE, 0))
            return "could not send the handshake";
        break;
    case HS_SHORT:
        if (!send_hs(*fd, TCP_MSG_HANDSHAKE,
                     (uint32_t)sizeof(TcpHandshakePayload) - 1))
            return "could not send the handshake";
        break;
    case HS_OTHER_HANGUP:
        if (!send_hs(*fd, TCP_MSG_HEARTBEAT,
                     (uint32_t)sizeof(TcpHandshakePayload)))
            return "could not send the message";
        if (!closed_by_peer(*fd))
            return "the message wasn't refused";
        hup = connect_loopback(port);
        if (hup < 0)
            return "could not connect the peer that hangs up";
        close(hup);
        return NULL;
    default:
        return NULL;
    }
    return closed_by_peer(*fd) ? NULL : "the handshake wasn't refused";
}

/*
 * Connect a peer that sends a good handshake, the header in two parts if
 * split, and check it's accepted.
 */
static const char *hs_good_peer(TcpBackendPrivate *priv, uint16_t port,
                                bool split, int *fd)
{
    TcpConnection *conn;

    *fd = connect_loopback(port);
    if (*fd < 0)
        return "could not connect the good peer";
    if (!send_hs_split(*fd, TCP_MSG_HANDSHAKE,
                       (uint32_t)sizeof(TcpHandshakePayload),
                       split ? HS_SPLIT_AT : 0, true))
        return "could not send the good handshake";
    /* The thread answers after it has published the connection */
    if (!recv_type(*fd, TCP_MSG_HANDSHAKE_RESP, 5000))
        return "the good handshake wasn't answered";
    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(HS_NODE));
    qemu_mutex_unlock(&priv->conn_table_lock);
    return conn ? NULL : "the good peer's connection isn't in the table";
}

static int run_hs_case(const struct hs_case *c)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    int bad = -1, good = -1;
    int want_joins = hs_waits(c->peer) ? 1 : 2;
    double start, took = 0.0;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->local_node_id, 1);
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    qemu_mutex_init(&priv->conn_table_lock);
    tcp_bufpool_init(&priv->recv_pool);
    priv->listen_fd = listen_loopback();
    memset(&addr, 0, sizeof(addr));

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (priv->listen_fd < 0 ||
        getsockname(priv->listen_fd, (struct sockaddr *)&addr, &len) < 0) {
        why = "could not open a listening socket";
    } else {
        atomic_store(&priv->accept_thread_running, true);
        qemu_thread_create(&priv->accept_thread, "tcp-accept",
                           tcp_accept_thread, priv, QEMU_THREAD_JOINABLE);
        if (c->peer != HS_GOOD && c->peer != HS_SPLIT)
            why = hs_bad_peer(c->peer, ntohs(addr.sin_port), &bad);
        if (!why && !hs_waits(c->peer))
            why = hs_good_peer(priv, ntohs(addr.sin_port), c->peer == HS_SPLIT,
                               &good);
    }

    start = now_s();
    tcp_stop_accept_thread(priv);
    took = now_s() - start;
    tcp_stop_recv_threads(priv);
    if (!why && hs_waits(c->peer) && !closed_by_peer(bad))
        why = "the waiting peer's connection was left open";
    alarm(0);

    ok = !why && took < 2.0 && atomic_load(&g_joins) == want_joins &&
         atomic_load(&g_exits) == atomic_load(&g_creates);

    printf("%s handshake %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s%s%d join(s) (want %d), %d of %d exited, the accept "
               "thread's stop took %.3f s\n",
               why ? why : "", why ? "; " : "", atomic_load(&g_joins),
               want_joins, atomic_load(&g_exits), atomic_load(&g_creates),
               took);

    g_hash_table_destroy(priv->connections);
    tcp_free_pending_conns(priv);
    if (bad >= 0)
        close(bad);
    if (good >= 0)
        close(good);
    if (priv->listen_fd >= 0)
        close(priv->listen_fd);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->conn_table_lock);
    g_free(priv);
    return ok ? 0 : 1;
}

/* ---- Health-check reconnect -------------------------------------------- */

#define RC_MAX_NODES 3

/* The id the manager gives the first node to register */
#define RC_NEW_NODE_ID 10

/* The id of a connection that belongs to no node in the mesh */
#define RC_STRAY_ID 50

/*
 * A manager whose nodes 1 to nodes have stopped answering.  For node i + 1:
 * node[i] is its mesh entry, at port on loopback, old[i] its old connection,
 * with the receive thread running, and peer[i] the node's end of it.  The
 * fixture holds a reference to each old[i], so a row can still look at one
 * once the health check has released it.  A row can add a connection for no
 * node, whose far end is stray_peer.
 */
struct rc_fixture {
    TcpBackendPrivate *priv;
    uint32_t nodes;
    TcpConnection *old[RC_MAX_NODES];
    MeshNodeInfo *node[RC_MAX_NODES];
    int peer[RC_MAX_NODES];
    int stray_peer;
};

/* Set up fx; returns NULL, or what failed.  rc_teardown() either way. */
static const char *rc_setup(struct rc_fixture *fx, uint16_t port,
                            uint32_t nodes, uint32_t interval)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);

    memset(fx, 0, sizeof(*fx));
    fx->priv = priv;
    for (uint32_t i = 0; i < RC_MAX_NODES; i++)
        fx->peer[i] = -1;
    fx->stray_peer = -1;
    priv->is_manager = true;
    /* Clear of the fixture's nodes, and of node 0, the manager itself */
    priv->next_available_node_id = RC_NEW_NODE_ID;
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->tcp_stats.reconnect_attempts, 0);
    atomic_init(&priv->tcp_stats.reconnect_successes, 0);
    atomic_init(&priv->health_check_running, false);
    qemu_mutex_init(&priv->health_check_lock);
    qemu_cond_init(&priv->health_check_cond);
    priv->health_check_interval_sec = interval;
    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, mesh_node_info_destroy_notify);
    qemu_mutex_init(&priv->mesh_table_lock);
    qemu_mutex_init(&priv->topology_broadcast_lock);
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    qemu_mutex_init(&priv->conn_table_lock);
    tcp_bufpool_init(&priv->recv_pool);

    for (uint32_t i = 0; i < nodes && i < RC_MAX_NODES; i++) {
        uint32_t id = i + 1;

        fx->old[i] = recv_conn_new(priv, id, true, &fx->peer[i]);
        if (!fx->old[i])
            return "could not set up an old connection";
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(id),
                            tcp_connection_ref(fx->old[i]));
        fx->node[i] = mesh_node_info_new(id, "127.0.0.1", port);
        if (!fx->node[i])
            return "could not create a mesh node";
        fx->node[i]->is_alive = false; /* already reported dead: no broadcast */
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(id),
                            fx->node[i]);
        fx->nodes++;
    }
    return NULL;
}

/*
 * Make the nodes look dead_s seconds dead, and start the health-check
 * thread.  It starts just after the second ticks over, so its first pass,
 * which comes at once, sees exactly dead_s.
 */
static void rc_start(struct rc_fixture *fx, time_t dead_s)
{
    time_t t0 = time(NULL);

    while (time(NULL) == t0)
        sleep_ms(5);
    for (uint32_t i = 0; i < fx->nodes; i++)
        fx->node[i]->last_heartbeat = time(NULL) - dead_s;
    atomic_store(&fx->priv->health_check_running, true);
    qemu_thread_create(&fx->priv->health_check_thread, "tcp-health",
                       tcp_manager_health_check_thread, fx->priv,
                       QEMU_THREAD_JOINABLE);
}

static void rc_teardown(struct rc_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;

    tcp_stop_health_check_thread(priv);
    tcp_stop_recv_threads(priv);
    g_hash_table_destroy(priv->mesh_nodes);
    g_hash_table_destroy(priv->connections);
    tcp_free_pending_conns(priv);
    for (uint32_t i = 0; i < RC_MAX_NODES; i++) {
        tcp_connection_unref(fx->old[i]);
        if (fx->peer[i] >= 0)
            close(fx->peer[i]);
    }
    if (fx->stray_peer >= 0)
        close(fx->stray_peer);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->topology_broadcast_lock);
    qemu_mutex_destroy(&priv->mesh_table_lock);
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    g_free(priv);
}

/*
 * The health check reconnects to a node that has stopped answering: it
 * retires the node's old connection, joining its receive thread, and opens a
 * new one.  When the new connection's receive thread can't be created, the
 * retired connection must not be left in the connections table, or the next
 * attempt would join its thread a second time.
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
    struct rc_fixture fx;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why;
    int lfd = listen_loopback();
    uint16_t port = 0;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_store(&g_create_failures, 0);
    if (lfd >= 0 && getsockname(lfd, (struct sockaddr *)&addr, &len) == 0)
        port = ntohs(addr.sin_port);

    g_current_row = desc;
    alarm(ROW_TIMEOUT_S);
    why = rc_setup(&fx, port, 1, 1);
    if (!why && port == 0)
        why = "could not open a listening socket";

    if (!why) {
        TcpConnection *in_table, *old = fx.old[0];
        int fd;

        atomic_store(&g_creates_left, 1); /* the health thread; no more */
        rc_start(&fx, 4);

        for (uint32_t i = 0; i < 600 && atomic_load(&g_create_failures) == 0;
             i++)
            sleep_ms(10);

        /* Taken out of the table before the new receive thread is started */
        qemu_mutex_lock(&fx.priv->conn_table_lock);
        in_table =
            g_hash_table_lookup(fx.priv->connections, GUINT_TO_POINTER(1));
        qemu_mutex_unlock(&fx.priv->conn_table_lock);
        qemu_mutex_lock(&old->lock);
        fd = old->sockfd;
        qemu_mutex_unlock(&old->lock);

        if (atomic_load(&g_create_failures) == 0)
            why = "no reconnect was attempted";
        else if (in_table)
            why = "the node still has a connection in the table";
        else if (fd != -1 || atomic_load(&old->recv_thread_running))
            why = "the old connection wasn't retired";
        else if (atomic_load(&g_joins) != 1)
            why = "the old receive thread wasn't joined exactly once";

        tcp_stop_health_check_thread(fx.priv);
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

    rc_teardown(&fx);
    if (lfd >= 0)
        close(lfd);
    return ok ? 0 : 1;
}

/* ---- Reconnecting without the mesh table's lock ------------------------ */

/*
 * The health check used to hold mesh_table_lock for its whole pass,
 * reconnects included.  A reconnect joins the old connection's receive
 * thread, and that thread takes mesh_table_lock to record a heartbeat, so a
 * node that answered just as it was declared dead deadlocked the manager.
 * The connect, which can take minutes against a host that doesn't answer,
 * also held up every registration, and a stop.
 *
 * Without the lock, the reconnect has to check that the node is still
 * silent, since a heartbeat can now be recorded while it runs, and the
 * pass's reconnects have to stop after an interval between them, since no
 * heartbeats go out until they're done.
 *
 * These rows reconnect to a listener that doesn't answer (see
 * listen_unanswering()), so the health-check thread waits in the connect
 * for as long as a row needs.  The nodes look dead_s seconds dead at the
 * first pass, which is past three intervals and a whole number of backoffs,
 * so the pass attempts to reconnect to each, right after sending them all a
 * heartbeat.
 */

enum rc_scenario {
    RC_HEARTBEAT, /* a heartbeat from no node, then the connect completes */
    RC_RECOVERS,  /* the nodes answer, then the first connect completes */
    RC_STOP,      /* the thread is stopped */
    RC_TIMEOUT,   /* the connect waits out its timeout */
    RC_REGISTERS, /* a node registers while the reconnects wait */
    RC_STARVES,   /* the first node never answers, the second does */
};

struct rc_case {
    const char *category;
    const char *description;
    const char *expected;
    enum rc_scenario scenario;
    uint32_t nodes;
    uint32_t interval; /* health_check_interval_sec, the reconnects' budget */
    time_t dead_s;
};

static const struct rc_case rc_cases[] = {
    {"positive", "a heartbeat from no node while the health check reconnects",
     "answered at once; the old thread is joined once, and the node gets the "
     "new connection",
     RC_HEARTBEAT, 1, LONG_INTERVAL_S, 3 * LONG_INTERVAL_S + 60},
    {"negative",
     "two nodes answer while the health check reconnects to the first",
     "neither is reconnected: the first's new connection is closed unused, "
     "and the second isn't tried",
     RC_RECOVERS, 2, LONG_INTERVAL_S, 3 * LONG_INTERVAL_S + 60},
    {"negative", "a stop while reconnecting to a node that doesn't answer",
     "returns at once, without waiting for the connect; the mesh table is "
     "free meanwhile, and the old connection untouched",
     RC_STOP, 1, LONG_INTERVAL_S, 3 * LONG_INTERVAL_S + 60},
    {"boundary", "a reconnect to a node that doesn't answer, on a 1 s interval",
     "the connect gives up after the interval, and the next pass runs",
     RC_TIMEOUT, 1, 1, 4},
    {"boundary",
     "three nodes that don't answer, and one that registers meanwhile, on a "
     "2 s interval",
     "the reconnects stop after an interval, so the new node is sent a "
     "heartbeat before it can be declared dead",
     RC_REGISTERS, 3, 2, 8},
    {"corner",
     "two nodes due together, the first never answering and the second "
     "answering, on a 1 s interval",
     "the second is deferred, not starved: it goes first on the next pass, "
     "and is reconnected",
     RC_STARVES, 2, 1, 4},
};

/*
 * A loopback listener that doesn't answer, like a host that has gone away.
 * Its backlog holds one connection, which *filler takes up, and the kernel
 * drops the SYN of every connect after that, so the connecting side waits
 * and retransmits.  Accepting the filler makes room, and the next
 * retransmission, a second or more after the connect began, gets through.
 */
static int listen_unanswering(uint16_t *port, int *filler)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    *port = 0;
    *filler = -1;
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 0) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0 ||
        (*filler = connect_loopback(ntohs(addr.sin_port))) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

/* What the connect rows connect to. */
enum connect_target {
    CT_LISTENING,       /* a listening port */
    CT_CLOSED,          /* a port nothing listens on */
    CT_UNANSWERING,     /* a listener that doesn't answer */
    CT_TWO_UNANSWERING, /* two addresses, neither of which answers */
    CT_REFUSED_FIRST,   /* two addresses, the first refused, then listening */
};

struct connect_case {
    const char *category;
    const char *description;
    const char *expected;
    enum connect_target target;
    uint32_t timeout_s; /* from the call to its deadline; 0 for none */
    bool want_fd;
    double min_s, max_s; /* how long the call may take */
};

static const struct connect_case connect_cases[] = {
    {"positive", "connect to a listening port",
     "a connected socket, left non-blocking", CT_LISTENING, 0, true, 0.0, 1.0},
    {"negative", "connect to a port nothing listens on", "fails at once",
     CT_CLOSED, 0, false, 0.0, 1.0},
    {"boundary", "connect to a node that doesn't answer, with a 1 s timeout",
     "gives up after 1 s", CT_UNANSWERING, 1, false, 1.0, 3.0},
    {"boundary",
     "connect to two addresses, neither of which answers, with a 1 s timeout",
     "gives up after 1 s in all, not 1 s for each", CT_TWO_UNANSWERING, 1,
     false, 1.0, 1.8},
    {"negative",
     "connect to two addresses, the first refused, with a 1 s timeout",
     "a failure before the deadline moves on: connected to the second",
     CT_REFUSED_FIRST, 1, true, 0.0, 1.0},
};

static int run_connect_case(const struct connect_case *c)
{
    struct sockaddr_in addr[2];
    struct addrinfo ai[2];
    socklen_t len = sizeof(addr[0]);
    int lfd[2] = {-1, -1}, filler[2] = {-1, -1}, fd = -1;
    uint16_t port[2] = {0, 0};
    uint32_t addrs =
        c->target == CT_TWO_UNANSWERING || c->target == CT_REFUSED_FIRST ? 2
                                                                         : 1;
    uint64_t deadline_ms = 0;
    double start, took = 0.0;
    bool ok = false;

    switch (c->target) {
    case CT_LISTENING:
    case CT_CLOSED:
    case CT_REFUSED_FIRST:
        for (uint32_t i = 0; i < addrs; i++) {
            len = sizeof(addr[i]);
            lfd[i] = listen_loopback();
            if (lfd[i] >= 0 &&
                getsockname(lfd[i], (struct sockaddr *)&addr[i], &len) == 0)
                port[i] = ntohs(addr[i].sin_port);
        }
        /* Nothing listens on the first port once its listener is closed */
        if (c->target != CT_LISTENING && lfd[0] >= 0) {
            close(lfd[0]);
            lfd[0] = -1;
        }
        break;
    case CT_UNANSWERING:
    case CT_TWO_UNANSWERING:
        for (uint32_t i = 0; i < addrs; i++)
            lfd[i] = listen_unanswering(&port[i], &filler[i]);
        break;
    default:
        break;
    }

    /* The list getaddrinfo() would return for a name with both addresses */
    memset(addr, 0, sizeof(addr));
    memset(ai, 0, sizeof(ai));
    for (uint32_t i = 0; i < addrs; i++) {
        addr[i].sin_family = AF_INET;
        addr[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr[i].sin_port = htons(port[i]);
        ai[i].ai_family = AF_INET;
        ai[i].ai_socktype = SOCK_STREAM;
        ai[i].ai_addr = (struct sockaddr *)&addr[i];
        ai[i].ai_addrlen = sizeof(addr[i]);
        ai[i].ai_next = i + 1 < addrs ? &ai[i + 1] : NULL;
    }

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port[0] != 0 && (addrs == 1 || port[1] != 0)) {
        start = now_s();
        if (c->timeout_s != 0)
            deadline_ms = tcp_monotonic_ms() + c->timeout_s * UINT64_C(1000);
        if (addrs == 1)
            fd = tcp_connect_to_remote("127.0.0.1", port[0], NULL, deadline_ms);
        else
            fd = tcp_connect_any(ai, NULL, deadline_ms);
        took = now_s() - start;
        ok = (fd >= 0) == c->want_fd && took >= c->min_s && took < c->max_s &&
             (fd < 0 || (fcntl(fd, F_GETFL, 0) & O_NONBLOCK));
    }
    alarm(0);

    printf("%s connect %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     port %u, got fd %d after %.3f s\n", (unsigned)port[0], fd,
               took);

    if (fd >= 0)
        close(fd);
    for (uint32_t i = 0; i < 2; i++) {
        if (filler[i] >= 0)
            close(filler[i]);
        if (lfd[i] >= 0)
            close(lfd[i]);
    }
    return ok ? 0 : 1;
}

/* Accept a connection on lfd within 5 s, or return -1. */
static int accept_within(int lfd)
{
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};

    return poll(&pfd, 1, 5000) == 1 ? accept(lfd, NULL, NULL) : -1;
}

/* Send a heartbeat as node src, which is how a worker answers the manager. */
static bool send_heartbeat(int fd, uint32_t src)
{
    TcpMsgHeader hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(TCP_MSG_HEARTBEAT);
    hdr.src_node_id = htonl(src);
    return send(fd, &hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr);
}

/* Read messages from fd until one of type arrives, for up to timeout_ms. */
static bool wait_for_type(int fd, TcpMsgType type, int timeout_ms)
{
    double end = now_s() + timeout_ms / 1000.0;

    while (now_s() < end) {
        int left = (int)((end - now_s()) * 1000.0);

        if (recv_type(fd, type, left > 0 ? left : 1))
            return true;
    }
    return false;
}

/*
 * RC_HEARTBEAT: a heartbeat arrives while the health check connects, then
 * the connect is let through.  It comes on a connection that belongs to no
 * node, so that it doesn't revive node 1, though it names node 1: the
 * manager credits a heartbeat to the node whose connection it came on.
 * *filler and *fresh get the listener's ends of the filler and of the new
 * connection, for the caller to close.
 */
static const char *rc_heartbeat(struct rc_fixture *fx, int lfd, int *filler,
                                int *fresh)
{
    TcpConnection *in_table, *stray;
    bool alive;

    stray = recv_conn_new(fx->priv, RC_STRAY_ID, true, &fx->stray_peer);
    if (!stray)
        return "could not set up the connection for no node";
    /* In the table, so teardown stops and frees it */
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    g_hash_table_insert(fx->priv->connections, GUINT_TO_POINTER(RC_STRAY_ID),
                        stray);
    qemu_mutex_unlock(&fx->priv->conn_table_lock);

    if (!send_heartbeat(fx->stray_peer, 1))
        return "could not send the heartbeat";
    if (!recv_type(fx->stray_peer, TCP_MSG_HEARTBEAT_RESP, 2000))
        return "a receive thread couldn't record a heartbeat while the health "
               "check connected";

    *filler = accept_within(lfd);
    *fresh = accept_within(lfd);
    if (*filler < 0 || *fresh < 0)
        return "the connect never got through";
    if (!recv_type(*fresh, TCP_MSG_HANDSHAKE, 5000))
        return "the new connection sent no handshake";
    /* The broadcast comes after the new connection is published */
    if (!recv_type(*fresh, TCP_MSG_MESH_TOPOLOGY, 5000))
        return "the reconnect wasn't broadcast";

    qemu_mutex_lock(&fx->priv->mesh_table_lock);
    alive = fx->node[0]->is_alive;
    qemu_mutex_unlock(&fx->priv->mesh_table_lock);
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    in_table = g_hash_table_lookup(fx->priv->connections, GUINT_TO_POINTER(1));
    qemu_mutex_unlock(&fx->priv->conn_table_lock);

    if (atomic_load(&fx->priv->tcp_stats.reconnect_successes) != 1)
        return "the reconnect didn't succeed";
    if (!in_table || in_table == fx->old[0] || !alive)
        return "the node isn't alive with a new connection in the table";
    if (atomic_load(&g_joins) != 1)
        return "the old receive thread wasn't joined exactly once";
    return NULL;
}

/*
 * The reconnects to every node have failed or been abandoned, and left the
 * old connections as they were.
 */
static const char *rc_untouched(struct rc_fixture *fx)
{
    if (atomic_load(&fx->priv->tcp_stats.reconnect_attempts) != 0)
        return "a connect succeeded";
    for (uint32_t i = 0; i < fx->nodes; i++) {
        TcpConnection *in_table;
        int fd;

        qemu_mutex_lock(&fx->priv->conn_table_lock);
        in_table =
            g_hash_table_lookup(fx->priv->connections, GUINT_TO_POINTER(i + 1));
        qemu_mutex_unlock(&fx->priv->conn_table_lock);
        qemu_mutex_lock(&fx->old[i]->lock);
        fd = fx->old[i]->sockfd;
        qemu_mutex_unlock(&fx->old[i]->lock);
        if (in_table != fx->old[i] || fd < 0 ||
            !atomic_load(&fx->old[i]->recv_thread_running))
            return "an old connection was retired";
    }
    return NULL;
}

/*
 * RC_RECOVERS: both nodes answer the pass's heartbeat late, while the
 * health check connects to whichever comes first, then that connect is let
 * through.  It has to see the answer and close the new connection, and skip
 * the other node.  *filler and *fresh are as for rc_heartbeat().
 */
static const char *rc_recovers(struct rc_fixture *fx, int lfd, int *filler,
                               int *fresh)
{
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};

    for (uint32_t i = 0; i < fx->nodes; i++) {
        if (!send_heartbeat(fx->peer[i], i + 1))
            return "could not send a heartbeat";
        /*
         * The manager answers once it has recorded the heartbeat.  The
         * topology update reporting an earlier node alive again can come
         * first.
         */
        if (!wait_for_type(fx->peer[i], TCP_MSG_HEARTBEAT_RESP, 2000))
            return "a heartbeat wasn't recorded while the health check "
                   "connected";
    }

    *filler = accept_within(lfd);
    *fresh = accept_within(lfd);
    if (*filler < 0 || *fresh < 0)
        return "the connect never got through";
    if (!closed_by_peer(*fresh))
        return "a node that answered was reconnected to";
    /* A connect to the second node would get straight in now */
    if (poll(&pfd, 1, 1500) != 0)
        return "the second node was reconnected to";
    if (atomic_load(&g_joins) != 0)
        return "an old receive thread was joined";
    return rc_untouched(fx);
}

/*
 * RC_REGISTERS: a node registers while the pass's reconnects wait.  The
 * next pass has to come soon enough to send it a heartbeat while it still
 * counts as alive.
 */
static const char *rc_registers(struct rc_fixture *fx)
{
    MeshNodeInfo *node;
    bool alive = false;
    int fd = worker_attach(fx->priv, NULL);
    const char *why = NULL;

    if (fd < 0 || !send_register(fd, "127.0.0.1", UINT32_MAX))
        why = "could not register the new node";
    else if (!wait_for_type(fd, TCP_MSG_HEARTBEAT, 9000))
        why = "the new node was never sent a heartbeat";

    if (!why) {
        qemu_mutex_lock(&fx->priv->mesh_table_lock);
        node = g_hash_table_lookup(fx->priv->mesh_nodes,
                                   GUINT_TO_POINTER(RC_NEW_NODE_ID));
        alive = node && node->is_alive;
        qemu_mutex_unlock(&fx->priv->mesh_table_lock);
        if (!alive)
            why = "the new node was declared dead before its first heartbeat";
    }
    if (fd >= 0)
        close(fd);
    return why;
}

/*
 * RC_STARVES: point the node the health check comes to second at a listener
 * that answers, leaving the first at the one that doesn't, which uses up
 * the pass's interval.  Returns the new listener, or -1, and the node in
 * *second.  Called before the health check starts.
 */
static int rc_second_answers(struct rc_fixture *fx, MeshNodeInfo **second)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    GHashTableIter iter;
    gpointer value;
    uint32_t n = 0;
    int lfd = listen_loopback();

    *second = NULL;
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
        if (lfd >= 0)
            close(lfd);
        return -1;
    }
    /* The scan walks the table in this order */
    g_hash_table_iter_init(&iter, fx->priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (n++ == 1)
            *second = value;
    if (*second)
        (*second)->port = ntohs(addr.sin_port);
    return lfd;
}

/*
 * RC_STARVES: the second node is passed over on the first pass, and has to
 * be reconnected on a later one.  *fresh is as for rc_heartbeat().
 */
static const char *rc_starves(struct rc_fixture *fx, MeshNodeInfo *second,
                              int lfd, int *fresh)
{
    TcpConnection *in_table, *old = NULL;

    *fresh = accept_within(lfd);
    if (*fresh < 0)
        return "the second node was never tried";
    if (!recv_type(*fresh, TCP_MSG_HANDSHAKE, 5000))
        return "the second node's new connection sent no handshake";
    /* The broadcast comes after the new connection is published */
    if (!recv_type(*fresh, TCP_MSG_MESH_TOPOLOGY, 5000))
        return "the reconnect wasn't broadcast";

    qemu_mutex_lock(&fx->priv->conn_table_lock);
    in_table = g_hash_table_lookup(fx->priv->connections,
                                   GUINT_TO_POINTER(second->node_id));
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    for (uint32_t i = 0; i < fx->nodes; i++)
        if (fx->node[i] == second)
            old = fx->old[i];

    if (atomic_load(&fx->priv->tcp_stats.reconnect_successes) != 1)
        return "the second node's reconnect didn't succeed";
    if (!in_table || in_table == old)
        return "the second node has no new connection in the table";
    return NULL;
}

static int run_rc_case(const struct rc_case *c)
{
    struct rc_fixture fx;
    const char *why;
    int filler = -1, filler_accepted = -1, fresh = -1, answering = -1;
    uint16_t port;
    int lfd = listen_unanswering(&port, &filler);
    MeshNodeInfo *second = NULL;
    double start, took = 0.0;
    bool ok;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    why = rc_setup(&fx, port, c->nodes, c->interval);
    if (!why && lfd < 0)
        why = "could not open a listener that doesn't answer";
    if (!why && c->scenario == RC_STARVES &&
        (answering = rc_second_answers(&fx, &second)) < 0)
        why = "could not open a listener that answers";

    if (!why) {
        rc_start(&fx, c->dead_s);
        for (uint32_t i = 0; i < fx.nodes && !why; i++)
            if (!recv_type(fx.peer[i], TCP_MSG_HEARTBEAT, 5000))
                why = "the first pass sent no heartbeat";
    }
    if (!why) {
        sleep_ms(200); /* into the connect */
        switch (c->scenario) {
        case RC_HEARTBEAT:
            why = rc_heartbeat(&fx, lfd, &filler_accepted, &fresh);
            break;
        case RC_RECOVERS:
            why = rc_recovers(&fx, lfd, &filler_accepted, &fresh);
            break;
        case RC_STOP:
            if (qemu_mutex_trylock(&fx.priv->mesh_table_lock) != 0) {
                why = "the health check held mesh_table_lock while connecting";
            } else {
                qemu_mutex_unlock(&fx.priv->mesh_table_lock);
            }
            start = now_s();
            tcp_stop_health_check_thread(fx.priv);
            took = now_s() - start;
            if (!why && (took >= 1.0 || atomic_load(&g_exits) != 1))
                why = "the stop waited for the connect";
            else if (!why)
                why = rc_untouched(&fx);
            break;
        case RC_TIMEOUT:
            /* The next pass only comes once the connect has given up */
            if (!recv_type(fx.peer[0], TCP_MSG_HEARTBEAT, 5000))
                why = "the connect didn't give up after the interval";
            else
                why = rc_untouched(&fx);
            break;
        case RC_REGISTERS:
            why = rc_registers(&fx);
            break;
        case RC_STARVES:
            why = second ? rc_starves(&fx, second, answering, &fresh)
                         : "the fixture has no second node";
            break;
        default:
            why = "unknown scenario";
            break;
        }
    }

    ok = !why;
    printf("%s reconnect %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; stop took %.3f s\n", why, took);
    fflush(stdout);

    rc_teardown(&fx);
    alarm(0);
    if (filler >= 0)
        close(filler);
    if (filler_accepted >= 0)
        close(filler_accepted);
    if (fresh >= 0)
        close(fresh);
    if (answering >= 0)
        close(answering);
    if (lfd >= 0)
        close(lfd);
    return ok ? 0 : 1;
}

/*
 * A node deferred for lack of time has to keep its place in line, or nodes
 * tried more recently can get ahead of it.  With a deferred flag alone,
 * three nodes due on every pass, A and B never answering and C answering,
 * were tried A, B, A, B, and C never: whichever of A and B had just been
 * tried was deferred on the next pass, and back in front of C on the one
 * after.  The thread can't be made to find all three due on every pass,
 * which takes their backoffs lining up with the passes, so this row runs
 * the reconnect phase itself, pass by pass, as if it had.
 */
static int run_queue_case(void)
{
    static const char desc[] =
        "three nodes due every pass, the first two never answering";
    static const char expected[] =
        "the third, deferred longest, goes first on the third pass, and is "
        "reconnected";
    struct rc_fixture fx;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    GArray *items = g_array_new(FALSE, FALSE, sizeof(TcpHealthCheckItem));
    const char *why;
    int filler = -1, fresh = -1, answering = listen_loopback();
    uint16_t port;
    int lfd = listen_unanswering(&port, &filler);
    uint32_t passes = 0;
    bool ok;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = desc;
    alarm(ROW_TIMEOUT_S);
    why = rc_setup(&fx, port, 3, LONG_INTERVAL_S);
    if (!why && (lfd < 0 || answering < 0 ||
                 getsockname(answering, (struct sockaddr *)&addr, &len) < 0))
        why = "could not open the listeners";

    if (!why) {
        fx.node[2]->port = ntohs(addr.sin_port);
        for (uint32_t i = 0; i < fx.nodes; i++)
            fx.node[i]->last_heartbeat = time(NULL) - 4 * LONG_INTERVAL_S;
        /* The connects stop when this is cleared; no thread is started */
        atomic_store(&fx.priv->health_check_running, true);

        while (passes < 6 &&
               atomic_load(&fx.priv->tcp_stats.reconnect_successes) == 0) {
            g_array_set_size(items, 0);
            for (uint32_t i = 0; i < fx.nodes; i++) {
                /* The fixture's reference stands in for the scan's */
                TcpHealthCheckItem item = {fx.node[i], fx.old[i], true, false};

                g_array_append_val(items, item);
            }
            /* Long enough to try one node that doesn't answer, and no more */
            tcp_health_check_reconnects(fx.priv, items,
                                        tcp_monotonic_ms() + 300);
            passes++;
        }
        atomic_store(&fx.priv->health_check_running, false);

        if (atomic_load(&fx.priv->tcp_stats.reconnect_successes) == 0)
            why = "the third node was never tried";
        else if (passes != 3)
            why = "the third node wasn't tried on the third pass";
        else if ((fresh = accept_within(answering)) < 0 ||
                 !recv_type(fresh, TCP_MSG_HANDSHAKE, 5000))
            why = "the third node's new connection sent no handshake";
        else if (!tcp_has_connection(fx.priv, 3))
            why = "the third node has no connection";
    }

    ok = !why;
    printf("%s reconnect corner: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           desc, expected);
    if (!ok)
        printf("     %s after %u pass(es)\n", why, passes);
    fflush(stdout);

    rc_teardown(&fx);
    alarm(0);
    g_array_free(items, TRUE);
    if (fresh >= 0)
        close(fresh);
    if (filler >= 0)
        close(filler);
    if (answering >= 0)
        close(answering);
    if (lfd >= 0)
        close(lfd);
    return ok ? 0 : 1;
}

/* ---- Registering twice ------------------------------------------------- */

/*
 * A manager used to register a connection for every REGISTER_NODE it
 * received on it.  A second registration on the same socket gave the
 * connection a second node id, so it was in the connections table under
 * both, and tcp_fini() freed it twice.  Only a connection on the pending
 * list can register now, and registering takes it off.
 *
 * The worker sends its registrations and then shuts down its side, so the
 * receive thread handles every message before it reads end-of-file and
 * exits.  Once it has, everything it sent is waiting in the worker's socket.
 */

struct reg_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *first_host; /* the hostname in the first registration */
    bool second;            /* send a second registration */
    uint32_t second_id;     /* the node id the second one asks for */
};

static const struct reg_case reg_cases[] = {
    {"positive", "one registration", "registered once, with one answer",
     "127.0.0.1", false, 0},
    {"negative", "a second registration asking for any node id",
     "ignored: one table entry, one mesh node and one answer", "127.0.0.1",
     true, UINT32_MAX},
    {"negative", "a second registration asking for an unused node id",
     "ignored: the id isn't given out", "127.0.0.1", true, 42},
    {"corner", "a registration with no hostname, then a good one",
     "the first leaves the connection pending; the second registers it", "",
     true, UINT32_MAX},
};

/* Read every message waiting on fd, and count those of type. */
static int count_type(int fd, TcpMsgType type)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int n = 0;

    while (poll(&pfd, 1, 0) == 1) {
        TcpMsgHeader hdr;
        void *payload = NULL;

        if (tcp_recv_message(fd, &hdr, &payload, NULL, NULL, 0) < 0)
            break;
        g_free(payload);
        if (hdr.msg_type == type)
            n++;
    }
    return n;
}

static guint mesh_node_count(TcpBackendPrivate *priv)
{
    guint n;

    qemu_mutex_lock(&priv->mesh_table_lock);
    n = g_hash_table_size(priv->mesh_nodes);
    qemu_mutex_unlock(&priv->mesh_table_lock);
    return n;
}

static int run_reg_case(const struct reg_case *c)
{
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    TcpConnection *conn = NULL;
    char config[64];
    const char *why = NULL;
    uint16_t port = free_port();
    guint entries = 0, table = 0, pending = 0, nodes_before, nodes = 0;
    int fd, answers = 0;
    bool ok;

    atomic_store(&g_exits, 0);
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", "3600", 1);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port == 0 || tcp_init(&dev, config) != 0) {
        printf("FAIL registration %s: %s; could not start a manager\n",
               c->category, c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;
    nodes_before = mesh_node_count(priv);
    /*
     * The health check's first pass comes at once.  One that came after the
     * receive thread has seen the end of the stream would retire the
     * connection, as one whose node hung up, and take it out of the table.
     * Stopping the thread now makes that pass come first; the exits counted
     * from here on are the receive thread's.
     */
    tcp_stop_health_check_thread(priv);
    atomic_store(&g_exits, 0);

    fd = worker_attach(priv, &conn);
    if (fd < 0) {
        why = "could not attach the worker";
    } else if (!send_register(fd, c->first_host, UINT32_MAX) ||
               (c->second && !send_register(fd, "127.0.0.1", c->second_id)) ||
               shutdown(fd, SHUT_WR) < 0) {
        why = "could not send the registrations";
    } else if (!wait_for_exits(1)) {
        why = "the receive thread never read to the end of the stream";
    } else {
        GHashTableIter iter;
        gpointer value;

        nodes = mesh_node_count(priv) - nodes_before;
        qemu_mutex_lock(&priv->conn_table_lock);
        table = g_hash_table_size(priv->connections);
        g_hash_table_iter_init(&iter, priv->connections);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            entries += value == conn ? 1u : 0u;
        pending = priv->pending_conns->len;
        qemu_mutex_unlock(&priv->conn_table_lock);
        answers = count_type(fd, TCP_MSG_REGISTER_RESP);
    }

    ok = !why && entries == 1 && table == 1 && pending == 0 && nodes == 1 &&
         answers == 1;

    printf("%s registration %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s%s%u table entr%s for the connection (want 1), %u in "
               "the table (want 1), %u pending (want 0), %u new mesh node(s) "
               "(want 1), %d answer(s) (want 1)\n",
               why ? why : "", why ? "; " : "", entries,
               entries == 1 ? "y" : "ies", table, pending, nodes, answers);
    /* A connection in the table twice is freed twice, and ASan aborts */
    fflush(stdout);

    tcp_fini(&dev);
    alarm(0);
    if (fd >= 0)
        close(fd);
    return ok ? 0 : 1;
}

/* ---- A worker connecting to its peers ---------------------------------- */

/*
 * A worker connects to the peers the manager names in a topology update.  It
 * used to connect on the receive thread the update came on, holding
 * conn_table_lock, which the data path takes to look up a connection.  A
 * peer that didn't answer then stalled the worker's data path, its answers
 * to the manager's heartbeats, and its SIGTERM, until the kernel gave up on
 * the connect about two minutes later, and a peer that couldn't be reached
 * wasn't tried again until the mesh changed.
 *
 * The connects are now made by a peer-connect thread, without the lock.
 * Each gives up after a health-check interval, or at once when the thread is
 * stopped.  Every peer is tried on every pass, and a pass comes on each
 * update and otherwise an interval after the last.  A peer that an update
 * during the pass drops isn't connected to, or its new connection is closed
 * before the handshake, which would have the peer publish it.
 *
 * The worker here is node TP_LOCAL.  Its connection to the manager is a
 * socketpair whose other end the test writes the updates to, and its
 * peer-connect thread is running.  The peers are loopback listeners, and a
 * peer that doesn't answer is listen_unanswering().  peer_connect_attempts
 * shows when the thread has decided to connect to a peer.
 */

#define TP_LOCAL 3
#define TP_LOWER 2 /* a worker with a lower id, which connects to this one */
#define TP_PEER  5 /* higher than TP_LOCAL, so the worker connects to it */
#define TP_OTHER 6 /* a second such peer */
#define TP_THIRD 7 /* and a third */

enum tp_scenario {
    TP_CONNECTS,   /* a peer that is listening */
    TP_SKIPS,      /* peers to skip, then one that is listening */
    TP_STOP,       /* a peer that doesn't answer, then a stop */
    TP_STARVES,    /* one that doesn't answer, then one that is listening */
    TP_RETRY,      /* a peer that refuses the connect, then listens */
    TP_RACE,       /* the peer connects to the worker while it connects */
    TP_NO_THREADS, /* the new connection's receive thread can't start */
    TP_DROPPED, /* a peer reported dead while the worker connects to another */
    TP_DIES,    /* a peer reported dead while the worker connects to it */
};

struct tp_case {
    const char *category;
    const char *description;
    const char *expected;
    enum tp_scenario scenario;
    uint32_t interval; /* health_check_interval_sec: per connect, and passes */
};

static const struct tp_case tp_cases[] = {
    {"positive", "a topology update naming a peer that is listening",
     "connected, handshake sent, and the connection published under the "
     "peer's id; every thread joined once",
     TP_CONNECTS, LONG_INTERVAL_S},
    {"negative",
     "an update naming the worker itself, the manager, a lower id, a peer "
     "already connected and a peer reported dead, then a peer that is "
     "listening",
     "only the last is connected to", TP_SKIPS, LONG_INTERVAL_S},
    {"negative", "a stop while connecting to a peer that doesn't answer",
     "heartbeats are answered and the connection table is free meanwhile, "
     "and the stop returns at once",
     TP_STOP, LONG_INTERVAL_S},
    {"boundary",
     "a peer that doesn't answer ahead of one that is listening, sent twice, "
     "on a 1 s interval",
     "the second is connected once the first gives up, an interval later",
     TP_STARVES, 1},
    {"boundary",
     "a peer that refuses the connect, then listens, on a 1 s "
     "interval",
     "connected on a later pass, with no new update", TP_RETRY, 1},
    {"corner",
     "the peer connects to the worker while the worker connects to it",
     "the peer's connection is kept, and the worker's closed without a "
     "handshake",
     TP_RACE, LONG_INTERVAL_S},
    {"corner", "a new peer connection whose receive thread can't be started",
     "closed after its handshake, not published", TP_NO_THREADS,
     LONG_INTERVAL_S},
    {"corner",
     "a second update, while the worker connects to a peer, reporting a later "
     "peer dead and adding one after it",
     "the dead peer isn't connected to, and the others are", TP_DROPPED,
     LONG_INTERVAL_S},
    {"corner",
     "an update reporting a peer dead while the worker's connect to it is in "
     "progress",
     "closed without a handshake, not published", TP_DIES, LONG_INTERVAL_S},
};

/* A worker that is node TP_LOCAL, with a connection to the manager. */
struct tp_fixture {
    TcpBackendPrivate *priv;
    int mgr; /* the manager's end of manager_conn */
};

/*
 * The peers' listeners, at port[i] on loopback, and the descriptors a row
 * opens on the peers' side, all closed at the end of the row.
 */
#define TP_LISTENERS 3
#define TP_MAX_FDS   4

struct tp_net {
    int lfd[TP_LISTENERS];
    uint16_t port[TP_LISTENERS];
    int filler[TP_LISTENERS]; /* the connection filling an unanswering
                                 listener's backlog */
    int fds[TP_MAX_FDS];
    uint32_t nfds;
};

/* Set up fx and start its threads; returns NULL, or what failed. */
static const char *tp_setup(struct tp_fixture *fx, uint32_t interval)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);

    fx->priv = priv;
    fx->mgr = -1;
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->health_check_running, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->local_node_id, TP_LOCAL);
    atomic_init(&priv->tcp_stats.peer_connect_attempts, 0);
    priv->health_check_interval_sec = interval;
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    qemu_mutex_init(&priv->conn_table_lock);
    tcp_bufpool_init(&priv->recv_pool);
    priv->peer_targets = g_array_new(FALSE, FALSE, sizeof(TcpPeerTarget));
    qemu_mutex_init(&priv->peer_lock);
    qemu_cond_init(&priv->peer_cond);
    atomic_init(&priv->peer_thread_running, false);

    priv->manager_conn = recv_conn_new(priv, 0, true, &fx->mgr);
    if (!priv->manager_conn)
        return "could not connect to the manager";
    if (!tcp_start_thread(&priv->peer_thread, "tcp-peers",
                          tcp_worker_peer_thread, priv,
                          &priv->peer_thread_running))
        return "could not start the peer-connect thread";
    return NULL;
}

/*
 * Stop every thread, as teardown does, and free everything.  Returns how long
 * the stop took.
 */
static double tp_teardown(struct tp_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;
    double start = now_s(), took;

    tcp_stop_threads(priv);
    took = now_s() - start;
    g_hash_table_destroy(priv->connections);
    tcp_free_pending_conns(priv);
    tcp_connection_unref(priv->manager_conn);
    if (fx->mgr >= 0)
        close(fx->mgr);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_cond_destroy(&priv->peer_cond);
    qemu_mutex_destroy(&priv->peer_lock);
    g_array_free(priv->peer_targets, TRUE);
    g_free(priv);
    return took;
}

/* Keep fd, if it's open, to be closed at the end of the row; returns it. */
static int tp_keep(struct tp_net *net, int fd)
{
    if (fd >= 0 && net->nfds < TP_MAX_FDS)
        net->fds[net->nfds++] = fd;
    return fd;
}

static void tp_close(struct tp_net *net)
{
    for (uint32_t i = 0; i < TP_LISTENERS; i++) {
        if (net->lfd[i] >= 0)
            close(net->lfd[i]);
        if (net->filler[i] >= 0)
            close(net->filler[i]);
    }
    for (uint32_t i = 0; i < net->nfds; i++)
        close(net->fds[i]);
}

/* A listener on loopback port port, or -1. */
static int listen_on(uint16_t port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Open listener i, answering or not, and record its port. */
static bool tp_listener(struct tp_net *net, uint32_t i, bool answering)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (!answering) {
        net->lfd[i] = listen_unanswering(&net->port[i], &net->filler[i]);
        return net->lfd[i] >= 0;
    }
    net->lfd[i] = listen_loopback();
    if (net->lfd[i] < 0 ||
        getsockname(net->lfd[i], (struct sockaddr *)&addr, &len) < 0)
        return false;
    net->port[i] = ntohs(addr.sin_port);
    return true;
}

/* Open the listeners the row's peers are at; returns NULL, or what failed. */
static const char *tp_listen(struct tp_net *net, enum tp_scenario scenario)
{
    bool ok;

    memset(net, 0, sizeof(*net));
    for (uint32_t i = 0; i < TP_LISTENERS; i++)
        net->lfd[i] = net->filler[i] = -1;

    switch (scenario) {
    case TP_SKIPS:
        ok = tp_listener(net, 0, true) && tp_listener(net, 1, true);
        break;
    case TP_STOP:
    case TP_RACE:
    case TP_DIES:
        ok = tp_listener(net, 0, false);
        break;
    case TP_DROPPED:
        ok = tp_listener(net, 0, false) && tp_listener(net, 1, true) &&
             tp_listener(net, 2, true);
        break;
    case TP_STARVES:
        ok = tp_listener(net, 0, false) && tp_listener(net, 1, true);
        break;
    case TP_RETRY:
        /* Nothing listens on the port once its listener is closed */
        ok = tp_listener(net, 0, true);
        if (ok) {
            close(net->lfd[0]);
            net->lfd[0] = -1;
        }
        break;
    case TP_CONNECTS:
    case TP_NO_THREADS:
        ok = tp_listener(net, 0, true);
        break;
    default:
        ok = false;
        break;
    }
    return ok ? NULL : "could not open the peers' listeners";
}

/* Add a topology entry for node id at port on loopback. */
static void tp_entry(TcpMeshTopologyPayload *topo, uint32_t id, uint16_t port,
                     bool alive)
{
    uint32_t n = ntohl(topo->num_nodes);
    TcpMeshNodeInfo *entry = &topo->nodes[n];

    entry->node_id = htonl(id);
    snprintf(entry->hostname, sizeof(entry->hostname), "127.0.0.1");
    entry->port = htons(port);
    entry->is_alive = alive ? 1 : 0;
    topo->num_nodes = htonl(n + 1);
}

/* Send topo as the manager. */
static bool tp_send(struct tp_fixture *fx, const TcpMeshTopologyPayload *topo)
{
    struct {
        TcpMsgHeader hdr;
        TcpMeshTopologyPayload topo;
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(TCP_MSG_MESH_TOPOLOGY);
    msg.hdr.msg_len = htonl((uint32_t)sizeof(msg.topo));
    memcpy(&msg.topo, topo, sizeof(msg.topo));
    return send(fx->mgr, &msg, sizeof(msg), 0) == (ssize_t)sizeof(msg);
}

/*
 * Whether the worker's receive thread for its manager connection is free:
 * it echoes a heartbeat within timeout_ms.
 */
static bool tp_answers(struct tp_fixture *fx, int timeout_ms)
{
    return send_heartbeat(fx->mgr, 0) &&
           wait_for_type(fx->mgr, TCP_MSG_HEARTBEAT, timeout_ms);
}

static TcpConnection *tp_lookup(struct tp_fixture *fx, uint32_t id)
{
    TcpConnection *conn;

    qemu_mutex_lock(&fx->priv->conn_table_lock);
    conn = g_hash_table_lookup(fx->priv->connections, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    return conn;
}

static guint tp_count(struct tp_fixture *fx)
{
    guint n;

    qemu_mutex_lock(&fx->priv->conn_table_lock);
    n = g_hash_table_size(fx->priv->connections);
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    return n;
}

/* Wait up to 5 s for the thread to have made n connect attempts. */
static bool tp_attempts(struct tp_fixture *fx, uint64_t n)
{
    for (int i = 0; i < 500; i++) {
        if (atomic_load(&fx->priv->tcp_stats.peer_connect_attempts) >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/*
 * The worker connects to the listener lfd and sends a handshake, then
 * publishes the connection under id.  *fd gets the peer's end.
 */
static const char *tp_connected_to(struct tp_fixture *fx, struct tp_net *net,
                                   int lfd, uint32_t id, int *fd)
{
    *fd = tp_keep(net, accept_within(lfd));
    if (*fd < 0)
        return "the worker never connected";
    if (!recv_type(*fd, TCP_MSG_HANDSHAKE, 5000))
        return "the worker sent no handshake";
    /* Published after the handshake is sent */
    for (int i = 0; i < 500 && !tp_lookup(fx, id); i++)
        sleep_ms(10);
    return tp_lookup(fx, id) ? NULL : "the connection wasn't published";
}

/* A connection to id, as the accept thread would publish it. */
static TcpConnection *tp_publish(struct tp_fixture *fx, struct tp_net *net,
                                 uint32_t id, bool start)
{
    int peer = -1;
    TcpConnection *conn = recv_conn_new(fx->priv, id, start, &peer);

    tp_keep(net, peer);
    if (conn) {
        qemu_mutex_lock(&fx->priv->conn_table_lock);
        g_hash_table_insert(fx->priv->connections, GUINT_TO_POINTER(id), conn);
        qemu_mutex_unlock(&fx->priv->conn_table_lock);
    }
    return conn;
}

/*
 * TP_SKIPS: the peers before TP_THIRD are skipped.  A connect to any of them
 * would come before TP_THIRD's, so once TP_THIRD is connected, nothing more
 * can arrive at their listener.
 */
static const char *tp_skips(struct tp_fixture *fx, struct tp_net *net)
{
    struct pollfd pfd = {.fd = net->lfd[0], .events = POLLIN};
    const char *why;
    int fd;

    why = tp_connected_to(fx, net, net->lfd[1], TP_THIRD, &fd);
    if (why)
        return why;
    if (poll(&pfd, 1, 0) != 0)
        return "the worker connected to a node it should have skipped";
    return tp_count(fx) == 2 ? NULL
                             : "a skipped node's connection was published";
}

/*
 * TP_STOP: the worker is waiting in the connect, and its receive thread and
 * the data path's lock are free.  The row's stop is tp_teardown().
 */
static const char *tp_stop(struct tp_fixture *fx)
{
    if (!tp_attempts(fx, 1))
        return "the worker never connected";
    if (!tp_answers(fx, 2000))
        return "a heartbeat wasn't answered while the worker connected";
    if (qemu_mutex_trylock(&fx->priv->conn_table_lock) != 0)
        return "the worker held conn_table_lock while connecting";
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    return NULL;
}

/*
 * TP_STARVES: the update is sent again while the worker waits on TP_PEER.
 * TP_OTHER is connected once that connect gives up, an interval in, and
 * heartbeats are answered meanwhile.
 */
static const char *tp_starves(struct tp_fixture *fx, struct tp_net *net,
                              const TcpMeshTopologyPayload *topo, double sent)
{
    const char *why;
    double took;
    int fd;

    if (!tp_attempts(fx, 1))
        return "the worker never connected";
    if (!tp_send(fx, topo))
        return "could not send the update again";
    if (!tp_answers(fx, 2000))
        return "a heartbeat wasn't answered while the worker connected";
    why = tp_connected_to(fx, net, net->lfd[1], TP_OTHER, &fd);
    if (why)
        return why;
    took = now_s() - sent;
    if (took < 0.9)
        return "the first connect gave up before the interval";
    return took < 3.0 ? NULL : "the second peer waited more than an interval";
}

/*
 * TP_RETRY: the first connect is refused.  Once a second has begun, which
 * only a later pass can start, a listener opens on the peer's port, and the
 * worker connects to it with no new update.
 */
static const char *tp_retry(struct tp_fixture *fx, struct tp_net *net)
{
    int fd;

    if (!tp_attempts(fx, 2))
        return "the worker never tried the peer again";
    net->lfd[0] = listen_on(net->port[0]);
    if (net->lfd[0] < 0)
        return "could not listen on the peer's port again";
    return tp_connected_to(fx, net, net->lfd[0], TP_PEER, &fd);
}

/*
 * TP_RACE: once the worker is connecting, the peer's own connection to it is
 * published, as the accept thread would.  The connect is then let through:
 * accepting the filler makes room for its next SYN.
 */
static const char *tp_race(struct tp_fixture *fx, struct tp_net *net)
{
    TcpConnection *theirs;
    int fd;

    /* Past the check for a connection, which the race has to beat */
    if (!tp_attempts(fx, 1))
        return "the worker never connected";
    theirs = tp_publish(fx, net, TP_PEER, true);
    if (!theirs)
        return "could not set up the peer's connection";

    tp_keep(net, accept_within(net->lfd[0]));
    fd = tp_keep(net, accept_within(net->lfd[0]));
    if (fd < 0)
        return "the connect never got through";
    if (!closed_by_peer(fd))
        return "the worker sent a handshake, or kept its own connection";
    if (tp_lookup(fx, TP_PEER) != theirs || tp_count(fx) != 1)
        return "the peer's connection was replaced";
    return NULL;
}

/* TP_NO_THREADS: the worker gives up on the connection it can't serve. */
static const char *tp_no_threads(struct tp_fixture *fx, struct tp_net *net)
{
    int fd = tp_keep(net, accept_within(net->lfd[0]));

    if (fd < 0)
        return "the worker never connected";
    if (!recv_type(fd, TCP_MSG_HANDSHAKE, 5000))
        return "the worker sent no handshake";
    if (!closed_by_peer(fd))
        return "the connection wasn't closed";
    if (atomic_load(&g_create_failures) != 1)
        return "no receive thread was refused";
    return tp_count(fx) == 0 ? NULL : "the connection was published";
}

/*
 * TP_DROPPED: while the worker connects to TP_PEER, an update reports
 * TP_OTHER, which is listening, dead, and adds TP_THIRD after it.  Once the
 * heartbeat after the update shows it was handled, the connect is let
 * through, as in tp_race(), and the pass goes on to TP_OTHER.  A connect to
 * TP_OTHER would come before TP_THIRD's, so once TP_THIRD is connected,
 * nothing more can arrive at TP_OTHER's listener.
 */
static const char *tp_dropped(struct tp_fixture *fx, struct tp_net *net)
{
    TcpMeshTopologyPayload topo;
    struct pollfd pfd = {.fd = net->lfd[1], .events = POLLIN};
    const char *why;
    int fd;

    if (!tp_attempts(fx, 1))
        return "the worker never connected";
    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, TP_PEER, net->port[0], true);
    tp_entry(&topo, TP_OTHER, net->port[1], false);
    tp_entry(&topo, TP_THIRD, net->port[2], true);
    if (!tp_send(fx, &topo))
        return "could not send the second update";
    if (!tp_answers(fx, 2000))
        return "the heartbeat after the update wasn't answered";

    tp_keep(net, accept_within(net->lfd[0]));
    why = tp_connected_to(fx, net, net->lfd[0], TP_PEER, &fd);
    if (why)
        return why;
    why = tp_connected_to(fx, net, net->lfd[2], TP_THIRD, &fd);
    if (why)
        return why;
    if (poll(&pfd, 1, 0) != 0)
        return "the worker connected to the peer reported dead";
    return tp_count(fx) == 2 ? NULL
                             : "the dead peer's connection was published";
}

/*
 * TP_DIES: while the worker connects to TP_PEER, an update reports it dead.
 * The heartbeat after the update shows the update was handled, and the
 * connect is then let through, as in tp_race().
 */
static const char *tp_dies(struct tp_fixture *fx, struct tp_net *net)
{
    TcpMeshTopologyPayload topo;
    int fd;

    if (!tp_attempts(fx, 1))
        return "the worker never connected";
    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, TP_PEER, net->port[0], false);
    if (!tp_send(fx, &topo))
        return "could not send the second update";
    if (!tp_answers(fx, 2000))
        return "the heartbeat after the update wasn't answered";

    tp_keep(net, accept_within(net->lfd[0]));
    fd = tp_keep(net, accept_within(net->lfd[0]));
    if (fd < 0)
        return "the connect never got through";
    if (!closed_by_peer(fd))
        return "the worker sent a handshake, or kept the connection";
    return tp_count(fx) == 0 ? NULL : "the connection was published";
}

/* Build the row's update; returns NULL, or what failed. */
static const char *tp_topology(struct tp_fixture *fx, struct tp_net *net,
                               enum tp_scenario scenario,
                               TcpMeshTopologyPayload *topo)
{
    memset(topo, 0, sizeof(*topo));
    switch (scenario) {
    case TP_SKIPS:
        tp_entry(topo, TP_LOCAL, net->port[0], true);
        tp_entry(topo, 0, net->port[0], true);
        tp_entry(topo, TP_LOWER, net->port[0], true);
        tp_entry(topo, TP_OTHER, net->port[0], true);
        tp_entry(topo, TP_PEER, net->port[0], false);
        tp_entry(topo, TP_THIRD, net->port[1], true);
        return tp_publish(fx, net, TP_OTHER, false)
                   ? NULL
                   : "could not set up the existing connection";
    case TP_STARVES:
    case TP_DROPPED:
        tp_entry(topo, TP_PEER, net->port[0], true);
        tp_entry(topo, TP_OTHER, net->port[1], true);
        return NULL;
    case TP_CONNECTS:
    case TP_STOP:
    case TP_RETRY:
    case TP_RACE:
    case TP_NO_THREADS:
    case TP_DIES:
        tp_entry(topo, TP_PEER, net->port[0], true);
        return NULL;
    default:
        return "unknown scenario";
    }
}

/* Send the row's update and check what the worker does with it. */
static const char *tp_run(struct tp_fixture *fx, struct tp_net *net,
                          enum tp_scenario scenario)
{
    TcpMeshTopologyPayload topo;
    const char *why = tp_topology(fx, net, scenario, &topo);
    double sent = now_s();
    int fd;

    if (why)
        return why;
    if (scenario == TP_NO_THREADS)
        atomic_store(&g_creates_left, 0);
    if (!tp_send(fx, &topo))
        return "could not send the topology update";

    switch (scenario) {
    case TP_CONNECTS:
        return tp_connected_to(fx, net, net->lfd[0], TP_PEER, &fd);
    case TP_SKIPS:
        return tp_skips(fx, net);
    case TP_STOP:
        return tp_stop(fx);
    case TP_STARVES:
        return tp_starves(fx, net, &topo, sent);
    case TP_RETRY:
        return tp_retry(fx, net);
    case TP_RACE:
        return tp_race(fx, net);
    case TP_NO_THREADS:
        return tp_no_threads(fx, net);
    case TP_DROPPED:
        return tp_dropped(fx, net);
    case TP_DIES:
        return tp_dies(fx, net);
    default:
        return "unknown scenario";
    }
}

/*
 * Run one topology row: set up the worker and the row's peers, send the
 * update and check what the worker does, then stop it as teardown does.
 * Whatever the row did, the stop has to return promptly, having joined every
 * thread that was started exactly once.
 */
static int run_tp_case(const struct tp_case *c)
{
    struct tp_fixture fx;
    struct tp_net net;
    const char *why;
    double took;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_store(&g_create_failures, 0);
    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);

    why = tp_setup(&fx, c->interval);
    if (!why)
        why = tp_listen(&net, c->scenario);
    else
        tp_listen(&net, TP_CONNECTS); /* so tp_close() has something valid */
    if (!why)
        why = tp_run(&fx, &net, c->scenario);
    atomic_store(&g_creates_left, -1);

    took = tp_teardown(&fx);
    alarm(0);
    if (!why && took >= 1.0)
        why = "the stop waited for the worker";
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s topology %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited, stop took "
               "%.3f s\n",
               why, atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits), took);
    fflush(stdout);

    tp_close(&net);
    return ok ? 0 : 1;
}

/* ---- A message that arrives in parts ----------------------------------- */

/*
 * tcp_recv_message() reads a message from a non-blocking socket, and returns
 * -EAGAIN when nothing has arrived.  It used to return -EAGAIN as well when
 * part of the header had arrived and the rest hadn't, and the bytes it had
 * read were lost: the next call started in the middle of the header, failed
 * the magic check, and the connection was dropped.  A header can straddle a
 * TCP segment boundary, so a busy connection would eventually hit this.
 *
 * Each row writes part of a stream of two messages to a socketpair before the
 * first call, and more of it 50 ms later from another thread, while a call is
 * waiting.  The rows check the messages read, then what the next call
 * returns: -EAGAIN once all that was sent has been read, so nothing was lost
 * or left behind.  Others clear the stop flag, let a deadline pass, or hang
 * up while a call waits for the rest, and that call must fail promptly.
 */

#define SP_HDR      ((uint32_t)sizeof(TcpMsgHeader))
#define SP_A_LEN    24u                 /* the first message's payload */
#define SP_B_LEN    16u                 /* the second's */
#define SP_A        (SP_HDR + SP_A_LEN) /* the first message, on the wire */
#define SP_ALL      (SP_A + SP_HDR + SP_B_LEN)
#define SP_LATER_MS 50

/* What the writer does after sending the later bytes */
enum sp_end {
    SP_NOTHING, /* nothing more */
    SP_STOP,    /* clears the receiver's running flag */
    SP_HANGUP,  /* shuts down its end */
};

struct sp_case {
    const char *category;
    const char *description;
    const char *expected;
    uint32_t first;       /* bytes of the stream sent before the first call */
    uint32_t later;       /* bytes sent after SP_LATER_MS */
    enum sp_end end;      /* and then */
    uint32_t deadline_ms; /* from the first call; 0 for none */
    int want_msgs;        /* messages that must be read, in order */
    int want_ret;         /* what the call after them returns */
};

static const struct sp_case sp_cases[] = {
    {"positive", "a whole message already there",
     "read at once; the next call finds nothing", SP_A, 0, SP_NOTHING, 0, 1,
     -EAGAIN},
    {"positive", "10 bytes of a header, the rest of the message 50 ms later",
     "the call waits for the rest and reads the message", 10, SP_A - 10,
     SP_NOTHING, 0, 1, -EAGAIN},
    {"boundary", "nothing there", "-EAGAIN at once, for the caller to wait", 0,
     0, SP_NOTHING, 0, 0, -EAGAIN},
    {"boundary", "all of a header but its last byte, then the rest",
     "the call waits for the last byte and reads the message", SP_HDR - 1,
     SP_A - (SP_HDR - 1), SP_NOTHING, 0, 1, -EAGAIN},
    {"boundary", "a header, then its payload 50 ms later",
     "the call waits for the payload and reads the message", SP_HDR, SP_A_LEN,
     SP_NOTHING, 0, 1, -EAGAIN},
    {"corner", "a message and 10 bytes of the next, then the rest",
     "both read, in order, the second after the wait", SP_A + 10,
     SP_ALL - SP_A - 10, SP_NOTHING, 0, 2, -EAGAIN},
    {"negative", "10 bytes of a header, then the stop flag cleared",
     "the call gives up promptly with -1", 10, 0, SP_STOP, 0, 0, -1},
    {"negative", "10 bytes of a header and a 200 ms deadline",
     "the call gives up at the deadline with -1", 10, 0, SP_NOTHING, 200, 0,
     -1},
    {"negative", "a header and part of its payload and a 200 ms deadline",
     "the call gives up at the deadline with -1", SP_HDR + 8, 0, SP_NOTHING,
     200, 0, -1},
    {"negative", "10 bytes of a header, then the peer hangs up",
     "the call gives up with -1", 10, 0, SP_HANGUP, 0, 0, -1},
};

/* The writer's half of a row: the rest of what the row sends, and the end */
struct sp_writer {
    int fd;
    const uint8_t *buf;
    uint32_t len;
    enum sp_end end;
    atomic_bool *running;
    bool ok;
};

static void *sp_write(void *opaque)
{
    struct sp_writer *w = opaque;

    sleep_ms(SP_LATER_MS);
    w->ok = w->len == 0 || send(w->fd, w->buf, w->len, 0) == (ssize_t)w->len;
    if (w->end == SP_STOP)
        atomic_store(w->running, false);
    else if (w->end == SP_HANGUP)
        w->ok = w->ok && shutdown(w->fd, SHUT_WR) == 0;
    return NULL;
}

/* Put a message on the wire at buf: header, then len bytes of fill. */
static void sp_put(uint8_t *buf, TcpMsgType type, uint32_t seq, uint32_t len,
                   uint8_t fill)
{
    TcpMsgHeader hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.msg_len = htonl(len);
    hdr.seq = htonl(seq);
    hdr.src_node_id = htonl(seq + 1);
    hdr.dst_node_id = htonl(seq + 2);
    hdr.src_qpn = htonl(seq + 3);
    hdr.dst_qpn = htonl(seq + 4);
    memcpy(buf, &hdr, sizeof(hdr));
    memset(buf + sizeof(hdr), fill, len);
}

/* Check a message read against what sp_put() wrote. */
static bool sp_matches(const TcpMsgHeader *hdr, const void *payload,
                       TcpMsgType type, uint32_t seq, uint32_t len,
                       uint8_t fill)
{
    if (hdr->magic != TCP_PROTOCOL_MAGIC || hdr->msg_type != type ||
        hdr->msg_len != len || hdr->seq != seq || hdr->src_node_id != seq + 1 ||
        hdr->dst_node_id != seq + 2 || hdr->src_qpn != seq + 3 ||
        hdr->dst_qpn != seq + 4 || !payload)
        return false;
    for (uint32_t i = 0; i < len; i++)
        if (((const uint8_t *)payload)[i] != fill)
            return false;
    return true;
}

/*
 * Read the row's messages and then one more call, which may wait for the
 * writer.  Returns why the row failed, or NULL.
 */
static const char *sp_read(const struct sp_case *c, int fd,
                           atomic_bool *running, int *ret)
{
    static const struct {
        TcpMsgType type;
        uint32_t seq, len;
        uint8_t fill;
    } want[] = {{TCP_MSG_HEARTBEAT, 100, SP_A_LEN, 0xa5},
                {TCP_MSG_HANDSHAKE, 200, SP_B_LEN, 0x5a}};
    uint64_t deadline_ms =
        c->deadline_ms ? tcp_monotonic_ms() + c->deadline_ms : 0;

    for (int i = 0; i <= c->want_msgs; i++) {
        TcpMsgHeader hdr;
        void *payload = NULL;
        const char *why = NULL;

        *ret = tcp_recv_message(fd, &hdr, &payload, NULL, running, deadline_ms);
        if (i == c->want_msgs)
            why = payload ? "the last call returned a payload" : NULL;
        else if (*ret != 0)
            why = "a message wasn't read";
        else if (!sp_matches(&hdr, payload, want[i].type, want[i].seq,
                             want[i].len, want[i].fill))
            why = "a message read isn't the one sent";
        g_free(payload);
        if (why)
            return why;
    }
    return NULL;
}

/*
 * Start the writer on the rest of stream, read the row's messages from fd
 * while it writes, and join it.  *took is how long the reads took.
 */
static const char *sp_run(const struct sp_case *c, int fd, int peer,
                          const uint8_t *stream, atomic_bool *running, int *ret,
                          double *took)
{
    struct sp_writer w = {peer,   stream + c->first, c->later,
                          c->end, running,           false};
    pthread_t writer;
    const char *why;
    double start;

    if (pthread_create(&writer, NULL, sp_write, &w) != 0)
        return "could not start the writer";
    start = now_s();
    why = sp_read(c, fd, running, ret);
    *took = now_s() - start;
    pthread_join(writer, NULL);
    if (!why && !w.ok)
        why = "the writer could not send";
    return why;
}

static int run_sp_case(const struct sp_case *c)
{
    uint8_t stream[SP_ALL];
    atomic_bool running;
    const char *why = NULL;
    int sv[2] = {-1, -1}, ret = 0;
    double took = 0.0;
    bool ok;

    /* The two messages sp_read() expects */
    sp_put(stream, TCP_MSG_HEARTBEAT, 100, SP_A_LEN, 0xa5);
    sp_put(stream + SP_A, TCP_MSG_HANDSHAKE, 200, SP_B_LEN, 0x5a);
    atomic_init(&running, true);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (c->first + c->later > SP_ALL) {
        why = "the row sends more than the stream";
    } else if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        why = "could not open a socketpair";
    } else if (fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK) <
                   0 ||
               (c->first &&
                send(sv[1], stream, c->first, 0) != (ssize_t)c->first)) {
        why = "could not send the first bytes";
    } else {
        why = sp_run(c, sv[0], sv[1], stream, &running, &ret, &took);
    }
    alarm(0);

    if (!why && ret != c->want_ret)
        why = "the last call returned the wrong value";
    /*
     * A wait that ends early, or on the writer, rather than at the deadline.
     * tcp_monotonic_ms() truncates to the millisecond, so the deadline can
     * fall just before start + deadline_ms.
     */
    if (!why && c->deadline_ms &&
        (took < (c->deadline_ms - 10) / 1000.0 ||
         took >= c->deadline_ms / 1000.0 + 1))
        why = "the call didn't give up at the deadline";
    if (!why && took >= 1.0 + (c->deadline_ms ? c->deadline_ms / 1000.0 : 0))
        why = "the call didn't give up promptly";

    ok = !why;
    printf("%s partial %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; last call returned %d (want %d), %.3f s\n", why, ret,
               c->want_ret, took);

    if (sv[0] >= 0)
        close(sv[0]);
    if (sv[1] >= 0)
        close(sv[1]);
    return ok ? 0 : 1;
}

/* ---- Connections that are done with ------------------------------------ */

/*
 * A manager kept every connection it accepted until teardown, or until a
 * reconnect replaced it.  A peer that connected and hung up without
 * registering -- a port probe, a load balancer's health check, a peer
 * refused for running an older protocol version -- left its descriptor in
 * CLOSE_WAIT and its receive thread unjoined, and so did a registered worker
 * that went away.  A peer that connected and said nothing kept a thread
 * waiting for it.  Enough of them ran the process out of descriptors, and
 * the accept thread then retried accept(), which failed at once, in a loop
 * that filled the disk with its log.
 *
 * The accept thread now reaps the pending connections that are done with
 * whenever a peer connects, and the health check on every pass: those whose
 * receive thread has exited, and those that haven't registered within
 * TCP_REGISTER_TIMEOUT_MS.  At most TCP_MAX_PENDING_CONNS wait at once, and a
 * peer that sends anything but a registration before registering is hung
 * up on, since a relay made for it could block the reaper.  The health
 * check also closes the connection of a registered node that hangs up, and
 * reports the node dead.  A failed accept() is retried after 100 ms.
 *
 * Each row starts a manager with tcp_init(), and its peers connect over
 * loopback, or, to register, are attached over socketpairs as in the fini
 * rows.  tcp_fini() then has to join every thread exactly once.
 */

enum reap_scenario {
    REAP_PROBES,  /* peers that connect and hang up */
    REAP_REFUSED, /* a peer refused for running an older protocol version */
    REAP_EARLY,   /* a peer that sends something else before registering */
    REAP_TIMEOUT, /* a peer that says nothing for the registration timeout */
    REAP_YOUNG,   /* a peer that has said nothing, with time left */
    REAP_CAP,     /* more peers waiting to register than the cap */
    REAP_NO_FDS,  /* a peer connects while the process is out of descriptors */
    REAP_HANGUP,  /* a registered worker hangs up */
};

struct reap_case {
    const char *category;
    const char *description;
    const char *expected;
    enum reap_scenario scenario;
    const char *interval; /* ERNIC_TCP_HEALTH_INTERVAL */
};

static const struct reap_case reap_cases[] = {
    {"positive", "100 peers that connect and hang up without registering",
     "each is closed, and its thread joined, by the time the next connects: "
     "one left waiting, and 2 descriptors more open, not 102",
     REAP_PROBES, "3600"},
    {"negative", "a peer refused for running an older protocol version",
     "sent the refusal, then closed by the next health-check pass",
     REAP_REFUSED, "1"},
    {"negative",
     "a peer that, before registering, sends an ionic message for a "
     "registered worker",
     "not relayed; the connection is closed by the next health-check pass",
     REAP_EARLY, "1"},
    {"negative", "a peer that has sent nothing for the registration timeout",
     "closed by the next health-check pass", REAP_TIMEOUT, "1"},
    {"boundary", "a peer that has sent nothing, with time left to register",
     "kept through two health-check passes", REAP_YOUNG, "1"},
    {"boundary", "64 peers waiting to register, then another",
     "the next is closed at once; once one of the 64 hangs up, the one after "
     "it is let in",
     REAP_CAP, "3600"},
    {"negative", "a peer that connects while the manager is out of descriptors",
     "accept() is retried every 100 ms, not in a loop that floods the log, "
     "and succeeds once descriptors are free",
     REAP_NO_FDS, "3600"},
    {"negative", "a registered worker that hangs up",
     "the next health-check pass closes its connection, joins its thread, "
     "takes it out of the connections table, and reports it dead to the "
     "other workers",
     REAP_HANGUP, "1"},
};

/* The peers' descriptors a row keeps open, closed at the end of the row */
#define REAP_MAX_FDS (TCP_MAX_PENDING_CONNS + 4u)

struct reap_fds {
    int fd[REAP_MAX_FDS];
    uint32_t n;
};

/* Keep fd, if it's open, to be closed at the end of the row; returns it. */
static int reap_keep(struct reap_fds *k, int fd)
{
    if (fd >= 0 && k->n < REAP_MAX_FDS)
        k->fd[k->n++] = fd;
    return fd;
}

/* How many descriptors the process has open, give or take a constant. */
static int open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    int n = 0;

    if (!dir)
        return -1;
    while (readdir(dir))
        n++;
    closedir(dir);
    return n;
}

/* Wait up to 5 s for n threads to have been created, or joined, in all. */
static bool wait_for_count(atomic_int *count, int n)
{
    for (int i = 0; i < 5000 && atomic_load(count) < n; i++)
        sleep_ms(1);
    return atomic_load(count) >= n;
}

static guint pending_count(TcpBackendPrivate *priv)
{
    guint n;

    qemu_mutex_lock(&priv->conn_table_lock);
    n = priv->pending_conns->len;
    qemu_mutex_unlock(&priv->conn_table_lock);
    return n;
}

/*
 * Connect to the manager, and wait for it to start the connection's receive
 * thread, the creates-th thread created.  Returns the peer's end, or -1.
 */
static int reap_connect(uint16_t port, int creates)
{
    int fd = connect_loopback(port);

    if (fd >= 0 && !wait_for_count(&g_creates, creates)) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * REAP_PROBES: once the threads have seen their peers hang up, the next peer
 * to connect has the accept thread reap them all.
 */
static const char *reap_probes(TcpBackendPrivate *priv, uint16_t port,
                               struct reap_fds *k)
{
    int base = atomic_load(&g_creates);
    int fds = open_fds();

    for (int i = 0; i < 100; i++) {
        int fd = reap_connect(port, base + i + 1);

        if (fd < 0)
            return "a peer wasn't given a receive thread";
        close(fd);
    }
    if (!wait_for_exits(100))
        return "a receive thread didn't see its peer hang up";
    if (reap_keep(k, reap_connect(port, base + 101)) < 0)
        return "the last peer wasn't given a receive thread";
    if (atomic_load(&g_joins) != 100)
        return "the threads of the peers that hung up weren't each joined once";
    if (pending_count(priv) != 1)
        return "peers that hung up are still waiting to register";
    return open_fds() - fds <= 2
               ? NULL
               : "the descriptors of the peers that hung up are still open";
}

/* REAP_REFUSED: the refusal ends the receive thread, so a pass reaps it. */
static const char *reap_refused(TcpBackendPrivate *priv, uint16_t port,
                                struct reap_fds *k)
{
    int fd = reap_keep(k, reap_connect(port, atomic_load(&g_creates) + 1));

    if (fd < 0)
        return "the peer wasn't given a receive thread";
    /* All zeros, so version 0 */
    if (!send_message(fd, TCP_MSG_REGISTER_NODE, TCP_REGISTER_NODE_LEN_V3,
                      TCP_REGISTER_NODE_LEN_V3))
        return "could not send the registration";
    if (!recv_type(fd, TCP_MSG_REGISTER_RESP, 2000))
        return "the registration wasn't refused";
    /* A pass comes within the 1 s interval, the timeout after 5 s */
    if (!closed_within(fd, 2500))
        return "the refused peer's connection was left open";
    if (!wait_for_count(&g_joins, 1))
        return "the refused peer's thread wasn't joined";
    return pending_count(priv) == 0 ? NULL
                                    : "the refused peer is still waiting";
}

/* Send an ionic message from node src to node dst, with an 8-byte payload. */
static bool send_ionic(int fd, uint32_t src, uint32_t dst)
{
    struct {
        TcpMsgHeader hdr;
        uint8_t payload[8];
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(TCP_MSG_IONIC);
    msg.hdr.msg_len = htonl((uint32_t)sizeof(msg.payload));
    msg.hdr.src_node_id = htonl(src);
    msg.hdr.dst_node_id = htonl(dst);
    return send(fd, &msg, sizeof(msg), 0) == (ssize_t)sizeof(msg);
}

/*
 * REAP_EARLY: a peer that hasn't registered asks the manager to relay an
 * ionic message to worker 1.  A relay to a worker that has stopped reading
 * would block the peer's receive thread, and the reaper that joins it.  The
 * message must not be relayed, and the peer is hung up on, so its thread
 * ends and the next pass closes it.  Anything relayed would be in the
 * worker's socket before the peer's thread ended.
 */
static const char *reap_early(TcpBackendPrivate *priv, uint16_t port,
                              struct reap_fds *k)
{
    int w = reap_keep(k, worker_register(priv));
    int fd;

    if (w < 0 || !wait_for_registered(priv, 1))
        return "the worker did not register";
    fd = reap_keep(k, reap_connect(port, atomic_load(&g_creates) + 1));
    if (fd < 0)
        return "the peer wasn't given a receive thread";
    if (!send_ionic(fd, 5, 1))
        return "could not send the ionic message";
    /* A pass comes within the 1 s interval, the timeout after 5 s */
    if (!closed_within(fd, 2500))
        return "the peer's connection was left open";
    if (!wait_for_count(&g_joins, 1))
        return "the peer's thread wasn't joined";
    if (count_type(w, TCP_MSG_IONIC) != 0)
        return "the peer's message was relayed";
    return pending_count(priv) == 0 ? NULL : "the peer is still waiting";
}

/*
 * REAP_TIMEOUT: the peer is made to look TCP_REGISTER_TIMEOUT_MS old, and
 * the next pass has to close it.
 */
static const char *reap_timeout(TcpBackendPrivate *priv, uint16_t port,
                                struct reap_fds *k)
{
    int fd = reap_keep(k, reap_connect(port, atomic_load(&g_creates) + 1));
    bool aged = false;

    if (fd < 0)
        return "the peer wasn't given a receive thread";
    qemu_mutex_lock(&priv->conn_table_lock);
    if (priv->pending_conns->len == 1) {
        TcpConnection *conn = g_ptr_array_index(priv->pending_conns, 0);

        if (conn->accepted_ms >= TCP_REGISTER_TIMEOUT_MS) {
            conn->accepted_ms -= TCP_REGISTER_TIMEOUT_MS;
            aged = true;
        }
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (!aged)
        return "could not age the peer's connection";
    if (!closed_within(fd, 2500))
        return "the peer's connection was left open";
    if (!wait_for_count(&g_joins, 1))
        return "the peer's thread wasn't joined";
    return pending_count(priv) == 0 ? NULL : "the peer is still waiting";
}

/*
 * REAP_YOUNG: a registered worker shows when passes run.  Its heartbeats are
 * drained once the peer is waiting, so the second that comes after is from
 * a pass that began after the peer connected.
 */
static const char *reap_young(TcpBackendPrivate *priv, uint16_t port,
                              struct reap_fds *k)
{
    struct pollfd pfd;
    int w = reap_keep(k, worker_register(priv));
    int fd;

    if (w < 0 || !wait_for_registered(priv, 1))
        return "the worker did not register";
    fd = reap_keep(k, reap_connect(port, atomic_load(&g_creates) + 1));
    if (fd < 0)
        return "the peer wasn't given a receive thread";
    count_type(w, TCP_MSG_HEARTBEAT);
    if (!wait_for_type(w, TCP_MSG_HEARTBEAT, 3000) ||
        !wait_for_type(w, TCP_MSG_HEARTBEAT, 3000))
        return "the health check didn't make two passes";
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 0 || pending_count(priv) != 1)
        return "the peer was closed with time left to register";
    return NULL;
}

/*
 * REAP_CAP: a peer past the cap is closed without a receive thread.  One of
 * those waiting hangs up, and the next peer takes its place.
 */
static const char *reap_cap(TcpBackendPrivate *priv, uint16_t port,
                            struct reap_fds *k)
{
    int base = atomic_load(&g_creates);
    int cap = (int)TCP_MAX_PENDING_CONNS;
    uint32_t first = k->n;

    for (int i = 0; i < cap; i++)
        if (reap_keep(k, reap_connect(port, base + i + 1)) < 0)
            return "a peer under the cap wasn't given a receive thread";
    if (!closed_within(reap_keep(k, connect_loopback(port)), 2000))
        return "the peer past the cap was kept";
    if (atomic_load(&g_creates) != base + cap)
        return "the peer past the cap was given a receive thread";

    close(k->fd[first]);
    k->fd[first] = -1;
    if (!wait_for_exits(1))
        return "the thread didn't see its peer hang up";
    if (reap_keep(k, reap_connect(port, base + cap + 1)) < 0)
        return "the peer after one hung up wasn't let in";
    if (atomic_load(&g_joins) != 1)
        return "the thread of the peer that hung up wasn't joined once";
    return pending_count(priv) == TCP_MAX_PENDING_CONNS
               ? NULL
               : "the wrong number of peers are waiting";
}

/*
 * REAP_NO_FDS: lower the descriptor limit until the peer's socket takes the
 * last one, so the manager's accept() fails with EMFILE while the
 * connection waits.  Each failure is logged once, so the log shows how often
 * it is retried.  *errors gets how many there were in 500 ms.
 */
static const char *reap_no_fds(uint16_t port, struct reap_fds *k, int *errors)
{
    struct rlimit old, low;
    int base = atomic_load(&g_creates);
    int lowest, fd;

    *errors = -1;
    if (getrlimit(RLIMIT_NOFILE, &old) < 0)
        return "could not read the descriptor limit";
    /*
     * The sanitizer runtime briefly takes a descriptor now and then, and
     * if it takes the last one first, the peer's socket() fails with
     * EMFILE.  Lift the limit and try again.
     */
    fd = -1;
    for (int attempt = 0; attempt < 5 && fd < 0; attempt++) {
        setrlimit(RLIMIT_NOFILE, &old);
        /* Every descriptor below the lowest free one is open */
        lowest = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (lowest < 0)
            return "could not open /dev/null";
        low = old;
        low.rlim_cur = (rlim_t)lowest + 1;
        if (setrlimit(RLIMIT_NOFILE, &low) < 0) {
            close(lowest);
            return "could not lower the descriptor limit";
        }
        close(lowest);
        fd = reap_keep(k, connect_loopback(port));
    }

    atomic_store(&g_errors, 0);
    sleep_ms(500);
    *errors = atomic_load(&g_errors);
    setrlimit(RLIMIT_NOFILE, &old);

    if (fd < 0)
        return "could not connect";
    if (*errors == 0)
        return "accept() didn't fail";
    if (*errors > 8)
        return "accept() was retried without a pause";
    /* The next retry has a descriptor to give it */
    return wait_for_count(&g_creates, base + 1)
               ? NULL
               : "the peer wasn't accepted once descriptors were free";
}

/*
 * Read messages from fd for up to timeout_ms, until a topology update that
 * reports node id alive, or dead.
 */
static bool reported(int fd, uint32_t id, bool alive, int timeout_ms)
{
    double end = now_s() + timeout_ms / 1000.0;

    while (now_s() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int left = (int)((end - now_s()) * 1000.0);
        TcpMsgHeader hdr;
        void *payload = NULL;
        bool found = false;

        if (poll(&pfd, 1, left > 0 ? left : 1) != 1)
            continue;
        if (tcp_recv_message(fd, &hdr, &payload, NULL, NULL, 0) < 0)
            return false;
        if (hdr.msg_type == TCP_MSG_MESH_TOPOLOGY && payload &&
            hdr.msg_len >= sizeof(TcpMeshTopologyPayload)) {
            const TcpMeshTopologyPayload *topo = payload;
            uint32_t n = ntohl(topo->num_nodes);

            for (uint32_t i = 0; i < n && i < G_N_ELEMENTS(topo->nodes); i++)
                if (ntohl(topo->nodes[i].node_id) == id &&
                    (topo->nodes[i].is_alive != 0) == alive)
                    found = true;
        }
        g_free(payload);
        if (found)
            return true;
    }
    return false;
}

/*
 * REAP_HANGUP: worker 2 hangs up.  A node is declared dead after three
 * intervals without a heartbeat, which can't be before 3 s here, so a report
 * sooner than that is from the hang-up.
 */
static const char *reap_hangup(TcpBackendPrivate *priv, struct reap_fds *k)
{
    TcpConnection *conn;
    MeshNodeInfo *node;
    const char *why = NULL;
    bool alive;
    double start;
    int w1 = reap_keep(k, worker_register(priv));
    int w2, fd;

    if (w1 < 0 || !wait_for_registered(priv, 1))
        return "worker 1 did not register";
    w2 = worker_register(priv);
    if (w2 < 0 || !wait_for_registered(priv, 2)) {
        if (w2 >= 0)
            close(w2);
        return "worker 2 did not register";
    }
    /* A reference, so the connection can be looked at once it's released */
    conn = tcp_connection_lookup(priv, 2);
    if (!conn)
        return "worker 2 isn't node 2";

    close(w2);
    start = now_s();
    if (!reported(w1, 2, false, 5000))
        why = "worker 1 wasn't told worker 2 is dead";
    else if (now_s() - start >= 2.5)
        why = "worker 2 was only declared dead once it went silent";

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(2));
    alive = node && node->is_alive;
    qemu_mutex_unlock(&priv->mesh_table_lock);
    qemu_mutex_lock(&conn->lock);
    fd = conn->sockfd;
    qemu_mutex_unlock(&conn->lock);

    if (!why && (!node || alive))
        why = "node 2 is gone, or still alive";
    else if (!why && tcp_has_connection(priv, 2))
        why = "worker 2's connection is still in the connections table";
    else if (!why && (fd != -1 || atomic_load(&conn->recv_thread_running)))
        why = "worker 2's connection wasn't closed";
    else if (!why && atomic_load(&g_joins) != 1)
        why = "worker 2's receive thread wasn't joined once";
    tcp_connection_unref(conn);
    return why;
}

static const char *reap_run(TcpBackendPrivate *priv, uint16_t port,
                            enum reap_scenario scenario, struct reap_fds *k,
                            int *errors)
{
    switch (scenario) {
    case REAP_PROBES:
        return reap_probes(priv, port, k);
    case REAP_REFUSED:
        return reap_refused(priv, port, k);
    case REAP_EARLY:
        return reap_early(priv, port, k);
    case REAP_TIMEOUT:
        return reap_timeout(priv, port, k);
    case REAP_YOUNG:
        return reap_young(priv, port, k);
    case REAP_CAP:
        return reap_cap(priv, port, k);
    case REAP_NO_FDS:
        return reap_no_fds(port, k, errors);
    case REAP_HANGUP:
        return reap_hangup(priv, k);
    default:
        return "unknown scenario";
    }
}

/*
 * Whether the test runs under valgrind.  Valgrind doesn't pass a change to
 * RLIMIT_NOFILE to the kernel but enforces the limit itself, and not as the
 * kernel does: an accept() already waiting when the limit is lowered gets a
 * descriptor over it, and one that starts afterwards fails with EMFILE but
 * takes the connection off the queue, so the retry finds nothing.  The
 * REAP_NO_FDS row can't run under it.
 */
static bool under_valgrind(void)
{
#ifdef ERNIC_HAVE_VALGRIND_H
    return RUNNING_ON_VALGRIND != 0;
#else
    return false;
#endif
}

static int run_reap_case(const struct reap_case *c)
{
    RdmaBackendDev dev;
    struct reap_fds k;
    char config[64];
    const char *why;
    uint16_t port;
    int errors = -1;
    double start, took;
    bool ok;

    if (c->scenario == REAP_NO_FDS && under_valgrind()) {
        printf("skip reap %s: %s; valgrind enforces the descriptor limit "
               "itself, so accept() can't be made to fail as it would\n",
               c->category, c->description);
        fflush(stdout);
        return 0;
    }

    port = free_port();
    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&k, 0, sizeof(k));
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", c->interval, 1);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port == 0 || tcp_init(&dev, config) != 0) {
        printf("FAIL reap %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    why = reap_run(dev.backend_private, port, c->scenario, &k, &errors);

    start = now_s();
    tcp_fini(&dev);
    took = now_s() - start;
    alarm(0);
    if (!why && took >= 2.0)
        why = "tcp_fini() waited";
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s reap %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited, %d "
               "error(s) logged, tcp_fini() took %.3f s\n",
               why, atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits), errors, took);
    fflush(stdout);

    for (uint32_t i = 0; i < k.n; i++)
        if (k.fd[i] >= 0)
            close(k.fd[i]);
    return ok ? 0 : 1;
}

/* ---- Which node a message is from -------------------------------------- */

/*
 * A message's header names the node that sent it, and the sender writes
 * that.  The manager used to credit a heartbeat to the node the header named,
 * so any registered worker could keep another, dead, node alive.  Every node
 * also sent its answer to a data-path request, a completion or the memory
 * asked for, to the node the request named, so a peer could have it
 * delivered to another node.  A heartbeat is now credited to the node whose
 * connection it came on, and an answer goes back on the connection the
 * request came on.
 *
 * Data that arrives before a receive is posted waits for one, and its
 * completion goes out when the receive is posted, from another thread.  That
 * one went to whichever connection the node the data named had by then.  It
 * now goes back on the connection the data came on, which the waiting data
 * holds a reference to: the pending rows check it reaches worker 1, and that
 * nothing is sent, and nothing is used after it is freed, if worker 1's
 * connection is retired while the data waits.  Teardown frees what is still
 * queued on a QP: the teardown rows leave data waiting, with worker 1's
 * connection in the table or retired, and check that teardown drops the
 * data's reference; and leave an RDMA write waiting for its completion and
 * a receive waiting for data, both mapped through a DMA memory region, and
 * check that teardown unmaps each mapping once.
 *
 * A node the manager had declared dead that answered a heartbeat again
 * stayed dead to the rest of the mesh until its topology next changed.  The
 * manager now reports it alive.
 *
 * Each row starts a manager with tcp_init(), on a one-hour interval so the
 * health check leaves the nodes alone, and registers two workers over
 * socketpairs, as in the fini rows: nodes 1 and 2.
 */

enum sender_scenario {
    SENDER_HEARTBEAT, /* worker 1's heartbeat names node 2, which is dead */
    SENDER_REVIVES,   /* node 2, declared dead, answers a heartbeat */
    SENDER_WRITE,     /* worker 1's RDMA write names node 2 as its sender */
    SENDER_READ,      /* worker 1's RDMA read names node 2 as its sender */
    SENDER_PENDING,   /* worker 1's data, naming node 2, waits for a receive */
    SENDER_PENDING_GONE,  /* and worker 1's connection is retired meanwhile */
    SENDER_TEARDOWN_KEPT, /* data waits, never received, at teardown */
    SENDER_TEARDOWN_RETIRED, /* and worker 1's connection is retired first */
    SENDER_TEARDOWN_QUEUED,  /* a write and a receive still queued */
};

struct sender_case {
    const char *category;
    const char *description;
    const char *expected;
    enum sender_scenario scenario;
};

static const struct sender_case sender_cases[] = {
    {"negative", "worker 1 sends a heartbeat naming node 2, which is dead",
     "credited to node 1; node 2 stays dead", SENDER_HEARTBEAT},
    {"positive", "node 2, declared dead, answers a heartbeat",
     "node 2 is alive again, and worker 1 is told so", SENDER_REVIVES},
    {"negative", "worker 1 sends an RDMA write naming node 2 as its sender",
     "the write lands, and the completion goes to worker 1, not worker 2",
     SENDER_WRITE},
    {"negative", "worker 1 asks to read memory, naming node 2 as its sender",
     "the memory goes to worker 1, not worker 2", SENDER_READ},
    {"negative",
     "worker 1 sends data naming node 2 as its sender, before a receive is "
     "posted",
     "once one is, the data lands and the completion goes to worker 1, not "
     "worker 2",
     SENDER_PENDING},
    {"corner",
     "worker 1 sends data before a receive is posted, and its connection is "
     "retired before one is",
     "the data lands, no completion is sent, and the connection is freed",
     SENDER_PENDING_GONE},
    {"corner",
     "worker 1 sends data before a receive is posted, and none is before "
     "teardown",
     "teardown frees the data and drops its reference to worker 1's "
     "connection, as well as the table's",
     SENDER_TEARDOWN_KEPT},
    {"corner",
     "worker 1 sends data before a receive is posted, its connection is "
     "retired, and no receive is posted before teardown",
     "teardown frees the data and drops the connection's last reference but "
     "the test's",
     SENDER_TEARDOWN_RETIRED},
    {"corner",
     "an RDMA write waits for worker 1's completion, and a receive for data, "
     "both mapped through a DMA memory region, at teardown",
     "teardown unmaps each mapping once and frees both requests",
     SENDER_TEARDOWN_QUEUED},
};

/* The memory region the RDMA rows write and read, at g_mr while they run */
#define SENDER_MR_START UINT64_C(0x10000)
#define SENDER_MR_RKEY  0x5eedu
#define SENDER_LEN      8u
#define SENDER_BYTE     0xab /* what the write writes */

#define SENDER_QPN      12u    /* the QP the requests are addressed to */
#define SENDER_DMA_KEY  0xd3au /* a DMA memory region's key */
#define SENDER_DMA_ADDR UINT64_C(0x20000)

static uint8_t g_sender_mem[SENDER_LEN];
static RdmaRmMR g_sender_mr;
static RdmaRmMR g_sender_dma_mr; /* starts at 0 with no length: DMA */
static TcpCQ g_sender_cq;        /* the QP these rows add has to have one */
/* A reference a teardown row keeps to worker 1's connection past tcp_fini() */
static TcpConnection *g_sender_held;

/* Set node id's liveness and the time of its last heartbeat. */
static void node_set(TcpBackendPrivate *priv, uint32_t id, bool alive,
                     time_t last)
{
    MeshNodeInfo *node;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(id));
    if (node) {
        node->is_alive = alive;
        node->last_heartbeat = last;
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);
}

/* Get them; false if there is no node id. */
static bool node_get(TcpBackendPrivate *priv, uint32_t id, bool *alive,
                     time_t *last)
{
    MeshNodeInfo *node;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(id));
    if (node) {
        *alive = node->is_alive;
        *last = node->last_heartbeat;
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);
    return node != NULL;
}

/*
 * Send an RDMA write of SENDER_LEN bytes of SENDER_BYTE to the region, or a
 * request to read SENDER_LEN bytes of it, naming node src as the sender.
 * The operation header is in host byte order, as the backend sends it.
 */
static bool send_rdma(int fd, TcpMsgType type, uint32_t src)
{
    TcpMsgHeader hdr;
    TcpRdmaOpHeader op;
    uint8_t buf[sizeof(hdr) + sizeof(op) + SENDER_LEN];
    size_t len =
        type == TCP_MSG_RDMA_WRITE ? sizeof(buf) : sizeof(hdr) + sizeof(op);

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.msg_len = htonl((uint32_t)(len - sizeof(hdr)));
    hdr.seq = htonl(1);
    hdr.src_node_id = htonl(src);
    hdr.src_qpn = htonl(11);
    hdr.dst_qpn = htonl(SENDER_QPN);
    memset(&op, 0, sizeof(op));
    op.remote_addr = SENDER_MR_START;
    op.rkey = SENDER_MR_RKEY;
    op.data_len = SENDER_LEN;
    memset(buf, SENDER_BYTE, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &op, sizeof(op));
    return send(fd, buf, len, 0) == (ssize_t)len;
}

/*
 * SENDER_HEARTBEAT: node 1 is alive but hasn't answered for a while, and
 * node 2 is dead.  The manager answers the heartbeat once it has recorded
 * it.
 */
static const char *sender_heartbeat(TcpBackendPrivate *priv, int w1)
{
    time_t dead_since = time(NULL) - 1000, before, last1, last2;
    bool alive1, alive2;

    node_set(priv, 1, true, dead_since);
    node_set(priv, 2, false, dead_since);
    before = time(NULL);
    if (!send_heartbeat(w1, 2))
        return "could not send the heartbeat";
    if (!wait_for_type(w1, TCP_MSG_HEARTBEAT_RESP, 2000))
        return "the heartbeat wasn't answered";
    if (!node_get(priv, 1, &alive1, &last1) ||
        !node_get(priv, 2, &alive2, &last2))
        return "a node is missing";
    if (alive2 || last2 != dead_since)
        return "node 2 was revived by a heartbeat that wasn't its own";
    return last1 >= before ? NULL
                           : "the heartbeat wasn't credited to node 1, whose "
                             "connection it came on";
}

/*
 * Wait for the manager to finish with everything the worker on fd has sent
 * it so far, its registration included.  The worker's receive thread
 * handles one message at a time, so once it has answered a heartbeat sent
 * now, the handlers before it have returned, broadcasts and all, and what
 * they sent is waiting in the workers' sockets.
 */
static bool worker_settled(int fd, uint32_t id)
{
    return send_heartbeat(fd, id) &&
           wait_for_type(fd, TCP_MSG_HEARTBEAT_RESP, 2000);
}

/*
 * SENDER_REVIVES: anything reporting node 2 alive after it is declared dead
 * has to be from the revival.  Either registration's broadcast can report
 * node 2 alive: wait_for_registered() returns once a connection is in the
 * table, before its registration has broadcast, so worker 1's can come
 * after node 2 has registered.  Both are finished and discarded first.
 */
static const char *sender_revives(TcpBackendPrivate *priv, int w1, int w2)
{
    time_t last;
    bool alive;

    if (!worker_settled(w1, 1) || !worker_settled(w2, 2))
        return "a registration wasn't finished";
    count_type(w1, TCP_MSG_MESH_TOPOLOGY);
    node_set(priv, 2, false, time(NULL) - 1000);
    if (!send_heartbeat(w2, 2))
        return "could not send the heartbeat";
    if (!reported(w1, 2, true, 2000))
        return "worker 1 wasn't told node 2 is alive again";
    if (!node_get(priv, 2, &alive, &last) || !alive)
        return "node 2 is still dead";
    return NULL;
}

/* SENDER_WRITE and SENDER_READ */
static const char *sender_rdma(int w1, int w2, enum sender_scenario scenario)
{
    bool is_write = scenario == SENDER_WRITE;
    TcpMsgType answer = is_write ? TCP_MSG_COMPLETION : TCP_MSG_RDMA_READ_RESP;

    if (!send_rdma(w1, is_write ? TCP_MSG_RDMA_WRITE : TCP_MSG_RDMA_READ_REQ,
                   2))
        return "could not send the request";
    if (!wait_for_type(w1, answer, 2000))
        return "the answer didn't come back to worker 1";
    if (count_type(w2, answer) != 0)
        return "the answer went to worker 2";
    if (is_write) {
        for (uint32_t i = 0; i < SENDER_LEN; i++)
            if (g_sender_mem[i] != SENDER_BYTE)
                return "the write didn't land";
    }
    return NULL;
}

/* Send SENDER_LEN bytes of SENDER_BYTE as data for SENDER_QPN from node src */
static bool send_data(int fd, uint32_t src)
{
    TcpMsgHeader hdr;
    uint8_t buf[sizeof(hdr) + SENDER_LEN];

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(TCP_MSG_DATA);
    hdr.msg_len = htonl(SENDER_LEN);
    hdr.seq = htonl(1);
    hdr.src_node_id = htonl(src);
    hdr.src_qpn = htonl(11);
    hdr.dst_qpn = htonl(SENDER_QPN);
    memset(buf, SENDER_BYTE, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));
    return send(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf);
}

/*
 * Add SENDER_QPN to the manager's QPs, connected to node remote, as
 * tcp_create_qp() and an RTR would.
 */
static void sender_qp_add(TcpBackendPrivate *priv, uint32_t remote)
{
    TcpQP *tqp = g_new0(TcpQP, 1);

    tqp->qpn = SENDER_QPN;
    tqp->backend_dev = priv->backend_dev;
    tqp->remote_node_id = remote;
    tqp->scq = &g_sender_cq;
    tqp->rcq = &g_sender_cq;
    tqp->send_queue = g_queue_new();
    tqp->recv_queue = g_queue_new();
    tqp->pending_data = g_queue_new();
    qemu_mutex_init(&tqp->lock);
    qemu_mutex_lock(&priv->lock);
    g_hash_table_insert(priv->qps, GUINT_TO_POINTER(SENDER_QPN), tqp);
    qemu_mutex_unlock(&priv->lock);
}

/*
 * Take node id's connection out of the table and retire it, as the health
 * check does with one that has hung up.
 */
static bool retire_node_conn(TcpBackendPrivate *priv, uint32_t id)
{
    TcpConnection *conn;
    bool taken = false;

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(id));
    if (conn)
        taken = tcp_conn_table_take(priv, id, conn);
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (taken) {
        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
    }
    return taken;
}

/* How many requests or buffers wait on each of SENDER_QPN's queues */
static void sender_queued(TcpBackendPrivate *priv, guint *sends, guint *recvs,
                          guint *pending)
{
    TcpQP *tqp;

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(SENDER_QPN));
    *sends = tqp ? g_queue_get_length(tqp->send_queue) : 0;
    *recvs = tqp ? g_queue_get_length(tqp->recv_queue) : 0;
    *pending = tqp ? g_queue_get_length(tqp->pending_data) : 0;
    qemu_mutex_unlock(&priv->lock);
}

/*
 * Have worker 1's data, naming node 2 as its sender, wait for a receive on
 * SENDER_QPN.  The manager has finished with the data once it has answered a
 * heartbeat sent after it.
 */
static const char *sender_buffer(TcpBackendPrivate *priv, int w1)
{
    guint sends, recvs, pending;

    sender_qp_add(priv, 0);
    if (!send_data(w1, 2))
        return "could not send the data";
    if (!worker_settled(w1, 1))
        return "the data wasn't taken";
    sender_queued(priv, &sends, &recvs, &pending);
    return pending == 1 ? NULL : "the data isn't waiting for a receive";
}

/* SENDER_PENDING and SENDER_PENDING_GONE */
static const char *sender_pending(TcpBackendPrivate *priv, int w1, int w2,
                                  enum sender_scenario scenario)
{
    RdmaBackendQP qp;
    struct ibv_sge sge;
    const char *why = sender_buffer(priv, w1);

    if (why)
        return why;
    if (scenario == SENDER_PENDING_GONE && !retire_node_conn(priv, 1))
        return "worker 1's connection wasn't in the table";
    count_type(w2, TCP_MSG_COMPLETION);

    memset(&qp, 0, sizeof(qp));
    qp.ibqp = (struct ibv_qp *)(uintptr_t)SENDER_QPN;
    memset(&sge, 0, sizeof(sge));
    sge.addr = SENDER_MR_START;
    sge.length = SENDER_LEN;
    sge.lkey = SENDER_MR_RKEY;
    tcp_post_recv(priv->backend_dev, &qp, IBV_QPT_RC, &sge, 1, NULL);

    for (uint32_t i = 0; i < SENDER_LEN; i++)
        if (g_sender_mem[i] != SENDER_BYTE)
            return "the data didn't land";
    if (count_type(w2, TCP_MSG_COMPLETION) != 0)
        return "the completion went to worker 2";
    /* Sent before tcp_post_recv() returned, if at all */
    if (scenario == SENDER_PENDING_GONE)
        return count_type(w1, TCP_MSG_COMPLETION) == 0
                   ? NULL
                   : "a completion went out on a retired connection";
    return wait_for_type(w1, TCP_MSG_COMPLETION, 2000)
               ? NULL
               : "the completion didn't come back to worker 1";
}

/*
 * SENDER_TEARDOWN_KEPT and SENDER_TEARDOWN_RETIRED.  The row keeps a
 * reference to worker 1's connection in g_sender_held, which must be the
 * only one left once tcp_fini() has freed the waiting data.
 */
static const char *sender_teardown_pending(TcpBackendPrivate *priv, int w1,
                                           enum sender_scenario scenario)
{
    const char *why = sender_buffer(priv, w1);

    if (why)
        return why;
    g_sender_held = tcp_connection_lookup(priv, 1);
    if (!g_sender_held)
        return "worker 1 has no connection";
    if (scenario == SENDER_TEARDOWN_RETIRED && !retire_node_conn(priv, 1))
        return "worker 1's connection wasn't in the table";
    return NULL;
}

/*
 * SENDER_TEARDOWN_QUEUED.  A receive with no data waiting stays queued, and
 * a request to worker 1, which never answers, waits on the send queue for
 * its completion.  Each maps its one entry through the DMA region.  With no
 * completion context the request's opcode is 0, PVRDMA_WR_RDMA_WRITE, so it
 * goes out as an RDMA write.
 */
static const char *sender_teardown_queued(TcpBackendPrivate *priv, int w1)
{
    RdmaBackendQP qp;
    struct ibv_sge sge;
    guint sends, recvs, pending;

    atomic_store(&g_mr, &g_sender_dma_mr);
    sender_qp_add(priv, 1);
    memset(&qp, 0, sizeof(qp));
    qp.ibqp = (struct ibv_qp *)(uintptr_t)SENDER_QPN;
    memset(&sge, 0, sizeof(sge));
    sge.addr = SENDER_DMA_ADDR;
    sge.length = SENDER_LEN;
    sge.lkey = SENDER_DMA_KEY;

    tcp_post_recv(priv->backend_dev, &qp, IBV_QPT_RC, &sge, 1, NULL);
    tcp_post_send(priv->backend_dev, &qp, IBV_QPT_RC, &sge, 1, 0, NULL, NULL, 0,
                  0, NULL);
    if (!wait_for_type(w1, TCP_MSG_RDMA_WRITE, 2000))
        return "the write didn't reach worker 1";
    sender_queued(priv, &sends, &recvs, &pending);
    if (sends != 1 || recvs != 1)
        return "the write and the receive aren't both queued";
    return atomic_load(&g_dma_maps) == 2 ? NULL
                                         : "the requests weren't both mapped";
}

static const char *sender_run(TcpBackendPrivate *priv, int w1, int w2,
                              enum sender_scenario scenario)
{
    switch (scenario) {
    case SENDER_HEARTBEAT:
        return sender_heartbeat(priv, w1);
    case SENDER_REVIVES:
        return sender_revives(priv, w1, w2);
    case SENDER_WRITE:
    case SENDER_READ:
        return sender_rdma(w1, w2, scenario);
    case SENDER_PENDING:
    case SENDER_PENDING_GONE:
        return sender_pending(priv, w1, w2, scenario);
    case SENDER_TEARDOWN_KEPT:
    case SENDER_TEARDOWN_RETIRED:
        return sender_teardown_pending(priv, w1, scenario);
    case SENDER_TEARDOWN_QUEUED:
        return sender_teardown_queued(priv, w1);
    default:
        return "unknown scenario";
    }
}

static int run_sender_case(const struct sender_case *c)
{
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    char config[64];
    const char *why;
    uint16_t port = free_port();
    int w1 = -1, w2 = -1;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(g_sender_mem, 0, sizeof(g_sender_mem));
    memset(&g_sender_mr, 0, sizeof(g_sender_mr));
    g_sender_mr.virt = g_sender_mem;
    g_sender_mr.start = SENDER_MR_START;
    g_sender_mr.length = sizeof(g_sender_mem);
    g_sender_mr.rkey = SENDER_MR_RKEY;
    memset(&g_sender_dma_mr, 0, sizeof(g_sender_dma_mr));
    g_sender_dma_mr.rkey = SENDER_DMA_KEY;
    atomic_store(&g_mr, &g_sender_mr);
    dma_reset();
    g_sender_held = NULL;
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", "3600", 1);

    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    if (port == 0 || tcp_init(&dev, config) != 0) {
        printf("FAIL sender %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        atomic_store(&g_mr, NULL);
        return 1;
    }
    priv = dev.backend_private;

    w1 = worker_register(priv);
    if (w1 < 0 || !wait_for_registered(priv, 1))
        why = "worker 1 did not register";
    else if ((w2 = worker_register(priv)) < 0 || !wait_for_registered(priv, 2))
        why = "worker 2 did not register";
    else
        why = sender_run(priv, w1, w2, c->scenario);

    tcp_fini(&dev);
    alarm(0);
    atomic_store(&g_mr, NULL);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";
    if (!why && !dma_unmapped_once())
        why = "a DMA mapping wasn't unmapped exactly once";
    if (g_sender_held) {
        if (!why && atomic_load(&g_sender_held->refcount) != 1)
            why = "teardown left a reference to worker 1's connection";
        /* Frees it; LeakSanitizer reports it, and the data, otherwise */
        tcp_connection_unref(g_sender_held);
        g_sender_held = NULL;
    }

    ok = !why;
    printf("%s sender %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s\n", why);
    fflush(stdout);

    if (w1 >= 0)
        close(w1);
    if (w2 >= 0)
        close(w2);
    return ok ? 0 : 1;
}

/* ---- Connection lifetime ---------------------------------------------- */

/*
 * A lookup used to return a bare pointer to a connection, and the health
 * check's reconnect freed the node's old connection by replacing it in the
 * table, so a thread that had looked it up went on to use freed memory.
 * Connections are reference counted now: a lookup returns a reference, the
 * table holds one, and a reconnect takes the old connection out of the table,
 * retires it, and drops the table's, leaving it to the last holder to free.
 *
 * The reconnect rows run tcp_health_check_reconnect() on node 1 of an
 * rc_fixture, at a listener that answers, while the test holds a reference
 * to the old connection, or while another thread is blocked sending on it.
 * The registration row has a pending connection register while teardown is
 * stopping its receive thread.
 */

enum lt_scenario {
    LT_HELD,   /* the test holds the old connection across the reconnect */
    LT_SENDER, /* another thread is blocked sending on it */
    LT_REGISTER_VS_STOP, /* a registration lands while the stop runs */
};

struct lt_case {
    const char *category;
    const char *description;
    const char *expected;
    enum lt_scenario scenario;
};

static const struct lt_case lt_cases[] = {
    {"positive",
     "a reconnect replaces a connection the test holds a reference to",
     "the old connection is retired and out of the table, and stays readable "
     "until the reference is dropped",
     LT_HELD},
    {"negative",
     "a reconnect replaces a connection another thread is blocked sending on",
     "the send fails with -EIO, nothing hangs, and the new connection is in "
     "the table",
     LT_SENDER},
    {"corner",
     "a pending connection registers while teardown stops its receive thread",
     "it ends up in the connections table, not the pending list, and its "
     "thread is joined once",
     LT_REGISTER_VS_STOP},
};

/* What the blocked sender of LT_SENDER sends with, and gets back */
struct lt_sender {
    RdmaBackendDev dev;
    pthread_t tid;
    atomic_int rc;
};

static void *lt_send(void *opaque)
{
    static const uint8_t hdr[16];
    struct lt_sender *sender = opaque;

    atomic_store(&sender->rc, tcp_backend_send_ionic_v(&sender->dev, 1, hdr,
                                                       sizeof(hdr), NULL, 0));
    return NULL;
}

/*
 * Reconnect to node 1 of fx, at lfd, and check the node has a new
 * connection, other than old, in the table, which sent its handshake.
 * *fresh gets the listener's end of it, for the caller to close.
 */
static const char *lt_reconnect(struct rc_fixture *fx, TcpConnection *old,
                                int lfd, int *fresh)
{
    TcpHealthCheckItem item = {fx->node[0], NULL, true, false};
    TcpConnection *in_table;
    bool reconnected;

    /* The scan's reference, released at the end of the pass */
    item.conn = tcp_connection_lookup(fx->priv, 1);
    fx->node[0]->last_heartbeat = time(NULL) - 4 * LONG_INTERVAL_S;
    /* The connect gives up when this is cleared; no thread is started */
    atomic_store(&fx->priv->health_check_running, true);
    reconnected =
        tcp_health_check_reconnect(fx->priv, &item, tcp_monotonic_ms() + 5000);
    atomic_store(&fx->priv->health_check_running, false);
    tcp_connection_unref(item.conn);

    if (!reconnected)
        return "the reconnect failed";
    *fresh = accept_within(lfd);
    if (*fresh < 0 || !recv_type(*fresh, TCP_MSG_HANDSHAKE, 5000))
        return "the new connection sent no handshake";
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    in_table = g_hash_table_lookup(fx->priv->connections, GUINT_TO_POINTER(1));
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    return in_table && in_table != old ? NULL
                                       : "the node has no new connection";
}

/* Whether conn is retired: closed, marked down, and its thread stopped. */
static bool lt_retired(TcpConnection *conn)
{
    int fd;

    qemu_mutex_lock(&conn->lock);
    fd = conn->sockfd;
    qemu_mutex_unlock(&conn->lock);
    return fd == -1 && !atomic_load(&conn->is_connected) &&
           !atomic_load(&conn->recv_thread_running);
}

/*
 * LT_HELD and LT_SENDER.  The fixture's reference to the old connection is
 * dropped first, so that the table's and the test's are the only ones, and
 * a reconnect that freed the connection would leave the test reading freed
 * memory, which ASan reports.
 */
static const char *lt_reconnect_run(enum lt_scenario scenario)
{
    struct rc_fixture fx;
    struct sockaddr_in addr;
    struct lt_sender sender;
    socklen_t len = sizeof(addr);
    TcpConnection *held = NULL;
    const char *why;
    int lfd = listen_loopback(), fresh = -1;
    bool setup = false, sending = false;

    memset(&sender, 0, sizeof(sender));
    atomic_init(&sender.rc, 1);
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
        why = "could not open a listening socket";
    } else {
        why = rc_setup(&fx, ntohs(addr.sin_port), 1, LONG_INTERVAL_S);
        setup = true;
    }

    if (!why) {
        held = tcp_connection_lookup(fx.priv, 1);
        tcp_connection_unref(fx.old[0]);
        fx.old[0] = NULL;
        if (!held)
            why = "node 1 has no connection";
    }
    if (!why && scenario == LT_SENDER) {
        sender.dev.backend_type = RDMA_BACKEND_TYPE_TCP;
        sender.dev.backend_private = fx.priv;
        /*
         * Node 1 has stopped reading, so the send waits for room, holding
         * the connection's lock, and nothing else ever has it.
         */
        if (!fill_conn(fx.priv, 1))
            why = "could not fill node 1's socket";
        else if (pthread_create(&sender.tid, NULL, lt_send, &sender) != 0)
            why = "could not start the sender";
        else
            sending = true;
        if (sending && !held_elsewhere(&held->lock))
            why = "the sender never blocked on node 1";
    }
    if (!why)
        why = lt_reconnect(&fx, held, lfd, &fresh);
    if (sending) {
        pthread_join(sender.tid, NULL);
        if (!why && atomic_load(&sender.rc) != -EIO)
            why = "the blocked send didn't fail with -EIO";
    }
    if (!why && !lt_retired(held))
        why = "the old connection wasn't retired";
    if (!why && atomic_load(&g_joins) != 1)
        why = "the old receive thread wasn't joined exactly once";

    tcp_connection_unref(held);
    if (setup)
        rc_teardown(&fx);
    if (fresh >= 0)
        close(fresh);
    if (lfd >= 0)
        close(lfd);
    return why;
}

/* Wait up to 5 s for the manager to have read everything sent on conn. */
static bool lt_consumed(TcpConnection *conn)
{
    for (int i = 0; i < 500; i++) {
        int unread = -1;

        if (ioctl(conn->sockfd, FIONREAD, &unread) == 0 && unread == 0)
            return true;
        sleep_ms(10);
    }
    return false;
}

static void *lt_stop(void *opaque)
{
    tcp_stop_recv_threads(opaque);
    return NULL;
}

/*
 * LT_REGISTER_VS_STOP.  The test holds mesh_table_lock, which registration
 * needs, while the receive thread reads a REGISTER_NODE: once the message is
 * read, the thread handles it whatever its stop flag says.  The stop then
 * runs, on another thread, and claims the receive thread while the connection
 * is still pending; only then is the lock let go, and the registration moves
 * the connection while the stop waits to join its thread.  The health-check
 * and accept threads are stopped first, as teardown stops them.
 */
static const char *lt_register_vs_stop(void)
{
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    TcpConnection *conn = NULL, *in_table;
    char config[64];
    const char *why = NULL;
    pthread_t stopper;
    uint16_t port = free_port();
    guint pending;
    int fd = -1, joins;

    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    /* No backend threads exist yet, so nothing reads the environment */
    setenv("ERNIC_TCP_HEALTH_INTERVAL", "3600", 1);
    if (port == 0 || tcp_init(&dev, config) != 0)
        return "could not start a manager";
    priv = dev.backend_private;
    tcp_stop_health_check_thread(priv);
    tcp_stop_accept_thread(priv);

    fd = worker_attach(priv, &conn);
    if (fd < 0) {
        why = "could not attach the worker";
        goto out;
    }
    joins = atomic_load(&g_joins);
    qemu_mutex_lock(&priv->mesh_table_lock);
    if (!send_register(fd, "127.0.0.1", UINT32_MAX) || !lt_consumed(conn)) {
        qemu_mutex_unlock(&priv->mesh_table_lock);
        why = "the registration wasn't read";
        goto out;
    }
    if (pthread_create(&stopper, NULL, lt_stop, priv) != 0) {
        qemu_mutex_unlock(&priv->mesh_table_lock);
        why = "could not start the stop";
        goto out;
    }
    for (int i = 0; i < 500 && atomic_load(&conn->recv_thread_running); i++)
        sleep_ms(10);
    if (atomic_load(&conn->recv_thread_running))
        why = "the stop never claimed the receive thread";
    qemu_mutex_unlock(&priv->mesh_table_lock);
    pthread_join(stopper, NULL);

    qemu_mutex_lock(&priv->conn_table_lock);
    in_table = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(1));
    pending = priv->pending_conns->len;
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (!why && in_table != conn)
        why = "the connection isn't in the connections table";
    else if (!why && pending != 0)
        why = "the connection is still on the pending list";
    else if (!why && atomic_load(&g_joins) - joins != 1)
        why = "the receive thread wasn't joined exactly once";

out:
    /* Joins nothing more; ASan reports it if the connection is freed twice */
    tcp_fini(&dev);
    if (fd >= 0)
        close(fd);
    return why;
}

static int run_lt_case(const struct lt_case *c)
{
    const char *why;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    why = c->scenario == LT_REGISTER_VS_STOP ? lt_register_vs_stop()
                                             : lt_reconnect_run(c->scenario);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s lifetime %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited\n", why,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));
    fflush(stdout);
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
    for (size_t i = 0; i < sizeof(hs_cases) / sizeof(hs_cases[0]); i++)
        failures += run_hs_case(&hs_cases[i]);
    failures += run_reconnect_case();
    for (size_t i = 0; i < sizeof(connect_cases) / sizeof(connect_cases[0]);
         i++)
        failures += run_connect_case(&connect_cases[i]);
    for (size_t i = 0; i < sizeof(rc_cases) / sizeof(rc_cases[0]); i++)
        failures += run_rc_case(&rc_cases[i]);
    failures += run_queue_case();
    for (size_t i = 0; i < sizeof(reg_cases) / sizeof(reg_cases[0]); i++)
        failures += run_reg_case(&reg_cases[i]);
    for (size_t i = 0; i < sizeof(tp_cases) / sizeof(tp_cases[0]); i++)
        failures += run_tp_case(&tp_cases[i]);
    for (size_t i = 0; i < sizeof(sp_cases) / sizeof(sp_cases[0]); i++)
        failures += run_sp_case(&sp_cases[i]);
    for (size_t i = 0; i < sizeof(reap_cases) / sizeof(reap_cases[0]); i++)
        failures += run_reap_case(&reap_cases[i]);
    for (size_t i = 0; i < sizeof(sender_cases) / sizeof(sender_cases[0]); i++)
        failures += run_sender_case(&sender_cases[i]);
    for (size_t i = 0; i < sizeof(lt_cases) / sizeof(lt_cases[0]); i++)
        failures += run_lt_case(&lt_cases[i]);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
