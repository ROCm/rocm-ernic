/*
 * s3_tcp.c — the in-band TCP endpoint the S3 control plane listens on
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/net_headers.h"
#include "s3_http.h"
#include "s3_tcp.h"

#define S3_TCP_MSS_DEFAULT 1460u
#define S3_TCP_MSS_FLOOR   536u
#define S3_TCP_WINDOW      65535u
#define S3_TCP_RTO_MS      200u
#define S3_TCP_MAX_REXMIT  8u
#define S3_TCP_IDLE_MS     60000u

/*
 * Ceiling on what one connection may have queued towards the guest.  A
 * body-carrying GET of a large object is the only thing that comes near
 * it, and a client that wants a large object should be using the RDMA
 * data plane, which queues nothing here at all.
 */
#define S3_TCP_TXBUF_MAX (16u << 20)

#define FRAME_MAX 1514u

static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

enum s3_conn_state {
    S3_CONN_FREE = 0,
    S3_CONN_SYN_RCVD,
    S3_CONN_ESTABLISHED,
    S3_CONN_CLOSE_WAIT, /* peer sent FIN; we still owe it our own data */
    S3_CONN_LAST_ACK,
    S3_CONN_FIN_WAIT,
};

struct s3_conn {
    enum s3_conn_state state;

    uint8_t peer_mac[6];
    uint32_t peer_ip; /* host byte order */
    uint16_t peer_port;

    uint32_t snd_una; /* first byte the peer has not acknowledged */
    uint32_t rcv_nxt; /* next sequence number we expect */
    uint32_t mss;
    uint32_t peer_win;

    uint8_t *rxbuf;
    size_t rxlen, rxcap;

    uint8_t *txbuf; /* unacknowledged plus unsent, snd_una at [0] */
    size_t txlen, txcap;
    size_t txout; /* how much of txbuf is in flight */

    bool fin_queued; /* send FIN once txbuf has drained */
    bool fin_sent;
    bool fin_acked; /* the peer acknowledged our FIN's sequence number */
    bool peer_fin;

    uint64_t rto_at;
    uint64_t last_rx_ms;
    unsigned rexmits;
};

struct s3_tcp {
    struct s3_tcp_cfg cfg;
    struct s3_target *target;
    const struct s3_dma_ops *dma;
    void *dma_ctx;
    s3_tcp_tx_fn tx;
    void *tx_ctx;

    struct s3_conn *conns;
    unsigned conn_cap;

    struct s3_tcp_stats stats;
    uint32_t iss; /* rotating initial send sequence */
    uint16_t ip_id;
    uint8_t frame[FRAME_MAX];
};

/* ------------------------------------------------------------------ */
/* Checksums                                                          */
/* ------------------------------------------------------------------ */

/*
 * net_headers.h has checksum helpers, but its TCP one substitutes 0xffff
 * for a computed zero.  That is the UDP rule; in TCP a zero checksum is
 * a legal value that must be sent as-is, and substituting for it makes
 * roughly one segment in 65536 arrive corrupt.  Hence these.
 */
static uint32_t csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += (uint32_t)p[0] << 8;
    }
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return htons((uint16_t)~sum);
}

static uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                            uint16_t len)
{
    uint32_t sum = 0;

    sum += (src >> 16) & 0xffff;
    sum += src & 0xffff;
    sum += (dst >> 16) & 0xffff;
    sum += dst & 0xffff;
    sum += proto;
    sum += len;
    return sum;
}

/* ------------------------------------------------------------------ */
/* Sequence arithmetic                                                */
/* ------------------------------------------------------------------ */

static bool seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* ------------------------------------------------------------------ */
/* Frame emission                                                     */
/* ------------------------------------------------------------------ */

static void conn_free(struct s3_conn *c)
{
    free(c->rxbuf);
    free(c->txbuf);
    memset(c, 0, sizeof(*c));
}

