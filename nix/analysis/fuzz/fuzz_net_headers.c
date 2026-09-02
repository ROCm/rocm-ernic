/*
 * libFuzzer harness for the net_headers.h parse + checksum helpers.
 *
 * These static-inline helpers parse Ethernet/IP/TCP/UDP headers out of a
 * raw frame and compute checksums over the payload. They are the untrusted
 * frame-parsing primitives used by the TCP-mesh path. The harness walks the
 * full eth -> ip -> {tcp,udp} chain and then runs the checksum routines over
 * the derived payload — the same sequence clang-analyzer flagged in
 * tcp_checksum() (net_headers.h:308).
 *
 * All inputs point directly into an exact-size heap buffer, so ASan flags
 * any read past the frame the parsers/checksums perform.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "net_headers.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *frame = malloc(size ? size : 1);
    if (!frame) {
        return 0;
    }
    memcpy(frame, data, size);

    struct eth_header *eth = NULL;
    if (parse_eth_header(frame, size, &eth)) {
        /* IP checksum over the (attacker-sized) IP header. */
        struct ip_header *ip = NULL;
        if (parse_ip_header(frame, size, eth, &ip)) {
            size_t ihl = (size_t)(ip->version_ihl & 0x0F) * 4;
            size_t ip_off = sizeof(struct eth_header);
            if (ip_off + ihl <= size && ihl >= sizeof(struct ip_header)) {
                (void)ip_checksum(ip, ihl);
            }

            struct tcp_header *tcp = NULL;
            size_t tcp_off = 0;
            if (parse_tcp_header(frame, size, ip, &tcp, &tcp_off)) {
                size_t payload_off = get_tcp_payload_offset(tcp, tcp_off);
                if (payload_off <= size) {
                    (void)tcp_checksum(ip, tcp, frame + payload_off,
                                       size - payload_off);
                }
            }

            struct udp_header *udp = NULL;
            size_t udp_off = 0;
            if (parse_udp_header(frame, size, ip, &udp, &udp_off)) {
                size_t payload_off = get_udp_payload_offset(udp_off);
                if (payload_off <= size) {
                    (void)udp_checksum(ip, udp, frame + payload_off,
                                       size - payload_off);
                }
            }
        }
    }

    free(frame);
    return 0;
}
