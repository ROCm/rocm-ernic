/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * s3_rdma_client.c — guest-side S3-over-RDMA client for the ernic emulator
 *
 * The emulator's s3 backend is a storage server: an HTTP control plane it
 * terminates on the emulated NIC, and a data plane that moves object bytes
 * straight into and out of registered guest memory.  This is the other half
 * -- the client hipObject would be in production -- so the whole path can be
 * exercised from inside the VM with nothing but libibverbs and a socket.
 *
 * A request carries an x-amz-rdma-token minted over a registered buffer; the
 * server DMAs against the rkey in it and answers in x-amz-rdma-reply.  No
 * queue pair is ever connected: the rkey resolves in the emulator's own MR
 * table, which is what makes a one-VM, one-server deployment a complete
 * object fabric.  The queue pair exists only so the token carries a real
 * QPN, as it must on hardware.
 *
 *   cc -O2 -Wall -Wextra -o s3_rdma_client s3_rdma_client.c -libverbs
 *   ./s3_rdma_client -d rocm_ernic0 -a 192.168.200.1 -p 9000
 *
 * The token encoding is duplicated here rather than shared with
 * src/s3_token.c: this file is scp'd into a guest and built there on its
 * own.  tests/test_s3_token.c pins the same layout against a hand-assembled
 * reference image, so the two cannot drift silently.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TOKEN_BIN_LEN  44
#define TOKEN_HEX_LEN  (TOKEN_BIN_LEN * 2)
#define TRANSPORT_RC   0x01
#define RESP_MAX       65536
#define DEFAULT_BUCKET "ernic"
#define DEFAULT_ADDR   "192.168.200.1"
#define DEFAULT_PORT   9000
#define GID_INDEX      0

static int failures;
static bool verbose;

struct client {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint8_t *buf;
    size_t buflen;
    union ibv_gid gid;
    uint16_t lid;
    uint8_t port_num;

    const char *host;
    uint16_t port;
    const char *bucket;
};

/* An HTTP response, split into the fields the checks below look at. */
struct reply {
    int status;
    long content_length;
    char rdma_reply[TOKEN_HEX_LEN + 16];
    long rdma_bytes;
    char etag[64];
    const char *body;
    size_t body_len;
    char raw[RESP_MAX];
    size_t raw_len;
};

static void check(const char *what, bool ok, const char *detail)
{
    if (ok) {
        printf("ok   %-28s %s\n", what, detail ? detail : "");
    } else {
        printf("FAIL %-28s %s\n", what, detail ? detail : "");
        failures++;
    }
}

/* ── token codec ──────────────────────────────────────────────── */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/*
 * Mint a token over [addr, addr+len) of the registered buffer.  Field order
 * and endianness are the cuObject v1.2.0 layout: transport, qp_num, gid,
 * rkey, remote_addr, length, port_num, lid.
 */
static void mint_token(const struct client *c, uint64_t addr, uint64_t len,
                       char *hex)
{
    uint8_t bin[TOKEN_BIN_LEN] = {0};

    bin[0] = TRANSPORT_RC;
    put_le32(&bin[1], c->qp ? c->qp->qp_num : 0);
    memcpy(&bin[5], c->gid.raw, 16);
    put_le32(&bin[21], c->mr->rkey);
    put_le64(&bin[25], addr);
    put_le64(&bin[33], len);
    bin[41] = c->port_num;
    bin[42] = (uint8_t)(c->lid);
    bin[43] = (uint8_t)(c->lid >> 8);

    for (int i = 0; i < TOKEN_BIN_LEN; i++) {
        snprintf(&hex[i * 2], 3, "%02x", bin[i]);
    }
}

/* ── HTTP ─────────────────────────────────────────────────────── */

static int http_connect(const struct client *c)
{
    struct sockaddr_in sa;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(c->port);
    if (inet_pton(AF_INET, c->host, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad address: %s\n", c->host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "connect %s:%u: %s\n", c->host, c->port,
                strerror(errno));
        close(fd);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static const char *header_value(const char *head, const char *name, char *out,
                                size_t outlen)
{
    size_t namelen = strlen(name);
    const char *p = head;

    out[0] = '\0';
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, name, namelen) != 0 || p[namelen] != ':') {
            continue;
        }
        p += namelen + 1;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *end = strpbrk(p, "\r\n");
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= outlen) {
            n = outlen - 1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
        return out;
    }
    return NULL;
}

