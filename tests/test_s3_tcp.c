/*
 * test_s3_tcp.c — unit tests for the in-band TCP endpoint
 *
 * The endpoint terminates TCP on the emulated wire, so the only way to
 * reach it is with Ethernet frames. This test builds them by hand and
 * dissects what comes back, with its own header layouts and its own
 * checksum routine, so a header layout or checksum bug in s3_tcp.c
 * cannot hide behind the same bug in the test.
 *
 * The cases are the ones a guest actually produces: ARP for the target,
 * ping it, open a connection, send a request in several segments because
 * the guest's stack chose to, read the answer back, reuse the connection,
 * and close it, with the MSS and window it advertises. Plus the paths a
 * guest produces when something is wrong -- a segment for a connection
 * that does not exist, more connections than there are slots, an idle
 * connection that has to be reaped, a reset, segments that are lost and
 * have to be retransmitted, and frames malformed at every layer.
 *
 * Failures print "FAIL <case>: <detail>" and main() returns the count, so
 * a regression names itself rather than just tripping an assert.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3_alloc_fail.h"
#include "s3_http.h"
#include "s3_target.h"
#include "s3_tcp.h"
#include "s3_token.h"

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

/* Each header's length and its fields' byte offsets.  Multi-byte fields are
 * big-endian; get_be() and put_be() read and write them. */

#define ETH_LEN    ((size_t)14)
#define T_ETH_DST  0
#define T_ETH_SRC  6
#define T_ETH_TYPE 12

#define ARP_LEN          ((size_t)28)
#define T_ARP_HW_TYPE    0
#define T_ARP_PROTO_TYPE 2
#define T_ARP_HW_LEN     4
#define T_ARP_PROTO_LEN  5
#define T_ARP_OP         6
#define T_ARP_SENDER_MAC 8
#define T_ARP_SENDER_IP  14
#define T_ARP_TARGET_MAC 18
#define T_ARP_TARGET_IP  24

#define IP_LEN           ((size_t)20)
#define T_IP_VERSION_IHL 0
#define T_IP_TOTAL_LEN   2
#define T_IP_ID          4
#define T_IP_FRAG        6
#define T_IP_TTL         8
#define T_IP_PROTOCOL    9
#define T_IP_CHECKSUM    10
#define T_IP_SRC         12
#define T_IP_DST         16

#define TCP_LEN        ((size_t)20)
#define T_TCP_SRC_PORT 0
#define T_TCP_DST_PORT 2
#define T_TCP_SEQ      4
#define T_TCP_ACK      8
#define T_TCP_DATA_OFF 12
#define T_TCP_FLAGS    13
#define T_TCP_WINDOW   14
#define T_TCP_CHECKSUM 16

#define ICMP_LEN        ((size_t)8)
#define T_ICMP_TYPE     0
#define T_ICMP_CHECKSUM 2
#define T_ICMP_ID       4
#define T_ICMP_SEQ      6

/* Store @value big-endian in @width bytes at @at. */
static void put_be(uint8_t *at, unsigned width, uint32_t value)
{
    for (unsigned i = 0; i < width; i++)
        at[i] = (uint8_t)(value >> (8 * (width - 1 - i)));
}

/* The big-endian value in @width bytes at @at. */
static uint32_t get_be(const uint8_t *at, unsigned width)
{
    uint32_t value = 0;

    for (unsigned i = 0; i < width; i++)
        value = (value << 8) | at[i];
    return value;
}

static uint8_t tcp_flags(const uint8_t *tcp)
{
    return tcp[T_TCP_FLAGS];
}

static uint32_t tcp_seq(const uint8_t *tcp)
{
    return get_be(tcp + T_TCP_SEQ, 4);
}

static uint32_t tcp_ack(const uint8_t *tcp)
{
    return get_be(tcp + T_TCP_ACK, 4);
}

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
    uint8_t *arp = out + ETH_LEN;

    memcpy(out + T_ETH_DST, BCAST_MAC, 6);
    memcpy(out + T_ETH_SRC, GUEST_MAC, 6);
    put_be(out + T_ETH_TYPE, 2, 0x0806);

    memset(arp, 0, ARP_LEN);
    put_be(arp + T_ARP_HW_TYPE, 2, 1);
    put_be(arp + T_ARP_PROTO_TYPE, 2, 0x0800);
    arp[T_ARP_HW_LEN] = 6;
    arp[T_ARP_PROTO_LEN] = 4;
    put_be(arp + T_ARP_OP, 2, 1);
    memcpy(arp + T_ARP_SENDER_MAC, GUEST_MAC, 6);
    put_be(arp + T_ARP_SENDER_IP, 4, GUEST_IP);
    put_be(arp + T_ARP_TARGET_IP, 4, target_ip);

    return ETH_LEN + ARP_LEN;
}

static void build_ip(uint8_t *out, const uint8_t *dst_mac, uint8_t proto,
                     size_t payload_len)
{
    uint8_t *ip = out + ETH_LEN;

    memcpy(out + T_ETH_DST, dst_mac, 6);
    memcpy(out + T_ETH_SRC, GUEST_MAC, 6);
    put_be(out + T_ETH_TYPE, 2, 0x0800);

    memset(ip, 0, IP_LEN);
    ip[T_IP_VERSION_IHL] = 0x45;
    put_be(ip + T_IP_TOTAL_LEN, 2, (uint32_t)(IP_LEN + payload_len));
    put_be(ip + T_IP_ID, 2, 0x1234);
    put_be(ip + T_IP_FRAG, 2, 0x4000);
    ip[T_IP_TTL] = 64;
    ip[T_IP_PROTOCOL] = proto;
    put_be(ip + T_IP_SRC, 4, GUEST_IP);
    put_be(ip + T_IP_DST, 4, TARGET_IP);
    put_be(ip + T_IP_CHECKSUM, 2, ones_sum(ip, IP_LEN, 0));
}

static size_t build_ping(uint8_t *out, const uint8_t *dst_mac, uint16_t id,
                         uint16_t seq, const char *data)
{
    size_t dlen = strlen(data);
    size_t icmp_len = ICMP_LEN + dlen;
    uint8_t *icmp = out + ETH_LEN + IP_LEN;

    build_ip(out, dst_mac, 1, icmp_len);
    memset(icmp, 0, ICMP_LEN);
    icmp[T_ICMP_TYPE] = 8; /* echo request */
    put_be(icmp + T_ICMP_ID, 2, id);
    put_be(icmp + T_ICMP_SEQ, 2, seq);
    memcpy(icmp + ICMP_LEN, data, dlen);
    put_be(icmp + T_ICMP_CHECKSUM, 2, ones_sum(icmp, icmp_len, 0));

    return ETH_LEN + IP_LEN + icmp_len;
}

/* A segment with an advertised @window and @optlen bytes of TCP options,
 * which must be a multiple of four. */
static size_t build_tcp_ex(uint8_t *out, const uint8_t *dst_mac, uint16_t sport,
                           uint32_t seq, uint32_t ack, uint8_t flags,
                           uint16_t window, const uint8_t *opt, size_t optlen,
                           const void *payload, size_t payload_len)
{
    size_t hdrlen = TCP_LEN + optlen;
    uint8_t *tcp = out + ETH_LEN + IP_LEN;

    build_ip(out, dst_mac, 6, hdrlen + payload_len);
    memset(tcp, 0, TCP_LEN);
    put_be(tcp + T_TCP_SRC_PORT, 2, sport);
    put_be(tcp + T_TCP_DST_PORT, 2, TARGET_PORT);
    put_be(tcp + T_TCP_SEQ, 4, seq);
    put_be(tcp + T_TCP_ACK, 4, ack);
    tcp[T_TCP_DATA_OFF] = (uint8_t)((hdrlen / 4) << 4);
    tcp[T_TCP_FLAGS] = flags;
    put_be(tcp + T_TCP_WINDOW, 2, window);
    if (optlen)
        memcpy(tcp + TCP_LEN, opt, optlen);
    if (payload_len)
        memcpy(tcp + hdrlen, payload, payload_len);

    uint32_t seed =
        pseudo_seed(GUEST_IP, TARGET_IP, 6, (uint16_t)(hdrlen + payload_len));
    put_be(tcp + T_TCP_CHECKSUM, 2, ones_sum(tcp, hdrlen + payload_len, seed));

    return ETH_LEN + IP_LEN + hdrlen + payload_len;
}

static size_t build_tcp(uint8_t *out, const uint8_t *dst_mac, uint16_t sport,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        const void *payload, size_t payload_len)
{
    return build_tcp_ex(out, dst_mac, sport, seq, ack, flags, 32768, NULL, 0,
                        payload, payload_len);
}

/* ---- Frame dissection --------------------------------------------------- */

/* The TCP header in frame @f, or NULL if @f is not a TCP segment. */
static const uint8_t *as_tcp(const uint8_t *f, size_t len, size_t *payload_len)
{
    if (len < ETH_LEN + IP_LEN + TCP_LEN || get_be(f + T_ETH_TYPE, 2) != 0x0800)
        return NULL;
    const uint8_t *ip = f + ETH_LEN;
    if (ip[T_IP_PROTOCOL] != 6)
        return NULL;

    size_t total = get_be(ip + T_IP_TOTAL_LEN, 2);
    const uint8_t *tcp = ip + IP_LEN;
    size_t hdrlen = (size_t)((tcp[T_TCP_DATA_OFF] >> 4) & 0xf) * 4;
    if (payload_len != NULL)
        *payload_len = total - IP_LEN - hdrlen;
    return tcp;
}

