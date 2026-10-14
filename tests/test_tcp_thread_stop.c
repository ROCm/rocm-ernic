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
static atomic_int g_joins;
static atomic_int g_exits;

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
    struct trampoline *t = malloc(sizeof(*t));

    (void)name;
    (void)mode;
    if (!t)
        return -1;
    t->fn = start_routine;
    t->arg = arg;
    if (pthread_create(&thread->thread, NULL, trampoline_run, t) != 0) {
        free(t);
        return -1;
    }
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
     "a heartbeat each pass, about 1 s apart; the stop returns at once",
     BLOCKED, 1, true, 1, 1, true},
    {"negative", "stop when the thread was never started", "a no-op: no join",
     NOT_STARTED, LONG_INTERVAL_S, false, 1, 0, false},
    {"boundary", "stop straight after the thread is created",
     "the stop isn't lost if it lands before the thread starts waiting",
     JUST_STARTED, LONG_INTERVAL_S, false, 1, 1, true},
    {"corner", "stop twice", "the second stop is a no-op: still one join",
     BLOCKED, LONG_INTERVAL_S, false, 2, 1, true},
};

/*
 * Wait up to timeout_ms for one message on fd and check that it's a
 * heartbeat.  Returns its arrival time, or a negative value.
 */
static double recv_heartbeat(int fd, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    TcpMsgHeader hdr;
    void *payload = NULL;
    double at;

    if (poll(&pfd, 1, timeout_ms) != 1)
        return -1.0;
    at = now_s();
    if (tcp_recv_message(fd, &hdr, &payload, NULL) < 0)
        return -1.0;
    g_free(payload);
    return hdr.msg_type == TCP_MSG_HEARTBEAT ? at : -1.0;
}

static int run_health_case(const struct health_case *c)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);
    TcpConnection *conn = NULL;
    int sv[2] = {-1, -1};
    double start, took, first = -1.0, second = -1.0;
    bool ok = true;

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

    /*
     * The first pass sends at once and the second after one interval.  Only
     * a lower bound on the gap is checked, loosely, so a slow runner can't
     * fail the row: a wait that returned early would send them back to back.
     */
    if (c->peer) {
        first = recv_heartbeat(sv[1], 5000);
        second = first < 0 ? -1.0 : recv_heartbeat(sv[1], 5000);
    }

    start = now_s();
    for (int i = 0; i < c->stops; i++)
        tcp_stop_health_check_thread(priv);
    took = now_s() - start;
    alarm(0);

    ok = atomic_load(&g_joins) == c->want_joins &&
         atomic_load(&g_exits) == (c->want_exited ? 1 : 0) &&
         !atomic_load(&priv->health_check_running) && took < 2.0 &&
         (!c->peer || (first >= 0 && second >= 0 && second - first >= 0.5));

    printf("%s health %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     got %d join(s) (want %d), exited %d (want %d), "
               "stop took %.3f s, heartbeat gap %.3f s\n",
               atomic_load(&g_joins), c->want_joins, atomic_load(&g_exits),
               (int)c->want_exited, took,
               first >= 0 && second >= 0 ? second - first : -1.0);

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

struct recv_case {
    const char *category;
    const char *description;
    const char *expected;
    uint32_t table;       /* registered connections, threads running */
    uint32_t pending;     /* pending connections, threads running */
    bool manager;         /* a manager connection, thread running */
    bool first_exited;    /* the first table thread returns before the stop */
    bool one_not_started; /* plus a table connection with no thread */
    int stops;            /* how many times to call the stop */
    int want_joins;
};

static const struct recv_case recv_cases[] = {
    {"positive", "two registered, one pending and a manager connection",
     "every thread is stopped and joined once", 2, 1, true, false, false, 1, 4},
    {"negative", "no connections at all", "a no-op: no join", 0, 0, false,
     false, false, 1, 0},
    {"boundary", "a thread that already returned when its peer hung up",
     "still joined, once, along with the running one", 2, 0, false, true, false,
     1, 2},
    {"corner", "a connection whose thread was never started",
     "not joined; the running one is", 1, 0, false, false, true, 1, 1},
    {"corner", "stop twice", "the second stop is a no-op: no more joins", 1, 1,
     false, false, false, 2, 2},
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
    *peer = sv[1];
    if (start) {
        atomic_store(&conn->recv_thread_running, true);
        qemu_thread_create(&conn->recv_thread, "tcp-recv",
                           tcp_recv_thread_per_conn, conn,
                           QEMU_THREAD_JOINABLE);
    }
    return conn;
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
    qemu_mutex_destroy(&priv->conn_table_lock);
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

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
