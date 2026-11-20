/*
 * devmem_tx_gpu: devmem_tx.c with the dma-buf optionally taken from a GPU.
 *
 * As devmem_tx, plus, on the sender: -G NODE allocates the buffer as an
 * amdgpu GEM object on the DRM node NODE (e.g. /dev/dri/renderD128) in
 * VRAM, exports it as a dma-buf and binds that instead of a udmabuf; -J
 * with it asks for the GTT domain (system RAM) instead of VRAM.
 *
 * devmem_tx: devmem TCP TX exerciser with a self-verifying receiver.
 *
 * Sender: creates a udmabuf filled with a position-dependent pattern, binds
 * it for TX on a netdev with a raw generic-netlink NETDEV_CMD_BIND_TX, and
 * sends from arbitrary dmabuf offsets / lengths / iovec counts with
 * MSG_ZEROCOPY + SCM_DEVMEM_DMABUF. It reaps every zerocopy completion from
 * the error queue and reports them (ranges, ee_code, ee_errno).
 *
 * Receiver (-r): an ordinary TCP sink that recomputes the expected byte
 * stream from the same plan options and compares every byte.
 *
 * Options (both sides): -p PORT, and the plan: -S dmabuf bytes, -P seed,
 * -o first offset, -l bytes per sendmsg, -z iovec length (default: one
 * iovec), -g gap between iovecs, -a offset advance per send, -n sends
 * (receiver: 0 = verify until EOF). Numbers are decimal and take k/m/g
 * suffixes. Sender: -f IF, -s ADDR (IPv4 or IPv6), -c SRC, -T secs (send for
 * secs instead of -n), -w N (most sends awaiting completion; 0 = one at a
 * time), -B (no SO_BINDTODEVICE), -m devmem|zc|copy (controls with the same
 * framing), -t SO_SNDTIMEO secs, -N TCP_NODELAY, -b SO_SNDBUF, -u usecs
 * between sends, -C secs to wait for the last completions, -R secs to
 * retry a send refused because the route/device is momentarily gone,
 * -X close-sock|unbind|free-dmabuf|exit (teardown with sends in flight),
 * -D (bracket the bind for DMA-API debugfs accounting), -F FILE (dump the
 * dmabuf's PFNs), -v. Receiver: -r, -s ADDR to listen on, -W idle secs.
 *
 * The plan (-S -P -o -l -z -g -a -n) must be given identically to both
 * sides. Send i (0-based) starts at dmabuf offset
 *     start(i) = (o + i * a) mod (S - span + 1)
 * where span = iovcnt * (z + g) - g is the dmabuf range one send covers;
 * iovec j of it is [start + j * (z + g), +min(z, bytes left)).
 *
 * Build, from the top of the repository (see tests/devmem/README.md),
 * with v7.4 uapi headers for <drm/amdgpu_drm.h>:
 *   gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -O2 -Wall -Wextra -static -pthread -isystem UAPI -Isrc \
 *       -o devmem_tx_gpu tests/devmem/src/devmem_tx_gpu.c src/ernic_gnu.c
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
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "parse_int.h"

#ifndef SO_DEVMEM_DMABUF
#define SO_DEVMEM_DMABUF 79
#endif
#ifndef SCM_DEVMEM_DMABUF
#define SCM_DEVMEM_DMABUF SO_DEVMEM_DMABUF
#endif
#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif
#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif
#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY 5
#endif
#ifndef SO_EE_CODE_ZEROCOPY_COPIED
#define SO_EE_CODE_ZEROCOPY_COPIED 1
#endif
#ifndef NETLINK_EXT_ACK
#define NETLINK_EXT_ACK 11
#endif
/* include/uapi/linux/netdev.h (v7.2.15 and v7.4-rc3 agree) */
#define DT_NETDEV_CMD_BIND_TX      15
#define DT_NETDEV_A_DMABUF_IFINDEX 1
#define DT_NETDEV_A_DMABUF_FD      3
#define DT_NETDEV_A_DMABUF_ID      4

#define DMA_API_DIR "/sys/kernel/debug/dma-api/"

enum mode { MODE_DEVMEM, MODE_ZC, MODE_COPY };
enum xact { X_NONE, X_CLOSE_SOCK, X_UNBIND, X_FREE_DMABUF, X_EXIT };

