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
 * the stop needs first.  A broadcast used to send under conn_table_lock,
 * which tcp_stop_recv_threads() takes before it shuts anything down, and the
 * health-check thread, joined before any socket is shut down, could be
 * blocked in a heartbeat.  tcp_stop_threads() therefore sets priv->stopping
 * before any join, and a send that would have to wait gives up instead.
 * Broadcasts now send with conn_table_lock dropped, and the health check
 * skips a node it would have to wait for.  Later rows run tcp_init() and
 * tcp_fini() on a manager with a worker that stops reading, and check that
 * lookups and the heartbeats to another worker go on meanwhile.
 *
 * Every stop joins its thread only if the thread's flag is set, so a thread
 * that couldn't be created must leave its flag clear; a join of a thread
 * that doesn't exist is undefined behavior.  The create rows make
 * qemu_thread_create() fail in tcp_init() and in the accept thread.
 *
 * The manager used to reconnect to a node that stopped answering, and the
 * node then ignored everything the manager sent on that connection.  It now
 * closes a silent node's connection, which the silent rows check, and the
 * node registers again under its id, which the registering-again rows
 * check.  The connect rows check tcp_connect_to_remote(), which waits for the
 * connect, on its own, including a deadline shared by several addresses.
 *
 * A worker's accept thread waits for a handshake from each peer that
 * connects, and has to stop waiting when it is stopped.  The handshake rows
 * check that, and also send it handshakes it has to refuse.
 *
 * A manager must register a connection only once, or teardown frees it
 * once for each time.  The registration rows send a second REGISTER_NODE
 * on the same connection.
 *
 * A manager never dropped a node from its table, so once more nodes had
 * registered than its broadcasts carry, it left nodes out of them.  The
 * eviction rows fill the table and check that a new node evicts the node
 * dead longest that can't be heard from again, and nothing else, that a new
 * node is refused when there is no such node, and that a health-check pass
 * reads nothing of a node evicted under it.
 *
 * A worker must not hold the connection table's lock while it connects to
 * the peers in a topology update, since the data path takes it.  The
 * topology rows name peers that don't answer, and check that the lock is
 * free meanwhile, that a stop or the interval ends the wait, and that a
 * connection the peer opened meanwhile is kept.
 *
 * A worker must take its node id only from the manager, only once, and must
 * not introduce itself to its peers until it has it.  The worker
 * registration rows run tcp_init() as a worker against a manager played by
 * the test.  The manager refuses the worker or answers it twice, a peer
 * answers instead, or a peer connects, or a topology update arrives, before
 * the manager answers.
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
 * wrote.  The sender rows name another node in a heartbeat, and check that
 * the heartbeat is credited by connection instead.  They also check that a
 * dead node that answers a heartbeat again is reported alive to the rest of
 * the mesh, and that the retired data-path messages are ignored.
 *
 * A connection is reference counted.  A thread that looked one up used to
 * get a bare pointer, and the health check's reconnect freed the old
 * connection while the thread could still be using it.  The lifetime rows
 * have a node register again, which replaces its connection, while the test
 * holds a reference to the old one, and while its send thread is blocked
 * sending on it, and have a pending connection register while teardown
 * stops its receive thread.
 *
 * A worker kept its connection to another worker after that worker hung up,
 * and the two never connected again.  The dead connection rows run two
 * workers, have the manager report one's peer dead and then alive, and have
 * the peer drop the connection, and check that the worker closes its end and
 * connects again.  They also check that a connection from a peer that hung
 * up is closed.  The publish rows, and the duplicate handshake rows, check
 * what a worker does with a new connection when the table already has a
 * live or a dead one to the same node.  A worker now also takes topology
 * updates only from the manager, which a topology row checks.
 *
 * A worker whose connection to the manager dropped was cut off from the
 * mesh.  The manager link rows have the manager close the connection, fall
 * silent, refuse the worker, or go away, and check that the worker connects
 * and registers again under its own id, and only when it should.  One has
 * the manager evict a node meanwhile, and checks that the worker drops the
 * node's GIDs.
 *
 * A send to a peer that stayed connected but stopped reading waited for as
 * long as the peer took, holding up the device's main thread among others.
 * The stuck peer rows check that a send gives up after the send timeout and
 * shuts the connection down, that a sender waiting for the connection's lock
 * then fails at once, that the data path only queues its messages, up to a
 * limit, and that a manager relaying to such a peer gives up on it before
 * the worker it relays for would give up on the manager.
 *
 * Each row starts (or deliberately does not start) the real threads, stops
 * them, and checks that the stop returned promptly, that the threads really
 * exited, and how many joins it took.  The health-check rows also check
 * that the thread still runs once per interval when nobody stops it.  A
 * per-row alarm() turns a hang into a named failure rather than a ctest
 * timeout.  The test links the TCP backend, reaches its internals
 * through rdma_backend_tcp_priv.h, and stubs its externals; the thread
 * entry points are backed by real pthreads.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/sockios.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
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

#include <glib.h>
#include <infiniband/verbs.h>

#include "ernic_time.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_ops.h"
#include "rdma/rdma_backend_tcp_priv.h"
#include "rdma_backend.h"
#include "rdma_backend_defs.h"
#include "rocm-ernic-warnings.h"

/* ---- Stubs ------------------------------------------------------------- */

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

int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg)
{
    struct trampoline *t;
    int left = atomic_load(&g_creates_left);

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
void qemu_thread_join(QemuThread *thread)
{
    atomic_fetch_add(&g_joins, 1);
    pthread_join(thread->thread, NULL);
}

/* ---- Watchdog ---------------------------------------------------------- */

/*
 * Whether the test runs under valgrind.  Two rows care.
 *
 * REAP_NO_FDS cannot run at all: valgrind doesn't pass a change to
 * RLIMIT_NOFILE to the kernel but enforces the limit itself, and not as the
 * kernel does -- an accept() already waiting when the limit is lowered gets
 * a descriptor over it, and one that starts afterwards fails with EMFILE but
 * takes the connection off the queue, so the retry finds nothing.
 *
 * The rest need longer; see row_timeout_s().
 */
static bool under_valgrind(void)
{
#ifdef ERNIC_HAVE_VALGRIND_H
    return RUNNING_ON_VALGRIND != 0;
#else
    return false;
#endif
}

/*
 * How long a row may take before the watchdog calls it hung.
 *
 * This is a hang detector, not a performance assertion: every wait a row
 * makes is bounded, so a row that is merely slow still finishes and a row
 * that is stuck never does.  The budget has to cover the slowest legitimate
 * row, which is REAP_CAP -- 64 peers connected one at a time, each with a
 * receive thread, and then a 65th.
 *
 * Valgrind runs all of those threads on one core and costs tens of times
 * native speed, so the row takes well under a second natively and ran into
 * ten seconds on a loaded four-CPU CI runner with 63 of the 64 peers
 * connected: nothing was stuck, the budget was just a native-speed one.
 * Under valgrind it is six times larger, which is still far below the
 * minutes a genuinely stuck row would take, and the counters the watchdog
 * prints say which case it was.
 */
#define ROW_TIMEOUT_S 10

static unsigned row_timeout_s(void)
{
    return under_valgrind() ? ROW_TIMEOUT_S * 6u : ROW_TIMEOUT_S;
}

static const char *volatile g_current_row = "";

/*
 * Write "label<n>" with write(), which is async-signal-safe; printf() is
 * not.  n is one of the thread counters, so it needs no sign and no more
 * than ten digits.
 */
static void watchdog_count(const char *label, size_t label_len, int n)
{
    char buf[16];
    size_t i = sizeof(buf);

    if (n < 0) {
        n = 0;
    }
    do {
        buf[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0 && i > 0);

    if (write(STDOUT_FILENO, label, label_len) < 0 ||
        write(STDOUT_FILENO, buf + i, sizeof(buf) - i) < 0) {
        _exit(1);
    }
}

#define WATCHDOG_COUNT(label, n) watchdog_count(label, sizeof(label) - 1, n)

/*
 * The row took longer than row_timeout_s(), so something it was waiting for
 * never happened and the process has to be killed for ctest to see it.
 *
 * The counters go out with the row name because killing the process leaves
 * every thread still running, and valgrind then reports each one's thread-
 * local storage as possibly lost: a run that ends here is reported as a
 * memory leak whose stacks point at whatever created the threads, which
 * says nothing about what hung.  Created against joined against exited does
 * say: threads that exited but were not joined mean a join never ran, and
 * threads that never exited mean one is stuck in the loop it was told to
 * leave.
 */
_Noreturn static void watchdog(int sig)
{
    static const char msg[] = "FAIL hung stopping a thread: ";

    (void)sig;
    /* write() is async-signal-safe; the row text is a string literal. */
    if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0 ||
        write(STDOUT_FILENO, g_current_row, strlen(g_current_row)) < 0)
        _exit(1);
    WATCHDOG_COUNT("; threads created ", atomic_load(&g_creates));
    WATCHDOG_COUNT(", joined ", atomic_load(&g_joins));
    WATCHDOG_COUNT(", exited ", atomic_load(&g_exits));
    if (write(STDOUT_FILENO, "\n", 1) < 0) {
        _exit(1);
    }
    _exit(1);
}

/* ---- Fixture ----------------------------------------------------------- */

/*
 * A zeroed TcpBackendPrivate, for the rows that set up only what they use,
 * with the send timeout tcp_init() would give it: zero would have every send
 * that has to wait for room give up at once.
 */
static TcpBackendPrivate *priv_alloc(void)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);

    atomic_init(&priv->send_timeout_ms, TCP_DEFAULT_SEND_TIMEOUT_MS);
    return priv;
}

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

/* Whether fd is close-on-exec, and non-blocking exactly when nonblocking is */
static bool fd_flags_are(int fd, bool nonblocking)
{
    int fd_flags = fcntl(fd, F_GETFD);
    int status_flags = fcntl(fd, F_GETFL);

    return fd_flags >= 0 && status_flags >= 0 && (fd_flags & FD_CLOEXEC) &&
           ((status_flags & O_NONBLOCK) != 0) == nonblocking;
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

/*
 * priv's connection to id, live or dead, or NULL, for comparing with one the
 * caller holds a reference to; it can be freed once the lock is dropped.
 */
static const TcpConnection *table_get(TcpBackendPrivate *priv, uint32_t id)
{
    const TcpConnection *conn;

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&priv->conn_table_lock);
    return conn;
}

/*
 * Wait up to 5 s for priv's connection to id to be a live one other than
 * old, which the caller holds a reference to if it isn't NULL.  Returns it,
 * with a reference, or NULL.
 */
static TcpConnection *wait_live(TcpBackendPrivate *priv, uint32_t id,
                                const TcpConnection *old)
{
    for (int i = 0; i < 500; i++) {
        TcpConnection *conn = tcp_connection_lookup(priv, id);

        if (conn && conn != old && !tcp_conn_dead(conn))
            return conn;
        tcp_connection_unref(conn);
        sleep_ms(10);
    }
    return NULL;
}

/* Wait up to 5 s for conn's receive thread to return. */
static bool wait_dead(const TcpConnection *conn)
{
    for (int i = 0; i < 500 && !tcp_conn_dead(conn); i++)
        sleep_ms(10);
    return tcp_conn_dead(conn);
}

/* Whether conn is retired: closed, marked down, and its thread stopped. */
static bool conn_retired(TcpConnection *conn)
{
    int fd;

    qemu_mutex_lock(&conn->lock);
    fd = conn->sockfd;
    qemu_mutex_unlock(&conn->lock);
    return fd == -1 && !atomic_load(&conn->is_connected) &&
           !atomic_load(&conn->recv_thread_running);
}

/* Wait up to 5 s for conn to be retired. */
static bool wait_retired(TcpConnection *conn)
{
    for (int i = 0; i < 500 && !conn_retired(conn); i++)
        sleep_ms(10);
    return conn_retired(conn);
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
    TcpBackendPrivate *priv = priv_alloc();
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
        qemu_thread_create(&priv->accept_thread, tcp_accept_thread, priv);
        if (c->setup == BLOCKED)
            sleep_ms(100);
    }

    g_current_row = c->description;
    alarm(row_timeout_s());
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
    if (tcp_recv_message(fd, &hdr, &payload, NULL, 0) < 0)
        return false;
    free(payload);
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
    TcpBackendPrivate *priv = priv_alloc();
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
    qemu_mutex_init(&priv->mesh_table_lock);
    qemu_mutex_init(&priv->conn_table_lock);
    /* The health-check thread, which reaches priv, starts after this */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_mesh_node_info_destroy_notify);
    /* Each pass reaps the pending connections, of which there are none */
    priv->pending_conns = g_ptr_array_new();
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    if (c->peer) {
        MeshNodeInfo *node = tcp_mesh_node_info_new(1, "peer", 0);

        if (!node || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            printf("FAIL health %s: %s; could not set up the peer\n",
                   c->category, c->description);
            tcp_mesh_node_info_free(node);
            ok = false;
            goto out;
        }
        conn = tcp_connection_new(NULL, 1, "peer", 0, sv[0]);
        node->is_alive = true;
        node->last_heartbeat_ms = ernic_monotonic_ms();
        ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(1), conn);
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(1), node);
        ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    }

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (c->setup != NOT_STARTED) {
        atomic_store(&priv->health_check_running, true);
        qemu_thread_create(&priv->health_check_thread,
                           tcp_manager_health_check_thread, priv);
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
    /* The thread has been stopped */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->mesh_nodes);
    g_hash_table_destroy(priv->connections);
    g_ptr_array_free(priv->pending_conns, TRUE);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    if (sv[1] >= 0)
        close(sv[1]);
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

    /* A successful socketpair() always gives two descriptors, but the static
     * analyzer doesn't know that, and would otherwise follow *peer into a
     * send() as -1. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 || sv[0] < 0 || sv[1] < 0)
        return NULL;
    conn = tcp_connection_new(priv, id, "peer", 0, sv[0]);
    /* Non-blocking, as the backend makes every connection's socket. */
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    *peer = sv[1];
    if (start) {
        atomic_store(&conn->recv_thread_running, true);
        qemu_thread_create(&conn->recv_thread, tcp_recv_thread_per_conn, conn);
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
        /* Under the lock, like every write to the connection's socket */
        qemu_mutex_lock(&conn->lock);
        while (send(conn->sockfd, fill, sizeof(fill), 0) > 0)
            ;
        qemu_mutex_unlock(&conn->lock);
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
    TcpBackendPrivate *priv = priv_alloc();
    TcpConnection *all[MAX_RECV_CONNS];
    int peers[MAX_RECV_CONNS], n = 0, started = 0;
    /* The registered ones first, then a manager connection, then pending. */
    uint32_t in_table = c->table + (c->one_not_started ? 1u : 0u);
    uint32_t total = in_table + (c->manager ? 1u : 0u) + c->pending;
    double start, took;
    bool ok = true, flags_clear = true;

    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    /* Before the first receive thread starts, which can reach priv */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_mutex_init(&priv->conn_table_lock);
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
        /* The receive threads started so far are running */
        qemu_mutex_lock(&priv->conn_table_lock);
        if (i < in_table)
            g_hash_table_insert(priv->connections, GUINT_TO_POINTER(i + 1),
                                conn);
        else if (c->manager && !atomic_load(&priv->manager_conn))
            atomic_store(&priv->manager_conn, conn);
        else
            g_ptr_array_add(priv->pending_conns, conn);
        qemu_mutex_unlock(&priv->conn_table_lock);
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
        alarm(row_timeout_s());
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
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->connections);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    tcp_free_pending_conns(priv);
    tcp_connection_unref(atomic_load(&priv->manager_conn));
    for (int i = 0; i < n; i++)
        if (peers[i] >= 0)
            close(peers[i]);
    qemu_mutex_destroy(&priv->conn_table_lock);
    g_free(priv);
    return ok ? 0 : 1;
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

/* ---- The whole backend ------------------------------------------------- */

/*
 * tcp_init() as a manager, with workers that are plain sockets in this
 * process, then tcp_fini().  A worker that stops reading leaves a manager
 * thread blocked sending to it, holding that connection's lock and, in a
 * broadcast, conn_table_lock as well.  tcp_fini() has to return anyway.
 * Broadcasts used to be sent on the thread that handled the change, so a
 * worker's registration held up its own receive thread too; they are now
 * sent by a thread of their own.
 *
 * The workers are attached over socketpairs rather than accepted over TCP.
 * A TCP socket whose peer has stopped reading doesn't refuse every send:
 * the kernel grows its buffer for a while, and appends a small send to a
 * segment already queued, so whether a given heartbeat blocks is up to the
 * kernel.  A Unix stream socket that is full stays full until the peer
 * reads.  The backend handles either the same way.
 */

/* Where the manager could be left blocked when tcp_fini() is called. */
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
    uint32_t interval; /* health_check_interval_sec */
};

static const struct fini_case fini_cases[] = {
    {"positive", "a manager with two registered workers",
     "tcp_fini() returns and every thread exits", BLOCK_NONE, LONG_INTERVAL_S},
    {"negative", "a worker that stops reading the manager's heartbeats",
     "the health check skips it rather than wait, goes on sending heartbeats "
     "to the other worker, and tcp_fini() returns",
     BLOCK_HEARTBEAT, 1},
    {"negative", "a worker that stops reading while another registers",
     "the broadcast blocks on it, on the broadcaster thread, without holding "
     "conn_table_lock, so lookups on other threads go on, the worker that "
     "registered is still answered, and tcp_fini() returns",
     BLOCK_BROADCAST, LONG_INTERVAL_S},
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
    conn = tcp_connection_new(priv, UINT32_MAX, "worker", 0, sv[0]);
    /* Or a reap would find it past the registration timeout */
    conn->accepted_ms = ernic_monotonic_ms();
    qemu_mutex_lock(&priv->conn_table_lock);
    g_ptr_array_add(priv->pending_conns, conn);
    atomic_store(&conn->recv_thread_running, true);
    qemu_thread_create(&conn->recv_thread, tcp_recv_thread_per_conn, conn);
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (connp)
        *connp = conn;
    return sv[1];
}

/* The port the test's workers say they listen on.  Nothing does. */
#define WORKER_PORT 1

/*
 * Send a v4 REGISTER_NODE for a worker at hostname:port, asking for node id
 * requested.
 */
static bool send_register_from(int fd, const char *hostname, uint16_t port,
                               uint32_t requested)
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
    reg.port = htons(port);
    reg.requested_node_id = htonl(requested);
    reg.version = htonl(TCP_PROTOCOL_VERSION);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &reg, sizeof(reg));
    return send(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf);
}

/* As a worker at port WORKER_PORT, which nothing listens on */
static bool send_register(int fd, const char *hostname, uint32_t requested)
{
    return send_register_from(fd, hostname, WORKER_PORT, requested);
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
 * Whether n heartbeats arrive on fd, a blocking socket, among the other
 * messages that do, within timeout_ms.
 */
static bool heartbeats_within(int fd, int n, int timeout_ms)
{
    double end = now_s() + timeout_ms / 1000.0;

    while (n > 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        double left_ms = (end - now_s()) * 1000.0;
        TcpMsgHeader hdr;
        void *payload = NULL;

        if (left_ms <= 0.0 || poll(&pfd, 1, (int)left_ms) != 1 ||
            tcp_recv_message(fd, &hdr, &payload, NULL, 0) < 0)
            return false;
        free(payload);
        if (hdr.msg_type == TCP_MSG_HEARTBEAT)
            n--;
    }
    return true;
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
    TcpConnection *conn1 = NULL;
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

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 || tcp_init_with_interval(&dev, config, c->interval) != 0) {
        printf("FAIL fini %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;

    /* The accept thread blocks in accept() until teardown wakes it */
    if (!fd_flags_are(priv->listen_fd, false)) {
        why = "the listen socket isn't close-on-exec and blocking";
        goto stop;
    }

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
    conn1 = tcp_connection_lookup(priv, 1);
    if (!conn1) {
        why = "worker 1's connection is gone";
        goto stop;
    }

    /* Its registration's broadcast blocks on worker 1, if that is full */
    w2 = worker_register(priv);
    if (w2 < 0) {
        why = "could not attach worker 2";
        goto stop;
    }
    if (c->block == BLOCK_BROADCAST) {
        double start_lookup;
        TcpConnection *found;

        if (!held_elsewhere(&conn1->lock)) {
            why = "worker 2's broadcast never blocked on worker 1";
            goto stop;
        }
        /* While the broadcast waits, until its send times out */
        start_lookup = now_s();
        found = tcp_connection_lookup(priv, 1);
        tcp_connection_unref(found);
        if (!found || now_s() - start_lookup > 0.1) {
            why = "a lookup waited for the blocked broadcast";
            goto stop;
        }
    }
    /* The registration publishes worker 2 before it broadcasts */
    if (!wait_for_registered(priv, 2)) {
        why = "worker 2 did not register";
        goto stop;
    }
    /*
     * Within the 5 s the blocked send can take: worker 2's receive thread
     * only asked for the broadcast, so it isn't the one waiting.
     */
    if (c->block == BLOCK_BROADCAST &&
        (!send_heartbeat(w2, 2) ||
         !wait_for_type(w2, TCP_MSG_HEARTBEAT_RESP, 1000))) {
        why = "worker 2's heartbeat wasn't answered while the broadcast "
              "waited";
        goto stop;
    }
    /*
     * One a second, so two within about two seconds.  Neither worker
     * answers, so the manager declares worker 1 dead three intervals after
     * it registered, and the broadcast that tells the mesh waits behind
     * worker 2's, which waits on worker 1 until the send times out.  Both
     * wait on the broadcaster thread, so the health check goes on sending
     * heartbeats meanwhile.
     */
    if (c->block == BLOCK_HEARTBEAT && !heartbeats_within(w2, 2, 2500))
        why = "the health check stopped sending worker 2 heartbeats";

stop:
    tcp_connection_unref(conn1);
    start = now_s();
    rdma_backend_ops_tcp.fini(&dev);
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
    {"negative", "the manager's broadcaster thread can't be created",
     "tcp_init() fails and joins only the accept and health-check threads",
     FAIL_IN_INIT, 2, false},
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

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0) {
        why = "no free TCP port";
        goto out;
    }
    if (c->when == FAIL_IN_INIT)
        atomic_store(&g_creates_left, c->creates_left);
    init_ret = tcp_init_with_interval(&dev, config, LONG_INTERVAL_S);

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
        rdma_backend_ops_tcp.fini(&dev);

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
 *
 * A second peer can claim the node of a connection the thread has already
 * published.  The thread used to refuse it whatever the state of that
 * connection, so a node whose connection had died could never connect again.
 * It now replaces a dead connection, but still refuses a second peer while
 * the first is connected: replacing a live one would let anyone who can
 * reach the port take over a working link by claiming its node.
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
    HS_DUP_LIVE,     /* a good peer, then a second claiming its node */
    HS_DUP_DEAD,     /* the same, once the first has hung up */
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
    {"negative",
     "a second peer's handshake claiming the node of a connected peer",
     "refused; the first peer's connection is kept", HS_DUP_LIVE},
    {"corner",
     "a second peer's handshake claiming the node of a peer that hung up",
     "answered; the dead connection is replaced, closed, and its thread "
     "joined once",
     HS_DUP_DEAD},
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

/* The rows in which a second peer follows the good one */
static bool hs_dup(enum hs_peer peer)
{
    return peer == HS_DUP_LIVE || peer == HS_DUP_DEAD;
}

/*
 * The joins a row takes: the accept thread's, and, unless the row leaves it
 * waiting, the good peer's receive thread's.  The second peer of HS_DUP_DEAD
 * gets a receive thread too.
 */
static int hs_joins(enum hs_peer peer)
{
    if (hs_waits(peer))
        return 1;
    return peer == HS_DUP_DEAD ? 3 : 2;
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
    case HS_DUP_LIVE:
    case HS_DUP_DEAD:
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
    bool flags_ok;

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
    conn = tcp_connection_lookup(priv, HS_NODE);
    if (!conn)
        return "the good peer's connection isn't in the table";
    qemu_mutex_lock(&conn->lock);
    flags_ok = fd_flags_are(conn->sockfd, true);
    qemu_mutex_unlock(&conn->lock);
    tcp_connection_unref(conn);
    return flags_ok ? NULL
                    : "the accepted socket isn't close-on-exec and "
                      "non-blocking";
}

/*
 * HS_DUP_LIVE and HS_DUP_DEAD: once the good peer, at *good, is connected, a
 * second peer, at *second, sends a handshake claiming the same node.  For
 * HS_DUP_DEAD the good peer hangs up first, and the test waits for its
 * connection to die.
 */
static const char *hs_second_peer(TcpBackendPrivate *priv, uint16_t port,
                                  enum hs_peer peer, int *good, int *second)
{
    TcpConnection *first = tcp_connection_lookup(priv, HS_NODE);
    const char *why = NULL;

    if (!first)
        return "the good peer's connection isn't in the table";
    if (peer == HS_DUP_DEAD) {
        close(*good);
        *good = -1;
        if (!wait_dead(first))
            why = "the good peer's connection didn't die when it hung up";
    }
    if (!why) {
        *second = connect_loopback(port);
        if (*second < 0 || !send_hs(*second, TCP_MSG_HANDSHAKE,
                                    (uint32_t)sizeof(TcpHandshakePayload)))
            why = "could not connect the second peer";
    }

    if (!why && peer == HS_DUP_LIVE) {
        if (!closed_by_peer(*second))
            why = "the second peer wasn't refused";
        else if (table_get(priv, HS_NODE) != first)
            why = "the connected peer's connection was replaced";
    } else if (!why) {
        /* Answered once the dead connection is replaced and retired */
        if (!recv_type(*second, TCP_MSG_HANDSHAKE_RESP, 5000))
            why = "the second peer wasn't answered";
        else if (table_get(priv, HS_NODE) == first)
            why = "the dead connection wasn't replaced";
        else if (!conn_retired(first))
            why = "the dead connection wasn't closed";
    }
    tcp_connection_unref(first);
    return why;
}

static int run_hs_case(const struct hs_case *c)
{
    TcpBackendPrivate *priv = priv_alloc();
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    int bad = -1, good = -1;
    int want_joins = hs_joins(c->peer);
    double start, took = 0.0;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->local_node_id, 1);
    /* Before the accept thread starts, which can reach priv */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_mutex_init(&priv->conn_table_lock);
    priv->listen_fd = listen_loopback();
    memset(&addr, 0, sizeof(addr));

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (priv->listen_fd < 0 ||
        getsockname(priv->listen_fd, (struct sockaddr *)&addr, &len) < 0) {
        why = "could not open a listening socket";
    } else {
        atomic_store(&priv->accept_thread_running, true);
        qemu_thread_create(&priv->accept_thread, tcp_accept_thread, priv);
        if (c->peer != HS_GOOD && c->peer != HS_SPLIT && !hs_dup(c->peer))
            why = hs_bad_peer(c->peer, ntohs(addr.sin_port), &bad);
        if (!why && !hs_waits(c->peer))
            why = hs_good_peer(priv, ntohs(addr.sin_port), c->peer == HS_SPLIT,
                               &good);
        /* bad gets the second peer */
        if (!why && hs_dup(c->peer))
            why = hs_second_peer(priv, ntohs(addr.sin_port), c->peer, &good,
                                 &bad);
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

    /* Every thread has been stopped */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->connections);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    tcp_free_pending_conns(priv);
    if (bad >= 0)
        close(bad);
    if (good >= 0)
        close(good);
    if (priv->listen_fd >= 0)
        close(priv->listen_fd);
    qemu_mutex_destroy(&priv->conn_table_lock);
    g_free(priv);
    return ok ? 0 : 1;
}

/* ---- A node that stops answering --------------------------------------- */

/*
 * The health check used to reconnect to a node that had stopped answering:
 * it connected to the port the node registered, and replaced the node's
 * connection with the new one.  A worker takes topology updates and GID
 * tables only on the connection it opened to the manager itself, so it
 * ignored them from then on, and the connection it had opened was never
 * closed.  The manager no longer connects to a node.  It retires a silent
 * node's connection instead, as it does one whose node hung up, so that the
 * node can register again under its id (see the registering-again rows),
 * rather than a half-open connection staying published, looking alive.  The
 * silent rows check that, and that a node that answers just before its
 * connection is retired keeps it.
 *
 * The rows use a manager built by hand, an mgr_fixture, as do the lifetime
 * rows.
 */

#define MGR_MAX_NODES 2

/*
 * A manager whose nodes 1 to nodes each have a connection.  For node i + 1:
 * node[i] is its mesh entry, registered from WORKER_PORT on loopback, old[i]
 * its connection, with the receive thread running, and peer[i] the node's
 * end of it.  The fixture holds a reference to each old[i], so a row can
 * still look at one once the manager has released it.  The nodes start out
 * dead, as a manager leaves a node that stopped answering.
 */
struct mgr_fixture {
    TcpBackendPrivate *priv;
    uint32_t nodes;
    TcpConnection *old[MGR_MAX_NODES];
    MeshNodeInfo *node[MGR_MAX_NODES];
    int peer[MGR_MAX_NODES];
};

/* Set up fx; returns NULL, or what failed.  mgr_teardown() either way. */
static const char *mgr_setup(struct mgr_fixture *fx, uint32_t nodes,
                             uint32_t interval)
{
    TcpBackendPrivate *priv = priv_alloc();

    memset(fx, 0, sizeof(*fx));
    fx->priv = priv;
    for (uint32_t i = 0; i < MGR_MAX_NODES; i++)
        fx->peer[i] = -1;
    priv->is_manager = true;
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->health_check_running, false);
    qemu_mutex_init(&priv->health_check_lock);
    qemu_cond_init(&priv->health_check_cond);
    priv->health_check_interval_sec = interval;
    qemu_mutex_init(&priv->mesh_table_lock);
    /* Its thread isn't started; a row that wants one starts it */
    tcp_broadcaster_init(priv);
    qemu_mutex_init(&priv->conn_table_lock);
    qemu_mutex_init(&priv->gid_table_lock);
    /* Before the first receive thread starts, which can reach priv */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->next_available_node_id = 1;
    priv->mesh_nodes = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_mesh_node_info_destroy_notify);
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    /* The GID claim tables tcp_init() builds, which a registration updates */
    priv->node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->gid_owner = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            tcp_gid_key_free, NULL);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    for (uint32_t i = 0; i < nodes && i < MGR_MAX_NODES; i++) {
        uint32_t id = i + 1;

        fx->old[i] = recv_conn_new(priv, id, true, &fx->peer[i]);
        if (!fx->old[i])
            return "could not set up a node's connection";
        qemu_mutex_lock(&priv->conn_table_lock);
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(id),
                            tcp_connection_ref(fx->old[i]));
        qemu_mutex_unlock(&priv->conn_table_lock);
        fx->node[i] = tcp_mesh_node_info_new(id, "127.0.0.1", WORKER_PORT);
        if (!fx->node[i])
            return "could not create a mesh node";
        fx->node[i]->is_alive = false;
        qemu_mutex_lock(&priv->mesh_table_lock);
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(id),
                            fx->node[i]);
        qemu_mutex_unlock(&priv->mesh_table_lock);
        fx->nodes++;
    }
    return NULL;
}