static const uint8_t *tcp_payload(const uint8_t *f)
{
    const uint8_t *tcp = f + ETH_LEN + IP_LEN;
    size_t hdrlen = (size_t)((tcp[T_TCP_DATA_OFF] >> 4) & 0xf) * 4;

    return tcp + hdrlen;
}

/* Recompute both checksums the way a receiving NIC would. */
static bool checksums_ok(const char *name, const uint8_t *f, size_t len)
{
    if (len < ETH_LEN || get_be(f + T_ETH_TYPE, 2) != 0x0800)
        return true; /* ARP carries no checksum */

    const uint8_t *ip = f + ETH_LEN;
    if (ones_sum(ip, IP_LEN, 0) != 0) {
        fail(name, "IP checksum is wrong");
        return false;
    }

    size_t total = get_be(ip + T_IP_TOTAL_LEN, 2);
    if (ETH_LEN + total > len) {
        fail(name, "IP total_len %zu overruns the %zu byte frame", total, len);
        return false;
    }
    size_t l4len = total - IP_LEN;
    const uint8_t *l4 = ip + IP_LEN;

    if (ip[T_IP_PROTOCOL] == 6) {
        uint32_t seed =
            pseudo_seed(get_be(ip + T_IP_SRC, 4), get_be(ip + T_IP_DST, 4), 6,
                        (uint16_t)l4len);
        if (ones_sum(l4, l4len, seed) != 0) {
            fail(name, "TCP checksum is wrong");
            return false;
        }
    } else if (ip[T_IP_PROTOCOL] == 1) {
        if (ones_sum(l4, l4len, 0) != 0) {
            fail(name, "ICMP checksum is wrong");
            return false;
        }
    }
    return true;
}

static unsigned count_http_payloads(const struct capture *cap)
{
    unsigned n = 0;

    for (unsigned i = 0; i < cap->n; i++) {
        size_t plen = 0;
        if (as_tcp(cap->f[i].buf, cap->f[i].len, &plen) == NULL)
            continue;
        if (plen >= 8 && memcmp(tcp_payload(cap->f[i].buf), "HTTP/1.1", 8) == 0)
            n++;
    }
    return n;
}

static void check_all_checksums(const char *name, const struct capture *cap)
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

/*
 * Fake guest memory, so the RDMA data plane can be exercised through the
 * TCP endpoint rather than stubbed out.  The real one resolves an rkey
 * in the emulator's MR table and DMAs against the guest; here a single
 * flat window stands in for the client's registered buffer.
 */
#define FAKE_RKEY 0x00000101u
#define FAKE_BASE 0x70000000ull
#define FAKE_SIZE (256u * 1024u)

struct fake_mem {
    uint8_t buf[FAKE_SIZE];
};

static bool fake_range_ok(uint32_t key, uint64_t addr, uint32_t len,
                          uint64_t *off)
{
    if (key != FAKE_RKEY)
        return false;
    if (addr < FAKE_BASE || addr - FAKE_BASE + len > FAKE_SIZE)
        return false;
    *off = addr - FAKE_BASE;
    return true;
}

static uint32_t fake_from_host(void *ctx, uint32_t key, uint64_t addr,
                               void *dst, uint32_t len)
{
    struct fake_mem *m = ctx;
    uint64_t off;

    if (!fake_range_ok(key, addr, len, &off))
        return 0;
    memcpy(dst, m->buf + off, len);
    return len;
}

static uint32_t fake_to_host(void *ctx, uint32_t key, uint64_t addr,
                             const void *src, uint32_t len)
{
    struct fake_mem *m = ctx;
    uint64_t off;

    if (!fake_range_ok(key, addr, len, &off))
        return 0;
    memcpy(m->buf + off, src, len);
    return len;
}

static const struct s3_dma_ops fake_dma = {
    .from_host = fake_from_host,
    .to_host = fake_to_host,
};

/* Mint the token the guest client would over a window of that buffer. */
static void mint(char *out, uint64_t off, uint64_t len)
{
    struct s3_token t;

    memset(&t, 0, sizeof(t));
    t.transport = S3_TRANSPORT_RC;
    t.qp_num = 2;
    t.gid[10] = 0xff;
    t.gid[11] = 0xff;
    t.gid[12] = 192;
    t.gid[13] = 168;
    t.gid[14] = 200;
    t.gid[15] = 9;
    t.rkey = FAKE_RKEY;
    t.remote_addr = FAKE_BASE + off;
    t.length = len;
    t.port_num = 1;
    s3_token_encode(&t, out);
}

static bool fixture_up_dma(struct fixture *fx, const char *name,
                           unsigned max_conns, const struct s3_dma_ops *dma,
                           void *dma_ctx)
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

    fx->tcp = s3_tcp_create(&ncfg, fx->target, dma, dma_ctx, cap_tx, &fx->cap,
                            err, sizeof(err));
    if (fx->tcp == NULL) {
        fail(name, "tcp create: %s", err);
        s3_target_destroy(fx->target);
        return false;
    }
    return true;
}

static bool fixture_up(struct fixture *fx, const char *name, unsigned max_conns)
{
    return fixture_up_dma(fx, name, max_conns, NULL, NULL);
}

static void fixture_down(struct fixture *fx)
{
    s3_tcp_destroy(fx->tcp);
    s3_target_destroy(fx->target);
}

/*
 * Offer @len bytes of the fixture's frame in a buffer of exactly that
 * size, so a sanitizer sees any read past the end of what arrived.
 */
static bool offer_exact(struct fixture *fx, const char *name, size_t len,
                        uint64_t now)
{
    uint8_t *f = malloc(len);

    if (f == NULL) {
        fail(name, "out of memory");
        return false;
    }
    memcpy(f, fx->frame, len);
    bool consumed = s3_tcp_rx_frame(fx->tcp, f, len, now);
    free(f);
    return consumed;
}

/*
 * Three-way handshake.  Returns the target's initial sequence number
 * plus one, i.e. the sequence its first data byte will carry.
 */
static bool handshake_opt(struct fixture *fx, const char *name, uint16_t sport,
                          uint32_t client_iss, const uint8_t *opt,
                          size_t optlen, uint32_t *srv_seq, uint64_t now)
{
    cap_reset(&fx->cap);
    size_t n = build_tcp_ex(fx->frame, fx->mac, sport, client_iss, 0, FLAG_SYN,
                            32768, opt, optlen, NULL, 0);
    if (!offer_exact(fx, name, n, now)) {
        fail(name, "the SYN was not accepted as ours");
        return false;
    }
    if (fx->cap.n != 1) {
        fail(name, "a SYN produced %u frames, want 1", fx->cap.n);
        return false;
    }
    if (!checksums_ok(name, fx->cap.f[0].buf, fx->cap.f[0].len))
        return false;

    const uint8_t *tcp = as_tcp(fx->cap.f[0].buf, fx->cap.f[0].len, NULL);
    if (tcp == NULL ||
        (tcp_flags(tcp) & (FLAG_SYN | FLAG_ACK)) != (FLAG_SYN | FLAG_ACK)) {
        fail(name, "the answer to a SYN was not a SYN|ACK (flags 0x%02x)",
             tcp ? (unsigned)tcp_flags(tcp) : 0u);
        return false;
    }
    if (tcp_ack(tcp) != client_iss + 1) {
        fail(name, "SYN|ACK acks %u, want %u", tcp_ack(tcp), client_iss + 1);
        return false;
    }
    if (get_be(tcp + T_TCP_SRC_PORT, 2) != TARGET_PORT) {
        fail(name, "SYN|ACK came from port %u",
             get_be(tcp + T_TCP_SRC_PORT, 2));
        return false;
    }
    *srv_seq = tcp_seq(tcp) + 1;

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

static bool handshake(struct fixture *fx, const char *name, uint16_t sport,
                      uint32_t client_iss, uint32_t *srv_seq, uint64_t now)
{
    return handshake_opt(fx, name, sport, client_iss, NULL, 0, srv_seq, now);
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

/* Byte @i of every object seed_object() stores. */
static char seed_byte(size_t i)
{
    return (char)('a' + i % 26);
}

/*
 * Store a @len byte object at /ernic/@key straight through the target, so
 * a test can GET a response larger than one segment without first
 * sending a request larger than one frame.
 */
static bool seed_object(struct fixture *fx, const char *name, const char *key,
                        size_t len)
{
    char head[128];
    int hl = snprintf(head, sizeof(head),
                      "PUT /ernic/%s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n",
                      key, len);
    if (hl < 0 || (size_t)hl >= sizeof(head)) {
        fail(name, "seed request for %s does not fit", key);
        return false;
    }

    char *text = malloc((size_t)hl + len);
    if (text == NULL) {
        fail(name, "out of memory");
        return false;
    }
    memcpy(text, head, (size_t)hl);
    for (size_t i = 0; i < len; i++)
        text[(size_t)hl + i] = seed_byte(i);

    struct s3_http_request req;
    struct s3_http_response resp;
    bool stored = false;
    if (s3_http_parse(text, (size_t)hl + len, &req) > 0) {
        s3_target_exec(fx->target, &req, NULL, NULL, &resp);
        stored = resp.status == 200;
        s3_http_response_free(&resp);
    }
    free(text);
    if (!stored)
        fail(name, "could not seed %s", key);
    return stored;
}

/* True when @resp is a 200 whose body is the @len byte seeded object. */
static bool is_seeded_object(const char *resp, size_t resp_len, size_t len)
{
    const char *body = strstr(resp, "\r\n\r\n");

    if (strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) != 0 || body == NULL)
        return false;
    body += 4;
    if ((size_t)(resp + resp_len - body) != len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (body[i] != seed_byte(i))
            return false;
    return true;
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
    const uint8_t *arp = f + ETH_LEN;

    if (get_be(f + T_ETH_TYPE, 2) != 0x0806)
        fail(name, "reply ethertype 0x%04x", get_be(f + T_ETH_TYPE, 2));
    if (memcmp(f + T_ETH_DST, GUEST_MAC, 6) != 0)
        fail(name, "reply was not unicast back to the requester");
    if (memcmp(f + T_ETH_SRC, fx.mac, 6) != 0)
        fail(name, "reply source MAC is not the endpoint's");
    if (get_be(arp + T_ARP_OP, 2) != 2)
        fail(name, "ARP op %u, want 2 (reply)", get_be(arp + T_ARP_OP, 2));
    if (get_be(arp + T_ARP_SENDER_IP, 4) != TARGET_IP)
        fail(name, "reply sender IP 0x%08x", get_be(arp + T_ARP_SENDER_IP, 4));
    if (memcmp(arp + T_ARP_SENDER_MAC, fx.mac, 6) != 0)
        fail(name, "reply advertises the wrong MAC for our address");
    if (memcmp(arp + T_ARP_TARGET_MAC, GUEST_MAC, 6) != 0)
        fail(name, "reply is not addressed to the requester's MAC");
    if (get_be(arp + T_ARP_TARGET_IP, 4) != GUEST_IP)
        fail(name, "reply target IP 0x%08x", get_be(arp + T_ARP_TARGET_IP, 4));
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
    const uint8_t *ip = f + ETH_LEN;
    const uint8_t *icmp = ip + IP_LEN;

    if (ip[T_IP_PROTOCOL] != 1)
        fail(name, "reply protocol %u, want ICMP", ip[T_IP_PROTOCOL]);
    if (get_be(ip + T_IP_DST, 4) != GUEST_IP)
        fail(name, "reply addressed to 0x%08x", get_be(ip + T_IP_DST, 4));
    if (icmp[T_ICMP_TYPE] != 0)
        fail(name, "ICMP type %u, want 0 (echo reply)", icmp[T_ICMP_TYPE]);
    if (get_be(icmp + T_ICMP_ID, 2) != 0x4242 ||
        get_be(icmp + T_ICMP_SEQ, 2) != 7)
        fail(name, "reply id/seq %u/%u do not match the request",
             get_be(icmp + T_ICMP_ID, 2), get_be(icmp + T_ICMP_SEQ, 2));
    if (memcmp(icmp + ICMP_LEN, payload, strlen(payload)) != 0)
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
        const uint8_t *tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, &plen);
        if (tcp == NULL || plen == 0)
            continue;
        if (tcp_seq(tcp) != expect) {
            fail(name, "segment %u has seq %u, want %u", i, tcp_seq(tcp),
                 expect);
            break;
        }
        expect += (uint32_t)plen;
    }

    /* No FIN while a keep-alive response is still outstanding. */
    for (unsigned i = 0; i < fx.cap.n; i++) {
        const uint8_t *tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp_flags(tcp) & FLAG_FIN))
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
        const uint8_t *tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp_flags(tcp) & FLAG_FIN))
            fin = true;
    }
    if (!fin)
        fail(name, "no FIN after the closing response was acknowledged");

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * One whole transaction the way the guest client does it: a fresh
 * connection, one request carrying Connection: close, read the answer,
 * then acknowledge the data and the FIN and close.  Returns the number
 * of response bytes collected.
 */
