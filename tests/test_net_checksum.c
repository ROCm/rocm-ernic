/*
 * Unit tests for tcp_checksum() / udp_checksum() in net_headers.h.
 *
 * Regression coverage for a NULL-pointer dereference: both helpers summed
 * the payload without checking it, so a NULL payload dereferenced NULL.
 * The payload buffers here are exact-size heap allocations, so when the
 * test is built with AddressSanitizer any over-read also fails the test.
 *
 * Table-driven: negative (NULL payload, zero length), positive (even and
 * odd payload lengths, exercising the odd-byte path), and a determinism
 * check that a NULL payload yields the same result as an empty buffer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_headers.h"

#define NULL_PAYLOAD ((size_t) - 1)

static void fill_ip(struct ip_header *ip)
{
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = 6; /* TCP; udp test overrides to 17 */
    ip->src_ip = htonl(0x0a000001);
    ip->dst_ip = htonl(0x0a000002);
}

static void fill_tcp(struct tcp_header *tcp)
{
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(1234);
    tcp->dst_port = htons(5678);
    tcp->data_off = (5 << 4); /* 20-byte header */
}

static void fill_udp(struct udp_header *udp, size_t payload_len)
{
    memset(udp, 0, sizeof(*udp));
    udp->src_port = htons(1234);
    udp->dst_port = htons(5678);
    udp->len = htons((uint16_t)(sizeof(struct udp_header) + payload_len));
}

/* Allocate an exact-size payload so ASan brackets any over-read. */
static uint8_t *make_payload(size_t len)
{
    uint8_t *p = malloc(len ? len : 1);
    if (p) {
        for (size_t i = 0; i < len; i++) {
            p[i] = (uint8_t)(0xA0 + (i & 0x0F));
        }
    }
    return p;
}

struct testcase {
    const char *name;
    size_t payload_len; /* NULL_PAYLOAD => pass a NULL pointer */
    int odd;            /* informational: exercises the odd-byte branch */
};

static int run_tcp(const struct testcase *tc)
{
    struct ip_header ip;
    struct tcp_header tcp;
    fill_ip(&ip);
    ip.protocol = 6;
    fill_tcp(&tcp);

    if (tc->payload_len == NULL_PAYLOAD) {
        /* Must not dereference NULL even with a nonzero length, and a NULL
         * payload must be treated as "no payload" regardless of the length
         * argument. */
        uint16_t a = tcp_checksum(&ip, &tcp, NULL, 8);
        uint16_t b = tcp_checksum(&ip, &tcp, NULL, 0);
        uint8_t empty[1] = {0};
        uint16_t c = tcp_checksum(&ip, &tcp, empty, 0);
        if (a != b) {
            printf("FAIL tcp %-18s: NULL len-8 != NULL len-0 %#x != %#x\n",
                   tc->name, (unsigned)a, (unsigned)b);
            return 1;
        }
        if (b != c) {
            printf("FAIL tcp %-18s: NULL/empty len-0 mismatch %#x != %#x\n",
                   tc->name, (unsigned)b, (unsigned)c);
            return 1;
        }
        return 0;
    }

    uint8_t *p = make_payload(tc->payload_len);
    if (!p) {
        printf("FAIL tcp %-18s: allocation failed\n", tc->name);
        return 1;
    }
    uint16_t r1 = tcp_checksum(&ip, &tcp, p, tc->payload_len);
    uint16_t r2 = tcp_checksum(&ip, &tcp, p, tc->payload_len);
    free(p);
    if (r1 != r2) {
        printf("FAIL tcp %-18s: not deterministic %#x != %#x\n", tc->name,
               (unsigned)r1, (unsigned)r2);
        return 1;
    }
    if (r1 == 0) {
        printf("FAIL tcp %-18s: checksum is zero (should be 0xFFFF form)\n",
               tc->name);
        return 1;
    }
    return 0;
}

static int run_udp(const struct testcase *tc)
{
    struct ip_header ip;
    struct udp_header udp;
    fill_ip(&ip);
    ip.protocol = 17;

    if (tc->payload_len == NULL_PAYLOAD) {
        fill_udp(&udp, 0);
        uint16_t a = udp_checksum(&ip, &udp, NULL, 8);
        uint16_t b = udp_checksum(&ip, &udp, NULL, 0);
        uint8_t empty[1] = {0};
        uint16_t c = udp_checksum(&ip, &udp, empty, 0);
        if (a != b) {
            printf("FAIL udp %-18s: NULL len-8 != NULL len-0 %#x != %#x\n",
                   tc->name, (unsigned)a, (unsigned)b);
            return 1;
        }
        if (b != c) {
            printf("FAIL udp %-18s: NULL/empty len-0 mismatch %#x != %#x\n",
                   tc->name, (unsigned)b, (unsigned)c);
            return 1;
        }
        return 0;
    }

    fill_udp(&udp, tc->payload_len);
    uint8_t *p = make_payload(tc->payload_len);
    if (!p) {
        printf("FAIL udp %-18s: allocation failed\n", tc->name);
        return 1;
    }
    uint16_t r1 = udp_checksum(&ip, &udp, p, tc->payload_len);
    uint16_t r2 = udp_checksum(&ip, &udp, p, tc->payload_len);
    free(p);
    if (r1 != r2) {
        printf("FAIL udp %-18s: not deterministic %#x != %#x\n", tc->name,
               (unsigned)r1, (unsigned)r2);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const struct testcase cases[] = {
        /* negative / regression */
        {"null-payload", NULL_PAYLOAD, 0},
        {"zero-length", 0, 0},
        /* positive: even and odd lengths (odd exercises the odd-byte path) */
        {"len-1-odd", 1, 1},
        {"len-2-even", 2, 0},
        {"len-3-odd", 3, 1},
        {"len-16-even", 16, 0},
        {"len-31-odd", 31, 1},
        {"len-64-even", 64, 0},
        {"len-255-odd", 255, 1},
    };

    int failures = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        failures += run_tcp(&cases[i]);
        failures += run_udp(&cases[i]);
    }

    if (failures) {
        printf("net_checksum: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("net_checksum: all %zu cases passed (tcp+udp)\n", n);
    return 0;
}