/*
 * Make the nodes look dead_s seconds silent, and start the health-check
 * thread.  Its first pass comes at once, well within a second.  On a machine
 * up for less than dead_s, the time wraps, but the age the scan works out
 * from it, in unsigned arithmetic, is still right.
 */
static void mgr_start(struct mgr_fixture *fx, uint64_t dead_s)
{
    uint64_t last_ms = ernic_monotonic_ms() - dead_s * 1000u;

    for (uint32_t i = 0; i < fx->nodes; i++)
        fx->node[i]->last_heartbeat_ms = last_ms;
    atomic_store(&fx->priv->health_check_running, true);
    qemu_thread_create(&fx->priv->health_check_thread,
                       tcp_manager_health_check_thread, fx->priv);
}

static void mgr_teardown(struct mgr_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;

    tcp_stop_health_check_thread(priv);
    tcp_stop_broadcast_thread(priv);
    tcp_stop_recv_threads(priv);
    /* Every thread has been stopped */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->mesh_nodes);
    g_hash_table_destroy(priv->connections);
    g_hash_table_destroy(priv->gid_owner);
    g_hash_table_destroy(priv->node_gids);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    tcp_free_pending_conns(priv);
    for (uint32_t i = 0; i < MGR_MAX_NODES; i++) {
        tcp_connection_unref(fx->old[i]);
        if (fx->peer[i] >= 0)
            close(fx->peer[i]);
    }
    qemu_mutex_destroy(&priv->gid_table_lock);
    qemu_mutex_destroy(&priv->conn_table_lock);
    tcp_broadcaster_destroy(priv);
    qemu_mutex_destroy(&priv->mesh_table_lock);
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    g_free(priv);
}

enum sl_scenario {
    SL_SILENT,  /* node 1 is connected, but hasn't answered for 4 intervals */
    SL_ANSWERS, /* it answers before the pass retires its connection */
};

struct sl_case {
    const char *category;
    const char *description;
    const char *expected;
    enum sl_scenario scenario;
};

static const struct sl_case sl_cases[] = {
    {"positive",
     "a node that is connected but hasn't answered for four 1 s intervals",
     "declared dead, its connection closed and taken out of the table, and "
     "its thread joined once; nothing connects to the port it registered",
     SL_SILENT},
    {"negative", "a silent node that answers before its connection is retired",
     "the connection is kept", SL_ANSWERS},
};

/*
 * SL_SILENT.  The node registered a port that is listening, so a connect to
 * it would get through; the old health check's came straight after its
 * scan.
 */
static const char *sl_silent(struct mgr_fixture *fx, int *lfd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    struct pollfd pfd = {.events = POLLIN};
    bool alive;

    *lfd = listen_loopback();
    if (*lfd < 0 || getsockname(*lfd, (struct sockaddr *)&addr, &len) < 0)
        return "could not open a listening socket";
    /* Before the thread starts, so nothing reads it meanwhile */
    fx->node[0]->port = ntohs(addr.sin_port);
    fx->node[0]->is_alive = true;
    mgr_start(fx, 4);

    if (!wait_retired(fx->old[0]))
        return "the silent node's connection wasn't closed";
    if (!closed_by_peer(fx->peer[0]))
        return "the node didn't see its connection end";
    if (table_get(fx->priv, 1))
        return "the connection is still in the table";
    qemu_mutex_lock(&fx->priv->mesh_table_lock);
    alive = fx->node[0]->is_alive;
    qemu_mutex_unlock(&fx->priv->mesh_table_lock);
    if (alive)
        return "the node wasn't declared dead";
    pfd.fd = *lfd;
    if (poll(&pfd, 1, 1500) != 0)
        return "the manager connected to the node";
    return atomic_load(&g_joins) == 1
               ? NULL
               : "the node's receive thread wasn't joined exactly once";
}

/*
 * SL_ANSWERS: the item is as the scan left it for a silent node, but by the
 * time tcp_health_check_retire() looks, the node has answered.  No thread
 * runs a pass meanwhile.
 */
static const char *sl_answers(struct mgr_fixture *fx)
{
    GArray *items = g_array_new(FALSE, FALSE, sizeof(TcpHealthCheckItem));
    TcpHealthCheckItem item = {1, NULL, false, true};
    const TcpHealthCheckItem *after;
    const char *why = NULL;

    item.conn = tcp_connection_lookup(fx->priv, 1);
    qemu_mutex_lock(&fx->priv->mesh_table_lock);
    fx->node[0]->is_alive = true;
    fx->node[0]->last_heartbeat_ms = ernic_monotonic_ms();
    qemu_mutex_unlock(&fx->priv->mesh_table_lock);
    g_array_append_val(items, item);
    tcp_health_check_retire(fx->priv, items);

    after = &g_array_index(items, TcpHealthCheckItem, 0);
    if (after->conn != fx->old[0])
        why = "the pass let go of the connection";
    else if (table_get(fx->priv, 1) != fx->old[0])
        why = "the connection was taken out of the table";
    else if (conn_retired(fx->old[0]))
        why = "the connection was closed";
    tcp_connection_unref(after->conn);
    g_array_free(items, TRUE);
    return why;
}

static int run_silent_case(const struct sl_case *c)
{
    struct mgr_fixture fx;
    const char *why;
    int lfd = -1;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = c->description;
    alarm(row_timeout_s());
    why = mgr_setup(&fx, 1, 1);
    if (!why)
        why = c->scenario == SL_SILENT ? sl_silent(&fx, &lfd) : sl_answers(&fx);
    mgr_teardown(&fx);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s silent %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited\n", why,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));
    fflush(stdout);

    if (lfd >= 0)
        close(lfd);
    return ok ? 0 : 1;
}

/* ---- Connecting with a deadline ---------------------------------------- */

/*
 * tcp_connect_to_remote() waits for its connect without blocking, so a host
 * that doesn't answer doesn't hold the caller until the kernel gives up, about
 * two minutes later.  The connect rows check it on its own, including a
 * deadline shared by several addresses.
 */
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
    CT_LISTENING,         /* a listening port */
    CT_CLOSED,            /* a port nothing listens on */
    CT_UNANSWERING,       /* a listener that doesn't answer */
    CT_TWO_UNANSWERING,   /* two addresses, neither of which answers */
    CT_REFUSED_FIRST,     /* two addresses, the first refused, then listening */
    CT_UNANSWERING_FIRST, /* two addresses, the first not answering */
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
     "a connected socket, left non-blocking and close-on-exec", CT_LISTENING, 0,
     true, 0.0, 1.0},
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
    {"boundary",
     "connect to two addresses, the first not answering, with a 2 s timeout",
     "the first gives up after its half: connected to the second after 1 s",
     CT_UNANSWERING_FIRST, 2, true, 1.0, 1.8},
};

static int run_connect_case(const struct connect_case *c)
{
    struct sockaddr_in addr[2];
    struct addrinfo ai[2];
    socklen_t len = sizeof(addr[0]);
    int lfd[2] = {-1, -1}, filler[2] = {-1, -1}, fd = -1;
    uint16_t port[2] = {0, 0};
    uint32_t addrs = c->target == CT_TWO_UNANSWERING ||
                             c->target == CT_REFUSED_FIRST ||
                             c->target == CT_UNANSWERING_FIRST
                         ? 2
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
    case CT_UNANSWERING_FIRST:
        lfd[0] = listen_unanswering(&port[0], &filler[0]);
        lfd[1] = listen_loopback();
        if (lfd[1] >= 0 &&
            getsockname(lfd[1], (struct sockaddr *)&addr[1], &len) == 0)
            port[1] = ntohs(addr[1].sin_port);
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
    alarm(row_timeout_s());
    if (port[0] != 0 && (addrs == 1 || port[1] != 0)) {
        start = now_s();
        if (c->timeout_s != 0)
            deadline_ms = ernic_monotonic_ms() + c->timeout_s * UINT64_C(1000);
        if (addrs == 1)
            fd = tcp_connect_to_remote("127.0.0.1", port[0], NULL, deadline_ms);
        else
            fd = tcp_connect_any(ai, NULL, deadline_ms);
        took = now_s() - start;
        ok = (fd >= 0) == c->want_fd && took >= c->min_s && took < c->max_s &&
             (fd < 0 || fd_flags_are(fd, true));
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

        if (tcp_recv_message(fd, &hdr, &payload, NULL, 0) < 0)
            break;
        free(payload);
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

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 ||
        tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0) {
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

    rdma_backend_ops_tcp.fini(&dev);
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
 * Only the manager's updates count.  A worker used to take one from any
 * connection, and an update now also decides which of the worker's
 * connections it closes, so a peer could take down its links.
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
    TP_FROM_PEER, /* an update from a peer, then from the manager */
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
    {"negative",
     "an update on a peer's connection, then the same update from the "
     "manager",
     "the peer's is ignored, and the peer it names connected to only after "
     "the manager's",
     TP_FROM_PEER, LONG_INTERVAL_S},
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

static bool tp_start_peers(struct tp_fixture *fx)
{
    return tcp_start_thread(&fx->priv->peer_thread, "tcp-peers",
                            tcp_worker_peer_thread, fx->priv,
                            &fx->priv->peer_thread_running);
}

/*
 * Set up fx and start its threads, the peer-connect thread only if peers;
 * returns NULL, or what failed.
 */
static const char *tp_setup(struct tp_fixture *fx, uint32_t interval,
                            bool peers)
{
    TcpBackendPrivate *priv = priv_alloc();

    fx->priv = priv;
    fx->mgr = -1;
    atomic_init(&priv->stopping, false);
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->health_check_running, false);
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->local_node_id, TP_LOCAL);
    atomic_init(&priv->tcp_stats.peer_connect_attempts, 0);
    priv->health_check_interval_sec = interval;
    /* Before the first thread starts, which can reach priv */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    priv->peer_targets = g_array_new(FALSE, FALSE, sizeof(TcpPeerTarget));
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_mutex_init(&priv->conn_table_lock);
    qemu_mutex_init(&priv->peer_lock);
    qemu_cond_init(&priv->peer_cond);
    atomic_init(&priv->peer_thread_running, false);
    qemu_mutex_init(&priv->registration_mutex);
    qemu_cond_init(&priv->registration_cond);
    /*
     * Where the peer-connect thread would connect to the manager again, if
     * a row ran three intervals without a word from it: a port nothing
     * listens on, so the connect fails at once.
     */
    priv->manager_host = g_strdup("127.0.0.1");
    priv->manager_port = 1;

    atomic_store(&priv->manager_conn, recv_conn_new(priv, 0, true, &fx->mgr));
    if (!atomic_load(&priv->manager_conn))
        return "could not connect to the manager";
    if (peers && !tp_start_peers(fx))
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
    /* Every thread has been stopped */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->connections);
    g_array_free(priv->peer_targets, TRUE);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    tcp_free_pending_conns(priv);
    tcp_connection_unref(atomic_load(&priv->manager_conn));
    if (fx->mgr >= 0)
        close(fx->mgr);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_cond_destroy(&priv->peer_cond);
    qemu_mutex_destroy(&priv->peer_lock);
    qemu_cond_destroy(&priv->registration_cond);
    qemu_mutex_destroy(&priv->registration_mutex);
    g_free(priv->manager_host);
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

/*
 * A listener on loopback port port, or -1.  Connections the test closed on
 * the port may still hold it, in TIME_WAIT, so it is bound with
 * SO_REUSEADDR.
 */
static int listen_on(uint16_t port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
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
    case TP_FROM_PEER:
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

/* Send topo on fd, as the manager does. */
static bool send_topology(int fd, const TcpMeshTopologyPayload *topo)
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
    return send(fd, &msg, sizeof(msg), 0) == (ssize_t)sizeof(msg);
}

/* Send topo as the manager. */
static bool tp_send(struct tp_fixture *fx, const TcpMeshTopologyPayload *topo)
{
    return send_topology(fx->mgr, topo);
}

/*
 * Have the worker's latest update name only *target, as if the receive
 * thread had handed it over, for a row that doesn't run the thread that
 * would act on it.
 */
static void tp_set_target(struct tp_fixture *fx, const TcpPeerTarget *target)
{
    qemu_mutex_lock(&fx->priv->peer_lock);
    g_array_set_size(fx->priv->peer_targets, 0);
    g_array_append_vals(fx->priv->peer_targets, target, 1);
    qemu_mutex_unlock(&fx->priv->peer_lock);
}

