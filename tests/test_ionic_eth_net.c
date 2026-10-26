/*
 * Unit tests for the TAP backend's interface name check (ionic_eth_net.c).
 *
 * A name longer than the kernel's interface names must be refused rather
 * than cut short, because the shortened name may be another interface. The
 * check runs before /dev/net/tun is opened, so these rows don't depend on
 * the device or on privileges.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ionic/ionic_eth_net.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* Fill buf with len copies of 'a' and terminate it. */
static void make_name(char *buf, size_t len)
{
    memset(buf, 'a', len);
    buf[len] = '\0';
}

static void test_overlong_name_refused(size_t len)
{
    char name[128];
    char out[64];
    char what[96];

    make_name(name, len);
    memset(out, 'x', sizeof(out));
    out[sizeof(out) - 1] = '\0';

    errno = 0;
    struct ionic_eth_net *net = ionic_eth_net_open_tap(name, out, sizeof(out));
    (void)snprintf(what, sizeof(what), "%zu-character name is refused", len);
    check(net == NULL, what);
    (void)snprintf(what, sizeof(what), "%zu-character name sets ENAMETOOLONG",
                   len);
    check(errno == ENAMETOOLONG, what);
    (void)snprintf(what, sizeof(what),
                   "%zu-character name leaves out_ifname alone", len);
    check(out[0] == 'x', what);
    ionic_eth_net_close(net);
}

/*
 * A name of exactly IONIC_ETH_NET_IFNAME_MAX characters passes the check.
 * Whether the attach then succeeds depends on /dev/net/tun and
 * CAP_NET_ADMIN, so only the length error is ruled out; if it does succeed,
 * the TAP isn't persistent and goes away on close.
 */
static void test_longest_name_accepted(void)
{
    char name[IONIC_ETH_NET_IFNAME_MAX + 1];
    char out[64];

    make_name(name, IONIC_ETH_NET_IFNAME_MAX);
    errno = 0;
    struct ionic_eth_net *net = ionic_eth_net_open_tap(name, out, sizeof(out));
    if (net) {
        check(strcmp(out, name) == 0, "longest name is attached as given");
        ionic_eth_net_close(net);
    } else {
        check(errno != ENAMETOOLONG, "longest name is not too long");
    }
}

int main(void)
{
    test_overlong_name_refused(IONIC_ETH_NET_IFNAME_MAX + 1);
    test_overlong_name_refused(100);
    test_longest_name_accepted();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ionic-eth-net-unit: all tests passed\n");
    return 0;
}
