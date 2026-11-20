/*
 * pcapcap IFACE FILE SECONDS [TCPPORT]: capture every frame seen on IFACE
 * (AF_PACKET, promiscuous) into a pcap file for SECONDS or until SIGTERM.
 * With TCPPORT only IPv4/IPv6 TCP frames with that source or destination
 * port are kept. Static, for running in a container's network namespace.
 *
 * Build, from the top of the repository:
 *   gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -O2 -Wall -Wextra -static -Isrc -o pcapcap tests/devmem/src/pcapcap.c
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "parse_int.h"

static volatile sig_atomic_t stop;
static void on_sig(int s)
{
    (void)s;
    stop = 1;
}

static int want(const uint8_t *p, int n, int port)
{
    int off = 14, proto;

    if (!port)
        return 1;
    if (n < 34)
        return 0;
    uint16_t et = p[12] << 8 | p[13];

    if (et == 0x8100) {
        et = p[16] << 8 | p[17];
        off = 18;
    }
    if (et == 0x0800) {
        proto = p[off + 9];
        off += (p[off] & 15) * 4;
    } else if (et == 0x86dd) {
        proto = p[off + 6];
        off += 40;
    } else
        return 0;
    if (proto != 6 || n < off + 4)
        return 0;
    return (p[off] << 8 | p[off + 1]) == port ||
           (p[off + 2] << 8 | p[off + 3]) == port;
}

int main(int argc, char **argv)
{
    static uint8_t buf[65536];
    struct sockaddr_ll sll = {.sll_family = AF_PACKET,
                              .sll_protocol = htons(ETH_P_ALL)};
    struct packet_mreq mr = {.mr_type = PACKET_MR_PROMISC};
    uint32_t hdr[6] = {0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
    unsigned long kept = 0, seen = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: pcapcap IFACE FILE SECONDS [TCPPORT]\n");
        return 2;
    }
    int port = 0, secs;

    if (!ernic_parse_int(argv[3], &secs) ||
        (argc > 4 && !ernic_parse_int(argv[4], &port))) {
        fprintf(stderr, "pcapcap: bad number\n");
        return 2;
    }
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    FILE *f = fopen(argv[2], "w");

    if (fd < 0 || !f) {
        perror("socket/file");
        return 1;
    }
    sll.sll_ifindex = mr.mr_ifindex = if_nametoindex(argv[1]);
    if (!sll.sll_ifindex || bind(fd, (struct sockaddr *)&sll, sizeof(sll))) {
        perror("bind");
        return 1;
    }
    setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));
    int rcv = 64 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof(rcv));
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    fwrite(hdr, sizeof(hdr), 1, f);
    fprintf(stderr, "pcapcap: capturing on %s port %d\n", argv[1], port);
    time_t end = time(NULL) + secs;

    while (!stop && time(NULL) < end) {
        struct pollfd p = {.fd = fd, .events = POLLIN};

        if (poll(&p, 1, 200) <= 0)
            continue;
        int n = recv(fd, buf, sizeof(buf), MSG_TRUNC);

        if (n <= 0)
            continue;
        seen++;
        if (n > (int)sizeof(buf))
            n = sizeof(buf);
        if (!want(buf, n, port))
            continue;
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        uint32_t ph[4] = {ts.tv_sec, ts.tv_nsec / 1000, n, n};

        fwrite(ph, sizeof(ph), 1, f);
        fwrite(buf, n, 1, f);
        kept++;
    }
    fclose(f);
    fprintf(stderr, "pcapcap: seen %lu frames, kept %lu\n", seen, kept);
    return 0;
}