static struct {
    int receiver;
    const char *ifname;
    const char *addr;
    const char *src;
    int port;
    uint64_t size;   /* -S */
    uint64_t seed;   /* -P */
    uint64_t off;    /* -o */
    uint64_t len;    /* -l */
    uint64_t chunk;  /* -z */
    uint64_t gap;    /* -g */
    uint64_t adv;    /* -a */
    uint64_t count;  /* -n */
    double secs;     /* -T */
    unsigned window; /* -w */
    int no_bindtodevice;
    enum mode mode;
    int sndtimeo; /* -t */
    int nodelay;
    enum xact xact;
    int dma_debug;  /* -D */
    int compl_wait; /* -C */
    int idle;       /* -W receiver idle timeout */
    int sndbuf;     /* -b */
    int pace_us;    /* -u */
    const char
        *gpu_node; /* -G: amdgpu render node, VRAM dma-buf instead of udmabuf */
    int gpu_gtt;   /* -g is taken; -J = ask for GTT instead of VRAM */
    const char *pfn_file; /* -F */
    int retry_secs;       /* -R */
    int verbose;
} o = {
    .port = 5400,
    .size = 16u << 20,
    .seed = 1,
    .len = 65536,
    .count = 1,
    .window = 64,
    .compl_wait = 30,
    .idle = 20,
};

/* derived plan */
static uint64_t p_iovcnt, p_step, p_span, p_wrap;

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void die(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static inline uint8_t pat(uint64_t x, uint64_t seed)
{
    uint64_t w = (x >> 3) + seed * UINT64_C(0x100000001b3);

    w ^= w >> 30;
    w *= UINT64_C(0xbf58476d1ce4e5b9);
    w ^= w >> 27;
    w *= UINT64_C(0x94d049bb133111eb);
    w ^= w >> 31;
    return (uint8_t)(w >> ((x & 7) * 8));
}

static void fill(uint8_t *p, uint64_t size, uint64_t seed)
{
    for (uint64_t x = 0; x < size; x++)
        p[x] = pat(x, seed);
}

static void plan_init(void)
{
    if (!o.len)
        die("-l must be > 0");
    if (!o.chunk)
        o.chunk = o.len;
    p_iovcnt = (o.len + o.chunk - 1) / o.chunk;
    if (p_iovcnt > 1024)
        die("%" PRIu64 " iovecs per send > UIO_MAXIOV", p_iovcnt);
    p_step = o.chunk + o.gap;
    p_span = (p_iovcnt - 1) * p_step + (o.len - (p_iovcnt - 1) * o.chunk);
    if (p_span > o.size)
        die("one send spans %" PRIu64 " bytes > dmabuf %" PRIu64, p_span,
            o.size);
    p_wrap = o.size - p_span + 1;
}

static uint64_t plan_start(uint64_t i)
{
    return (o.off + i * o.adv) % p_wrap;
}

/* iovec j of a send starting at 'start': base and length */
static void plan_iov(uint64_t start, uint64_t j, uint64_t *base, uint64_t *len)
{
    uint64_t done = j * o.chunk;

    *base = start + j * p_step;
    *len = o.len - done < o.chunk ? o.len - done : o.chunk;
}

static void parse_addr(const char *s, int port, struct sockaddr_in6 *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(port);
    if (!s) {
        sa->sin6_addr = in6addr_any;
        return;
    }
    if (inet_pton(AF_INET6, s, &sa->sin6_addr) == 1)
        return;
    struct in_addr v4;

    if (inet_pton(AF_INET, s, &v4) != 1)
        die("bad address %s", s);
    memset(&sa->sin6_addr, 0, 10);
    sa->sin6_addr.s6_addr[10] = 0xff;
    sa->sin6_addr.s6_addr[11] = 0xff;
    memcpy(&sa->sin6_addr.s6_addr[12], &v4, 4);
}

/* ---------------------------------------------------------------- receiver */

static int run_receiver(void)
{
    struct sockaddr_in6 sa;
    int one = 1, ls, fd;
    uint8_t *image, *buf;
    const size_t bufsz = 1 << 20;

    image = malloc(o.size);
    buf = malloc(bufsz);
    if (!image || !buf)
        die("malloc");
    fill(image, o.size, o.seed);

    ls = socket(AF_INET6, SOCK_STREAM, 0);
    if (ls < 0)
        die("socket: %m");
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    parse_addr(o.addr, o.port, &sa);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)))
        die("bind: %m");
    if (listen(ls, 1))
        die("listen: %m");
    say("RX listening port=%d size=%" PRIu64 " seed=%" PRIu64 " o=%" PRIu64
        " l=%" PRIu64 " z=%" PRIu64 " g=%" PRIu64 " a=%" PRIu64 " n=%" PRIu64
        " iovcnt=%" PRIu64 "\n",
        o.port, o.size, o.seed, o.off, o.len, o.chunk, o.gap, o.adv, o.count,
        p_iovcnt);
    fd = accept(ls, NULL, NULL);
    if (fd < 0)
        die("accept: %m");
    close(ls);
    if (o.idle) {
        struct timeval tv = {.tv_sec = o.idle};

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    uint64_t total = 0, mism = 0, mism_zero = 0, first_mism = UINT64_MAX;
    uint64_t i = 0, j = 0,
             in_iov = 0; /* send index, iovec index, bytes done in iovec */
    uint64_t start = plan_start(0), extra = 0;
    const char *end = "eof";
    double t0 = now(), tlast = t0;

    for (;;) {
        ssize_t n = recv(fd, buf, bufsz, 0);

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            end = errno == EAGAIN ? "idle-timeout" : strerror(errno);
            break;
        }
        size_t pos = 0;

        while (pos < (size_t)n) {
            uint64_t base, len, take;

            if (o.count && i >= o.count) {
                extra += n - pos;
                break;
            }
            plan_iov(start, j, &base, &len);
            take = len - in_iov;
            if (take > n - pos)
                take = n - pos;
            if (memcmp(buf + pos, image + base + in_iov, take)) {
                for (uint64_t k = 0; k < take; k++) {
                    uint8_t got = buf[pos + k], exp = image[base + in_iov + k];

                    if (got == exp)
                        continue;
                    if (first_mism == UINT64_MAX) {
                        first_mism = total + pos + k;
                        say("MISMATCH first at stream=%" PRIu64 " send=%" PRIu64
                            " iov=%" PRIu64 " dmabuf_off=%" PRIu64
                            " exp=%02x got=%02x\n",
                            first_mism, i, j, base + in_iov + k, exp, got);
                    }
                    mism++;
                    if (!got)
                        mism_zero++;
                }
            }
            pos += take;
            in_iov += take;
            if (in_iov == len) {
                in_iov = 0;
                if (++j == p_iovcnt) {
                    j = 0;
                    start = plan_start(++i);
                }
            }
        }
        total += n;
        if (o.verbose && now() - tlast >= 10) {
            tlast = now();
            say("RX progress t=%.0f bytes=%" PRIu64 " mismatches=%" PRIu64 "\n",
                tlast - t0, total, mism);
        }
    }
    double dt = now() - t0;
    uint64_t want = o.count * o.len;
    int complete =
        o.count ? (total == want && !extra) : (j == 0 && in_iov == 0);
    int ok = !mism && complete;

    say("RESULT rx %s bytes=%" PRIu64 " expected=%" PRIu64
        " whole_sends=%" PRIu64 " partial_send=%s extra=%" PRIu64
        " mismatches=%" PRIu64 " mismatch_zero_bytes=%" PRIu64
        " end=%s secs=%.2f MiB/s=%.1f\n",
        ok ? "PASS" : "FAIL", total, want, i, (j || in_iov) ? "yes" : "no",
        extra, mism, mism_zero, end, dt, dt > 0 ? total / dt / 1048576.0 : 0.0);
    close(fd);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ netlink */

