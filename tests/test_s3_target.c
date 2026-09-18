/*
 * test_s3_target.c — unit tests for the in-process S3-over-RDMA object store
 *
 * The target in src/s3_target.c is deliberately transport- and
 * DMA-agnostic: it is handed a parsed HTTP request and a pair of
 * callbacks and hands back an HTTP response. That is what makes this
 * test possible at all -- it stands a plain byte array in for the guest
 * memory a client registers, and drives the request sequence hipObject
 * emits, from a plain PUT through a ranged RDMA GET to a three-call
 * multipart upload, with no VM, no RDMA device, and no server process.
 *
 * The RDMA path is the interesting half, so the fake DMA records what it
 * was asked to move and the test checks the client buffer afterwards
 * rather than trusting the status code: a 200 with nothing written to
 * the client's memory is the failure this suite exists to catch.
 *
 * Failures print "FAIL <case>: <detail>" and main() returns the count, so
 * a regression names itself rather than just tripping an assert.
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3_http.h"
#include "s3_target.h"
#include "s3_token.h"

#define HOST_BUF_SIZE (1u << 20)
#define HOST_BASE     0x40000000ull
#define HOST_RKEY     0x00c0ffeeu

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

/* ---- Fake guest memory -------------------------------------------------- */

struct host_mem {
    uint8_t buf[HOST_BUF_SIZE];
    uint32_t short_by; /* move this many bytes fewer, to fake a failed WR */
    bool wrong_key;    /* refuse anything that is not HOST_RKEY */
};

static bool host_range_ok(struct host_mem *h, uint32_t key, uint64_t addr,
                          uint32_t len, uint64_t *off)
{
    if (h->wrong_key && key != HOST_RKEY)
        return false;
    if (addr < HOST_BASE || addr - HOST_BASE + len > HOST_BUF_SIZE)
        return false;
    *off = addr - HOST_BASE;
    return true;
}

static uint32_t host_from(void *ctx, uint32_t key, uint64_t addr, void *dst,
                          uint32_t len)
{
    struct host_mem *h = ctx;
    uint64_t off;

    if (!host_range_ok(h, key, addr, len, &off))
        return 0;
    if (h->short_by >= len)
        return 0;
    len -= h->short_by;
    memcpy(dst, h->buf + off, len);
    return len;
}

static uint32_t host_to(void *ctx, uint32_t key, uint64_t addr, const void *src,
                        uint32_t len)
{
    struct host_mem *h = ctx;
    uint64_t off;

    if (!host_range_ok(h, key, addr, len, &off))
        return 0;
    if (h->short_by >= len)
        return 0;
    len -= h->short_by;
    memcpy(h->buf + off, src, len);
    return len;
}

static const struct s3_dma_ops host_ops = {
    .from_host = host_from,
    .to_host = host_to,
};

/* ---- Request helpers ---------------------------------------------------- */

/*
 * Build a request the same way the TCP endpoint would: render it as
 * text and run it through the real parser, so the tests never construct
 * a struct the parser could not itself produce.
 */
static bool build(struct s3_http_request *req, char *scratch, size_t scratchlen,
                  const char *method, const char *target, const char *token,
                  const char *extra, const void *body, size_t body_len)
{
    int n = snprintf(scratch, scratchlen,
                     "%s %s HTTP/1.1\r\nHost: 192.168.200.1:9000\r\n", method,
                     target);
    if (n < 0 || (size_t)n >= scratchlen)
        return false;
    size_t at = (size_t)n;

    if (token != NULL) {
        n = snprintf(scratch + at, scratchlen - at, "x-amz-rdma-token: %s\r\n",
                     token);
        if (n < 0 || (size_t)n >= scratchlen - at)
            return false;
        at += (size_t)n;
    }
    if (extra != NULL) {
        n = snprintf(scratch + at, scratchlen - at, "%s\r\n", extra);
        if (n < 0 || (size_t)n >= scratchlen - at)
            return false;
        at += (size_t)n;
    }
    if (body_len > 0) {
        n = snprintf(scratch + at, scratchlen - at, "Content-Length: %zu\r\n",
                     body_len);
        if (n < 0 || (size_t)n >= scratchlen - at)
            return false;
        at += (size_t)n;
    }
    if (at + 2 + body_len >= scratchlen)
        return false;
    memcpy(scratch + at, "\r\n", 2);
    at += 2;
    if (body_len > 0)
        memcpy(scratch + at, body, body_len);

    return s3_http_parse(scratch, at + body_len, req) > 0;
}

