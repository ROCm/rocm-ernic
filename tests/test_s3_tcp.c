/*
 * test_s3_tcp.c — unit tests for the in-band TCP endpoint
 *
 * The endpoint terminates TCP on the emulated wire, so the only way to
 * reach it is with Ethernet frames. This test builds them by hand and
 * dissects what comes back, with its own packed headers and its own
 * checksum routine, so a header layout or checksum bug in s3_tcp.c
 * cannot hide behind the same bug in the test.
 *
 * The cases are the ones a guest actually produces: ARP for the target,
 * ping it, open a connection, send a request in several segments because
 * the guest's stack chose to, read the answer back, reuse the connection,
 * and close it. Plus the paths a guest produces when something is wrong
 * -- a segment for a connection that does not exist, more connections
 * than there are slots, an idle connection that has to be reaped.
 *
 * Failures print "FAIL <case>: <detail>" and main() returns the count, so
 * a regression names itself rather than just tripping an assert.
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3_http.h"
#include "s3_target.h"
#include "s3_tcp.h"

#define TARGET_IP   0xc0a8c801u /* 192.168.200.1 */
#define TARGET_PORT 9000
#define GUEST_IP    0xc0a8c809u /* 192.168.200.9 */
#define GUEST_PORT  44001

#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

static const uint8_t GUEST_MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t BCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static int failures;