static int nl_fd = -1;

static int nl_txrx(void *req, size_t len, char *reply, size_t rlen)
{
    ssize_t n;

    if (send(nl_fd, req, len, 0) != (ssize_t)len)
        die("netlink send: %m");
    n = recv(nl_fd, reply, rlen, 0);
    if (n < 0)
        die("netlink recv: %m");
    return n;
}

static struct nlattr *nla_find(struct nlattr *a, int alen, int type)
{
    while (alen >= (int)sizeof(*a) && a->nla_len >= sizeof(*a) &&
           a->nla_len <= alen) {
        if ((a->nla_type & NLA_TYPE_MASK) == type)
            return a;
        alen -= NLA_ALIGN(a->nla_len);
        a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
    }
    return NULL;
}

/* returns 0 or -errno; prints the extack message */
static int nl_check_error(struct nlmsghdr *h, const char *what)
{
    if (h->nlmsg_type != NLMSG_ERROR)
        return 0;
    struct nlmsgerr *e = NLMSG_DATA(h);

    if (!e->error)
        return 0;
    const char *msg = "";

    if (h->nlmsg_flags & NLM_F_ACK_TLVS) {
        /* NLM_F_CAPPED: only the header of the request is echoed */
        size_t skip = sizeof(*e);

        if (!(h->nlmsg_flags & NLM_F_CAPPED))
            skip += e->msg.nlmsg_len - sizeof(struct nlmsghdr);
        struct nlattr *a = (struct nlattr *)((char *)e + NLMSG_ALIGN(skip));
        int alen = (char *)h + h->nlmsg_len - (char *)a;
        struct nlattr *m = nla_find(a, alen, NLMSGERR_ATTR_MSG);

        if (m)
            msg = (char *)(m + 1);
    }
    say("%s: netlink error %d (%s) extack='%s'\n", what, e->error,
        strerror(-e->error), msg);
    return e->error;
}