static size_t transact(struct fixture *fx, const char *name, uint16_t sport,
                       const char *req, char *out, size_t outlen, uint64_t now)
{
    const uint32_t iss = 1000;
    uint32_t srv = 0;

    out[0] = '\0';
    if (!handshake(fx, name, sport, iss, &srv, now))
        return 0;

    cap_reset(&fx->cap);
    size_t n = build_tcp(fx->frame, fx->mac, sport, iss + 1, srv,
                         FLAG_ACK | FLAG_PSH, req, strlen(req));
    if (!s3_tcp_rx_frame(fx->tcp, fx->frame, n, now + 1)) {
        fail(name, "the request segment was not consumed");
        return 0;
    }
    check_all_checksums(name, &fx->cap);

    size_t got = collect(&fx->cap, out, outlen);
    if (got == 0)
        return 0;

    /*
     * Acknowledge the response.  The FIN a Connection: close request is
     * owed deliberately waits for this, so that it never overtakes data
     * that may still need retransmitting.
     */
    const uint32_t cseq = iss + 1 + (uint32_t)strlen(req);
    cap_reset(&fx->cap);
    n = build_tcp(fx->frame, fx->mac, sport, cseq, srv + (uint32_t)got,
                  FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx->tcp, fx->frame, n, now + 2);

    bool fin = false;
    for (unsigned i = 0; i < fx->cap.n; i++) {
        const uint8_t *tcp = as_tcp(fx->cap.f[i].buf, fx->cap.f[i].len, NULL);
        if (tcp != NULL && (tcp_flags(tcp) & FLAG_FIN))
            fin = true;
    }
    if (!fin) {
        fail(name, "no FIN after the closing response was acknowledged");
        return got;
    }

    /* Ack the FIN and send our own: this is what retires the connection,
     * and a slot that does not come back is a slot leaked. */
    cap_reset(&fx->cap);
    n = build_tcp(fx->frame, fx->mac, sport, cseq, srv + (uint32_t)got + 1,
                  FLAG_ACK | FLAG_FIN, NULL, 0);
    s3_tcp_rx_frame(fx->tcp, fx->frame, n, now + 3);
    return got;
}

/*
 * The whole point of the backend, driven the way the guest client drives
 * it: a PUT whose bytes come out of the client's registered buffer over
 * the data plane, then a GET of the same object into a different window
 * of that buffer.  Nothing below the HTTP layer is stubbed -- this is the
 * real TCP endpoint, the real parser and the real store.
 */
static void test_rdma_round_trip(void)
{
    const char *name = "rdma-round-trip";
    struct fixture fx;
    struct fake_mem *mem = calloc(1, sizeof(*mem));
    char hex[S3_TOKEN_HEX_LEN + 1];
    char req[1024];
    char body[8192];
    const size_t len = 4096;
    const uint64_t dst = FAKE_SIZE / 2;

    if (mem == NULL) {
        fail(name, "out of memory");
        return;
    }
    if (!fixture_up_dma(&fx, name, 0, &fake_dma, mem)) {
        free(mem);
        return;
    }

    for (size_t i = 0; i < len; i++)
        mem->buf[i] = (uint8_t)(i * 7 + 1);

    /*
     * Byte for byte what tests/s3_rdma_client.c puts on the wire: the
     * token, and a Content-Length naming the object with no body behind
     * it.  The object is smaller than S3_HTTP_REQUEST_MAX here only so
     * the frame fits; the size rule itself is pinned in the HTTP tests.
     */
    mint(hex, 0, len);
    snprintf(req, sizeof(req),
             "PUT /ernic/roundtrip.bin HTTP/1.1\r\n"
             "Host: 192.168.200.1:9000\r\n"
             "Connection: close\r\n"
             "x-amz-rdma-token: %s\r\n"
             "Content-Length: %zu\r\n\r\n",
             hex, len);

    if (transact(&fx, name, GUEST_PORT, req, body, sizeof(body), 10) == 0) {
        fail(name, "the RDMA PUT went unanswered");
        goto out;
    }
    if (strncmp(body, "HTTP/1.1 200 OK\r\n", 17) != 0) {
        fail(name, "RDMA PUT answered %.32s", body);
        goto out;
    }
    if (strstr(body, "x-amz-rdma-reply") == NULL)
        fail(name, "no x-amz-rdma-reply on the PUT response");

    mint(hex, dst, len);
    snprintf(req, sizeof(req),
             "GET /ernic/roundtrip.bin HTTP/1.1\r\n"
             "Host: 192.168.200.1:9000\r\n"
             "Connection: close\r\n"
             "x-amz-rdma-token: %s\r\n"
             "Content-Length: 0\r\n\r\n",
             hex);

    if (transact(&fx, name, GUEST_PORT + 1, req, body, sizeof(body), 20) == 0) {
        fail(name, "the RDMA GET went unanswered");
        goto out;
    }
    if (strncmp(body, "HTTP/1.1 200 OK\r\n", 17) != 0)
        fail(name, "RDMA GET answered %.32s", body);
    else if (memcmp(mem->buf, mem->buf + dst, len) != 0)
        fail(name, "the object did not come back byte for byte");
    else
        ok(name);

out:
    fixture_down(&fx);
    free(mem);
}

/*
 * A slot has to come back when the connection that held it closes.  The
 * guest client opens one connection per request, so a slot leaked on any
 * path silently black-holes every request after the eighth -- which is
 * how one stalled transfer turned into a cascade of unanswered requests.
 */
