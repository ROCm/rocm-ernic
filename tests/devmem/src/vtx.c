/*
 * vtx: independent devmem TCP TX sender for the verification pass.
 * Written from the uapi, not from devmem_tx.c or ncdevmem.
 *
 * The dmabuf (udmabuf over a memfd) is filled from getrandom(), so the
 * stream has no structure a wrong offset could reproduce. Every byte the
 * kernel accepts is also written, in order, to the "expected" file (-e),
 * read through the memfd mapping; the receiver hashes what arrives.
 *
 * vtx [-S bytes] [-R reps] [-T secs] [-e FILE] [-B] [-N] [-b sndbuf]
 *     [-t sndtimeo] [-C compl-wait-secs] [-O] IF DST PORT SPEC...
 * SPEC = K:OFF:LEN[:CHUNK[:GAP[:ADV]]], run in order, once per rep:
 *   K = d  devmem send (iov_base = dmabuf offset, SCM_DEVMEM_DMABUF,
 *          MSG_ZEROCOPY)
 *       c  ordinary copying send of the same bytes (from the mapping)
 *       z  MSG_ZEROCOPY of user memory (the mapping), no devmem cmsg
 *       p  sleep OFF milliseconds
 *   offset used in rep r = (OFF + r*ADV) mod (S - span + 1); -O: no modulo
 *   and no bounds check (for out-of-range iovecs).
 * Build, from the top of the repository (see tests/devmem/README.md):
 *   gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -O2 -Wall -Wextra -static -pthread -Isrc -o vtx \
 *       tests/devmem/src/vtx.c src/ernic_gnu.c
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/udmabuf.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "parse_int.h"

#define V_SCM_DEVMEM_DMABUF  79
#define V_SO_ZEROCOPY        60
#define V_MSG_ZEROCOPY       0x4000000
#define V_NETDEV_CMD_BIND_TX 15

static uint64_t S = 4 << 20, reps = 1;
static double secs;
static uint8_t *map;
static int expfd = -1, raw_off, sock, prewrite;
static uint32_t dmabuf_id;
static uint64_t n_send_calls, n_bytes, n_zc_sends, compl_cnt, compl_copied,
    compl_err;
static uint64_t bytes_by_kind[256];

static long dbg(const char *name, const char *wr)
{
    char path[128], b[64] = "";
    int fd;
    long v = -1;

    snprintf(path, sizeof(path), "/sys/kernel/debug/dma-api/%s", name);
    fd = open(path, wr ? O_WRONLY : O_RDONLY);
    if (fd < 0)
        return -1;
    if (wr) {
        if (write(fd, wr, strlen(wr)) < 0)
            v = -2;
    } else if (read(fd, b, sizeof(b) - 1) > 0) {
        int n;

        b[strcspn(b, "\n")] = 0;
        if (ernic_parse_int(b, &n))
            v = n;
    }
    close(fd);
    return v;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void die(const char *what)
{
    fprintf(stderr, "vtx: %s: %s (errno %d)\n", what, strerror(errno), errno);
    exit(2);
}

/* Decimal with an optional k, m or g, or hex with a 0x prefix. */
static uint64_t num(const char *s)
{
    uint64_t v;
    bool ok;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        ok = ernic_parse_hex_u64_span(s + 2, strlen(s + 2), &v);
    else
        ok = ernic_parse_size(s, &v);
    if (!ok) {
        errno = EINVAL;
        die(s);
    }
    return v;
}

static int num_int(const char *s)
{
    int v;

    if (!ernic_parse_int(s, &v)) {
        errno = EINVAL;
        die(s);
    }
    return v;
}

/*
 * Split "K:F0[:F1[:F2[:F3[:F4]]]]" into the kind K and up to five fields
 * of at most 31 characters; fields left out keep what @f held. Returns -1
 * if there is no kind or no first field.
 */
static int spec_split(const char *spec, char *k, char f[5][32])
{
    const char *p = spec;
    int n = 0;

    if (!p[0] || p[1] != ':')
        return -1;
    *k = p[0];
    p += 2;
    while (n < 5) {
        size_t len = strcspn(p, ":");

        if (!len || len > 31)
            break;
        memcpy(f[n], p, len);
        f[n][len] = 0;
        n++;
        p += len;
        if (*p != ':')
            break;
        p++;
    }
    return n ? 0 : -1;
}