/*
 * Send one request and read the whole response.  Connection: close, so the
 * server's FIN delimits the body and no second request shares the socket --
 * an object client is not a benchmark of the TCP stack.
 */
static bool http_do(struct client *c, const char *method, const char *path,
                    const char *const *extra, int nextra, const void *body,
                    size_t body_len, struct reply *r)
{
    char req[2048];
    int n;
    int fd = http_connect(c);

    if (fd < 0) {
        return false;
    }

    n = snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n",
                 method, path, c->host, c->port);
    for (int i = 0; i < nextra && n > 0 && (size_t)n < sizeof(req); i++) {
        n += snprintf(req + n, sizeof(req) - (size_t)n, "%s\r\n", extra[i]);
    }
    if (n > 0 && (size_t)n < sizeof(req)) {
        n += snprintf(req + n, sizeof(req) - (size_t)n,
                      "Content-Length: %zu\r\n\r\n", body_len);
    }
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        fprintf(stderr, "request too long\n");
        close(fd);
        return false;
    }
    if (verbose) {
        printf("--> %s %s\n", method, path);
    }
    if (!write_all(fd, req, (size_t)n) ||
        (body_len > 0 && !write_all(fd, body, body_len))) {
        fprintf(stderr, "short write to %s:%u\n", c->host, c->port);
        close(fd);
        return false;
    }

    memset(r, 0, sizeof(*r));
    while (r->raw_len + 1 < sizeof(r->raw)) {
        ssize_t got =
            read(fd, r->raw + r->raw_len, sizeof(r->raw) - r->raw_len - 1);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        r->raw_len += (size_t)got;
    }
    close(fd);
    r->raw[r->raw_len] = '\0';

    if (sscanf(r->raw, "HTTP/1.%*d %d", &r->status) != 1) {
        fprintf(stderr, "unparsable response (%zu bytes)\n", r->raw_len);
        return false;
    }

    char *sep = strstr(r->raw, "\r\n\r\n");
    if (sep == NULL) {
        fprintf(stderr, "response has no header terminator\n");
        return false;
    }
    *sep = '\0';
    r->body = sep + 4;
    r->body_len = r->raw_len - (size_t)(r->body - r->raw);

    char val[TOKEN_HEX_LEN + 16];
    r->content_length = -1;
    if (header_value(r->raw, "Content-Length", val, sizeof(val))) {
        r->content_length = strtol(val, NULL, 10);
    }
    r->rdma_bytes = -1;
    if (header_value(r->raw, "x-amz-rdma-bytes", val, sizeof(val))) {
        r->rdma_bytes = strtol(val, NULL, 10);
    }
    if (header_value(r->raw, "x-amz-rdma-reply", val, sizeof(val))) {
        snprintf(r->rdma_reply, sizeof(r->rdma_reply), "%s", val);
    }
    header_value(r->raw, "ETag", r->etag, sizeof(r->etag));
    if (verbose) {
        printf("<-- %d rdma-reply=%s bytes=%ld\n", r->status,
               r->rdma_reply[0] ? r->rdma_reply : "-", r->rdma_bytes);
    }
    return true;
}

/* The status the server put in front of any peer token in the reply. */
static int reply_code(const struct reply *r)
{
    if (r->rdma_reply[0] == '\0') {
        return -1;
    }
    if (strncmp(r->rdma_reply, "ok", 2) == 0) {
        return 200;
    }
    if (strncmp(r->rdma_reply, "err", 3) == 0) {
        return -1;
    }
    return (int)strtol(r->rdma_reply, NULL, 10);
}

/* ── verbs setup ──────────────────────────────────────────────── */

