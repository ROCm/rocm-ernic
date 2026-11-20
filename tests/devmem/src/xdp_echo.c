/*
 * XDP programs for the devmem TX matrix: exercise the ionic XDP transmit
 * paths, which share the Tx descriptor-info buffer slots with skbs.
 *
 * UDP datagrams to port 7777 are echoed (MACs, addresses and ports
 * swapped; the UDP checksum is unchanged by the swap) with
 *   -DACT_TX        XDP_TX              (page-pool mapping, not unmapped)
 *   -DACT_REDIRECT  bpf_redirect(ingress ifindex): ndo_xdp_xmit, i.e. the
 *                   XDP_REDIRECT branch that maps and later unmaps
 *   neither         XDP_PASS for everything
 * -DFRAGS puts the program in "xdp.frags" (multi-buffer, for jumbo MTU).
 * No libbpf headers needed:
 *   clang -O2 -g -target bpf -I/usr/include/x86_64-linux-gnu -c xdp_echo.c
 *
 * A BPF object, not C11 userspace: it needs the section attribute and the
 * compiler's builtins (there is no libc), which the rest of the tree does
 * not use.  See tests/devmem/README.md.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#define SEC(n) __attribute__((section(n), used))
#ifdef ACT_REDIRECT
static long (*bpf_redirect)(__u32 ifindex, __u64 flags) = (void *)23;
#endif

#ifdef FRAGS
SEC("xdp.frags")
#else
SEC("xdp")
#endif
int xdp_echo(struct xdp_md *ctx)
{
#if defined(ACT_TX) || defined(ACT_REDIRECT)
    void *end = (void *)(long)ctx->data_end;
    struct ethhdr *eth = (void *)(long)ctx->data;
    struct iphdr *ip = (void *)(eth + 1);
    struct udphdr *udp = (void *)(ip + 1);
    unsigned char mac[ETH_ALEN];
    __be32 a;
    __be16 p;

    if ((void *)(udp + 1) > end)
        return XDP_PASS;
    if (eth->h_proto != __builtin_bswap16(ETH_P_IP) || ip->ihl != 5 ||
        ip->protocol != IPPROTO_UDP || udp->dest != __builtin_bswap16(7777))
        return XDP_PASS;
    __builtin_memcpy(mac, eth->h_dest, ETH_ALEN);
    __builtin_memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
    __builtin_memcpy(eth->h_source, mac, ETH_ALEN);
    a = ip->saddr;
    ip->saddr = ip->daddr;
    ip->daddr = a;
    p = udp->source;
    udp->source = udp->dest;
    udp->dest = p;
#ifdef ACT_TX
    return XDP_TX;
#else
    return bpf_redirect(ctx->ingress_ifindex, 0);
#endif
#else
    return XDP_PASS;
#endif
}

char _license[] SEC("license") = "GPL";