static int nl_bind_tx(int ifindex, int dfd)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char a[256];
    } req;
    char rep[4096];
    struct nlattr *a;
    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC), one = 1, len,
        fam = -1;

    if (nl < 0)
        die("netlink socket");
    setsockopt(nl, SOL_NETLINK, NETLINK_EXT_ACK, &one, sizeof(one));
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_type = GENL_ID_CTRL;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.g.cmd = CTRL_CMD_GETFAMILY;
    req.g.version = 1;
    a = (struct nlattr *)req.a;
    a->nla_type = CTRL_ATTR_FAMILY_NAME;
    a->nla_len = NLA_HDRLEN + 7;
    memcpy((char *)a + NLA_HDRLEN, "netdev", 7);
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN) + NLA_ALIGN(a->nla_len);
    if (send(nl, &req, req.n.nlmsg_len, 0) < 0)
        die("nl send");
    len = recv(nl, rep, sizeof(rep), 0);
    if (len < 0)
        die("nl recv");
    {
        struct nlmsghdr *h = (struct nlmsghdr *)rep;
        int alen = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

        if (h->nlmsg_type == NLMSG_ERROR) {
            errno = EPROTO;
            die("getfamily");
        }
        for (a = (struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
             alen >= NLA_HDRLEN; alen -= NLA_ALIGN(a->nla_len),
            a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)))
            if (a->nla_type == CTRL_ATTR_FAMILY_ID)
                fam = *(uint16_t *)((char *)a + NLA_HDRLEN);
    }
    if (fam < 0) {
        errno = ENOENT;
        die("netdev genl family");
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_type = fam;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.g.cmd = V_NETDEV_CMD_BIND_TX;
    req.g.version = 1;
    a = (struct nlattr *)req.a;
    a->nla_type = 1; /* NETDEV_A_DMABUF_IFINDEX */
    a->nla_len = NLA_HDRLEN + 4;
    memcpy((char *)a + NLA_HDRLEN, &ifindex, 4);
    a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
    a->nla_type = 3; /* NETDEV_A_DMABUF_FD */
    a->nla_len = NLA_HDRLEN + 4;
    memcpy((char *)a + NLA_HDRLEN, &dfd, 4);
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN) + 2 * NLA_ALIGN(NLA_HDRLEN + 4);
    if (send(nl, &req, req.n.nlmsg_len, 0) < 0)
        die("nl send bind");
    for (;;) {
        struct nlmsghdr *h;

        len = recv(nl, rep, sizeof(rep), 0);
        if (len < 0)
            die("nl recv bind");
        for (h = (struct nlmsghdr *)rep; NLMSG_OK(h, (unsigned)len);
             h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);

                if (!e->error)
                    return nl; /* ack; keep the socket: it owns the binding */
                fprintf(stderr, "vtx: BIND_TX failed: %s (%d)",
                        strerror(-e->error), e->error);
                if (h->nlmsg_flags & NLM_F_ACK_TLVS) {
                    int off = NLMSG_HDRLEN + sizeof(*e) +
                              (e->msg.nlmsg_len - NLMSG_HDRLEN);
                    /* without NETLINK_CAP_ACK the request is echoed; the TLVs
                     * follow it */
                    for (a = (struct nlattr *)((char *)h + NLMSG_ALIGN(off));
                         (char *)a + NLA_HDRLEN <= (char *)h + h->nlmsg_len;
                         a = (struct nlattr *)((char *)a +
                                               NLA_ALIGN(a->nla_len))) {
                        if (a->nla_type == NLMSGERR_ATTR_MSG)
                            fprintf(stderr, " extack='%s'",
                                    (char *)a + NLA_HDRLEN);
                        if (a->nla_len < NLA_HDRLEN)
                            break;
                    }
                }
                fprintf(stderr, "\n");
                exit(3);
            }
            if (h->nlmsg_type == fam) {
                int alen = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

                for (a = (struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
                     alen >= NLA_HDRLEN; alen -= NLA_ALIGN(a->nla_len),
                    a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)))
                    if (a->nla_type == 4) /* NETDEV_A_DMABUF_ID */
                        memcpy(&dmabuf_id, (char *)a + NLA_HDRLEN, 4);
            }
        }
    }
}