/* Mint the token a client would over a window of its registered buffer. */
static void mint(char *out, uint64_t off, uint64_t len)
{
    struct s3_token t;

    memset(&t, 0, sizeof(t));
    t.transport = S3_TRANSPORT_RC;
    t.qp_num = 0x11;
    t.gid[10] = 0xff;
    t.gid[11] = 0xff;
    t.gid[12] = 192;
    t.gid[13] = 168;
    t.gid[14] = 200;
    t.gid[15] = 9;
    t.rkey = HOST_RKEY;
    t.remote_addr = HOST_BASE + off;
    t.length = len;
    t.port_num = 1;
    s3_token_encode(&t, out);
}

/* Exec a request and hand the caller the response plus its body as text. */
struct reply {
    struct s3_http_response resp;
    char *text; /* NUL-terminated copy of the body */
};

static bool exec_req(struct s3_target *t, struct reply *r,
                     const struct s3_dma_ops *dma, void *dma_ctx,
                     const char *method, const char *target, const char *token,
                     const char *extra, const void *body, size_t body_len)
{
    static char scratch[S3_HTTP_REQUEST_MAX];
    struct s3_http_request req;

    memset(r, 0, sizeof(*r));
    if (!build(&req, scratch, sizeof(scratch), method, target, token, extra,
               body, body_len))
        return false;

    s3_target_exec(t, &req, dma, dma_ctx, &r->resp);

    r->text = malloc(r->resp.body_len + 1);
    if (r->text == NULL)
        return false;
    if (r->resp.body_len)
        memcpy(r->text, r->resp.body, r->resp.body_len);
    r->text[r->resp.body_len] = '\0';
    return true;
}

static void reply_free(struct reply *r)
{
    free(r->text);
    r->text = NULL;
    s3_http_response_free(&r->resp);
}

static const char *hdr(const struct reply *r, const char *name)
{
    for (unsigned i = 0; i < r->resp.nhdr; i++)
        if (strcasecmp(r->resp.hdr[i].name, name) == 0)
            return r->resp.hdr[i].value;
    return NULL;
}

static struct s3_target *make_target(const char *opts)
{
    struct s3_target_cfg cfg;
    char err[128] = "";

    s3_target_cfg_defaults(&cfg);
    if (opts != NULL && !s3_target_cfg_parse(&cfg, opts, err, sizeof(err))) {
        printf("FAIL setup: cfg parse \"%s\": %s\n", opts, err);
        failures++;
        return NULL;
    }
    struct s3_target *t = s3_target_create(&cfg, err, sizeof(err));
    if (t == NULL) {
        printf("FAIL setup: create: %s\n", err);
        failures++;
    }
    return t;
}

/* ---- Tests -------------------------------------------------------------- */

static void test_cfg_parse(void)
{
    const char *name = "cfg-parse";
    struct s3_target_cfg cfg;
    char err[128];

    s3_target_cfg_defaults(&cfg);
    if (strcmp(cfg.bucket, "ernic") != 0)
        fail(name, "default bucket \"%s\"", cfg.bucket);
    if (cfg.trsvcid != 9000)
        fail(name, "default port %u", cfg.trsvcid);
    if (cfg.traddr != 0xc0a8c801u)
        fail(name, "default traddr 0x%08x", cfg.traddr);

    s3_target_cfg_defaults(&cfg);
    if (!s3_target_cfg_parse(&cfg,
                             "bucket=data,size=4M,ip=10.0.0.5,port=8080,"
                             "objects=8",
                             err, sizeof(err))) {
        fail(name, "rejected a valid option string: %s", err);
    } else {
        if (strcmp(cfg.bucket, "data") != 0)
            fail(name, "bucket \"%s\"", cfg.bucket);
        if (cfg.capacity != 4ull * 1024 * 1024)
            fail(name, "capacity %llu", (unsigned long long)cfg.capacity);
        if (cfg.traddr != 0x0a000005u)
            fail(name, "traddr 0x%08x", cfg.traddr);
        if (cfg.trsvcid != 8080)
            fail(name, "port %u", cfg.trsvcid);
        if (cfg.max_objects != 8)
            fail(name, "objects %u", cfg.max_objects);
    }

    static const char *bad[] = {
        "bucket=",      "bucket=No_Caps_Allowed",
        "ip=256.1.1.1", "ip=not-an-address",
        "port=0",       "port=99999",
        "size=12Q",     "nosuchoption=1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s3_target_cfg_defaults(&cfg);
        err[0] = '\0';
        if (s3_target_cfg_parse(&cfg, bad[i], err, sizeof(err)))
            fail(name, "accepted \"%s\"", bad[i]);
        else if (err[0] == '\0')
            fail(name, "\"%s\" failed without saying why", bad[i]);
    }

    ok(name);
}