/* A target for node id at port on loopback */
static TcpPeerTarget tp_target(uint32_t id, uint16_t port)
{
    TcpPeerTarget target;

    memset(&target, 0, sizeof(target));
    target.node_id = id;
    snprintf(target.host, sizeof(target.host), "127.0.0.1");
    target.port = port;
    return target;
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

static guint tp_count(const struct tp_fixture *fx)
{
    guint n;

    qemu_mutex_lock(&fx->priv->conn_table_lock);
    n = g_hash_table_size(fx->priv->connections);
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    return n;
}

/* Wait up to 5 s for the thread to have made n connect attempts. */
static bool tp_attempts(const struct tp_fixture *fx, uint64_t n)
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

/*
 * A connection to id, as the accept thread would publish it, or, if port
 * isn't 0, as the worker would if it had connected to id at port on
 * loopback.  Its receive thread is started if start.  *fd, if fd isn't NULL,
 * gets the peer's end, which is closed at the end of the row.
 */
static TcpConnection *tp_publish(struct tp_fixture *fx, struct tp_net *net,
                                 uint32_t id, uint16_t port, bool start,
                                 int *fd)
{
    int peer = -1;
    TcpConnection *conn = recv_conn_new(fx->priv, id, false, &peer);

    tp_keep(net, peer);
    if (fd)
        *fd = peer;
    if (!conn)
        return NULL;
    if (port != 0) {
        g_free(conn->remote_host);
        conn->remote_host = g_strdup("127.0.0.1");
        conn->remote_port = port;
    }
    if (start) {
        atomic_store(&conn->recv_thread_running, true);
        qemu_thread_create(&conn->recv_thread, tcp_recv_thread_per_conn, conn);
    }
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    g_hash_table_insert(fx->priv->connections, GUINT_TO_POINTER(id), conn);
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
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
    theirs = tp_publish(fx, net, TP_PEER, 0, true, NULL);
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

/*
 * TP_FROM_PEER: the update comes on the connection of TP_LOWER, a peer.  The
 * heartbeat that connection answers after it shows it was handled, by being
 * ignored: the worker has no peers to connect to.  The same update from the
 * manager is then acted on.
 */
static const char *tp_from_peer(struct tp_fixture *fx, struct tp_net *net,
                                const TcpMeshTopologyPayload *topo)
{
    guint targets;
    int peer = -1, fd;

    if (!tp_publish(fx, net, TP_LOWER, 0, true, &peer))
        return "could not set up the peer's connection";
    if (!send_topology(peer, topo))
        return "could not send the update as the peer";
    if (!send_heartbeat(peer, TP_LOWER) ||
        !wait_for_type(peer, TCP_MSG_HEARTBEAT, 2000))
        return "the heartbeat after the update wasn't answered";

    qemu_mutex_lock(&fx->priv->peer_lock);
    targets = fx->priv->peer_targets->len;
    qemu_mutex_unlock(&fx->priv->peer_lock);
    if (targets != 0 ||
        atomic_load(&fx->priv->tcp_stats.peer_connect_attempts) != 0)
        return "the worker took the peer's update";

    if (!tp_send(fx, topo))
        return "could not send the update as the manager";
    return tp_connected_to(fx, net, net->lfd[0], TP_PEER, &fd);
}

/*
 * TP_SKIPS: the worker connected to TP_OTHER at net->port[0] on an earlier
 * update, which named it there.  The peer-connect thread is started only
 * once both are in place: each pass closes a connection to a higher id that
 * the latest update doesn't name at its address, as a first pass with no
 * update would.
 */
static const char *tp_skips_setup(struct tp_fixture *fx, struct tp_net *net)
{
    TcpPeerTarget earlier = tp_target(TP_OTHER, net->port[0]);

    tp_set_target(fx, &earlier);
    if (!tp_publish(fx, net, TP_OTHER, net->port[0], false, NULL))
        return "could not set up the existing connection";
    return tp_start_peers(fx) ? NULL
                              : "could not start the peer-connect thread";
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
        return tp_skips_setup(fx, net);
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
    case TP_FROM_PEER:
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
    if (scenario == TP_FROM_PEER)
        return tp_from_peer(fx, net, &topo);
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
    case TP_FROM_PEER: /* sent above */
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
    alarm(row_timeout_s());

    /* TP_SKIPS starts the peer-connect thread itself */
    why = tp_setup(&fx, c->interval, c->scenario != TP_SKIPS);
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

/* ---- A worker's registration ------------------------------------------- */

/*
 * A worker takes its node id from the manager's REGISTER_RESP.  It used to
 * take one from any connection, at any time, so a peer could rewrite a
 * worker's id, and with it the GIDs the worker claims, by sending it a
 * REGISTER_RESP.  Only the manager's connection may answer now, and only
 * once, with an id a worker can have.  An answer that refuses the worker
 * makes tcp_init() fail at once, rather than at the registration timeout.
 *
 * A worker also used to start its accept and peer-connect threads before it
 * registered, while its id still read as 0, the manager's.  A peer that
 * connected meanwhile was told it was talking to the manager.  A topology
 * update that came before the answer, as the broadcast for another node's
 * registration can, had the worker connect to its peers as node 0, and to
 * peers with lower ids, which connect to it.  The threads now start once the
 * worker has its id, and an update before then is ignored.  The last rows
 * have one of them fail to start, after which tcp_init() has to join the
 * threads it did start.
 *
 * Each row runs tcp_init() as a worker on a thread of its own, against a
 * manager the test plays over loopback.  The worker's id is above HS_NODE,
 * which the peers that connect to it claim, so each has the lower id, as the
 * protocol requires.  The peer-connect thread closes a connection from a
 * higher id that no update names, on a pass that can come at any time.
 */

#define WR_ID       8  /* the id the manager assigns */
#define WR_OTHER_ID 9  /* the id a second answer, or a peer's, names */
#define WR_LOWER    2  /* a node with a lower id, which the worker leaves be */
#define WR_HIGHER   10 /* one with a higher id, which the worker connects to */

enum wr_scenario {
    WR_ASSIGNED,       /* the manager assigns WR_ID */
    WR_REFUSED,        /* the manager refuses the registration */
    WR_RESULT_MIN,     /* it refuses with INT32_MIN, which no errno is */
    WR_OLD_VERSION,    /* an answer with no version, as a pre-v4 manager's */
    WR_SHORT,          /* an answer too short to hold an id */
    WR_ID_ZERO,        /* the manager assigns 0 */
    WR_ID_NONE,        /* the manager assigns UINT32_MAX */
    WR_TWICE,          /* a second answer, naming WR_OTHER_ID */
    WR_FROM_PEER,      /* a peer's answer, naming WR_OTHER_ID */
    WR_EARLY_PEER,     /* a peer's handshake before the answer */
    WR_EARLY_TOPOLOGY, /* a topology update before the answer */
    WR_NO_RECV,        /* its connection's receive thread can't start */
    WR_NO_ACCEPT,      /* registered, but the accept thread can't start */
    WR_NO_PEERS,       /* registered, but the peer-connect thread can't */
};

struct wr_case {
    const char *category;
    const char *description;
    const char *expected;
    enum wr_scenario scenario;
};

static const struct wr_case wr_cases[] = {
    {"positive", "the manager assigns the worker an id",
     "tcp_init() succeeds, and the worker takes the id and advertises its "
     "GIDs as that node",
     WR_ASSIGNED},
    {"negative", "the manager refuses the registration",
     "tcp_init() fails at once, not at the registration timeout", WR_REFUSED},
    {"boundary", "the manager refuses with INT32_MIN, which can't be negated",
     "refused: tcp_init() fails at once", WR_RESULT_MIN},
    {"negative", "an answer with no version, as a pre-v4 manager sends",
     "tcp_init() fails at once", WR_OLD_VERSION},
    {"boundary", "an answer one byte too short to hold a pre-v4 manager's",
     "refused: tcp_init() fails at once", WR_SHORT},
    {"boundary", "the manager assigns node id 0, its own",
     "refused: tcp_init() fails at once", WR_ID_ZERO},
    {"boundary", "the manager assigns node id UINT32_MAX",
     "refused: tcp_init() fails at once", WR_ID_NONE},
    {"corner", "a second answer from the manager, naming another id",
     "ignored: the id is unchanged, and the GIDs aren't advertised again",
     WR_TWICE},
    {"negative", "an answer from a peer, naming another id",
     "ignored: the id is unchanged, and nobody is sent an advertisement",
     WR_FROM_PEER},
    {"corner", "a peer's handshake before the manager answers",
     "answered only once the worker has its id, with that id", WR_EARLY_PEER},
    {"corner",
     "a topology update before the manager answers, naming a lower and a "
     "higher id",
     "ignored: the worker connects only to the peer a later update names, as "
     "its own id",
     WR_EARLY_TOPOLOGY},
    {"negative",
     "the receive thread for the worker's connection to the manager can't be "
     "created",
     "tcp_init() fails at once, having sent nothing, and closes the "
     "connection",
     WR_NO_RECV},
    {"negative", "the worker registers, and its accept thread can't be created",
     "tcp_init() fails, and joins the thread it started", WR_NO_ACCEPT},
    {"negative",
     "the worker registers, and its peer-connect thread can't be created",
     "tcp_init() fails, and joins the threads it started", WR_NO_PEERS},
};

/*
 * How many of the worker's qemu_thread_create() calls succeed, for g_creates
 * to stop at: the first is the receive thread for the manager's connection,
 * then the accept thread, then the peer-connect thread.  -1 for all.
 */
static int wr_creates_left(enum wr_scenario scenario)
{
    switch (scenario) {
    case WR_NO_RECV:
        return 0;
    case WR_NO_ACCEPT:
        return 1;
    case WR_NO_PEERS:
        return 2;
    case WR_ASSIGNED:
    case WR_REFUSED:
    case WR_RESULT_MIN:
    case WR_OLD_VERSION:
    case WR_SHORT:
    case WR_ID_ZERO:
    case WR_ID_NONE:
    case WR_TWICE:
    case WR_FROM_PEER:
    case WR_EARLY_PEER:
    case WR_EARLY_TOPOLOGY:
    default:
        return -1;
    }
}

/* tcp_init() as a worker, run on a thread of its own by wr_init_run() */
struct wr_init {
    RdmaBackendDev dev;
    char config[64];
    uint32_t interval; /* health_check_interval_sec */
    pthread_t tid;
    bool running; /* tid hasn't been joined */
    int ret;
    double took; /* how long tcp_init() took */
};

static void *wr_init_run(void *opaque)
{
    struct wr_init *in = opaque;
    double start = now_s();

    in->ret = tcp_init_with_interval(&in->dev, in->config, in->interval);
    in->took = now_s() - start;
    return NULL;
}

/* Wait for tcp_init() to return; its results are in `in` once it has. */
static void wr_init_join(struct wr_init *in)
{
    if (in->running) {
        pthread_join(in->tid, NULL);
        in->running = false;
    }
}

/* Read the next message on fd, waiting up to timeout_ms for it to start. */
static bool wr_recv(int fd, TcpMsgHeader *hdr, void **payload, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    *payload = NULL;
    return poll(&pfd, 1, timeout_ms) == 1 &&
           tcp_recv_message(fd, hdr, payload, NULL, 0) == 0;
}

/*
 * Read messages on fd, for up to timeout_ms, until one of type arrives, and
 * return its header and payload, which the caller frees.  The NODE_GIDS on
 * the way, if type isn't that, are counted in *gids, if it isn't NULL.
 */
static bool wr_until(int fd, TcpMsgType type, int timeout_ms, TcpMsgHeader *hdr,
                     void **payload, int *gids)
{
    double end = now_s() + timeout_ms / 1000.0;

    while (now_s() < end) {
        int left = (int)((end - now_s()) * 1000.0);

        if (!wr_recv(fd, hdr, payload, left > 0 ? left : 1))
            return false;
        if (hdr->msg_type == type)
            return true;
        if (gids && hdr->msg_type == TCP_MSG_NODE_GIDS)
            (*gids)++;
        free(*payload);
    }
    *payload = NULL;
    return false;
}

/*
 * The node id in the next message of type on fd, from its payload: a
 * handshake's or an advertisement's.  UINT32_MAX if none arrives within 5 s.
 * *src, if it isn't NULL, gets the sender the header names.
 */
static uint32_t wr_node_in(int fd, TcpMsgType type, uint32_t *src)
{
    TcpMsgHeader hdr;
    void *payload;
    uint32_t id = UINT32_MAX;

    if (!wr_until(fd, type, 5000, &hdr, &payload, NULL))
        return UINT32_MAX;
    /* Both payloads start with the node id, in network order */
    if (payload && hdr.msg_len >= sizeof(uint32_t)) {
        memcpy(&id, payload, sizeof(id));
        id = ntohl(id);
    }
    if (src)
        *src = hdr.src_node_id;
    free(payload);
    return id;
}

/*
 * Whether fd's other end answers a heartbeat within 2 s, having handled
 * everything sent before it; *gids counts the advertisements it sent first.
 */
static bool wr_settled(int fd, int *gids)
{
    TcpMsgHeader hdr;
    void *payload;

    if (!send_heartbeat(fd, 0) ||
        !wr_until(fd, TCP_MSG_HEARTBEAT, 2000, &hdr, &payload, gids))
        return false;
    free(payload);
    return true;
}

/*
 * Send a REGISTER_RESP assigning id, with result, as the manager does.  len
 * is the payload's length: TCP_REGISTER_RESP_LEN_V3 leaves off the version.
 */
static bool send_register_resp(int fd, uint32_t id, int32_t result,
                               uint32_t len)
{
    struct {
        TcpMsgHeader hdr;
        TcpRegisterRespPayload resp;
    } msg;
    size_t size = sizeof(msg.hdr) + len;

    if (len > sizeof(msg.resp))
        return false;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(TCP_MSG_REGISTER_RESP);
    msg.hdr.msg_len = htonl(len);
    msg.resp.assigned_node_id = htonl(id);
    msg.resp.num_nodes = htonl(2);
    msg.resp.result = htonl((uint32_t)result);
    msg.resp.version = htonl(TCP_PROTOCOL_VERSION);
    return send(fd, &msg, size, 0) == (ssize_t)size;
}

static bool wr_assign(int mgr, uint32_t id)
{
    return send_register_resp(mgr, id, 0,
                              (uint32_t)sizeof(TcpRegisterRespPayload));
}

/*
 * Accept the worker's connection on lfd and read its registration; *port
 * gets the port it listens on for its peers.  Returns NULL, or what failed.
 */
static const char *wr_registered(int lfd, int *mgr, uint16_t *port)
{
    TcpMsgHeader hdr;
    void *payload;
    const char *why = NULL;

    *mgr = accept_within(lfd);
    if (*mgr < 0)
        return "the worker never connected to the manager";
    if (!wr_recv(*mgr, &hdr, &payload, 5000))
        return "the worker sent no registration";
    if (hdr.msg_type != TCP_MSG_REGISTER_NODE || !payload ||
        hdr.msg_len < sizeof(TcpRegisterNodePayload))
        why = "the worker's first message wasn't a registration";
    else
        *port = ntohs(((const TcpRegisterNodePayload *)payload)->port);
    free(payload);
    return why;
}

/* The worker's id, once tcp_init() has succeeded */
static uint32_t wr_local(const struct wr_init *in)
{
    return tcp_local_node(in->dev.backend_private);
}

/*
 * A handshake from a peer claiming to be node HS_NODE on a new connection to
 * the worker's listen port; returns the connection, or -1.
 */
static int wr_peer(uint16_t port)
{
    int fd = connect_loopback(port);

    if (fd >= 0 && !send_hs(fd, TCP_MSG_HANDSHAKE,
                            (uint32_t)sizeof(TcpHandshakePayload))) {
        close(fd);
        return -1;
    }
    return fd;
}

/* After the answer that registers the worker as WR_ID */
static const char *wr_after_assign(const struct wr_init *in, int mgr)
{
    if (in->ret != 0)
        return "tcp_init() failed";
    if (wr_local(in) != WR_ID)
        return "the worker didn't take the id it was assigned";
    if (wr_node_in(mgr, TCP_MSG_NODE_GIDS, NULL) != WR_ID)
        return "the worker didn't advertise its GIDs as its own id";
    return NULL;
}

/* WR_TWICE, once the worker has registered */
static const char *wr_twice(const struct wr_init *in, int mgr)
{
    int gids = 0;

    if (!wr_assign(mgr, WR_OTHER_ID) || !wr_settled(mgr, &gids))
        return "the worker stopped answering the manager";
    if (wr_local(in) != WR_ID)
        return "the second answer changed the worker's id";
    return gids == 0 ? NULL : "the worker advertised its GIDs again";
}

/* WR_FROM_PEER, once the worker has registered */
static const char *wr_from_peer(const struct wr_init *in, int mgr,
                                uint16_t port, int *peer)
{
    uint32_t src = UINT32_MAX;
    int peer_gids = 0, mgr_gids = 0;

    *peer = wr_peer(port);
    if (*peer < 0)
        return "the peer could not connect";
    if (wr_node_in(*peer, TCP_MSG_HANDSHAKE_RESP, &src) != WR_ID ||
        src != WR_ID)
        return "the peer's handshake wasn't answered as WR_ID";
    if (!send_register_resp(*peer, WR_OTHER_ID, 0,
                            (uint32_t)sizeof(TcpRegisterRespPayload)) ||
        !wr_settled(*peer, &peer_gids) || !wr_settled(mgr, &mgr_gids))
        return "the worker stopped answering";
    if (wr_local(in) != WR_ID)
        return "the peer changed the worker's id";
    return peer_gids == 0 && mgr_gids == 0
               ? NULL
               : "the worker advertised its GIDs again";
}

/*
 * WR_EARLY_PEER, before the manager answers: a peer's handshake isn't
 * answered until then, and is then answered as WR_ID.
 */
static const char *wr_early_peer(struct wr_init *in, int mgr, uint16_t port,
                                 int *peer)
{
    struct pollfd pfd = {.events = POLLIN};
    uint32_t src = UINT32_MAX;

    *peer = wr_peer(port);
    if (*peer < 0)
        return "the peer could not connect";
    pfd.fd = *peer;
    if (poll(&pfd, 1, 300) != 0)
        return "the worker answered the handshake before it had an id";
    if (!wr_assign(mgr, WR_ID))
        return "could not answer the registration";
    wr_init_join(in);
    if (in->ret != 0)
        return "tcp_init() failed";
    if (wr_node_in(*peer, TCP_MSG_HANDSHAKE_RESP, &src) != WR_ID ||
        src != WR_ID)
        return "the handshake wasn't answered as the assigned id";
    return NULL;
}

/*
 * WR_EARLY_TOPOLOGY, before the manager answers: an update naming WR_LOWER
 * at low and WR_HIGHER at high.  The worker connects to nothing until a
 * second update, after the answer, names WR_HIGHER alone, and then
 * introduces itself to it as WR_ID.
 */
static const char *wr_early_topology(struct wr_init *in, int mgr, int low,
                                     int high, int *peer)
{
    TcpMeshTopologyPayload topo;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    struct pollfd pfd = {.fd = low, .events = POLLIN};
    uint16_t low_port, high_port;

    if (getsockname(low, (struct sockaddr *)&addr, &len) < 0)
        return "could not find the peers' ports";
    low_port = ntohs(addr.sin_port);
    len = sizeof(addr);
    if (getsockname(high, (struct sockaddr *)&addr, &len) < 0)
        return "could not find the peers' ports";
    high_port = ntohs(addr.sin_port);

    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, WR_LOWER, low_port, true);
    tp_entry(&topo, WR_HIGHER, high_port, true);
    if (!send_topology(mgr, &topo))
        return "could not send the first update";
    sleep_ms(300); /* time for a worker that didn't ignore it to connect */
    if (!wr_assign(mgr, WR_ID))
        return "could not answer the registration";
    wr_init_join(in);
    if (in->ret != 0)
        return "tcp_init() failed";

    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, WR_HIGHER, high_port, true);
    if (!send_topology(mgr, &topo))
        return "could not send the second update";
    *peer = accept_within(high);
    if (*peer < 0)
        return "the worker never connected to the higher id";
    if (wr_node_in(*peer, TCP_MSG_HANDSHAKE, NULL) != WR_ID)
        return "the worker didn't introduce itself as the assigned id";
    return poll(&pfd, 1, 0) == 0 ? NULL
                                 : "the worker connected to the lower id";
}

/* Play the manager, and any peers, for the row; returns NULL or what failed */
static const char *wr_run(const struct wr_case *c, struct wr_init *in, int lfd,
                          int *mgr, int *peer)
{
    const char *why;
    uint16_t port = 0;
    int low = -1, high = -1;

    if (c->scenario == WR_NO_RECV) {
        *mgr = accept_within(lfd);
        if (*mgr < 0)
            return "the worker never connected to the manager";
        if (!closed_by_peer(*mgr))
            return "the worker didn't close its connection";
        wr_init_join(in);
        if (in->ret == 0)
            return "tcp_init() succeeded";
        return in->took < 2.0 ? NULL : "tcp_init() waited for the timeout";
    }
    why = wr_registered(lfd, mgr, &port);
    if (why)
        return why;

    switch (c->scenario) {
    case WR_ASSIGNED:
    case WR_TWICE:
    case WR_FROM_PEER:
        if (!wr_assign(*mgr, WR_ID))
            return "could not answer the registration";
        wr_init_join(in);
        why = wr_after_assign(in, *mgr);
        if (!why && c->scenario == WR_TWICE)
            why = wr_twice(in, *mgr);
        if (!why && c->scenario == WR_FROM_PEER)
            why = wr_from_peer(in, *mgr, port, peer);
        return why;
    case WR_REFUSED:
        if (!send_register_resp(*mgr, 0, -EPROTO,
                                (uint32_t)sizeof(TcpRegisterRespPayload)))
            return "could not answer the registration";
        break;
    case WR_RESULT_MIN:
        if (!send_register_resp(*mgr, 0, INT32_MIN,
                                (uint32_t)sizeof(TcpRegisterRespPayload)))
            return "could not answer the registration";
        break;
    case WR_OLD_VERSION:
        if (!send_register_resp(*mgr, WR_ID, 0, TCP_REGISTER_RESP_LEN_V3))
            return "could not answer the registration";
        break;
    case WR_SHORT:
        if (!send_register_resp(*mgr, WR_ID, 0, TCP_REGISTER_RESP_LEN_V3 - 1))
            return "could not answer the registration";
        break;
    case WR_ID_ZERO:
        if (!wr_assign(*mgr, 0))
            return "could not answer the registration";
        break;
    case WR_ID_NONE:
        if (!wr_assign(*mgr, UINT32_MAX))
            return "could not answer the registration";
        break;
    case WR_NO_ACCEPT:
    case WR_NO_PEERS:
        if (!wr_assign(*mgr, WR_ID))
            return "could not answer the registration";
        break;
    case WR_EARLY_PEER:
        return wr_early_peer(in, *mgr, port, peer);
    case WR_EARLY_TOPOLOGY:
        low = listen_loopback();
        high = listen_loopback();
        why = low < 0 || high < 0
                  ? "could not open the peers' listeners"
                  : wr_early_topology(in, *mgr, low, high, peer);
        if (low >= 0)
            close(low);
        if (high >= 0)
            close(high);
        return why;
    case WR_NO_RECV: /* handled above */
    default:
        return "unknown scenario";
    }

    /* The rows in which tcp_init() fails */
    wr_init_join(in);
    if (in->ret == 0)
        return "tcp_init() succeeded";
    return in->took < 2.0 ? NULL : "tcp_init() waited for the timeout";
}

/*
 * Run one registration row: start tcp_init() as a worker, play the manager
 * for it, then stop the worker, if it started, and check that every thread
 * it created exited.
 */
static int run_wr_case(const struct wr_case *c)
{
    struct wr_init in;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    int lfd, mgr = -1, peer = -1;
    int creates_left = wr_creates_left(c->scenario);
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    atomic_store(&g_create_failures, 0);
    memset(&in, 0, sizeof(in));
    in.interval = LONG_INTERVAL_S;
    in.ret = -1;

    g_current_row = c->description;
    alarm(row_timeout_s());
    lfd = listen_loopback();
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
        why = "could not open the manager's listener";
    } else {
        snprintf(in.config, sizeof(in.config), "tcp:worker:127.0.0.1:%u",
                 (unsigned)ntohs(addr.sin_port));
        atomic_store(&g_creates_left, creates_left);
        /* Not counted: it isn't one of the worker's threads */
        if (pthread_create(&in.tid, NULL, wr_init_run, &in) != 0)
            why = "could not start tcp_init()";
        else
            in.running = true;
    }
    if (!why)
        why = wr_run(c, &in, lfd, &mgr, &peer);

    /* A row that failed before the answer: refuse, so tcp_init() returns */
    if (in.running && mgr >= 0)
        send_register_resp(mgr, 0, -EPROTO,
                           (uint32_t)sizeof(TcpRegisterRespPayload));
    wr_init_join(&in);
    atomic_store(&g_creates_left, -1);
    if (in.ret == 0)
        rdma_backend_ops_tcp.fini(&in.dev);
    alarm(0);
    if (!why && creates_left >= 0 &&
        (atomic_load(&g_creates) != creates_left ||
         atomic_load(&g_create_failures) != 1))
        why = "the thread that couldn't be created wasn't the expected one";
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread the worker started wasn't joined exactly once";

    ok = !why;
    printf("%s worker registration %s: %s; expected: %s\n",
           ok ? "ok  " : "FAIL", c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; tcp_init() returned %d after %.3f s, %d thread(s) "
               "created, %d joined, %d exited\n",
               why, in.ret, in.took, atomic_load(&g_creates),
               atomic_load(&g_joins), atomic_load(&g_exits));
    fflush(stdout);

    if (peer >= 0)
        close(peer);
    if (mgr >= 0)
        close(mgr);
    if (lfd >= 0)
        close(lfd);
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
                           const atomic_bool *running, int *ret)
{
    static const struct {
        TcpMsgType type;
        uint32_t seq, len;
        uint8_t fill;
    } want[] = {{TCP_MSG_HEARTBEAT, 100, SP_A_LEN, 0xa5},
                {TCP_MSG_HANDSHAKE, 200, SP_B_LEN, 0x5a}};
    uint64_t deadline_ms =
        c->deadline_ms ? ernic_monotonic_ms() + c->deadline_ms : 0;

    for (int i = 0; i <= c->want_msgs; i++) {
        TcpMsgHeader hdr;
        void *payload = NULL;
        const char *why = NULL;

        *ret = tcp_recv_message(fd, &hdr, &payload, running, deadline_ms);
        if (i == c->want_msgs)
            why = payload ? "the last call returned a payload" : NULL;
        else if (*ret != 0)
            why = "a message wasn't read";
        else if (!sp_matches(&hdr, payload, want[i].type, want[i].seq,
                             want[i].len, want[i].fill))
            why = "a message read isn't the one sent";
        free(payload);
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
    alarm(row_timeout_s());
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
     * ernic_monotonic_ms() truncates to the millisecond, so the deadline can
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
    uint32_t interval; /* health_check_interval_sec */
};

static const struct reap_case reap_cases[] = {
    {"positive", "100 peers that connect and hang up without registering",
     "each is closed, and its thread joined, by the time the next connects: "
     "one left waiting, and 2 descriptors more open, not 102",
     REAP_PROBES, LONG_INTERVAL_S},
    {"negative", "a peer refused for running an older protocol version",
     "sent the refusal, then closed by the next health-check pass",
     REAP_REFUSED, 1},
    {"negative",
     "a peer that, before registering, sends an ionic message for a "
     "registered worker",
     "not relayed; the connection is closed by the next health-check pass",
     REAP_EARLY, 1},
    {"negative", "a peer that has sent nothing for the registration timeout",
     "closed by the next health-check pass", REAP_TIMEOUT, 1},
    {"boundary", "a peer that has sent nothing, with time left to register",
     "kept through two health-check passes", REAP_YOUNG, 1},
    {"boundary", "64 peers waiting to register, then another",
     "the next is closed at once; once one of the 64 hangs up, the one after "
     "it is let in",
     REAP_CAP, LONG_INTERVAL_S},
    {"negative", "a peer that connects while the manager is out of descriptors",
     "accept() is retried every 100 ms, not in a loop that floods the log, "
     "and succeeds once descriptors are free",
     REAP_NO_FDS, LONG_INTERVAL_S},
    {"negative", "a registered worker that hangs up",
     "the next health-check pass closes its connection, joins its thread, "
     "takes it out of the connections table, and reports it dead to the "
     "other workers",
     REAP_HANGUP, 1},
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
static bool wait_for_count(const atomic_int *count, int n)
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
    /* As a worker answers, so no pass finds the worker silent and closes it */
    if (!send_heartbeat(w, 1))
        return "could not answer the health check";
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
 * Make every peer waiting to register look as if it had just connected, so
 * that none reaches TCP_REGISTER_TIMEOUT_MS before the next peer connects.
 * Before it counts the peers waiting, the accept thread reaps those that
 * have, so on a slow machine, such as under valgrind, one timing out would
 * make room under the cap.  REAP_TIMEOUT checks the timeout.
 */
static void reap_renew(TcpBackendPrivate *priv)
{
    qemu_mutex_lock(&priv->conn_table_lock);
    uint64_t now_ms = ernic_monotonic_ms();

    for (guint i = 0; i < priv->pending_conns->len; i++) {
        TcpConnection *conn = g_ptr_array_index(priv->pending_conns, i);

        conn->accepted_ms = now_ms;
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
}

/*
 * REAP_CAP: a peer past the cap is closed without a receive thread.  One of
 * those waiting hangs up, and the next peer takes its place.
 */
static const char *reap_cap(TcpBackendPrivate *priv, uint16_t port,
                            struct reap_fds *k)
{
    static char why[256];
    int base = atomic_load(&g_creates);
    int cap = (int)TCP_MAX_PENDING_CONNS;
    uint32_t first = k->n;
    uint64_t start_ms = ernic_monotonic_ms();
    guint waiting;

    for (int i = 0; i < cap; i++) {
        reap_renew(priv);
        if (reap_keep(k, reap_connect(port, base + i + 1)) < 0)
            return "a peer under the cap wasn't given a receive thread";
    }
    /*
     * A peer's receive thread is created under conn_table_lock, which is
     * held until the peer is on the list, so this counts the last peer too.
     */
    waiting = pending_count(priv);
    if (waiting != TCP_MAX_PENDING_CONNS) {
        snprintf(why, sizeof(why),
                 "only %u of the %d peers under the cap are waiting, %d "
                 "receive thread(s) exited, and they took %" PRIu64
                 " ms to connect",
                 waiting, cap, atomic_load(&g_exits),
                 ernic_monotonic_ms() - start_ms);
        return why;
    }
    reap_renew(priv);
    if (!closed_within(reap_keep(k, connect_loopback(port)), 2000)) {
        snprintf(why, sizeof(why),
                 "the peer past the cap was kept, with %u waiting and %d "
                 "receive thread(s) exited",
                 pending_count(priv), atomic_load(&g_exits));
        return why;
    }
    if (atomic_load(&g_creates) != base + cap)
        return "the peer past the cap was given a receive thread";

    close(k->fd[first]);
    k->fd[first] = -1;
    if (!wait_for_exits(1))
        return "the thread didn't see its peer hang up";
    reap_renew(priv);
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
        if (tcp_recv_message(fd, &hdr, &payload, NULL, 0) < 0)
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
        free(payload);
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
    else if (!why && table_get(priv, 2))
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

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 || tcp_init_with_interval(&dev, config, c->interval) != 0) {
        printf("FAIL reap %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    why = reap_run(dev.backend_private, port, c->scenario, &k, &errors);

    start = now_s();
    rdma_backend_ops_tcp.fini(&dev);
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
 * so any registered worker could keep another, dead, node alive.  A heartbeat
 * is now credited to the node whose connection it came on.
 *
 * A node the manager had declared dead that answered a heartbeat again
 * stayed dead to the rest of the mesh until its topology next changed.  The
 * manager now reports it alive.
 *
 * The data-path messages the PVRDMA device used are retired.  Their handlers
 * answered a request, a completion or the memory asked for, and kept data
 * that came before a receive was posted until the QP was destroyed, which a
 * peer could use to grow the node's memory without bound.  A retired message
 * now gets no answer and nothing of it is kept.
 *
 * The ionic data path's messages are handed to its callback with the buffer
 * they were received into, which the callback then owns, so a large message
 * isn't copied.  With no callback, the manager frees the buffer itself.  A
 * callback with no room for the message refuses it, leaving the buffer with
 * the receive thread, which offers it again, and reads nothing more from the
 * peer, until it is taken or the thread is stopped.  The peer is slowed down
 * by TCP, rather than the node buffering what it can't keep up with.
 *
 * Each row starts a manager with tcp_init(), stops its health-check thread,
 * and registers two workers over socketpairs, as in the fini rows: nodes 1
 * and 2.  The health check's first pass comes as soon as the thread runs,
 * which can be after the row has set up the nodes' liveness or drained the
 * workers' sockets, and its heartbeats, or a node it declares dead, would
 * then fail the row.  Stopping the thread first makes that pass come first.
 */

enum sender_scenario {
    SENDER_HEARTBEAT, /* worker 1's heartbeat names node 2, which is dead */
    SENDER_REVIVES,   /* node 2, declared dead, answers a heartbeat */
    SENDER_RETIRED,   /* worker 1 sends the retired data-path messages */
    SENDER_IONIC,     /* worker 1 sends the manager's data path a message */
    SENDER_NO_DP,     /* and the manager has no data path */
    SENDER_FULL,      /* and the data path has no room for it, for a while */
    SENDER_FULL_STOP, /* and never has, and the manager is torn down */
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
    {"negative",
     "worker 1 sends each retired data-path message, naming node 2 as its "
     "sender",
     "ignored: nothing is sent to either worker, and worker 1 is still "
     "answered",
     SENDER_RETIRED},
    {"positive", "worker 1 sends an ionic message to the manager's data path",
     "the data path's callback is handed the payload, from node 1, once, and "
     "keeps it after the call",
     SENDER_IONIC},
    {"negative",
     "worker 1 sends an ionic message to a manager with no data path",
     "dropped, and worker 1 is still answered", SENDER_NO_DP},
    {"positive",
     "worker 1 sends an ionic message the manager's data path has no room "
     "for, then a heartbeat, and then the data path makes room",
     "the message is offered until it is taken, once, whole; the heartbeat "
     "isn't read until then, and is answered after",
     SENDER_FULL},
    {"corner",
     "worker 1 sends an ionic message the manager's data path never has room "
     "for, and the manager is torn down",
     "the receive thread stops offering it, gives it up, and is joined",
     SENDER_FULL_STOP},
};

/* The payload each retired or ionic message carries */
#define SENDER_LEN  8u
#define SENDER_BYTE 0xab

/*
 * Set node id's liveness, as the manager declares it, with its claims, and
 * the time of its last heartbeat.
 */
static void node_set(TcpBackendPrivate *priv, uint32_t id, bool alive,
                     uint64_t last_ms)
{
    MeshNodeInfo *node;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(id));
    if (node) {
        tcp_node_set_alive(priv, node, alive, NULL);
        node->last_heartbeat_ms = last_ms;
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);
}

/* Get them; false if there is no node id. */
static bool node_get(TcpBackendPrivate *priv, uint32_t id, bool *alive,
                     uint64_t *last_ms)
{
    MeshNodeInfo *node;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(id));
    if (node) {
        *alive = node->is_alive;
        *last_ms = node->last_heartbeat_ms;
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);
    return node != NULL;
}

/*
 * SENDER_HEARTBEAT: node 1 is alive but hasn't answered for a while, and
 * node 2 is dead.  The manager answers the heartbeat once it has recorded
 * it.
 */
static const char *sender_heartbeat(TcpBackendPrivate *priv, int w1)
{
    uint64_t dead_since = ernic_monotonic_ms() - UINT64_C(1000000), before,
             last1, last2;
    bool alive1, alive2;

    node_set(priv, 1, true, dead_since);
    node_set(priv, 2, false, dead_since);
    before = ernic_monotonic_ms();
    if (!send_heartbeat(w1, 2))
        return "could not send the heartbeat";
    if (!wait_for_type(w1, TCP_MSG_HEARTBEAT_RESP, 2000))
        return "the heartbeat wasn't answered";
    if (!node_get(priv, 1, &alive1, &last1) ||
        !node_get(priv, 2, &alive2, &last2))
        return "a node is missing";
    if (alive2 || last2 != dead_since)
        return "node 2 was revived by a heartbeat that wasn't its own";
    /*
     * On a machine up for less than 1000 s, dead_since wraps to a value
     * above before, so it has to be ruled out separately.
     */
    return last1 != dead_since && last1 >= before
               ? NULL
               : "the heartbeat wasn't credited to node 1, whose connection "
                 "it came on";
}

/*
 * Wait up to timeout_ms for the manager's broadcast passes to have taken n
 * asks, and sent what they took.
 */
static bool broadcasts_sent_reach(TcpBackendPrivate *priv, uint64_t n,
                                  int timeout_ms)
{
    for (int i = 0; i <= timeout_ms / 10; i++) {
        uint64_t sent;

        qemu_mutex_lock(&priv->broadcast_lock);
        sent = priv->broadcasts_sent;
        qemu_mutex_unlock(&priv->broadcast_lock);
        if (sent >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/*
 * Wait up to 5 s for the manager's broadcaster to have sent everything it
 * has been asked for so far.
 */
static bool broadcasts_settled(TcpBackendPrivate *priv)
{
    uint64_t asked;

    qemu_mutex_lock(&priv->broadcast_lock);
    asked = priv->broadcasts_asked;
    qemu_mutex_unlock(&priv->broadcast_lock);
    return broadcasts_sent_reach(priv, asked, 5000);
}

/*
 * Wait for the manager to finish with everything the worker on fd has sent
 * it so far, its registration included.  The worker's receive thread
 * handles one message at a time, so once it has answered a heartbeat sent
 * now, the handlers before it have returned, and asked the broadcaster for
 * what they had to tell the mesh.  Once the broadcaster has sent that too,
 * it is waiting in the workers' sockets.
 */
static bool worker_settled(TcpBackendPrivate *priv, int fd, uint32_t id)
{
    return send_heartbeat(fd, id) &&
           wait_for_type(fd, TCP_MSG_HEARTBEAT_RESP, 2000) &&
           broadcasts_settled(priv);
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
    uint64_t last;
    bool alive;

    if (!worker_settled(priv, w1, 1) || !worker_settled(priv, w2, 2))
        return "a registration wasn't finished";
    count_type(w1, TCP_MSG_MESH_TOPOLOGY);
    node_set(priv, 2, false, ernic_monotonic_ms() - UINT64_C(1000000));
    if (!send_heartbeat(w2, 2))
        return "could not send the heartbeat";
    if (!reported(w1, 2, true, 2000))
        return "worker 1 wasn't told node 2 is alive again";
    if (!node_get(priv, 2, &alive, &last) || !alive)
        return "node 2 is still dead";
    return NULL;
}

/* Send SENDER_LEN bytes of SENDER_BYTE as a message of type from node src */
static bool send_payload(int fd, TcpMsgType type, uint32_t src)
{
    TcpMsgHeader hdr;
    uint8_t buf[sizeof(hdr) + SENDER_LEN];

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.msg_len = htonl(SENDER_LEN);
    hdr.seq = htonl(1);
    hdr.src_node_id = htonl(src);
    hdr.src_qpn = htonl(11);
    hdr.dst_qpn = htonl(12);
    memset(buf, SENDER_BYTE, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));
    return send(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf);
}

/*
 * Read messages from fd until one of type arrives, for up to timeout_ms.
 * Returns how many others came first, or -1 if none of type came.
 */
static int others_before(int fd, TcpMsgType type, int timeout_ms)
{
    double end = now_s() + timeout_ms / 1000.0;
    int others = 0;

    while (now_s() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int left = (int)((end - now_s()) * 1000.0);
        TcpMsgHeader hdr;
        void *payload = NULL;

        if (poll(&pfd, 1, left > 0 ? left : 1) != 1)
            continue;
        if (tcp_recv_message(fd, &hdr, &payload, NULL, 0) < 0)
            return -1;
        free(payload);
        if (hdr.msg_type == type)
            return others;
        others++;
    }
    return -1;
}

/*
 * SENDER_RETIRED.  Worker 1's receive thread on the manager handles one
 * message at a time, so once it has answered a heartbeat sent after the
 * retired messages, anything it sent for them, to either worker, is already
 * waiting.
 */
static const char *sender_retired(TcpBackendPrivate *priv, int w1, int w2)
{
    static const TcpMsgType retired[] = {
        TCP_MSG_RESERVED_3,  TCP_MSG_RESERVED_4,  TCP_MSG_RESERVED_5,
        TCP_MSG_RESERVED_6,  TCP_MSG_RESERVED_7,  TCP_MSG_RESERVED_8,
        TCP_MSG_RESERVED_9,  TCP_MSG_RESERVED_10, TCP_MSG_RESERVED_11,
        TCP_MSG_RESERVED_21, TCP_MSG_RESERVED_22, TCP_MSG_RESERVED_23,
    };
    struct pollfd pfd = {.fd = w2, .events = POLLIN};

    /* Finish the registrations, and discard what they sent */
    if (!worker_settled(priv, w1, 1) || !worker_settled(priv, w2, 2))
        return "a registration wasn't finished";
    count_type(w1, TCP_MSG_HEARTBEAT_RESP);
    count_type(w2, TCP_MSG_HEARTBEAT_RESP);

    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (!send_payload(w1, retired[i], 2))
            return "could not send a retired message";
    if (!send_heartbeat(w1, 1))
        return "could not send the heartbeat";

    switch (others_before(w1, TCP_MSG_HEARTBEAT_RESP, 2000)) {
    case -1:
        return "the heartbeat after the retired messages wasn't answered";
    case 0:
        break;
    default:
        return "a retired message was answered";
    }
    return poll(&pfd, 1, 0) == 0 ? NULL
                                 : "a retired message sent worker 2 something";
}

/*
 * What sender_ionic_recv() was handed.  It runs on the manager's receive
 * thread and the row reads these on its own, so they are atomic; buf is
 * stored last, so a row that finds it also finds len and src.
 */
static atomic_int g_ionic_calls;
static atomic_size_t g_ionic_len;
static atomic_uint g_ionic_src;
static _Atomic(void *) g_ionic_buf;

/*
 * The data path's callback, keeping the first payload, as the data path
 * queues it, for the row to read after the call has returned and then free.
 */
static bool sender_ionic_recv(void *opaque, uint32_t src_node, void *buf,
                              size_t len)
{
    void *none = NULL;

    (void)opaque;
    atomic_store(&g_ionic_len, len);
    atomic_store(&g_ionic_src, src_node);
    if (atomic_compare_exchange_strong(&g_ionic_buf, &none, buf))
        buf = NULL;
    atomic_fetch_add(&g_ionic_calls, 1);
    free(buf);
    return true;
}

/* Whether sender_ionic_recv() was handed worker 1's payload, once */
static const char *sender_ionic_check(const uint8_t *buf)
{
    if (atomic_load(&g_ionic_calls) != 1 || !buf)
        return "the callback wasn't called once";
    if (atomic_load(&g_ionic_src) != 1)
        return "the callback wasn't told the message is from node 1";
    if (atomic_load(&g_ionic_len) != SENDER_LEN)
        return "the callback wasn't given the payload's length";
    for (size_t i = 0; i < SENDER_LEN; i++)
        if (buf[i] != SENDER_BYTE)
            return "the callback wasn't given the payload";
    return NULL;
}

/*
 * SENDER_IONIC and SENDER_NO_DP: as in SENDER_RETIRED, once the
 * heartbeat sent after the message is answered, the message has been
 * handled.  The callback owns the payload: if the manager freed it as well,
 * reading it here is a use after free, and freeing it a double free.  With
 * no callback, the manager frees it, or leaks it.  Only the sanitizers and
 * valgrind see either.
 */
static const char *sender_ionic(RdmaBackendDev *dev, int w1, bool registered)
{
    uint8_t *buf;
    const char *why = NULL;

    if (!worker_settled(dev->backend_private, w1, 1))
        return "worker 1's registration wasn't finished";
    count_type(w1, TCP_MSG_HEARTBEAT_RESP);

    atomic_store(&g_ionic_calls, 0);
    atomic_store(&g_ionic_buf, NULL);
    /* tcp_init() leaves this to rdma_backend_init_with_ops(), which the row
     * doesn't use, and the callback is only set on a TCP backend */
    dev->backend_type = RDMA_BACKEND_TYPE_TCP;
    if (registered)
        tcp_backend_set_ionic_recv_cb(dev, sender_ionic_recv, NULL);
    if (!send_payload(w1, TCP_MSG_IONIC, 1))
        why = "could not send the ionic message";
    else if (!worker_settled(dev->backend_private, w1, 1))
        why = "the heartbeat after the ionic message wasn't answered";
    tcp_backend_set_ionic_recv_cb(dev, NULL, NULL);

    buf = atomic_load(&g_ionic_buf);
    if (!why && registered)
        why = sender_ionic_check(buf);
    else if (!why && atomic_load(&g_ionic_calls) != 0)
        why = "a callback was called";
    free(buf);
    return why;
}

/*
 * Whether full_ionic_recv() refuses what it is offered, as the data path does
 * while its queue is full, and how many times it has.  The row sets the
 * first, and the callback, on the manager's receive thread, counts the
 * second.
 */
static atomic_bool g_ionic_full;
static atomic_int g_ionic_refusals;

/* The data path's callback, with no room until g_ionic_full is cleared */
static bool full_ionic_recv(void *opaque, uint32_t src_node, void *buf,
                            size_t len)
{
    if (atomic_load(&g_ionic_full)) {
        atomic_fetch_add(&g_ionic_refusals, 1);
        return false;
    }
    return sender_ionic_recv(opaque, src_node, buf, len);
}

/* Wait up to 5 s for full_ionic_recv() to have refused n offers */
static bool refusals_reach(int n)
{
    for (int i = 0; i < 500; i++) {
        if (atomic_load(&g_ionic_refusals) >= n)
            return true;
        sleep_ms(10);
    }
    return false;
}

/*
 * SENDER_FULL and SENDER_FULL_STOP.  The receive thread handles one message
 * at a time, so while it is still offering the ionic message after the
 * heartbeat was sent, it hasn't read the heartbeat.  In SENDER_FULL_STOP the
 * callback stays registered, refusing, for the row's teardown, which has to
 * stop the thread; the payload it gave up is freed, or leaked, which only
 * the sanitizers and valgrind see.
 */
static const char *sender_full(RdmaBackendDev *dev, int w1, bool stop)
{
    const char *why = NULL;
    uint8_t *buf;

    if (!worker_settled(dev->backend_private, w1, 1))
        return "worker 1's registration wasn't finished";
    count_type(w1, TCP_MSG_HEARTBEAT_RESP);

    atomic_store(&g_ionic_calls, 0);
    atomic_store(&g_ionic_buf, NULL);
    atomic_store(&g_ionic_refusals, 0);
    atomic_store(&g_ionic_full, true);
    /* As in sender_ionic() */
    dev->backend_type = RDMA_BACKEND_TYPE_TCP;
    tcp_backend_set_ionic_recv_cb(dev, full_ionic_recv, NULL);
    if (!send_payload(w1, TCP_MSG_IONIC, 1)) {
        why = "could not send the ionic message";
    } else if (!refusals_reach(2)) {
        why = "the refused message wasn't offered again";
    } else if (stop) {
        return NULL;
    } else if (!send_heartbeat(w1, 1)) {
        why = "could not send the heartbeat";
    } else if (!refusals_reach(atomic_load(&g_ionic_refusals) + 2)) {
        why = "the message wasn't offered again after the heartbeat";
    } else if (count_type(w1, TCP_MSG_HEARTBEAT_RESP) != 0) {
        why = "the heartbeat behind the refused message was answered";
    }

    atomic_store(&g_ionic_full, false);
    if (!why && !wait_for_type(w1, TCP_MSG_HEARTBEAT_RESP, 2000))
        why = "the heartbeat wasn't answered once the message was taken";
    tcp_backend_set_ionic_recv_cb(dev, NULL, NULL);

    buf = atomic_load(&g_ionic_buf);
    if (!why)
        why = sender_ionic_check(buf);
    free(buf);
    return why;
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

static const char *sender_run(RdmaBackendDev *dev, int w1, int w2,
                              enum sender_scenario scenario)
{
    TcpBackendPrivate *priv = dev->backend_private;

    switch (scenario) {
    case SENDER_HEARTBEAT:
        return sender_heartbeat(priv, w1);
    case SENDER_REVIVES:
        return sender_revives(priv, w1, w2);
    case SENDER_RETIRED:
        return sender_retired(priv, w1, w2);
    case SENDER_IONIC:
        return sender_ionic(dev, w1, true);
    case SENDER_NO_DP:
        return sender_ionic(dev, w1, false);
    case SENDER_FULL:
        return sender_full(dev, w1, false);
    case SENDER_FULL_STOP:
        return sender_full(dev, w1, true);
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
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 ||
        tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0) {
        printf("FAIL sender %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;
    tcp_stop_health_check_thread(priv);

    w1 = worker_register(priv);
    if (w1 < 0 || !wait_for_registered(priv, 1))
        why = "worker 1 did not register";
    else if ((w2 = worker_register(priv)) < 0 || !wait_for_registered(priv, 2))
        why = "worker 2 did not register";
    else
        why = sender_run(&dev, w1, w2, c->scenario);

    rdma_backend_ops_tcp.fini(&dev);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

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

/* ---- Registering again ------------------------------------------------- */

/*
 * A worker that has given up on its connection to the manager connects
 * again and asks for its own id back.  The manager used to give a worker
 * that asked for a taken id another one, and a node wasn't evicted, so a
 * worker's own id was always taken: it would have come back as a second
 * node, claiming its GIDs under both ids.  The manager now gives a node its
 * id back if it asks from the address it registered, and can't be answering
 * on the connection it had, and otherwise refuses it, giving it no id at
 * all.  A node the manager had declared dead owns its GIDs again at once,
 * before it advertises them.
 *
 * Each row starts a manager with tcp_init(), stops its health-check thread,
 * as the sender rows do, and registers worker 1 over a socketpair.  A second
 * socketpair then registers asking for an id.
 */

enum ra_scenario {
    RA_HUNG_UP,    /* worker 1's connection has ended */
    RA_DEAD,       /* worker 1 is declared dead, its connection still up */
    RA_LIVE,       /* worker 1 is alive and connected */
    RA_OTHER_PORT, /* worker 1 is dead; the registration is from another port */
    RA_UNKNOWN,    /* a registration asking for an id no node has */
    RA_MANAGER,    /* one asking for node id 0, the manager's */
};

struct ra_case {
    const char *category;
    const char *description;
    const char *expected;
    enum ra_scenario scenario;
};

static const struct ra_case ra_cases[] = {
    {"positive",
     "a worker whose connection ended registers again, asking for its id",
     "given its id back; the new connection is in the table, the old one "
     "closed, and the mesh has no new node",
     RA_HUNG_UP},
    {"positive",
     "a worker the manager declared dead registers again, its old connection "
     "still up",
     "given its id back, reported alive, and its old connection closed; it "
     "owns its GID again before it advertises it",
     RA_DEAD},
    {"negative", "a registration asking for the id of a worker that is alive",
     "refused with -EBUSY, and given no other id; the worker's connection is "
     "kept",
     RA_LIVE},
    {"negative",
     "a registration asking for a dead worker's id, from another "
     "port",
     "refused with -EACCES", RA_OTHER_PORT},
    {"negative", "a registration asking for an id no node has",
     "refused with -ENOENT, and given neither that id nor another", RA_UNKNOWN},
    {"boundary", "a registration asking for node id 0, the manager's",
     "refused with -ENOENT", RA_MANAGER},
};

/* The one GID of RA_DEAD: ::ffff:192.168.251.7 */
static const uint8_t ra_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                   0, 0, 0xff, 0xff, 192, 168, 251, 7};

static uint32_t ra_resolve(TcpBackendPrivate *priv)
{
    union ibv_gid gid;

    memcpy(gid.raw, ra_gid, 16);
    return tcp_resolve_node_from_gid(priv, &gid);
}

/*
 * Read messages on fd, for up to 5 s, until the answer to a registration,
 * and return the id and result it carries.
 */
static bool register_answer(int fd, uint32_t *id, int32_t *result)
{
    TcpMsgHeader hdr;
    void *payload;
    bool ok;

    if (!wr_until(fd, TCP_MSG_REGISTER_RESP, 5000, &hdr, &payload, NULL))
        return false;
    ok = payload && hdr.msg_len >= sizeof(TcpRegisterRespPayload);
    if (ok) {
        const TcpRegisterRespPayload *resp = payload;

        *id = ntohl(resp->assigned_node_id);
        *result = (int32_t)ntohl(resp->result);
    }
    free(payload);
    return ok;
}

/* Read and discard what comes on fd, for up to 5 s, until it ends. */
static bool ends(int fd)
{
    double end = now_s() + 5.0;

    while (now_s() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        char buf[4096];

        if (poll(&pfd, 1, 100) == 1 && recv(fd, buf, sizeof(buf), 0) <= 0)
            return true;
    }
    return false;
}

/*
 * Read messages on fd, for up to 2 s, until an advertisement for node id;
 * returns how many GIDs it carries, or -1 if none came.
 */
static int gids_for(int fd, uint32_t id)
{
    double end = now_s() + 2.0;

    while (now_s() < end) {
        int left = (int)((end - now_s()) * 1000.0);
        TcpMsgHeader hdr;
        void *payload;
        int n = -1;

        if (!wr_until(fd, TCP_MSG_NODE_GIDS, left > 0 ? left : 1, &hdr,
                      &payload, NULL))
            return -1;
        if (payload && hdr.msg_len >= sizeof(TcpNodeGidsPayload) &&
            ntohl(((const TcpNodeGidsPayload *)payload)->node_id) == id)
            n = (int)ntohl(((const TcpNodeGidsPayload *)payload)->num_gids);
        free(payload);
        if (n >= 0)
            return n;
    }
    return -1;
}

/*
 * RA_DEAD and RA_OTHER_PORT: worker 1 had claimed ra_gid, and the manager
 * declared it dead, as the health check does a node that stops answering,
 * which takes its claims out of resolution.
 */
static const char *ra_declare_dead(TcpBackendPrivate *priv)
{
    tcp_gid_claims_set(priv, 1, (const uint8_t(*)[16]) & ra_gid, 1);
    if (ra_resolve(priv) != 1)
        return "worker 1's claim did not take effect";
    node_set(priv, 1, false, ernic_monotonic_ms() - UINT64_C(1000000));
    return ra_resolve(priv) == UINT32_MAX
               ? NULL
               : "a node declared dead still owned its GID";
}

/*
 * The checks once the second connection, fresh, at *w2, has been answered.
 * old is worker 1's connection before, which the caller holds a reference
 * to, and nodes how many nodes the mesh had.
 */
static const char *ra_check(TcpBackendPrivate *priv, enum ra_scenario scenario,
                            int w1, int w2, TcpConnection *old,
                            const TcpConnection *fresh, guint nodes,
                            uint32_t id, int32_t result)
{
    bool alive = false;
    uint64_t last;

    switch (scenario) {
    case RA_HUNG_UP:
    case RA_DEAD:
        if (result != 0 || id != 1)
            return "worker 1 wasn't given its id back";
        if (table_get(priv, 1) != fresh)
            return "the new connection isn't node 1's";
        if (!conn_retired(old))
            return "the old connection wasn't closed";
        if (mesh_node_count(priv) != nodes)
            return "the mesh has a new node";
        if (scenario == RA_HUNG_UP)
            return NULL;
        if (!node_get(priv, 1, &alive, &last) || !alive)
            return "worker 1 is still dead";
        if (ra_resolve(priv) != 1)
            return "worker 1 didn't own its GID again";
        if (!reported(w2, 1, true, 2000))
            return "the workers weren't told worker 1 is alive";
        if (gids_for(w2, 1) != 1)
            return "the workers weren't told worker 1 owns its GID";
        return ends(w1) ? NULL : "worker 1's old connection didn't end";
    case RA_LIVE:
    case RA_OTHER_PORT:
    case RA_UNKNOWN:
    case RA_MANAGER:
        if (result != (scenario == RA_LIVE         ? -EBUSY
                       : scenario == RA_OTHER_PORT ? -EACCES
                                                   : -ENOENT) ||
            id != 0)
            return "refused with the wrong answer, or given an id";
        if (!wait_dead(fresh))
            return "the refused connection was read on";
        if (table_get(priv, 1) != old || conn_retired(old))
            return "worker 1's connection was changed";
        if (mesh_node_count(priv) != nodes)
            return "the mesh has a new node";
        return pending_count(priv) == 1
                   ? NULL
                   : "the refused connection isn't waiting to be reaped";
    default:
        return "unknown scenario";
    }
}

static const char *ra_run(TcpBackendPrivate *priv, int w1,
                          enum ra_scenario scenario, int *w2)
{
    TcpConnection *old = tcp_connection_lookup(priv, 1), *fresh = NULL;
    uint32_t want = 1, id = UINT32_MAX;
    uint16_t port = WORKER_PORT;
    int32_t result = 1;
    guint nodes;
    const char *why = NULL;

    if (!old)
        return "worker 1 isn't node 1";
    if (!worker_settled(priv, w1, 1))
        why = "worker 1's registration wasn't finished";
    nodes = mesh_node_count(priv);

    switch (scenario) {
    case RA_HUNG_UP:
        if (!why && (shutdown(w1, SHUT_RDWR) < 0 || !wait_dead(old)))
            why = "worker 1's connection didn't end";
        break;
    case RA_DEAD:
        why = why ? why : ra_declare_dead(priv);
        break;
    case RA_OTHER_PORT:
        why = why ? why : ra_declare_dead(priv);
        port = WORKER_PORT + 1;
        break;
    case RA_UNKNOWN:
        want = 42;
        break;
    case RA_MANAGER:
        want = 0;
        break;
    case RA_LIVE:
    default:
        break;
    }

    if (!why) {
        *w2 = worker_attach(priv, &fresh);
        if (*w2 < 0 || !send_register_from(*w2, "127.0.0.1", port, want))
            why = "could not register again";
        else if (!register_answer(*w2, &id, &result))
            why = "the registration wasn't answered";
        else
            why = ra_check(priv, scenario, w1, *w2, old, fresh, nodes, id,
                           result);
    }
    tcp_connection_unref(old);
    return why;
}

static int run_ra_case(const struct ra_case *c)
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
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 ||
        tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0) {
        printf("FAIL register again %s: %s; could not start a manager\n",
               c->category, c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;
    tcp_stop_health_check_thread(priv);

    w1 = worker_register(priv);
    if (w1 < 0 || !wait_for_registered(priv, 1))
        why = "worker 1 did not register";
    else
        why = ra_run(priv, w1, c->scenario, &w2);

    rdma_backend_ops_tcp.fini(&dev);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s register again %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
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

/* ---- Evicting a dead node ---------------------------------------------- */

/*
 * A manager never dropped a node from its table, and its broadcasts carry at
 * most TCP_MAX_MESH_NODES nodes, so once more nodes than that had registered,
 * restarts included, the ones past the cap, live ones as likely as dead,
 * dropped out of the mesh.  Now a node registering for the first time in a
 * full table evicts the node that has been dead longest, of those with no
 * connection in the table, or one that has ended.  Its id isn't given out
 * again, its claims go, and the workers are told it owns nothing.  If no
 * node can be evicted, the new one is refused with -ENOSPC.  A
 * connection leaves the table before its receive thread is joined, so the
 * thread can still be handling the node's messages once it is evicted; an
 * advertisement handled then must not put the claims back.
 *
 * The health check kept a pointer to each node it was acting on after
 * dropping the lock, which was safe only while nodes were never freed.  It
 * keeps the node's id now, and the health-check row has a pass straddle two
 * evictions.
 *
 * Each row starts a manager with tcp_init(), stops its health-check thread,
 * and registers worker 1 over a socketpair, as the registering-again rows
 * do.  It fills the rest of the table by hand with nodes 2 and up, as a
 * manager leaves the nodes that restarted or went away: dead, with no
 * connection, unless the row says otherwise.  Node EV_OLDEST has
 * been dead longest, then node EV_NEXT, then the nodes with the highest ids,
 * so the oldest is neither the first id nor the last.  Node EV_OLDEST claims
 * ev_gid.  Then a new worker registers.
 */

/*
 * Advertise on fd that node id owns *gid: to a worker, as the manager does,
 * or to the manager, as node id does for itself
 */
static bool send_gids(int fd, uint32_t id, const uint8_t (*gid)[16])
{
    struct {
        TcpMsgHeader hdr;
        TcpNodeGidsPayload gids;
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    msg.hdr.msg_type = htonl(TCP_MSG_NODE_GIDS);
    msg.hdr.msg_len = htonl((uint32_t)sizeof(msg.gids));
    tcp_fill_node_gids(&msg.gids, id, gid, 1);
    return send(fd, &msg, sizeof(msg), MSG_NOSIGNAL) == (ssize_t)sizeof(msg);
}

#define EV_OLDEST 7
#define EV_NEXT   8

enum ev_scenario {
    EV_DEAD,        /* the table is full of dead nodes with no connection */
    EV_ENDED,       /* EV_OLDEST's connection has ended, but is in the table */
    EV_LIVE_CONN,   /* EV_OLDEST is dead, but its connection is still up */
    EV_ROOM,        /* the table has room for one more */
    EV_AGAIN,       /* worker 1, dead longest, registers again under its id */
    EV_NONE,        /* every node is alive, with a connection */
    EV_CONNECTED,   /* every node is dead, but its connection is still up */
    EV_HEALTH,      /* a health-check pass straddles two evictions */
    EV_UNPUBLISHED, /* EV_OLDEST's connection is out of the table, but its
                       thread hasn't been joined, and it advertises */
};

struct ev_case {
    const char *category;
    const char *description;
    const char *expected;
    enum ev_scenario scenario;
};

static const struct ev_case ev_cases[] = {
    {"positive",
     "a new node registers in a full table of dead nodes with no connection",
     "the node dead longest is evicted, with its claims; the new node gets "
     "the next id, and the broadcasts carry every node in the table, then "
     "the evicted one as owning nothing, with no error",
     EV_DEAD},
    {"positive", "the node dead longest has a connection that has ended",
     "it is evicted, and its connection taken out of the table and closed",
     EV_ENDED},
    {"negative",
     "the node dead longest has been declared dead, but its connection is "
     "still up",
     "it is kept, with its connection and claims; the next oldest is "
     "evicted",
     EV_LIVE_CONN},
    {"boundary", "a new node registers in a table one short of full",
     "nothing is evicted", EV_ROOM},
    {"negative",
     "the worker dead longest registers again under its id in a full table",
     "nothing is evicted", EV_AGAIN},
    {"negative", "a new node registers in a full table of live nodes",
     "refused with -ENOSPC: nothing is evicted, no connection is closed, and "
     "the table stays full",
     EV_NONE},
    {"negative",
     "a new node registers in a full table of dead nodes whose connections "
     "are still up, then again once the oldest one's has ended",
     "refused with -ENOSPC, then given the id it would have had, the oldest "
     "node evicted",
     EV_CONNECTED},
    {"corner",
     "a health-check pass decides about two dead nodes, and two "
     "registrations evict them before it acts",
     "the pass reads nothing of the freed nodes, and leaves their "
     "connections to the registrations, which close them",
     EV_HEALTH},
    {"corner",
     "the node dead longest advertises its GID after its eviction, on a "
     "connection out of the table whose thread hasn't been joined",
     "it is evicted, and the advertisement is ignored: its claims stay gone",
     EV_UNPUBLISHED},
};

/* The one GID of EV_OLDEST: ::ffff:192.168.251.11 */
static const uint8_t ev_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                   0, 0, 0xff, 0xff, 192, 168, 251, 11};

/*
 * A row's mesh.  Nodes 2 to last are made by hand; conn[id] is node id's
 * connection, if the row gave it one, and peer[id] the node's end of it.
 * fresh[] are the ends of the workers that register once the table is set.
 */
struct ev_fixture {
    TcpBackendPrivate *priv;
    int w1;
    uint32_t last;
    TcpConnection *conn[TCP_MAX_MESH_NODES];
    int peer[TCP_MAX_MESH_NODES];
    int fresh[2];
};

static uint32_t ev_resolve(TcpBackendPrivate *priv)
{
    union ibv_gid gid;

    memcpy(gid.raw, ev_gid, 16);
    return tcp_resolve_node_from_gid(priv, &gid);
}

/* Put node id in the table, last heard from dead_s seconds ago. */
static bool ev_plant(TcpBackendPrivate *priv, uint32_t id, bool alive,
                     uint64_t dead_s)
{
    MeshNodeInfo *node = tcp_mesh_node_info_new(id, "127.0.0.1", WORKER_PORT);

    if (!node)
        return false;
    node->is_alive = alive;
    node->last_heartbeat_ms = ernic_monotonic_ms() - dead_s * 1000u;
    qemu_mutex_lock(&priv->mesh_table_lock);
    g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(id), node);
    qemu_mutex_unlock(&priv->mesh_table_lock);
    return true;
}

/* Give node id a connection, with its thread running, and end it if ended. */
static bool ev_connect(struct ev_fixture *fx, uint32_t id, bool ended)
{
    TcpConnection *conn = recv_conn_new(fx->priv, id, true, &fx->peer[id]);

    if (!conn)
        return false;
    fx->conn[id] = conn;
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    g_hash_table_insert(fx->priv->connections, GUINT_TO_POINTER(id),
                        tcp_connection_ref(conn));
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    if (!ended)
        return true;
    close(fx->peer[id]);
    fx->peer[id] = -1;
    return wait_dead(conn);
}

/* Fill the table as the header comment says; returns NULL, or what failed */
static const char *ev_setup(struct ev_fixture *fx, enum ev_scenario scenario)
{
    TcpBackendPrivate *priv = fx->priv;

    fx->last =
        scenario == EV_ROOM ? TCP_MAX_MESH_NODES - 2 : TCP_MAX_MESH_NODES - 1;
    for (uint32_t id = 2; id <= fx->last; id++) {
        uint64_t dead_s = id == EV_OLDEST ? 5000u
                          : id == EV_NEXT ? 4000u
                                          : 1000u + id;
        bool connected = scenario == EV_NONE || scenario == EV_CONNECTED;

        if (!ev_plant(priv, id, scenario == EV_NONE, dead_s))
            return "could not make a node";
        if (connected && !ev_connect(fx, id, false))
            return "could not connect a node";
    }
    if ((scenario == EV_ENDED || scenario == EV_HEALTH) &&
        !ev_connect(fx, EV_OLDEST, true))
        return "could not end a node's connection";
    if (scenario == EV_HEALTH && !ev_connect(fx, EV_NEXT, true))
        return "could not end a node's connection";
    if (scenario == EV_LIVE_CONN && !ev_connect(fx, EV_OLDEST, false))
        return "could not connect a node";
    /* As tcp_health_check_unpublish() leaves it until it has retired it */
    if (scenario == EV_UNPUBLISHED) {
        fx->conn[EV_OLDEST] =
            recv_conn_new(priv, EV_OLDEST, true, &fx->peer[EV_OLDEST]);
        if (!fx->conn[EV_OLDEST])
            return "could not connect a node";
    }

    qemu_mutex_lock(&priv->mesh_table_lock);
    priv->next_available_node_id = fx->last + 1;
    qemu_mutex_unlock(&priv->mesh_table_lock);

    tcp_gid_claims_set(priv, EV_OLDEST, (const uint8_t(*)[16]) & ev_gid, 1);
    return ev_resolve(priv) == EV_OLDEST ? NULL
                                         : "node EV_OLDEST's claim didn't take";
}

/* Register a new worker on *fd, asking for want; the answer's id and result */
static const char *ev_register(TcpBackendPrivate *priv, int *fd, uint32_t want,
                               uint32_t *id, int32_t *result)
{
    *fd = worker_attach(priv, NULL);
    if (*fd < 0 || !send_register(*fd, "127.0.0.1", want))
        return "could not register";
    return register_answer(*fd, id, result)
               ? NULL
               : "the registration wasn't answered";
}

static bool ev_in_table(TcpBackendPrivate *priv, uint32_t id)
{
    bool in;

    qemu_mutex_lock(&priv->mesh_table_lock);
    in = g_hash_table_contains(priv->mesh_nodes, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&priv->mesh_table_lock);
    return in;
}

/*
 * Whether nodes 0 to last are all in the table, but gone, which isn't;
 * UINT32_MAX for none
 */
static const char *ev_kept(TcpBackendPrivate *priv, uint32_t last,
                           uint32_t gone)
{
    for (uint32_t id = 0; id <= last; id++) {
        if (id != gone && !ev_in_table(priv, id))
            return gone == UINT32_MAX ? "a node was evicted"
                                      : "the wrong node was evicted";
    }
    return gone == UINT32_MAX || !ev_in_table(priv, gone)
               ? NULL
               : "the node that should have been evicted wasn't";
}

static bool ev_has_claims(TcpBackendPrivate *priv, uint32_t id)
{
    bool has;

    qemu_mutex_lock(&priv->gid_table_lock);
    has = g_hash_table_contains(priv->node_gids, GUINT_TO_POINTER(id));
    qemu_mutex_unlock(&priv->gid_table_lock);
    return has;
}

/*
 * Check the broadcasts that follow the answer to a registration on fd, which
 * gave the new node id fresh, and evicted node evicted to make room: the
 * topology update names every node in the table, which is full, and the GID
 * table advertises each of them, then the evicted node, as owning nothing.
 */
static const char *ev_broadcasts(int fd, uint32_t fresh, uint32_t evicted)
{
    bool seen[TCP_MAX_MESH_NODES + 1] = {false};
    const TcpMeshTopologyPayload *topo;
    TcpMsgHeader hdr;
    void *payload;
    const char *why = NULL;
    uint32_t n;

    if (fresh >= G_N_ELEMENTS(seen))
        return "the new node's id is out of range";
    if (!wr_until(fd, TCP_MSG_MESH_TOPOLOGY, 2000, &hdr, &payload, NULL))
        return "no topology update came";
    if (!payload || hdr.msg_len < sizeof(*topo)) {
        free(payload);
        return "the topology update is short";
    }
    topo = payload;
    n = ntohl(topo->num_nodes);
    /* n distinct ids of 0 to fresh, but evicted, are all of them */
    if (n != TCP_MAX_MESH_NODES)
        why = "the topology update doesn't carry a full table";
    for (uint32_t i = 0; !why && i < n; i++) {
        uint32_t id = ntohl(topo->nodes[i].node_id);

        if (id > fresh || id == evicted || seen[id])
            why = "the topology update names a node that isn't in the table";
        else
            seen[id] = true;
    }
    free(payload);
    if (why)
        return why;

    memset(seen, 0, sizeof(seen));
    for (n = 0;;) {
        const TcpNodeGidsPayload *adv;
        uint32_t id, num;

        if (!wr_until(fd, TCP_MSG_NODE_GIDS, 2000, &hdr, &payload, NULL))
            return "the evicted node wasn't advertised";
        if (!payload || hdr.msg_len < sizeof(*adv)) {
            free(payload);
            return "an advertisement is short";
        }
        adv = payload;
        id = ntohl(adv->node_id);
        num = ntohl(adv->num_gids);
        free(payload);
        if (id == evicted)
            return num != 0 ? "the evicted node still owns GIDs"
                   : n != TCP_MAX_MESH_NODES ? "a node in the table wasn't "
                                               "advertised before the evicted "
                                               "one"
                                             : NULL;
        if (id > fresh || seen[id])
            return "an advertisement names a node that isn't in the table";
        seen[id] = true;
        n++;
    }
}

/* EV_DEAD and EV_ENDED */
static const char *ev_dead(struct ev_fixture *fx, enum ev_scenario scenario)
{
    TcpBackendPrivate *priv = fx->priv;
    int errors = atomic_load(&g_errors);
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    why = ev_register(priv, &fx->fresh[0], UINT32_MAX, &id, &result);
    if (why)
        return why;
    if (result != 0 || id != fx->last + 1)
        return "the new node wasn't given the next id";
    why = ev_kept(priv, id, EV_OLDEST);
    if (why)
        return why;
    if (mesh_node_count(priv) != TCP_MAX_MESH_NODES)
        return "the table isn't full";
    if (ev_has_claims(priv, EV_OLDEST))
        return "the evicted node's claims were kept";
    if (ev_resolve(priv) != UINT32_MAX)
        return "the evicted node still owns its GID";
    why = ev_broadcasts(fx->fresh[0], id, EV_OLDEST);
    if (why)
        return why;
    if (atomic_load(&g_errors) != errors)
        return "an error was reported";
    if (scenario != EV_ENDED)
        return NULL;
    if (table_get(priv, EV_OLDEST))
        return "the evicted node's connection is still in the table";
    return wait_retired(fx->conn[EV_OLDEST])
               ? NULL
               : "the evicted node's connection wasn't closed";
}

/* EV_LIVE_CONN */
static const char *ev_live_conn(struct ev_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    why = ev_register(priv, &fx->fresh[0], UINT32_MAX, &id, &result);
    if (why)
        return why;
    if (result != 0 || id != fx->last + 1)
        return "the new node wasn't given the next id";
    why = ev_kept(priv, id, EV_NEXT);
    if (why)
        return why;
    if (table_get(priv, EV_OLDEST) != fx->conn[EV_OLDEST] ||
        conn_retired(fx->conn[EV_OLDEST]))
        return "the oldest node's connection was closed";
    return ev_resolve(priv) == EV_OLDEST ? NULL
                                         : "the oldest node lost its GID";
}

/* EV_ROOM and EV_AGAIN: nothing is evicted */
static const char *ev_none(struct ev_fixture *fx, enum ev_scenario scenario)
{
    TcpBackendPrivate *priv = fx->priv;
    uint32_t want = UINT32_MAX, id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    if (scenario == EV_AGAIN) {
        TcpConnection *old = tcp_connection_lookup(priv, 1);
        bool ended;

        /* Dead longest, so a new node would evict it */
        node_set(priv, 1, false, ernic_monotonic_ms() - UINT64_C(6000000));
        ended = old && shutdown(fx->w1, SHUT_RDWR) == 0 && wait_dead(old);
        tcp_connection_unref(old);
        if (!ended)
            return "worker 1's connection didn't end";
        want = 1;
    }
    why = ev_register(priv, &fx->fresh[0], want, &id, &result);
    if (why)
        return why;
    if (scenario == EV_ROOM) {
        if (result != 0 || id != fx->last + 1)
            return "the new node wasn't given the next id";
        why = ev_kept(priv, id, UINT32_MAX);
        if (!why && mesh_node_count(priv) != TCP_MAX_MESH_NODES)
            why = "the table isn't full";
        if (!why && !ev_has_claims(priv, EV_OLDEST))
            why = "the oldest node's claims were dropped";
        return why;
    }
    /* EV_AGAIN */
    if (result != 0 || id != 1)
        return "worker 1 wasn't given its id back";
    why = ev_kept(priv, fx->last, UINT32_MAX);
    if (!why && mesh_node_count(priv) != TCP_MAX_MESH_NODES)
        why = "the table isn't full";
    return why;
}

/*
 * EV_NONE and EV_CONNECTED: no node can be evicted, so a new node is refused,
 * and the table left as it was.  In EV_CONNECTED, the oldest node's
 * connection then ends, and the new node, asking again, takes its place.
 */
static const char *ev_full(struct ev_fixture *fx, enum ev_scenario scenario)
{
    TcpBackendPrivate *priv = fx->priv;
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    why = ev_register(priv, &fx->fresh[0], UINT32_MAX, &id, &result);
    if (why)
        return why;
    if (result != -ENOSPC || id != 0)
        return "refused with the wrong answer, or given an id";
    why = ev_kept(priv, fx->last, UINT32_MAX);
    if (why)
        return why;
    if (mesh_node_count(priv) != TCP_MAX_MESH_NODES)
        return "the table isn't full";
    for (uint32_t i = 2; i <= fx->last; i++)
        if (table_get(priv, i) != fx->conn[i] || conn_retired(fx->conn[i]))
            return "a node's connection was closed";
    if (!ev_has_claims(priv, EV_OLDEST))
        return "the oldest node's claims were dropped";
    if (pending_count(priv) != 1)
        return "the refused connection isn't waiting to be reaped";
    if (scenario == EV_NONE)
        return NULL;

    /* EV_CONNECTED: the oldest node can be evicted once its connection ends */
    close(fx->peer[EV_OLDEST]);
    fx->peer[EV_OLDEST] = -1;
    if (!wait_dead(fx->conn[EV_OLDEST]))
        return "the oldest node's connection didn't end";
    why = ev_register(priv, &fx->fresh[1], UINT32_MAX, &id, &result);
    if (why)
        return why;
    /* The refusal gave the id to nothing, so it is still the next one */
    if (result != 0 || id != fx->last + 1)
        return "the new node wasn't given the next id";
    why = ev_kept(priv, id, EV_OLDEST);
    if (why)
        return why;
    if (mesh_node_count(priv) != TCP_MAX_MESH_NODES)
        return "the table isn't full";
    return wait_retired(fx->conn[EV_OLDEST])
               ? NULL
               : "the evicted node's connection wasn't closed";
}

/*
 * EV_HEALTH: the items are as a pass's scan leaves them for two dead nodes,
 * one whose connection had ended and one that had stopped answering, and
 * the pass acts on them only after registrations have evicted both.
 */
static const char *ev_health(struct ev_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(TcpHealthCheckItem));
    TcpHealthCheckItem ended = {EV_OLDEST, NULL, true, true};
    TcpHealthCheckItem silent = {EV_NEXT, NULL, false, true};
    const TcpHealthCheckItem *after;
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    ended.conn = tcp_connection_ref(fx->conn[EV_OLDEST]);
    silent.conn = tcp_connection_ref(fx->conn[EV_NEXT]);
    g_array_append_val(items, ended);
    g_array_append_val(items, silent);

    why = ev_register(priv, &fx->fresh[0], UINT32_MAX, &id, &result);
    if (!why && (result != 0 || ev_in_table(priv, EV_OLDEST)))
        why = "the first registration didn't evict the oldest node";
    /* Read what it sent, so the next registration's broadcasts fit */
    if (!why && (!worker_settled(priv, fx->fresh[0], id) ||
                 !worker_settled(priv, fx->w1, 1)))
        why = "the first registration wasn't finished";
    if (!why)
        why = ev_register(priv, &fx->fresh[1], UINT32_MAX, &id, &result);
    if (!why && (result != 0 || ev_in_table(priv, EV_NEXT)))
        why = "the second registration didn't evict the next oldest node";

    if (!why) {
        tcp_health_check_retire(priv, items);
        after = &g_array_index(items, TcpHealthCheckItem, 0);
        if (after->conn)
            why = "the pass kept the ended connection";
        after = &g_array_index(items, TcpHealthCheckItem, 1);
        if (!why && after->conn != fx->conn[EV_NEXT])
            why = "the pass let go of the silent node's connection";
    }
    if (!why && (!wait_retired(fx->conn[EV_OLDEST]) ||
                 !wait_retired(fx->conn[EV_NEXT])))
        why = "an evicted node's connection wasn't closed";

    for (guint i = 0; i < items->len; i++)
        tcp_connection_unref(g_array_index(items, TcpHealthCheckItem, i).conn);
    g_array_free(items, TRUE);
    return why;
}

/*
 * EV_UNPUBLISHED: the node's connection is out of the table, as a health
 * check leaves it between taking it out and joining its thread.  The test
 * retires it, as the health check would, once it is done.
 */
static const char *ev_unpublished(struct ev_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;
    TcpConnection *conn = fx->conn[EV_OLDEST];
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const char *why;

    why = ev_register(priv, &fx->fresh[0], UINT32_MAX, &id, &result);
    if (!why && (result != 0 || ev_in_table(priv, EV_OLDEST)))
        why = "the oldest node wasn't evicted";
    /* Its thread answers the heartbeat once it has handled the GIDs */
    if (!why && (!send_gids(fx->peer[EV_OLDEST], EV_OLDEST,
                            (const uint8_t(*)[16]) & ev_gid) ||
                 !worker_settled(priv, fx->peer[EV_OLDEST], EV_OLDEST)))
        why = "the evicted node's advertisement wasn't handled";
    if (!why && ev_has_claims(priv, EV_OLDEST))
        why = "the advertisement put the evicted node's claims back";
    if (!why && ev_resolve(priv) != UINT32_MAX)
        why = "the evicted node owns its GID again";
    tcp_retire_conn(conn);
    return why;
}

static const char *ev_run(struct ev_fixture *fx, enum ev_scenario scenario)
{
    const char *why;

    if (!worker_settled(fx->priv, fx->w1, 1))
        return "worker 1's registration wasn't finished";
    why = ev_setup(fx, scenario);
    if (why)
        return why;
    switch (scenario) {
    case EV_DEAD:
    case EV_ENDED:
        return ev_dead(fx, scenario);
    case EV_LIVE_CONN:
        return ev_live_conn(fx);
    case EV_HEALTH:
        return ev_health(fx);
    case EV_UNPUBLISHED:
        return ev_unpublished(fx);
    case EV_NONE:
    case EV_CONNECTED:
        return ev_full(fx, scenario);
    case EV_ROOM:
    case EV_AGAIN:
    default:
        return ev_none(fx, scenario);
    }
}

static int run_ev_case(const struct ev_case *c)
{
    RdmaBackendDev dev;
    struct ev_fixture fx;
    char config[64];
    const char *why;
    uint16_t port = free_port();
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&dev, 0, sizeof(dev));
    memset(&fx, 0, sizeof(fx));
    fx.w1 = -1;
    for (uint32_t i = 0; i < TCP_MAX_MESH_NODES; i++)
        fx.peer[i] = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(fx.fresh); i++)
        fx.fresh[i] = -1;
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 ||
        tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0) {
        printf("FAIL evict %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    fx.priv = dev.backend_private;
    tcp_stop_health_check_thread(fx.priv);

    fx.w1 = worker_register(fx.priv);
    if (fx.w1 < 0 || !wait_for_registered(fx.priv, 1))
        why = "worker 1 did not register";
    else
        why = ev_run(&fx, c->scenario);

    rdma_backend_ops_tcp.fini(&dev);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s evict %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok)
        printf("     %s\n", why);
    fflush(stdout);

    for (uint32_t i = 0; i < TCP_MAX_MESH_NODES; i++) {
        tcp_connection_unref(fx.conn[i]);
        if (fx.peer[i] >= 0)
            close(fx.peer[i]);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(fx.fresh); i++)
        if (fx.fresh[i] >= 0)
            close(fx.fresh[i]);
    if (fx.w1 >= 0)
        close(fx.w1);
    return ok ? 0 : 1;
}

/* ---- A node's liveness ------------------------------------------------- */

/*
 * The manager keeps a node's liveness twice: in the topology (is_alive),
 * and in whether its GID claims take part in resolution.  They could
 * disagree.  A node declared dead that advertised before it answered again
 * owned its GIDs while the topology said it was dead, and a registration
 * that found a node with no live connection took its claims out of
 * resolution but left the topology saying it was alive.  Both are now
 * written together, by tcp_node_set_alive().
 *
 * Each row starts a manager with tcp_init(), stops its health-check thread,
 * as the sender rows do, so that only the row declares a node dead, and
 * registers two workers over socketpairs: nodes 1 and 2.
 */

enum lv_scenario {
    LV_DEAD_ADVERTISES, /* node 2, declared dead, advertises a GID */
    LV_GONE,            /* node 2 hangs up, and a third worker registers */
};

struct lv_case {
    const char *category;
    const char *description;
    const char *expected;
    enum lv_scenario scenario;
};

static const struct lv_case lv_cases[] = {
    {"negative", "node 2, declared dead, advertises a GID before it answers",
     "the GID resolves to nothing, and worker 1 is told node 2 owns nothing; "
     "once node 2 answers a heartbeat, it owns the GID, and worker 1 is told "
     "so",
     LV_DEAD_ADVERTISES},
    {"positive",
     "node 2 hangs up, and a worker registers before the health check has "
     "noticed",
     "node 2 is declared dead, its GID resolves to nothing, and worker 1 is "
     "told both",
     LV_GONE},
};

/* The one GID of these rows: ::ffff:192.168.251.11 */
static const uint8_t lv_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                   0, 0, 0xff, 0xff, 192, 168, 251, 11};

static uint32_t lv_resolve(TcpBackendPrivate *priv)
{
    union ibv_gid gid;

    memcpy(gid.raw, lv_gid, 16);
    return tcp_resolve_node_from_gid(priv, &gid);
}

/*
 * LV_DEAD_ADVERTISES.  The manager answers each advertisement by sending
 * every worker the node's GID set, and a heartbeat that revives a node by
 * sending the topology and the set again, each snapshotted once the change
 * is made, so what worker 1 is told comes after the change it reports.
 */
static const char *lv_dead_advertises(TcpBackendPrivate *priv, int w1, int w2)
{
    /* Finish the registrations, and discard what they sent */
    if (!worker_settled(priv, w1, 1) || !worker_settled(priv, w2, 2))
        return "a registration wasn't finished";
    count_type(w1, TCP_MSG_NODE_GIDS);

    node_set(priv, 2, false, ernic_monotonic_ms() - UINT64_C(1000000));
    if (!send_gids(w2, 2, (const uint8_t(*)[16]) & lv_gid))
        return "could not send the advertisement";
    if (gids_for(w1, 2) != 0)
        return "worker 1 was told a dead node owns the GID it advertised";
    if (lv_resolve(priv) != UINT32_MAX)
        return "a dead node owns the GID it advertised";

    if (!send_heartbeat(w2, 2))
        return "could not send the heartbeat";
    if (gids_for(w1, 2) != 1)
        return "worker 1 wasn't told node 2 owns its GID once it answered";
    return lv_resolve(priv) == 2 ? NULL
                                 : "node 2 didn't own its GID once it answered";
}

/*
 * LV_GONE.  Node 2 is alive when its connection ends, and nothing else
 * declares it dead: the health-check thread is stopped.  The registration
 * sends every worker the topology, then node 2's GID set.
 */
static const char *lv_gone(TcpBackendPrivate *priv, int w1, int w2, int *w3)
{
    TcpConnection *conn = tcp_connection_lookup(priv, 2);
    const char *why = NULL;
    uint64_t last;
    bool alive = true;

    if (!conn)
        return "worker 2 isn't node 2";
    /* Answered once the advertisement is handled */
    if (!worker_settled(priv, w1, 1) ||
        !send_gids(w2, 2, (const uint8_t(*)[16]) & lv_gid) ||
        !worker_settled(priv, w2, 2))
        why = "a registration or the advertisement wasn't finished";
    else if (lv_resolve(priv) != 2)
        why = "node 2's claim didn't take";
    count_type(w1, TCP_MSG_MESH_TOPOLOGY);

    if (!why && (shutdown(w2, SHUT_RDWR) < 0 || !wait_dead(conn)))
        why = "node 2's connection didn't end";
    if (!why && (!node_get(priv, 2, &alive, &last) || !alive))
        why = "node 2 was declared dead before the registration";
    if (!why && (*w3 = worker_register(priv)) < 0)
        why = "could not register worker 3";
    if (!why && !reported(w1, 2, false, 2000))
        why = "worker 1 wasn't told node 2 is dead";
    if (!why && gids_for(w1, 2) != 0)
        why = "worker 1 wasn't told node 2 owns nothing";
    if (!why && (!node_get(priv, 2, &alive, &last) || alive))
        why = "node 2 wasn't declared dead";
    if (!why && lv_resolve(priv) != UINT32_MAX)
        why = "node 2 still owns its GID";

    tcp_connection_unref(conn);
    return why;
}

static int run_lv_case(const struct lv_case *c)
{
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    char config[64];
    const char *why;
    uint16_t port = free_port();
    int w1 = -1, w2 = -1, w3 = -1;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);

    g_current_row = c->description;
    alarm(row_timeout_s());
    if (port == 0 ||
        tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0) {
        printf("FAIL liveness %s: %s; could not start a manager\n", c->category,
               c->description);
        alarm(0);
        return 1;
    }
    priv = dev.backend_private;
    tcp_stop_health_check_thread(priv);

    w1 = worker_register(priv);
    if (w1 < 0 || !wait_for_registered(priv, 1))
        why = "worker 1 did not register";
    else if ((w2 = worker_register(priv)) < 0 || !wait_for_registered(priv, 2))
        why = "worker 2 did not register";
    else if (c->scenario == LV_DEAD_ADVERTISES)
        why = lv_dead_advertises(priv, w1, w2);
    else
        why = lv_gone(priv, w1, w2, &w3);

    rdma_backend_ops_tcp.fini(&dev);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s liveness %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s\n", why);
    fflush(stdout);

    if (w1 >= 0)
        close(w1);
    if (w2 >= 0)
        close(w2);
    if (w3 >= 0)
        close(w3);
    return ok ? 0 : 1;
}

/* ---- Connection lifetime ---------------------------------------------- */

/*
 * A lookup used to return a bare pointer to a connection, and the health
 * check's reconnect freed the node's old connection by replacing it in the
 * table, so a thread that had looked it up went on to use freed memory.
 * Connections are reference counted now: a lookup returns a reference, the
 * table holds one, and whatever replaces a connection takes the old one out
 * of the table, retires it, and drops the table's, leaving it to the last
 * holder to free.
 *
 * The replacement rows have node 1 of an mgr_fixture, which the manager has
 * declared dead, register again, while the test holds a reference to its old
 * connection, or while another thread is blocked sending on it.  The
 * registration rows have a pending connection register, as a new node or
 * again under a node's id, while teardown is stopping its receive thread.
 */

enum lt_scenario {
    LT_HELD,   /* the test holds the old connection across the replacement */
    LT_SENDER, /* its send thread is blocked sending on it */
    LT_REGISTER_VS_STOP, /* a registration lands while the stop runs */
    LT_AGAIN_VS_STOP,    /* a node registering again does */
    LT_GIDS_BACK,     /* the node went silent; registering again gets it back */
    LT_EVICT_VS_STOP, /* a new node registers in a full table meanwhile */
};

struct lt_case {
    const char *category;
    const char *description;
    const char *expected;
    enum lt_scenario scenario;
};

static const struct lt_case lt_cases[] = {
    {"positive",
     "a registration replaces a connection the test holds a reference to",
     "the old connection is retired and out of the table, and stays readable "
     "until the reference is dropped",
     LT_HELD},
    {"negative",
     "a registration replaces a connection whose send thread is blocked "
     "sending to it",
     "the data path's send is queued at once, the registration stops and "
     "joins both of the old connection's threads, nothing hangs, and the new "
     "connection is in the table",
     LT_SENDER},
    {"corner",
     "a pending connection registers while teardown stops its receive thread",
     "it ends up in the connections table, not the pending list, and its "
     "thread is joined once",
     LT_REGISTER_VS_STOP},
    {"corner",
     "a node registers again while teardown stops the receive threads, its "
     "old connection's among them",
     "refused: the old connection, which the stop has claimed, stays in the "
     "table rather than being freed under it, and each thread is joined once",
     LT_AGAIN_VS_STOP},
    {"positive",
     "a node the manager had declared dead registers again under its id",
     "the node owns its GIDs again at once, and declaring it dead on a "
     "finding from before doesn't take them away",
     LT_GIDS_BACK},
    {"corner",
     "a new node registers in a full table while teardown stops the receive "
     "threads, the connection of the node dead longest, which has ended, "
     "among them",
     "nothing is evicted, so the new node is refused: that connection, "
     "which the stop has claimed, stays in the table rather than being freed "
     "under it, and each thread is joined once",
     LT_EVICT_VS_STOP},
};

/* The one GID of LT_GIDS_BACK: ::ffff:192.168.251.9, the form
 * adminq_parse_roce_hdr() builds for an IPv4 address. */
static const uint8_t lt_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                   0, 0, 0xff, 0xff, 192, 168, 251, 9};

static uint32_t lt_resolve(TcpBackendPrivate *priv)
{
    union ibv_gid gid;

    memcpy(gid.raw, lt_gid, 16);
    return tcp_resolve_node_from_gid(priv, &gid);
}

/*
 * Register node 1 of fx again, on a new connection, and check the node has
 * it, rather than old, in the table.  *fd gets the worker's end of it, for
 * the caller to close.
 */
static const char *lt_register_again(struct mgr_fixture *fx, TcpConnection *old,
                                     int *fd)
{
    uint32_t id = UINT32_MAX;
    int32_t result = 1;
    const TcpConnection *in_table;

    *fd = worker_attach(fx->priv, NULL);
    if (*fd < 0 || !send_register(*fd, "127.0.0.1", 1))
        return "could not register node 1 again";
    if (!register_answer(*fd, &id, &result) || result != 0 || id != 1)
        return "node 1 wasn't given its id back";
    in_table = table_get(fx->priv, 1);
    return in_table && in_table != old ? NULL
                                       : "the node has no new connection";
}

/*
 * LT_HELD, LT_SENDER and LT_GIDS_BACK.  The fixture's reference to the old
 * connection is dropped first, so that the table's and the test's are the
 * only ones, and a registration that freed the connection would leave the
 * test reading freed memory, which ASan reports.
 */
static const char *lt_replace_run(enum lt_scenario scenario)
{
    static const uint8_t hdr[16];
    struct mgr_fixture fx;
    RdmaBackendDev dev;
    TcpConnection *held = NULL;
    const char *why;
    int fd = -1;

    why = mgr_setup(&fx, 1, LONG_INTERVAL_S);

    if (!why) {
        held = tcp_connection_lookup(fx.priv, 1);
        tcp_connection_unref(fx.old[0]);
        fx.old[0] = NULL;
        if (!held)
            why = "node 1 has no connection";
    }
    if (!why && scenario == LT_GIDS_BACK) {
        /*
         * Node 1 claimed an address while alive, then went quiet for three
         * intervals and the manager declared it dead, which takes its claims
         * out of resolution.  It did not restart, so it registers again under
         * its id, and nothing makes it advertise again until then.
         */
        node_set(fx.priv, 1, true, ernic_monotonic_ms());
        tcp_gid_claims_set(fx.priv, 1, (const uint8_t(*)[16]) & lt_gid, 1);
        if (lt_resolve(fx.priv) != 1)
            why = "the claim did not take effect";
        node_set(fx.priv, 1, false, ernic_monotonic_ms() - UINT64_C(1000000));
        if (!why && lt_resolve(fx.priv) != UINT32_MAX)
            why = "a node declared dead still owned its GID";
    }
    if (!why && scenario == LT_SENDER) {
        memset(&dev, 0, sizeof(dev));
        dev.backend_type = RDMA_BACKEND_TYPE_TCP;
        dev.backend_private = fx.priv;
        /*
         * Node 1 has stopped reading, so the send thread waits for room,
         * holding the connection's lock, and nothing else ever has it.  The
         * data path only queued the message, and has it back at once.
         */
        if (!fill_conn(fx.priv, 1))
            why = "could not fill node 1's socket";
        else if (tcp_backend_send_ionic_v(&dev, 1, hdr, sizeof(hdr), NULL, 0, 0,
                                          false) != 0)
            why = "the data path's send wasn't queued";
        else if (!held_elsewhere(&held->lock))
            why = "the send thread never blocked on node 1";
    }
    if (!why)
        why = lt_register_again(&fx, held, &fd);
    if (!why && scenario == LT_GIDS_BACK && lt_resolve(fx.priv) != 1)
        why = "the node registered again did not get its GID back";
    /*
     * A registration that collected this node as unreachable before it
     * registered again must not act on that list now: declaring it dead here
     * would undo the revival, and the mesh would be told it is dead and owns
     * nothing until it next answered a heartbeat.
     */
    if (!why && scenario == LT_GIDS_BACK) {
        if (tcp_declare_dead_if_unreachable(fx.priv, 1))
            why = "a stale declaration fired against a node registered again";
        else if (lt_resolve(fx.priv) != 1)
            why = "a stale declaration undid the registration's revival";
    }
    if (!why && !conn_retired(held))
        why = "the old connection wasn't retired";
    /* LT_SENDER's connection has a send thread as well */
    if (!why && atomic_load(&g_joins) != (scenario == LT_SENDER ? 2 : 1))
        why = "the old connection's threads weren't each joined once";

    tcp_connection_unref(held);
    mgr_teardown(&fx);
    if (fd >= 0)
        close(fd);
    return why;
}

/* Wait up to 5 s for the manager to have read everything sent on conn. */
static bool lt_consumed(TcpConnection *conn)
{
    for (int i = 0; i < 500; i++) {
        int unread = -1, rc;

        qemu_mutex_lock(&conn->lock);
        rc = ioctl(conn->sockfd, FIONREAD, &unread);
        qemu_mutex_unlock(&conn->lock);
        if (rc == 0 && unread == 0)
            return true;
        sleep_ms(10);
    }
    return false;
}

/*
 * Whether node id's connection has a send thread, which a stop joins.  The
 * manager answers a heartbeat on it when another thread, such as the
 * broadcaster, is sending on the connection.
 */
static bool has_send_thread(TcpBackendPrivate *priv, uint32_t id)
{
    TcpConnection *conn = tcp_connection_lookup(priv, id);
    bool started = false;

    if (conn) {
        qemu_mutex_lock(&conn->sendq_lock);
        started = conn->send_thread_started;
        qemu_mutex_unlock(&conn->sendq_lock);
        tcp_connection_unref(conn);
    }
    return started;
}

static void *lt_stop(void *opaque)
{
    tcp_stop_recv_threads(opaque);
    return NULL;
}

/*
 * LT_REGISTER_VS_STOP and, if again, LT_AGAIN_VS_STOP.  The test holds
 * mesh_table_lock, which registration needs, while the receive thread reads
 * a REGISTER_NODE: once the message is read, the thread handles it whatever
 * its stop flag says.  The stop then runs, on another thread, and claims the
 * receive thread while the connection is still pending; only then is the lock
 * let go, and the registration is handled while the stop waits to join its
 * thread.  The health-check and accept threads are stopped first, and
 * stopping set, as teardown does.
 *
 * For LT_AGAIN_VS_STOP, worker 1 has registered, and been declared dead, and
 * the registration asks for its id.  The stop claims worker 1's connection
 * too, keeping only a pointer to it to join its thread with; a registration
 * that took it out of the table then would free it under the stop.
 *
 * For LT_EVICT_VS_STOP, worker 1 has registered, and the table is filled up
 * as for the eviction rows' EV_ENDED: node EV_OLDEST, dead longest, has a
 * connection that has ended, whose only reference is the table's.  The stop
 * claims it too, and an eviction that took it out of the table and retired
 * it would free it under the stop.
 */
static const char *lt_register_vs_stop(enum lt_scenario scenario)
{
    bool again = scenario == LT_AGAIN_VS_STOP;
    bool evict = scenario == LT_EVICT_VS_STOP;
    const TcpConnection *ended = NULL;
    struct ev_fixture fx;
    RdmaBackendDev dev;
    TcpBackendPrivate *priv;
    TcpConnection *conn = NULL, *in_table;
    const TcpConnection *old = NULL;
    char config[64];
    const char *why = NULL;
    pthread_t stopper;
    uint16_t port = free_port();
    guint pending;
    int fd = -1, w1 = -1, joins;

    memset(&dev, 0, sizeof(dev));
    snprintf(config, sizeof(config), "tcp:manager:listen:%u", (unsigned)port);
    if (port == 0 || tcp_init_with_interval(&dev, config, LONG_INTERVAL_S) != 0)
        return "could not start a manager";
    priv = dev.backend_private;
    tcp_stop_health_check_thread(priv);
    tcp_stop_accept_thread(priv);

    if (again || evict) {
        w1 = worker_register(priv);
        if (w1 < 0 || !wait_for_registered(priv, 1) ||
            !worker_settled(priv, w1, 1)) {
            why = "worker 1 did not register";
            goto out;
        }
    }
    if (again) {
        old = table_get(priv, 1);
        node_set(priv, 1, false, ernic_monotonic_ms() - UINT64_C(1000000));
    }
    if (evict) {
        memset(&fx, 0, sizeof(fx));
        fx.priv = priv;
        for (uint32_t i = 0; i < TCP_MAX_MESH_NODES; i++)
            fx.peer[i] = -1;
        why = ev_setup(&fx, EV_ENDED);
        /* Leaves the table's reference the only one */
        ended = fx.conn[EV_OLDEST];
        tcp_connection_unref(fx.conn[EV_OLDEST]);
        if (why)
            goto out;
    }
    fd = worker_attach(priv, &conn);
    if (fd < 0) {
        why = "could not attach the worker";
        goto out;
    }
    /*
     * Only the stop's joins of receive threads are counted: worker 1's
     * heartbeat may have been answered on a send thread, which it joins too
     */
    joins = atomic_load(&g_joins) +
            ((again || evict) && has_send_thread(priv, 1) ? 1 : 0);
    atomic_store(&priv->stopping, true);
    qemu_mutex_lock(&priv->mesh_table_lock);
    if (!send_register(fd, "127.0.0.1", again ? 1 : UINT32_MAX) ||
        !lt_consumed(conn)) {
        qemu_mutex_unlock(&priv->mesh_table_lock);
        why = "the registration wasn't read";
        goto out;
    }
    if (pthread_create(&stopper, NULL, lt_stop, priv) != 0) {
        qemu_mutex_unlock(&priv->mesh_table_lock);
        why = "could not start the stop";
        goto out;
    }
    /* The stop claims every thread in one pass under conn_table_lock */
    for (int i = 0; i < 500 && atomic_load(&conn->recv_thread_running); i++)
        sleep_ms(10);
    if (atomic_load(&conn->recv_thread_running))
        why = "the stop never claimed the receive thread";
    qemu_mutex_unlock(&priv->mesh_table_lock);
    pthread_join(stopper, NULL);

    /*
     * The stop shut the socket down, so the refusal never reaches the
     * worker; the connection left pending, and the table, say it was refused.
     */
    if (evict) {
        if (!why && table_get(priv, EV_OLDEST) != ended)
            why = "the claimed connection was taken out of the table";
        else if (!why && !ev_in_table(priv, EV_OLDEST))
            why = "a node was evicted during teardown";
        else if (!why && (table_get(priv, TCP_MAX_MESH_NODES) ||
                          mesh_node_count(priv) != TCP_MAX_MESH_NODES))
            why = "the new node was added to a full table";
        else if (!why && pending_count(priv) != 1)
            why = "the refused connection isn't on the pending list";
        else if (!why && atomic_load(&g_joins) - joins != 3)
            why = "a receive thread wasn't joined exactly once";
        goto out;
    }

    qemu_mutex_lock(&priv->conn_table_lock);
    in_table = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(1));
    pending = priv->pending_conns->len;
    qemu_mutex_unlock(&priv->conn_table_lock);
    if (!why && in_table != (again ? old : conn))
        why = again ? "the claimed connection was replaced"
                    : "the connection isn't in the connections table";
    else if (!why && pending != (again ? 1u : 0u))
        why = again ? "the refused connection isn't on the pending list"
                    : "the connection is still on the pending list";
    else if (!why && atomic_load(&g_joins) - joins != (again ? 2 : 1))
        why = "a receive thread wasn't joined exactly once";

out:
    /* Joins nothing more; ASan reports it if the connection is freed twice */
    rdma_backend_ops_tcp.fini(&dev);
    if (fd >= 0)
        close(fd);
    if (w1 >= 0)
        close(w1);
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
    alarm(row_timeout_s());
    if (c->scenario == LT_REGISTER_VS_STOP || c->scenario == LT_AGAIN_VS_STOP ||
        c->scenario == LT_EVICT_VS_STOP)
        why = lt_register_vs_stop(c->scenario);
    else
        why = lt_replace_run(c->scenario);
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

/* ---- Publishing a worker's connection to a peer ------------------------ */

/*
 * A worker publishes a connection it made to a peer only after sending the
 * handshake, and by then the table can have a connection to the peer: the
 * peer's own, which the accept thread published, or one that has died.  The
 * worker used to close its new connection whenever there was one, so a dead
 * one was kept until shutdown.  tcp_worker_publish_peer() now keeps only a
 * live one, and replaces a dead one.  It also closes the new connection if
 * the latest topology update no longer names the peer, which it checks under
 * the locks it publishes under.
 *
 * Each row calls it on a worker, set up as for the topology rows, whose
 * peer-connect thread isn't running, so no pass changes the table meanwhile.
 * The new connection is a socketpair, as is the one already in the table.
 */

enum pp_scenario {
    PP_LIVE,    /* the table has a live connection to the peer */
    PP_DEAD,    /* the table has a dead one */
    PP_DROPPED, /* the latest update doesn't name the peer */
};

struct pp_case {
    const char *category;
    const char *description;
    const char *expected;
    enum pp_scenario scenario;
};

static const struct pp_case pp_cases[] = {
    {"negative", "the table has a live connection to the peer",
     "kept; the new connection is closed, with no receive thread started",
     PP_LIVE},
    {"positive", "the table has a dead connection to the peer",
     "the data path sends through the manager meanwhile; replaced by the new "
     "connection, which gets a receive thread, and the dead one is closed and "
     "its thread joined once",
     PP_DEAD},
    {"negative", "the latest update no longer names the peer",
     "the new connection is closed, not published", PP_DROPPED},
};

/*
 * Publish a new connection to TP_PEER, at *ours on the peer's side, once the
 * row has set the table up; *theirs gets the connection already there, with
 * a reference.
 */
static const char *pp_run(struct tp_fixture *fx, struct tp_net *net,
                          enum pp_scenario scenario, TcpConnection **theirs,
                          int *ours)
{
    TcpPeerTarget target = tp_target(TP_PEER, net->port[0]);
    TcpConnection *conn;
    int fd = -1, creates;
    bool published;

    if (scenario != PP_DROPPED) {
        tp_set_target(fx, &target);
        conn = tp_publish(fx, net, TP_PEER, 0, true, &fd);
        if (!conn)
            return "could not set up the connection in the table";
        *theirs = tcp_connection_ref(conn);
    }
    if (scenario == PP_DEAD) {
        TcpConnection *route;

        /* Its peer hangs up; fd is closed at the end of the row */
        if (shutdown(fd, SHUT_RDWR) < 0 || !wait_dead(*theirs))
            return "the connection in the table didn't die";
        /* The manager's relay, the one other way to the peer */
        route = tcp_route_to_node(fx->priv, TP_PEER);
        tcp_connection_unref(route);
        if (route != atomic_load(&fx->priv->manager_conn))
            return "the data path still picks the dead connection";
    }

    conn = recv_conn_new(fx->priv, TP_PEER, false, ours);
    if (!conn)
        return "could not set up the new connection";
    creates = atomic_load(&g_creates);
    published = tcp_worker_publish_peer(fx->priv, &target, conn);

    if (scenario == PP_DEAD) {
        if (!published || table_get(fx->priv, TP_PEER) != conn)
            return "the new connection wasn't published";
        if (atomic_load(&g_creates) != creates + 1)
            return "the new connection got no receive thread";
        return conn_retired(*theirs) ? NULL
                                     : "the dead connection wasn't closed";
    }
    if (published)
        return "the new connection was published";
    if (!closed_by_peer(*ours))
        return "the new connection wasn't closed";
    if (atomic_load(&g_creates) != creates)
        return "a receive thread was started for the new connection";
    return table_get(fx->priv, TP_PEER) == *theirs
               ? NULL
               : "the connection in the table was changed";
}

static int run_pp_case(const struct pp_case *c)
{
    struct tp_fixture fx;
    struct tp_net net;
    TcpConnection *theirs = NULL;
    const char *why;
    int ours = -1;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = c->description;
    alarm(row_timeout_s());

    why = tp_setup(&fx, LONG_INTERVAL_S, false);
    /* For a port to name, and the peer's ends to close */
    if (!why)
        why = tp_listen(&net, TP_CONNECTS);
    else
        tp_listen(&net, TP_CONNECTS);
    if (!why)
        why = pp_run(&fx, &net, c->scenario, &theirs, &ours);

    tp_teardown(&fx);
    alarm(0);
    tcp_connection_unref(theirs);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s publish %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited\n", why,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));
    fflush(stdout);

    if (ours >= 0)
        close(ours);
    tp_close(&net);
    return ok ? 0 : 1;
}

/* ---- Dead connections between workers ---------------------------------- */

/*
 * A worker kept its connection to another worker after that worker hung up,
 * with the socket in CLOSE_WAIT, until it shut down.  Its receive thread
 * returned, and nothing else took any notice: the data path went on sending
 * into the socket, the peer-connect thread skipped the peer as connected,
 * and the accept thread refused the peer's next connection as a duplicate,
 * so the two never connected again.  A worker also kept its connection to a
 * peer the manager reported dead, so one that died without hanging up looked
 * connected until a send failed.
 *
 * A receive thread now marks its connection dead as it returns, and the data
 * path passes it over.  Each pass of the peer-connect thread closes the dead
 * connections, and the ones to peers the latest topology update no longer
 * names, then connects to the peers it does name.  The accept thread replaces
 * a dead connection.
 *
 * Each row runs tcp_init() for two workers, nodes DC_A and DC_B, against a
 * manager the test plays over loopback, as the worker registration rows do.
 * DC_A connects to DC_B, which has the higher id.
 */

#define DC_A 3
#define DC_B 8 /* higher than HS_NODE, the lower peer of DC_ACCEPTED */

enum dc_scenario {
    DC_DEAD_THEN_ALIVE, /* the manager reports DC_B dead, then alive */
    DC_HANGS_UP,        /* DC_B drops its connection to DC_A */
    DC_ACCEPTED,        /* a peer that connected to DC_B hangs up */
};

struct dc_case {
    const char *category;
    const char *description;
    const char *expected;
    enum dc_scenario scenario;
    uint32_t interval; /* health_check_interval_sec */
};

static const struct dc_case dc_cases[] = {
    {"positive", "the manager reports the worker's peer dead, then alive",
     "the worker closes its connection, which ends the peer's, and connects "
     "again, and the peer replaces its dead connection; each old one is "
     "closed and its thread joined once",
     DC_DEAD_THEN_ALIVE, LONG_INTERVAL_S},
    {"negative",
     "the peer drops its connection to the worker, on a 1 s interval",
     "the worker closes its end and connects again, with no update",
     DC_HANGS_UP, 1},
    {"negative",
     "a peer that connected to the worker hangs up, on a 1 s interval",
     "the worker closes the connection, rather than keeping it in CLOSE_WAIT",
     DC_ACCEPTED, 1},
};

/* A worker, its connection to the manager, and the port it listens on */
struct dc_worker {
    struct wr_init in;
    int mgr; /* the manager's end */
    uint16_t port;
};

static void dc_init(struct dc_worker *w, uint32_t interval)
{
    memset(w, 0, sizeof(*w));
    w->in.interval = interval;
    w->in.ret = -1;
    w->mgr = -1;
}

/*
 * Start w and register it, as node id, with the manager listening on lfd at
 * port; returns NULL, or what failed.
 */
static const char *dc_start(struct dc_worker *w, int lfd, uint16_t port,
                            uint32_t id)
{
    const char *why;

    snprintf(w->in.config, sizeof(w->in.config), "tcp:worker:127.0.0.1:%u",
             (unsigned)port);
    if (pthread_create(&w->in.tid, NULL, wr_init_run, &w->in) != 0)
        return "could not start tcp_init()";
    w->in.running = true;
    why = wr_registered(lfd, &w->mgr, &w->port);
    if (why)
        return why;
    if (!wr_assign(w->mgr, id))
        return "could not answer the registration";
    wr_init_join(&w->in);
    return w->in.ret == 0 ? NULL : "tcp_init() failed";
}

/* Stop w, if it started. */
static void dc_stop(struct dc_worker *w)
{
    /* Failed before the answer: refuse, so tcp_init() returns */
    if (w->in.running && w->mgr >= 0)
        send_register_resp(w->mgr, 0, -EPROTO,
                           (uint32_t)sizeof(TcpRegisterRespPayload));
    wr_init_join(&w->in);
    if (w->in.ret == 0)
        rdma_backend_ops_tcp.fini(&w->in.dev);
    if (w->mgr >= 0)
        close(w->mgr);
}

static TcpBackendPrivate *dc_priv(struct dc_worker *w)
{
    return w->in.dev.backend_private;
}

/* Tell a, as the manager, that b is node DC_B, alive or dead. */
static bool dc_report(struct dc_worker *a, struct dc_worker *b, bool alive)
{
    TcpMeshTopologyPayload topo;

    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, DC_B, b->port, alive);
    return send_topology(a->mgr, &topo);
}

/*
 * The connections a row looks at, each with a reference, dropped at the end
 * of the row: ab is DC_A's to DC_B, and ba DC_B's to DC_A; ab2 and ba2 are
 * the ones that replace them.  DC_ACCEPTED uses ba for its peer's.
 */
struct dc_conns {
    TcpConnection *ab, *ba, *ab2, *ba2;
};

/*
 * Have a connect to b, and wait for both to publish the connection.  Returns
 * NULL, or what failed.
 */
static const char *dc_connect(struct dc_worker *a, struct dc_worker *b,
                              struct dc_conns *held)
{
    if (!dc_report(a, b, true))
        return "could not report the peer alive";
    held->ab = wait_live(dc_priv(a), DC_B, NULL);
    if (!held->ab)
        return "the worker didn't connect to its peer";
    held->ba = wait_live(dc_priv(b), DC_A, NULL);
    return held->ba ? NULL : "the peer didn't publish the worker's connection";
}

/*
 * DC_DEAD_THEN_ALIVE.  DC_B's peer-connect thread is stopped first, so its
 * dead connection is still in its table when DC_A connects again, and the
 * accept thread has to replace it.  Its first pass runs whenever the thread
 * is scheduled, whatever the interval, and could otherwise come after the
 * connection died and close it.  DC_B connects to nobody in this row.
 */
static const char *dc_dead_then_alive(struct dc_worker *a, struct dc_worker *b,
                                      struct dc_conns *held)
{
    const char *why;

    tcp_stop_peer_thread(dc_priv(b));
    why = dc_connect(a, b, held);
    if (why)
        return why;
    if (!dc_report(a, b, false))
        return "could not report the peer dead";
    if (!wait_retired(held->ab) || table_get(dc_priv(a), DC_B))
        return "the worker kept its connection to a peer reported dead";
    if (!wait_dead(held->ba))
        return "the peer's connection didn't die when the worker closed its "
               "own";
    if (table_get(dc_priv(b), DC_A) != held->ba)
        return "the peer's dead connection left its table early";

    if (!dc_report(a, b, true))
        return "could not report the peer alive again";
    held->ab2 = wait_live(dc_priv(a), DC_B, held->ab);
    if (!held->ab2)
        return "the worker didn't connect again";
    held->ba2 = wait_live(dc_priv(b), DC_A, held->ba);
    if (!held->ba2)
        return "the peer didn't replace its dead connection";
    return wait_retired(held->ba) ? NULL
                                  : "the peer's dead connection wasn't closed";
}

/*
 * DC_HANGS_UP: DC_B takes its connection out of its table and retires it,
 * as it would one it was done with.  DC_A's next pass, within an interval,
 * finds its end dead and connects again.
 */
static const char *dc_hangs_up(struct dc_worker *a, struct dc_worker *b,
                               struct dc_conns *held)
{
    const char *why = dc_connect(a, b, held);

    if (why)
        return why;
    if (!retire_node_conn(dc_priv(b), DC_A))
        return "the peer's connection wasn't in its table";
    held->ab2 = wait_live(dc_priv(a), DC_B, held->ab);
    if (!held->ab2)
        return "the worker didn't connect again";
    if (!wait_retired(held->ab))
        return "the worker's dead connection wasn't closed";
    held->ba2 = wait_live(dc_priv(b), DC_A, held->ba);
    return held->ba2 ? NULL : "the peer didn't publish the new connection";
}

/*
 * DC_ACCEPTED: a peer claiming HS_NODE, lower than DC_B, connects to DC_B
 * and hangs up.  DC_B's next pass, within an interval, closes the
 * connection.
 */
static const char *dc_accepted(struct dc_worker *b, struct dc_conns *held)
{
    int peer = wr_peer(b->port);
    uint32_t answered;

    if (peer < 0)
        return "the peer could not connect";
    answered = wr_node_in(peer, TCP_MSG_HANDSHAKE_RESP, NULL);
    /* Published before the answer */
    held->ba = tcp_connection_lookup(dc_priv(b), HS_NODE);
    close(peer);
    if (answered != DC_B || !held->ba)
        return "the worker didn't take the peer's connection";
    if (!wait_retired(held->ba))
        return "the connection of the peer that hung up wasn't closed";
    return table_get(dc_priv(b), HS_NODE)
               ? "the connection of the peer that hung up is still in the "
                 "table"
               : NULL;
}

static const char *dc_run(enum dc_scenario scenario, struct dc_worker *a,
                          struct dc_worker *b, struct dc_conns *held)
{
    switch (scenario) {
    case DC_DEAD_THEN_ALIVE:
        return dc_dead_then_alive(a, b, held);
    case DC_HANGS_UP:
        return dc_hangs_up(a, b, held);
    case DC_ACCEPTED:
        return dc_accepted(b, held);
    default:
        return "unknown scenario";
    }
}

/*
 * Run one row: start the workers, play the manager and the peers for the
 * row, then stop the workers and check that every thread they started was
 * joined exactly once.
 */
static int run_dc_case(const struct dc_case *c)
{
    struct dc_worker a, b;
    struct dc_conns held = {NULL, NULL, NULL, NULL};
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    int lfd;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    dc_init(&a, c->interval);
    dc_init(&b, c->interval);

    g_current_row = c->description;
    alarm(row_timeout_s());
    lfd = listen_loopback();
    if (lfd < 0 || getsockname(lfd, (struct sockaddr *)&addr, &len) < 0)
        why = "could not open the manager's listener";
    if (!why)
        why = dc_start(&a, lfd, ntohs(addr.sin_port), DC_A);
    if (!why)
        why = dc_start(&b, lfd, ntohs(addr.sin_port), DC_B);
    if (!why)
        why = dc_run(c->scenario, &a, &b, &held);

    dc_stop(&a);
    dc_stop(&b);
    alarm(0);
    tcp_connection_unref(held.ab);
    tcp_connection_unref(held.ba);
    tcp_connection_unref(held.ab2);
    tcp_connection_unref(held.ba2);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s dead connection %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited\n", why,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));
    fflush(stdout);

    if (lfd >= 0)
        close(lfd);
    return ok ? 0 : 1;
}

/* ---- A worker's connection to the manager ------------------------------ */

/*
 * When a worker's connection to the manager dropped, the manager connected to
 * the worker instead, but the worker takes topology updates and GID tables
 * only on the connection it opened itself, so it heard nothing more from the
 * mesh.  A connection that died without a hang-up wasn't noticed at all,
 * since a worker timed nothing.  The worker now owns the link: its
 * peer-connect thread connects and registers again, asking for its own id,
 * when the connection has ended, or when nothing has come on it for three
 * intervals.  A check that finds nothing has come for an interval first
 * sends the manager a heartbeat, which the manager answers, so a manager
 * whose own interval is longer isn't taken for silent.  The checks come
 * between connects to peers, which can take an interval each, and don't wait
 * on the connection's lock, which a send to a manager that has stopped
 * reading holds.
 *
 * A worker cut off while the manager evicts a node misses the one message
 * saying the node owns nothing, and the GID tables after it don't mention
 * the node.  The worker drops the claims of nodes a topology update leaves
 * out, which the evicted row checks.
 *
 * Each row runs tcp_init() for a worker, node WL_ID, on a 1 s interval,
 * against a manager the test plays over loopback, as the dead connection rows
 * do.
 */

#define WL_ID 4
/* A peer with a higher id, which the worker connects to */
#define WL_HIGHER 6
/* A node whose GIDs the manager tells the worker */
#define WL_OTHER 9
/*
 * A node WL_EVICTED evicts.  Lower than WL_ID, so the worker waits for it to
 * connect rather than connecting to it.
 */
#define WL_GONE_ID 2
/* Peers with higher ids that don't answer, for WL_BUSY */
#define WL_SLOW       3
#define WL_SLOW_FIRST 10
/* What WL_STUCK sends: more than a socket and its peer's hold */
#define WL_STUCK_LEN (TCP_MAX_PAYLOAD_LEN - 16u)
/* And the QP of a request it queues behind that, then cancels */
#define WL_CANCEL_QP 7u

enum wl_scenario {
    WL_HANGS_UP, /* the manager closes the connection */
    WL_SILENT,   /* the manager stops sending, without closing it */
    WL_ANSWERED, /* it sends nothing, but answers the worker's heartbeats */
    WL_BUSY,     /* the same, while the worker connects to slow peers */
    WL_STUCK,    /* it stops reading, with a send to it under way */
    WL_REFUSED,  /* it refuses the worker's first registration again */
    WL_OTHER_ID, /* it registers the worker again as another node */
    WL_UNKNOWN,  /* it refuses the worker, having no node with its id */
    WL_GONE,     /* nothing listens on its port for a while */
    WL_STOP,     /* the worker is stopped while it waits for the answer */
    WL_EVICTED,  /* a node is evicted while the worker is cut off */
};

struct wl_case {
    const char *category;
    const char *description;
    const char *expected;
    enum wl_scenario scenario;
};

static const struct wl_case wl_cases[] = {
    {"positive", "the manager closes the worker's connection",
     "the worker connects again, registers asking for its own id, advertises "
     "its GIDs, and takes topology updates and GID tables on the new "
     "connection; the old one is closed",
     WL_HANGS_UP},
    {"negative", "the manager stops sending, without closing the connection",
     "the worker sends it a heartbeat, then, three intervals on, closes the "
     "connection and registers again",
     WL_SILENT},
    {"positive", "the manager sends nothing, but answers heartbeats",
     "the worker sends heartbeats and keeps its connection", WL_ANSWERED},
    {"boundary",
     "the manager sends nothing, but answers heartbeats, while the worker "
     "connects to three peers that don't answer, on a 1 s interval",
     "the worker sends heartbeats between the connects, which take three "
     "intervals, and keeps its connection",
     WL_BUSY},
    {"negative",
     "the manager stops reading and sending, while the send thread is "
     "blocked sending the data path's message to it",
     "the data path's send is queued at once, and a QP's request queued "
     "behind it can be cancelled; the worker doesn't wait for the "
     "connection's lock: three intervals on, it closes the connection, which "
     "stops the send thread, and registers again",
     WL_STUCK},
    {"negative", "the manager refuses the worker's registration once",
     "the worker keeps its id, and registers again on its next pass",
     WL_REFUSED},
    {"negative", "the manager registers the worker again as another node",
     "the worker refuses: it keeps its id, closes the connection, and doesn't "
     "try again, since only a manager that predates registering again does "
     "that",
     WL_OTHER_ID},
    {"negative",
     "the manager refuses the worker's registering again with -ENOENT, "
     "having evicted the node or restarted",
     "the worker keeps its id, closes the connection, and doesn't try again, "
     "since the manager never gives the id out again",
     WL_UNKNOWN},
    {"boundary", "nothing listens on the manager's port for a while",
     "the worker tries every pass, and registers once the manager is back",
     WL_GONE},
    {"corner",
     "a stop while the worker waits for the answer to registering "
     "again",
     "returns at once, without waiting out the registration timeout; the "
     "worker has no connection to the manager meanwhile",
     WL_STOP},
    {"negative",
     "the manager evicts a node while the worker is cut off, and the worker "
     "registers again",
     "the worker drops the evicted node's GIDs, which no GID table mentions "
     "again, once a topology update leaves the node out, and keeps the "
     "others'",
     WL_EVICTED},
};

/* The one GID WL_OTHER owns, and WL_GONE_ID in WL_EVICTED:
 * ::ffff:192.168.251.11 */
static const uint8_t wl_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                   0, 0, 0xff, 0xff, 192, 168, 251, 11};

