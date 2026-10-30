/*
 * Unit tests for picking the TCP backend's role from its config string.
 *
 * tcp_init() used to pick manager or worker mode with strstr(), taking any
 * config that contained "manager:" anywhere for a manager's.  A worker whose
 * manager's host name ends in "manager", such as
 * "tcp:worker:mesh-manager:5000", was then parsed as a manager config and
 * init failed, saying a manager config must start with "manager:".
 * tcp_config_role() now matches the role only at the start, after an
 * optional "tcp:".
 *
 * The cases cover both forms the config comes in (the server passes what
 * follows "--backend tcp:", the tests the whole string), host names that
 * contain the other role's name, and strings that name no role, which
 * tcp_init() must refuse before it sets anything up.
 *
 * The address after the role is "host:port", with an IPv6 address in
 * brackets.  The parser used to split it at every ':' and ignore what came
 * after the port, so "worker:fe80::1:9000" was taken for host "fe80" and port
 * 1.  The cases check that such an address is refused, and that the mesh's
 * sockets take both families: the listen socket accepts IPv4 and IPv6
 * connections, and reports an IPv4 peer's address as a.b.c.d, not as the
 * ::ffff:a.b.c.d an IPv6 socket sees.
 *
 * The test links the TCP backend and reaches its helpers through
 * rdma_backend_tcp_priv.h; none of this needs a thread, so the rest of the
 * backend's externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "pvrdma/rdma_backend_defs.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_tcp_priv.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 * Only error_report() is reachable, from tcp_init_with_interval() refusing
 * a config; it keeps the format so the test can check why.  The rest exist
 * only to satisfy the linker.
 */
static const char *last_error;