static void test_conn_recycle(void)
{
    const char *name = "conn-recycle";
    struct fixture fx;
    const unsigned slots = 4;
    char body[8192];

    if (!fixture_up_dma(&fx, name, slots, NULL, NULL))
        return;

    static const char req[] = "GET / HTTP/1.1\r\n"
                              "Host: 192.168.200.1:9000\r\n"
                              "Connection: close\r\n\r\n";

    for (unsigned i = 0; i < slots * 3; i++) {
        size_t got = transact(&fx, name, (uint16_t)(GUEST_PORT + i), req, body,
                              sizeof(body), 100 + i * 10);
        if (got == 0) {
            fail(name, "connection %u of %u went unanswered: a slot leaked",
                 i + 1, slots * 3);
            goto out;
        }
        if (strncmp(body, "HTTP/1.1 200 OK\r\n", 17) != 0) {
            fail(name, "connection %u answered %.32s", i + 1, body);
            goto out;
        }
    }
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
        fail(name, "stored %" PRIu64 " bytes, want 11",
             s3_target_bytes_used(fx.target));

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
    const uint8_t *tcp = as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL);
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_RST))
        fail(name, "a stray segment was not reset (flags 0x%02x)",
             tcp ? (unsigned)tcp_flags(tcp) : 0u);
    checksums_ok(name, fx.cap.f[0].buf, fx.cap.f[0].len);

    /* Another port on the same wire is not ours. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, 40001, 1, 0, FLAG_SYN, NULL, 0);
    put_be(fx.frame + ETH_LEN + IP_LEN + T_TCP_DST_PORT, 2, TARGET_PORT + 1);
    if (s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0))
        fail(name, "consumed a segment for somebody else's port");
    if (fx.cap.n != 0)
        fail(name, "answered a segment for somebody else's port");

    /* And neither is a frame addressed to another MAC. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, 40002, 1, 0, FLAG_SYN, NULL, 0);
    fx.frame[T_ETH_DST + 5] ^= 0xff;
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
    const uint8_t *tcp = as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL);
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_RST))
        fail(name, "a SYN with no slot free was not reset");
    if (s3_tcp_conn_count(fx.tcp) != slots)
        fail(name, "the refused SYN still consumed a slot");

    const struct s3_tcp_stats *st = s3_tcp_statistics(fx.tcp);
    if (st->refused != 1)
        fail(name, "refused counter is %" PRIu64 ", want 1", st->refused);
    if (st->accepts != slots)
        fail(name, "accepts counter is %" PRIu64 ", want %u", st->accepts,
             slots);

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

/*
 * An oversized body is answered once.  The endpoint used to queue a fresh
 * 413 for every segment that overflowed the receive buffer, so once the
 * peer acknowledged the first one the FIN went out and every answer after
 * it landed on a sequence number the FIN had already consumed -- with the
 * slot held until the idle reaper took it back.
 */
static void test_oversized_stream(void)
{
    const char *name = "oversized-stream";
    struct fixture fx;
    uint32_t srv = 0;
    uint8_t junk[1460];
    unsigned responses = 0;
    unsigned after_fin = 0;
    uint32_t fin_seq = 0;
    size_t answered = 0;
    bool fin_seen = false;
    bool acked = false;

    memset(junk, 'A', sizeof(junk));

    if (!fixture_up(&fx, name, 0))
        return;
    if (!handshake(&fx, name, GUEST_PORT, 1, &srv, 10))
        goto out;

    /* Enough segments to fill the receive buffer twice over, so most of
     * the body arrives after the endpoint has given up on the request. */
    const unsigned rounds = S3_HTTP_REQUEST_MAX / (unsigned)sizeof(junk) + 12;
    uint32_t seq = 2;

    for (unsigned i = 0; i < rounds; i++) {
        /* Acknowledge the answer on its own the way a client still
         * writing its body does: that is what releases the FIN. */
        bool ack_only = answered > 0 && !acked;

        cap_reset(&fx.cap);
        size_t n = build_tcp(
            fx.frame, fx.mac, GUEST_PORT, seq, srv + (uint32_t)answered,
            FLAG_ACK, ack_only ? NULL : junk, ack_only ? 0 : sizeof(junk));
        if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20 + i)) {
            fail(name, "a body segment was not consumed");
            goto out;
        }
        if (ack_only)
            acked = true;
        else
            seq += (uint32_t)sizeof(junk);
        check_all_checksums(name, &fx.cap);

        for (unsigned j = 0; j < fx.cap.n; j++) {
            size_t plen = 0;
            const uint8_t *tcp =
                as_tcp(fx.cap.f[j].buf, fx.cap.f[j].len, &plen);
            if (tcp == NULL)
                continue;
            if (plen > 0) {
                if (fin_seen)
                    after_fin++;
                else
                    answered += plen;
                if (plen >= 8 &&
                    memcmp(tcp_payload(fx.cap.f[j].buf), "HTTP/1.1", 8) == 0)
                    responses++;
            }
            if (tcp_flags(tcp) & FLAG_FIN) {
                fin_seen = true;
                fin_seq = tcp_seq(tcp);
            }
        }
    }

    if (responses == 0) {
        fail(name, "an oversized body went unanswered");
        goto out;
    }
    if (responses != 1)
        fail(name, "an oversized body drew %u answers, want 1", responses);
    if (!fin_seen) {
        fail(name, "the endpoint never closed on an oversized body");
        goto out;
    }
    if (after_fin != 0)
        fail(name, "%u data segments followed the FIN at seq %u", after_fin,
             fin_seq);

    /* The ACK of our own FIN has to disarm its retransmit timer as well as
     * clear a send buffer it has no bytes left in. */
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq,
                         srv + (uint32_t)answered + 1, FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 100);
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "the ACK of our FIN left its retransmit armed");

    /* The peer's own close has to retire the slot: a drain that holds one
     * until the idle reaper starves an endpoint with eight of them. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq,
                  srv + (uint32_t)answered + 1, FLAG_ACK | FLAG_FIN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 101);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "the connection kept its slot after both sides closed");

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * The hole the segment-sized test above cannot reach.  Refusing an
 * oversized segment leaves the receive buffer exactly as full as it was,
 * so a later segment that fits the remaining room is still accepted --
 * and once it lands the buffer holds S3_HTTP_REQUEST_MAX bytes, which the
 * parser answers with a second 413 of its own.  That one is queued behind
 * a FIN that has already been decided on, so it is emitted at a sequence
 * number the FIN consumes.
 *
 * Sized deliberately rather than with a round segment: the gap has to be
 * filled to the byte for the parser to see a full buffer.
 */
static void test_oversized_exact_fit(void)
{
    const char *name = "oversized-exact-fit";
    struct fixture fx;
    uint32_t srv = 0;
    uint8_t junk[1460];
    unsigned responses = 0;
    uint32_t seq = 2;

    memset(junk, 'A', sizeof(junk));

    if (!fixture_up(&fx, name, 0))
        return;
    if (!handshake(&fx, name, GUEST_PORT, 1, &srv, 10))
        goto out;

    /* Whole segments until the next one would not fit, then the one that
     * does not: that is the segment the endpoint answers. */
    const unsigned whole = S3_HTTP_REQUEST_MAX / (unsigned)sizeof(junk);
    const size_t gap = S3_HTTP_REQUEST_MAX - whole * sizeof(junk);

    for (unsigned i = 0; i <= whole; i++) {
        cap_reset(&fx.cap);
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK,
                             junk, sizeof(junk));
        if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20 + i)) {
            fail(name, "a body segment was not consumed");
            goto out;
        }
        seq += (uint32_t)sizeof(junk);
        responses += count_http_payloads(&fx.cap);
    }

    if (responses != 1) {
        fail(name, "filling the buffer drew %u answers, want 1", responses);
        goto out;
    }
    if (gap == 0) {
        /* Nothing to fit; the segment size divides the limit exactly. */
        ok(name);
        goto out;
    }

    /* The refusal consumed no buffer, so this still fits -- and brings the
     * buffer to exactly the parser's limit. */
    cap_reset(&fx.cap);
    size_t n =
        build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK, junk, gap);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 100)) {
        fail(name, "the exact-fit segment was not consumed");
        goto out;
    }
    check_all_checksums(name, &fx.cap);

    unsigned extra = count_http_payloads(&fx.cap);
    if (extra != 0) {
        fail(name,
             "a segment that exactly filled the buffer drew %u more "
             "answers, want 0",
             extra);
        goto out;
    }

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * A port probe: the peer completes the handshake and closes without ever
 * sending a request, so nothing was ever queued for it.  The ACK of our
 * own FIN then acknowledges a sequence number no send buffer backs.
 */
static void test_probe_close(void)
{
    const char *name = "probe-close";
    struct fixture fx;
    uint32_t srv = 0;

    if (!fixture_up(&fx, name, 0))
        return;

    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                         FLAG_ACK | FLAG_FIN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
    check_all_checksums(name, &fx.cap);

    bool fin = false;
    for (unsigned i = 0; i < fx.cap.n; i++) {
        const uint8_t *tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp_flags(tcp) & FLAG_FIN))
            fin = true;
    }
    if (!fin) {
        fail(name, "the endpoint did not close back on a peer FIN");
        goto out;
    }

    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1002, srv + 1, FLAG_ACK, NULL,
                  0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 21);
    for (unsigned i = 0; i < fx.cap.n; i++) {
        const uint8_t *tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
        if (tcp != NULL && (tcp_flags(tcp) & FLAG_RST))
            fail(name, "the ACK of our FIN drew a reset");
    }
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "a probed-and-closed connection kept its slot");
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "a retired connection still claims work");

    ok(name);
out:
    fixture_down(&fx);
}

/* Find the one captured frame carrying TCP data, or NULL if there is not
 * exactly one. */
static const uint8_t *only_data_frame(const struct capture *cap, size_t *plen)
{
    const uint8_t *found = NULL;

    for (unsigned i = 0; i < cap->n; i++) {
        size_t len = 0;
        if (as_tcp(cap->f[i].buf, cap->f[i].len, &len) == NULL || len == 0)
            continue;
        if (found != NULL)
            return NULL;
        found = cap->f[i].buf;
        *plen = len;
    }
    return found;
}

