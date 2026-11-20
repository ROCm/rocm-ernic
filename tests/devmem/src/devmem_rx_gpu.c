/*
 * devmem_rx_gpu: devmem TCP Rx into a GPU buffer, verified through the GPU
 * path.
 *
 * Binds a dma-buf to RX queue(s) of a netdev (NETDEV_CMD_BIND_RX), accepts one
 * TCP connection, receives with recvmsg(MSG_SOCK_DEVMEM), and checks every
 * payload byte against the stream devmem_tx_ext2 sends for the same plan
 * options. The payload is never read from the socket: for each
 * SCM_DEVMEM_DMABUF cmsg (frag_offset, frag_size, frag_token) the bytes are
 * read from the dma-buf's memory at frag_offset
 *   - through this process's GEM mmap of the amdgpu BO (-G), or
 *   - through the parent's HIP device-to-host copy (-E fd -Y r,w -M shmfd;
 *     hip_devmem_rx: hipMalloc + hsa_amd_portable_export_dmabuf), or
 *   - through the memfd mapping (udmabuf, the control).
 * Tokens are held until their bytes have been verified, then returned with
 * SO_DEVMEM_DONTNEED (the pages go back to the NIC and are overwritten).
 *
 * dma-buf source: udmabuf (default) | -G /dev/dri/renderD128 [-J = GTT] |
 *                 -E FD [-O base offset] (inherited, e.g. from hip_devmem_rx).
 * The buffer is pre-filled with 0xa5 (by the parent for -E), so bytes the NIC
 * did not write cannot pass.
 *
 * Options: -f IF  -s ADDR (listen)  -p PORT  -t first queue (0)  -q queues (1)
 *   -S buffer bytes  plan as devmem_tx_ext2: -P seed -o -l -z -g -a -n (-S of
 *   the SENDER's buffer is -B here; default = -S)
 *   -H N   verify+return tokens whenever N tokens are held (default: pages/4;
 *          0 = after every recvmsg)
 *   -R FILE  write one line per cmsg: "D stream_pos frag_offset frag_size
 * token" or "L stream_pos size" -K FILE  after EOF, with the last batch
 * verified but its tokens still held and the binding live, print HOLD and wait
 * (-k secs, default 60) for FILE to appear (host-side check of the NIC server's
 * memory) -W idle secs, -v The interface must already have tcp-data-split on
 * and the flow must land on the bound queue (the scripts use one combined
 * channel).
 *
 * Numbers are decimal and take k/m/g suffixes.
 *
 * Build it for a guest as tests/devmem/README.md says; it needs the amdgpu
 * uapi headers and links src/ernic_gnu.c:
 *
 *   gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -O2 -Wall -Wextra -static -pthread -Isrc -isystem "$UAPI" \
 *       -o devmem_rx_gpu tests/devmem/src/devmem_rx_gpu.c src/ernic_gnu.c
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/udmabuf.h>
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "parse_int.h"

#ifndef SO_DEVMEM_LINEAR
#define SO_DEVMEM_LINEAR 78
#endif
#ifndef SO_DEVMEM_DMABUF
#define SO_DEVMEM_DMABUF 79
#endif
#ifndef SO_DEVMEM_DONTNEED
#define SO_DEVMEM_DONTNEED 80
#endif
#ifndef MSG_SOCK_DEVMEM
#define MSG_SOCK_DEVMEM 0x2000000
#endif
#ifndef NETLINK_EXT_ACK
#define NETLINK_EXT_ACK 11
#endif
/* include/uapi/linux/netdev.h, v7.2.6 */
#define DR_NETDEV_CMD_BIND_RX      13
#define DR_NETDEV_A_DMABUF_IFINDEX 1
#define DR_NETDEV_A_DMABUF_QUEUES  2
#define DR_NETDEV_A_DMABUF_FD      3
#define DR_NETDEV_A_DMABUF_ID      4
#define DR_NETDEV_A_QUEUE_ID       1
#define DR_NETDEV_A_QUEUE_TYPE     3
#define DR_NETDEV_QUEUE_TYPE_RX    0

