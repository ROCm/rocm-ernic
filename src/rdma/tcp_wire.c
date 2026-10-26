/*
 * RDMA Backend: TCP mesh - sockets, and messages on them
 *
 * Connecting, listening and accepting, and writing and reading one mesh
 * message: the fixed header (TcpMsgHeader), big-endian, and its payload.
 * Nothing here knows what a message means; see tcp_conn.c for the threads that
 * send and receive them, and the handlers they go to.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma_backend_tcp_priv.h"
#include "byteorder.h"
#include "rdma_utils.h"
#include "ernic_time.h"
#include "parse_int.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <inttypes.h>

/* Linux gives EWOULDBLOCK the same value as EAGAIN, so errno checks in
 * this file test EAGAIN alone. */
_Static_assert(EWOULDBLOCK == EAGAIN, "EWOULDBLOCK must equal EAGAIN");

/* Tunable default -- overridable via ERNIC_TCP_LISTEN_BACKLOG */
#define TCP_DEFAULT_LISTEN_BACKLOG 32

/*
 * Size the socket buffers.  Doing nothing is the fast default: Linux autotunes
 * up to tcp_wmem[2]/tcp_rmem[2], which are megabytes, whereas any explicit
 * SO_SNDBUF/SO_RCVBUF is both clamped to net.core.{w,r}mem_max -- typically
 * 208 KiB -- and latches SOCK_{SND,RCV}BUF_LOCK, so the buffer can never grow
 * again.  Asking for 4 MiB that way yields a socket an order of magnitude
 * smaller than leaving it alone, which throttles the large-message path.
 * ERNIC_TCP_SOCKBUF remains for hosts that really do want a fixed size.
 */
void tcp_tune_sockbuf(int sockfd)
{
    const char *val = getenv("ERNIC_TCP_SOCKBUF");
    int bufsize, got;
    socklen_t len;

    if (!val || !ernic_parse_int(val, &bufsize) || bufsize <= 0)
        return;

    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    len = sizeof(got);
    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &got, &len) == 0 &&
        got / 2 < bufsize) {
        rdma_warn_report("TCP: ERNIC_TCP_SOCKBUF=%d clamped to %d by "
                         "net.core.wmem_max; raise it or unset the variable "
                         "to let the kernel autotune",
                         bufsize, got / 2);
    }
}

/*
 * Wait for the non-blocking connect() in progress on sockfd to finish.
 * Returns 0 once it has succeeded, or -1 with errno set: to the connect's
 * own error, to ETIMEDOUT once ernic_monotonic_ms() reaches deadline_ms (if
 * deadline_ms isn't zero), or to ECANCELED once *running is cleared (if
 * running isn't NULL).
 */
static int tcp_wait_connected(int sockfd, const atomic_bool *running,
                              uint64_t deadline_ms)
{
    for (;;) {
        struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};
        int err = 0;
        socklen_t len = sizeof(err);
        int ret;

        if (running && !atomic_load(running)) {
            errno = ECANCELED;
            return -1;
        }
        if (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        ret = poll(&pfd, 1, 100);
        if (ret < 0 && errno != EINTR) {
            return -1;
        }
        if (ret <= 0) {
            continue;
        }
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            return -1;
        }
        if (err != 0) {
            errno = err;
            return -1;
        }
        return 0;
    }
}

/*
 * Connect to each address in addrs in turn until one connects, all within
 * the one deadline (see tcp_wait_connected()).  Returns the socket,
 * non-blocking, or -1 with errno set by the last attempt.  Gives up on the
 * remaining addresses once the deadline passes or *running is cleared.
 *
 * With a deadline, an address gets only its share of the time left, so one
 * that doesn't answer leaves time for those after it.  Otherwise a name
 * whose first address drops connection requests, such as an IPv6 address
 * on a network that doesn't route IPv6, would use up every attempt's
 * deadline, and its other addresses would never be tried.
 */