static int nl_bind_tx(int ifindex, int dmabuf_fd, uint32_t *id)
{
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    char reply[8192];
    int one = 1, n, family = -1;
    struct {
        struct nlmsghdr h;
        struct genlmsghdr g;
        char attrs[64];
    } req;
    struct nlattr *a;

    nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (nl_fd < 0)
        die("netlink socket: %m");
    setsockopt(nl_fd, SOL_NETLINK, NETLINK_EXT_ACK, &one, sizeof(one));
    if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)))
        die("netlink bind: %m");

    memset(&req, 0, sizeof(req));
    req.h.nlmsg_type = GENL_ID_CTRL;
    req.h.nlmsg_flags = NLM_F_REQUEST;
    req.h.nlmsg_seq = 1;
    req.g.cmd = CTRL_CMD_GETFAMILY;
    req.g.version = 1;
    a = (struct nlattr *)req.attrs;
    a->nla_type = CTRL_ATTR_FAMILY_NAME;
    a->nla_len = NLA_HDRLEN + sizeof("netdev");
    memcpy(a + 1, "netdev", sizeof("netdev"));
    req.h.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_ALIGN(a->nla_len));
    n = nl_txrx(&req, req.h.nlmsg_len, reply, sizeof(reply));
    struct nlmsghdr *h = (struct nlmsghdr *)reply;

    if (n < (int)sizeof(*h) || nl_check_error(h, "getfamily"))
        die("cannot resolve the netdev genetlink family");
    a = nla_find((struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN),
                 h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN), CTRL_ATTR_FAMILY_ID);
    if (!a)
        die("no family id");
    family = *(uint16_t *)(a + 1);

    memset(&req, 0, sizeof(req));
    req.h.nlmsg_type = family;
    req.h.nlmsg_flags = NLM_F_REQUEST;
    req.h.nlmsg_seq = 2;
    req.g.cmd = DT_NETDEV_CMD_BIND_TX;
    req.g.version = 1;
    a = (struct nlattr *)req.attrs;
    a->nla_type = DT_NETDEV_A_DMABUF_IFINDEX;
    a->nla_len = NLA_HDRLEN + 4;
    memcpy(a + 1, &ifindex, 4);
    a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
    a->nla_type = DT_NETDEV_A_DMABUF_FD;
    a->nla_len = NLA_HDRLEN + 4;
    memcpy(a + 1, &dmabuf_fd, 4);
    req.h.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + 2 * NLA_ALIGN(NLA_HDRLEN + 4));
    n = nl_txrx(&req, req.h.nlmsg_len, reply, sizeof(reply));
    h = (struct nlmsghdr *)reply;
    if (n < (int)sizeof(*h))
        die("short netlink reply");
    n = nl_check_error(h, "bind-tx");
    if (n)
        return n;
    a = nla_find((struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN),
                 h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN),
                 DT_NETDEV_A_DMABUF_ID);
    if (!a)
        die("bind-tx reply has no id");
    *id = *(uint32_t *)(a + 1);
    return 0;
}

/* ------------------------------------------------------------- dma-api debug
 */

static long dbg_read(const char *name)
{
    char path[128], b[32];
    int fd, n;

    snprintf(path, sizeof(path), DMA_API_DIR "%s", name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    b[n] = 0;
    b[strcspn(b, "\n")] = 0;
    if (!ernic_parse_int(b, &n))
        return -1;
    return n;
}

static void dbg_write(const char *name, long v)
{
    char path[128], b[32];
    int fd;

    snprintf(path, sizeof(path), DMA_API_DIR "%s", name);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    snprintf(b, sizeof(b), "%ld\n", v);
    if (write(fd, b, strlen(b)) < 0)
        perror(path);
    close(fd);
}

/* PFNs backing the dmabuf, one hex number per line (needs CAP_SYS_ADMIN).
 * With no IOMMU the binding's DMA addresses are these PFNs << PAGE_SHIFT. */
static void dump_pfns(const uint8_t *map, uint64_t size, const char *file)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    FILE *f = fopen(file, "w");
    long ps = getpagesize();

    if (fd < 0 || !f)
        die("pagemap/%s: %m", file);
    for (uint64_t x = 0; x < size; x += ps) {
        uint64_t e;

        if (pread(fd, &e, 8, ((uintptr_t)map + x) / ps * 8) != 8)
            die("pagemap read: %m");
        if (!(e >> 63))
            die("page %" PRIu64 " not present", x / ps);
        fprintf(f, "%" PRIx64 "\n", e & ((UINT64_C(1) << 55) - 1));
    }
    fclose(f);
    close(fd);
}

/* ------------------------------------------------------------------- sender */