/* A PUT with the payload in the HTTP body, read back the same way: the
 * control-plane-only path that makes the store reachable from curl. */
static void test_body_put_get(void)
{
    const char *name = "body-put-get";
    struct s3_target *t = make_target(NULL);
    struct reply r;

    if (t == NULL)
        return;

    static const char payload[] = "the quick brown fox";
    if (!exec_req(t, &r, NULL, NULL, "PUT", "/ernic/fox.txt", NULL, NULL,
                  payload, strlen(payload))) {
        fail(name, "PUT could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "PUT status %d", r.resp.status);

    /* Clients compare the ETag against their own digest, so it has to be
     * the real MD5; this one is cross-checked against md5sum(1). */
    const char *etag = hdr(&r, "ETag");
    if (etag == NULL ||
        strcmp(etag, "\"30f3c93e46436deb58ba70816a8ec124\"") != 0)
        fail(name, "ETag %s, want \"30f3c93e46436deb58ba70816a8ec124\"",
             etag ? etag : "(absent)");
    reply_free(&r);

    if (s3_target_object_count(t) != 1)
        fail(name, "object count %u after one PUT", s3_target_object_count(t));
    if (s3_target_bytes_used(t) != strlen(payload))
        fail(name, "bytes used %llu",
             (unsigned long long)s3_target_bytes_used(t));

    if (!exec_req(t, &r, NULL, NULL, "GET", "/ernic/fox.txt", NULL, NULL, NULL,
                  0)) {
        fail(name, "GET could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "GET status %d", r.resp.status);
    else if (r.resp.body_len != strlen(payload) ||
             memcmp(r.resp.body, payload, strlen(payload)) != 0)
        fail(name, "GET returned %zu bytes, not the payload", r.resp.body_len);
    reply_free(&r);

    /* HEAD carries the metadata and no body. */
    if (!exec_req(t, &r, NULL, NULL, "HEAD", "/ernic/fox.txt", NULL, NULL, NULL,
                  0)) {
        fail(name, "HEAD could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "HEAD status %d", r.resp.status);
    if (r.resp.body_len != 0)
        fail(name, "HEAD returned a %zu byte body", r.resp.body_len);
    const char *clen = hdr(&r, "x-amz-content-length");
    if (clen == NULL || atoi(clen) != (int)strlen(payload))
        fail(name, "HEAD length header \"%s\"", clen ? clen : "(absent)");
    reply_free(&r);

    /* Delete, then confirm the key is gone but the delete is idempotent. */
    if (!exec_req(t, &r, NULL, NULL, "DELETE", "/ernic/fox.txt", NULL, NULL,
                  NULL, 0)) {
        fail(name, "DELETE could not be built");
        goto out;
    }
    if (r.resp.status != 204)
        fail(name, "DELETE status %d", r.resp.status);
    reply_free(&r);

    if (exec_req(t, &r, NULL, NULL, "DELETE", "/ernic/fox.txt", NULL, NULL,
                 NULL, 0)) {
        if (r.resp.status != 204)
            fail(name, "second DELETE status %d, want 204", r.resp.status);
        reply_free(&r);
    }
    if (s3_target_object_count(t) != 0)
        fail(name, "object count %u after DELETE", s3_target_object_count(t));

    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic/fox.txt", NULL, NULL, NULL,
                 0)) {
        if (r.resp.status != 404)
            fail(name, "GET after DELETE status %d, want 404", r.resp.status);
        if (strstr(r.text, "<Code>NoSuchKey</Code>") == NULL)
            fail(name, "404 body is not an S3 error document: %s", r.text);
        reply_free(&r);
    }

    ok(name);
out:
    s3_target_destroy(t);
}

/* The RDMA path: the bytes must land in the client's buffer, and the
 * reply header must say how many did. */
static void test_rdma_put_get(void)
{
    const char *name = "rdma-put-get";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;
    const size_t len = 64 * 1024;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    for (size_t i = 0; i < len; i++)
        h->buf[i] = (uint8_t)(i * 31 + 7);

    mint(token, 0, len);
    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/blob", token, NULL, NULL,
                  0)) {
        fail(name, "RDMA PUT could not be built");
        goto out;
    }
    if (r.resp.status != 200) {
        fail(name, "RDMA PUT status %d: %s", r.resp.status, r.text);
        reply_free(&r);
        goto out;
    }

    const char *reply = hdr(&r, "x-amz-rdma-reply");
    int code = 0;
    struct s3_token peer;
    if (reply == NULL) {
        fail(name, "no x-amz-rdma-reply on an RDMA PUT");
    } else if (!s3_reply_parse_peer(reply, strlen(reply), &peer, &code)) {
        fail(name, "reply \"%s\" does not parse", reply);
    } else {
        if (code != 200)
            fail(name, "reply code %d", code);
        if (peer.length != len)
            fail(name, "reply token length %llu, want %zu",
                 (unsigned long long)peer.length, len);
        if (!s3_target_is_target_qpn(peer.qp_num))
            fail(name, "reply qpn 0x%x is not in the target range",
                 peer.qp_num);
    }
    const char *bytes = hdr(&r, "x-amz-rdma-bytes");
    if (bytes == NULL || strtoull(bytes, NULL, 10) != len)
        fail(name, "x-amz-rdma-bytes \"%s\"", bytes ? bytes : "(absent)");
    reply_free(&r);

    if (s3_target_bytes_used(t) != len)
        fail(name, "stored %llu bytes, want %zu",
             (unsigned long long)s3_target_bytes_used(t), len);

    /* GET the object back into a different window of the same buffer and
     * compare, which is what a real client's verify step does. */
    const uint64_t dst = 512u * 1024;
    memset(h->buf + dst, 0, len);
    mint(token, dst, len);
    if (!exec_req(t, &r, &host_ops, h, "GET", "/ernic/blob", token, NULL, NULL,
                  0)) {
        fail(name, "RDMA GET could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "RDMA GET status %d: %s", r.resp.status, r.text);
    else if (r.resp.body_len != 0)
        fail(name, "RDMA GET put %zu bytes in the HTTP body", r.resp.body_len);
    else if (memcmp(h->buf, h->buf + dst, len) != 0)
        fail(name, "the object did not come back byte-for-byte");
    reply_free(&r);

    const struct s3_target_stats *st = s3_target_statistics(t);
    if (st->puts != 1 || st->gets != 1)
        fail(name, "stats: %llu puts, %llu gets", (unsigned long long)st->puts,
             (unsigned long long)st->gets);
    if (st->write_bytes != len || st->read_bytes != len)
        fail(name, "stats: %llu written, %llu read",
             (unsigned long long)st->write_bytes,
             (unsigned long long)st->read_bytes);

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

/*
 * What a real client puts on the wire: a token *and* a Content-Length
 * naming the object, with no body behind it.  The length is well past
 * S3_HTTP_REQUEST_MAX, which bounds a body on the socket and must not
 * bound an object that never touches it.
 */
static void test_rdma_put_with_content_length(void)
{
    const char *name = "rdma-put-content-length";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;
    const size_t len = 64 * 1024;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    for (size_t i = 0; i < len; i++)
        h->buf[i] = (uint8_t)(i * 17 + 3);

    mint(token, 0, len);
    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/declared", token,
                  "Content-Length: 65536", NULL, 0)) {
        fail(name, "the request the client sends could not be parsed");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "status %d: %s", r.resp.status, r.text);
    else if (s3_target_bytes_used(t) != len)
        fail(name, "stored %llu bytes, want %zu",
             (unsigned long long)s3_target_bytes_used(t), len);
    else
        ok(name);
    reply_free(&r);

out:
    free(h);
    s3_target_destroy(t);
}

static void test_rdma_ranged_get(void)
{
    const char *name = "rdma-ranged-get";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;
    const size_t len = 4096;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    for (size_t i = 0; i < len; i++)
        h->buf[i] = (uint8_t)i;
    mint(token, 0, len);
    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/ramp", token, NULL, NULL,
                  0)) {
        fail(name, "PUT could not be built");
        goto out;
    }
    reply_free(&r);

    struct {
        const char *range;
        size_t off, want;
        int status;
    } cases[] = {
        {"bytes=0-99", 0, 100, 206},
        {"bytes=100-199", 100, 100, 206},
        {"bytes=4000-", 4000, 96, 206},
        {"bytes=-64", len - 64, 64, 206}, /* suffix range */
        {"bytes=0-99999", 0, len, 206},   /* clamped to the object */
        {"bytes=9999-10000", 0, 0, 416},  /* starts past the end */
        {"bytes=200-100", 0, 0, 416},     /* inverted */
        {"chunks=0-99", 0, 0, 416},       /* unsupported unit */
    };

    const uint64_t dst = 512u * 1024;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char hdrline[64];
        snprintf(hdrline, sizeof(hdrline), "Range: %s", cases[i].range);
        memset(h->buf + dst, 0xaa, len);
        mint(token, dst, len);

        if (!exec_req(t, &r, &host_ops, h, "GET", "/ernic/ramp", token, hdrline,
                      NULL, 0)) {
            fail(name, "\"%s\": could not be built", cases[i].range);
            continue;
        }
        if (r.resp.status != cases[i].status) {
            fail(name, "\"%s\": status %d, want %d", cases[i].range,
                 r.resp.status, cases[i].status);
            reply_free(&r);
            continue;
        }
        if (cases[i].status == 206) {
            if (memcmp(h->buf + dst, h->buf + cases[i].off, cases[i].want) != 0)
                fail(name, "\"%s\": wrong bytes delivered", cases[i].range);
            const char *cr = hdr(&r, "Content-Range");
            if (cr == NULL)
                fail(name, "\"%s\": no Content-Range", cases[i].range);
        }
        reply_free(&r);
    }

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

/*
 * A suffix range names the last N bytes, and an empty object has no
 * last byte: the answer is 416, not a 206 of nothing.  The clamp to the
 * object size takes the count to zero, which is what has to be caught.
 */
static void test_range_on_empty_object(void)
{
    const char *name = "range-on-empty-object";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    struct reply r;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/empty", NULL, NULL, NULL,
                  0)) {
        fail(name, "PUT could not be built");
        goto out;
    }
    if (r.resp.status != 200) {
        fail(name, "PUT status %d, want 200", r.resp.status);
        reply_free(&r);
        goto out;
    }
    reply_free(&r);

    if (!exec_req(t, &r, &host_ops, h, "GET", "/ernic/empty", NULL,
                  "Range: bytes=-64", NULL, 0)) {
        fail(name, "GET could not be built");
        goto out;
    }
    if (r.resp.status != 416)
        fail(name, "status %d, want 416", r.resp.status);
    else
        ok(name);
    reply_free(&r);
