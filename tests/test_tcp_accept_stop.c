/*
 * Unit tests for stopping the TCP backend's accept thread.
 *
 * The listen socket is blocking, and tcp_fini() used to stop the accept
 * thread by clearing its flag and joining it.  A thread parked in accept()
 * never looks at the flag again until a peer connects, so the join -- and
 * with it a SIGTERM shutdown of a worker or manager -- hung until the
 * process was killed.  tcp_stop_accept_thread() now shuts the listen
 * socket down before the join, which wakes accept() with EINVAL.
 *
 * Each row starts (or deliberately does not start) the real
 * tcp_accept_thread() on a loopback listen socket, stops it, and checks
 * that the stop returned, that the thread really exited, and how many
 * joins it took.  A per-row alarm() turns a hang into a named failure
 * rather than a ctest timeout.  The translation unit is #included to reach
 * the static functions, and its externals are stubbed; the thread entry
 * points are backed by real pthreads.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
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
 * Real threads.  The trampoline records that the start routine returned,
 * so "the join came back" and "the thread actually left its loop" are
 * checked separately.
 */
static atomic_int g_joins;
static atomic_bool g_thread_exited;

struct trampoline {
    void *(*fn)(void *);
    void *arg;
};
static struct trampoline g_trampoline;

static void *trampoline_run(void *opaque)
{
    struct trampoline *t = opaque;
    void *ret = t->fn(t->arg);

    atomic_store(&g_thread_exited, true);
    return ret;
}

int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    (void)name;
    (void)mode;
    g_trampoline.fn = start_routine;
    g_trampoline.arg = arg;
    return pthread_create(&thread->thread, NULL, trampoline_run,
                          &g_trampoline) == 0
               ? 0
               : -1;
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
    static const char msg[] = "FAIL hung stopping the accept thread: ";

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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

/* ---- Tests ------------------------------------------------------------- */

enum setup {
    NOT_STARTED,      /* flag clear, no thread */
    BLOCKED,          /* thread started and given time to park in accept() */
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
    atomic_store(&g_thread_exited, false);
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
         atomic_load(&g_thread_exited) == c->want_exited &&
         (fd < 0 || still_listening(fd) == c->want_listening) &&
         !atomic_load(&priv->accept_thread_running) && took < 2.0;

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok)
        printf("     got %d join(s) (want %d), exited %d (want %d), "
               "listening %d (want %d), %.3f s\n",
               atomic_load(&g_joins), c->want_joins,
               (int)atomic_load(&g_thread_exited), (int)c->want_exited,
               fd < 0 ? -1 : (still_listening(fd) ? 1 : 0),
               (int)c->want_listening, took);

    if (fd >= 0)
        close(fd);
    g_free(priv);
    return ok ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    signal(SIGALRM, watchdog);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failures += run_case(&cases[i]);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