static uint64_t c_next; /* next sendmsg index we expect a completion for */
static uint64_t c_notif, c_copied, c_err, c_gap;

static int reap(int fd)
{
    int got = 0;

    for (;;) {
        char ctrl[256];
        struct msghdr msg = {.msg_control = ctrl,
                             .msg_controllen = sizeof(ctrl)};
        ssize_t r = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);

        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return got;
            say("errqueue recvmsg: %s\n", strerror(errno));
            return got;
        }
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
             cm = CMSG_NXTHDR(&msg, cm)) {
            struct sock_extended_err *ee = (void *)CMSG_DATA(cm);

            if (!((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) ||
                  (cm->cmsg_level == SOL_IPV6 &&
                   cm->cmsg_type == IPV6_RECVERR)))
                continue;
            if (ee->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
                say("errqueue: origin %u errno %u (not zerocopy)\n",
                    ee->ee_origin, ee->ee_errno);
                c_err++;
                continue;
            }
            uint32_t lo = ee->ee_info, hi = ee->ee_data;

            c_notif++;
            if (ee->ee_errno)
                c_err++;
            if (ee->ee_code & SO_EE_CODE_ZEROCOPY_COPIED)
                c_copied += hi - lo + 1;
            if (lo != (uint32_t)c_next)
                c_gap++;
            if (o.verbose > 1)
                say("completion [%u,%u] code=%u errno=%u\n", lo, hi,
                    ee->ee_code, ee->ee_errno);
            c_next += (uint32_t)(hi - (uint32_t)c_next) + 1;
            got++;
        }
    }
}

static void wait_err(int fd, int ms)
{
    struct pollfd p = {.fd = fd, .events = 0};

    poll(&p, 1, ms);
}


/* ---- amdgpu dma-buf source (-G) -------------------------------------- *
 * Allocate a buffer object on the emulated GPU and export it as a dma-buf,
 * so that the devmem TX binding's exporter is amdgpu instead of udmabuf.
 * Raw ioctls, so the program stays static and needs no libdrm.
 */
static int gpu_alloc_dmabuf(const char *node, uint64_t size, int want_gtt,
                            uint8_t **map_out, int *drm_fd_out,
                            uint32_t *handle_out)
{
    union drm_amdgpu_gem_create c = {0};
    union drm_amdgpu_gem_mmap m = {0};
    struct drm_prime_handle p = {0};
    uint8_t *map;
    int fd;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        die("%s: %m", node);
    c.in.bo_size = size;
    c.in.alignment = 4096;
    c.in.domains = want_gtt ? AMDGPU_GEM_DOMAIN_GTT : AMDGPU_GEM_DOMAIN_VRAM;
    c.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &c))
        die("AMDGPU_GEM_CREATE(%s, %" PRIu64 " bytes): %m",
            want_gtt ? "GTT" : "VRAM", size);
    say("gpu: created BO handle=%u domain=%s size=%" PRIu64 "\n", c.out.handle,
        want_gtt ? "GTT" : "VRAM", size);

    m.in.handle = c.out.handle;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &m))
        die("AMDGPU_GEM_MMAP: %m");
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               m.out.addr_ptr);
    if (map == MAP_FAILED)
        die("mmap BO: %m");

    p.handle = c.out.handle;
    p.flags = O_CLOEXEC | O_RDWR;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p))
        die("PRIME_HANDLE_TO_FD: %m");
    say("gpu: exported dma-buf fd=%d from %s\n", p.fd, node);

    *map_out = map;
    *drm_fd_out = fd;
    *handle_out = c.out.handle;
    return p.fd;
}