static void fail(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void fail(const char *name, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("FAIL %s: ", name);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

static void ok(const char *name)
{
    printf("ok   %s\n", name);
}

/* ---- Wire formats, independently declared ------------------------------- */

struct t_eth {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct t_arp {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t op;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct t_ip {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct t_tcp {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_off;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct t_icmp {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

#define ETH_LEN ((size_t)sizeof(struct t_eth))
#define IP_LEN  ((size_t)sizeof(struct t_ip))
#define TCP_LEN ((size_t)sizeof(struct t_tcp))

/* One's-complement sum, written out rather than shared with the code
 * under test: a checksum bug has to be visible from one side or the
 * other, not cancel itself out. */
static uint16_t ones_sum(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = data;
    uint32_t sum = seed;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t pseudo_seed(uint32_t src, uint32_t dst, uint8_t proto,
                            uint16_t len)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto +
           len;
}

/* ---- Frame capture ------------------------------------------------------ */

#define CAP_MAX 64

struct capture {
    struct {
        uint8_t buf[1514];
        size_t len;
    } f[CAP_MAX];
    unsigned n;
    unsigned dropped;
};

static void cap_tx(void *ctx, const void *frame, size_t len)
{
    struct capture *c = ctx;

    if (c->n >= CAP_MAX || len > sizeof(c->f[0].buf)) {
        c->dropped++;
        return;
    }
    memcpy(c->f[c->n].buf, frame, len);
    c->f[c->n].len = len;
    c->n++;
}

static void cap_reset(struct capture *c)
{
    c->n = 0;
    c->dropped = 0;
}

/* ---- Frame builders ----------------------------------------------------- */

static size_t build_arp_request(uint8_t *out, uint32_t target_ip)
{
    struct t_eth *eth = (struct t_eth *)out;
    struct t_arp *arp = (struct t_arp *)(out + ETH_LEN);

    memcpy(eth->dst, BCAST_MAC, 6);
    memcpy(eth->src, GUEST_MAC, 6);
    eth->type = htons(0x0806);

    memset(arp, 0, sizeof(*arp));
    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->op = htons(1);
    memcpy(arp->sender_mac, GUEST_MAC, 6);
    arp->sender_ip = htonl(GUEST_IP);
    arp->target_ip = htonl(target_ip);

    return ETH_LEN + sizeof(*arp);
}

static struct t_ip *build_ip(uint8_t *out, const uint8_t *dst_mac,
                             uint8_t proto, size_t payload_len)
{
    struct t_eth *eth = (struct t_eth *)out;
    struct t_ip *ip = (struct t_ip *)(out + ETH_LEN);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, GUEST_MAC, 6);
    eth->type = htons(0x0800);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->total_len = htons((uint16_t)(IP_LEN + payload_len));
    ip->id = htons(0x1234);
    ip->frag_off = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = proto;
    ip->src = htonl(GUEST_IP);
    ip->dst = htonl(TARGET_IP);
    ip->checksum = htons(ones_sum(ip, IP_LEN, 0));

    return ip;
}

static size_t build_ping(uint8_t *out, const uint8_t *dst_mac, uint16_t id,
                         uint16_t seq, const char *data)
{
    size_t dlen = strlen(data);
    size_t icmp_len = sizeof(struct t_icmp) + dlen;

    build_ip(out, dst_mac, 1, icmp_len);
    struct t_icmp *icmp = (struct t_icmp *)(out + ETH_LEN + IP_LEN);
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = 8; /* echo request */
    icmp->id = htons(id);
    icmp->seq = htons(seq);
    memcpy((uint8_t *)icmp + sizeof(*icmp), data, dlen);
    icmp->checksum = htons(ones_sum(icmp, icmp_len, 0));

    return ETH_LEN + IP_LEN + icmp_len;
}

static size_t build_tcp(uint8_t *out, const uint8_t *dst_mac, uint16_t sport,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        const void *payload, size_t payload_len)
{
    build_ip(out, dst_mac, 6, TCP_LEN + payload_len);
    struct t_tcp *tcp = (struct t_tcp *)(out + ETH_LEN + IP_LEN);

    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(sport);
    tcp->dst_port = htons(TARGET_PORT);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->data_off = 5 << 4;
    tcp->flags = flags;
    tcp->window = htons(32768);
    if (payload_len)
        memcpy((uint8_t *)tcp + TCP_LEN, payload, payload_len);

    uint32_t seed =
        pseudo_seed(GUEST_IP, TARGET_IP, 6, (uint16_t)(TCP_LEN + payload_len));
    tcp->checksum = htons(ones_sum(tcp, TCP_LEN + payload_len, seed));

    return ETH_LEN + IP_LEN + TCP_LEN + payload_len;
}

/* ---- Frame dissection --------------------------------------------------- */

static const struct t_tcp *as_tcp(const uint8_t *f, size_t len,
                                  size_t *payload_len)
{
    const struct t_eth *eth = (const struct t_eth *)f;

    if (len < ETH_LEN + IP_LEN + TCP_LEN || ntohs(eth->type) != 0x0800)
        return NULL;
    const struct t_ip *ip = (const struct t_ip *)(f + ETH_LEN);
    if (ip->protocol != 6)
        return NULL;

    size_t total = ntohs(ip->total_len);
    const struct t_tcp *tcp = (const struct t_tcp *)(f + ETH_LEN + IP_LEN);
    size_t hdrlen = (size_t)((tcp->data_off >> 4) & 0xf) * 4;
    if (payload_len != NULL)
        *payload_len = total - IP_LEN - hdrlen;
    return tcp;
}

static const uint8_t *tcp_payload(const uint8_t *f)
{
    const struct t_tcp *tcp = (const struct t_tcp *)(f + ETH_LEN + IP_LEN);
    size_t hdrlen = (size_t)((tcp->data_off >> 4) & 0xf) * 4;

    return f + ETH_LEN + IP_LEN + hdrlen;
}

/* Recompute both checksums the way a receiving NIC would. */
static bool checksums_ok(const char *name, const uint8_t *f, size_t len)
{
    const struct t_eth *eth = (const struct t_eth *)f;

    if (len < ETH_LEN || ntohs(eth->type) != 0x0800)
        return true; /* ARP carries no checksum */

    const struct t_ip *ip = (const struct t_ip *)(f + ETH_LEN);
    if (ones_sum(ip, IP_LEN, 0) != 0) {
        fail(name, "IP checksum is wrong");
        return false;
    }

    size_t total = ntohs(ip->total_len);
    if (ETH_LEN + total > len) {
        fail(name, "IP total_len %zu overruns the %zu byte frame", total, len);
        return false;
    }
    size_t l4len = total - IP_LEN;
    const uint8_t *l4 = f + ETH_LEN + IP_LEN;

    if (ip->protocol == 6) {
        uint32_t seed =
            pseudo_seed(ntohl(ip->src), ntohl(ip->dst), 6, (uint16_t)l4len);
        if (ones_sum(l4, l4len, seed) != 0) {
            fail(name, "TCP checksum is wrong");
            return false;
        }
    } else if (ip->protocol == 1) {
        if (ones_sum(l4, l4len, 0) != 0) {
            fail(name, "ICMP checksum is wrong");
            return false;
        }
    }
    return true;
}

static void check_all_checksums(const char *name, struct capture *cap)
{
    for (unsigned i = 0; i < cap->n; i++)
        if (!checksums_ok(name, cap->f[i].buf, cap->f[i].len))
            return;
}

/* ---- Fixture ------------------------------------------------------------ */

struct fixture {
    struct s3_target *target;
    struct s3_tcp *tcp;
    struct capture cap;
    uint8_t mac[6];
    uint8_t frame[2048];
};

static bool fixture_up(struct fixture *fx, const char *name, unsigned max_conns)
{
    struct s3_target_cfg tcfg;
    struct s3_tcp_cfg ncfg;
    char err[128] = "";

    memset(fx, 0, sizeof(*fx));
    s3_target_cfg_defaults(&tcfg);
    fx->target = s3_target_create(&tcfg, err, sizeof(err));
    if (fx->target == NULL) {
        fail(name, "target create: %s", err);
        return false;
    }

    s3_tcp_cfg_from_target(&ncfg, fx->target);
    if (max_conns)
        ncfg.max_conns = max_conns;
    memcpy(fx->mac, ncfg.mac, 6);

    fx->tcp = s3_tcp_create(&ncfg, fx->target, NULL, NULL, cap_tx, &fx->cap,
                            err, sizeof(err));
    if (fx->tcp == NULL) {
        fail(name, "tcp create: %s", err);
        s3_target_destroy(fx->target);
        return false;
    }
    return true;
}

static void fixture_down(struct fixture *fx)
{
    s3_tcp_destroy(fx->tcp);
    s3_target_destroy(fx->target);
}

/*
 * Three-way handshake.  Returns the target's initial sequence number
 * plus one, i.e. the sequence its first data byte will carry.
 */
static bool handshake(struct fixture *fx, const char *name, uint16_t sport,
                      uint32_t client_iss, uint32_t *srv_seq, uint64_t now)
{
    cap_reset(&fx->cap);
    size_t n =
        build_tcp(fx->frame, fx->mac, sport, client_iss, 0, FLAG_SYN, NULL, 0);
    if (!s3_tcp_rx_frame(fx->tcp, fx->frame, n, now)) {
        fail(name, "the SYN was not accepted as ours");
        return false;
    }
    if (fx->cap.n != 1) {
        fail(name, "a SYN produced %u frames, want 1", fx->cap.n);
        return false;
    }
    if (!checksums_ok(name, fx->cap.f[0].buf, fx->cap.f[0].len))
        return false;

    const struct t_tcp *tcp = as_tcp(fx->cap.f[0].buf, fx->cap.f[0].len, NULL);
    if (tcp == NULL ||
        (tcp->flags & (FLAG_SYN | FLAG_ACK)) != (FLAG_SYN | FLAG_ACK)) {
        fail(name, "the answer to a SYN was not a SYN|ACK (flags 0x%02x)",
             tcp ? tcp->flags : 0);
        return false;
    }
    if (ntohl(tcp->ack) != client_iss + 1) {
        fail(name, "SYN|ACK acks %u, want %u", ntohl(tcp->ack), client_iss + 1);
        return false;
    }
    if (ntohs(tcp->src_port) != TARGET_PORT) {
        fail(name, "SYN|ACK came from port %u", ntohs(tcp->src_port));
        return false;
    }
    *srv_seq = ntohl(tcp->seq) + 1;

    cap_reset(&fx->cap);
    n = build_tcp(fx->frame, fx->mac, sport, client_iss + 1, *srv_seq, FLAG_ACK,
                  NULL, 0);
    s3_tcp_rx_frame(fx->tcp, fx->frame, n, now);
    if (fx->cap.n != 0) {
        fail(name, "the handshake ACK drew %u frames, want none", fx->cap.n);
        return false;
    }
    return true;
}

/* Gather the payload of every captured data segment, in order. */
static size_t collect(struct capture *cap, char *out, size_t outlen)
{
    size_t at = 0;

    for (unsigned i = 0; i < cap->n; i++) {
        size_t plen = 0;
        if (as_tcp(cap->f[i].buf, cap->f[i].len, &plen) == NULL || plen == 0)
            continue;
        if (at + plen >= outlen)
            break;
        memcpy(out + at, tcp_payload(cap->f[i].buf), plen);
        at += plen;
    }
    out[at] = '\0';
    return at;
}

/* ---- Tests -------------------------------------------------------------- */

static void test_arp(void)
{
    const char *name = "arp";
    struct fixture fx;

    if (!fixture_up(&fx, name, 0))
        return;

    size_t n = build_arp_request(fx.frame, TARGET_IP);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0)) {
        fail(name, "an ARP request for our address was not consumed");
        goto out;
    }
    if (fx.cap.n != 1) {
        fail(name, "ARP produced %u frames, want 1", fx.cap.n);
        goto out;
    }

    const uint8_t *f = fx.cap.f[0].buf;
    const struct t_eth *eth = (const struct t_eth *)f;
    const struct t_arp *arp = (const struct t_arp *)(f + ETH_LEN);

    if (ntohs(eth->type) != 0x0806)
        fail(name, "reply ethertype 0x%04x", ntohs(eth->type));
    if (memcmp(eth->dst, GUEST_MAC, 6) != 0)
        fail(name, "reply was not unicast back to the requester");
    if (memcmp(eth->src, fx.mac, 6) != 0)
        fail(name, "reply source MAC is not the endpoint's");
    if (ntohs(arp->op) != 2)
        fail(name, "ARP op %u, want 2 (reply)", ntohs(arp->op));
    if (ntohl(arp->sender_ip) != TARGET_IP)
        fail(name, "reply sender IP 0x%08x", ntohl(arp->sender_ip));
    if (memcmp(arp->sender_mac, fx.mac, 6) != 0)
        fail(name, "reply advertises the wrong MAC for our address");
    if (ntohl(arp->target_ip) != GUEST_IP)
        fail(name, "reply target IP 0x%08x", ntohl(arp->target_ip));
    if (fx.cap.f[0].len < 60)
        fail(name, "reply is %zu bytes, short of the Ethernet minimum",
             fx.cap.f[0].len);

    /* An ARP for somebody else stays on the wire. */
    cap_reset(&fx.cap);
    n = build_arp_request(fx.frame, GUEST_IP + 1);
    if (s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0))
        fail(name, "consumed an ARP request for another address");
    if (fx.cap.n != 0)
        fail(name, "answered an ARP request for another address");

    ok(name);
out:
    fixture_down(&fx);
}

static void test_ping(void)
{
    const char *name = "ping";
    struct fixture fx;
    static const char payload[] = "abcdefghijklmnopqrstuvwxyz012345";

    if (!fixture_up(&fx, name, 0))
        return;

    size_t n = build_ping(fx.frame, fx.mac, 0x4242, 7, payload);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0)) {
        fail(name, "an echo request was not consumed");
        goto out;
    }
    if (fx.cap.n != 1) {
        fail(name, "ping produced %u frames, want 1", fx.cap.n);
        goto out;
    }
    if (!checksums_ok(name, fx.cap.f[0].buf, fx.cap.f[0].len))
        goto out;

    const uint8_t *f = fx.cap.f[0].buf;
    const struct t_ip *ip = (const struct t_ip *)(f + ETH_LEN);
    const struct t_icmp *icmp = (const struct t_icmp *)(f + ETH_LEN + IP_LEN);

    if (ip->protocol != 1)
        fail(name, "reply protocol %u, want ICMP", ip->protocol);
    if (ntohl(ip->dst) != GUEST_IP)
        fail(name, "reply addressed to 0x%08x", ntohl(ip->dst));
    if (icmp->type != 0)
        fail(name, "ICMP type %u, want 0 (echo reply)", icmp->type);
    if (ntohs(icmp->id) != 0x4242 || ntohs(icmp->seq) != 7)
        fail(name, "reply id/seq %u/%u do not match the request",
             ntohs(icmp->id), ntohs(icmp->seq));
    if (memcmp(f + ETH_LEN + IP_LEN + sizeof(*icmp), payload,
               strlen(payload)) != 0)
        fail(name, "the echo payload did not come back intact");

    ok(name);
out:
    fixture_down(&fx);
}

static void test_request_response(void)
{
    const char *name = "request-response";
    struct fixture fx;
    uint32_t srv = 0;
    char body[8192];

    if (!fixture_up(&fx, name, 0))
        return;

    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    static const char req[] = "GET / HTTP/1.1\r\nHost: 192.168.200.1\r\n\r\n";
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                         FLAG_ACK | FLAG_PSH, req, strlen(req));
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20)) {
        fail(name, "the request segment was not consumed");
        goto out;
    }
    check_all_checksums(name, &fx.cap);

    size_t got = collect(&fx.cap, body, sizeof(body));
    if (got == 0) {
        fail(name, "no response data was sent");
        goto out;
    }
    if (strncmp(body, "HTTP/1.1 200 OK\r\n", 17) != 0)
        fail(name, "response does not start with a 200: %.32s", body);
    if (strstr(body, "<Name>ernic</Name>") == NULL)
        fail(name, "response body is not the bucket listing");
    if (strstr(body, "Connection: keep-alive") == NULL)
        fail(name, "a keep-alive request got a closing response");

    /* Every data segment must be sequenced from the server's ISS with no
     * gaps, or the guest's stack silently stalls waiting for the hole. */
    uint32_t expect = srv;
    for (unsigned i = 0; i < fx.cap.n; i++) {
        size_t plen = 0;
        const struct t_tcp *tcp =
            as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, &plen);
        if (tcp == NULL || plen == 0)
            continue;
        if (ntohl(tcp->seq) != expect) {
            fail(name, "segment %u has seq %u, want %u", i, ntohl(tcp->seq),
                 expect);
            break;
        }
        expect += (uint32_t)plen;
    }

    /* No FIN while a keep-alive response is still outstanding. */
    for (unsigned i = 0; i < fx.cap.n; i++) {
        const struct t_tcp *tcp =
            as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp->flags & FLAG_FIN))
            fail(name, "the endpoint closed a keep-alive connection");
    }

    /* Acknowledge it, then send a second request down the same
     * connection: keep-alive is what a client library relies on. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, (uint32_t)(1001 + strlen(req)),
                  srv + (uint32_t)got, FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 30);

    static const char req2[] =
        "GET /ernic HTTP/1.1\r\nConnection: close\r\n\r\n";
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, (uint32_t)(1001 + strlen(req)),
                  srv + (uint32_t)got, FLAG_ACK | FLAG_PSH, req2, strlen(req2));
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 40);
    check_all_checksums(name, &fx.cap);

    size_t got2 = collect(&fx.cap, body, sizeof(body));
    if (got2 == 0) {
        fail(name, "the second request on the connection went unanswered");
        goto out;
    }
    if (strstr(body, "<ListBucketResult") == NULL)
        fail(name, "second response is not a bucket listing: %.64s", body);
    if (strstr(body, "Connection: close") == NULL)
        fail(name, "Connection: close was not honoured in the response");

    /*
     * The FIN follows the data, but only once the data has been
     * acknowledged -- an unacknowledged response may still need
     * retransmitting, and a FIN sent over the top of it would retire the
     * connection with bytes still in flight.
     */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT,
                  (uint32_t)(1001 + strlen(req) + strlen(req2)),
                  srv + (uint32_t)got + (uint32_t)got2, FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 50);

    bool fin = false;
    for (unsigned i = 0; i < fx.cap.n; i++) {
        const struct t_tcp *tcp =
            as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp->flags & FLAG_FIN))
            fin = true;
    }
    if (!fin)
        fail(name, "no FIN after the closing response was acknowledged");

    ok(name);