/* include/uapi/linux/uio.h */
struct dr_dmabuf_cmsg {
    uint64_t frag_offset;
    uint32_t frag_size;
    uint32_t frag_token;
    uint32_t dmabuf_id;
    uint32_t flags;
};
struct dr_dmabuf_token {
    uint32_t token_start;
    uint32_t token_count;
};

static struct {
    const char *ifname, *addr;
    int port;
    unsigned qstart, qcount;
    uint64_t size;  /* -S RX dma-buf bytes */
    uint64_t psize; /* -B sender's buffer bytes (plan) */
    uint64_t seed, off, len, chunk, gap, adv, count;
    const char *gpu_node;
    int gpu_gtt;
    int ext_fd;
    uint64_t ext_base;
    int gate_r, gate_w, shm_fd;
    long hold; /* -H, -1 = default */
    const char *rec_file, *hold_file;
    int hold_secs;
    int idle;
    int verbose;
} o = {
    .port = 5400,
    .qcount = 1,
    .size = 64u << 20,
    .seed = 1,
    .len = 65536,
    .ext_fd = -1,
    .gate_r = -1,
    .gate_w = -1,
    .shm_fd = -1,
    .hold = -1,
    .hold_secs = 60,
    .idle = 30,
};

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

/* _Noreturn so that clang does not take the paths after a die() as live */
static _Noreturn void die(const char *fmt, ...)
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
    uint64_t w = (x >> 3) + seed * 0x100000001b3ULL;

    w ^= w >> 30;
    w *= 0xbf58476d1ce4e5b9ULL;
    w ^= w >> 27;
    w *= 0x94d049bb133111ebULL;
    w ^= w >> 31;
    return (uint8_t)(w >> ((x & 7) * 8));
}

static void plan_init(void)
{
    if (!o.psize)
        o.psize = o.size;
    if (!o.len)
        die("-l must be > 0");
    if (!o.chunk)
        o.chunk = o.len;
    p_iovcnt = (o.len + o.chunk - 1) / o.chunk;
    p_step = o.chunk + o.gap;
    p_span = (p_iovcnt - 1) * p_step + (o.len - (p_iovcnt - 1) * o.chunk);
    if (p_span > o.psize)
        die("one send spans %" PRIu64 " bytes > sender buffer %" PRIu64, p_span,
            o.psize);
    p_wrap = o.psize - p_span + 1;
}

static uint64_t plan_start(uint64_t i)
{
    return (o.off + i * o.adv) % p_wrap;
}

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

/* ------------------------------------------------------------------ netlink */

static int nl_fd = -1;

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