int tcp_connect_any(const struct addrinfo *addrs, const atomic_bool *running,
                    uint64_t deadline_ms)
{
    int err = ENOENT;
    uint32_t left = 0;

    for (const struct addrinfo *rp = addrs; rp; rp = rp->ai_next) {
        left++;
    }

    for (const struct addrinfo *rp = addrs; rp; rp = rp->ai_next, left--) {
        uint64_t attempt_ms = deadline_ms;
        uint64_t now_ms;

        if (deadline_ms != 0 && left > 1) {
            now_ms = ernic_monotonic_ms();
            if (now_ms < deadline_ms) {
                attempt_ms = now_ms + (deadline_ms - now_ms) / left;
            }
        }

        int sockfd = socket(rp->ai_family,
                            rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                            rp->ai_protocol);
        if (sockfd < 0) {
            err = errno;
            continue;
        }

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0 ||
            (errno == EINPROGRESS &&
             tcp_wait_connected(sockfd, running, attempt_ms) == 0))
            return sockfd;

        err = errno;
        close(sockfd);
        /*
         * Any other failure, including the kernel giving up on this address
         * with ETIMEDOUT or its share of the time running out, moves on to
         * the next address.
         */
        if (err == ECANCELED ||
            (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms))
            break;
    }
    errno = err;
    return -1;
}

/*
 * host and port as a newly allocated "host:port", or "[host]:port" when host is
 * an IPv6 address, for a log message.  g_free() it.
 */
char *tcp_addr_str(const char *host, uint16_t port)
{
    return g_strdup_printf(strchr(host, ':') ? "[%s]:%u" : "%s:%u", host,
                           (unsigned)port);
}

/*
 * Connect to host:port and return the connected socket, non-blocking, or -1.
 * host is a name or an IPv4 or IPv6 address, and a name is tried at each
 * address it resolves to, of either family.
 *
 * The connect itself is non-blocking, so a host that doesn't answer doesn't
 * hold the caller until the kernel gives up on it, about two minutes.  A
 * caller that passes its thread's stop flag in running gets control back
 * within 100 ms of the flag being cleared, and a nonzero deadline_ms, a time
 * on ernic_monotonic_ms()'s clock, gives up then, however many addresses host
 * has.  With neither, it waits as long as a blocking connect() would.
 * Resolving host can still block.
 */
int tcp_connect_to_remote(const char *host, uint16_t port,
                          const atomic_bool *running, uint64_t deadline_ms)
{
    struct addrinfo hints, *res;
    char port_str[8];
    char *addr;
    int sockfd;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        rdma_error_report("TCP: Failed to resolve host '%s': %s", host,
                          gai_strerror(rc));
        return -1;
    }

    sockfd = tcp_connect_any(res, running, deadline_ms);
    err = errno;
    freeaddrinfo(res);

    addr = tcp_addr_str(host, port);
    if (sockfd < 0 && err == ECANCELED) {
        rdma_info_report("TCP: Stopped while connecting to %s", addr);
    } else if (sockfd < 0) {
        rdma_error_report("TCP: Failed to connect to %s: %s", addr,
                          strerror(err));
    } else {
        /* Disable Nagle -- send immediately */
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        tcp_tune_sockbuf(sockfd);

        rdma_info_report("TCP: Connected to %s", addr);
    }
    g_free(addr);
    return sockfd;
}

/*
 * Listen on *port, on every address of family, AF_INET6 or AF_INET.  An
 * AF_INET6 socket takes IPv4 connections too, which arrive from
 * ::ffff:a.b.c.d.  If *port is 0, the kernel picks a free port, which is
 * stored in *port.  Asking for it on the socket that keeps it is the only way
 * to be sure no other process has taken it.  Returns the socket, or -1 with
 * errno set, having logged why unless the host doesn't support family
 * (EAFNOSUPPORT).
 */