static bool client_open(struct client *c, const char *want_dev, size_t buflen)
{
    struct ibv_device **list;
    struct ibv_device *dev = NULL;
    int num = 0;

    list = ibv_get_device_list(&num);
    if (list == NULL || num == 0) {
        fprintf(stderr, "no RDMA devices found\n");
        return false;
    }
    for (int i = 0; i < num; i++) {
        const char *name = ibv_get_device_name(list[i]);
        if (want_dev == NULL || strcmp(name, want_dev) == 0) {
            dev = list[i];
            break;
        }
    }
    if (dev == NULL) {
        fprintf(stderr, "device %s not found\n", want_dev);
        ibv_free_device_list(list);
        return false;
    }
    printf("device: %s\n", ibv_get_device_name(dev));

    c->ctx = ibv_open_device(dev);
    ibv_free_device_list(list);
    if (c->ctx == NULL) {
        fprintf(stderr, "ibv_open_device failed\n");
        return false;
    }

    c->port_num = 1;
    struct ibv_port_attr pattr;
    if (ibv_query_port(c->ctx, c->port_num, &pattr) != 0) {
        fprintf(stderr, "ibv_query_port failed\n");
        return false;
    }
    c->lid = pattr.lid;
    if (ibv_query_gid(c->ctx, c->port_num, GID_INDEX, &c->gid) != 0) {
        fprintf(stderr, "ibv_query_gid failed; is the NIC addressed?\n");
        return false;
    }

    c->pd = ibv_alloc_pd(c->ctx);
    c->cq = ibv_create_cq(c->ctx, 16, NULL, NULL, 0);
    if (c->pd == NULL || c->cq == NULL) {
        fprintf(stderr, "ibv_alloc_pd/ibv_create_cq failed\n");
        return false;
    }

    c->buflen = buflen;
    c->buf = aligned_alloc(4096, buflen);
    if (c->buf == NULL) {
        fprintf(stderr, "cannot allocate a %zu byte buffer\n", buflen);
        return false;
    }
    memset(c->buf, 0, buflen);

    /*
     * REMOTE_READ as well as REMOTE_WRITE: a PUT has the server read out of
     * this buffer and a GET has it write in, both against the same rkey.
     */
    c->mr = ibv_reg_mr(c->pd, c->buf, buflen,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE);
    if (c->mr == NULL) {
        fprintf(stderr, "ibv_reg_mr of %zu bytes failed: %s\n", buflen,
                strerror(errno));
        return false;
    }

    struct ibv_qp_init_attr qia = {
        .send_cq = c->cq,
        .recv_cq = c->cq,
        .cap = {.max_send_wr = 8,
                .max_recv_wr = 8,
                .max_send_sge = 1,
                .max_recv_sge = 1},
        .qp_type = IBV_QPT_RC,
    };
    /* Cosmetic: the token carries a QPN even where nothing connects it. */
    c->qp = ibv_create_qp(c->pd, &qia);
    if (c->qp == NULL) {
        fprintf(stderr, "ibv_create_qp failed: %s\n", strerror(errno));
        return false;
    }

    char gidstr[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, c->gid.raw, gidstr, sizeof(gidstr));
    printf("buffer: %zu bytes, rkey 0x%08x, gid %s, qpn 0x%06x\n\n", buflen,
           c->mr->rkey, gidstr, c->qp->qp_num);
    return true;
}

static void client_close(struct client *c)
{
    if (c->qp) {
        ibv_destroy_qp(c->qp);
    }
    if (c->mr) {
        ibv_dereg_mr(c->mr);
    }
    if (c->cq) {
        ibv_destroy_cq(c->cq);
    }
    if (c->pd) {
        ibv_dealloc_pd(c->pd);
    }
    if (c->ctx) {
        ibv_close_device(c->ctx);
    }
    free(c->buf);
}

/* ── object operations ────────────────────────────────────────── */

/* PUT [off, off+len) of the registered buffer as @key. */
static bool obj_put(struct client *c, const char *key, size_t off, size_t len,
                    struct reply *r)
{
    char path[512];
    char tokhdr[TOKEN_HEX_LEN + 64];
    char hex[TOKEN_HEX_LEN + 1];
    const char *extra[1];

    mint_token(c, (uint64_t)(uintptr_t)(c->buf + off), len, hex);
    snprintf(tokhdr, sizeof(tokhdr), "x-amz-rdma-token: %s", hex);
    snprintf(path, sizeof(path), "/%s/%s", c->bucket, key);
    extra[0] = tokhdr;
    return http_do(c, "PUT", path, extra, 1, NULL, len, r);
}

/* GET @key into the registered buffer at @off, optionally a byte range. */
static bool obj_get(struct client *c, const char *key, size_t off, size_t len,
                    const char *range, struct reply *r)
{
    char path[512];
    char tokhdr[TOKEN_HEX_LEN + 64];
    char rangehdr[64];
    char hex[TOKEN_HEX_LEN + 1];
    const char *extra[2];
    int nextra = 1;

    mint_token(c, (uint64_t)(uintptr_t)(c->buf + off), len, hex);
    snprintf(tokhdr, sizeof(tokhdr), "x-amz-rdma-token: %s", hex);
    snprintf(path, sizeof(path), "/%s/%s", c->bucket, key);
    extra[0] = tokhdr;
    if (range != NULL) {
        snprintf(rangehdr, sizeof(rangehdr), "Range: %s", range);
        extra[nextra++] = rangehdr;
    }
    return http_do(c, "GET", path, extra, nextra, NULL, 0, r);
}