static int nl_check_error(struct nlmsghdr *h, const char *what)
{
    if (h->nlmsg_type != NLMSG_ERROR)
        return 0;
    struct nlmsgerr *e = NLMSG_DATA(h);

    if (!e->error)
        return 0;
    const char *msg = "";

    if (h->nlmsg_flags & NLM_F_ACK_TLVS) {
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

static struct nlattr *nla_put_u32(struct nlattr *a, int type, uint32_t v)
{
    a->nla_type = type;
    a->nla_len = NLA_HDRLEN + 4;
    memcpy(a + 1, &v, 4);
    return (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
}

static int nl_bind_rx(int ifindex, int dmabuf_fd, unsigned qstart,
                      unsigned qcount, uint32_t *id)
{
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    char reply[8192];
    int one = 1, n, family;
    struct {
        struct nlmsghdr h;
        struct genlmsghdr g;
        char attrs[1024];
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
    if (send(nl_fd, &req, req.h.nlmsg_len, 0) < 0)
        die("netlink send: %m");
    n = recv(nl_fd, reply, sizeof(reply), 0);
    struct nlmsghdr *h = (struct nlmsghdr *)reply;

    if (n < (int)sizeof(*h) || nl_check_error(h, "getfamily"))
        die("cannot resolve the netdev genetlink family");
    a = nla_find((struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN),
                 h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN), CTRL_ATTR_FAMILY_ID);
    if (!a)
        die("no family id");
    family = *(uint16_t *)(a + 1);

    if (qcount > 32)
        die("-q too large");
    memset(&req, 0, sizeof(req));
    req.h.nlmsg_type = family;
    req.h.nlmsg_flags = NLM_F_REQUEST;
    req.h.nlmsg_seq = 2;
    req.g.cmd = DR_NETDEV_CMD_BIND_RX;
    req.g.version = 1;
    a = (struct nlattr *)req.attrs;
    a = nla_put_u32(a, DR_NETDEV_A_DMABUF_IFINDEX, ifindex);
    a = nla_put_u32(a, DR_NETDEV_A_DMABUF_FD, dmabuf_fd);
    for (unsigned q = 0; q < qcount; q++) {
        struct nlattr *nest = a;

        nest->nla_type = DR_NETDEV_A_DMABUF_QUEUES | NLA_F_NESTED;
        a = (struct nlattr *)((char *)nest + NLA_HDRLEN);
        a = nla_put_u32(a, DR_NETDEV_A_QUEUE_TYPE, DR_NETDEV_QUEUE_TYPE_RX);
        a = nla_put_u32(a, DR_NETDEV_A_QUEUE_ID, qstart + q);
        nest->nla_len = (char *)a - (char *)nest;
    }
    req.h.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + ((char *)a - req.attrs));
    if (send(nl_fd, &req, req.h.nlmsg_len, 0) < 0)
        die("netlink send: %m");
    n = recv(nl_fd, reply, sizeof(reply), 0);
    h = (struct nlmsghdr *)reply;
    if (n < (int)sizeof(*h))
        die("short netlink reply");
    n = nl_check_error(h, "bind-rx");
    if (n)
        return n;
    a = nla_find((struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN),
                 h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN),
                 DR_NETDEV_A_DMABUF_ID);
    if (!a)
        die("bind-rx reply has no id");
    *id = *(uint32_t *)(a + 1);
    return 0;
}

/* ------------------------------------------------------------- dma-buf source
 */

static int gpu_alloc_dmabuf(const char *node, uint64_t size, int want_gtt,
                            uint8_t **map_out)
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
    return p.fd;
}

/* ------------------------------------------------------------------ records */

struct rec {
    uint64_t off; /* frag_offset; for linear: index into lin[] */
    uint32_t len;
    uint32_t token; /* UINT32_MAX = linear */
};

static struct rec *recs;
static size_t nrecs, caprecs;
static uint8_t *lin;
static size_t nlin, caplin;

static const uint8_t *rb; /* where frag bytes are read from */
static const char *rb_how;
static uint8_t *pagemap_seen; /* one byte per 4 KiB page of the dma-buf */

/* plan cursor */
static uint64_t c_i, c_j, c_in, c_start;
static uint64_t v_bytes, v_mism, v_mism_a5, v_mism_zero, v_first = UINT64_MAX;
static uint64_t st_dmabuf, st_linear, st_linear_bytes, st_tokens_ret,
    st_batches, st_readbacks;
static uint64_t st_minoff = UINT64_MAX, st_maxend, st_aligned, st_maxfrag,
                st_oob, st_extra;
static double st_rb_secs;

static void readback(void)
{
    double t = now();

    if (o.gate_w >= 0) {
        char ack;

        if (write(o.gate_w, "R\n", 2) != 2 || read(o.gate_r, &ack, 1) != 1)
            die("gate: parent gone");
        if (ack != 'k')
            die("gate: parent reported a failed device->host copy");
    }
    st_readbacks++;
    st_rb_secs += now() - t;
}