int tcp_listen_socket(int family, uint16_t *port)
{
    union {
        struct sockaddr sa;
        struct sockaddr_in in;
        struct sockaddr_in6 in6;
    } addr;
    socklen_t addr_len;
    uint16_t bound;
    const char *what;
    int sockfd;
    int err;
    int on = 1;
    int off = 0;

    memset(&addr, 0, sizeof(addr));
    if (family == AF_INET6) {
        addr.in6.sin6_family = AF_INET6;
        addr.in6.sin6_addr = in6addr_any;
        addr.in6.sin6_port = htons(*port);
        addr_len = sizeof(addr.in6);
    } else if (family == AF_INET) {
        addr.in.sin_family = AF_INET;
        addr.in.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.in.sin_port = htons(*port);
        addr_len = sizeof(addr.in);
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    sockfd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
        err = errno;
        if (err != EAFNOSUPPORT) {
            rdma_error_report("TCP: Failed to create listen socket: %s",
                              strerror(err));
        }
        errno = err;
        return -1;
    }

    what = "set SO_REUSEADDR on";
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        goto fail;
    }

    /* Linux takes both families by default, but net.ipv6.bindv6only can
     * change that */
    what = "set IPV6_V6ONLY on";
    if (family == AF_INET6 &&
        setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        goto fail;
    }

    what = "bind";
    if (bind(sockfd, &addr.sa, addr_len) < 0) {
        goto fail;
    }

    what = "read the port of";
    addr_len = sizeof(addr);
    if (getsockname(sockfd, &addr.sa, &addr_len) < 0) {
        goto fail;
    }
    bound = ntohs(family == AF_INET6 ? addr.in6.sin6_port : addr.in.sin_port);

    what = "listen on";
    if (listen(sockfd, tcp_env_int("ERNIC_TCP_LISTEN_BACKLOG",
                                   TCP_DEFAULT_LISTEN_BACKLOG)) < 0) {
        goto fail;
    }

    *port = bound;
    rdma_info_report("TCP: Listening on port %u (%s)", *port,
                     family == AF_INET6 ? "IPv4 and IPv6" : "IPv4 only");
    return sockfd;

fail:
    err = errno;
    rdma_error_report("TCP: Failed to %s the listen socket on port %u: %s",
                      what, *port, strerror(err));
    close(sockfd);
    errno = err;
    return -1;
}

/*
 * Listen on *port (see tcp_listen_socket()) on every IPv4 and IPv6 address,
 * or on every IPv4 one on a host without IPv6, such as one booted with
 * ipv6.disable=1.
 */
int tcp_listen_on_port(uint16_t *port)
{
    int sockfd = tcp_listen_socket(AF_INET6, port);

    if (sockfd < 0 && errno == EAFNOSUPPORT) {
        sockfd = tcp_listen_socket(AF_INET, port);
    }
    return sockfd;
}

/*
 * Format the address of an accepted peer, ss, into host, len bytes, and
 * *port.  An IPv4 peer on an IPv6 socket, ::ffff:a.b.c.d, is shown as
 * a.b.c.d, as an IPv4 socket would show it.  Returns false with errno set if
 * ss isn't an IPv4 or IPv6 address or host is too small (INET6_ADDRSTRLEN
 * always fits).
 */
bool tcp_format_peer_addr(const struct sockaddr_storage *ss, char *host,
                          size_t len, uint16_t *port)
{
    const void *in_addr;
    int family = ss->ss_family;

    if (family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;

        in_addr = &sin->sin_addr;
        *port = ntohs(sin->sin_port);
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;

        in_addr = &sin6->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
            /* The IPv4 address is the last 4 bytes */
            in_addr = &sin6->sin6_addr.s6_addr[12];
            family = AF_INET;
        }
        *port = ntohs(sin6->sin6_port);
    } else {
        errno = EAFNOSUPPORT;
        return false;
    }
    /* len only matters to inet_ntop() up to INET6_ADDRSTRLEN */
    if (len > INET6_ADDRSTRLEN) {
        len = INET6_ADDRSTRLEN;
    }
    return inet_ntop(family, in_addr, host, (socklen_t)len) != NULL;
}

/*
 * How long a send to a peer may go without a byte leaving before it gives up
 * (see tcp_send_message2()).  A manager gives up in half the time a worker
 * does, so that when a worker stops reading, the manager's relay to it gives
 * up, and the manager reads the sending worker again, before the sending
 * worker gives up on its connection to the manager.
 */