out:
    free(h);
    s3_target_destroy(t);
}

/* A DMA that moves fewer bytes than asked is a failed transfer, and the
 * client only learns that from the reply header. */
static void test_rdma_short_transfer(void)
{
    const char *name = "rdma-short-transfer";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;
    const size_t len = 8192;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    h->short_by = 16;
    mint(token, 0, len);
    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/torn", token, NULL, NULL,
                  0)) {
        fail(name, "PUT could not be built");
        goto out;
    }
    if (r.resp.status != 500)
        fail(name, "short RDMA read gave status %d, want 500", r.resp.status);
    const char *reply = hdr(&r, "x-amz-rdma-reply");
    int code = 0;
    struct s3_token peer;
    if (reply == NULL ||
        !s3_reply_parse_peer(reply, strlen(reply), &peer, &code) || code != 500)
        fail(name, "reply header did not carry the failure: \"%s\"",
             reply ? reply : "(absent)");
    reply_free(&r);

    if (s3_target_object_count(t) != 0)
        fail(name, "a torn PUT still created an object");

    /* A token whose rkey the DMA layer will not resolve is the same
     * failure seen from the other side. */
    h->short_by = 0;
    h->wrong_key = true;
    struct s3_token bad;
    memset(&bad, 0, sizeof(bad));
    bad.transport = S3_TRANSPORT_RC;
    bad.rkey = 0x1234u;
    bad.remote_addr = HOST_BASE;
    bad.length = len;
    s3_token_encode(&bad, token);

    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/torn", token, NULL, NULL,
                  0)) {
        fail(name, "second PUT could not be built");
        goto out;
    }
    if (r.resp.status != 500)
        fail(name, "unresolvable rkey gave status %d, want 500", r.resp.status);
    reply_free(&r);

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