static bool obj_delete(struct client *c, const char *key, struct reply *r)
{
    char path[512];

    snprintf(path, sizeof(path), "/%s/%s", c->bucket, key);
    return http_do(c, "DELETE", path, NULL, 0, NULL, 0, r);
}

static void fill_pattern(uint8_t *p, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

static size_t diff_at(const uint8_t *a, const uint8_t *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return len;
}

/* ── functional checks ────────────────────────────────────────── */

/*
 * The buffer is split in two: the low half is what a PUT sends, the high
 * half is where a GET lands.  Comparing the halves is the whole point --
 * a server that answers 200 without moving bytes fails here and nowhere
 * else.
 */
static void test_round_trip(struct client *c, size_t len)
{
    uint8_t *src = c->buf;
    uint8_t *dst = c->buf + c->buflen / 2;
    struct reply r;
    char detail[256];

    fill_pattern(src, len, 0xa5a5u);
    memset(dst, 0, len);

    if (!obj_put(c, "roundtrip.bin", 0, len, &r)) {
        check("rdma-put", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "%zu bytes, status %d, reply %s", len,
             r.status, r.rdma_reply[0] ? r.rdma_reply : "-");
    check("rdma-put", r.status == 200 && reply_code(&r) == 200, detail);
    check("rdma-put-bytes", r.rdma_bytes == (long)len, detail);

    if (!obj_get(c, "roundtrip.bin", c->buflen / 2, len, NULL, &r)) {
        check("rdma-get", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "%zu bytes, status %d, etag %s", len,
             r.status, r.etag[0] ? r.etag : "-");
    check("rdma-get", r.status == 200 && r.rdma_bytes == (long)len, detail);

    size_t at = diff_at(src, dst, len);
    if (at == len) {
        snprintf(detail, sizeof(detail), "%zu bytes identical", len);
    } else {
        snprintf(detail, sizeof(detail), "first difference at byte %zu", at);
    }
    check("rdma-payload", at == len, detail);
}

static void test_ranged_get(struct client *c)
{
    const size_t len = 4096;
    const size_t off = 1024;
    const size_t span = 512;
    uint8_t *src = c->buf;
    uint8_t *dst = c->buf + c->buflen / 2;
    struct reply r;
    char detail[128];

    fill_pattern(src, len, 0x1234u);
    memset(dst, 0, len);

    if (!obj_put(c, "ranged.bin", 0, len, &r) || r.status != 200) {
        check("ranged-setup", false, "PUT failed");
        return;
    }

    char range[64];
    snprintf(range, sizeof(range), "bytes=%zu-%zu", off, off + span - 1);
    if (!obj_get(c, "ranged.bin", c->buflen / 2, span, range, &r)) {
        check("ranged-get", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "status %d, %ld bytes", r.status,
             r.rdma_bytes);
    check("ranged-get", r.status == 206 && r.rdma_bytes == (long)span, detail);
    check("ranged-payload", diff_at(src + off, dst, span) == span, range);
}

/*
 * A GET into a buffer smaller than the object is a short transfer, not an
 * error: the token's length caps what the server may write, which is the
 * only thing standing between a large object and a client's heap.
 */
static void test_short_buffer(struct client *c)
{
    const size_t len = 8192;
    const size_t window = 1024;
    uint8_t *src = c->buf;
    uint8_t *dst = c->buf + c->buflen / 2;
    struct reply r;
    char detail[128];

    fill_pattern(src, len, 0x9e37u);
    memset(dst, 0, len);

    if (!obj_put(c, "short.bin", 0, len, &r) || r.status != 200) {
        check("short-setup", false, "PUT failed");
        return;
    }
    if (!obj_get(c, "short.bin", c->buflen / 2, window, NULL, &r)) {
        check("short-get", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "asked for %zu of %zu, got %ld", window,
             len, r.rdma_bytes);
    check("short-get", r.status == 200 && r.rdma_bytes == (long)window, detail);
    check("short-payload", diff_at(src, dst, window) == window, detail);
    check("short-no-overrun", dst[window] == 0, "byte past the window");
}

static void test_body_get(struct client *c)
{
    struct reply r;
    char detail[128];

    /* No token: the object comes back in the HTTP body, which is what an
     * ordinary S3 client that knows nothing about RDMA would see. */
    char path[512];
    snprintf(path, sizeof(path), "/%s/%s", c->bucket, "ranged.bin");
    if (!http_do(c, "GET", path, NULL, 0, NULL, 0, &r)) {
        check("body-get", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "status %d, %ld bytes", r.status,
             r.content_length);
    check("body-get", r.status == 200 && r.content_length == 4096, detail);
}

static void test_list_and_delete(struct client *c)
{
    struct reply r;
    char path[512];
    char detail[128];

    snprintf(path, sizeof(path), "/%s/", c->bucket);
    if (!http_do(c, "GET", path, NULL, 0, NULL, 0, &r)) {
        check("list-objects", false, "no response");
        return;
    }
    bool listed = r.status == 200 && strstr(r.body, "roundtrip.bin") != NULL &&
                  strstr(r.body, "ranged.bin") != NULL;
    snprintf(detail, sizeof(detail), "status %d, %zu bytes of XML", r.status,
             r.body_len);
    check("list-objects", listed, detail);

    for (const char *const *k =
             (const char *const[]){"roundtrip.bin", "ranged.bin", "short.bin",
                                   NULL};
         *k != NULL; k++) {
        if (!obj_delete(c, *k, &r) || r.status != 204) {
            check("delete-object", false, *k);
            return;
        }
    }
    check("delete-object", true, "three objects removed");

    if (!obj_get(c, "roundtrip.bin", c->buflen / 2, 4096, NULL, &r)) {
        check("delete-is-gone", false, "no response");
        return;
    }
    snprintf(detail, sizeof(detail), "status %d", r.status);
    check("delete-is-gone", r.status == 404, detail);
}

static void test_errors(struct client *c)
{
    struct reply r;
    char path[512];
    char detail[64];

    snprintf(path, sizeof(path), "/%s/%s", c->bucket, "absent.bin");
    if (http_do(c, "GET", path, NULL, 0, NULL, 0, &r)) {
        snprintf(detail, sizeof(detail), "status %d", r.status);
        check("missing-object-404", r.status == 404, detail);
    } else {
        check("missing-object-404", false, "no response");
    }

    /* A token the server cannot decode must be refused before any DMA. */
    const char *bad[] = {"x-amz-rdma-token: not-a-token"};
    snprintf(path, sizeof(path), "/%s/%s", c->bucket, "bad.bin");
    if (http_do(c, "PUT", path, bad, 1, NULL, 0, &r)) {
        snprintf(detail, sizeof(detail), "status %d", r.status);
        check("bad-token-400", r.status == 400, detail);
    } else {
        check("bad-token-400", false, "no response");
    }

    if (http_do(c, "GET", "/no-such-bucket/x", NULL, 0, NULL, 0, &r)) {
        snprintf(detail, sizeof(detail), "status %d", r.status);
        check("wrong-bucket-404", r.status == 404, detail);
    } else {
        check("wrong-bucket-404", false, "no response");
    }
}

/* ── perf sweep ───────────────────────────────────────────────── */

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * One CSV row per size, in the schema ci/report/publish-perf.py already
 * ingests: verb,size,bw_peak_GBs,bw_avg_GBs,msg_rate_mpps.  Verb "s3" is
 * what keeps these rows apart from the perftest and nvmeof ones.
 *
 * GETs, not PUTs: a GET has the server RDMA-WRITE into guest memory, which
 * is the direction the badge speaks for, and mixing the two would make the
 * number mean half of each.
 */
static void perf_sweep(struct client *c, const size_t *sizes, int nsizes,
                       int iters, FILE *csv)
{
    uint8_t *src = c->buf;
    size_t dst_off = c->buflen / 2;

    printf("\nGET sweep, %d iterations per size:\n", iters);
    printf("%12s %12s %12s %12s\n", "size", "avg GB/s", "peak GB/s", "Mops/s");

    for (int i = 0; i < nsizes; i++) {
        size_t len = sizes[i];
        struct reply r;
        char key[64];

        if (len > c->buflen / 2) {
            printf("%12zu  (skipped: larger than half the buffer)\n", len);
            continue;
        }
        snprintf(key, sizeof(key), "perf-%zu.bin", len);
        fill_pattern(src, len, (uint32_t)len);
        if (!obj_put(c, key, 0, len, &r) || r.status != 200) {
            fprintf(stderr, "perf: PUT of %zu bytes failed\n", len);
            failures++;
            continue;
        }

        /* One warm GET first: the first request on a fresh connection pays
         * for the object's allocation, not for the transfer. */
        if (!obj_get(c, key, dst_off, len, NULL, &r) || r.status != 200) {
            fprintf(stderr, "perf: GET of %zu bytes failed\n", len);
            failures++;
            continue;
        }

        double best = 0.0;
        double total = 0.0;
        for (int n = 0; n < iters; n++) {
            double t0 = now_s();
            if (!obj_get(c, key, dst_off, len, NULL, &r) || r.status != 200) {
                fprintf(stderr, "perf: GET of %zu bytes failed at iter %d\n",
                        len, n);
                failures++;
                break;
            }
            double dt = now_s() - t0;
            total += dt;
            if (dt > 0.0 && (double)len / dt > best) {
                best = (double)len / dt;
            }
        }
        if (total <= 0.0) {
            continue;
        }
        double avg = (double)len * iters / total;
        double ops = iters / total;

        printf("%12zu %12.4f %12.4f %12.4f\n", len, avg / 1e9, best / 1e9,
               ops / 1e6);
        if (csv != NULL) {
            fprintf(csv, "s3,%zu,%.6f,%.6f,%.6f\n", len, best / 1e9, avg / 1e9,
                    ops / 1e6);
        }
        obj_delete(c, key, &r);
    }
}

/* ── main ─────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -d DEV     RDMA device (default: the first one)\n"
            "  -a ADDR    endpoint address (default: %s)\n"
            "  -p PORT    endpoint port (default: %u)\n"
            "  -b BUCKET  bucket name (default: %s)\n"
            "  -s BYTES   buffer size (default: 8388608)\n"
            "  -i ITERS   perf iterations per size (default: 20)\n"
            "  -c FILE    write the perf sweep to FILE as CSV\n"
            "  -P         run only the perf sweep\n"
            "  -F         run only the functional checks\n"
            "  -v         log each request\n",
            argv0, DEFAULT_ADDR, DEFAULT_PORT, DEFAULT_BUCKET);
}

int main(int argc, char **argv)
{
    struct client c;
    const char *dev = NULL;
    const char *csv_path = NULL;
    size_t buflen = 8u << 20;
    int iters = 20;
    bool do_perf = true;
    bool do_func = true;
    int opt;

    memset(&c, 0, sizeof(c));
    c.host = DEFAULT_ADDR;
    c.port = DEFAULT_PORT;
    c.bucket = DEFAULT_BUCKET;

    while ((opt = getopt(argc, argv, "d:a:p:b:s:i:c:PFvh")) != -1) {
        switch (opt) {
        case 'd':
            dev = optarg;
            break;
        case 'a':
            c.host = optarg;
            break;
        case 'p':
            c.port = (uint16_t)atoi(optarg);
            break;
        case 'b':
            c.bucket = optarg;
            break;
        case 's':
            buflen = strtoul(optarg, NULL, 0);
            break;
        case 'i':
            iters = atoi(optarg);
            break;
        case 'c':
            csv_path = optarg;
            break;
        case 'P':
            do_func = false;
            break;
        case 'F':
            do_perf = false;
            break;
        case 'v':
            verbose = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (iters < 1 || buflen < (1u << 16)) {
        usage(argv[0]);
        return 1;
    }

    printf("S3 over RDMA: http://%s:%u/%s\n", c.host, c.port, c.bucket);
    if (!client_open(&c, dev, buflen)) {
        client_close(&c);
        return 1;
    }

    if (do_func) {
        test_round_trip(&c, 4096);
        test_round_trip(&c, 1u << 20);
        test_ranged_get(&c);
        test_short_buffer(&c);
        test_body_get(&c);
        test_list_and_delete(&c);
        test_errors(&c);
    }

    if (do_perf) {
        static const size_t sizes[] = {4096, 65536, 1u << 20};
        FILE *csv = NULL;

        if (csv_path != NULL) {
            csv = fopen(csv_path, "w");
            if (csv == NULL) {
                fprintf(stderr, "cannot write %s: %s\n", csv_path,
                        strerror(errno));
                failures++;
            } else {
                fprintf(csv, "verb,size,bw_peak_GBs,bw_avg_GBs,"
                             "msg_rate_mpps\n");
            }
        }
        perf_sweep(&c, sizes, (int)(sizeof(sizes) / sizeof(sizes[0])), iters,
                   csv);
        if (csv != NULL) {
            fclose(csv);
            printf("\nperf CSV written to %s\n", csv_path);
        }
    }

    client_close(&c);
    printf("\n%s\n",
           failures == 0 ? "all checks passed" : "there were failures");
    return failures == 0 ? 0 : 1;
}