out:
    fixture_down(&fx);
}

/* The guest's stack decides where the segment boundaries fall, and it
 * will happily split a request line in half. */
static void test_split_request(void)
{
    const char *name = "split-request";
    struct fixture fx;
    uint32_t srv = 0;
    char body[8192];

    if (!fixture_up(&fx, name, 0))
        return;

    if (!handshake(&fx, name, GUEST_PORT, 5000, &srv, 10))
        goto out;

    static const char req[] = "PUT /ernic/split HTTP/1.1\r\n"
                              "Host: 192.168.200.1\r\n"
                              "Content-Length: 11\r\n"
                              "\r\n"
                              "hello world";
    size_t total = strlen(req);
    uint32_t seq = 5001;

    cap_reset(&fx.cap);
    for (size_t at = 0; at < total; at += 7) {
        size_t chunk = total - at < 7 ? total - at : 7;
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK,
                             req + at, chunk);
        if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20)) {
            fail(name, "a mid-request segment was not consumed");
            goto out;
        }
        seq += (uint32_t)chunk;
    }
    check_all_checksums(name, &fx.cap);

    size_t got = collect(&fx.cap, body, sizeof(body));
    if (got == 0) {
        fail(name, "the reassembled request went unanswered");
        goto out;
    }
    if (strncmp(body, "HTTP/1.1 200 OK\r\n", 17) != 0)
        fail(name, "reassembled PUT answered %.32s", body);

    /* The object has to be there, with the right bytes: a reassembly bug
     * that drops or duplicates a chunk shows up here and nowhere else. */
    if (s3_target_object_count(fx.target) != 1)
        fail(name, "the PUT did not create an object");
    if (s3_target_bytes_used(fx.target) != 11)
        fail(name, "stored %llu bytes, want 11",
             (unsigned long long)s3_target_bytes_used(fx.target));

    /* A duplicate segment must be discarded, not appended. */
    cap_reset(&fx.cap);
    static const char dup[] = "GET /ernic/split HTTP/1.1\r\n\r\n";
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK, dup,
                         strlen(dup));
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 30);
    size_t first = collect(&fx.cap, body, sizeof(body));

    cap_reset(&fx.cap);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 31); /* same segment again */
    size_t again = collect(&fx.cap, body, sizeof(body));
    if (first == 0)
        fail(name, "the GET went unanswered");
    if (again != 0)
        fail(name, "a retransmitted segment was served a second time");

    ok(name);