static void test_rdma_errors(void)
{
    const char *name = "rdma-errors";
    struct s3_target *t = make_target("maxpart=64K");
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    /* A token that is not a token at all. */
    if (exec_req(t, &r, &host_ops, h, "PUT", "/ernic/k", "not-a-token", NULL,
                 NULL, 0)) {
        if (r.resp.status != 400)
            fail(name, "malformed token gave status %d, want 400",
                 r.resp.status);
        if (strstr(r.text, "<Code>InvalidArgument</Code>") == NULL)
            fail(name, "malformed token: wrong error code");
        reply_free(&r);
    }

    /* A zero-length token with no Content-Length: nothing to transfer. */
    mint(token, 0, 0);
    if (exec_req(t, &r, &host_ops, h, "PUT", "/ernic/k", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 411)
            fail(name, "lengthless RDMA PUT gave status %d, want 411",
                 r.resp.status);
        reply_free(&r);
    }

    /* Larger than maxpart. */
    mint(token, 0, 128 * 1024);
    if (exec_req(t, &r, &host_ops, h, "PUT", "/ernic/k", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 400)
            fail(name, "oversized RDMA PUT gave status %d, want 400",
                 r.resp.status);
        reply_free(&r);
    }

    /* A token on an endpoint with no data plane attached at all. */
    mint(token, 0, 4096);
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/k", token, NULL, NULL, 0)) {
        if (r.resp.status != 501)
            fail(name, "PUT with no data plane gave status %d, want 501",
                 r.resp.status);
        reply_free(&r);
    }

    /* A GET of a missing key still has to answer in the reply header, or
     * a client that is blocked on it never wakes up. */
    mint(token, 0, 4096);
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/absent", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 404)
            fail(name, "GET of a missing key gave status %d", r.resp.status);
        int code = 0;
        struct s3_token peer;
        const char *reply = hdr(&r, "x-amz-rdma-reply");
        if (reply == NULL ||
            !s3_reply_parse_peer(reply, strlen(reply), &peer, &code) ||
            code != 404)
            fail(name, "404 did not reach the reply header");
        reply_free(&r);
    }

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