static const uint8_t *frame_tcp(const uint8_t *f)
{
    return f + ETH_LEN + IP_LEN;
}

/*
 * An unacknowledged response goes out again when its timer expires,
 * from the first byte the peer has not acknowledged and with the timer
 * doubled, and an ACK of everything disarms it.
 */
static void test_retransmit_data(void)
{
    const char *name = "retransmit-data";
    struct fixture fx;
    uint32_t srv = 0;
    uint8_t first[1514];
    size_t first_len = 0;

    if (!fixture_up(&fx, name, 0))
        return;
    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    static const char req[] = "GET / HTTP/1.1\r\n\r\n";
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                         FLAG_ACK | FLAG_PSH, req, strlen(req));
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);

    const uint8_t *f = only_data_frame(&fx.cap, &first_len);
    if (f == NULL || tcp_seq(frame_tcp(f)) != srv) {
        fail(name, "the response was not one segment at seq %u", srv);
        goto out;
    }
    memcpy(first, tcp_payload(f), first_len);
    if (!s3_tcp_has_work(fx.tcp))
        fail(name, "unacknowledged data leaves no work for the timer");

    /* The timer runs 200 ms from the send, then 400 ms from the resend. */
    static const uint64_t fire[] = {220, 620};
    for (unsigned i = 0; i < 2; i++) {
        cap_reset(&fx.cap);
        if (s3_tcp_poll(fx.tcp, fire[i] - 1) || fx.cap.n != 0)
            fail(name, "resend %u came before its timer at %" PRIu64, i + 1,
                 fire[i]);

        cap_reset(&fx.cap);
        if (!s3_tcp_poll(fx.tcp, fire[i]))
            fail(name, "the poll at %" PRIu64 " reported nothing done",
                 fire[i]);
        check_all_checksums(name, &fx.cap);
        size_t plen = 0;
        f = only_data_frame(&fx.cap, &plen);
        if (f == NULL) {
            fail(name, "resend %u was %u frames, want one segment", i + 1,
                 fx.cap.n);
            goto out;
        }
        if (tcp_seq(frame_tcp(f)) != srv)
            fail(name, "resend %u at seq %u, want %u", i + 1,
                 tcp_seq(frame_tcp(f)), srv);
        if (plen != first_len || memcmp(tcp_payload(f), first, plen) != 0)
            fail(name, "resend %u is not the bytes first sent", i + 1);
    }
    if (s3_tcp_statistics(fx.tcp)->retransmits != 2)
        fail(name, "retransmits counter %" PRIu64 ", want 2",
             s3_tcp_statistics(fx.tcp)->retransmits);

    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, (uint32_t)(1001 + strlen(req)),
                  srv + (uint32_t)first_len, FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 630);
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "an ACK of everything left the timer armed");
    if (s3_tcp_poll(fx.tcp, 5000) || fx.cap.n != 0)
        fail(name, "acknowledged data was sent again");

    ok(name);
out:
    fixture_down(&fx);
}

/* A FIN is retransmitted like data, at the same sequence number, until
 * the peer acknowledges it. */
static void test_retransmit_fin(void)
{
    const char *name = "retransmit-fin";
    struct fixture fx;
    uint32_t srv = 0;

    if (!fixture_up(&fx, name, 0))
        return;
    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    /* The peer closes first, with nothing sent either way. */
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                         FLAG_ACK | FLAG_FIN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
    const uint8_t *tcp =
        fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_FIN) || tcp_seq(tcp) != srv) {
        fail(name, "the peer's FIN was not answered with a FIN at seq %u", srv);
        goto out;
    }

    cap_reset(&fx.cap);
    if (s3_tcp_poll(fx.tcp, 219) || fx.cap.n != 0)
        fail(name, "the FIN was resent before its timer");
    cap_reset(&fx.cap);
    s3_tcp_poll(fx.tcp, 220);
    check_all_checksums(name, &fx.cap);
    tcp = fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || tcp_flags(tcp) != (FLAG_FIN | FLAG_ACK))
        fail(name, "the timer did not resend the FIN");
    else if (tcp_seq(tcp) != srv || tcp_ack(tcp) != 1002)
        fail(name, "the FIN was resent as seq %u ack %u, want %u and 1002",
             tcp_seq(tcp), tcp_ack(tcp), srv);
    if (s3_tcp_statistics(fx.tcp)->retransmits != 1)
        fail(name, "retransmits counter %" PRIu64 ", want 1",
             s3_tcp_statistics(fx.tcp)->retransmits);

    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1002, srv + 1, FLAG_ACK, NULL,
                  0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 230);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "the ACK of the resent FIN did not retire the connection");
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "a retired connection still claims work");

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * A peer whose SYN|ACKs are all lost retransmits its SYN, which keeps the
 * connection clear of the idle reaper, so it is the retransmit limit that
 * gives up on it: eight resends on a doubling timer, then a reset.
 */
static void test_retransmit_syn_ack(void)
{
    const char *name = "retransmit-syn-ack";
    struct fixture fx;
    uint32_t srv_iss = 0;
    uint64_t sent_at[16];
    unsigned sent = 0;
    uint64_t reset_at = 0;

    /* 200 ms after the SYN, then each wait twice the one before. */
    static const uint64_t want_at[] = {200,  600,   1400,  3000,
                                       6200, 12600, 25400, 51000};
    const unsigned want = sizeof(want_at) / sizeof(want_at[0]);
    const uint64_t want_reset = 102200;

    if (!fixture_up(&fx, name, 0))
        return;

    size_t n =
        build_tcp(fx.frame, fx.mac, GUEST_PORT, 500, 0, FLAG_SYN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 0);
    const uint8_t *tcp =
        fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || tcp_flags(tcp) != (FLAG_SYN | FLAG_ACK)) {
        fail(name, "the SYN was not answered with a SYN|ACK");
        goto out;
    }
    srv_iss = tcp_seq(tcp);

    for (uint64_t now = 100; now <= 2 * want_reset && reset_at == 0;
         now += 100) {
        cap_reset(&fx.cap);
        if (now % 10000 == 0) {
            s3_tcp_rx_frame(fx.tcp, fx.frame, n, now);
            if (fx.cap.n != 0)
                fail(name, "a repeated SYN at %" PRIu64 " drew a frame", now);
        }
        s3_tcp_poll(fx.tcp, now);
        check_all_checksums(name, &fx.cap);

        for (unsigned i = 0; i < fx.cap.n; i++) {
            tcp = as_tcp(fx.cap.f[i].buf, fx.cap.f[i].len, NULL);
            if (tcp == NULL)
                continue;
            if (tcp_flags(tcp) & FLAG_RST) {
                reset_at = now;
            } else if (tcp_flags(tcp) == (FLAG_SYN | FLAG_ACK)) {
                if (tcp_seq(tcp) != srv_iss || tcp_ack(tcp) != 501)
                    fail(name, "SYN|ACK resent as seq %u ack %u", tcp_seq(tcp),
                         tcp_ack(tcp));
                if ((tcp[T_TCP_DATA_OFF] >> 4) != 6)
                    fail(name, "SYN|ACK resent without its MSS option");
                if (sent < sizeof(sent_at) / sizeof(sent_at[0]))
                    sent_at[sent] = now;
                sent++;
            } else {
                fail(name, "unexpected flags 0x%02x at %" PRIu64,
                     (unsigned)tcp_flags(tcp), now);
            }
        }
    }

    if (sent != want) {
        fail(name, "SYN|ACK resent %u times, want %u", sent, want);
    } else {
        for (unsigned i = 0; i < want; i++)
            if (sent_at[i] != want_at[i])
                fail(name, "resend %u at %" PRIu64 " ms, want %" PRIu64, i + 1,
                     sent_at[i], want_at[i]);
    }
    if (reset_at != want_reset)
        fail(name, "reset at %" PRIu64 " ms, want %" PRIu64, reset_at,
             want_reset);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "the connection outlived its retransmit limit");

    const struct s3_tcp_stats *st = s3_tcp_statistics(fx.tcp);
    if (st->retransmits != want || st->resets != 1)
        fail(name,
             "%" PRIu64 " retransmits and %" PRIu64 " resets, want %u and 1",
             st->retransmits, st->resets, want);

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * The MSS option on a SYN sets the largest segment the endpoint sends,
 * found by walking the option list.  A value below the 536-byte floor
 * every IPv4 host must accept, or above the endpoint's own 1460, is not
 * used, and a list that is malformed is read no further.  The handshake
 * offers the SYN in a buffer of its exact size, so a walk that strays
 * past the end of the list is a sanitizer report rather than a read of
 * whatever the fixture's frame buffer held.
 */