out:
    fixture_down(&fx);
}

static void test_stray_segment(void)
{
    const char *name = "stray-segment";
    struct fixture fx;

    if (!fixture_up(&fx, name, 0))
        return;

    /* Data for a connection that does not exist earns a reset. */
    size_t n = build_tcp(fx.frame, fx.mac, 40000, 777, 888, FLAG_ACK, "x", 1);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0)) {
        fail(name, "a segment for our port was not consumed");
        goto out;
    }
    if (fx.cap.n != 1) {
        fail(name, "a stray segment produced %u frames, want 1", fx.cap.n);
        goto out;
    }
    const struct t_tcp *tcp = as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL);
    if (tcp == NULL || !(tcp->flags & FLAG_RST))
        fail(name, "a stray segment was not reset (flags 0x%02x)",
             tcp ? tcp->flags : 0);
    checksums_ok(name, fx.cap.f[0].buf, fx.cap.f[0].len);

    /* Another port on the same wire is not ours. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, 40001, 1, 0, FLAG_SYN, NULL, 0);
    struct t_tcp *t = (struct t_tcp *)(fx.frame + ETH_LEN + IP_LEN);
    t->dst_port = htons(TARGET_PORT + 1);
    if (s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0))
        fail(name, "consumed a segment for somebody else's port");
    if (fx.cap.n != 0)
        fail(name, "answered a segment for somebody else's port");

    /* And neither is a frame addressed to another MAC. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, 40002, 1, 0, FLAG_SYN, NULL, 0);
    struct t_eth *eth = (struct t_eth *)fx.frame;
    eth->dst[5] ^= 0xff;
    if (s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0))
        fail(name, "consumed a frame addressed to another station");

    ok(name);
out:
    fixture_down(&fx);
}

/* More connections than there are slots: the extra ones are refused
 * outright rather than left half-open. */
