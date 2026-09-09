/*
 * ionic_eth_net.c — host network backend for the emulated ionic NIC
 *
 * The emulated LIF needs somewhere to put the frames the guest transmits and
 * somewhere to get the frames it receives.  A Linux TAP interface is the
 * cheapest option that gives the guest a real, routable Ethernet segment:
 * the host end is an ordinary netdev, so ARP, ICMP, DHCP and TCP all work
 * against the host stack with no protocol emulation in this process.
 *
 * The fd is non-blocking and drained from the server's existing poll loop --
 * no thread is spawned, because every DMA into guest memory has to stay on
 * the thread that owns the vfio-user context.
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include "ionic_eth_net.h"

struct ionic_eth_net {
    int fd;
    char ifname[IFNAMSIZ];
};

struct ionic_eth_net *ionic_eth_net_open_tap(const char *ifname,
                                             char *out_ifname,
                                             size_t out_ifname_len)
{
    struct ionic_eth_net *net = calloc(1, sizeof(*net));
    if (!net)
        return NULL;

    net->fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (net->fd < 0) {
        free(net);
        return NULL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    /* IFF_NO_PI: we want raw frames, not the 4-byte tun_pi prefix. */
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ifname && *ifname)
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(net->fd, TUNSETIFF, &ifr) < 0) {
        int err = errno;
        close(net->fd);
        free(net);
        errno = err;
        return NULL;
    }

    if (fcntl(net->fd, F_SETFL, O_NONBLOCK) < 0) {
        int err = errno;
        close(net->fd);
        free(net);
        errno = err;
        return NULL;
    }

    memcpy(net->ifname, ifr.ifr_name, sizeof(net->ifname));
    net->ifname[IFNAMSIZ - 1] = '\0';

    if (out_ifname && out_ifname_len) {
        size_t n = strnlen(net->ifname, sizeof(net->ifname));
        if (n > out_ifname_len - 1)
            n = out_ifname_len - 1;
        memcpy(out_ifname, net->ifname, n);
        out_ifname[n] = '\0';
    }

    return net;
}

void ionic_eth_net_close(struct ionic_eth_net *net)
{
    if (!net)
        return;
    if (net->fd >= 0)
        close(net->fd);
    free(net);
}

int ionic_eth_net_send(struct ionic_eth_net *net, const void *frame, size_t len)
{
    if (!net || net->fd < 0)
        return -EBADF;

    for (;;) {
        ssize_t n = write(net->fd, frame, len);
        if (n >= 0)
            return (size_t)n == len ? 0 : -EIO;
        if (errno == EINTR)
            continue;
        /* A tap with no reader on the other end fills up; dropping is what
         * real hardware does with an oversubscribed link. */
        return -errno;
    }
}

ssize_t ionic_eth_net_recv(struct ionic_eth_net *net, void *buf, size_t cap)
{
    if (!net || net->fd < 0)
        return -EBADF;

    for (;;) {
        ssize_t n = read(net->fd, buf, cap);
        if (n > 0)
            return n;
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)
            return 0;
        return -errno;
    }
}
