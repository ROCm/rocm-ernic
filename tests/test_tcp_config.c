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
 * The test links rdma_backend_tcp.c and reaches its helpers through
 * rdma_backend_tcp_priv.h; choosing a role needs no thread or socket, so the
 * rest of the backend's externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_tcp_priv.h"
#include "rdma_backend_defs.h"

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
int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg)
{
    (void)thread;
    (void)start_routine;
    (void)arg;
    return 0;
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

int main(void)
{
    printf("tcp config unit tests\n");

    test_role_is_the_prefix();
    test_no_role_is_refused();
    test_init_refuses_no_role();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