static bool buf_reserve(uint8_t **buf, size_t *cap, size_t need, size_t limit)
{
    if (need <= *cap) {
        return true;
    }
    if (need > limit) {
        return false;
    }
    size_t want = *cap ? *cap : 1024;
    while (want < need) {
        want *= 2;
    }
    if (want > limit) {
        want = limit;
    }
    uint8_t *p = realloc(*buf, want);
    if (p == NULL) {
        return false;
    }
    *buf = p;
    *cap = want;
    return true;
}

static void send_frame(struct s3_tcp *s, size_t len)
{
    /* The guest's receive path expects a minimum-length Ethernet frame;
     * the pad is never interpreted because IP carries its own length. */
    if (len < 60) {
        memset(s->frame + len, 0, 60 - len);
        len = 60;
    }
    s->stats.frames_tx++;
    s->tx(s->tx_ctx, s->frame, len);
}

static uint8_t *build_ip(struct s3_tcp *s, const uint8_t dst_mac[6],
                         uint32_t dst_ip, uint8_t proto, size_t payload_len)
{
    struct eth_header *eth = (struct eth_header *)s->frame;
    struct ip_header *ip = (struct ip_header *)(s->frame + sizeof(*eth));

    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, s->cfg.mac, 6);
    eth->ethertype = htons(ETH_ETHERTYPE_IP);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->total_len = htons((uint16_t)(sizeof(*ip) + payload_len));
    ip->id = htons(s->ip_id++);
    ip->frag_off = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->protocol = proto;
    ip->src_ip = htonl(s->cfg.ip);
    ip->dst_ip = htonl(dst_ip);
    ip->checksum = csum_fold(csum_add(0, ip, sizeof(*ip)));

    return s->frame + sizeof(*eth) + sizeof(*ip);
}

static void send_tcp(struct s3_tcp *s, const struct s3_conn *c, uint32_t seq,
                     uint8_t flags, const void *payload, size_t payload_len,
                     bool with_mss)
{
    size_t optlen = with_mss ? 4 : 0;
    size_t hdrlen = sizeof(struct tcp_header) + optlen;

    uint8_t *p = build_ip(s, c->peer_mac, c->peer_ip, IP_PROTOCOL_TCP,
                          hdrlen + payload_len);
    struct tcp_header *tcp = (struct tcp_header *)p;

    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(s->cfg.port);
    tcp->dst_port = htons(c->peer_port);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(c->rcv_nxt);
    tcp->data_off = (uint8_t)((hdrlen / 4) << 4);
    tcp->flags = flags;
    tcp->window = htons(S3_TCP_WINDOW);

    if (with_mss) {
        uint8_t *opt = p + sizeof(*tcp);
        opt[0] = 2; /* MSS */
        opt[1] = 4;
        opt[2] = (uint8_t)(S3_TCP_MSS_DEFAULT >> 8);
        opt[3] = (uint8_t)S3_TCP_MSS_DEFAULT;
    }
    if (payload_len > 0) {
        memcpy(p + hdrlen, payload, payload_len);
    }

    uint32_t sum = csum_pseudo(s->cfg.ip, c->peer_ip, IP_PROTOCOL_TCP,
                               (uint16_t)(hdrlen + payload_len));
    tcp->checksum = csum_fold(csum_add(sum, p, hdrlen + payload_len));

    send_frame(s, sizeof(struct eth_header) + sizeof(struct ip_header) +
                      hdrlen + payload_len);
}

/*
 * A reset for a segment that belongs to no connection.  It has no
 * s3_conn to hang off, so it takes the peer's addressing directly.
 */
static void send_reset(struct s3_tcp *s, const uint8_t peer_mac[6],
                       uint32_t peer_ip, uint16_t peer_port, uint32_t seq,
                       uint32_t ack, uint8_t flags)
{
    struct s3_conn tmp;

    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.peer_mac, peer_mac, 6);
    tmp.peer_ip = peer_ip;
    tmp.peer_port = peer_port;
    tmp.rcv_nxt = ack;

    s->stats.resets++;
    send_tcp(s, &tmp, seq, flags, NULL, 0, false);
}