static void test_conn_limit(void)
{
    const char *name = "conn-limit";
    struct fixture fx;
    const unsigned slots = 2;

    if (!fixture_up(&fx, name, slots))
        return;

    for (unsigned i = 0; i < slots; i++) {
        uint32_t srv = 0;
        if (!handshake(&fx, name, (uint16_t)(50000 + i), 100 * (i + 1), &srv,
                       10))
            goto out;
    }
    if (s3_tcp_conn_count(fx.tcp) != slots)
        fail(name, "%u connections open, want %u", s3_tcp_conn_count(fx.tcp),
             slots);

    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, 50099, 9000, 0, FLAG_SYN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 10);
    if (fx.cap.n != 1) {
        fail(name, "the refused SYN produced %u frames, want 1", fx.cap.n);
        goto out;
    }
    const struct t_tcp *tcp = as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL);
    if (tcp == NULL || !(tcp->flags & FLAG_RST))
        fail(name, "a SYN with no slot free was not reset");
    if (s3_tcp_conn_count(fx.tcp) != slots)
        fail(name, "the refused SYN still consumed a slot");

    const struct s3_tcp_stats *st = s3_tcp_statistics(fx.tcp);
    if (st->refused != 1)
        fail(name, "refused counter is %llu, want 1",
             (unsigned long long)st->refused);
    if (st->accepts != slots)
        fail(name, "accepts counter is %llu, want %u",
             (unsigned long long)st->accepts, slots);

    ok(name);