/* The one GID the manager, node 0, owns in WL_EVICTED: ::ffff:192.168.251.12 */
static const uint8_t wl_mgr_gid[16] = {0, 0, 0,    0,    0,   0,   0,   0,
                                       0, 0, 0xff, 0xff, 192, 168, 251, 12};

/* The descriptors a row opens on the manager's side, closed at its end */
#define WL_MAX_FDS 4

struct wl_fds {
    int fd[WL_MAX_FDS];
    uint32_t n;
};

static int wl_keep(struct wl_fds *k, int fd)
{
    if (fd >= 0 && k->n < WL_MAX_FDS)
        k->fd[k->n++] = fd;
    return fd;
}

/* A header-only message of type from node src */
static bool send_header(int fd, TcpMsgType type, uint32_t src)
{
    TcpMsgHeader hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.src_node_id = htonl(src);
    return send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) == (ssize_t)sizeof(hdr);
}

/* How many times the worker has set out to register again */
static uint64_t wl_attempts(struct dc_worker *w)
{
    return atomic_load(&dc_priv(w)->tcp_stats.manager_reconnects);
}

/*
 * Accept the worker's next connection to the manager on lfd, and read its
 * registration, which has to ask for WL_ID from the port the worker
 * registered before.  *mgr gets the manager's end.  Returns NULL, or what
 * failed.
 */