/* ------------------------------------------------------------------ */
/* ARP and ICMP                                                       */
/* ------------------------------------------------------------------ */

static bool handle_arp(struct s3_tcp *s, const uint8_t *frame, size_t len)
{
    const struct eth_header *eth = (const struct eth_header *)frame;
    const struct arp_header *arp;

    if (len < sizeof(*eth) + sizeof(*arp)) {
        return false;
    }
    arp = (const struct arp_header *)(frame + sizeof(*eth));

    if (ntohs(arp->hw_type) != 1 ||
        ntohs(arp->proto_type) != ETH_ETHERTYPE_IP || arp->hw_addr_len != 6 ||
        arp->proto_addr_len != 4 || ntohs(arp->op) != ARP_OP_REQUEST) {
        return false;
    }
    if (ntohl(arp->target_proto_addr) != s->cfg.ip) {
        return false;
    }

    struct eth_header *reth = (struct eth_header *)s->frame;
    struct arp_header *rarp = (struct arp_header *)(s->frame + sizeof(*reth));

    memcpy(reth->dst_mac, arp->sender_hw_addr, 6);
    memcpy(reth->src_mac, s->cfg.mac, 6);
    reth->ethertype = htons(ETH_ETHERTYPE_ARP);

    rarp->hw_type = htons(1);
    rarp->proto_type = htons(ETH_ETHERTYPE_IP);
    rarp->hw_addr_len = 6;
    rarp->proto_addr_len = 4;
    rarp->op = htons(ARP_OP_REPLY);
    memcpy(rarp->sender_hw_addr, s->cfg.mac, 6);
    rarp->sender_proto_addr = htonl(s->cfg.ip);
    memcpy(rarp->target_hw_addr, arp->sender_hw_addr, 6);
    rarp->target_proto_addr = arp->sender_proto_addr;

    s->stats.arp_replies++;
    send_frame(s, sizeof(*reth) + sizeof(*rarp));
    return true;
}

static bool handle_icmp(struct s3_tcp *s, const uint8_t *frame, size_t len,
                        const struct ip_header *ip, size_t ip_hdr_len)
{
    size_t off = sizeof(struct eth_header) + ip_hdr_len;
    size_t total = ntohs(ip->total_len);

    if (total < ip_hdr_len + sizeof(struct icmp_header) ||
        len < sizeof(struct eth_header) + total) {
        return false;
    }
    const struct icmp_header *icmp = (const struct icmp_header *)(frame + off);
    if (icmp->type != ICMP_TYPE_ECHO_REQUEST) {
        return false;
    }

    size_t icmp_len = total - ip_hdr_len;
    if (sizeof(struct eth_header) + sizeof(struct ip_header) + icmp_len >
        FRAME_MAX) {
        return false;
    }

    const struct eth_header *eth = (const struct eth_header *)frame;
    uint8_t *p = build_ip(s, eth->src_mac, ntohl(ip->src_ip), IP_PROTOCOL_ICMP,
                          icmp_len);
    memcpy(p, icmp, icmp_len);

    struct icmp_header *reply = (struct icmp_header *)p;
    reply->type = ICMP_TYPE_ECHO_REPLY;
    reply->checksum = 0;
    reply->checksum = csum_fold(csum_add(0, p, icmp_len));

    s->stats.pings++;
    send_frame(s,
               sizeof(struct eth_header) + sizeof(struct ip_header) + icmp_len);
    return true;
}

/* ------------------------------------------------------------------ */
/* Connection table                                                   */
/* ------------------------------------------------------------------ */

static struct s3_conn *conn_find(struct s3_tcp *s, uint32_t ip, uint16_t port)
{
    for (unsigned i = 0; i < s->conn_cap; i++) {
        struct s3_conn *c = &s->conns[i];
        if (c->state != S3_CONN_FREE && c->peer_ip == ip &&
            c->peer_port == port) {
            return c;
        }
    }
    return NULL;
}

