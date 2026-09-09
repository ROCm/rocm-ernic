/*
 * ionic_eth_net.h — host network backend for the emulated ionic NIC
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ETH_NET_H
#define IONIC_ETH_NET_H

#include <stddef.h>
#include <sys/types.h>

/* Largest frame we will move in either direction (jumbo + VLAN + slack). */
#define IONIC_ETH_NET_MTU_MAX 9600

struct ionic_eth_net;

/*
 * Attach to a Linux TAP interface.  @ifname may name an existing persistent
 * tap (created with "ip tuntap add dev NAME mode tap user USER"), which is the
 * only form that works without CAP_NET_ADMIN.  On success the interface name
 * actually assigned is copied into @out_ifname.
 *
 * Returns NULL and sets errno on failure.
 */
struct ionic_eth_net *ionic_eth_net_open_tap(const char *ifname,
                                             char *out_ifname,
                                             size_t out_ifname_len);

void ionic_eth_net_close(struct ionic_eth_net *net);

/* Transmit one Ethernet frame.  Returns 0 on success, negative errno else. */
int ionic_eth_net_send(struct ionic_eth_net *net, const void *frame,
                       size_t len);

/*
 * Receive one Ethernet frame without blocking.  Returns the frame length,
 * 0 when nothing is pending, or a negative errno.
 */
ssize_t ionic_eth_net_recv(struct ionic_eth_net *net, void *buf, size_t cap);

#endif /* IONIC_ETH_NET_H */