/* compare n bytes at p with the expected stream at the cursor */
static void check_bytes(const uint8_t *p, uint64_t n, uint64_t where)
{
    while (n) {
        uint64_t base, len, take;

        if (o.count && c_i >= o.count) {
            st_extra += n;
            return;
        }
        plan_iov(c_start, c_j, &base, &len);
        take = len - c_in;
        if (take > n)
            take = n;
        for (uint64_t k = 0; k < take; k++) {
            uint8_t exp = pat(base + c_in + k, o.seed);

            if (p[k] == exp)
                continue;
            if (v_first == UINT64_MAX) {
                v_first = v_bytes + k;
                say("MISMATCH first at stream=%" PRIu64
                    " rx_dmabuf_off=%" PRIu64 " exp=%02x got=%02x\n",
                    v_first, where + k, exp, p[k]);
            }
            v_mism++;
            v_mism_a5 += p[k] == 0xa5;
            v_mism_zero += p[k] == 0;
        }
        v_bytes += take;
        p += take;
        where += take;
        n -= take;
        c_in += take;
        if (c_in == len) {
            c_in = 0;
            if (++c_j == p_iovcnt) {
                c_j = 0;
                c_start = plan_start(++c_i);
            }
        }
    }
}

static void verify_batch(void)
{
    int need = 0;

    for (size_t r = 0; r < nrecs; r++)
        need |= recs[r].token != UINT32_MAX;
    if (need)
        readback();
    for (size_t r = 0; r < nrecs; r++) {
        if (recs[r].token == UINT32_MAX)
            check_bytes(lin + recs[r].off, recs[r].len, UINT64_MAX);
        else
            check_bytes(rb + recs[r].off, recs[r].len, recs[r].off);
    }
    st_batches++;
}