uint32_t tcp_send_timeout_ms(const TcpBackendPrivate *priv)
{
    uint32_t ms =
        atomic_load_explicit(&priv->send_timeout_ms, memory_order_relaxed);

    return priv->is_manager ? ms / 2 + ms % 2 : ms;
}

/* Write @hdr to the TCP_MSG_HDR_LEN bytes at @wire. */
void tcp_msg_hdr_put(uint8_t *wire, const TcpMsgHeader *hdr)
{
    ernic_put_be32(wire + TCP_MSG_HDR_MAGIC_OFF, hdr->magic);
    ernic_put_be32(wire + TCP_MSG_HDR_TYPE_OFF, hdr->msg_type);
    ernic_put_be32(wire + TCP_MSG_HDR_MSG_LEN_OFF, hdr->msg_len);
    ernic_put_be32(wire + TCP_MSG_HDR_SEQ_OFF, hdr->seq);
    ernic_put_be32(wire + TCP_MSG_HDR_SRC_NODE_OFF, hdr->src_node_id);
    ernic_put_be32(wire + TCP_MSG_HDR_DST_NODE_OFF, hdr->dst_node_id);
    ernic_put_be32(wire + TCP_MSG_HDR_SRC_QPN_OFF, hdr->src_qpn);
    ernic_put_be32(wire + TCP_MSG_HDR_DST_QPN_OFF, hdr->dst_qpn);
}

/* Read the TCP_MSG_HDR_LEN bytes at @wire into @hdr. */
void tcp_msg_hdr_get(TcpMsgHeader *hdr, const uint8_t *wire)
{
    hdr->magic = ernic_get_be32(wire + TCP_MSG_HDR_MAGIC_OFF);
    hdr->msg_type = ernic_get_be32(wire + TCP_MSG_HDR_TYPE_OFF);
    hdr->msg_len = ernic_get_be32(wire + TCP_MSG_HDR_MSG_LEN_OFF);
    hdr->seq = ernic_get_be32(wire + TCP_MSG_HDR_SEQ_OFF);
    hdr->src_node_id = ernic_get_be32(wire + TCP_MSG_HDR_SRC_NODE_OFF);
    hdr->dst_node_id = ernic_get_be32(wire + TCP_MSG_HDR_DST_NODE_OFF);
    hdr->src_qpn = ernic_get_be32(wire + TCP_MSG_HDR_SRC_QPN_OFF);
    hdr->dst_qpn = ernic_get_be32(wire + TCP_MSG_HDR_DST_QPN_OFF);
}

/*
 * Send a message on conn.  The caller holds conn->lock, which serialises
 * writes to the socket, and has checked conn->sockfd under it.
 *
 * The payload may be given in two parts so a caller that already holds a
 * header and a body separately does not have to splice them into one buffer
 * first -- sendmsg() sends them as one message either way.
 *
 * The socket is non-blocking, so when the peer isn't reading fast enough the
 * loop waits for room, but only for tcp_send_timeout_ms() without a byte
 * leaving: a peer that stays connected but stops reading would otherwise
 * hold conn->lock, and everyone waiting for it, indefinitely.  Once
 * priv->stopping is set it fails instead of waiting at all; teardown is about
 * to shut the socket down anyway.
 *
 * A send that fails once part of the message may be on the wire leaves the
 * stream unusable, so it shuts the connection down.  Senders waiting for
 * conn->lock then fail at once with EPIPE rather than each waiting out a
 * timeout of its own, and the receive thread sees end-of-file and returns,
 * which has the connection retired: by the health check on a manager, by
 * the peer-connect thread's sweep on a worker, and, for a worker's
 * manager_conn, by its next pass, which registers again.
 */