static void test_mss_option(void)
{
    const char *name = "mss-option";
    const size_t obj_len = 4000;
    char resp[8192];

    static const struct {
        const char *what;
        uint8_t opt[8];
        size_t optlen;
        uint32_t want;
    } cases[] = {
        {"no options", {0}, 0, 1460},
        {"MSS 536", {2, 4, 0x02, 0x18}, 4, 536},
        {"NOPs then MSS", {1, 1, 2, 4, 0x03, 0x00, 0, 0}, 8, 768},
        {"window scale then MSS", {3, 3, 7, 2, 4, 0x04, 0x00, 0}, 8, 1024},
        {"MSS below the floor", {2, 4, 0x02, 0x17}, 4, 1460},
        {"MSS above our own", {2, 4, 0x05, 0xb5}, 4, 1460},
        {"MSS past the end of the list",
         {0, 0, 0, 0, 2, 4, 0x02, 0x18},
         8,
         1460},
        {"MSS of the wrong length", {2, 6, 0x02, 0x18, 0, 0, 0, 0}, 8, 1460},
        {"length below two", {1, 8, 1, 2, 4, 0x02, 0x18, 0}, 8, 1460},
        {"length past the list", {8, 10, 0, 0}, 4, 1460},
        {"kind with no length", {1, 1, 1, 2}, 4, 1460},
        {"MSS cut off by the end", {1, 1, 1, 1, 1, 1, 2, 4}, 8, 1460},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture fx;
        uint32_t srv = 0;

        if (!fixture_up(&fx, name, 0))
            return;
        if (!seed_object(&fx, name, "obj", obj_len) ||
            !handshake_opt(&fx, name, GUEST_PORT, 1000, cases[i].opt,
                           cases[i].optlen, &srv, 10)) {
            fixture_down(&fx);
            continue;
        }

        static const char req[] = "GET /ernic/obj HTTP/1.1\r\n\r\n";
        cap_reset(&fx.cap);
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                             FLAG_ACK | FLAG_PSH, req, strlen(req));
        s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
        check_all_checksums(name, &fx.cap);

        size_t largest = 0;
        for (unsigned j = 0; j < fx.cap.n; j++) {
            size_t plen = 0;
            if (as_tcp(fx.cap.f[j].buf, fx.cap.f[j].len, &plen) != NULL &&
                plen > largest)
                largest = plen;
        }
        size_t got = collect(&fx.cap, resp, sizeof(resp));

        if (largest != cases[i].want)
            fail(name, "%s: largest segment %zu bytes, want %" PRIu32,
                 cases[i].what, largest, cases[i].want);
        else if (!is_seeded_object(resp, got, obj_len))
            fail(name, "%s: the response did not carry the object",
                 cases[i].what);
        fixture_down(&fx);
    }

    ok(name);
}

/*
 * A zero window is read as a window of one byte, so the endpoint keeps
 * probing with single bytes rather than waiting for a window update that
 * might never come, and it sends the rest once the window opens.
 */
static void test_zero_window(void)
{
    const char *name = "zero-window";
    struct fixture fx;
    uint32_t srv = 0;
    const size_t obj_len = 3000;
    char resp[8192];
    size_t got = 0;

    if (!fixture_up(&fx, name, 0))
        return;
    if (!seed_object(&fx, name, "obj", obj_len) ||
        !handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    static const char req[] = "GET /ernic/obj HTTP/1.1\r\n\r\n";
    const uint32_t cseq = 1001 + (uint32_t)strlen(req);

    /* The request closes the window; then each one-byte probe is
     * acknowledged with the window still closed. */
    for (unsigned i = 0; i < 2; i++) {
        size_t n;
        if (i == 0)
            n = build_tcp_ex(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                             FLAG_ACK | FLAG_PSH, 0, NULL, 0, req, strlen(req));
        else
            n = build_tcp_ex(fx.frame, fx.mac, GUEST_PORT, cseq, srv + i,
                             FLAG_ACK, 0, NULL, 0, NULL, 0);
        cap_reset(&fx.cap);
        s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20 + i * 10);

        size_t plen = 0;
        const uint8_t *f = only_data_frame(&fx.cap, &plen);
        if (f == NULL || plen != 1 || tcp_seq(frame_tcp(f)) != srv + i) {
            fail(name, "into a zero window, segment %u was not 1 byte at %u", i,
                 srv + i);
            goto out;
        }
        got += collect(&fx.cap, resp + got, sizeof(resp) - got);

        /* With that byte in flight the window is full. */
        cap_reset(&fx.cap);
        s3_tcp_poll(fx.tcp, 21 + i * 10);
        if (fx.cap.n != 0)
            fail(name, "sent past a full window");
    }

    /* A window update with nothing new acknowledged opens the window. */
    cap_reset(&fx.cap);
    size_t n = build_tcp_ex(fx.frame, fx.mac, GUEST_PORT, cseq, srv + 2,
                            FLAG_ACK, 8192, NULL, 0, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 40);
    check_all_checksums(name, &fx.cap);
    got += collect(&fx.cap, resp + got, sizeof(resp) - got);
    if (!is_seeded_object(resp, got, obj_len)) {
        fail(name, "the response did not arrive whole once the window opened");
        goto out;
    }

    /* Acknowledging part of it leaves the rest under the timer. */
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, cseq, srv + 1000, FLAG_ACK,
                  NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 50);
    if (!s3_tcp_has_work(fx.tcp))
        fail(name, "a partial ACK disarmed the timer");
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, cseq, srv + (uint32_t)got,
                  FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 60);
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "an ACK of everything left the timer armed");

    ok(name);
out:
    fixture_down(&fx);
}

/*
 * An RST tears the connection down silently: answering a reset with a
 * reset is how two stacks loop.  One for no connection is dropped the
 * same way.  A SYN|ACK for no connection is not, and is reset from the
 * sequence number it acknowledged.
 */
static void test_reset_segments(void)
{
    const char *name = "reset-segments";
    struct fixture fx;
    uint32_t srv = 0;

    if (!fixture_up(&fx, name, 0))
        return;
    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;

    cap_reset(&fx.cap);
    size_t n =
        build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, 0, FLAG_RST, NULL, 0);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20))
        fail(name, "an RST for a connection was not consumed");
    if (fx.cap.n != 0)
        fail(name, "an RST drew %u frames, want none", fx.cap.n);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "an RST left the connection open");
    if (s3_tcp_has_work(fx.tcp))
        fail(name, "a reset connection still claims work");

    n = build_tcp(fx.frame, fx.mac, GUEST_PORT + 1, 1, 0, FLAG_RST, NULL, 0);
    if (!s3_tcp_rx_frame(fx.tcp, fx.frame, n, 30))
        fail(name, "an RST for no connection was not consumed");
    if (fx.cap.n != 0)
        fail(name, "an RST for no connection drew %u frames", fx.cap.n);
    if (s3_tcp_statistics(fx.tcp)->resets != 0)
        fail(name, "resets counter %" PRIu64 " after two RSTs in, want 0",
             s3_tcp_statistics(fx.tcp)->resets);

    /* The connection is gone, so its next segment is a stray. */
    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv, FLAG_ACK, "x", 1);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 40);
    const uint8_t *tcp =
        fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_RST))
        fail(name, "data on a reset connection was not reset");

    cap_reset(&fx.cap);
    n = build_tcp(fx.frame, fx.mac, GUEST_PORT + 2, 7000, 0x12345678u,
                  FLAG_SYN | FLAG_ACK, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 50);
    check_all_checksums(name, &fx.cap);
    tcp = fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || tcp_flags(tcp) != FLAG_RST)
        fail(name, "a stray SYN|ACK was not answered with a bare RST");
    else if (tcp_seq(tcp) != 0x12345678u)
        fail(name, "the RST has seq 0x%08x, want the SYN|ACK's ack",
             tcp_seq(tcp));
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "a stray SYN|ACK opened a connection");

    ok(name);
out:
    fixture_down(&fx);
}

/* ---- Malformed frames ---------------------------------------------------- */

#define PING_DATA     "ping"
#define PING_DATA_LEN (sizeof(PING_DATA) - 1)

#define ETH_FIELD(f)  ((size_t)T_ETH_##f)
#define IP_FIELD(f)   (ETH_LEN + T_IP_##f)
#define TCP_FIELD(f)  (ETH_LEN + IP_LEN + T_TCP_##f)
#define ARP_FIELD(f)  (ETH_LEN + T_ARP_##f)
#define ICMP_FIELD(f) (ETH_LEN + IP_LEN + T_ICMP_##f)
#define NO_FIELD      SIZE_MAX

enum frame_base { BASE_SYN, BASE_PING, BASE_ARP };

static size_t build_base(struct fixture *fx, enum frame_base base)
{
    switch (base) {
    case BASE_SYN:
        return build_tcp(fx->frame, fx->mac, 40100, 1, 0, FLAG_SYN, NULL, 0);
    case BASE_PING:
        return build_ping(fx->frame, fx->mac, 1, 1, PING_DATA);
    case BASE_ARP:
        return build_arp_request(fx->frame, TARGET_IP);
    default:
        return 0;
    }
}

/*
 * Every length and field the endpoint checks before it trusts a frame,
 * broken one at a time.  Each is a frame the guest can put on the wire,
 * and none may be consumed or answered.  Some checks repeat an earlier
 * one for a frame that reaches them, so a case may be refused by either.
 * The same frames unbroken are then offered last, so no case passes
 * because its base frame was refused already.
 */