static const char *wl_registers(struct dc_worker *w, int lfd, struct wl_fds *k,
                                int *mgr)
{
    const TcpRegisterNodePayload *reg;
    TcpMsgHeader hdr;
    void *payload;
    const char *why = NULL;

    *mgr = wl_keep(k, accept_within(lfd));
    if (*mgr < 0)
        return "the worker didn't connect to the manager again";
    if (!wr_recv(*mgr, &hdr, &payload, 5000))
        return "the worker sent no registration";
    reg = payload;
    if (hdr.msg_type != TCP_MSG_REGISTER_NODE || !reg ||
        hdr.msg_len < sizeof(*reg))
        why = "the worker's first message wasn't a registration";
    else if (ntohl(reg->requested_node_id) != WL_ID)
        why = "the worker didn't ask for its own id";
    else if (ntohs(reg->port) != w->port)
        why = "the worker registered another port";
    free(payload);
    return why;
}

/*
 * Answer the registration on mgr with WL_ID, and check that the worker takes
 * the mesh's word on that connection now: it advertises its GIDs there, and
 * acts on a GID table and a topology update that come on it.
 */
static const char *wl_accepted(struct dc_worker *w, int mgr, int high)
{
    TcpBackendPrivate *priv = dc_priv(w);
    TcpMeshTopologyPayload topo;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    union ibv_gid gid;
    int peer;

    if (!wr_assign(mgr, WL_ID))
        return "could not answer the registration";
    if (wr_node_in(mgr, TCP_MSG_NODE_GIDS, NULL) != WL_ID)
        return "the worker didn't advertise its GIDs as WL_ID";
    if (tcp_local_node(priv) != WL_ID)
        return "the worker's id changed";

    memcpy(gid.raw, wl_gid, 16);
    if (!send_gids(mgr, WL_OTHER, &wl_gid) || !wr_settled(mgr, NULL))
        return "the worker stopped answering";
    if (tcp_resolve_node_from_gid(priv, &gid) != WL_OTHER)
        return "the worker ignored a GID table on the new connection";

    if (getsockname(high, (struct sockaddr *)&addr, &len) < 0)
        return "could not find the peer's port";
    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, WL_HIGHER, ntohs(addr.sin_port), true);
    if (!send_topology(mgr, &topo))
        return "could not send the topology update";
    peer = accept_within(high);
    if (peer < 0)
        return "the worker ignored a topology update on the new connection";
    close(peer);
    return NULL;
}