out:
    fixture_down(&fx);
}

/* A connection whose peer has vanished must not hold its slot forever. */
static void test_idle_reap(void)
{
    const char *name = "idle-reap";
    struct fixture fx;
    uint32_t srv = 0;

    if (!fixture_up(&fx, name, 0))
        return;

    if (!handshake(&fx, name, GUEST_PORT, 1, &srv, 1000))
        goto out;
    if (s3_tcp_conn_count(fx.tcp) != 1) {
        fail(name, "the handshake did not open a connection");
        goto out;
    }

    cap_reset(&fx.cap);
    if (s3_tcp_poll(fx.tcp, 2000))
        fail(name, "an idle connection was disturbed after one second");
    if (s3_tcp_conn_count(fx.tcp) != 1)
        fail(name, "a connection idle for one second was reaped");

    cap_reset(&fx.cap);
    s3_tcp_poll(fx.tcp, 1000 + 120 * 1000);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "a connection idle for two minutes was not reaped");
    if (fx.cap.n == 0)
        fail(name, "the reaped connection was not reset");

    ok(name);
out:
    fixture_down(&fx);
}

/* A request larger than the parser will ever accept has to be answered
 * and closed, not buffered until the emulator runs out of memory. */
static void test_oversized_request(void)
{
    const char *name = "oversized-request";
    struct fixture fx;
    uint32_t srv = 0;
    char body[8192];
    char *junk = malloc(1024);

    if (junk == NULL)
        return;
    memset(junk, 'A', 1024);

    if (!fixture_up(&fx, name, 0)) {
        free(junk);
        return;
    }
    if (!handshake(&fx, name, GUEST_PORT, 1, &srv, 10))
        goto out;

    uint32_t seq = 2;
    bool answered = false;
    for (unsigned i = 0; i < S3_HTTP_REQUEST_MAX / 1024 + 4; i++) {
        cap_reset(&fx.cap);
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK,
                             junk, 1024);
        s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20 + i);
        seq += 1024;
        if (collect(&fx.cap, body, sizeof(body)) > 0) {
            answered = true;
            break;
        }
    }
    if (!answered)
        fail(name, "an unbounded request was buffered instead of refused");
    else if (strncmp(body, "HTTP/1.1 413", 12) != 0 &&
             strncmp(body, "HTTP/1.1 400", 12) != 0)
        fail(name, "oversized request answered %.16s, want 413 or 400", body);

    ok(name);
out:
    free(junk);
    fixture_down(&fx);
}

int main(void)
{
    test_arp();
    test_ping();
    test_request_response();
    test_split_request();
    test_stray_segment();
    test_conn_limit();
    test_idle_reap();
    test_oversized_request();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 tcp tests passed\n");
    return failures;
}