static void release_batch(int sock)
{
    struct dr_dmabuf_token tok[64];
    size_t n = 0;

    for (size_t r = 0; r <= nrecs; r++) {
        if (r < nrecs && recs[r].token != UINT32_MAX) {
            tok[n].token_start = recs[r].token;
            tok[n].token_count = 1;
            n++;
        }
        if (n == 64 || (r == nrecs && n)) {
            int ret = setsockopt(sock, SOL_SOCKET, SO_DEVMEM_DONTNEED, tok,
                                 n * sizeof(tok[0]));

            if (ret < 0)
                die("SO_DEVMEM_DONTNEED: %m");
            if ((size_t)ret != n)
                say("SO_DEVMEM_DONTNEED freed %d of %zu tokens\n", ret, n);
            st_tokens_ret += ret;
            n = 0;
        }
    }
    nrecs = 0;
    nlin = 0;
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

/* -Y RFD,WFD: the two ends of the gate the parent reads the buffer back on */
static void parse_gate(const char *s, int *rfd, int *wfd)
{
    const char *comma = strchr(s, ',');
    uint64_t r, w;

    if (!comma || !comma[1] ||
        !ernic_parse_u64_span(s, (size_t)(comma - s), &r) ||
        !ernic_parse_u64(comma + 1, &w) || r > INT_MAX || w > INT_MAX)
        die("-Y RFD,WFD");
    *rfd = (int)r;
    *wfd = (int)w;
}

int main(int argc, char **argv)
{
    int c, one = 1, dmabuf = -1, ifindex;
    uint8_t *map = NULL;
    uint32_t id = 0;
    FILE *recf = NULL;

    signal(SIGPIPE, SIG_IGN);
    while ((c = getopt(argc, argv,
                       "f:s:p:t:q:S:B:P:o:l:z:g:a:n:G:JE:O:Y:M:H:R:K:k:W:v")) !=
           -1) {
        switch (c) {
        case 'f':
            o.ifname = optarg;
            break;
        case 's':
            o.addr = optarg;
            break;
        case 'p':
            o.port = num_int(optarg);
            break;
        case 't':
            o.qstart = (unsigned)num(optarg);
            break;
        case 'q':
            o.qcount = (unsigned)num(optarg);
            break;
        case 'S':
            o.size = num(optarg);
            break;
        case 'B':
            o.psize = num(optarg);
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
        case 'G':
            o.gpu_node = optarg;
            break;
        case 'J':
            o.gpu_gtt = 1;
            break;
        case 'E':
            o.ext_fd = num_int(optarg);
            break;
        case 'O':
            o.ext_base = num(optarg);
            break;
        case 'Y':
            parse_gate(optarg, &o.gate_r, &o.gate_w);
            break;
        case 'M':
            o.shm_fd = num_int(optarg);
            break;
        case 'H':
            o.hold = (long)num(optarg);
            break;
        case 'R':
            o.rec_file = optarg;
            break;
        case 'K':
            o.hold_file = optarg;
            break;
        case 'k':
            o.hold_secs = num_int(optarg);
            break;
        case 'W':
            o.idle = num_int(optarg);
            break;
        case 'v':
            o.verbose++;
            break;
        default:
            die("usage: see the comment at the top of devmem_rx_gpu.c");
        }
    }
    plan_init();
    if (!o.ifname)
        die("-f IF required");
    ifindex = if_nametoindex(o.ifname);
    if (!ifindex)
        die("no such interface %s", o.ifname);
    if (o.size % 4096)
        die("-S must be a page multiple");

    if (o.ext_fd >= 0) {
        dmabuf = o.ext_fd;
        if (o.gate_r < 0 || o.shm_fd < 0)
            die("-E needs -Y and -M (the parent reads the buffer back)");
        rb = mmap(NULL, o.size, PROT_READ, MAP_SHARED, o.shm_fd, 0);
        if (rb == MAP_FAILED)
            die("mmap shm: %m");
        rb_how = "parent's device->host copy (HIP)";
        say("ext: inherited dma-buf fd=%d (size %lld), base offset %" PRIu64
            "\n",
            dmabuf, (long long)lseek(dmabuf, 0, SEEK_END), o.ext_base);
    } else if (o.gpu_node) {
        dmabuf = gpu_alloc_dmabuf(o.gpu_node, o.size, o.gpu_gtt, &map);
        memset(map, 0xa5, o.size);
        rb = map;
        rb_how = "GEM mmap of the BO";
        say("gpu: pre-filled %" PRIu64
            " bytes with 0xa5 through the GEM mapping\n",
            o.size);
    } else {
        int devfd = open("/dev/udmabuf", O_RDWR), memfd;

        if (devfd < 0)
            die("/dev/udmabuf: %m");
        memfd = ernic_gnu_memfd_create("devmem_rx");
        if (memfd < 0)
            die("memfd_create: %m");
        if (ftruncate(memfd, (off_t)o.size))
            die("ftruncate: %m");
        if (ernic_gnu_seal_size(memfd) < 0)
            die("F_ADD_SEALS: %m");
        map = mmap(NULL, o.size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (map == MAP_FAILED)
            die("mmap memfd: %m");
        memset(map, 0xa5, o.size);
        struct udmabuf_create uc = {
            .memfd = memfd, .offset = 0, .size = o.size};

        dmabuf = ioctl(devfd, UDMABUF_CREATE, &uc);
        if (dmabuf < 0)
            die("UDMABUF_CREATE: %m");
        rb = map;
        rb_how = "memfd mapping (udmabuf)";
    }
    rb += o.ext_base;

    uint64_t npages = o.size / 4096;
    size_t hold = o.hold < 0 ? npages / 4 : (size_t)o.hold;

    pagemap_seen = calloc(npages, 1);
    if (!pagemap_seen)
        die("calloc");
    if (o.rec_file && !(recf = fopen(o.rec_file, "w")))
        die("%s: %m", o.rec_file);

    c = nl_bind_rx(ifindex, dmabuf, o.qstart, o.qcount, &id);
    if (c) {
        say("RESULT rx BIND-FAILED errno=%d (%s)\n", -c, strerror(-c));
        return 3;
    }
    say("bind-rx ok: ifindex=%d queues=%u..%u dmabuf_id=%u size=%" PRIu64
        " pages=%" PRIu64 " hold=%zu tokens readback=%s\n",
        ifindex, o.qstart, o.qstart + o.qcount - 1, id, o.size, npages, hold,
        rb_how);

    struct sockaddr_in6 sa;
    int ls = socket(AF_INET6, SOCK_STREAM, 0), fd;

    if (ls < 0)
        die("socket: %m");
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    parse_addr(o.addr, o.port, &sa);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)))
        die("bind: %m");
    if (listen(ls, 1))
        die("listen: %m");
    say("RX listening port=%d\n", o.port);
    fd = accept(ls, NULL, NULL);
    if (fd < 0)
        die("accept: %m");
    close(ls);
    if (o.idle) {
        struct timeval tv = {.tv_sec = o.idle};

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    static char iobuf[1 << 16];
    static char ctrl[64 * 1024];
    uint64_t total = 0, ncalls = 0;
    const char *end = "eof";
    double t0 = now(), tlast = t0;

    c_start = plan_start(0);
    for (;;) {
        struct iovec iov = {.iov_base = iobuf, .iov_len = sizeof(iobuf)};
        struct msghdr msg = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = ctrl,
                             .msg_controllen = sizeof(ctrl)};
        ssize_t n = recvmsg(fd, &msg, MSG_SOCK_DEVMEM);
        size_t linpos = 0;

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            end = errno == EAGAIN ? "idle-timeout" : strerror(errno);
            break;
        }
        ncalls++;
        if (msg.msg_flags & MSG_CTRUNC)
            die("control data truncated");
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
             cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET ||
                (cm->cmsg_type != SO_DEVMEM_DMABUF &&
                 cm->cmsg_type != SO_DEVMEM_LINEAR))
                continue;
            struct dr_dmabuf_cmsg *d = (void *)CMSG_DATA(cm);

            if (nrecs == caprecs) {
                caprecs = caprecs ? caprecs * 2 : 4096;
                recs = realloc(recs, caprecs * sizeof(*recs));
                if (!recs)
                    die("realloc");
            }
            if (cm->cmsg_type == SO_DEVMEM_LINEAR) {
                /* the bytes are in the iov, in cmsg order */
                if (nlin + d->frag_size > caplin) {
                    caplin = (nlin + d->frag_size) * 2;
                    lin = realloc(lin, caplin);
                    if (!lin)
                        die("realloc");
                }
                memcpy(lin + nlin, iobuf + linpos, d->frag_size);
                recs[nrecs++] = (struct rec){
                    .off = nlin, .len = d->frag_size, .token = UINT32_MAX};
                nlin += d->frag_size;
                linpos += d->frag_size;
                st_linear++;
                st_linear_bytes += d->frag_size;
                if (recf)
                    fprintf(recf, "L %" PRIu64 " %u\n", total, d->frag_size);
                total += d->frag_size;
                continue;
            }
            if (d->dmabuf_id != id)
                say("cmsg for dmabuf id %u, bound %u\n", d->dmabuf_id, id);
            if (d->frag_offset + d->frag_size > o.size) {
                st_oob++;
                say("cmsg outside the buffer: off=%" PRIu64 " size=%u\n",
                    (uint64_t)d->frag_offset, d->frag_size);
                total += d->frag_size;
                continue;
            }
            recs[nrecs++] = (struct rec){.off = d->frag_offset,
                                         .len = d->frag_size,
                                         .token = d->frag_token};
            st_dmabuf++;
            if (d->frag_offset < st_minoff)
                st_minoff = d->frag_offset;
            if (d->frag_offset + d->frag_size > st_maxend)
                st_maxend = d->frag_offset + d->frag_size;
            st_aligned += !(d->frag_offset & 4095);
            if (d->frag_size > st_maxfrag)
                st_maxfrag = d->frag_size;
            for (uint64_t pg = d->frag_offset / 4096;
                 pg <= (d->frag_offset + d->frag_size - 1) / 4096; pg++)
                pagemap_seen[pg] = 1;
            if (recf)
                fprintf(recf, "D %" PRIu64 " %" PRIu64 " %u %u\n", total,
                        (uint64_t)d->frag_offset, d->frag_size, d->frag_token);
            total += d->frag_size;
        }
        if (nrecs >= hold || !hold) {
            verify_batch();
            release_batch(fd);
        }
        if (o.verbose && now() - tlast >= 10) {
            tlast = now();
            say("RX progress t=%.0f bytes=%" PRIu64 " verified=%" PRIu64
                " mismatches=%" PRIu64 " tokens_returned=%" PRIu64
                " batches=%" PRIu64 "\n",
                tlast - t0, total, v_bytes, v_mism, st_tokens_ret, st_batches);
        }
    }
    double dt = now() - t0;

    if (recf)
        fclose(recf);
    size_t last = nrecs;

    if (nrecs)
        verify_batch();
    if (o.hold_file) {
        struct stat sb;
        double th = now();

        say("HOLD verified=%" PRIu64 " mismatches=%" PRIu64
            " last-batch records=%zu held; waiting"
            " for %s\n",
            v_bytes, v_mism, last, o.hold_file);
        while (stat(o.hold_file, &sb) && now() - th < o.hold_secs)
            usleep(100000);
        say("HOLD over after %.1fs\n", now() - th);
    }
    release_batch(fd);

    uint64_t pages = 0;

    for (uint64_t pg = 0; pg < npages; pg++)
        pages += pagemap_seen[pg];
    uint64_t want = o.count * o.len;
    int complete =
        o.count ? (v_bytes == want && !st_extra) : (c_j == 0 && c_in == 0);
    int ok = !v_mism && complete && !st_oob && v_bytes == total && total > 0;

    say("RESULT rx %s bytes=%" PRIu64 " verified=%" PRIu64 " expected=%" PRIu64
        " whole_sends=%" PRIu64 " partial_send=%s extra=%" PRIu64
        " mismatches=%" PRIu64 " mismatch_a5=%" PRIu64 " mismatch_zero=%" PRIu64
        " end=%s secs=%.2f MiB/s=%.1f\n",
        ok ? "PASS" : "FAIL", total, v_bytes, want, c_i,
        (c_j || c_in) ? "yes" : "no", st_extra, v_mism, v_mism_a5, v_mism_zero,
        end, dt, dt > 0 ? total / dt / 1048576.0 : 0.0);
    say("RXSTAT recvmsg_calls=%" PRIu64 " dmabuf_cmsgs=%" PRIu64
        " linear_cmsgs=%" PRIu64 " linear_bytes=%" PRIu64
        " tokens_returned=%" PRIu64 " batches=%" PRIu64 " readbacks=%" PRIu64
        " readback_secs=%.2f frag_off_min=%" PRIu64 " frag_end_max=%" PRIu64
        " page_aligned_frags=%" PRIu64 " max_frag=%" PRIu64
        " distinct_pages=%" PRIu64 " of %" PRIu64 " out_of_buffer=%" PRIu64
        " readback=%s\n",
        ncalls, st_dmabuf, st_linear, st_linear_bytes, st_tokens_ret,
        st_batches, st_readbacks, st_rb_secs,
        st_minoff == UINT64_MAX ? 0 : st_minoff, st_maxend, st_aligned,
        st_maxfrag, pages, npages, st_oob, rb_how);
    close(fd);
    close(nl_fd);
    return ok ? 0 : 1;
}