int tcp_send_message2(const TcpBackendPrivate *priv, TcpConnection *conn,
                      TcpMsgType msg_type, const void *payload,
                      size_t payload_len, const void *payload2,
                      size_t payload2_len, uint32_t seq, uint32_t src_node,
                      uint32_t dst_node, uint32_t src_qpn, uint32_t dst_qpn)
{
    int sockfd = conn->sockfd;
    uint32_t timeout_ms = tcp_send_timeout_ms(priv);
    uint64_t progress_ms;
    TcpMsgHeader hdr;
    uint8_t wire[TCP_MSG_HDR_LEN];
    ssize_t ret;

    /* The receiver drops the connection on anything larger. */
    if (payload_len > TCP_MAX_PAYLOAD_LEN ||
        payload2_len > TCP_MAX_PAYLOAD_LEN - payload_len) {
        rdma_error_report("TCP: payload too large: %zu + %zu", payload_len,
                          payload2_len);
        return -1;
    }
    if (sockfd < 0) {
        return -1;
    }

    hdr.magic = TCP_PROTOCOL_MAGIC;
    hdr.msg_type = msg_type;
    hdr.msg_len = (uint32_t)(payload_len + payload2_len);
    hdr.seq = seq;
    hdr.src_node_id = src_node;
    hdr.dst_node_id = dst_node;
    hdr.src_qpn = src_qpn;
    hdr.dst_qpn = dst_qpn;
    tcp_msg_hdr_put(wire, &hdr);

    struct iovec iov[3];
    int iovcnt = 1;
    size_t total = sizeof(wire);

    iov[0].iov_base = wire;
    iov[0].iov_len = sizeof(wire);

    /* struct iovec serves readv() as well as sendmsg(), so iov_base is not
     * const even though sendmsg() only reads through it. */
    ROCM_ERNIC_WARN_CAST_AWAY_CONST_OFF
    if (payload && payload_len > 0) {
        iov[iovcnt].iov_base = (void *)payload;
        iov[iovcnt].iov_len = payload_len;
        total += payload_len;
        iovcnt++;
    }
    if (payload2 && payload2_len > 0) {
        iov[iovcnt].iov_base = (void *)payload2;
        iov[iovcnt].iov_len = payload2_len;
        total += payload2_len;
        iovcnt++;
    }
    ROCM_ERNIC_WARN_CAST_AWAY_CONST_ON

    size_t sent = 0;

    progress_ms = ernic_monotonic_ms();
    while (sent < total) {
        struct iovec cur[3];
        size_t cur_cnt = 0;
        size_t skip = sent;
        struct msghdr msg;

        for (int i = 0; i < iovcnt; i++) {
            if (skip >= iov[i].iov_len) {
                skip -= iov[i].iov_len;
                continue;
            }
            cur[cur_cnt].iov_base = (char *)iov[i].iov_base + skip;
            cur[cur_cnt].iov_len = iov[i].iov_len - skip;
            cur_cnt++;
            skip = 0;
        }

        /*
         * MSG_NOSIGNAL: a peer that has gone, or a socket that teardown has
         * shut down to wake this loop, fails with EPIPE rather than raising
         * a SIGPIPE that would kill the server.
         */
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = cur;
        msg.msg_iovlen = cur_cnt;
        ret = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 && errno == EAGAIN) {
            struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};

            if (atomic_load(&priv->stopping)) {
                rdma_info_report("TCP: Stopped while sending message");
                goto broken;
            }
            if (ernic_monotonic_ms() - progress_ms >= timeout_ms) {
                rdma_error_report("TCP: Node %u has read nothing for %" PRIu32
                                  " ms; closing the connection",
                                  conn->node_id, timeout_ms);
                goto broken;
            }
            poll(&pfd, 1, 5);
            continue;
        }
        if (ret < 0) {
            rdma_error_report("TCP: Failed to send message to node %u: %s",
                              conn->node_id, strerror(errno));
            goto broken;
        }
        sent += (size_t)ret;
        progress_ms = ernic_monotonic_ms();
    }

    return 0;

broken:
    shutdown(sockfd, SHUT_RDWR);
    return -1;
}

int tcp_send_message(const TcpBackendPrivate *priv, TcpConnection *conn,
                     TcpMsgType msg_type, const void *payload,
                     size_t payload_len, uint32_t seq, uint32_t src_node,
                     uint32_t dst_node, uint32_t src_qpn, uint32_t dst_qpn)
{
    return tcp_send_message2(priv, conn, msg_type, payload, payload_len, NULL,
                             0, seq, src_node, dst_node, src_qpn, dst_qpn);
}