static void test_malformed_frames(void)
{
    const char *name = "malformed-frames";
    struct fixture fx;

    static const struct {
        const char *what;
        enum frame_base base;
        size_t field; /* byte offset of the field to break, or NO_FIELD */
        unsigned width;
        uint32_t value;
        size_t len; /* bytes to offer, or 0 for the whole frame */
    } cases[] = {
        {"runt Ethernet frame", BASE_SYN, NO_FIELD, 0, 0, ETH_LEN - 1},
        {"IPv6 ethertype", BASE_SYN, ETH_FIELD(TYPE), 2, 0x86dd, 0},
        {"IP header cut short", BASE_SYN, NO_FIELD, 0, 0, ETH_LEN + IP_LEN - 1},
        {"IP version 6", BASE_SYN, IP_FIELD(VERSION_IHL), 1, 0x65, 0},
        {"IP header under 20 bytes", BASE_SYN, IP_FIELD(VERSION_IHL), 1, 0x44,
         0},
        {"IP total length under the header", BASE_SYN, IP_FIELD(TOTAL_LEN), 2,
         IP_LEN - 1, 0},
        {"IP options cut short", BASE_SYN, IP_FIELD(VERSION_IHL), 1, 0x46,
         ETH_LEN + 24 - 1}, /* a 24-byte header */
        {"IP to another address", BASE_SYN, IP_FIELD(DST), 4, TARGET_IP + 1, 0},
        {"IP more fragments", BASE_SYN, IP_FIELD(FRAG), 2, 0x2000, 0},
        {"IP fragment offset", BASE_SYN, IP_FIELD(FRAG), 2, 0x0001, 0},
        {"UDP", BASE_SYN, IP_FIELD(PROTOCOL), 1, 17, 0},
        {"TCP header cut short", BASE_SYN, IP_FIELD(TOTAL_LEN), 2,
         IP_LEN + TCP_LEN - 1, 0},
        {"TCP total length past the frame", BASE_SYN, IP_FIELD(TOTAL_LEN), 2,
         IP_LEN + TCP_LEN + 1, 0},
        {"TCP data offset under 20 bytes", BASE_SYN, TCP_FIELD(DATA_OFF), 1,
         4 << 4, 0},
        {"TCP data offset past the segment", BASE_SYN, TCP_FIELD(DATA_OFF), 1,
         6 << 4, 0},
        {"ARP cut short", BASE_ARP, NO_FIELD, 0, 0, ETH_LEN + ARP_LEN - 1},
        {"ARP hardware type", BASE_ARP, ARP_FIELD(HW_TYPE), 2, 6, 0},
        {"ARP protocol type", BASE_ARP, ARP_FIELD(PROTO_TYPE), 2, 0x86dd, 0},
        {"ARP hardware length", BASE_ARP, ARP_FIELD(HW_LEN), 1, 8, 0},
        {"ARP protocol length", BASE_ARP, ARP_FIELD(PROTO_LEN), 1, 16, 0},
        {"ARP reply", BASE_ARP, ARP_FIELD(OP), 2, 2, 0},
        {"ICMP header cut short", BASE_PING, IP_FIELD(TOTAL_LEN), 2,
         IP_LEN + ICMP_LEN - 1, 0},
        {"ICMP total length past the frame", BASE_PING, IP_FIELD(TOTAL_LEN), 2,
         IP_LEN + ICMP_LEN + PING_DATA_LEN + 1, 0},
        {"ICMP echo reply", BASE_PING, ICMP_FIELD(TYPE), 1, 0, 0},
    };

    if (!fixture_up(&fx, name, 0))
        return;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t len = build_base(&fx, cases[i].base);
        if (cases[i].field != NO_FIELD)
            put_be(fx.frame + cases[i].field, cases[i].width, cases[i].value);
        if (cases[i].len != 0)
            len = cases[i].len;

        cap_reset(&fx.cap);
        if (offer_exact(&fx, name, len, 10))
            fail(name, "%s: consumed", cases[i].what);
        if (fx.cap.n != 0)
            fail(name, "%s: answered with %u frames", cases[i].what, fx.cap.n);
    }
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "a malformed SYN opened a connection");

    static const enum frame_base bases[] = {BASE_SYN, BASE_PING, BASE_ARP};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        size_t len = build_base(&fx, bases[i]);
        cap_reset(&fx.cap);
        if (!offer_exact(&fx, name, len, 10) || fx.cap.n != 1)
            fail(name, "unbroken frame %zu was not answered", i);
    }

    ok(name);
    fixture_down(&fx);
}

/*
 * An echo reply carries the request's whole ICMP message back in one
 * frame, so the largest request that fits a 1514-byte frame is answered
 * and one byte more is not: it could only be answered by overrunning the
 * endpoint's frame buffer.
 */
static void test_ping_too_big(void)
{
    const char *name = "ping-too-big";
    struct fixture fx;
    const size_t fits = 1514 - ETH_LEN - IP_LEN - ICMP_LEN;
    char data[1514];

    if (!fixture_up(&fx, name, 0))
        return;

    memset(data, 'p', fits + 1);
    data[fits] = '\0';
    size_t n = build_ping(fx.frame, fx.mac, 1, 1, data);
    if (!offer_exact(&fx, name, n, 10) || fx.cap.n != 1)
        fail(name, "a %zu byte echo was not answered", fits);
    else if (fx.cap.f[0].len != 1514)
        fail(name, "the reply is %zu bytes, want 1514", fx.cap.f[0].len);
    else
        checksums_ok(name, fx.cap.f[0].buf, fx.cap.f[0].len);

    data[fits] = 'p';
    data[fits + 1] = '\0';
    n = build_ping(fx.frame, fx.mac, 1, 2, data);
    cap_reset(&fx.cap);
    if (offer_exact(&fx, name, n, 10))
        fail(name, "a %zu byte echo was consumed", fits + 1);
    if (fx.cap.n != 0)
        fail(name, "a %zu byte echo was answered", fits + 1);

    ok(name);
    fixture_down(&fx);
}

/*
 * s3_tcp_create() needs a target and a transmit hook, and a max_conns of
 * zero selects S3_TCP_MAX_CONNS: that many connections open, and one more
 * is refused.
 */
static void test_create(void)
{
    const char *name = "create";
    struct s3_target_cfg tcfg;
    struct s3_tcp_cfg ncfg;
    struct capture *cap = calloc(1, sizeof(*cap));
    char err[128] = "";
    struct fixture fx;

    if (cap == NULL) {
        fail(name, "out of memory");
        return;
    }
    s3_target_cfg_defaults(&tcfg);
    struct s3_target *target = s3_target_create(&tcfg, err, sizeof(err));
    if (target == NULL) {
        fail(name, "target create: %s", err);
        free(cap);
        return;
    }
    s3_tcp_cfg_from_target(&ncfg, target);

    struct s3_tcp *s =
        s3_tcp_create(&ncfg, NULL, NULL, NULL, cap_tx, cap, err, sizeof(err));
    if (s != NULL || err[0] == '\0')
        fail(name, "created with no target, or failed without saying why");
    s3_tcp_destroy(s);

    err[0] = '\0';
    s = s3_tcp_create(&ncfg, target, NULL, NULL, NULL, NULL, err, sizeof(err));
    if (s != NULL || err[0] == '\0')
        fail(name, "created with no transmit hook, or failed without saying "
                   "why");
    s3_tcp_destroy(s);

    s3_tcp_destroy(NULL); /* like free(), a no-op */
    s3_target_destroy(target);
    free(cap);

    /* fixture_up() leaves max_conns at what s3_tcp_cfg_from_target() set;
     * zero it to ask for the default. */
    memset(&fx, 0, sizeof(fx));
    s3_target_cfg_defaults(&tcfg);
    fx.target = s3_target_create(&tcfg, err, sizeof(err));
    if (fx.target == NULL) {
        fail(name, "target create: %s", err);
        return;
    }
    s3_tcp_cfg_from_target(&ncfg, fx.target);
    ncfg.max_conns = 0;
    memcpy(fx.mac, ncfg.mac, 6);
    fx.tcp = s3_tcp_create(&ncfg, fx.target, NULL, NULL, cap_tx, &fx.cap, err,
                           sizeof(err));
    if (fx.tcp == NULL) {
        fail(name, "create with max_conns 0: %s", err);
        s3_target_destroy(fx.target);
        return;
    }

    for (unsigned i = 0; i < S3_TCP_MAX_CONNS; i++) {
        uint32_t srv = 0;
        if (!handshake(&fx, name, (uint16_t)(50000 + i), 100, &srv, 10))
            goto out;
    }
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, 50999, 100, 0, FLAG_SYN, NULL, 0);
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 10);
    const uint8_t *tcp =
        fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_RST))
        fail(name, "connection %d was not refused", S3_TCP_MAX_CONNS + 1);
    if (s3_tcp_conn_count(fx.tcp) != S3_TCP_MAX_CONNS)
        fail(name, "%u connections open, want %d", s3_tcp_conn_count(fx.tcp),
             S3_TCP_MAX_CONNS);

    ok(name);
out:
    fixture_down(&fx);
}

/* Hand out @len bytes of a fixed pattern as if read from a client buffer,
 * so the store can hold an object larger than any test buffer. */
static uint32_t pattern_from_host(void *ctx, uint32_t key, uint64_t addr,
                                  void *dst, uint32_t len)
{
    (void)ctx;
    (void)key;
    (void)addr;
    memset(dst, 'z', len);
    return len;
}

static const struct s3_dma_ops pattern_dma = {
    .from_host = pattern_from_host,
    .to_host = NULL,
};

/*
 * A response the endpoint cannot queue -- here an object fetched in the
 * HTTP body that is larger than the 16 MiB a connection may have queued
 * -- resets the connection rather than sending part of it.
 */
