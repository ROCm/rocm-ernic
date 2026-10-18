/*
 * Lifecycle tests for the rocm-ernic server: startup, misbehaving clients,
 * and signal-driven shutdown.
 *
 * Each row starts the real server binary, mostly on the loopback backend,
 * does something to it (nothing, a client that hangs up, a client that sends
 * a bad header, a client that never speaks), signals it, and checks the exit
 * status and the log.  Three things are pinned here:
 *
 *   - a client that drops out during vfio-user negotiation no longer takes
 *     the server down: it logs the drop and waits for the next client;
 *   - a clean shutdown frees the ionic layer.  When the server itself is
 *     built with -DERNIC_USE_SANITIZERS=ON, LeakSanitizer runs at its exit
 *     and any report fails the row;
 *   - on the TCP backend, SIGTERM stops the backend's threads promptly
 *     rather than waiting out a blocking accept() or the health-check
 *     interval.
 *
 * Usage: test_server_lifecycle <server binary>
 *
 * If ERNIC_TEST_SERVER_WRAPPER is set, each server runs under the command it
 * holds, such as valgrind and its options.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_WAIT_MS 20000 /* sanitizer builds start slowly */
#define EXIT_WAIT_MS   20000
#define LOG_WAIT_MS    5000
#define SETTLE_MS      300

#define DROPPED_LINE  "client dropped during negotiation"
#define SHUTDOWN_LINE "Shutdown complete"