/*
 * Read from a non-blocking socket into buf until it holds len bytes, of which
 * the first got are already there, waiting for each part as it comes.  A peer
 * can withhold the rest, or trickle it, indefinitely, so the wait gives up
 * once *running is cleared, if running isn't NULL (otherwise the thread's stop
 * would wait on the peer), or once ernic_monotonic_ms() reaches deadline_ms, if
 * deadline_ms isn't zero.  what names the part being read, for the log.
 */
static int tcp_recv_rest(int sockfd, void *buf, size_t len, size_t got,
                         const atomic_bool *running, uint64_t deadline_ms,
                         const char *what)
{
    ssize_t ret;

    while (got < len) {
        if (running && !atomic_load(running)) {
            rdma_info_report("TCP: Stopped while receiving %s", what);
            return -1;
        }
        if (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms) {
            rdma_error_report("TCP: Timed out receiving %s", what);
            return -1;
        }
        ret = recv(sockfd, (char *)buf + got, len - got, 0);
        if (ret < 0) {
            if (errno == EAGAIN) {
                struct pollfd pfd = {.fd = sockfd, .events = POLLIN};
                poll(&pfd, 1, 5);
                continue;
            }
            rdma_error_report("TCP: Failed to receive %s: %s", what,
                              strerror(errno));
            return -1;
        }
        if (ret == 0) {
            rdma_info_report("TCP: Connection closed during %s", what);
            return -1;
        }
        got += (size_t)ret;
    }

    return 0;
}

/*
 * Receive one message.  The socket is non-blocking.  If nothing has arrived,
 * this returns -EAGAIN for the caller to wait.  Once any of the message has,
 * it reads the whole message, waiting for the rest with tcp_recv_rest():
 * returning early would lose the bytes already read, and the next call would
 * start in the middle of the message.  A header can straddle a TCP segment
 * boundary, so this happens to the header as well as to the payload.  running
 * and deadline_ms bound that wait, as tcp_recv_rest() describes.
 *
 * *payload is set on every return, to NULL unless a payload was received, so
 * a caller that frees it after a failure never frees a buffer from an earlier
 * message.  It is from malloc(), not g_malloc(), so that it can be handed to
 * the ionic data path, which frees it with free().
 */
int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload,
                     const atomic_bool *running, uint64_t deadline_ms)
{
    uint8_t wire[TCP_MSG_HDR_LEN];
    ssize_t ret;

    *payload = NULL;

    /* Receive header */
    ret = recv(sockfd, wire, sizeof(wire), 0);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return -EAGAIN;
        }
        rdma_error_report("TCP: Failed to receive header: %s", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        rdma_info_report("TCP: Connection closed by peer");
        return -1;
    }
    if (tcp_recv_rest(sockfd, wire, sizeof(wire), (size_t)ret, running,
                      deadline_ms, "header") < 0) {
        return -1;
    }
    tcp_msg_hdr_get(hdr, wire);

    /* Validate magic */
    if (hdr->magic != TCP_PROTOCOL_MAGIC) {
        rdma_error_report("TCP: Invalid protocol magic: 0x%x", hdr->magic);
        return -1;
    }

    /* Allocate and receive payload */
    if (hdr->msg_len > TCP_MAX_PAYLOAD_LEN) {
        rdma_error_report("TCP: payload too large: %u", hdr->msg_len);
        return -1;
    }
    if (hdr->msg_len > 0) {
        /*
         * The peer chooses the length, so running out of memory here mustn't
         * abort the server, as g_malloc() would.  The payload is still in the
         * socket, so the stream can't go on without it: the caller closes the
         * connection, and the peer connects again.
         */
        *payload = malloc(hdr->msg_len);
        if (!*payload) {
            rdma_error_report("TCP: no memory for a %u-byte payload",
                              hdr->msg_len);
            return -1;
        }
        if (tcp_recv_rest(sockfd, *payload, hdr->msg_len, 0, running,
                          deadline_ms, "payload") < 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }

    return 0;
}