static void test_response_too_large(void)
{
    const char *name = "response-too-large";
    struct fixture fx;
    uint32_t srv = 0;
    char hex[S3_TOKEN_HEX_LEN + 1];
    char text[512];
    const size_t len = (16u << 20) + 1;

    if (!fixture_up(&fx, name, 0))
        return;

    /* Store the object over the data plane, straight through the target. */
    mint(hex, 0, len);
    int w = snprintf(text, sizeof(text),
                     "PUT /ernic/huge HTTP/1.1\r\nx-amz-rdma-token: %s\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     hex, len);
    struct s3_http_request req;
    struct s3_http_response resp;
    if (w < 0 || (size_t)w >= sizeof(text) ||
        s3_http_parse(text, (size_t)w, &req) != w) {
        fail(name, "the seed PUT did not parse");
        goto out;
    }
    s3_target_exec(fx.target, &req, &pattern_dma, NULL, &resp);
    int status = resp.status;
    s3_http_response_free(&resp);
    if (status != 200) {
        fail(name, "the seed PUT gave status %d", status);
        goto out;
    }

    if (!handshake(&fx, name, GUEST_PORT, 1000, &srv, 10))
        goto out;
    static const char get[] = "GET /ernic/huge HTTP/1.1\r\n\r\n";
    cap_reset(&fx.cap);
    size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                         FLAG_ACK | FLAG_PSH, get, strlen(get));
    s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
    check_all_checksums(name, &fx.cap);

    const uint8_t *tcp =
        fx.cap.n == 1 ? as_tcp(fx.cap.f[0].buf, fx.cap.f[0].len, NULL) : NULL;
    if (tcp == NULL || !(tcp_flags(tcp) & FLAG_RST))
        fail(name, "an unqueueable response drew %u frames, not one RST",
             fx.cap.n);
    if (s3_tcp_conn_count(fx.tcp) != 0)
        fail(name, "the connection survived an unqueueable response");
    if (s3_tcp_statistics(fx.tcp)->requests != 1)
        fail(name, "requests counter %" PRIu64 ", want 1",
             s3_tcp_statistics(fx.tcp)->requests);

    ok(name);
out:
    fixture_down(&fx);
}

/* Each allocation s3_tcp_create() makes, failed in turn, is reported and
 * leaves nothing behind. */
static void test_create_alloc_failures(void)
{
    const char *name = "create-alloc-failures";
    struct s3_target_cfg tcfg;
    struct s3_tcp_cfg ncfg;
    struct capture cap;
    char err[128] = "";
    unsigned nth;

    s3_target_cfg_defaults(&tcfg);
    struct s3_target *target = s3_target_create(&tcfg, err, sizeof(err));
    if (target == NULL) {
        fail(name, "target create: %s", err);
        return;
    }
    s3_tcp_cfg_from_target(&ncfg, target);

    for (nth = 0; nth < 16; nth++) {
        err[0] = '\0';
        s3_alloc_fail_arm(nth);
        struct s3_tcp *s = s3_tcp_create(&ncfg, target, NULL, NULL, cap_tx,
                                         &cap, err, sizeof(err));
        bool fired = s3_alloc_fail_disarm();
        if (!fired) {
            if (s == NULL)
                fail(name, "create failed with nothing failed: %s", err);
            s3_tcp_destroy(s);
            break;
        }
        if (s != NULL || strstr(err, "out of memory") == NULL)
            fail(name, "create with allocation %u failed: %p, \"%s\"", nth,
                 (void *)s, err);
        s3_tcp_destroy(s);
    }
    if (nth < 2)
        fail(name, "only %u allocations could be failed, want 2", nth);

    s3_target_destroy(target);
    ok(name);
}

/* What a connection sent in answer to one request. */
struct answer {
    bool reset;     /* an RST went out */
    bool fin;       /* a FIN went out */
    char first[16]; /* the start of the first data, or "" */
};

static void read_answer(const struct capture *cap, struct answer *a)
{
    memset(a, 0, sizeof(*a));
    for (unsigned i = 0; i < cap->n; i++) {
        size_t plen = 0;
        const uint8_t *tcp = as_tcp(cap->f[i].buf, cap->f[i].len, &plen);
        if (tcp == NULL)
            continue;
        if (tcp_flags(tcp) & FLAG_RST)
            a->reset = true;
        if (tcp_flags(tcp) & FLAG_FIN)
            a->fin = true;
        if (plen > 0 && a->first[0] == '\0') {
            size_t n =
                plen < sizeof(a->first) - 1 ? plen : sizeof(a->first) - 1;
            memcpy(a->first, tcp_payload(cap->f[i].buf), n);
            a->first[n] = '\0';
        }
    }
}

/*
 * A request served with each allocation along the way failed in turn:
 * the receive buffer, the response body, the serialised response and the
 * send buffer.  The connection either answers -- with the response, or
 * with a 413 when there was no room to hold the request -- or is reset;
 * it never sends part of a response, and the endpoint serves the next
 * connection as before.
 */
static void test_request_alloc_failures(void)
{
    const char *name = "request-alloc-failures";
    static const char req[] = "GET /ernic/obj HTTP/1.1\r\n\r\n";
    unsigned nth;

    for (nth = 0; nth < 64; nth++) {
        struct fixture fx;
        struct answer a;
        uint32_t srv = 0;

        if (!fixture_up(&fx, name, 0))
            return;
        if (!seed_object(&fx, name, "obj", 3000) ||
            !handshake(&fx, name, GUEST_PORT, 1000, &srv, 10)) {
            fixture_down(&fx);
            return;
        }

        cap_reset(&fx.cap);
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, 1001, srv,
                             FLAG_ACK | FLAG_PSH, req, strlen(req));
        s3_alloc_fail_arm(nth);
        s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
        bool fired = s3_alloc_fail_disarm();
        check_all_checksums(name, &fx.cap);
        read_answer(&fx.cap, &a);

        if (!fired) {
            if (strncmp(a.first, "HTTP/1.1 200", 12) != 0)
                fail(name, "with nothing failed, answered \"%s\"", a.first);
            fixture_down(&fx);
            break;
        }
        if (a.reset) {
            if (a.first[0] != '\0')
                fail(name, "allocation %u: data before the reset", nth);
            if (s3_tcp_conn_count(fx.tcp) != 0)
                fail(name, "allocation %u: reset but kept the slot", nth);
        } else if (strncmp(a.first, "HTTP/1.1 413", 12) != 0) {
            fail(name, "allocation %u: answered \"%s\", not a 413 or reset",
                 nth, a.first);
        }

        /* Whatever happened, a new connection is served. */
        char body[8192];
        static const char next[] = "GET /ernic/obj HTTP/1.1\r\n"
                                   "Connection: close\r\n\r\n";
        size_t got =
            transact(&fx, name, GUEST_PORT + 1, next, body, sizeof(body), 100);
        if (!is_seeded_object(body, got, 3000))
            fail(name, "allocation %u: the next connection was not served",
                 nth);
        fixture_down(&fx);
    }
    /* The receive buffer, body, serialised response and send buffer. */
    if (nth < 4)
        fail(name, "only %u allocations could be failed, want 4", nth);
    else
        ok(name);
}

/*
 * The 413 for an oversized request with each of its allocations failed:
 * with no 413 to send, the endpoint still closes the connection, by FIN
 * alone.
 */
static void test_refusal_alloc_failures(void)
{
    const char *name = "refusal-alloc-failures";
    uint8_t junk[1460];
    unsigned nth;

    memset(junk, 'A', sizeof(junk));

    for (nth = 0; nth < 64; nth++) {
        struct fixture fx;
        struct answer a;
        uint32_t srv = 0;
        uint32_t seq = 2;

        if (!fixture_up(&fx, name, 0))
            return;
        if (!handshake(&fx, name, GUEST_PORT, 1, &srv, 10)) {
            fixture_down(&fx);
            return;
        }

        /* Fill the buffer to within a segment of the limit. */
        const unsigned whole = S3_HTTP_REQUEST_MAX / (unsigned)sizeof(junk);
        for (unsigned i = 0; i < whole; i++) {
            size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv,
                                 FLAG_ACK, junk, sizeof(junk));
            s3_tcp_rx_frame(fx.tcp, fx.frame, n, 20);
            seq += (uint32_t)sizeof(junk);
        }

        cap_reset(&fx.cap);
        size_t n = build_tcp(fx.frame, fx.mac, GUEST_PORT, seq, srv, FLAG_ACK,
                             junk, sizeof(junk));
        s3_alloc_fail_arm(nth);
        s3_tcp_rx_frame(fx.tcp, fx.frame, n, 30);
        bool fired = s3_alloc_fail_disarm();
        check_all_checksums(name, &fx.cap);
        read_answer(&fx.cap, &a);

        if (!fired) {
            if (strncmp(a.first, "HTTP/1.1 413", 12) != 0)
                fail(name, "with nothing failed, answered \"%s\"", a.first);
            fixture_down(&fx);
            break;
        }
        if (a.first[0] != '\0')
            fail(name, "allocation %u: sent \"%s\" anyway", nth, a.first);
        else if (!a.fin)
            fail(name, "allocation %u: the connection was not closed", nth);
        fixture_down(&fx);
    }
    /* The body, the serialised response and the send buffer. */
    if (nth < 3)
        fail(name, "only %u allocations could be failed, want 3", nth);
    else
        ok(name);
}

int main(void)
{
    test_arp();
    test_ping();
    test_request_response();
    test_rdma_round_trip();
    test_conn_recycle();
    test_split_request();
    test_stray_segment();
    test_conn_limit();
    test_idle_reap();
    test_oversized_request();
    test_oversized_stream();
    test_oversized_exact_fit();
    test_probe_close();
    test_retransmit_data();
    test_retransmit_fin();
    test_retransmit_syn_ack();
    test_mss_option();
    test_zero_window();
    test_reset_segments();
    test_malformed_frames();
    test_ping_too_big();
    test_create();
    test_response_too_large();
    test_create_alloc_failures();
    test_request_alloc_failures();
    test_refusal_alloc_failures();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 tcp tests passed\n");
    return failures;
}