/* ---- Helpers ----------------------------------------------------------- */

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool path_is_socket(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int connect_unix(const char *path)
{
    struct sockaddr_un addr;
    size_t len = strlen(path);
    int fd;

    /* A path that does not fit sun_path cannot be connected to. */
    if (len >= sizeof(addr.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * A listening TCP socket on an ephemeral port, on every address as the TCP
 * manager binds.  Its port is stored in *port.  Closing it frees the port
 * for the server; keeping it open gives a worker a "manager" that accepts
 * its connection (in the kernel's backlog) and never answers.
 */
static int listen_ephemeral(uint16_t *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Read the whole log (bounded) into a NUL-terminated heap buffer. */
static char *read_log(const char *path)
{
    FILE *f = fopen(path, "re");
    char *buf;
    size_t n;

    if (!f)
        return NULL;
    buf = calloc(1, 1 << 20);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (1 << 20) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int count_in_log(const char *path, const char *needle)
{
    char *log = read_log(path);
    int n = 0;

    if (!log)
        return 0;
    for (const char *p = log; (p = strstr(p, needle)) != NULL;
         p += strlen(needle))
        n++;
    free(log);
    return n;
}

static bool wait_for_log(const char *path, const char *needle, int count)
{
    int64_t deadline = now_ms() + LOG_WAIT_MS;

    while (now_ms() < deadline) {
        if (count_in_log(path, needle) >= count)
            return true;
        sleep_ms(20);
    }
    return false;
}

/* Any sanitizer report in the server's output. */
static bool sanitizer_reported(const char *path)
{
    static const char *const markers[] = {
        "ERROR: LeakSanitizer",
        "ERROR: AddressSanitizer",
        "WARNING: ThreadSanitizer",
        "runtime error:",
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
        if (count_in_log(path, markers[i]) > 0)
            return true;
    return false;
}

/* waitpid() with a deadline; the server is SIGKILLed if it overstays. */
static int wait_exit(pid_t pid, int *status)
{
    int64_t deadline = now_ms() + EXIT_WAIT_MS;

    while (now_ms() < deadline) {
        pid_t r = waitpid(pid, status, WNOHANG);

        if (r == pid)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
        sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

/* ---- Client actions ---------------------------------------------------- */

enum action {
    IDLE,    /* no client at all */
    HANG_UP, /* connect, then close without a word */
    BAD_HDR, /* send a header that is not VFIO_USER_VERSION, then close */
    SILENT,  /* connect and stay connected, saying nothing */
};

/*
 * A vfio-user message header (struct vfio_user_header): msg_id, cmd,
 * msg_size, flags, error_no.  cmd 2 is not VFIO_USER_VERSION (1), so the
 * server's version negotiation rejects it with EINVAL.
 */
static int send_bad_header(int fd)
{
    const uint8_t hdr[16] = {0x01, 0x00, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    return send(fd, hdr, sizeof(hdr), MSG_NOSIGNAL) == (ssize_t)sizeof(hdr)
               ? 0
               : -1;
}

/* ---- Tests ------------------------------------------------------------- */

struct lifecycle_case {
    const char *category;
    const char *description;
    const char *expected;
    /* --backend value: @DIR@ is the scratch dir, @PORT@ a free TCP port */
    const char *backend;
    const char *env_name; /* an environment variable for the server, or NULL */
    const char *env_value;
    bool fake_manager; /* hold @PORT@ open as a manager that never answers */
    bool want_socket;  /* the socket should appear */
    bool settle;       /* let the server reach its attach loop first */
    enum action action;
    int clients;           /* how many times to do the action */
    int sig;               /* signal to send (0: none, wait for exit) */
    int sigs;              /* how many times to send it */
    int want_exit;         /* exit status; -1 means any non-zero */
    int want_dropped;      /* DROPPED_LINE count, checked before the signal */
    int want_shutdown;     /* SHUTDOWN_LINE count after exit */
    const char *want_line; /* another line the log must contain, or NULL */
};

static const struct lifecycle_case cases[] = {
    {.category = "positive",
     .description = "SIGTERM with no client",
     .expected = "exits 0 after one 'Shutdown complete', no sanitizer report",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "positive",
     .description = "SIGINT with no client",
     .expected = "the same clean shutdown as SIGTERM",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGINT,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "positive",
     .description = "nvmeof backend with a file-backed namespace, then SIGTERM",
     .expected = "a clean shutdown that also frees the parsed namespace path",
     .backend = "nvmeof:size=1M,file=@DIR@/ns.img",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    /* The interval is longer than EXIT_WAIT_MS, so a health-check thread
     * that sleeps it out instead of being woken fails the row. */
    {.category = "positive",
     .description = "TCP mesh manager with a 60 s health-check interval, then "
                    "SIGTERM",
     .expected = "exits 0 without waiting out the interval or a blocked "
                 "accept()",
     .backend = "tcp:manager:listen:@PORT@",
     .env_name = "ERNIC_TCP_HEALTH_INTERVAL",
     .env_value = "60",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "negative",
     .description = "a client connects and hangs up before negotiating",
     .expected = "logs the drop, keeps listening, then shuts down cleanly",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = HANG_UP,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 1,
     .want_shutdown = 1,
     .want_line = "No message of desired type"},
    {.category = "negative",
     .description = "a client sends a non-VERSION header and hangs up",
     .expected =
         "negotiation fails with EINVAL, logged as a drop; keeps listening",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = BAD_HDR,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 1,
     .want_shutdown = 1,
     .want_line = "Invalid argument"},
    {.category = "negative",
     .description = "an nvmeof namespace file that cannot be opened",
     .expected = "ionic init unwinds and the server exits non-zero, no "
                 "sanitizer report",
     .backend = "nvmeof:file=/nonexistent/rocm-ernic-ns.img",
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "nvmeof backend: cannot open"},
    {.category = "negative",
     .description = "TCP mesh worker whose manager accepts it and never "
                    "answers",
     .expected = "registration times out after 5 s, the backend unwinds its "
                 "threads, and the server exits non-zero",
     .backend = "tcp:worker:127.0.0.1:@PORT@",
     .fake_manager = true,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "Registration timeout"},
    {.category = "boundary",
     .description = "SIGTERM as soon as the socket appears",
     .expected = "a clean shutdown even before the attach loop is reached",
     .backend = "loopback",
     .want_socket = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "three clients hang up in a row",
     .expected = "three drops logged, still listening, then a clean shutdown",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = HANG_UP,
     .clients = 3,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 3,
     .want_shutdown = 1},
    {.category = "corner",
     .description = "a client connects and never speaks, then SIGTERM",
     .expected =
         "the signal interrupts the blocked negotiation: clean shutdown",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = SILENT,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "corner",
     .description = "SIGTERM sent twice",
     .expected = "still exits 0 with exactly one 'Shutdown complete'",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 2,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
};

static char *g_server;

/*
 * A command to run the server under, such as valgrind and its options, from
 * ERNIC_TEST_SERVER_WRAPPER.  It's split on whitespace, so no word of it can
 * contain a space.
 */
static char *g_wrapper[32];
static size_t g_wrapper_len;

/* Split ERNIC_TEST_SERVER_WRAPPER into g_wrapper.  False if it doesn't fit. */
static bool load_wrapper(void)
{
    const char *env = getenv("ERNIC_TEST_SERVER_WRAPPER");
    char *p;

    if (!env)
        return true;
    p = strdup(env);
    if (!p)
        return false;
    for (;;) {
        while (isspace((unsigned char)*p))
            *p++ = '\0';
        if (*p == '\0')
            return true;
        if (g_wrapper_len == sizeof(g_wrapper) / sizeof(g_wrapper[0]))
            return false;
        g_wrapper[g_wrapper_len++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p))
            p++;
    }
}

/*
 * The scratch directory.  It's always under /tmp, not $TMPDIR: the server's
 * socket goes in it, and a socket path has to fit the 108 bytes of sun_path.
 */
static char g_dir[] = "/tmp/ernic-lifecycle-XXXXXX";

/*
 * Copy src to dst with each "@DIR@" replaced by the scratch directory and
 * each "@PORT@" by port.  Returns false if the result doesn't fit.
 */
static bool expand_backend(const char *src, uint16_t port, char *dst,
                           size_t len)
{
    char port_str[8];
    size_t n = 0;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    while (*src) {
        const char *sub = NULL;
        size_t skip = 1;

        if (strncmp(src, "@DIR@", 5) == 0) {
            sub = g_dir;
            skip = 5;
        } else if (strncmp(src, "@PORT@", 6) == 0) {
            sub = port_str;
            skip = 6;
        }
        size_t sl = sub ? strlen(sub) : 1;

        if (sl >= len - n)
            return false;
        memcpy(dst + n, sub ? sub : src, sl);
        n += sl;
        src += skip;
    }
    dst[n] = '\0';
    return true;
}

static pid_t start_server(const struct lifecycle_case *c, char *backend,
                          char *sock, const char *log)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* dup2() clears close-on-exec on the copies, so the server still
         * inherits stdout and stderr.  With _FORTIFY_SOURCE, glibc 2.40 and
         * later give Clang an open(path, flags, mode) overload, and
         * android-cloexec-open looks for O_CLOEXEC in its mode */
        /* NOLINTBEGIN(android-cloexec-open) */
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        /* NOLINTEND(android-cloexec-open) */

        if (fd < 0)
            _exit(126);
        if (c->env_name && setenv(c->env_name, c->env_value, 1) < 0)
            _exit(126);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        char *args[sizeof(g_wrapper) / sizeof(g_wrapper[0]) + 6];
        size_t n = 0;

        for (size_t i = 0; i < g_wrapper_len; i++)
            args[n++] = g_wrapper[i];
        args[n++] = g_server;
        args[n++] = "--socket";
        args[n++] = sock;
        args[n++] = "--backend";
        args[n++] = backend;
        args[n] = NULL;
        execvp(args[0], args);
        _exit(127);
    }
    return pid;
}

static bool wait_for_socket(pid_t pid, const char *sock)
{
    int64_t deadline = now_ms() + SOCKET_WAIT_MS;

    while (now_ms() < deadline) {
        if (path_is_socket(sock))
            return true;
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return false;
        sleep_ms(10);
    }
    return false;
}

static bool exit_matches(int status, int want)
{
    if (!WIFEXITED(status))
        return false;
    if (want < 0)
        return WEXITSTATUS(status) != 0;
    return WEXITSTATUS(status) == want;
}

static void describe_status(int status, char *buf, size_t len)
{
    if (WIFEXITED(status))
        snprintf(buf, len, "exited %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, len, "killed by signal %d", WTERMSIG(status));
    else
        snprintf(buf, len, "status %#x", (unsigned)status);
}

static int run_case(size_t idx, const struct lifecycle_case *c)
{
    char sock[320], log[320], backend[512], why[480] = "", got[64] = "";
    int held[8], nheld = 0, status = 0, port_fd = -1;
    uint16_t port = 0;
    bool ok = true;
    pid_t pid;

    snprintf(sock, sizeof(sock), "%s/row%zu.sock", g_dir, idx);
    snprintf(log, sizeof(log), "%s/row%zu.log", g_dir, idx);
    unlink(sock);

    if (strstr(c->backend, "@PORT@")) {
        port_fd = listen_ephemeral(&port);
        if (port_fd < 0) {
            printf("FAIL %s: %s; no free TCP port\n", c->category,
                   c->description);
            return 1;
        }
        if (!c->fake_manager) {
            close(port_fd);
            port_fd = -1;
        }
    }
    if (!expand_backend(c->backend, port, backend, sizeof(backend))) {
        printf("FAIL %s: %s; backend string too long\n", c->category,
               c->description);
        if (port_fd >= 0)
            close(port_fd);
        return 1;
    }
    pid = start_server(c, backend, sock, log);
    if (pid < 0) {
        printf("FAIL %s: %s; could not fork\n", c->category, c->description);
        if (port_fd >= 0)
            close(port_fd);
        return 1;
    }

    if (c->want_socket && !wait_for_socket(pid, sock)) {
        snprintf(why, sizeof(why), "the socket never appeared");
        ok = false;
    }

    if (ok && c->settle && !wait_for_log(log, "waiting for client", 1)) {
        snprintf(why, sizeof(why), "never reached the attach loop");
        ok = false;
    }

    for (int i = 0; ok && i < c->clients; i++) {
        int fd = connect_unix(sock);

        if (fd < 0) {
            snprintf(why, sizeof(why), "client %d could not connect", i + 1);
            ok = false;
            break;
        }
        if (c->action == BAD_HDR && send_bad_header(fd) < 0) {
            snprintf(why, sizeof(why), "could not send the header");
            ok = false;
        }
        if (c->action == SILENT && nheld < (int)(sizeof(held) / sizeof(*held)))
            held[nheld++] = fd;
        else
            close(fd);
        if (ok && c->want_dropped > 0 &&
            !wait_for_log(log, DROPPED_LINE, i + 1)) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                describe_status(status, got, sizeof(got));
                snprintf(why, sizeof(why),
                         "the server %s instead of logging drop %d", got,
                         i + 1);
                pid = -1;
            } else {
                snprintf(why, sizeof(why), "drop %d was never logged", i + 1);
            }
            ok = false;
        }
    }
    if (ok && c->action == SILENT)
        sleep_ms(SETTLE_MS); /* let the server block in negotiation */

    /* Still serving: alive, and the socket still takes connections. */
    if (ok && c->want_socket && waitpid(pid, &status, WNOHANG) == pid) {
        describe_status(status, got, sizeof(got));
        snprintf(why, sizeof(why), "server already gone (%s)", got);
        ok = false;
        pid = -1;
    }
    if (ok && c->want_dropped > 0) {
        int fd = connect_unix(sock);

        if (fd < 0) {
            snprintf(why, sizeof(why), "the socket stopped accepting");
            ok = false;
        } else {
            close(fd);
            /* That probe is one more drop; wait for it to be logged so it
             * cannot race the signal below. */
            if (!wait_for_log(log, DROPPED_LINE, c->want_dropped + 1)) {
                snprintf(why, sizeof(why), "probe drop never logged");
                ok = false;
            }
        }
    }

    if (pid > 0) {
        for (int i = 0; i < c->sigs; i++) {
            kill(pid, c->sig);
            if (i + 1 < c->sigs)
                sleep_ms(20);
        }
        if (wait_exit(pid, &status) < 0) {
            snprintf(why, sizeof(why), "did not exit within %d ms",
                     EXIT_WAIT_MS);
            ok = false;
        }
        describe_status(status, got, sizeof(got));
    }
    for (int i = 0; i < nheld; i++)
        close(held[i]);
    if (port_fd >= 0)
        close(port_fd);

    if (ok && !exit_matches(status, c->want_exit)) {
        snprintf(why, sizeof(why), "%s (want %s)", got,
                 c->want_exit < 0 ? "non-zero" : "exit 0");
        ok = false;
    }
    if (ok && count_in_log(log, SHUTDOWN_LINE) != c->want_shutdown) {
        snprintf(why, sizeof(why), "%d '%s' line(s) (want %d)",
                 count_in_log(log, SHUTDOWN_LINE), SHUTDOWN_LINE,
                 c->want_shutdown);
        ok = false;
    }
    if (ok && c->want_line && count_in_log(log, c->want_line) == 0) {
        snprintf(why, sizeof(why), "log lacks '%s'", c->want_line);
        ok = false;
    }
    if (ok && sanitizer_reported(log)) {
        snprintf(why, sizeof(why), "sanitizer report in %s", log);
        ok = false;
    }

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     %s; server log: %s\n", why, log);
        return 1;
    }
    unlink(sock);
    unlink(log);
    return 0;
}

int main(int argc, char **argv)
{
    int failures = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <server binary>\n", argv[0]);
        return 2;
    }
    g_server = argv[1];
    if (access(g_server, X_OK) != 0) {
        printf("SKIP: server not found: %s\n", g_server);
        return 77;
    }
    if (!load_wrapper()) {
        fprintf(stderr, "ERNIC_TEST_SERVER_WRAPPER has too many words\n");
        return 2;
    }
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failures += run_case(i, &cases[i]);

    if (failures) {
        printf("\n%d check(s) failed; logs kept in %s\n", failures, g_dir);
        return 1;
    }
    {
        char ns[320];

        snprintf(ns, sizeof(ns), "%s/ns.img", g_dir);
        unlink(ns);
    }
    rmdir(g_dir);
    printf("\nall checks passed\n");
    return 0;
}