static struct s3_conn *conn_alloc(struct s3_tcp *s)
{
    for (unsigned i = 0; i < s->conn_cap; i++) {
        if (s->conns[i].state == S3_CONN_FREE) {
            return &s->conns[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Output scheduling                                                  */
/* ------------------------------------------------------------------ */

/*
 * Send whatever the window allows, then a FIN if the queue has drained
 * and one is owed.  Returns true if any segment went out.
 */
static bool conn_pump(struct s3_tcp *s, struct s3_conn *c, uint64_t now_ms)
{
    bool sent = false;

    while (c->txout < c->txlen) {
        size_t avail = c->txlen - c->txout;
        size_t win = c->peer_win > c->txout ? c->peer_win - c->txout : 0;
        if (win == 0) {
            break;
        }
        size_t n = avail;
        if (n > c->mss) {
            n = c->mss;
        }
        if (n > win) {
            n = win;
        }

        uint8_t flags = TCP_FLAG_ACK;
        if (c->txout + n == c->txlen) {
            flags |= TCP_FLAG_PSH;
        }
        send_tcp(s, c, c->snd_una + (uint32_t)c->txout, flags,
                 c->txbuf + c->txout, n, false);
        c->txout += n;
        sent = true;
    }

    if (sent) {
        c->rto_at = now_ms + S3_TCP_RTO_MS;
    }

    if (c->fin_queued && !c->fin_sent && c->txlen == 0) {
        send_tcp(s, c, c->snd_una, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0, false);
        c->fin_sent = true;
        c->state = (c->state == S3_CONN_CLOSE_WAIT) ? S3_CONN_LAST_ACK
                                                    : S3_CONN_FIN_WAIT;
        c->rto_at = now_ms + S3_TCP_RTO_MS;
        sent = true;
    }
    return sent;
}

static bool conn_queue(struct s3_conn *c, const void *data, size_t len)
{
    if (!buf_reserve(&c->txbuf, &c->txcap, c->txlen + len, S3_TCP_TXBUF_MAX)) {
        return false;
    }
    memcpy(c->txbuf + c->txlen, data, len);
    c->txlen += len;
    return true;
}

/* ------------------------------------------------------------------ */
/* Request service                                                    */
/* ------------------------------------------------------------------ */

static bool queue_canned(struct s3_conn *c, int status, const char *code,
                         const char *message)
{
    struct s3_http_response resp;
    size_t len = 0;
    bool ok = false;

    s3_http_response_init(&resp, status);
    resp.keep_alive = false;
    s3_http_response_header(&resp, "Content-Type", "application/xml");
    s3_http_response_printf(&resp,
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<Error><Code>%s</Code><Message>%s</Message>"
                            "</Error>\n",
                            code, message);

    uint8_t *wire = s3_http_response_serialize(&resp, &len);
    if (wire != NULL) {
        ok = conn_queue(c, wire, len);
        free(wire);
    }
    s3_http_response_free(&resp);
    return ok;
}

/*
 * Drain as many complete requests as the receive buffer holds.  Returns
 * false when the connection must be torn down.
 */
static bool conn_service(struct s3_tcp *s, struct s3_conn *c)
{
    size_t consumed = 0;

    /*
     * Once a FIN is queued the answer is already decided, and anything this
     * function appended would be sent past the sequence number the FIN
     * consumes.  The bytes still have to be drained so a peer that keeps
     * writing does not refill the buffer and stall, but nothing in them can
     * be acted on.
     */
    if (c->fin_queued) {
        c->rxlen = 0;
        return true;
    }

    while (consumed < c->rxlen) {
        struct s3_http_request req;
        ssize_t n =
            s3_http_parse(c->rxbuf + consumed, c->rxlen - consumed, &req);
        if (n == 0) {
            break; /* partial request; wait for the rest */
        }
        if (n < 0) {
            int status = (n == -EMSGSIZE) ? 413 : 400;
            const char *code =
                (n == -EMSGSIZE) ? "EntityTooLarge" : "InvalidRequest";
            queue_canned(c, status, code,
                         "The request could not be understood.");
            c->fin_queued = true;
            c->rxlen = 0;
            return true;
        }

        struct s3_http_response resp;
        s3_target_exec(s->target, &req, s->dma, s->dma_ctx, &resp);
        s->stats.requests++;

        size_t wire_len = 0;
        uint8_t *wire = s3_http_response_serialize(&resp, &wire_len);
        bool keep = resp.keep_alive && !resp.oom;
        s3_http_response_free(&resp);

        if (wire == NULL) {
            return false;
        }
        bool queued = conn_queue(c, wire, wire_len);
        free(wire);
        if (!queued) {
            return false;
        }

        consumed += (size_t)n;
        if (!keep) {
            c->fin_queued = true;
            break;
        }
    }

    if (consumed > 0) {
        memmove(c->rxbuf, c->rxbuf + consumed, c->rxlen - consumed);
        c->rxlen -= consumed;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* TCP input                                                          */
/* ------------------------------------------------------------------ */

static uint32_t parse_mss(const uint8_t *opt, size_t len)
{
    size_t i = 0;

    while (i < len) {
        uint8_t kind = opt[i];
        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) {
            break;
        }
        if (kind == 2 && opt[i + 1] == 4) {
            return ((uint32_t)opt[i + 2] << 8) | opt[i + 3];
        }
        i += opt[i + 1];
    }
    return 0;
}

static void conn_reset(struct s3_tcp *s, struct s3_conn *c)
{
    send_reset(s, c->peer_mac, c->peer_ip, c->peer_port, c->snd_una, c->rcv_nxt,
               TCP_FLAG_RST | TCP_FLAG_ACK);
    conn_free(c);
}

static bool handle_tcp(struct s3_tcp *s, const uint8_t *frame, size_t len,
                       const struct ip_header *ip, size_t ip_hdr_len,
                       uint64_t now_ms)
{
    const struct eth_header *eth = (const struct eth_header *)frame;
    size_t total = ntohs(ip->total_len);
    size_t off = sizeof(*eth) + ip_hdr_len;

    if (total < ip_hdr_len + sizeof(struct tcp_header) ||
        len < sizeof(*eth) + total) {
        return false;
    }

    const struct tcp_header *tcp = (const struct tcp_header *)(frame + off);
    size_t tcp_hdr_len = (size_t)((tcp->data_off >> 4) & 0xf) * 4;
    if (tcp_hdr_len < sizeof(*tcp) || ip_hdr_len + tcp_hdr_len > total) {
        return false;
    }
    if (ntohs(tcp->dst_port) != s->cfg.port) {
        return false;
    }

    /* Only now is the segment definitely ours, so only now do frames
     * count as consumed rather than passed on. */
    uint32_t peer_ip = ntohl(ip->src_ip);
    uint16_t peer_port = ntohs(tcp->src_port);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);
    const uint8_t *payload = frame + off + tcp_hdr_len;
    size_t payload_len = total - ip_hdr_len - tcp_hdr_len;

    struct s3_conn *c = conn_find(s, peer_ip, peer_port);

    if (tcp->flags & TCP_FLAG_RST) {
        if (c != NULL) {
            conn_free(c);
        }
        return true;
    }

    if (c == NULL) {
        if (!(tcp->flags & TCP_FLAG_SYN) || (tcp->flags & TCP_FLAG_ACK)) {
            send_reset(s, eth->src_mac, peer_ip, peer_port, ack,
                       seq + (uint32_t)payload_len, TCP_FLAG_RST);
            return true;
        }
        c = conn_alloc(s);
        if (c == NULL) {
            s->stats.refused++;
            send_reset(s, eth->src_mac, peer_ip, peer_port, 0, seq + 1,
                       TCP_FLAG_RST | TCP_FLAG_ACK);
            return true;
        }

        memset(c, 0, sizeof(*c));
        c->state = S3_CONN_SYN_RCVD;
        memcpy(c->peer_mac, eth->src_mac, 6);
        c->peer_ip = peer_ip;
        c->peer_port = peer_port;
        c->rcv_nxt = seq + 1;
        c->snd_una = s->iss;
        s->iss += 0x01000000u;
        c->peer_win = ntohs(tcp->window);
        c->mss = S3_TCP_MSS_DEFAULT;

        uint32_t pm =
            parse_mss(frame + off + sizeof(*tcp), tcp_hdr_len - sizeof(*tcp));
        if (pm >= S3_TCP_MSS_FLOOR && pm < c->mss) {
            c->mss = pm;
        }
        c->last_rx_ms = now_ms;

        send_tcp(s, c, c->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, true);
        c->rto_at = now_ms + S3_TCP_RTO_MS;
        s->stats.accepts++;
        return true;
    }

    c->last_rx_ms = now_ms;
    memcpy(c->peer_mac, eth->src_mac, 6);
    c->peer_win = ntohs(tcp->window);
    if (c->peer_win == 0) {
        c->peer_win = 1; /* keep the pump probing rather than wedging */
    }

    if (c->state == S3_CONN_SYN_RCVD) {
        if (!(tcp->flags & TCP_FLAG_ACK)) {
            return true;
        }
        /* Our SYN consumed one sequence number; the ACK for it moves
         * snd_una past it and data starts at the new snd_una. */
        c->snd_una = ack;
        c->state = S3_CONN_ESTABLISHED;
        c->rexmits = 0;
        c->rto_at = 0;
    }

    if (tcp->flags & TCP_FLAG_ACK) {
        uint32_t acked = ack - c->snd_una;
        if ((int32_t)acked > 0) {
            if (acked > c->txout) {
                acked = (uint32_t)c->txout; /* never ACK what we never sent */
            }
            if (acked > 0) {
                memmove(c->txbuf, c->txbuf + acked, c->txlen - acked);
                c->txlen -= acked;
                c->txout -= acked;
                c->snd_una += acked;
            }
            /* An ACK of our own FIN clamps to zero on a connection that
             * never queued a byte, so there is no send buffer to shuffle.
             * The timer reset stays outside the guard: nothing else
             * disarms the FIN's retransmit. */
            c->rexmits = 0;
            c->rto_at = c->txout > 0 ? now_ms + S3_TCP_RTO_MS : 0;
        }
        if (c->fin_sent && seq_lt(c->snd_una, ack)) {
            /* The FIN's own sequence number was acknowledged. */
            c->snd_una = ack;
            c->fin_acked = true;
            if (c->state == S3_CONN_LAST_ACK) {
                conn_free(c);
                return true;
            }
        }
    }

    bool need_ack = false;

    if (payload_len > 0) {
        if (seq == c->rcv_nxt) {
            if (!buf_reserve(&c->rxbuf, &c->rxcap, c->rxlen + payload_len,
                             S3_HTTP_REQUEST_MAX)) {
                /* The rest of the body still has to be absorbed so the
                 * peer can finish and close, but only the first segment
                 * over the limit is answered: a second response would be
                 * queued behind a FIN that has already gone out. */
                if (!c->fin_queued) {
                    queue_canned(c, 413, "EntityTooLarge",
                                 "The request exceeds the server's limit.");
                    c->fin_queued = true;
                }
                c->rcv_nxt += (uint32_t)payload_len;
                conn_pump(s, c, now_ms);
                send_tcp(s, c, c->snd_una + (uint32_t)c->txout, TCP_FLAG_ACK,
                         NULL, 0, false);
                return true;
            }
            memcpy(c->rxbuf + c->rxlen, payload, payload_len);
            c->rxlen += payload_len;
            c->rcv_nxt += (uint32_t)payload_len;

            if (!conn_service(s, c)) {
                conn_reset(s, c);
                return true;
            }
        }
        /* Out-of-order or duplicate: the ACK below tells the peer what we
         * are actually waiting for, and it retransmits. */
        need_ack = true;
    }

    if ((tcp->flags & TCP_FLAG_FIN) && seq_le(seq, c->rcv_nxt) &&
        !c->peer_fin) {
        c->peer_fin = true;
        c->rcv_nxt++;
        c->fin_queued = true;
        if (c->state == S3_CONN_ESTABLISHED) {
            c->state = S3_CONN_CLOSE_WAIT;
        }
        need_ack = true;
    }

    bool pumped = conn_pump(s, c, now_ms);
    if (need_ack && !pumped && c->state != S3_CONN_FREE) {
        send_tcp(s, c, c->snd_una + (uint32_t)c->txout, TCP_FLAG_ACK, NULL, 0,
                 false);
    }

    /*
     * Both directions are closed: we sent the FIN first because the
     * request asked for Connection: close, it has been acknowledged, and
     * the peer has now sent its own.  Nothing further can arrive, so the
     * slot goes back now.  Leaving it to the idle reaper would hold it
     * for a minute, and a client that opens one connection per request
     * -- which is what Connection: close means -- exhausts every slot
     * long before then and is refused with an RST it cannot explain.
     */
    if (c->state == S3_CONN_FIN_WAIT && c->peer_fin && c->fin_acked &&
        c->txlen == 0) {
        conn_free(c);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

void s3_tcp_cfg_from_target(struct s3_tcp_cfg *cfg,
                            const struct s3_target *target)
{
    const struct s3_target_cfg *tc = s3_target_config(target);

    memset(cfg, 0, sizeof(*cfg));
    cfg->ip = tc->traddr;
    cfg->port = tc->trsvcid;
    cfg->max_conns = S3_TCP_MAX_CONNS;

    /* A locally administered address derived from the IP, so the guest
     * always sees the same MAC for the same endpoint across restarts. */
    cfg->mac[0] = 0x02;
    cfg->mac[1] = 0x53; /* 'S' */
    cfg->mac[2] = (uint8_t)(tc->traddr >> 24);
    cfg->mac[3] = (uint8_t)(tc->traddr >> 16);
    cfg->mac[4] = (uint8_t)(tc->traddr >> 8);
    cfg->mac[5] = (uint8_t)tc->traddr;
}

struct s3_tcp *s3_tcp_create(const struct s3_tcp_cfg *cfg,
                             struct s3_target *target,
                             const struct s3_dma_ops *dma, void *dma_ctx,
                             s3_tcp_tx_fn tx, void *tx_ctx, char *err,
                             size_t errlen)
{
    if (target == NULL || tx == NULL) {
        set_err(err, errlen, "s3 tcp needs a target and a transmit hook");
        return NULL;
    }

    struct s3_tcp *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    s->cfg = *cfg;
    if (s->cfg.max_conns == 0) {
        s->cfg.max_conns = S3_TCP_MAX_CONNS;
    }
    s->conn_cap = s->cfg.max_conns;
    s->conns = calloc(s->conn_cap, sizeof(*s->conns));
    if (s->conns == NULL) {
        set_err(err, errlen, "out of memory");
        free(s);
        return NULL;
    }

    s->target = target;
    s->dma = dma;
    s->dma_ctx = dma_ctx;
    s->tx = tx;
    s->tx_ctx = tx_ctx;
    s->iss = 0x1000u;
    return s;
}

void s3_tcp_destroy(struct s3_tcp *s)
{
    if (s == NULL) {
        return;
    }
    for (unsigned i = 0; i < s->conn_cap; i++) {
        conn_free(&s->conns[i]);
    }
    free(s->conns);
    free(s);
}

bool s3_tcp_rx_frame(struct s3_tcp *s, const void *frame, size_t len,
                     uint64_t now_ms)
{
    const uint8_t *f = frame;

    if (len < sizeof(struct eth_header)) {
        return false;
    }
    const struct eth_header *eth = (const struct eth_header *)f;

    /* Unicast to us or broadcast; anything else is not ours to answer. */
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (memcmp(eth->dst_mac, s->cfg.mac, 6) != 0 &&
        memcmp(eth->dst_mac, bcast, 6) != 0) {
        return false;
    }

    s->stats.frames_rx++;

    uint16_t ethertype = ntohs(eth->ethertype);
    if (ethertype == ETH_ETHERTYPE_ARP) {
        return handle_arp(s, f, len);
    }
    if (ethertype != ETH_ETHERTYPE_IP) {
        return false;
    }
    if (len < sizeof(*eth) + sizeof(struct ip_header)) {
        return false;
    }

    const struct ip_header *ip = (const struct ip_header *)(f + sizeof(*eth));
    if ((ip->version_ihl >> 4) != 4) {
        return false;
    }
    size_t ip_hdr_len = (size_t)(ip->version_ihl & 0x0f) * 4;
    if (ip_hdr_len < sizeof(*ip) || ntohs(ip->total_len) < ip_hdr_len ||
        len < sizeof(*eth) + ip_hdr_len) {
        return false;
    }
    if (ntohl(ip->dst_ip) != s->cfg.ip) {
        return false;
    }
    /* A fragmented control-plane request would be a bug somewhere else. */
    if ((ntohs(ip->frag_off) & 0x3fff) != 0) {
        return false;
    }

    if (ip->protocol == IP_PROTOCOL_ICMP) {
        return handle_icmp(s, f, len, ip, ip_hdr_len);
    }
    if (ip->protocol == IP_PROTOCOL_TCP) {
        return handle_tcp(s, f, len, ip, ip_hdr_len, now_ms);
    }
    return false;
}

bool s3_tcp_poll(struct s3_tcp *s, uint64_t now_ms)
{
    bool did = false;

    for (unsigned i = 0; i < s->conn_cap; i++) {
        struct s3_conn *c = &s->conns[i];
        if (c->state == S3_CONN_FREE) {
            continue;
        }

        if (now_ms - c->last_rx_ms > S3_TCP_IDLE_MS) {
            conn_reset(s, c);
            did = true;
            continue;
        }

        if (c->rto_at != 0 && now_ms >= c->rto_at) {
            if (++c->rexmits > S3_TCP_MAX_REXMIT) {
                conn_reset(s, c);
                did = true;
                continue;
            }
            s->stats.retransmits++;
            if (c->state == S3_CONN_SYN_RCVD) {
                send_tcp(s, c, c->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0,
                         true);
                c->rto_at = now_ms + (S3_TCP_RTO_MS << c->rexmits);
            } else if (c->fin_sent && c->txlen == 0) {
                send_tcp(s, c, c->snd_una, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0,
                         false);
                c->rto_at = now_ms + (S3_TCP_RTO_MS << c->rexmits);
            } else {
                c->txout = 0; /* go back to the last unacknowledged byte */
                c->rto_at = 0;
                conn_pump(s, c, now_ms);
                c->rto_at = now_ms + (S3_TCP_RTO_MS << c->rexmits);
            }
            did = true;
            continue;
        }

        if (conn_pump(s, c, now_ms)) {
            did = true;
        }
    }
    return did;
}

bool s3_tcp_has_work(const struct s3_tcp *s)
{
    for (unsigned i = 0; i < s->conn_cap; i++) {
        const struct s3_conn *c = &s->conns[i];
        if (c->state == S3_CONN_FREE) {
            continue;
        }
        if (c->txout < c->txlen || c->rto_at != 0 ||
            (c->fin_queued && !c->fin_sent)) {
            return true;
        }
    }
    return false;
}

const struct s3_tcp_stats *s3_tcp_statistics(const struct s3_tcp *s)
{
    return &s->stats;
}

unsigned s3_tcp_conn_count(const struct s3_tcp *s)
{
    unsigned n = 0;

    for (unsigned i = 0; i < s->conn_cap; i++) {
        if (s->conns[i].state != S3_CONN_FREE) {
            n++;
        }
    }
    return n;
}