/* The node the worker resolves gid to, which is 16 bytes */
static uint32_t wl_resolve(struct dc_worker *w, const uint8_t *gid)
{
    union ibv_gid raw;

    memcpy(raw.raw, gid, 16);
    return tcp_resolve_node_from_gid(dc_priv(w), &raw);
}

/*
 * WL_EVICTED.  The worker learns that WL_GONE_ID owns wl_gid, and the
 * manager wl_mgr_gid, then loses its connection to the manager.  Meanwhile
 * the manager declares WL_GONE_ID dead and evicts it, telling only the
 * workers it is connected to.  The worker registers again, and the manager
 * sends what follows a registration: a topology update, then a GID table,
 * neither of which has WL_GONE_ID.
 */
static const char *wl_evicted(struct dc_worker *w, int lfd, struct wl_fds *k)
{
    TcpMeshTopologyPayload topo;
    const char *why;
    int mgr;

    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, 0, 1, true);
    tp_entry(&topo, WL_ID, w->port, true);
    tp_entry(&topo, WL_GONE_ID, 1, true);
    if (!send_topology(w->mgr, &topo) ||
        !send_gids(w->mgr, WL_GONE_ID, &wl_gid) ||
        !send_gids(w->mgr, 0, &wl_mgr_gid) || !wr_settled(w->mgr, NULL))
        return "the worker stopped answering";
    if (wl_resolve(w, wl_gid) != WL_GONE_ID || wl_resolve(w, wl_mgr_gid) != 0)
        return "the worker ignored the GID table";

    close(w->mgr);
    w->mgr = -1;
    why = wl_registers(w, lfd, k, &mgr);
    if (why)
        return why;
    if (!wr_assign(mgr, WL_ID))
        return "could not answer the registration";
    if (wr_node_in(mgr, TCP_MSG_NODE_GIDS, NULL) != WL_ID)
        return "the worker didn't advertise its GIDs as WL_ID";

    memset(&topo, 0, sizeof(topo));
    tp_entry(&topo, 0, 1, true);
    tp_entry(&topo, WL_ID, w->port, true);
    if (!send_topology(mgr, &topo) || !send_gids(mgr, 0, &wl_mgr_gid) ||
        !wr_settled(mgr, NULL))
        return "the worker stopped answering";
    if (wl_resolve(w, wl_gid) != UINT32_MAX)
        return "the worker kept the evicted node's GID";
    return wl_resolve(w, wl_mgr_gid) == 0
               ? NULL
               : "the worker dropped the GID of a node still in the mesh";
}