static void reap(void)
{
    for (;;) {
        char ctl[256];
        struct msghdr m = {.msg_control = ctl, .msg_controllen = sizeof(ctl)};
        struct cmsghdr *c;

        if (recvmsg(sock, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
            return;
        for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            struct sock_extended_err *ee = (void *)CMSG_DATA(c);

            if (ee->ee_origin != SO_EE_ORIGIN_ZEROCOPY || ee->ee_errno) {
                compl_err++;
                fprintf(stderr, "vtx: errqueue origin=%u errno=%u\n",
                        ee->ee_origin, ee->ee_errno);
                continue;
            }
            compl_cnt += ee->ee_data - ee->ee_info + 1;
            if (ee->ee_code & SO_EE_CODE_ZEROCOPY_COPIED)
                compl_copied += ee->ee_data - ee->ee_info + 1;
        }
    }
}

/* returns 0, or -1 with errno when the kernel refused (after any partial
 * progress) */
static int do_send(int kind, uint64_t off, uint64_t len, uint64_t chunk,
                   uint64_t gap)
{
    struct iovec iov[1024];
    uint64_t cnt = (len + chunk - 1) / chunk, j, left = len;
    char ctl[CMSG_SPACE(sizeof(uint32_t))];
    struct msghdr m = {0};
    int flags = kind == 'c' ? 0 : V_MSG_ZEROCOPY;

    if (cnt > 1024) {
        errno = E2BIG;
        die("iovec count");
    }
    for (j = 0; j < cnt; j++) {
        uint64_t base = off + j * (chunk + gap),
                 l = left < chunk ? left : chunk;

        iov[j].iov_base = kind == 'd' ? (void *)base : (void *)(map + base);
        iov[j].iov_len = l;
        left -= l;
    }
    m.msg_iov = iov;
    m.msg_iovlen = cnt;
    if (kind == 'd') {
        struct cmsghdr *c;

        memset(ctl, 0, sizeof(ctl));
        m.msg_control = ctl;
        m.msg_controllen = sizeof(ctl);
        c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = V_SCM_DEVMEM_DMABUF;
        c->cmsg_len = CMSG_LEN(sizeof(uint32_t));
        memcpy(CMSG_DATA(c), &dmabuf_id, sizeof(uint32_t));
    }
    if (prewrite &&
        expfd >= 0) /* -K: expected file leads the socket, for kill tests */
        for (j = 0; j < cnt; j++) {
            uint8_t *src =
                kind == 'd' ? map + (uint64_t)iov[j].iov_base : iov[j].iov_base;

            if (write(expfd, src, iov[j].iov_len) != (ssize_t)iov[j].iov_len)
                die("prewrite expected");
        }
    while (m.msg_iovlen) {
        ssize_t n = sendmsg(sock, &m, flags);
        uint64_t done;

        n_send_calls++;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (flags)
            n_zc_sends++;
        n_bytes += n;
        bytes_by_kind[kind] += n;
        done = n;
        while (done) {
            uint64_t l = m.msg_iov->iov_len < done ? m.msg_iov->iov_len : done;
            uint8_t *src = kind == 'd' ? map + (uint64_t)m.msg_iov->iov_base
                                       : m.msg_iov->iov_base;

            if (expfd >= 0 && !prewrite &&
                (kind != 'd' || (uint64_t)m.msg_iov->iov_base + l <= S)) {
                uint64_t w = 0;

                while (w < l) {
                    ssize_t r = write(expfd, src + w, l - w);

                    if (r < 0)
                        die("write expected");
                    w += r;
                }
            }
            m.msg_iov->iov_base = (char *)m.msg_iov->iov_base + l;
            m.msg_iov->iov_len -= l;
            done -= l;
            if (!m.msg_iov->iov_len) {
                m.msg_iov++;
                m.msg_iovlen--;
            }
        }
        reap();
    }
    return 0;
}

int main(int argc, char **argv)
{
    int opt, bindtodev = 1, nodelay = 0, sndbuf = 0, sndtimeo = 0, cwait = 10,
             one = 1;
    const char *expfile = NULL;

    while ((opt = getopt(argc, argv, "S:R:T:e:BNb:t:C:OK")) != -1) {
        switch (opt) {
        case 'S':
            S = num(optarg);
            break;
        case 'R':
            reps = num(optarg);
            break;
        case 'T':
            secs = num_int(optarg);
            reps = UINT64_MAX;
            break;
        case 'e':
            expfile = optarg;
            break;
        case 'B':
            bindtodev = 0;
            break;
        case 'N':
            nodelay = 1;
            break;
        case 'b':
            sndbuf = num(optarg);
            break;
        case 't':
            sndtimeo = num_int(optarg);
            break;
        case 'C':
            cwait = num_int(optarg);
            break;
        case 'O':
            raw_off = 1;
            break;
        case 'K':
            prewrite = 1;
            break;
        default:
            return 2;
        }
    }
    if (argc - optind < 4) {
        fprintf(stderr, "usage: see source\n");
        return 2;
    }
    const char *ifname = argv[optind], *dst = argv[optind + 1];
    int port = num_int(argv[optind + 2]);
    int ifindex = if_nametoindex(ifname);

    if (!ifindex)
        die("if_nametoindex");
    if (expfile) {
        expfd = strcmp(expfile, "-")
                    ? open(expfile, O_WRONLY | O_CREAT | O_TRUNC, 0600)
                    : 1;
        if (expfd < 0)
            die("open expected");
    }

    int mfd = ernic_gnu_memfd_create("vtx");

    if (mfd < 0)
        die("memfd_create");
    if (ftruncate(mfd, S))
        die("ftruncate");
    if (ernic_gnu_seal_size(mfd))
        die("seal");
    map = mmap(NULL, S, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED)
        die("mmap");
    for (uint64_t x = 0; x < S;) {
        ssize_t r =
            getrandom(map + x, S - x > (1 << 20) ? (1 << 20) : S - x, 0);

        if (r < 0)
            die("getrandom");
        x += r;
    }
    int ufd = open("/dev/udmabuf", O_RDWR);

    if (ufd < 0)
        die("open /dev/udmabuf");
    struct udmabuf_create uc = {
        .memfd = mfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = S};
    int dfd = ioctl(ufd, UDMABUF_CREATE, &uc);

    if (dfd < 0)
        die("UDMABUF_CREATE");
    /* DMA-API debug prints only while num_errors > 0: silence the known
     * sg-segment report of the bind, count it, and re-arm for the rest */
    dbg("num_errors", "0");
    long ec0 = dbg("error_count", NULL);
    int nl = nl_bind_tx(ifindex, dfd);
    long ec1 = dbg("error_count", NULL);

    dbg("num_errors", "100");
    fprintf(stderr, "vtx: DMA-API error_count during bind: %ld (before %ld)\n",
            ec1 - ec0, ec0);

    fprintf(stderr,
            "vtx: bound dmabuf fd %d (%" PRIu64
            " bytes) for TX on %s ifindex %d: binding id %u (nl fd %d)\n",
            dfd, S, ifname, ifindex, dmabuf_id, nl);

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};

    if (inet_pton(AF_INET, dst, &sa.sin_addr) != 1) {
        errno = EINVAL;
        die("address");
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        die("socket");
    if (bindtodev && setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                                strlen(ifname) + 1))
        die("SO_BINDTODEVICE");
    if (setsockopt(sock, SOL_SOCKET, V_SO_ZEROCOPY, &one, sizeof(one)))
        die("SO_ZEROCOPY");
    if (nodelay)
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (sndbuf)
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (sndtimeo) {
        struct timeval tv = {.tv_sec = sndtimeo};

        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)))
        die("connect");
    {
        struct sockaddr_in me;
        socklen_t ml = sizeof(me);

        getsockname(sock, (struct sockaddr *)&me, &ml);
        fprintf(stderr, "vtx: connected, local port %d\n", ntohs(me.sin_port));
    }

    double t0 = now();
    int failed = 0;

    for (uint64_t r = 0; r < reps && !failed; r++) {
        if (secs && now() - t0 >= secs)
            break;
        for (int i = optind + 3; i < argc && !failed; i++) {
            char k;
            char f[5][32] = {"0", "0", "0", "0", "0"};
            uint64_t off, len, chunk, gap, adv;

            if (spec_split(argv[i], &k, f)) {
                errno = EINVAL;
                die(argv[i]);
            }
            off = num(f[0]);
            len = num(f[1]);
            chunk = num(f[2]);
            gap = num(f[3]);
            adv = num(f[4]);
            if (k == 'p') {
                usleep(off * 1000);
                continue;
            }
            if (!chunk)
                chunk = len;
            uint64_t cnt = (len + chunk - 1) / chunk;
            uint64_t span =
                (cnt - 1) * (chunk + gap) + (len - (cnt - 1) * chunk);

            if (!raw_off) {
                if (span > S) {
                    errno = ERANGE;
                    die("span > dmabuf");
                }
                off = (off + r * adv) % (S - span + 1);
            }
            if (do_send(k, off, len, chunk, gap)) {
                int e = errno, soerr = 0;
                socklen_t sl = sizeof(soerr);

                getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                fprintf(stderr,
                        "vtx: SEND FAILED rep %" PRIu64 " spec %s off %" PRIu64
                        ": %s (errno %d) SO_ERROR=%d\n",
                        r, argv[i], off, strerror(e), e, soerr);
                failed = 1;
            }
        }
    }
    double t1 = now();

    shutdown(sock, SHUT_WR);
    while (compl_cnt < n_zc_sends && now() - t1 < cwait) {
        struct pollfd p = {.fd = sock, .events = 0};

        poll(&p, 1, 100);
        reap();
    }
    reap();
    fprintf(stderr,
            "vtx: RESULT %s sendmsg_calls=%" PRIu64 " bytes=%" PRIu64
            " (devmem=%" PRIu64 " copy=%" PRIu64 " userzc=%" PRIu64
            ") zc_sends=%" PRIu64 " completions=%" PRIu64 " copied=%" PRIu64
            " errq_errors=%" PRIu64 " secs=%.2f MiB/s=%.1f\n",
            failed ? "SEND-ERROR" : "sent-all", n_send_calls, n_bytes,
            bytes_by_kind['d'], bytes_by_kind['c'], bytes_by_kind['z'],
            n_zc_sends, compl_cnt, compl_copied, compl_err, t1 - t0,
            n_bytes / (t1 - t0 + 1e-9) / 1048576.0);
    close(sock);
    close(nl);
    return failed;
}