static int run_sender(void)
{
    struct sockaddr_in6 sa;
    int one = 1, sock, devfd = -1, memfd = -1, dmabuf = -1, ifindex = 0;
    uint8_t *map = NULL, *image = NULL;
    uint32_t id = 0;
    int rc = 0;

    if (!o.addr)
        die("-s ADDR required");
    if (o.mode == MODE_DEVMEM) {
        if (!o.ifname)
            die("-f IF required");
        ifindex = if_nametoindex(o.ifname);
        if (!ifindex)
            die("no such interface %s", o.ifname);
        if (o.size % getpagesize())
            die("-S must be a page multiple");
        if (o.gpu_node) {
            uint32_t h = 0;

            dmabuf = gpu_alloc_dmabuf(o.gpu_node, o.size, o.gpu_gtt, &map,
                                      &devfd, &h);
            fill(map, o.size, o.seed);
            say("gpu: filled %" PRIu64 " bytes through the CPU mapping\n",
                o.size);
            if (o.pfn_file)
                dump_pfns(map, o.size, o.pfn_file);
            goto have_dmabuf;
        }
        devfd = open("/dev/udmabuf", O_RDWR);
        if (devfd < 0)
            die("/dev/udmabuf: %m");
        memfd = ernic_gnu_memfd_create("devmem_tx");
        if (memfd < 0)
            die("memfd_create: %m");
        if (ftruncate(memfd, o.size))
            die("ftruncate: %m");
        if (ernic_gnu_seal_size(memfd) < 0)
            die("F_ADD_SEALS: %m");
        map = mmap(NULL, o.size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (map == MAP_FAILED)
            die("mmap memfd: %m");
        fill(map, o.size, o.seed);
        if (o.pfn_file)
            dump_pfns(map, o.size, o.pfn_file);
        struct udmabuf_create c = {.memfd = memfd, .offset = 0, .size = o.size};

        dmabuf = ioctl(devfd, UDMABUF_CREATE, &c);
        if (dmabuf < 0)
            die("UDMABUF_CREATE: %m");
    have_dmabuf:;
        long e0 = -1, shown = -1;

        if (o.dma_debug) {
            /* udmabuf maps segments above the 64K the device declares; keep
             * those known reports out of dmesg so that any other DMA-API
             * report is printed (num_errors > 0 afterwards). */
            e0 = dbg_read("error_count");
            shown = dbg_read("num_errors");
            dbg_write("num_errors", 0);
        }
        rc = nl_bind_tx(ifindex, dmabuf, &id);
        if (o.dma_debug) {
            long e1 = dbg_read("error_count");

            dbg_write("num_errors", shown > 0 ? shown : 100);
            say("DMA-API error_count during bind: %ld (before %ld after %ld)\n",
                e1 - e0, e0, e1);
        }
        if (rc) {
            say("RESULT tx BIND-FAILED errno=%d (%s)\n", -rc, strerror(-rc));
            return 3;
        }
        say("bind-tx ok: ifindex=%d dmabuf_id=%u size=%" PRIu64 "\n", ifindex,
            id, o.size);
    } else {
        image = malloc(o.size);
        if (!image)
            die("malloc");
        fill(image, o.size, o.seed);
    }

    sock = socket(AF_INET6, SOCK_STREAM, 0);
    if (sock < 0)
        die("socket: %m");
    if (o.ifname && !o.no_bindtodevice &&
        setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, o.ifname,
                   strlen(o.ifname) + 1))
        die("SO_BINDTODEVICE: %m");
    if (o.src) {
        parse_addr(o.src, 0, &sa);
        if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)))
            die("bind %s: %m", o.src);
    }
    if (o.mode != MODE_COPY &&
        setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)))
        die("SO_ZEROCOPY: %m");
    if (o.nodelay)
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (o.sndbuf &&
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &o.sndbuf, sizeof(o.sndbuf)))
        die("SO_SNDBUF: %m");
    if (o.sndtimeo) {
        struct timeval tv = {.tv_sec = o.sndtimeo};

        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    parse_addr(o.addr, o.port, &sa);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa))) {
        say("RESULT tx CONNECT-FAILED errno=%d (%s)\n", errno, strerror(errno));
        return 4;
    }
    say("connected to %s port %d mode=%s bindtodevice=%s\n", o.addr, o.port,
        o.mode == MODE_DEVMEM ? "devmem"
        : o.mode == MODE_ZC   ? "zc"
                              : "copy",
        (o.ifname && !o.no_bindtodevice) ? o.ifname : "no");

    struct iovec *iov = calloc(p_iovcnt, sizeof(*iov));
    char ctrl[CMSG_SPACE(sizeof(uint32_t))];
    uint64_t sent = 0, bytes = 0, short_sends = 0;
    int send_errno = 0;
    double t0 = now(), tlast = t0;
    int flags = o.mode == MODE_COPY ? 0 : MSG_ZEROCOPY;
    int zc = o.mode != MODE_COPY;

    while ((o.count && sent < o.count) || (!o.count && now() - t0 < o.secs)) {
        uint64_t start = plan_start(sent);
        struct msghdr msg = {.msg_iov = iov, .msg_iovlen = p_iovcnt};

        for (uint64_t j = 0; j < p_iovcnt; j++) {
            uint64_t base, len;

            plan_iov(start, j, &base, &len);
            /* devmem: iov_base is an offset into the dmabuf */
            iov[j].iov_base =
                o.mode == MODE_DEVMEM ? (void *)base : image + base;
            iov[j].iov_len = len;
        }
        if (o.mode == MODE_DEVMEM) {
            struct cmsghdr *cm;

            memset(ctrl, 0, sizeof(ctrl));
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);
            cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_DEVMEM_DMABUF;
            cm->cmsg_len = CMSG_LEN(sizeof(uint32_t));
            memcpy(CMSG_DATA(cm), &id, sizeof(id));
        }
        ssize_t r = sendmsg(sock, &msg, flags);

        /* -R: a send refused before anything was queued because the route
         * or the device is momentarily gone (link down, reconfiguration)
         * is retried, so that the stream can be verified across the event. */
        if (r < 0 && o.retry_secs &&
            (errno == EHOSTUNREACH || errno == ENETUNREACH || errno == ENODEV ||
             errno == ENETDOWN || (errno == EINVAL && sent))) {
            /* EINVAL after earlier sends worked: the socket's route
             * capabilities were zeroed by a failed route lookup, so the
             * kernel refuses MSG_ZEROCOPY with a dmabuf until TCP has
             * routed a packet again. */
            static double tfail;
            static uint64_t retries;

            static int last_errno;

            if (!tfail || sent != retries || errno != last_errno) {
                say("sendmsg #%" PRIu64
                    " errno=%d (%s) at %.2fs: retrying for up to %ds\n",
                    sent, errno, strerror(errno), now() - t0, o.retry_secs);
                if (!tfail || sent != retries)
                    tfail = now();
                retries = sent;
                last_errno = errno;
            }
            if (now() - tfail < o.retry_secs) {
                usleep(50000);
                continue;
            }
            errno = EHOSTUNREACH;
        }
        if (r < 0) {
            send_errno = errno;
            say("sendmsg #%" PRIu64 " failed: errno=%d (%s) after %.2fs\n",
                sent, errno, strerror(errno), now() - t0);
            break;
        }
        bytes += r;
        if ((uint64_t)r != o.len) {
            short_sends++;
            say("sendmsg #%" PRIu64 " short: %zd of %" PRIu64 " after %.2fs\n",
                sent, r, o.len, now() - t0);
            sent++;
            send_errno = EAGAIN;
            break;
        }
        sent++;
        if (zc) {
            reap(sock);
            double tw = now();

            while (sent - c_next > o.window) {
                wait_err(sock, 100);
                reap(sock);
                if (now() - tw > (o.sndtimeo ? o.sndtimeo : 120)) {
                    say("no completion for %.0fs: sent=%" PRIu64
                        " completed=%" PRIu64 "\n",
                        now() - tw, sent, c_next);
                    send_errno = ETIMEDOUT;
                    goto out;
                }
            }
        }
        if (o.pace_us)
            usleep(o.pace_us);
        if (o.verbose && now() - tlast >= 10) {
            tlast = now();
            say("TX progress t=%.0f sends=%" PRIu64 " bytes=%" PRIu64
                " completed=%" PRIu64 " copied=%" PRIu64 "\n",
                tlast - t0, sent, bytes, c_next, c_copied);
        }
    }