/* WL_HANGS_UP and WL_SILENT; old is the worker's connection before */
static const char *wl_again(struct dc_worker *w, int lfd, int high,
                            struct wl_fds *k, TcpConnection *old, bool silent)
{
    const char *why;
    int mgr;

    if (silent) {
        if (!wait_for_type(w->mgr, TCP_MSG_HEARTBEAT, 3000))
            return "the worker sent the quiet manager no heartbeat";
    } else {
        close(w->mgr);
        w->mgr = -1;
    }
    why = wl_registers(w, lfd, k, &mgr);
    if (why)
        return why;
    if (silent && !ends(w->mgr))
        return "the worker kept its connection to the silent manager";
    if (!conn_retired(old))
        return "the old connection wasn't closed";
    why = wl_accepted(w, mgr, high);
    if (why)
        return why;
    return wl_attempts(w) == 1 ? NULL
                               : "the worker connected again more than once";
}

/*
 * WL_BUSY: tell the worker of WL_SLOW peers that don't answer, so that each
 * of its passes spends an interval connecting to each.  *slow gets their
 * listeners and the connections filling their backlogs, two each, for the
 * caller to close.
 */
static const char *wl_slow_peers(struct dc_worker *w, int *slow)
{
    TcpMeshTopologyPayload topo;

    memset(&topo, 0, sizeof(topo));
    for (uint32_t i = 0; i < WL_SLOW; i++) {
        uint16_t port;

        slow[2 * i] = listen_unanswering(&port, &slow[2 * i + 1]);
        if (slow[2 * i] < 0)
            return "could not open the peers' listeners";
        tp_entry(&topo, WL_SLOW_FIRST + i, port, true);
    }
    return send_topology(w->mgr, &topo) ? NULL
                                        : "could not send the topology update";
}

/*
 * WL_ANSWERED and WL_BUSY: for longer than the three intervals the worker
 * waits before it gives up on a silent manager, the manager says nothing of
 * its own, but answers each heartbeat.
 */
static const char *wl_answered(struct dc_worker *w, int lfd, bool busy)
{
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    int slow[2 * WL_SLOW];
    const char *why = NULL;
    double end;
    int answered = 0;

    for (uint32_t i = 0; i < 2 * WL_SLOW; i++)
        slow[i] = -1;
    if (busy)
        why = wl_slow_peers(w, slow);
    end = now_s() + (busy ? 5.5 : 4.5);

    while (!why && now_s() < end) {
        int left = (int)((end - now_s()) * 1000.0);

        if (wait_for_type(w->mgr, TCP_MSG_HEARTBEAT, left > 0 ? left : 1)) {
            if (!send_header(w->mgr, TCP_MSG_HEARTBEAT_RESP, 0))
                why = "could not answer the heartbeat";
            answered++;
        }
    }
    if (!why && answered == 0)
        why = "the worker sent the quiet manager no heartbeat";
    else if (!why && (poll(&pfd, 1, 0) != 0 || wl_attempts(w) != 0))
        why = "the worker connected again to a manager that answered";
    else if (!why && busy &&
             atomic_load(&dc_priv(w)->tcp_stats.peer_connect_attempts) <
                 WL_SLOW)
        why = "the worker didn't connect to the slow peers";
    for (uint32_t i = 0; i < 2 * WL_SLOW; i++)
        if (slow[i] >= 0)
            close(slow[i]);
    return why;
}

/*
 * Queue a request from WL_CANCEL_QP behind the message the send thread is
 * blocked on, through dev, and cancel it.  The connection to the manager,
 * old, which the request goes on, isn't in the connection table, so this is
 * the cancel's look at manager_conn.
 */
static const char *wl_cancel(RdmaBackendDev *dev, TcpConnection *old)
{
    static const uint8_t hdr[16];
    bool cancelled;

    if (tcp_backend_send_ionic_v(dev, WL_OTHER, hdr, sizeof(hdr), NULL, 0,
                                 (uint64_t)WL_CANCEL_QP << 32, false) != 0)
        return "the request to cancel wasn't queued";
    tcp_backend_ionic_cancel(dev, (uint64_t)WL_CANCEL_QP << 32,
                             UINT64_C(0xffffffff00000000));
    qemu_mutex_lock(&old->sendq_lock);
    /* The message being written is off the queue, but still counted */
    cancelled = old->sendq.length == 0 &&
                old->sendq_bytes == WL_STUCK_LEN + sizeof(hdr);
    qemu_mutex_unlock(&old->sendq_lock);
    return cancelled ? NULL : "the request wasn't cancelled";
}

/*
 * WL_STUCK: the manager reads nothing, so a send to it larger than its
 * socket and the worker's together can hold waits for room, holding the
 * connection's lock.  The data path only queues it, and the connection's
 * send thread is what waits.  The manager sends nothing either, and the
 * worker has to give up on it without waiting for that lock.  old is the
 * worker's connection to the manager.
 */
static const char *wl_stuck(struct dc_worker *w, int lfd, int high,
                            struct wl_fds *k, TcpConnection *old)
{
    static const uint8_t hdr[16];
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    RdmaBackendDev dev;
    uint8_t *body = g_malloc0(WL_STUCK_LEN);
    const char *why = NULL;
    int mgr = -1;

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = dc_priv(w);
    /* No direct connection to WL_OTHER, so the manager would relay it */
    if (tcp_backend_send_ionic_v(&dev, WL_OTHER, hdr, sizeof(hdr), body,
                                 WL_STUCK_LEN, 0, false) != 0)
        why = "the data path's send wasn't queued";
    else if (!held_elsewhere(&old->lock))
        why = "the send thread never blocked on the manager's connection";
    else
        why = wl_cancel(&dev, old);
    /* Three intervals after the manager last said anything, and then some */
    if (!why && poll(&pfd, 1, 8000) != 1)
        why = "the worker didn't connect to the manager again";
    else if (!why)
        why = wl_registers(w, lfd, k, &mgr);
    if (!why && !wait_retired(old))
        why = "the old connection wasn't closed";
    g_free(body);
    return why ? why : wl_accepted(w, mgr, high);
}

/*
 * WL_REFUSED, WL_OTHER_ID and WL_UNKNOWN.  A manager that answers with
 * another id gives each worker that asks for its id a new one, so a worker
 * that went on asking would add a node to the mesh every interval.  One that
 * has no node with the worker's id would refuse it every interval, forever.
 */
static const char *wl_refused(struct dc_worker *w, int lfd, int high,
                              struct wl_fds *k, enum wl_scenario scenario)
{
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    TcpConnection *now;
    const char *why;
    bool answered;
    int mgr;

    close(w->mgr);
    w->mgr = -1;
    why = wl_registers(w, lfd, k, &mgr);
    if (why)
        return why;
    if (scenario == WL_OTHER_ID)
        answered = wr_assign(mgr, WL_ID + 1);
    else
        answered = send_register_resp(mgr, 0,
                                      scenario == WL_UNKNOWN ? -ENOENT : -EBUSY,
                                      (uint32_t)sizeof(TcpRegisterRespPayload));
    if (!answered)
        return "could not answer the registration";
    if (!ends(mgr))
        return "the worker kept the refused connection";
    if (tcp_local_node(dc_priv(w)) != WL_ID)
        return "the worker's id changed";
    now = tcp_worker_manager_conn(dc_priv(w));
    tcp_connection_unref(now);
    if (now)
        return "the worker took the refused connection for the manager's";
    if (scenario != WL_REFUSED) {
        /* Its next pass comes within the 1 s interval */
        if (poll(&pfd, 1, 2500) != 0 || wl_attempts(w) != 1)
            return "the worker registered again";
        return NULL;
    }

    why = wl_registers(w, lfd, k, &mgr);
    if (why)
        return why;
    return wl_accepted(w, mgr, high);
}

/*
 * WL_GONE: the manager closes the connection and stops listening, and the
 * worker's connects are refused, until it listens again on the same port.
 * *lfd is the listener, replaced here.
 */
static const char *wl_gone(struct dc_worker *w, int *lfd, uint16_t port,
                           int high, struct wl_fds *k)
{
    const char *why;
    int mgr;

    close(*lfd);
    *lfd = -1;
    close(w->mgr);
    w->mgr = -1;
    for (int i = 0; i < 500 && wl_attempts(w) < 2; i++)
        sleep_ms(10);
    if (wl_attempts(w) < 2)
        return "the worker didn't keep trying";
    *lfd = listen_on(port);
    if (*lfd < 0)
        return "could not listen on the manager's port again";
    why = wl_registers(w, *lfd, k, &mgr);
    return why ? why : wl_accepted(w, mgr, high);
}

/*
 * WL_STOP: the manager reads the registration and doesn't answer.  The stop
 * is tcp_fini(), which has to wake the wait.
 */
static const char *wl_stop(struct dc_worker *w, int lfd, struct wl_fds *k)
{
    TcpBackendPrivate *priv = dc_priv(w);
    TcpConnection *conn;
    const char *why;
    double start;
    int mgr;

    close(w->mgr);
    w->mgr = -1;
    why = wl_registers(w, lfd, k, &mgr);
    if (why)
        return why;
    conn = tcp_worker_manager_conn(priv);
    tcp_connection_unref(conn);
    if (conn)
        return "the worker has a connection to the manager while it registers";
    conn = tcp_route_to_node(priv, WL_OTHER);
    tcp_connection_unref(conn);
    if (conn)
        return "the worker routes through a manager it isn't registered with";

    start = now_s();
    rdma_backend_ops_tcp.fini(&w->in.dev);
    w->in.ret = -1; /* so dc_stop() doesn't stop it again */
    if (now_s() - start >= 1.0)
        return "the stop waited for the answer";
    return ends(mgr) ? NULL : "the registering connection wasn't closed";
}

static const char *wl_run(enum wl_scenario scenario, struct dc_worker *w,
                          int *lfd, uint16_t port, int high, struct wl_fds *k)
{
    TcpConnection *old = tcp_worker_manager_conn(dc_priv(w));
    const char *why;

    if (!old)
        return "the worker has no connection to the manager";
    switch (scenario) {
    case WL_HANGS_UP:
    case WL_SILENT:
        why = wl_again(w, *lfd, high, k, old, scenario == WL_SILENT);
        break;
    case WL_ANSWERED:
    case WL_BUSY:
        why = wl_answered(w, *lfd, scenario == WL_BUSY);
        break;
    case WL_STUCK:
        why = wl_stuck(w, *lfd, high, k, old);
        break;
    case WL_REFUSED:
    case WL_OTHER_ID:
    case WL_UNKNOWN:
        why = wl_refused(w, *lfd, high, k, scenario);
        break;
    case WL_GONE:
        why = wl_gone(w, lfd, port, high, k);
        break;
    case WL_STOP:
        why = wl_stop(w, *lfd, k);
        break;
    case WL_EVICTED:
        why = wl_evicted(w, *lfd, k);
        break;
    default:
        why = "unknown scenario";
        break;
    }
    tcp_connection_unref(old);
    return why;
}

/*
 * Run one row: start the worker, play the manager for the row, then stop the
 * worker and check that every thread it started was joined exactly once.
 */
static int run_wl_case(const struct wl_case *c)
{
    struct dc_worker w;
    struct wl_fds k;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const char *why = NULL;
    uint16_t port = 0;
    int lfd, high;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    memset(&k, 0, sizeof(k));
    dc_init(&w, 1);

    g_current_row = c->description;
    alarm(row_timeout_s());
    lfd = listen_loopback();
    high = listen_loopback();
    if (lfd < 0 || high < 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &len) < 0)
        why = "could not open the listeners";
    else
        port = ntohs(addr.sin_port);
    if (!why)
        why = dc_start(&w, lfd, port, WL_ID);
    if (!why)
        why = wl_run(c->scenario, &w, &lfd, port, high, &k);

    dc_stop(&w);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s manager link %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
           c->category, c->description, c->expected);
    if (!ok)
        printf("     %s; %d thread(s) created, %d joined, %d exited\n", why,
               atomic_load(&g_creates), atomic_load(&g_joins),
               atomic_load(&g_exits));
    fflush(stdout);

    for (uint32_t i = 0; i < k.n; i++)
        close(k.fd[i]);
    if (lfd >= 0)
        close(lfd);
    if (high >= 0)
        close(high);
    return ok ? 0 : 1;
}