static void test_routing(void)
{
    const char *name = "routing";
    struct s3_target *t = make_target(NULL);
    struct reply r;

    if (t == NULL)
        return;

    if (exec_req(t, &r, NULL, NULL, "GET", "/", NULL, NULL, NULL, 0)) {
        if (r.resp.status != 200 ||
            strstr(r.text, "<Name>ernic</Name>") == NULL)
            fail(name, "service GET: status %d body %s", r.resp.status, r.text);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "GET", "/nosuch/key", NULL, NULL, NULL,
                 0)) {
        if (r.resp.status != 404 ||
            strstr(r.text, "<Code>NoSuchBucket</Code>") == NULL)
            fail(name, "wrong bucket: status %d body %s", r.resp.status,
                 r.text);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "PATCH", "/ernic/key", NULL, NULL, NULL,
                 0)) {
        if (r.resp.status != 405)
            fail(name, "PATCH on an object gave status %d, want 405",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "DELETE", "/", NULL, NULL, NULL, 0)) {
        if (r.resp.status != 405)
            fail(name, "DELETE on the service endpoint gave status %d",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "POST", "/ernic/key", NULL, NULL, NULL,
                 0)) {
        if (r.resp.status != 400)
            fail(name, "POST with neither ?uploads nor ?uploadId gave %d",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "HEAD", "/ernic", NULL, NULL, NULL, 0)) {
        if (r.resp.status != 200 || r.resp.body_len != 0)
            fail(name, "HEAD on the bucket: status %d, %zu body bytes",
                 r.resp.status, r.resp.body_len);
        reply_free(&r);
    }

    /* Every response carries a request id, and it changes per request. */
    char first[64] = "";
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic", NULL, NULL, NULL, 0)) {
        const char *id = hdr(&r, "x-amz-request-id");
        if (id == NULL)
            fail(name, "no x-amz-request-id");
        else
            snprintf(first, sizeof(first), "%s", id);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic", NULL, NULL, NULL, 0)) {
        const char *id = hdr(&r, "x-amz-request-id");
        if (id != NULL && strcmp(id, first) == 0)
            fail(name, "two requests shared the id %s", id);
        reply_free(&r);
    }

    s3_target_destroy(t);
    ok(name);
}

static void test_list_objects(void)
{
    const char *name = "list-objects";
    struct s3_target *t = make_target(NULL);
    struct reply r;

    if (t == NULL)
        return;

    static const char *keys[] = {"a/one", "a/two", "b/three", "top"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char target[64];
        snprintf(target, sizeof(target), "/ernic/%s", keys[i]);
        if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1))
            reply_free(&r);
    }

    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2", NULL, NULL,
                 NULL, 0)) {
        if (strstr(r.text, "<KeyCount>4</KeyCount>") == NULL)
            fail(name, "unfiltered list: %s", r.text);
        if (strstr(r.text, "<IsTruncated>false</IsTruncated>") == NULL)
            fail(name, "unfiltered list claims truncation");
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            char want[64];
            snprintf(want, sizeof(want), "<Key>%s</Key>", keys[i]);
            if (strstr(r.text, want) == NULL)
                fail(name, "\"%s\" missing from the listing", keys[i]);
        }
        reply_free(&r);
    }

    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2&prefix=a%2F",
                 NULL, NULL, NULL, 0)) {
        if (strstr(r.text, "<KeyCount>2</KeyCount>") == NULL)
            fail(name, "prefix list: %s", r.text);
        if (strstr(r.text, "<Key>b/three</Key>") != NULL)
            fail(name, "prefix list leaked a non-matching key");
        reply_free(&r);
    }

    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2&max-keys=2",
                 NULL, NULL, NULL, 0)) {
        if (strstr(r.text, "<KeyCount>2</KeyCount>") == NULL)
            fail(name, "max-keys list: %s", r.text);
        if (strstr(r.text, "<IsTruncated>true</IsTruncated>") == NULL)
            fail(name, "max-keys list does not report truncation");
        reply_free(&r);
    }

    /* A key with XML metacharacters must not break the document. */
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/a%26b%3Cc", NULL, NULL, "x",
                 1))
        reply_free(&r);
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2", NULL, NULL,
                 NULL, 0)) {
        if (strstr(r.text, "<Key>a&amp;b&lt;c</Key>") == NULL)
            fail(name, "key metacharacters not escaped: %s", r.text);
        reply_free(&r);
    }

    s3_target_destroy(t);
    ok(name);
}