out:;
    double tsend = now() - t0;

    switch (o.xact) {
    case X_EXIT:
        say("X exit: _exit with sent=%" PRIu64 " completed=%" PRIu64
            " in flight\n",
            sent, c_next);
        _exit(0);
    case X_CLOSE_SOCK:
        reap(sock);
        say("X close-sock: closing TCP socket with sent=%" PRIu64
            " completed=%" PRIu64 "\n",
            sent, c_next);
        close(sock);
        sleep(3);
        say("X close-sock: closing netlink socket (unbind)\n");
        close(nl_fd);
        say("RESULT tx X-DONE sends=%" PRIu64 " bytes=%" PRIu64 "\n", sent,
            bytes);
        return 0;
    case X_UNBIND: {
        reap(sock);
        say("X unbind: closing netlink socket with sent=%" PRIu64
            " completed=%" PRIu64 "\n",
            sent, c_next);
        close(nl_fd);
        nl_fd = -1;
        /* a further send with the stale id must fail */
        struct msghdr msg = {.msg_iov = iov,
                             .msg_iovlen = 1,
                             .msg_control = ctrl,
                             .msg_controllen = sizeof(ctrl)};
        ssize_t r = sendmsg(sock, &msg, flags);

        say("X unbind: sendmsg with stale id -> %zd errno=%d (%s)\n", r,
            r < 0 ? errno : 0, r < 0 ? strerror(errno) : "-");
        break;
    }
    case X_FREE_DMABUF:
        say("X free-dmabuf: munmap + close dmabuf/memfd/udmabuf fds with "
            "sent=%" PRIu64 " completed=%" PRIu64 "\n",
            sent, c_next);
        munmap(map, o.size);
        close(dmabuf);
        close(memfd);
        close(devfd);
        break;
    case X_NONE:
        break;
    }

    /* wait for every completion */
    if (zc) {
        double tw = now();

        while (c_next < sent && now() - tw < o.compl_wait) {
            wait_err(sock, 100);
            reap(sock);
        }
    }
    double tall = now() - t0;
    int so_err = 0;
    socklen_t sl = sizeof(so_err);

    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &sl);
    int ok = !send_errno && !short_sends &&
             (!zc || (c_next == sent && !c_err && !c_gap)) &&
             (o.mode != MODE_DEVMEM || !c_copied) && !so_err;

    say("RESULT tx %s sends=%" PRIu64 " bytes=%" PRIu64 " completed=%" PRIu64
        " notifications=%" PRIu64 " copied=%" PRIu64 " compl_errors=%" PRIu64
        " compl_gaps=%" PRIu64 " short_sends=%" PRIu64
        " send_errno=%d(%s) so_error=%d(%s) send_secs=%.2f total_secs=%.2f"
        " MiB/s=%.1f\n",
        ok ? "PASS" : "FAIL", sent, bytes, c_next, c_notif, c_copied, c_err,
        c_gap, short_sends, send_errno, send_errno ? strerror(send_errno) : "-",
        so_err, so_err ? strerror(so_err) : "-", tsend, tall,
        tsend > 0 ? bytes / tsend / 1048576.0 : 0.0);
    close(sock);
    if (nl_fd >= 0)
        close(nl_fd);
    return ok ? 0 : 1;
}