void error_report(const char *fmt, ...)
{
    last_error = fmt;
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

static int failures;

#define CHECK(cond, ...)          \
    do {                          \
        if (!(cond)) {            \
            printf("    FAIL: "); \
            printf(__VA_ARGS__);  \
            printf("\n");         \
            failures++;           \
        }                         \
    } while (0)

static const char *mode_name(TcpMode mode)
{
    return mode == TCP_MODE_MANAGER ? "manager" : "worker";
}

/* ---- Cases ------------------------------------------------------------- */

static const struct role_case {
    const char *config;
    TcpMode mode;
    const char *args; /* what follows the role */
} role_cases[] = {
    {"tcp:manager:listen:5000", TCP_MODE_MANAGER, "listen:5000"},
    {"manager:listen:5000", TCP_MODE_MANAGER, "listen:5000"},
    {"tcp:manager:10.0.0.1:5000", TCP_MODE_MANAGER, "10.0.0.1:5000"},
    {"tcp:worker:10.0.0.1:5000", TCP_MODE_WORKER, "10.0.0.1:5000"},
    {"worker:10.0.0.1:5000", TCP_MODE_WORKER, "10.0.0.1:5000"},
    /* The regression: a manager's host name ending in "manager" */
    {"tcp:worker:mesh-manager:5000", TCP_MODE_WORKER, "mesh-manager:5000"},
    {"worker:mesh-manager:5000", TCP_MODE_WORKER, "mesh-manager:5000"},
    {"tcp:worker:manager:5000", TCP_MODE_WORKER, "manager:5000"},
    /* and the other way round, which the old order happened to get right */
    {"tcp:manager:worker:5000", TCP_MODE_MANAGER, "worker:5000"},
    /* The role is matched, not the rest; the parsers judge that */
    {"manager:", TCP_MODE_MANAGER, ""},
};

/* Strings that name no role */
static const char *const no_role_cases[] = {
    "",
    "tcp",
    "tcp:",
    "manager",
    "worker",
    "tcp:managers:5000",
    "tcp:Manager:5000",
    "tcp:tcp:manager:5000",
    " manager:5000",
    "x:manager:5000",
    "tcp:x-worker:5000",
    "loopback",
};

static void test_role_is_the_prefix(void)
{
    printf("  the role is read from the prefix\n");

    for (size_t i = 0; i < sizeof(role_cases) / sizeof(role_cases[0]); i++) {
        const struct role_case *c = &role_cases[i];
        /* Start from the wrong mode, so a role that isn't set shows */
        TcpMode mode =
            c->mode == TCP_MODE_MANAGER ? TCP_MODE_WORKER : TCP_MODE_MANAGER;
        const char *args = tcp_config_role(c->config, &mode);

        if (!args) {
            CHECK(false, "\"%s\" named no role", c->config);
            continue;
        }
        CHECK(mode == c->mode, "\"%s\" read as a %s, not a %s", c->config,
              mode_name(mode), mode_name(c->mode));
        CHECK(strcmp(args, c->args) == 0, "\"%s\" left \"%s\", not \"%s\"",
              c->config, args, c->args);
    }
}

static void test_no_role_is_refused(void)
{
    printf("  a config that names no role is refused\n");

    for (size_t i = 0; i < sizeof(no_role_cases) / sizeof(no_role_cases[0]);
         i++) {
        const char *config = no_role_cases[i];

        for (int m = 0; m < 2; m++) {
            TcpMode before = m ? TCP_MODE_WORKER : TCP_MODE_MANAGER;
            TcpMode mode = before;
            const char *args = tcp_config_role(config, &mode);

            CHECK(!args, "\"%s\" named a role, leaving \"%s\"", config,
                  args ? args : "");
            CHECK(mode == before, "\"%s\" changed the mode to %s", config,
                  mode_name(mode));
        }
    }
}

/* tcp_init() refuses such a config before setting anything up: with this
 * test's stubs, anything it started would be left behind. */
static void test_init_refuses_no_role(void)
{
    printf("  tcp_init() refuses a config that names no role\n");

    for (size_t i = 0; i < sizeof(no_role_cases) / sizeof(no_role_cases[0]);
         i++) {
        const char *config = no_role_cases[i];
        RdmaBackendDev dev;
        int ret;

        memset(&dev, 0, sizeof(dev));
        last_error = NULL;
        ret = tcp_init_with_interval(&dev, config, 1);

        CHECK(ret == -EINVAL, "tcp_init(\"%s\") returned %d, not -EINVAL",
              config, ret);
        CHECK(last_error && strstr(last_error, "Invalid config"),
              "tcp_init(\"%s\") didn't report an invalid config", config);
        CHECK(!dev.backend_private, "tcp_init(\"%s\") left private data",
              config);
    }
}

/* ---- Addresses --------------------------------------------------------- */

static const struct host_port_case {
    const char *s;
    const char *host;
    uint16_t port;
} host_port_cases[] = {
    {"10.0.0.1:5000", "10.0.0.1", 5000},
    {"mesh-manager:5000", "mesh-manager", 5000},
    {"localhost:1", "localhost", 1},
    {"h:65535", "h", 65535},
    {"[::1]:5000", "::1", 5000},
    {"[fe80::1%eth0]:9000", "fe80::1%eth0", 9000},
    {"[::ffff:10.0.0.1]:6320", "::ffff:10.0.0.1", 6320},
};

/* Each is refused, whatever the reason */
static const char *const bad_host_port_cases[] = {
    "",
    "host",
    "host:",
    ":5000",
    "host:0",
    "host:65536",
    "host:-1",
    "host: 5000",
    "host:5000x",
    "host:5000:6000",
    /* The regression: an IPv6 address without brackets */
    "fe80::1:9000",
    "::1:9000",
    "::1",
    "[::1]",
    "[::1]:",
    "[::1]5000",
    "[::1]:5000:6000",
    "[::1:5000",
    "[]:5000",
    "[host]:5000",
    "[10.0.0.1]:5000",
    "[[::1]]:5000",
    "[::[1]:5000",
    "a]b:5000",
    "a[b:5000",
    "host]:5000",
};

static void test_host_port_is_parsed(void)
{
    printf("  host:port and [IPv6]:port are parsed\n");

    for (size_t i = 0; i < sizeof(host_port_cases) / sizeof(host_port_cases[0]);
         i++) {
        const struct host_port_case *c = &host_port_cases[i];
        char *host = NULL;
        uint16_t port = 0;
        int ret = tcp_parse_host_port(c->s, "test", &host, &port);

        CHECK(ret == 0, "\"%s\" was refused (%d)", c->s, ret);
        CHECK(host && strcmp(host, c->host) == 0,
              "\"%s\" gave host \"%s\", not \"%s\"", c->s, host ? host : "",
              c->host);
        CHECK(port == c->port, "\"%s\" gave port %u, not %u", c->s, port,
              c->port);
        g_free(host);
    }
}

static void test_bad_host_port_is_refused(void)
{
    printf("  a malformed host:port is refused\n");

    for (size_t i = 0;
         i < sizeof(bad_host_port_cases) / sizeof(bad_host_port_cases[0]);
         i++) {
        const char *s = bad_host_port_cases[i];
        char *host = NULL;
        uint16_t port = 7;
        int ret;

        last_error = NULL;
        ret = tcp_parse_host_port(s, "test", &host, &port);

        CHECK(ret == -EINVAL, "\"%s\" returned %d, not -EINVAL", s, ret);
        CHECK(!host, "\"%s\" set host \"%s\"", s, host ? host : "");
        CHECK(port == 7, "\"%s\" set port %u", s, port);
        CHECK(last_error && strstr(last_error, "invalid address"),
              "\"%s\" didn't report an invalid address", s);
        g_free(host);
    }
}

/*
 * A manager's host goes in its topology entry, whose field holds
 * TCP_HOSTNAME_LEN - 1 characters and a NUL.  A longer host was cut short.
 */
static void test_long_host(void)
{
    char s[TCP_HOSTNAME_LEN + 16];

    printf("  a host that fills the mesh's hostname field is the longest "
           "taken\n");

    for (size_t len = TCP_HOSTNAME_LEN - 1; len <= TCP_HOSTNAME_LEN; len++) {
        bool fits = len < TCP_HOSTNAME_LEN;
        char *host = NULL;
        uint16_t port = 7;
        int ret;

        memset(s, 'h', len);
        snprintf(s + len, sizeof(s) - len, ":5000");
        last_error = NULL;
        ret = tcp_parse_host_port(s, "test", &host, &port);

        if (fits) {
            CHECK(ret == 0, "a %zu-character host was refused (%d)", len, ret);
            CHECK(host && strlen(host) == len && strspn(host, "h") == len,
                  "a %zu-character host wasn't kept whole", len);
            CHECK(port == 5000, "a %zu-character host gave port %u", len, port);
        } else {
            CHECK(ret == -EINVAL, "a %zu-character host returned %d", len, ret);
            CHECK(!host, "a %zu-character host was set", len);
            CHECK(last_error && strstr(last_error, "invalid address"),
                  "a %zu-character host didn't report an invalid address", len);
        }
        g_free(host);
    }
}

/* Configs whose role is fine and whose address isn't.  tcp_init() refuses
 * them before it listens or connects. */
static const char *const bad_address_configs[] = {
    "tcp:worker:fe80::1:9000",
    "tcp:worker:10.0.0.1:5000:6000",
    "tcp:worker:10.0.0.1",
    "tcp:worker:[::1]",
    "tcp:manager:fe80::1:9000",
    "tcp:manager:10.0.0.1:5000:6000",
    "tcp:manager:listen",
    "tcp:manager:listen:",
    "tcp:manager:listen:0",
    "tcp:manager:listen:5000:6000",
    "tcp:manager:listen:[::1]:5000",
};

static void test_init_refuses_bad_address(void)
{
    printf("  tcp_init() refuses a malformed address\n");

    for (size_t i = 0;
         i < sizeof(bad_address_configs) / sizeof(bad_address_configs[0]);
         i++) {
        const char *config = bad_address_configs[i];
        RdmaBackendDev dev;
        int ret;

        memset(&dev, 0, sizeof(dev));
        last_error = NULL;
        ret = tcp_init_with_interval(&dev, config, 1);

        CHECK(ret < 0, "tcp_init(\"%s\") returned %d", config, ret);
        CHECK(last_error && (strstr(last_error, "invalid address") ||
                             strstr(last_error, "invalid port")),
              "tcp_init(\"%s\") didn't report the address", config);
        CHECK(!dev.backend_private, "tcp_init(\"%s\") left private data",
              config);
    }
}

static void check_peer_addr(const struct sockaddr_storage *ss, const char *what,
                            const char *want_host, uint16_t want_port)
{
    char host[INET6_ADDRSTRLEN];
    uint16_t port = 0;

    if (!tcp_format_peer_addr(ss, host, sizeof(host), &port)) {
        CHECK(false, "%s wasn't formatted: %s", what, strerror(errno));
        return;
    }
    CHECK(strcmp(host, want_host) == 0,
          "%s was formatted as \"%s\", not \"%s\"", what, host, want_host);
    CHECK(port == want_port, "%s had port %u, not %u", what, port, want_port);
}

static void test_peer_addr_is_formatted(void)
{
    struct sockaddr_storage ss;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
    char host[INET6_ADDRSTRLEN];
    uint16_t port = 7;

    printf("  a peer's address is formatted\n");

    memset(&ss, 0, sizeof(ss));
    sin->sin_family = AF_INET;
    sin->sin_port = htons(5000);
    CHECK(inet_pton(AF_INET, "10.1.2.3", &sin->sin_addr) == 1, "inet_pton");
    check_peer_addr(&ss, "an IPv4 address", "10.1.2.3", 5000);

    /* Too small for the address */
    CHECK(!tcp_format_peer_addr(&ss, host, 8, &port),
          "10.1.2.3 fit in 8 bytes");

    memset(&ss, 0, sizeof(ss));
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(6320);
    CHECK(inet_pton(AF_INET6, "fe80::1", &sin6->sin6_addr) == 1, "inet_pton");
    check_peer_addr(&ss, "an IPv6 address", "fe80::1", 6320);

    CHECK(inet_pton(AF_INET6, "::ffff:10.1.2.3", &sin6->sin6_addr) == 1,
          "inet_pton");
    check_peer_addr(&ss, "an IPv4-mapped address", "10.1.2.3", 6320);

    memset(&ss, 0, sizeof(ss));
    ss.ss_family = AF_UNIX;
    errno = 0;
    CHECK(!tcp_format_peer_addr(&ss, host, sizeof(host), &port),
          "an AF_UNIX address was formatted");
    CHECK(errno == EAFNOSUPPORT, "an AF_UNIX address set errno %d", errno);
}

/* Whether this host can use ::1, which a container without IPv6 lacks */
static bool have_ipv6_loopback(void)
{
    struct sockaddr_in6 sin6;
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool ok;

    if (fd < 0) {
        return false;
    }
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_addr = in6addr_loopback;
    ok = bind(fd, (struct sockaddr *)&sin6, sizeof(sin6)) == 0;
    close(fd);
    return ok;
}

/*
 * Connect to host at port with tcp_connect_to_remote(), accept the connection
 * on listen_fd, and check the address the accept reports: want_host, and the
 * client socket's port.
 */
static void check_accepts(int listen_fd, const char *host, uint16_t port,
                          const char *want_host)
{
    struct sockaddr_storage peer;
    struct sockaddr_storage local;
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    char local_host[INET6_ADDRSTRLEN];
    uint16_t local_port = 0;
    int client;
    int fd;

    client = tcp_connect_to_remote(host, port, NULL, 0);
    if (client < 0) {
        CHECK(false, "couldn't connect to %s port %u", host, port);
        return;
    }
    fd = ernic_gnu_accept4(listen_fd, (struct sockaddr *)&peer, &peer_len, 0);
    if (fd < 0) {
        CHECK(false, "accepting from %s failed: %s", host, strerror(errno));
        close(client);
        return;
    }
    CHECK(getsockname(client, (struct sockaddr *)&local, &local_len) == 0 &&
              tcp_format_peer_addr(&local, local_host, sizeof(local_host),
                                   &local_port),
          "couldn't read the client's port");
    check_peer_addr(&peer, host, want_host, local_port);
    close(fd);
    close(client);
}

static void test_listen_takes_both_families(void)
{
    uint16_t port = 0;
    int fd;

    printf("  the listen socket takes IPv4 and IPv6\n");

    fd = tcp_listen_socket(AF_INET6, &port);
    if (fd < 0 && errno == EAFNOSUPPORT) {
        printf("    this host has no IPv6; the IPv4 socket is checked below\n");
        return;
    }
    if (fd < 0) {
        CHECK(false, "couldn't listen on IPv6: %s", strerror(errno));
        return;
    }
    CHECK(port != 0, "the kernel's port wasn't returned");

    check_accepts(fd, "127.0.0.1", port, "127.0.0.1");
    if (have_ipv6_loopback()) {
        check_accepts(fd, "::1", port, "::1");
    } else {
        printf("    this host has no ::1; only IPv4 was checked\n");
    }
    close(fd);
}

static void test_listen_ipv4_only(void)
{
    uint16_t port = 0;
    int fd;

    printf("  the IPv4 listen socket, for hosts without IPv6\n");

    fd = tcp_listen_socket(AF_INET, &port);
    if (fd < 0) {
        CHECK(false, "couldn't listen on IPv4: %s", strerror(errno));
        return;
    }
    CHECK(port != 0, "the kernel's port wasn't returned");
    check_accepts(fd, "127.0.0.1", port, "127.0.0.1");
    close(fd);
}

/* A dual-stack socket can't take a port an IPv4 socket is listening on */
static void test_listen_port_in_use(void)
{
    uint16_t port = 0;
    uint16_t again;
    int held;
    int fd;

    printf("  a port in use is refused, and *port left alone\n");

    held = tcp_listen_socket(AF_INET, &port);
    if (held < 0) {
        CHECK(false, "couldn't listen on IPv4: %s", strerror(errno));
        return;
    }

    for (int family = AF_INET; family != 0;
         family = family == AF_INET ? AF_INET6 : 0) {
        again = port;
        last_error = NULL;
        errno = 0;
        fd = tcp_listen_socket(family, &again);
        if (fd < 0 && errno == EAFNOSUPPORT) {
            continue; /* no IPv6 */
        }
        CHECK(fd < 0, "family %d took port %u twice", family, port);
        CHECK(errno == EADDRINUSE, "family %d set errno %d, not EADDRINUSE",
              family, errno);
        CHECK(again == port, "family %d changed the port to %u", family, again);
        CHECK(last_error && strstr(last_error, "listen socket"),
              "family %d didn't report the failure", family);
        if (fd >= 0) {
            close(fd);
        }
    }
    close(held);
}

static void test_listen_unknown_family(void)
{
    uint16_t port = 0;
    int fd;

    printf("  a family other than IPv4 and IPv6 is refused\n");

    errno = 0;
    fd = tcp_listen_socket(AF_UNIX, &port);
    CHECK(fd < 0, "AF_UNIX gave a listen socket");
    CHECK(errno == EAFNOSUPPORT, "AF_UNIX set errno %d", errno);
    if (fd >= 0) {
        close(fd);
    }
}

int main(void)
{
    printf("tcp config unit tests\n");

    test_role_is_the_prefix();
    test_no_role_is_refused();
    test_init_refuses_no_role();
    test_host_port_is_parsed();
    test_bad_host_port_is_refused();
    test_long_host();
    test_init_refuses_bad_address();
    test_peer_addr_is_formatted();
    test_listen_takes_both_families();
    test_listen_ipv4_only();
    test_listen_port_in_use();
    test_listen_unknown_family();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