/* ---- A peer that stops reading --------------------------------------- */

/*
 * A send to a peer that stayed connected but stopped reading used to wait
 * for room for as long as the peer took, holding the connection's lock, and
 * every thread that wanted the lock waited behind it: the device's main
 * thread sending the data path's messages, broadcasts, heartbeats, and a
 * manager relaying for another worker on that worker's receive thread.
 *
 * A send now gives up once nothing has gone out for the send timeout (a
 * manager's is half a worker's), and shuts the connection down, so senders
 * waiting for the lock fail at once and the connection is retired.  The data
 * path's messages are queued for a send thread instead of sent from the main
 * thread, and the queue has a limit.  The rows run an mgr_fixture whose
 * nodes' ends of their connections never read, except ST_STEADY's.
 *
 * What the queue holds can be taken back: a QP that is flushed or destroyed,
 * or a request that times out, has its requests dropped
 * (tcp_backend_ionic_cancel()), so none reaches the peer after its work
 * request was completed.  And a heartbeat is never skipped for want of the
 * lock, which a send thread under steady traffic almost always holds: it
 * goes ahead of the data on the queue, which holds one of each kind at most.
 */

enum st_scenario {
    ST_TIMEOUT,    /* a broadcast, with a second asked for meanwhile */
    ST_DATAPATH,   /* the data path's message, through the send thread */
    ST_QUEUE_FULL, /* the data path fills the send queue */
    ST_RELAY,      /* the manager relays to a node that stopped reading */
    ST_CANCEL,     /* a QP's queued requests are taken back */
    ST_TOO_LONG,   /* lengths whose sum wraps */
    ST_STEADY,     /* a node that reads, under steady traffic */
    ST_COALESCE,   /* heartbeats queue behind a busy connection */
    ST_UNPUBLISH,  /* a connection with a queue leaves the table */
};

struct st_case {
    const char *category;
    const char *description;
    const char *expected;
    enum st_scenario scenario;
};

static const struct st_case st_cases[] = {
    {"negative",
     "a broadcast to a node that stops reading, with a second broadcast "
     "asked for while the first is sent",
     "the first gives up after the manager's send timeout and shuts the "
     "connection down, the second then fails at once, and the health check "
     "retires the connection",
     ST_TIMEOUT},
    {"negative", "the data path sends to a node that stops reading",
     "the message is queued at once; the send thread gives up after the "
     "timeout and shuts the connection down, and the next send fails",
     ST_DATAPATH},
    {"boundary",
     "the data path queues messages for a node that stops reading until the "
     "queue is full",
     "a request past the limit is refused with -EAGAIN, which "
     "tcp_backend_ionic_has_room() foresees; a reply is queued up to its own "
     "limit, then refused with -ENOBUFS",
     ST_QUEUE_FULL},
    {"negative",
     "the manager relays a worker's message to a node that stops reading",
     "the relay gives up in the manager's timeout, so the heartbeat the "
     "worker sent after the message is answered within the worker's own "
     "timeout, and the worker's connection stays up",
     ST_RELAY},
    {"positive",
     "requests and a reply of one QP's and requests of another's are queued "
     "behind a message being written, and the first QP's are cancelled, and "
     "one request of the second's",
     "those requests are dropped and their bytes uncounted; the reply, the "
     "second QP's other requests, and the message being written stay",
     ST_CANCEL},
    {"boundary",
     "the data path sends a header and a body whose lengths add up past "
     "SIZE_MAX, or one byte past TCP_MAX_PAYLOAD_LEN",
     "refused with -EINVAL before either buffer is read, and nothing queued",
     ST_TOO_LONG},
    {"negative",
     "the data path keeps a node's queue full for four 1 s health-check "
     "intervals while the node reads slowly",
     "the manager's heartbeats get through ahead of the data, the node "
     "answers them, and it is never declared dead or its connection closed",
     ST_STEADY},
    {"corner",
     "while the send thread is blocked on a node, the health check sends it a "
     "heartbeat and the node sends a hundred of its own",
     "the queue holds one heartbeat and one answer, not one answer for each",
     ST_COALESCE},
    {"corner",
     "a node's connection is taken out of the table, as before it is retired, "
     "while its send thread is blocked and a request waits behind it",
     "the queue is closed and the request dropped at once, before the "
     "connection is retired, so a cancel that no longer finds the connection "
     "misses nothing; the next send to the node fails",
     ST_UNPUBLISH},
};

/* A worker's send timeout in these rows; the manager's is half */
#define ST_TIMEOUT_MS 2000u

/*
 * ST_TIMEOUT: the broadcaster's first pass sends the topology, and blocks on
 * node 1.  The second is asked for meanwhile, so it is the next pass's, and
 * it goes to node 1 too.
 */
static const char *st_timeout(struct mgr_fixture *fx)
{
    TcpBackendPrivate *priv = fx->priv;
    TcpConnection *conn = fx->old[0];
    uint64_t start_ms = ernic_monotonic_ms(), first_ms, second_ms;

    if (!tcp_start_thread(&priv->broadcast_thread, "tcp-broadcast",
                          tcp_broadcast_thread, priv, &priv->broadcast_running))
        return "could not start the broadcaster";
    tcp_broadcast_topology(priv);
    if (!held_elsewhere(&conn->lock))
        return "the first broadcast never blocked on node 1";
    tcp_broadcast_node_gids(priv, 1);

    if (!broadcasts_sent_reach(priv, 1, 3 * ST_TIMEOUT_MS))
        return "the first broadcast never gave up";
    first_ms = ernic_monotonic_ms();
    if (!broadcasts_sent_reach(priv, 2, 3 * ST_TIMEOUT_MS))
        return "the second broadcast never finished";
    second_ms = ernic_monotonic_ms();

    if (first_ms - start_ms < ST_TIMEOUT_MS / 2 - 100u)
        return "the first broadcast didn't give up after the manager's "
               "timeout";
    if (second_ms - first_ms > 250u)
        return "the second broadcast waited a timeout of its own";
    if (!wait_dead(conn))
        return "the connection wasn't shut down";
    mgr_start(fx, 0);
    return wait_retired(conn) ? NULL : "the connection wasn't retired";
}

/* The data path's sends of what it calls tag, as the main thread makes them */
static int st_send_tag(TcpBackendPrivate *priv, uint32_t len, uint64_t tag,
                       bool reply)
{
    static const uint8_t hdr[16];
    RdmaBackendDev dev;
    /* No body at all for an empty one, as the data path sends it */
    uint8_t *body = len > 0 ? g_malloc0(len) : NULL;
    int rc;

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = priv;
    rc = tcp_backend_send_ionic_v(&dev, 1, hdr, sizeof(hdr), body, len, tag,
                                  reply);
    g_free(body);
    return rc;
}

static int st_send(TcpBackendPrivate *priv, uint32_t len, bool reply)
{
    return st_send_tag(priv, len, 0, reply);
}

/* ST_DATAPATH */
static const char *st_datapath(struct mgr_fixture *fx)
{
    TcpConnection *conn = fx->old[0];
    uint64_t start_ms = ernic_monotonic_ms();
    uint64_t took_ms;
    bool closed = false;
    int rc;

    if (st_send(fx->priv, 0, false) != 0)
        return "the message wasn't queued";
    if (ernic_monotonic_ms() - start_ms > 100u)
        return "queueing the message waited for the peer";
    if (!wait_dead(conn))
        return "the send thread never gave up on the peer";
    took_ms = ernic_monotonic_ms() - start_ms;
    if (took_ms < ST_TIMEOUT_MS / 2 - 100u)
        return "the send thread gave up before the timeout";
    /* The send thread shuts the socket down first, then closes the queue */
    for (int i = 0; i < 500; i++) {
        qemu_mutex_lock(&conn->sendq_lock);
        closed = conn->sendq_closed;
        qemu_mutex_unlock(&conn->sendq_lock);
        if (closed)
            break;
        sleep_ms(10);
    }
    if (!closed)
        return "the send queue is still open";
    /* The connection is dead, so there is none to the node to send on */
    rc = st_send(fx->priv, 0, false);
    return rc == -ENOTCONN || rc == -EPIPE ? NULL : "the next send didn't fail";
}

/* ST_QUEUE_FULL */
static const char *st_queue_full(struct mgr_fixture *fx)
{
    /* With the 16-byte header, each is the largest message there is */
    const uint32_t len = TCP_MAX_PAYLOAD_LEN - 16u;
    RdmaBackendDev dev;

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = fx->priv;
    if (!tcp_backend_ionic_has_room(&dev, 1, TCP_MAX_PAYLOAD_LEN))
        return "an empty queue had no room";
    /* Requests up to their limit, the first of them being written */
    for (uint32_t i = 0; i < TCP_SENDQ_REQUEST_BYTES / TCP_MAX_PAYLOAD_LEN; i++)
        if (st_send(fx->priv, len, false) != 0)
            return "a request within the limit wasn't queued";
    if (tcp_backend_ionic_has_room(&dev, 1, 16))
        return "tcp_backend_ionic_has_room() saw room in a full queue";
    if (st_send(fx->priv, 0, false) != -EAGAIN)
        return "a request past the limit wasn't refused with -EAGAIN";
    /* Replies up to theirs */
    for (uint32_t i = TCP_SENDQ_REQUEST_BYTES / TCP_MAX_PAYLOAD_LEN;
         i < TCP_SENDQ_REPLY_BYTES / TCP_MAX_PAYLOAD_LEN; i++)
        if (st_send(fx->priv, len, true) != 0)
            return "a reply within its limit wasn't queued";
    if (st_send(fx->priv, 0, true) != -ENOBUFS)
        return "a reply past its limit wasn't refused with -ENOBUFS";
    return NULL;
}

/* ST_RELAY */
static const char *st_relay(struct mgr_fixture *fx)
{
    int w1 = fx->peer[0];
    uint64_t start_ms = ernic_monotonic_ms();
    uint64_t took_ms;
    bool alive;

    if (!send_ionic(w1, 1, 2) || !send_heartbeat(w1, 1))
        return "worker 1 couldn't send";
    if (!wait_for_type(w1, TCP_MSG_HEARTBEAT_RESP, 3 * ST_TIMEOUT_MS))
        return "worker 1's heartbeat wasn't answered";
    took_ms = ernic_monotonic_ms() - start_ms;
    if (took_ms < ST_TIMEOUT_MS / 2 - 100u)
        return "the relay didn't wait for node 2";
    if (took_ms >= ST_TIMEOUT_MS)
        return "the heartbeat wasn't answered within worker 1's timeout";
    if (!wait_dead(fx->old[1]))
        return "node 2's connection wasn't shut down";
    qemu_mutex_lock(&fx->priv->mesh_table_lock);
    alive = fx->node[0]->is_alive;
    qemu_mutex_unlock(&fx->priv->mesh_table_lock);
    if (!alive || tcp_conn_dead(fx->old[0]))
        return "worker 1 lost its connection, or was declared dead";
    return NULL;
}

/*
 * ST_CANCEL.  Tags as the data path makes them: a QP in the high half, a
 * request id in the low.
 */
#define ST_QP_MASK      UINT64_C(0xffffffff00000000)
#define ST_TAG(qp, req) ((uint64_t)(qp) << 32 | (req))

static const char *st_cancel(struct mgr_fixture *fx)
{
    /* After the first: QP 5's requests go, and QP 6's second, alone */
    static const struct {
        uint64_t tag;
        bool reply;
        uint32_t len;
        bool kept;
    } queued[] = {
        {ST_TAG(5, 2), false, 200, false}, {ST_TAG(6, 3), false, 300, true},
        {ST_TAG(6, 4), false, 350, false}, {ST_TAG(5, 5), true, 400, true},
        {ST_TAG(6, 6), false, 450, true},  {ST_TAG(5, 7), false, 500, false},
    };
    TcpConnection *conn = fx->old[0];
    RdmaBackendDev dev;
    size_t want = 100 + 16; /* the message being written, with its header */
    const char *why = NULL;
    GList *link;

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = fx->priv;

    /* The first, from QP 5 too, is off the queue: the send thread is on it */
    if (st_send_tag(fx->priv, 100, ST_TAG(5, 1), false) != 0)
        return "the first message wasn't queued";
    if (!held_elsewhere(&conn->lock))
        return "the send thread never blocked on the node";
    for (size_t i = 0; i < sizeof(queued) / sizeof(queued[0]); i++) {
        if (st_send_tag(fx->priv, queued[i].len, queued[i].tag,
                        queued[i].reply) != 0)
            return "a message wasn't queued";
        if (queued[i].kept)
            want += queued[i].len + 16u;
    }
    tcp_backend_ionic_cancel(&dev, ST_TAG(5, 0), ST_QP_MASK);
    tcp_backend_ionic_cancel(&dev, ST_TAG(6, 4), UINT64_MAX);

    qemu_mutex_lock(&conn->sendq_lock);
    link = conn->sendq.head;
    for (size_t i = 0; !why && i < sizeof(queued) / sizeof(queued[0]); i++) {
        const TcpQueuedMsg *msg = link ? link->data : NULL;

        if (!queued[i].kept)
            continue;
        if (!msg || msg->tag != queued[i].tag ||
            msg->reply != queued[i].reply || msg->len != queued[i].len + 16u)
            why = "the queue doesn't hold what wasn't cancelled, in order";
        link = link ? link->next : NULL;
    }
    if (!why && link)
        why = "a cancelled request is still queued";
    if (!why && conn->sendq_bytes != want)
        why = "the queue's byte count is wrong";
    qemu_mutex_unlock(&conn->sendq_lock);
    return why;
}

/* ST_TOO_LONG */
static const char *st_too_long(struct mgr_fixture *fx)
{
    /* Never read: each length is refused before either buffer is */
    static const uint8_t hdr[16], body[16];
    static const struct {
        size_t hdr_len, body_len;
    } rows[] = {
        {64, SIZE_MAX - 31}, /* adds up to 32 */
        {SIZE_MAX, 1},       /* adds up to 0 */
        {16, TCP_MAX_PAYLOAD_LEN - 15},
        {(size_t)TCP_MAX_PAYLOAD_LEN + 1u, 0},
    };
    TcpConnection *conn = fx->old[0];
    RdmaBackendDev dev;
    size_t bytes;
    bool queued;

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = fx->priv;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (tcp_backend_send_ionic_v(&dev, 1, hdr, rows[i].hdr_len, body,
                                     rows[i].body_len, 0, false) != -EINVAL)
            return "tcp_backend_send_ionic_v() didn't refuse the lengths";
        if (tcp_conn_enqueue(conn, hdr, rows[i].hdr_len, body, rows[i].body_len,
                             1, 0, false) != -EINVAL)
            return "tcp_conn_enqueue() didn't refuse the lengths";
    }
    qemu_mutex_lock(&conn->sendq_lock);
    bytes = conn->sendq_bytes;
    queued = conn->send_thread_started || conn->sendq.length != 0;
    qemu_mutex_unlock(&conn->sendq_lock);
    return bytes == 0 && !queued ? NULL : "something was queued";
}

/*
 * ST_STEADY: node 1 reads, but slower than the manager writes, so its socket
 * stays full and the send thread holds the connection's lock nearly all the
 * time.  It answers each heartbeat, as a worker does.
 */
#define ST_STEADY_LEN (256u << 10) /* larger than the socket's buffer */

struct st_reader {
    int fd;
    atomic_bool running;
    atomic_int heartbeats;
    atomic_bool failed;
    pthread_t tid;
};

static void *st_reader_run(void *opaque)
{
    struct st_reader *r = opaque;

    while (atomic_load(&r->running)) {
        struct pollfd pfd = {.fd = r->fd, .events = POLLIN};
        TcpMsgHeader hdr;
        void *payload = NULL;

        if (poll(&pfd, 1, 50) != 1)
            continue;
        if (tcp_recv_message(r->fd, &hdr, &payload, NULL, 0) < 0) {
            atomic_store(&r->failed, true);
            break;
        }
        free(payload);
        if (hdr.msg_type == TCP_MSG_HEARTBEAT) {
            atomic_fetch_add(&r->heartbeats, 1);
            if (!send_heartbeat(r->fd, 1)) {
                atomic_store(&r->failed, true);
                break;
            }
        }
        sleep_ms(2);
    }
    return NULL;
}

static const char *st_steady(struct mgr_fixture *fx)
{
    struct st_reader reader;
    const char *why = NULL;
    uint32_t sent = 0, full = 0;
    double end;
    bool alive;

    reader.fd = fx->peer[0];
    atomic_init(&reader.running, true);
    atomic_init(&reader.heartbeats, 0);
    atomic_init(&reader.failed, false);
    if (pthread_create(&reader.tid, NULL, st_reader_run, &reader) != 0)
        return "could not start the reader";

    /* Alive and just heard from; nothing is running yet to race this */
    fx->node[0]->is_alive = true;
    mgr_start(fx, 0);
    end = now_s() + 4.5;
    while (now_s() < end && !atomic_load(&reader.failed)) {
        int rc = st_send(fx->priv, ST_STEADY_LEN, false);

        if (rc == 0) {
            sent++;
        } else if (rc == -EAGAIN) {
            full++;
            sleep_ms(1);
        } else {
            break;
        }
    }

    qemu_mutex_lock(&fx->priv->mesh_table_lock);
    alive = fx->node[0]->is_alive;
    qemu_mutex_unlock(&fx->priv->mesh_table_lock);
    /* Declared dead, the node has its connection closed, and its end fails */
    if (!alive)
        why = "node 1 was declared dead";
    else if (tcp_conn_dead(fx->old[0]) || table_get(fx->priv, 1) != fx->old[0])
        why = "node 1's connection was closed";
    else if (atomic_load(&reader.failed))
        why = "node 1's end of the connection failed";
    else if (atomic_load(&reader.heartbeats) < 3)
        why = "node 1 got fewer than three heartbeats";
    else if (full == 0 || sent <= TCP_SENDQ_REQUEST_BYTES / ST_STEADY_LEN)
        why = "the queue was never full, or never drained";

    atomic_store(&reader.running, false);
    pthread_join(reader.tid, NULL);
    return why;
}

/*
 * ST_COALESCE.  How many of msg_type conn's send queue holds, and how many
 * messages in all.
 */
#define ST_FLOOD 100

static int st_queued(TcpConnection *conn, TcpMsgType msg_type, guint *all)
{
    int n = 0;

    qemu_mutex_lock(&conn->sendq_lock);
    for (GList *link = conn->sendq.head; link != NULL; link = link->next)
        if (((const TcpQueuedMsg *)link->data)->msg_type == msg_type)
            n++;
    if (all)
        *all = conn->sendq.length;
    qemu_mutex_unlock(&conn->sendq_lock);
    return n;
}

/* Wait up to 5 s for conn's send queue to hold a msg_type */
static bool st_wait_queued(TcpConnection *conn, TcpMsgType msg_type)
{
    for (int i = 0; i < 500; i++) {
        if (st_queued(conn, msg_type, NULL) > 0)
            return true;
        sleep_ms(10);
    }
    return false;
}

/* Wait up to 5 s for the manager to have read everything sent on fd */
static bool st_drained(int fd)
{
    for (int i = 0; i < 500; i++) {
        int unread = -1;

        if (ioctl(fd, SIOCOUTQ, &unread) == 0 && unread == 0)
            return true;
        sleep_ms(10);
    }
    return false;
}

static const char *st_coalesce(struct mgr_fixture *fx)
{
    TcpConnection *conn = fx->old[0];
    guint all;

    /* Alive, so its heartbeats revive nothing and start no broadcast */
    fx->node[0]->is_alive = true;
    if (st_send(fx->priv, 0, false) != 0)
        return "the data path's message wasn't queued";
    if (!held_elsewhere(&conn->lock))
        return "the send thread never blocked on the node";
    mgr_start(fx, 0);
    if (!st_wait_queued(conn, TCP_MSG_HEARTBEAT))
        return "the health check's heartbeat wasn't queued";
    for (int i = 0; i < ST_FLOOD; i++)
        if (!send_heartbeat(fx->peer[0], 1))
            return "the node couldn't send its heartbeats";
    if (!st_drained(fx->peer[0]) ||
        !st_wait_queued(conn, TCP_MSG_HEARTBEAT_RESP))
        return "the node's heartbeats weren't answered";
    /*
     * The last heartbeat's answer can still be on its way, which without the
     * coalescing would leave more answers than one, not fewer.
     */
    if (st_queued(conn, TCP_MSG_HEARTBEAT_RESP, &all) != 1 ||
        st_queued(conn, TCP_MSG_HEARTBEAT, NULL) != 1 || all != 2)
        return "the queue doesn't hold just one heartbeat and one answer";
    return NULL;
}

/* ST_UNPUBLISH */
static const char *st_unpublish(struct mgr_fixture *fx)
{
    TcpConnection *conn = fx->old[0];
    const char *why = NULL;
    bool taken, closed;
    guint queued;
    size_t bytes;
    int rc;

    if (st_send(fx->priv, 100, false) != 0)
        return "the first message wasn't queued";
    if (!held_elsewhere(&conn->lock))
        return "the send thread never blocked on the node";
    if (st_send(fx->priv, 200, false) != 0)
        return "the request wasn't queued";

    /* As the health check, reconnects and replacements do */
    qemu_mutex_lock(&fx->priv->conn_table_lock);
    taken = tcp_conn_table_take(fx->priv, 1, conn);
    qemu_mutex_unlock(&fx->priv->conn_table_lock);
    if (!taken)
        return "the connection couldn't be taken out of the table";

    qemu_mutex_lock(&conn->sendq_lock);
    closed = conn->sendq_closed;
    queued = conn->sendq.length;
    bytes = conn->sendq_bytes;
    qemu_mutex_unlock(&conn->sendq_lock);
    if (!closed || queued != 0 || bytes != 0)
        why = "the queue was still open, or still held the request";
    else if ((rc = st_send(fx->priv, 0, false)) != -ENOTCONN && rc != -EPIPE)
        why = "the next send to the node didn't fail";

    /* The table's reference is now this row's, to retire and drop */
    tcp_retire_conn(conn);
    tcp_connection_unref(conn);
    return why;
}

static int run_st_case(const struct st_case *c)
{
    struct mgr_fixture fx;
    uint32_t nodes = c->scenario == ST_RELAY ? 2 : 1;
    uint32_t stuck = nodes; /* the node that stops reading */
    const char *why;
    bool ok;

    atomic_store(&g_creates, 0);
    atomic_store(&g_joins, 0);
    atomic_store(&g_exits, 0);
    g_current_row = c->description;
    alarm(ROW_TIMEOUT_S);
    why = mgr_setup(&fx, nodes, c->scenario == ST_STEADY ? 1 : LONG_INTERVAL_S);
    /* ST_QUEUE_FULL keeps the default, so the queue isn't closed under it */
    if (!why && c->scenario != ST_QUEUE_FULL)
        atomic_store(&fx.priv->send_timeout_ms, ST_TIMEOUT_MS);
    if (!why && c->scenario != ST_STEADY && !fill_conn(fx.priv, stuck))
        why = "could not fill the node's socket";
    if (!why) {
        switch (c->scenario) {
        case ST_TIMEOUT:
            why = st_timeout(&fx);
            break;
        case ST_DATAPATH:
            why = st_datapath(&fx);
            break;
        case ST_QUEUE_FULL:
            why = st_queue_full(&fx);
            break;
        case ST_RELAY:
            why = st_relay(&fx);
            break;
        case ST_CANCEL:
            why = st_cancel(&fx);
            break;
        case ST_TOO_LONG:
            why = st_too_long(&fx);
            break;
        case ST_STEADY:
            why = st_steady(&fx);
            break;
        case ST_COALESCE:
            why = st_coalesce(&fx);
            break;
        case ST_UNPUBLISH:
        default:
            why = st_unpublish(&fx);
            break;
        }
    }
    mgr_teardown(&fx);
    alarm(0);
    if (!why && (atomic_load(&g_joins) != atomic_load(&g_creates) ||
                 atomic_load(&g_exits) != atomic_load(&g_creates)))
        why = "a thread wasn't joined exactly once";

    ok = !why;
    printf("%s stuck peer %s: %s; expected: %s\n", ok ? "ok  " : "FAIL",
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
    for (size_t i = 0; i < sizeof(sl_cases) / sizeof(sl_cases[0]); i++)
        failures += run_silent_case(&sl_cases[i]);
    for (size_t i = 0; i < sizeof(connect_cases) / sizeof(connect_cases[0]);
         i++)
        failures += run_connect_case(&connect_cases[i]);
    for (size_t i = 0; i < sizeof(reg_cases) / sizeof(reg_cases[0]); i++)
        failures += run_reg_case(&reg_cases[i]);
    for (size_t i = 0; i < sizeof(tp_cases) / sizeof(tp_cases[0]); i++)
        failures += run_tp_case(&tp_cases[i]);
    for (size_t i = 0; i < sizeof(wr_cases) / sizeof(wr_cases[0]); i++)
        failures += run_wr_case(&wr_cases[i]);
    for (size_t i = 0; i < sizeof(sp_cases) / sizeof(sp_cases[0]); i++)
        failures += run_sp_case(&sp_cases[i]);
    for (size_t i = 0; i < sizeof(reap_cases) / sizeof(reap_cases[0]); i++)
        failures += run_reap_case(&reap_cases[i]);
    for (size_t i = 0; i < sizeof(sender_cases) / sizeof(sender_cases[0]); i++)
        failures += run_sender_case(&sender_cases[i]);
    for (size_t i = 0; i < sizeof(ra_cases) / sizeof(ra_cases[0]); i++)
        failures += run_ra_case(&ra_cases[i]);
    for (size_t i = 0; i < sizeof(ev_cases) / sizeof(ev_cases[0]); i++)
        failures += run_ev_case(&ev_cases[i]);
    for (size_t i = 0; i < sizeof(lv_cases) / sizeof(lv_cases[0]); i++)
        failures += run_lv_case(&lv_cases[i]);
    for (size_t i = 0; i < sizeof(lt_cases) / sizeof(lt_cases[0]); i++)
        failures += run_lt_case(&lt_cases[i]);
    for (size_t i = 0; i < sizeof(pp_cases) / sizeof(pp_cases[0]); i++)
        failures += run_pp_case(&pp_cases[i]);
    for (size_t i = 0; i < sizeof(dc_cases) / sizeof(dc_cases[0]); i++)
        failures += run_dc_case(&dc_cases[i]);
    for (size_t i = 0; i < sizeof(wl_cases) / sizeof(wl_cases[0]); i++)
        failures += run_wl_case(&wl_cases[i]);
    for (size_t i = 0; i < sizeof(st_cases) / sizeof(st_cases[0]); i++)
        failures += run_st_case(&st_cases[i]);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