static uint64_t num(const char *s)
{
    uint64_t v;

    if (!ernic_parse_size(s, &v))
        die("bad number %s", s);
    return v;
}

static int num_int(const char *s)
{
    int v;

    if (!ernic_parse_int(s, &v))
        die("bad number %s", s);
    return v;
}

int main(int argc, char **argv)
{
    int c;

    signal(SIGPIPE, SIG_IGN);
    while ((c = getopt(
                argc, argv,
                "rf:s:c:p:S:P:o:l:z:g:a:n:T:w:Bm:t:NX:DC:W:b:u:vF:R:G:J")) !=
           -1) {
        switch (c) {
        case 'r':
            o.receiver = 1;
            break;
        case 'f':
            o.ifname = optarg;
            break;
        case 'G':
            o.gpu_node = optarg;
            break;
        case 'J':
            o.gpu_gtt = 1;
            break;
        case 's':
            o.addr = optarg;
            break;
        case 'c':
            o.src = optarg;
            break;
        case 'p':
            o.port = num_int(optarg);
            break;
        case 'S':
            o.size = num(optarg);
            break;
        case 'P':
            o.seed = num(optarg);
            break;
        case 'o':
            o.off = num(optarg);
            break;
        case 'l':
            o.len = num(optarg);
            break;
        case 'z':
            o.chunk = num(optarg);
            break;
        case 'g':
            o.gap = num(optarg);
            break;
        case 'a':
            o.adv = num(optarg);
            break;
        case 'n':
            o.count = num(optarg);
            break;
        case 'T':
            o.secs = num_int(optarg);
            o.count = 0;
            break;
        case 'w':
            o.window = num(optarg);
            break;
        case 'B':
            o.no_bindtodevice = 1;
            break;
        case 'm':
            if (!strcmp(optarg, "devmem"))
                o.mode = MODE_DEVMEM;
            else if (!strcmp(optarg, "zc"))
                o.mode = MODE_ZC;
            else if (!strcmp(optarg, "copy"))
                o.mode = MODE_COPY;
            else
                die("-m devmem|zc|copy");
            break;
        case 't':
            o.sndtimeo = num_int(optarg);
            break;
        case 'N':
            o.nodelay = 1;
            break;
        case 'X':
            if (!strcmp(optarg, "close-sock"))
                o.xact = X_CLOSE_SOCK;
            else if (!strcmp(optarg, "unbind"))
                o.xact = X_UNBIND;
            else if (!strcmp(optarg, "free-dmabuf"))
                o.xact = X_FREE_DMABUF;
            else if (!strcmp(optarg, "exit"))
                o.xact = X_EXIT;
            else
                die("-X close-sock|unbind|free-dmabuf|exit");
            break;
        case 'D':
            o.dma_debug = 1;
            break;
        case 'C':
            o.compl_wait = num_int(optarg);
            break;
        case 'W':
            o.idle = num_int(optarg);
            break;
        case 'b':
            o.sndbuf = num(optarg);
            break;
        case 'u':
            o.pace_us = num_int(optarg);
            break;
        case 'v':
            o.verbose++;
            break;
        case 'F':
            o.pfn_file = optarg;
            break;
        case 'R':
            o.retry_secs = num_int(optarg);
            break;
        default:
            die("usage: see the comment at the top of devmem_tx.c");
        }
    }
    plan_init();
    return o.receiver ? run_receiver() : run_sender();
}