/* The three-call multipart sequence, over RDMA, with the composite ETag
 * checked for shape and the reassembled object checked byte-for-byte. */
static void test_multipart(void)
{
    const char *name = "multipart";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    char upload_id[64] = "";
    struct reply r;
    const size_t part = 16 * 1024;
    const unsigned nparts = 3;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    for (size_t i = 0; i < part * nparts; i++)
        h->buf[i] = (uint8_t)(i * 17 + 3);

    if (!exec_req(t, &r, NULL, NULL, "POST", "/ernic/big?uploads", NULL, NULL,
                  NULL, 0)) {
        fail(name, "initiate could not be built");
        goto out;
    }
    const char *idp = strstr(r.text, "<UploadId>");
    if (idp == NULL) {
        fail(name, "initiate has no UploadId: %s", r.text);
        reply_free(&r);
        goto out;
    }
    idp += 10;
    const char *ide = strstr(idp, "</UploadId>");
    snprintf(upload_id, sizeof(upload_id), "%.*s", (int)(ide - idp), idp);
    reply_free(&r);

    /* Upload the parts out of order: the target sorts them on complete. */
    static const unsigned order[] = {3, 1, 2};
    for (unsigned i = 0; i < nparts; i++) {
        unsigned n = order[i];
        char target[128];
        snprintf(target, sizeof(target), "/ernic/big?partNumber=%u&uploadId=%s",
                 n, upload_id);
        mint(token, (n - 1) * part, part);
        if (!exec_req(t, &r, &host_ops, h, "PUT", target, token, NULL, NULL,
                      0)) {
            fail(name, "part %u could not be built", n);
            goto out;
        }
        if (r.resp.status != 200)
            fail(name, "part %u status %d: %s", n, r.resp.status, r.text);
        else if (hdr(&r, "ETag") == NULL)
            fail(name, "part %u has no ETag", n);
        reply_free(&r);
    }

    /* Parts are not visible as objects until the upload completes. */
    if (s3_target_object_count(t) != 0)
        fail(name, "parts became objects before complete");

    char target[128];
    snprintf(target, sizeof(target), "/ernic/big?uploadId=%s", upload_id);
    if (!exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        fail(name, "complete could not be built");
        goto out;
    }
    if (r.resp.status != 200) {
        fail(name, "complete status %d: %s", r.resp.status, r.text);
        reply_free(&r);
        goto out;
    }
    /* A multipart ETag is 32 hex digits, a dash, and the part count. */
    char want_suffix[16];
    snprintf(want_suffix, sizeof(want_suffix), "-%u&quot;", nparts);
    if (strstr(r.text, want_suffix) == NULL)
        fail(name, "complete ETag is not in multipart form: %s", r.text);
    reply_free(&r);

    if (s3_target_object_count(t) != 1)
        fail(name, "complete did not produce exactly one object");
    if (s3_target_bytes_used(t) != part * nparts)
        fail(name, "reassembled %llu bytes, want %zu",
             (unsigned long long)s3_target_bytes_used(t), part * nparts);

    /* Read it back and compare against what we uploaded. */
    const uint64_t dst = 512u * 1024;
    memset(h->buf + dst, 0, part * nparts);
    mint(token, dst, part * nparts);
    if (!exec_req(t, &r, &host_ops, h, "GET", "/ernic/big", token, NULL, NULL,
                  0)) {
        fail(name, "readback could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "readback status %d", r.resp.status);
    else if (memcmp(h->buf, h->buf + dst, part * nparts) != 0)
        fail(name, "the reassembled object does not match what went in");
    reply_free(&r);

    /* The upload id is consumed by complete. */
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404)
            fail(name, "a second complete gave status %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

static void test_multipart_errors(void)
{
    const char *name = "multipart-errors";
    struct s3_target *t = make_target(NULL);
    struct reply r;
    char upload_id[64] = "";

    if (t == NULL)
        return;

    if (exec_req(t, &r, NULL, NULL, "POST", "/ernic/x?uploads", NULL, NULL,
                 NULL, 0)) {
        const char *idp = strstr(r.text, "<UploadId>");
        if (idp != NULL) {
            idp += 10;
            const char *ide = strstr(idp, "</UploadId>");
            snprintf(upload_id, sizeof(upload_id), "%.*s", (int)(ide - idp),
                     idp);
        }
        reply_free(&r);
    }

    char target[128];

    snprintf(target, sizeof(target), "/ernic/x?partNumber=0&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "d", 1)) {
        if (r.resp.status != 400)
            fail(name, "partNumber=0 gave status %d, want 400", r.resp.status);
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/x?partNumber=1&uploadId=nope");
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "d", 1)) {
        if (r.resp.status != 404)
            fail(name, "unknown uploadId gave status %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }

    /* A part uploaded against the wrong key is not this upload's part. */
    snprintf(target, sizeof(target), "/ernic/y?partNumber=1&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "d", 1)) {
        if (r.resp.status != 404)
            fail(name, "part on the wrong key gave status %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }

    /* Completing with no parts at all. */
    snprintf(target, sizeof(target), "/ernic/x?uploadId=%s", upload_id);
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 400)
            fail(name, "empty complete gave status %d, want 400",
                 r.resp.status);
        reply_free(&r);
    }

    /* Aborting releases the upload. */
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 204)
            fail(name, "abort gave status %d, want 204", r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404)
            fail(name, "a second abort gave status %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }

    s3_target_destroy(t);
    ok(name);
}

/* Capacity and the object-slot ceiling are what stop a runaway client
 * from turning the emulator into an OOM kill. */
static void test_capacity(void)
{
    const char *name = "capacity";
    struct s3_target *t = make_target("size=4K,objects=2");
    struct reply r;
    char payload[3000];

    if (t == NULL)
        return;

    memset(payload, 'z', sizeof(payload));

    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/one", NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 200)
            fail(name, "first PUT status %d", r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/two", NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 507)
            fail(name, "PUT past capacity gave status %d, want 507",
                 r.resp.status);
        reply_free(&r);
    }

    /* Overwriting a key reuses its space rather than adding to it. */
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/one", NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 200)
            fail(name, "overwrite status %d, want 200", r.resp.status);
        reply_free(&r);
    }
    if (s3_target_object_count(t) != 1)
        fail(name, "overwrite left %u objects", s3_target_object_count(t));

    /* Two small objects fit; a third has no slot. */
    if (exec_req(t, &r, NULL, NULL, "DELETE", "/ernic/one", NULL, NULL, NULL,
                 0))
        reply_free(&r);
    for (int i = 0; i < 3; i++) {
        char target[32];
        snprintf(target, sizeof(target), "/ernic/s%d", i);
        if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1)) {
            int want = i < 2 ? 200 : 507;
            if (r.resp.status != want)
                fail(name, "small PUT %d status %d, want %d", i, r.resp.status,
                     want);
            reply_free(&r);
        }
    }

    s3_target_destroy(t);
    ok(name);
}

static void test_peer_token(void)
{
    const char *name = "peer-token";
    struct s3_target *t = make_target("ip=10.11.12.13");
    struct s3_token peer;
    char dotted[32];

    if (t == NULL)
        return;

    s3_target_peer_token(t, 0x1234u, 0x5000ull, 4096, &peer);
    if (peer.transport != S3_TRANSPORT_RC)
        fail(name, "transport %u, want RC", peer.transport);
    if (!s3_target_is_target_qpn(peer.qp_num))
        fail(name, "qpn 0x%x is outside the target range", peer.qp_num);
    if (peer.rkey != 0x1234u)
        fail(name, "rkey 0x%x", peer.rkey);
    if (peer.remote_addr != 0x5000ull)
        fail(name, "addr 0x%llx", (unsigned long long)peer.remote_addr);
    if (peer.length != 4096)
        fail(name, "length %llu", (unsigned long long)peer.length);
    if (!s3_token_gid_ipv4(&peer, dotted, sizeof(dotted)))
        fail(name, "the peer GID is not IPv4-mapped");
    else if (strcmp(dotted, "10.11.12.13") != 0)
        fail(name, "peer GID is %s, not the configured address", dotted);

    s3_target_destroy(t);
    ok(name);
}

int main(void)
{
    test_cfg_parse();
    test_body_put_get();
    test_rdma_put_get();
    test_rdma_put_with_content_length();
    test_rdma_ranged_get();
    test_range_on_empty_object();
    test_rdma_short_transfer();
    test_rdma_errors();
    test_routing();
    test_list_objects();
    test_multipart();
    test_multipart_errors();
    test_capacity();
    test_peer_token();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 target tests passed\n");
    return failures;
}
