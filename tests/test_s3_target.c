/*
 * test_s3_target.c — unit tests for the in-process S3-over-RDMA object store
 *
 * The target in src/s3/s3_target.c is deliberately transport- and
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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ernic_attributes.h"
#include "parse_int.h"
#include "s3_alloc_fail.h"
#include "s3/s3_http.h"
#include "s3/s3_target.h"
#include "s3/s3_token.h"

#define HOST_BUF_SIZE (1u << 20)
#define HOST_BASE     0x40000000ull
#define HOST_RKEY     0x00c0ffeeu

static int failures;

static void fail(const char *name, const char *fmt, ...)
    ERNIC_PRINTF_FORMAT(2, 3);

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

/*
 * Every field of @a and @b equal, the bucket name's unused bytes included.
 * A whole-struct memcmp() would also compare the padding, which assigning a
 * field may change.
 */
static bool cfg_equal(const struct s3_target_cfg *a,
                      const struct s3_target_cfg *b)
{
    return memcmp(a->bucket, b->bucket, sizeof(a->bucket)) == 0 &&
           a->capacity == b->capacity && a->max_objects == b->max_objects &&
           a->traddr == b->traddr && a->trsvcid == b->trsvcid &&
           a->max_part == b->max_part;
}

/*
 * Lift the upload id out of an InitiateMultipartUploadResult into dst.
 *
 * A reply that names no id, or that opens the element without closing it,
 * is a defect in the target rather than a case the caller should carry on
 * from: an id read as empty still forms a syntactically valid uploadId=
 * query, so the request it is spliced into fails several calls later as a
 * 404 and the failure is charged to whatever that request was testing.
 * Reporting it here keeps it attached to the reply that caused it.
 *
 * Returns false having already called fail(), so callers can stop.
 */
static bool take_upload_id(const char *text, char *dst, size_t dst_size,
                           const char *name)
{
    static const char open_tag[] = "<UploadId>";
    const char *begin = strstr(text, open_tag);
    const char *end = begin != NULL
                          ? strstr(begin + sizeof(open_tag) - 1, "</UploadId>")
                          : NULL;

    if (end == NULL) {
        fail(name, "reply carries no complete UploadId: %s", text);
        return false;
    }
    if (dst_size == 0) {
        fail(name, "take_upload_id() called with no room for a NUL");
        return false;
    }
    begin += sizeof(open_tag) - 1;

    size_t len = (size_t)(end - begin);
    if (len > dst_size - 1)
        len = dst_size - 1;
    memcpy(dst, begin, len);
    dst[len] = '\0';
    return true;
}

/* ---- Fake guest memory -------------------------------------------------- */

struct host_mem {
    uint8_t buf[HOST_BUF_SIZE];
    uint32_t short_by; /* move this many bytes fewer, to fake a failed WR */
    bool wrong_key;    /* refuse anything that is not HOST_RKEY */
};

static bool host_range_ok(const struct host_mem *h, uint32_t key, uint64_t addr,
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

    /*
     * Every header appended below is far shorter than this. Checking the
     * headroom once, against a bound the compiler can carry through the
     * appends, is what keeps -Wformat-truncation from having to rediscover
     * it at each snprintf() from nothing but "at < scratchlen".
     */
    if (scratchlen - at < 1024)
        return false;

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

/*
 * When inject is set, exec_req() fails allocation inject_nth made by
 * s3_target_exec() itself -- not the test's own -- and records in
 * inject_fired whether there was one to fail.
 */
static bool inject;
static unsigned inject_nth;
static bool inject_fired;

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

    if (inject)
        s3_alloc_fail_arm(inject_nth);
    s3_target_exec(t, &req, dma, dma_ctx, &r->resp);
    if (inject)
        inject_fired = s3_alloc_fail_disarm();

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
            fail(name, "capacity %" PRIu64, cfg.capacity);
        if (cfg.traddr != 0x0a000005u)
            fail(name, "traddr 0x%08x", cfg.traddr);
        if (cfg.trsvcid != 8080)
            fail(name, "port %u", cfg.trsvcid);
        if (cfg.max_objects != 8)
            fail(name, "objects %u", cfg.max_objects);
    }

    /* size= takes the same multipliers as the nvmeof backend's. */
    s3_target_cfg_defaults(&cfg);
    if (!s3_target_cfg_parse(&cfg, "size=1T", err, sizeof(err)))
        fail(name, "rejected size=1T: %s", err);
    else if (cfg.capacity != 1ull << 40)
        fail(name, "size=1T gave capacity %" PRIu64, cfg.capacity);

    static const char *const bad[] = {
        "bucket=",      "bucket=No_Caps_Allowed",
        "ip=256.1.1.1", "ip=not-an-address",
        "ip=10.0.0.5x", "port=0",
        "port=99999",   "port=0x1f90",
        "size=12Q",     "size=0x400000",
        "objects=-1",   "nosuchoption=1",
        "bucket=ab",    "bucket=-abc",
        "bucket=abc-",  "bucket=.abc",
        "bucket=abc.",  "maxpart=0",
        "maxpart=4G",   "maxpart=junk",
        "objects=0",    "objects=65537",
        "size=0",       "novalue",
        "size=4M,port", /* a bad option after a good one */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s3_target_cfg_defaults(&cfg);
        err[0] = '\0';
        if (s3_target_cfg_parse(&cfg, bad[i], err, sizeof(err)))
            fail(name, "accepted \"%s\"", bad[i]);
        else if (err[0] == '\0')
            fail(name, "\"%s\" failed without saying why", bad[i]);
    }

    /* The largest maxpart is the largest 32-bit transfer. */
    s3_target_cfg_defaults(&cfg);
    if (!s3_target_cfg_parse(&cfg, "maxpart=4294967295", err, sizeof(err)))
        fail(name, "rejected maxpart=4294967295: %s", err);
    else if (cfg.max_part != UINT32_MAX)
        fail(name, "maxpart=4294967295 gave %" PRIu32, cfg.max_part);

    /* No options, as NULL or as "", leave every default alone. */
    static const char *const none[] = {NULL, ""};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        struct s3_target_cfg want;
        s3_target_cfg_defaults(&want);
        s3_target_cfg_defaults(&cfg);
        if (!s3_target_cfg_parse(&cfg, none[i], err, sizeof(err)))
            fail(name, "rejected %s options: %s", none[i] ? "empty" : "NULL",
                 err);
        else if (!cfg_equal(&cfg, &want))
            fail(name, "%s options changed the defaults",
                 none[i] ? "empty" : "NULL");
    }

    ok(name);
}

/* A configuration that leaves the object and part limits at zero, as a
 * caller that never called s3_target_cfg_defaults() would, gets the
 * defaults rather than a store that refuses everything. */
static void test_create_defaults(void)
{
    const char *name = "create-defaults";
    struct s3_target_cfg cfg;
    char err[128] = "";

    s3_target_cfg_defaults(&cfg);
    cfg.max_objects = 0;
    cfg.max_part = 0;

    struct s3_target *t = s3_target_create(&cfg, err, sizeof(err));
    if (t == NULL) {
        fail(name, "create: %s", err);
        return;
    }
    const struct s3_target_cfg *got = s3_target_config(t);
    if (got->max_objects != 256)
        fail(name, "max_objects %" PRIu32 ", want 256", got->max_objects);
    if (got->max_part != 256u << 20)
        fail(name, "max_part %" PRIu32 ", want 256 MiB", got->max_part);
    s3_target_destroy(t);

    s3_target_destroy(NULL); /* like free(), a no-op */
    ok(name);
}

/* A PUT with the payload in the HTTP body, read back the same way: the
 * control-plane-only path that makes the store reachable from curl. */
static void test_body_put_get(void)
{
    const char *name = "body-put-get";
    struct s3_target *t = make_target(NULL);
    struct reply r;
    const char *etag, *clen;

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
    etag = hdr(&r, "ETag");
    if (etag == NULL ||
        strcmp(etag, "\"30f3c93e46436deb58ba70816a8ec124\"") != 0)
        fail(name, "ETag %s, want \"30f3c93e46436deb58ba70816a8ec124\"",
             etag ? etag : "(absent)");
    reply_free(&r);

    if (s3_target_object_count(t) != 1)
        fail(name, "object count %u after one PUT", s3_target_object_count(t));
    if (s3_target_bytes_used(t) != strlen(payload))
        fail(name, "bytes used %" PRIu64, s3_target_bytes_used(t));

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
    clen = hdr(&r, "x-amz-content-length");
    uint64_t clen_val;
    if (!ernic_parse_u64(clen, &clen_val) || clen_val != strlen(payload))
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
    const size_t len = (size_t)64 * 1024;
    const uint64_t dst = UINT64_C(512) * 1024;
    const char *reply, *bytes;
    int code = 0;
    const struct s3_target_stats *st;

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

    reply = hdr(&r, "x-amz-rdma-reply");
    struct s3_token peer;
    if (reply == NULL) {
        fail(name, "no x-amz-rdma-reply on an RDMA PUT");
    } else if (!s3_reply_parse_peer(reply, strlen(reply), &peer, &code)) {
        fail(name, "reply \"%s\" does not parse", reply);
    } else {
        if (code != 200)
            fail(name, "reply code %d", code);
        if (peer.length != len)
            fail(name, "reply token length %" PRIu64 ", want %zu", peer.length,
                 len);
        if (!s3_target_is_target_qpn(peer.qp_num))
            fail(name, "reply qpn 0x%x is not in the target range",
                 peer.qp_num);
    }
    bytes = hdr(&r, "x-amz-rdma-bytes");
    uint64_t bytes_val;
    if (!ernic_parse_u64(bytes, &bytes_val) || bytes_val != len)
        fail(name, "x-amz-rdma-bytes \"%s\"", bytes ? bytes : "(absent)");
    reply_free(&r);

    if (s3_target_bytes_used(t) != len)
        fail(name, "stored %" PRIu64 " bytes, want %zu",
             s3_target_bytes_used(t), len);

    /* GET the object back into a different window of the same buffer and
     * compare, which is what a real client's verify step does. */
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

    st = s3_target_statistics(t);
    if (st->puts != 1 || st->gets != 1)
        fail(name, "stats: %" PRIu64 " puts, %" PRIu64 " gets", st->puts,
             st->gets);
    if (st->write_bytes != len || st->read_bytes != len)
        fail(name, "stats: %" PRIu64 " written, %" PRIu64 " read",
             st->write_bytes, st->read_bytes);

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
    const size_t len = (size_t)64 * 1024;

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
        fail(name, "stored %" PRIu64 " bytes, want %zu",
             s3_target_bytes_used(t), len);
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
        {"bytes=--64", 0, 0, 416},        /* a sign, which wrapped */
        {"bytes=+0-99", 0, 0, 416},
        {"bytes=0-+99", 0, 0, 416},
        {"bytes=0x0-99", 0, 0, 416},
        {"bytes=0-99x", 0, 0, 416},
        {"bytes=-", 0, 0, 416},
        {"bytes=0-18446744073709551616", 0, 0, 416}, /* past UINT64_MAX */
    };
    const uint64_t dst = UINT64_C(512) * 1024;

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
 * An object written over RDMA and read back in an HTTP body has to be
 * the same object.  Nothing else crosses the two paths: the body tests
 * write with a body, the RDMA tests read with a token, and a store that
 * kept the two in separate places would pass both.
 */
static void test_rdma_put_body_get(void)
{
    const char *name = "rdma-put-body-get";
    struct s3_target *t = make_target(NULL);
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    struct reply r;
    char put_etag[64] = "";
    const size_t len = 4096;
    const char *e;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    for (size_t i = 0; i < len; i++)
        h->buf[i] = (uint8_t)(i * 31u + 7u);

    mint(token, 0, len);
    if (!exec_req(t, &r, &host_ops, h, "PUT", "/ernic/crossed", token, NULL,
                  NULL, 0)) {
        fail(name, "PUT could not be built");
        goto out;
    }
    if (r.resp.status != 200) {
        fail(name, "PUT status %d", r.resp.status);
        reply_free(&r);
        goto out;
    }
    e = hdr(&r, "ETag");
    if (e != NULL)
        snprintf(put_etag, sizeof(put_etag), "%s", e);
    reply_free(&r);

    /* No token: the object comes back in the body. */
    if (!exec_req(t, &r, &host_ops, h, "GET", "/ernic/crossed", NULL, NULL,
                  NULL, 0)) {
        fail(name, "GET could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "GET status %d", r.resp.status);
    else if (r.resp.body_len != len)
        fail(name, "body %zu bytes, want %zu", r.resp.body_len, len);
    else if (memcmp(r.resp.body, h->buf, len) != 0)
        fail(name, "body is not the bytes the RDMA PUT delivered");
    else if (put_etag[0] == '\0')
        fail(name, "the RDMA PUT reported no ETag");
    else if (hdr(&r, "ETag") == NULL || strcmp(hdr(&r, "ETag"), put_etag) != 0)
        fail(name, "ETag %s on GET, %s on PUT",
             hdr(&r, "ETag") ? hdr(&r, "ETag") : "(absent)", put_etag);
    else
        ok(name);
    reply_free(&r);
out:
    free(h);
    s3_target_destroy(t);
}

/*
 * Ranges on the body path, with no token: a satisfiable one is a 206
 * carrying just those bytes and a Content-Range saying which, and one
 * that is not is a 416 error document with no Content-Range and no RDMA
 * reply, since nothing asked for one.
 */
static void test_body_ranged_get(void)
{
    const char *name = "body-ranged-get";
    struct s3_target *t = make_target(NULL);
    struct reply r;
    char payload[100];
    const char *cr;

    if (t == NULL)
        return;

    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (char)('A' + i % 26);

    if (!exec_req(t, &r, NULL, NULL, "PUT", "/ernic/ranged", NULL, NULL,
                  payload, sizeof(payload))) {
        fail(name, "PUT could not be built");
        goto out;
    }
    if (r.resp.status != 200) {
        fail(name, "PUT status %d, want 200", r.resp.status);
        reply_free(&r);
        goto out;
    }
    reply_free(&r);

    if (!exec_req(t, &r, NULL, NULL, "GET", "/ernic/ranged", NULL,
                  "Range: bytes=10-19", NULL, 0)) {
        fail(name, "ranged GET could not be built");
        goto out;
    }
    cr = hdr(&r, "Content-Range");
    if (r.resp.status != 206)
        fail(name, "bytes=10-19: status %d, want 206", r.resp.status);
    else if (r.resp.body_len != 10 || memcmp(r.text, payload + 10, 10) != 0)
        fail(name, "bytes=10-19: body is not bytes 10..19: \"%s\"", r.text);
    else if (cr == NULL || strcmp(cr, "bytes 10-19/100") != 0)
        fail(name, "bytes=10-19: Content-Range \"%s\"", cr ? cr : "(absent)");
    reply_free(&r);

    static const char *const bad[] = {
        "bytes=5",     /* no dash */
        "bytes=-0",    /* a suffix of no bytes */
        "bytes=100-",  /* starts at the end */
        "bytes=50-10", /* inverted */
        "items=0-1",   /* unsupported unit */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char hdrline[64];
        snprintf(hdrline, sizeof(hdrline), "Range: %s", bad[i]);
        if (!exec_req(t, &r, NULL, NULL, "GET", "/ernic/ranged", NULL, hdrline,
                      NULL, 0)) {
            fail(name, "\"%s\": could not be built", bad[i]);
            continue;
        }
        if (r.resp.status != 416)
            fail(name, "\"%s\": status %d, want 416", bad[i], r.resp.status);
        else if (strstr(r.text, "<Code>InvalidRange</Code>") == NULL)
            fail(name, "\"%s\": not an InvalidRange error: %s", bad[i], r.text);
        else if (hdr(&r, "Content-Range") != NULL)
            fail(name, "\"%s\": a 416 carries a Content-Range", bad[i]);
        else if (hdr(&r, "x-amz-rdma-reply") != NULL)
            fail(name, "\"%s\": a tokenless 416 carries an RDMA reply", bad[i]);
        reply_free(&r);
    }

    ok(name);
out:
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
    const char *reply;
    int code = 0;

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
    reply = hdr(&r, "x-amz-rdma-reply");
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
    mint(token, 0, UINT64_C(128) * 1024);
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
    if (exec_req(t, &r, NULL, NULL, "HEAD", "/", NULL, NULL, NULL, 0)) {
        if (r.resp.status != 200 || r.resp.body_len != 0)
            fail(name, "HEAD on the service: status %d, %zu body bytes",
                 r.resp.status, r.resp.body_len);
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

    static const char *const keys[] = {"a/one", "a/two", "b/three", "top"};
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

    /* The keys before b/three do not match, so listing it means passing
     * over them, not stopping at the first non-match. */
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2&prefix=b%2F",
                 NULL, NULL, NULL, 0)) {
        if (strstr(r.text, "<KeyCount>1</KeyCount>") == NULL ||
            strstr(r.text, "<Key>b/three</Key>") == NULL)
            fail(name, "prefix b/ list: %s", r.text);
        else if (strstr(r.text, "<Key>a/one</Key>") != NULL ||
                 strstr(r.text, "<Key>top</Key>") != NULL)
            fail(name, "prefix b/ list leaked a non-matching key");
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
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/a%26b%3Cc%3Ed%22e", NULL,
                 NULL, "x", 1))
        reply_free(&r);
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic?list-type=2", NULL, NULL,
                 NULL, 0)) {
        if (strstr(r.text, "<Key>a&amp;b&lt;c&gt;d&quot;e</Key>") == NULL)
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
    const size_t part = (size_t)16 * 1024;
    const unsigned nparts = 3;
    const uint64_t dst = UINT64_C(512) * 1024;

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
    if (!take_upload_id(r.text, upload_id, sizeof(upload_id), name)) {
        reply_free(&r);
        goto out;
    }
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
        fail(name, "reassembled %" PRIu64 " bytes, want %zu",
             s3_target_bytes_used(t), part * nparts);

    /* Read it back and compare against what we uploaded. */
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
        take_upload_id(r.text, upload_id, sizeof(upload_id), name);
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

/* A part the store has no room for must leave no trace: a rejected slot
 * would otherwise pad the completed object's ETag with a part that has
 * no bytes. */
static void test_multipart_part_rejected(void)
{
    const char *name = "multipart-part-rejected";
    struct s3_target *t = make_target("size=4K");
    struct reply r;
    char upload_id[64] = "";
    char target[128];
    char payload[5000];

    if (t == NULL)
        return;

    memset(payload, 'q', sizeof(payload));

    if (!exec_req(t, &r, NULL, NULL, "POST", "/ernic/p?uploads", NULL, NULL,
                  NULL, 0)) {
        fail(name, "initiate could not be built");
        goto out;
    }
    if (!take_upload_id(r.text, upload_id, sizeof(upload_id), name)) {
        reply_free(&r);
        goto out;
    }
    reply_free(&r);

    snprintf(target, sizeof(target), "/ernic/p?partNumber=1&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload, 2000)) {
        if (r.resp.status != 200)
            fail(name, "part 1 status %d, want 200", r.resp.status);
        reply_free(&r);
    }

    /* A part that does not fit, then an oversized overwrite of the part
     * that does: neither may disturb what the upload already holds. */
    snprintf(target, sizeof(target), "/ernic/p?partNumber=2&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 507)
            fail(name, "part 2 past capacity gave status %d, want 507",
                 r.resp.status);
        reply_free(&r);
    }
    snprintf(target, sizeof(target), "/ernic/p?partNumber=1&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 507)
            fail(name, "oversized overwrite gave status %d, want 507",
                 r.resp.status);
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/p?uploadId=%s", upload_id);
    if (!exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        fail(name, "complete could not be built");
        goto out;
    }
    if (r.resp.status != 200)
        fail(name, "complete status %d: %s", r.resp.status, r.text);
    else if (strstr(r.text, "-1&quot;") == NULL)
        fail(name, "a rejected part is still counted in the ETag: %s", r.text);
    reply_free(&r);

    if (s3_target_object_count(t) != 1)
        fail(name, "complete did not produce exactly one object");
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic/p", NULL, NULL, NULL, 0)) {
        if (r.resp.body_len != 2000)
            fail(name, "the completed object is %zu bytes, want 2000",
                 r.resp.body_len);
        else if (memcmp(r.text, payload, 2000) != 0)
            fail(name, "the completed object is not the part that was kept");
        reply_free(&r);
    }

    ok(name);
out:
    s3_target_destroy(t);
}

/* An uploadId longer than the target's buffer is a wrong upload id, not
 * an absent one.  Read as absent, an UploadPart falls through to a plain
 * PUT and the part overwrites the object it was meant to be part of. */
static void test_overlong_upload_id(void)
{
    const char *name = "overlong-upload-id";
    struct s3_target *t = make_target(NULL);
    struct reply r;
    char id[41];
    char target[128];

    if (t == NULL)
        return;

    memset(id, 'a', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';

    static const char payload[] = "the quick brown fox";
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/fox.txt", NULL, NULL,
                 payload, strlen(payload))) {
        if (r.resp.status != 200)
            fail(name, "seed PUT gave status %d, want 200", r.resp.status);
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/fox.txt?partNumber=1&uploadId=%s",
             id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "d", 1)) {
        if (r.resp.status != 404)
            fail(name, "a part with an over-long uploadId gave %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }

    /* The part must not have landed on the key as an object of its own. */
    if (exec_req(t, &r, NULL, NULL, "HEAD", "/ernic/fox.txt", NULL, NULL, NULL,
                 0)) {
        const char *clen = hdr(&r, "x-amz-content-length");
        uint64_t clen_val;
        if (!ernic_parse_u64(clen, &clen_val) || clen_val != strlen(payload))
            fail(name, "the object is now %s bytes, want %zu",
                 clen ? clen : "(absent)", strlen(payload));
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/fox.txt?uploadId=%s", id);
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404)
            fail(name, "completing an over-long uploadId gave %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404)
            fail(name, "aborting an over-long uploadId gave %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }
    if (s3_target_object_count(t) != 1)
        fail(name, "the abort deleted the object, %u left",
             s3_target_object_count(t));

    s3_target_destroy(t);
    ok(name);
}

/* Initiate an upload of @key on @t and lift its id into @id. */
static bool initiate(struct s3_target *t, const char *key, char *id,
                     size_t idlen, const char *name)
{
    struct reply r;
    char target[128];
    bool got;

    snprintf(target, sizeof(target), "/ernic/%s?uploads", key);
    if (!exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        fail(name, "initiate %s could not be built", key);
        return false;
    }
    got = take_upload_id(r.text, id, idlen, name);
    reply_free(&r);
    return got;
}

static bool upload_id_shape_ok(const char *id)
{
    return strlen(id) == 16 && strspn(id, "0123456789abcdef") == 16;
}

/*
 * An uploadId goes back to the client, so it must carry nothing of the
 * host's.  It once held the low half of the upload slot's heap address,
 * so two uploads in turn through the same slot shared their last eight
 * hex digits.
 */
static void test_upload_id(void)
{
    const char *name = "upload-id";
    struct s3_target *t = make_target(NULL);
    struct s3_target *t2 = NULL;
    struct reply r;
    char first[64] = "", second[64] = "", other[64] = "";
    char target[128];

    if (t == NULL)
        return;

    if (!initiate(t, "k", first, sizeof(first), name))
        goto out;
    snprintf(target, sizeof(target), "/ernic/k?uploadId=%s", first);
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 204)
            fail(name, "abort gave status %d, want 204", r.resp.status);
        reply_free(&r);
    }
    /* With the first aborted, the second takes the same slot. */
    if (!initiate(t, "k", second, sizeof(second), name))
        goto out;

    if (!upload_id_shape_ok(first) || !upload_id_shape_ok(second))
        fail(name, "ids \"%s\" and \"%s\" are not 16 lowercase hex digits",
             first, second);
    if (strcmp(first + 8, second + 8) == 0)
        fail(name, "two uploads through one slot share a suffix: %s, %s", first,
             second);

    /* A fresh target, as after a restart, must not hand out the ids the
     * old one did, or a client's stale id would name a stranger's upload
     * of the same key. */
    t2 = make_target(NULL);
    if (t2 == NULL)
        goto out;
    if (!initiate(t2, "k", other, sizeof(other), name))
        goto out;
    if (strcmp(first, other) == 0)
        fail(name, "two targets both began with upload id %s", first);

    ok(name);
out:
    s3_target_destroy(t2);
    s3_target_destroy(t);
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

/* An in-flight multipart upload's bytes are the store's bytes: they have
 * to count against capacity for every other writer, not just for the
 * upload holding them. */
static void test_multipart_capacity(void)
{
    const char *name = "multipart-capacity";
    struct s3_target *t = make_target("size=4K,objects=2");
    struct reply r;
    char payload[3000];
    char ids[2][64] = {{0}, {0}};
    char target[128];

    if (t == NULL)
        return;

    memset(payload, 'q', sizeof(payload));

    static const char *const keys[] = {"a", "b"};
    for (int i = 0; i < 2; i++) {
        snprintf(target, sizeof(target), "/ernic/%s?uploads", keys[i]);
        if (!exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
            fail(name, "initiate %s could not be built", keys[i]);
            goto out;
        }
        if (!take_upload_id(r.text, ids[i], sizeof(ids[i]), name)) {
            reply_free(&r);
            goto out;
        }
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/a?partNumber=1&uploadId=%s",
             ids[0]);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 200)
            fail(name, "part status %d, want 200", r.resp.status);
        reply_free(&r);
    }
    if (s3_target_bytes_used(t) != sizeof(payload))
        fail(name, "a part in flight leaves used at %" PRIu64 ", want %zu",
             s3_target_bytes_used(t), sizeof(payload));

    /* Those 3000 bytes are gone from the 4096 the store has, whoever asks. */
    snprintf(target, sizeof(target), "/ernic/b?partNumber=1&uploadId=%s",
             ids[1]);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 507)
            fail(name, "second part past capacity gave status %d, want 507",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/plain", NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 507)
            fail(name, "a PUT past capacity gave status %d, want 507",
                 r.resp.status);
        reply_free(&r);
    }

    /* Complete charges the assembled object once, not twice and not zero
     * times: the part bytes become the object's bytes. */
    snprintf(target, sizeof(target), "/ernic/a?uploadId=%s", ids[0]);
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 200)
            fail(name, "complete status %d: %s", r.resp.status, r.text);
        reply_free(&r);
    }
    if (s3_target_bytes_used(t) != sizeof(payload))
        fail(name, "complete left used at %" PRIu64 ", want %zu",
             s3_target_bytes_used(t), sizeof(payload));
    if (s3_target_object_count(t) != 1)
        fail(name, "complete left %u objects, want 1",
             s3_target_object_count(t));

    if (exec_req(t, &r, NULL, NULL, "DELETE", "/ernic/a", NULL, NULL, NULL, 0))
        reply_free(&r);
    if (s3_target_bytes_used(t) != 0)
        fail(name, "delete left used at %" PRIu64 ", want 0",
             s3_target_bytes_used(t));

    /* An abandoned upload gives its bytes back when it is aborted. */
    snprintf(target, sizeof(target), "/ernic/b?partNumber=1&uploadId=%s",
             ids[1]);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 200)
            fail(name, "part on the second upload status %d, want 200",
                 r.resp.status);
        reply_free(&r);
    }
    snprintf(target, sizeof(target), "/ernic/b?uploadId=%s", ids[1]);
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 204)
            fail(name, "abort gave status %d, want 204", r.resp.status);
        reply_free(&r);
    }
    if (s3_target_bytes_used(t) != 0)
        fail(name, "abort left used at %" PRIu64 ", want 0",
             s3_target_bytes_used(t));
    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/plain", NULL, NULL, payload,
                 sizeof(payload))) {
        if (r.resp.status != 200)
            fail(name, "a PUT into reclaimed space gave status %d, want 200",
                 r.resp.status);
        reply_free(&r);
    }

    ok(name);
out:
    s3_target_destroy(t);
}

/*
 * Check an RDMA request that failed: the HTTP status, the S3 error code,
 * and an x-amz-rdma-reply carrying the same status and the @bytes that
 * moved, which is all a client blocked on its token ever reads.
 */
static void expect_rdma_failure(const char *name, const char *what,
                                const struct reply *r, int status,
                                const char *code, uint64_t bytes)
{
    char want[64];
    struct s3_token peer;
    int reply_code = 0;
    uint64_t bytes_val;

    snprintf(want, sizeof(want), "<Code>%s</Code>", code);
    if (r->resp.status != status) {
        fail(name, "%s: status %d, want %d: %s", what, r->resp.status, status,
             r->text);
        return;
    }
    if (strstr(r->text, want) == NULL)
        fail(name, "%s: not a %s error: %s", what, code, r->text);

    const char *reply = hdr(r, "x-amz-rdma-reply");
    if (reply == NULL)
        fail(name, "%s: no x-amz-rdma-reply", what);
    else if (!s3_reply_parse_peer(reply, strlen(reply), &peer, &reply_code))
        fail(name, "%s: reply \"%s\" does not parse", what, reply);
    else if (reply_code != status)
        fail(name, "%s: reply code %d, want %d", what, reply_code, status);

    const char *moved = hdr(r, "x-amz-rdma-bytes");
    if (!ernic_parse_u64(moved, &bytes_val) || bytes_val != bytes)
        fail(name, "%s: x-amz-rdma-bytes \"%s\", want %" PRIu64, what,
             moved ? moved : "(absent)", bytes);
}

/*
 * Once an RDMA PUT's token has parsed, every failure has to reach the
 * reply header, because a client blocked on its token learns of it from
 * nowhere else.  That includes the failures that have nothing to do with
 * RDMA: a bad part number, an unknown upload, a full store.
 */
static void test_rdma_put_failures(void)
{
    const char *name = "rdma-put-failures";
    struct s3_target *t = make_target("size=4K");
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    char upload_id[64] = "";
    char target[128];
    struct reply r;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    /* A data plane that can write to the client but not read from it. */
    static const struct s3_dma_ops write_only = {
        .from_host = NULL,
        .to_host = host_to,
    };
    mint(token, 0, 1024);
    if (exec_req(t, &r, &write_only, h, "PUT", "/ernic/k", token, NULL, NULL,
                 0)) {
        expect_rdma_failure(name, "no from_host", &r, 501, "NotImplemented", 0);
        reply_free(&r);
    }

    /* Content-Length names more than the token's buffer holds. */
    if (exec_req(t, &r, &host_ops, h, "PUT", "/ernic/k", token,
                 "Content-Length: 1025", NULL, 0)) {
        expect_rdma_failure(name, "length past the token", &r, 400,
                            "EntityTooLarge", 0);
        reply_free(&r);
    }

    if (!initiate(t, "mp", upload_id, sizeof(upload_id), name))
        goto out;

    snprintf(target, sizeof(target), "/ernic/mp?partNumber=0&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, &host_ops, h, "PUT", target, token, NULL, NULL, 0)) {
        expect_rdma_failure(name, "partNumber=0", &r, 400, "InvalidPart", 0);
        reply_free(&r);
    }

    if (exec_req(t, &r, &host_ops, h, "PUT",
                 "/ernic/mp?partNumber=1&uploadId=no", token, NULL, NULL, 0)) {
        expect_rdma_failure(name, "unknown upload", &r, 404, "NoSuchUpload", 0);
        reply_free(&r);
    }

    /* 5000 bytes do not fit a 4K store, as a part or as an object. */
    mint(token, 0, 5000);
    snprintf(target, sizeof(target), "/ernic/mp?partNumber=1&uploadId=%s",
             upload_id);
    if (exec_req(t, &r, &host_ops, h, "PUT", target, token, NULL, NULL, 0)) {
        expect_rdma_failure(name, "part past capacity", &r, 507,
                            "EntityTooLarge", 0);
        reply_free(&r);
    }
    if (exec_req(t, &r, &host_ops, h, "PUT", "/ernic/big", token, NULL, NULL,
                 0)) {
        expect_rdma_failure(name, "object past capacity", &r, 507,
                            "EntityTooLarge", 0);
        reply_free(&r);
    }

    if (s3_target_object_count(t) != 0 || s3_target_bytes_used(t) != 0)
        fail(name, "failed PUTs left %u objects and %" PRIu64 " bytes",
             s3_target_object_count(t), s3_target_bytes_used(t));

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

/* The x-amz-rdma-bytes of @r, or UINT64_MAX if it has none that parses. */
static uint64_t rdma_bytes(const struct reply *r)
{
    uint64_t v;

    if (!ernic_parse_u64(hdr(r, "x-amz-rdma-bytes"), &v))
        return UINT64_MAX;
    return v;
}

/*
 * The RDMA GET paths other than a whole object into a big enough buffer.
 * A buffer smaller than the object is a short read, not an error, and
 * must not be written past its end; every failure after the token has
 * parsed reaches the reply header, saying how many bytes did move.
 */
static void test_rdma_get_paths(void)
{
    const char *name = "rdma-get-paths";
    struct s3_target *t = make_target("maxpart=4K");
    struct host_mem *h = calloc(1, sizeof(*h));
    char token[S3_TOKEN_HEX_LEN + 1];
    char big[8192];
    char small[100];
    const struct {
        const char *target;
        const char *data;
        size_t len;
    } seed[] = {
        {"/ernic/big", big, sizeof(big)},
        {"/ernic/small", small, sizeof(small)},
        {"/ernic/empty", NULL, 0},
    };
    struct reply r;

    if (t == NULL || h == NULL) {
        fail(name, "setup failed");
        goto out;
    }

    memset(big, 'B', sizeof(big));
    for (size_t i = 0; i < sizeof(small); i++)
        small[i] = (char)('a' + i % 26);
    for (size_t i = 0; i < sizeof(seed) / sizeof(seed[0]); i++) {
        if (!exec_req(t, &r, NULL, NULL, "PUT", seed[i].target, NULL, NULL,
                      seed[i].data, seed[i].len)) {
            fail(name, "PUT %s could not be built", seed[i].target);
            goto out;
        }
        if (r.resp.status != 200)
            fail(name, "PUT %s status %d", seed[i].target, r.resp.status);
        reply_free(&r);
    }

    /* A token that does not parse has no buffer to reply about. */
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/small", "not-a-token",
                 NULL, NULL, 0)) {
        if (r.resp.status != 400 ||
            strstr(r.text, "<Code>InvalidArgument</Code>") == NULL)
            fail(name, "malformed token: status %d: %s", r.resp.status, r.text);
        else if (hdr(&r, "x-amz-rdma-reply") != NULL)
            fail(name, "malformed token: a reply about no buffer");
        reply_free(&r);
    }

    /* A data plane that can read from the client but not write to it. */
    static const struct s3_dma_ops read_only = {
        .from_host = host_from,
        .to_host = NULL,
    };
    mint(token, 0, sizeof(small));
    if (exec_req(t, &r, &read_only, h, "GET", "/ernic/small", token, NULL, NULL,
                 0)) {
        expect_rdma_failure(name, "no to_host", &r, 501, "NotImplemented", 0);
        reply_free(&r);
    }

    mint(token, 0, sizeof(big));
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/big", token, NULL, NULL,
                 0)) {
        expect_rdma_failure(name, "past maxpart", &r, 400, "EntityTooLarge", 0);
        reply_free(&r);
    }

    h->short_by = 16;
    mint(token, 0, sizeof(small));
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/small", token, NULL, NULL,
                 0)) {
        expect_rdma_failure(name, "short write", &r, 500, "InternalError",
                            sizeof(small) - 16);
        reply_free(&r);
    }
    h->short_by = 0;

    /* A 40-byte buffer gets the first 40 bytes and nothing past them. */
    memset(h->buf, 0xee, 64);
    mint(token, 0, 40);
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/small", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 200)
            fail(name, "small buffer: status %d: %s", r.resp.status, r.text);
        else if (rdma_bytes(&r) != 40)
            fail(name, "small buffer: x-amz-rdma-bytes %" PRIu64 ", want 40",
                 rdma_bytes(&r));
        else if (memcmp(h->buf, small, 40) != 0)
            fail(name, "small buffer: not the object's first 40 bytes");
        else if (h->buf[40] != 0xee)
            fail(name, "small buffer: written past its end");
        reply_free(&r);
    }

    /* A token of length zero names no buffer size, so the whole object
     * is sent. */
    memset(h->buf, 0xee, 128);
    mint(token, 0, 0);
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/small", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 200 || rdma_bytes(&r) != sizeof(small))
            fail(name, "lengthless token: status %d, %" PRIu64 " bytes",
                 r.resp.status, rdma_bytes(&r));
        else if (memcmp(h->buf, small, sizeof(small)) != 0)
            fail(name, "lengthless token: not the object");
        reply_free(&r);
    }

    /* An empty object moves nothing and is still a success. */
    memset(h->buf, 0xee, 64);
    mint(token, 0, 64);
    if (exec_req(t, &r, &host_ops, h, "GET", "/ernic/empty", token, NULL, NULL,
                 0)) {
        if (r.resp.status != 200 || rdma_bytes(&r) != 0)
            fail(name, "empty object: status %d, %" PRIu64 " bytes",
                 r.resp.status, rdma_bytes(&r));
        else if (h->buf[0] != 0xee)
            fail(name, "empty object: the client buffer was written");
        reply_free(&r);
    }

    ok(name);
out:
    free(h);
    s3_target_destroy(t);
}

/*
 * The upload table has 16 slots (S3_MAX_UPLOADS).  An initiate with none
 * free is a 503 SlowDown, whose status line has to name the status, and
 * aborting an upload frees its slot.
 */
static void test_upload_table_full(void)
{
    const char *name = "upload-table-full";
    struct s3_target *t = make_target(NULL);
    char id[64] = "";
    char target[128];
    struct reply r;
    size_t len = 0;
    uint8_t *wire;

    if (t == NULL)
        return;

    for (unsigned i = 0; i < 16; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%u", i);
        if (!initiate(t, key, id, sizeof(id), name))
            goto out;
    }

    if (!exec_req(t, &r, NULL, NULL, "POST", "/ernic/extra?uploads", NULL, NULL,
                  NULL, 0)) {
        fail(name, "the 17th initiate could not be built");
        goto out;
    }
    if (r.resp.status != 503 || strstr(r.text, "<Code>SlowDown</Code>") == NULL)
        fail(name, "the 17th initiate: status %d: %s", r.resp.status, r.text);

    wire = s3_http_response_serialize(&r.resp, &len);
    static const char line[] = "HTTP/1.1 503 Service Unavailable\r\n";
    if (wire == NULL)
        fail(name, "the 503 did not serialise");
    else if (len < strlen(line) || memcmp(wire, line, strlen(line)) != 0)
        fail(name, "the 503's status line is \"%.40s\"", (const char *)wire);
    free(wire);
    reply_free(&r);

    /* Abort the last upload; its slot takes the next initiate. */
    snprintf(target, sizeof(target), "/ernic/k15?uploadId=%s", id);
    if (exec_req(t, &r, NULL, NULL, "DELETE", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 204)
            fail(name, "abort gave status %d, want 204", r.resp.status);
        reply_free(&r);
    }
    if (!initiate(t, "extra", id, sizeof(id), name))
        goto out;

    ok(name);
out:
    s3_target_destroy(t);
}

/*
 * Completing an upload with no object slot free is a 507 that leaves the
 * upload as it was: its parts still count against capacity, and it can
 * be completed once a slot is.
 */
static void test_complete_no_slot(void)
{
    const char *name = "complete-no-slot";
    struct s3_target *t = make_target("objects=1");
    char id[64] = "";
    char target[128];
    struct reply r;

    if (t == NULL)
        return;

    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/x", NULL, NULL, "x", 1))
        reply_free(&r);
    if (!initiate(t, "y", id, sizeof(id), name))
        goto out;
    snprintf(target, sizeof(target), "/ernic/y?partNumber=1&uploadId=%s", id);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "hello", 5)) {
        if (r.resp.status != 200)
            fail(name, "part status %d, want 200", r.resp.status);
        reply_free(&r);
    }

    snprintf(target, sizeof(target), "/ernic/y?uploadId=%s", id);
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 507 ||
            strstr(r.text, "<Code>EntityTooLarge</Code>") == NULL)
            fail(name, "complete with no slot: status %d: %s", r.resp.status,
                 r.text);
        reply_free(&r);
    }
    if (s3_target_bytes_used(t) != 6)
        fail(name, "a failed complete left used at %" PRIu64 ", want 6",
             s3_target_bytes_used(t));

    if (exec_req(t, &r, NULL, NULL, "DELETE", "/ernic/x", NULL, NULL, NULL, 0))
        reply_free(&r);
    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 200)
            fail(name, "complete once a slot is free: status %d: %s",
                 r.resp.status, r.text);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "GET", "/ernic/y", NULL, NULL, NULL, 0)) {
        if (r.resp.body_len != 5 || memcmp(r.text, "hello", 5) != 0)
            fail(name, "the completed object is \"%s\"", r.text);
        reply_free(&r);
    }
    if (s3_target_bytes_used(t) != 5)
        fail(name, "used %" PRIu64 " after complete, want 5",
             s3_target_bytes_used(t));

    ok(name);
out:
    s3_target_destroy(t);
}

/*
 * A bucket name too long to be configured names no bucket.  One that only
 * began with the configured name used to be cut down to it and served.
 */
static void test_bucket_too_long(void)
{
    const char *name = "bucket-too-long";
    char bucket[S3_BUCKET_MAX];
    char opts[S3_BUCKET_MAX + 16];
    char target[S3_BUCKET_MAX + 32];
    struct reply r;

    memset(bucket, 'b', S3_BUCKET_MAX - 1);
    bucket[S3_BUCKET_MAX - 1] = '\0';
    snprintf(opts, sizeof(opts), "bucket=%s", bucket);

    struct s3_target *t = make_target(opts);
    if (t == NULL)
        return;

    /* One character more, as an object and as a bucket request. */
    snprintf(target, sizeof(target), "/%sX/key", bucket);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1)) {
        if (r.resp.status != 404 ||
            strstr(r.text, "<Code>NoSuchBucket</Code>") == NULL)
            fail(name, "PUT into a longer bucket: status %d: %s", r.resp.status,
                 r.text);
        reply_free(&r);
    }
    snprintf(target, sizeof(target), "/%sX", bucket);
    if (exec_req(t, &r, NULL, NULL, "GET", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404)
            fail(name, "listing a longer bucket: status %d, want 404",
                 r.resp.status);
        reply_free(&r);
    }
    if (exec_req(t, &r, NULL, NULL, "HEAD", target, NULL, NULL, NULL, 0)) {
        if (r.resp.status != 404 || r.resp.body_len != 0)
            fail(name, "HEAD of a longer bucket: status %d, %zu body bytes",
                 r.resp.status, r.resp.body_len);
        reply_free(&r);
    }
    if (s3_target_object_count(t) != 0)
        fail(name, "a PUT into a longer bucket was stored");

    /* The configured name itself, at the longest a name can be, works. */
    snprintf(target, sizeof(target), "/%s/key", bucket);
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1)) {
        if (r.resp.status != 200)
            fail(name, "PUT into the bucket: status %d, want 200",
                 r.resp.status);
        reply_free(&r);
    }
    if (s3_target_object_count(t) != 1)
        fail(name, "a PUT into the bucket was not stored");

    s3_target_destroy(t);
    ok(name);
}

/*
 * A HEAD that fails answers with the status a GET would, but with no
 * body: a HEAD response never has one, not even an error document.  A
 * client reads none, so on a kept-alive connection the bytes of one
 * would be taken for the next response.  The paths that refuse a request
 * before it reaches the object handlers are the ones most easily missed.
 */
static void test_head_errors(void)
{
    const char *name = "head-errors";
    struct s3_target *t = make_target(NULL);
    char long_key[S3_KEY_MAX + 16];
    char long_bucket[S3_BUCKET_MAX + 16];
    struct reply r;

    if (t == NULL)
        return;

    memcpy(long_key, "/ernic/", 7);
    memset(long_key + 7, 'k', S3_KEY_MAX);
    long_key[7 + S3_KEY_MAX] = '\0';
    long_bucket[0] = '/';
    memset(long_bucket + 1, 'b', S3_BUCKET_MAX);
    long_bucket[1 + S3_BUCKET_MAX] = '\0';

    const struct {
        const char *what;
        const char *target;
        int status;
    } cases[] = {
        {"missing key", "/ernic/absent", 404},
        {"missing bucket", "/nosuch/key", 404},
        {"over-long bucket", long_bucket, 404},
        {"over-long key", long_key, 400},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!exec_req(t, &r, NULL, NULL, "HEAD", cases[i].target, NULL, NULL,
                      NULL, 0)) {
            fail(name, "%s: HEAD could not be built", cases[i].what);
            continue;
        }
        if (r.resp.status != cases[i].status)
            fail(name, "%s: status %d, want %d", cases[i].what, r.resp.status,
                 cases[i].status);
        else if (r.resp.body_len != 0)
            fail(name, "%s: a HEAD carried a %zu byte body", cases[i].what,
                 r.resp.body_len);
        reply_free(&r);
    }

    /* Nor is a missing key given an object's metadata. */
    if (exec_req(t, &r, NULL, NULL, "HEAD", "/ernic/absent", NULL, NULL, NULL,
                 0)) {
        if (hdr(&r, "ETag") != NULL || hdr(&r, "x-amz-content-length") != NULL)
            fail(name, "a missing key was given object metadata");
        reply_free(&r);
    }
    if (s3_target_statistics(t)->errors != 5)
        fail(name, "errors counter %" PRIu64 ", want 5",
             s3_target_statistics(t)->errors);

    s3_target_destroy(t);
    ok(name);
}

/* A key must fit the store's key buffer with its NUL: the longest one
 * that does is stored, and one byte more is refused, not cut short. */
static void test_key_too_long(void)
{
    const char *name = "key-too-long";
    struct s3_target *t = make_target(NULL);
    struct reply r;
    char target[S3_KEY_MAX + 16];

    if (t == NULL)
        return;

    memcpy(target, "/ernic/", 7);
    memset(target + 7, 'k', S3_KEY_MAX);
    target[7 + S3_KEY_MAX] = '\0';

    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1)) {
        if (r.resp.status != 400)
            fail(name, "a %d byte key gave status %d, want 400", S3_KEY_MAX,
                 r.resp.status);
        else if (strstr(r.text, "<Code>KeyTooLongError</Code>") == NULL)
            fail(name, "not a KeyTooLongError: %s", r.text);
        reply_free(&r);
    } else {
        fail(name, "PUT of a long key could not be built");
    }
    if (s3_target_object_count(t) != 0)
        fail(name, "a key that is too long was stored");

    target[7 + S3_KEY_MAX - 1] = '\0';
    if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL, "x", 1)) {
        if (r.resp.status != 200)
            fail(name, "a %d byte key gave status %d, want 200", S3_KEY_MAX - 1,
                 r.resp.status);
        reply_free(&r);
    } else {
        fail(name, "PUT of the longest key could not be built");
    }
    if (s3_target_object_count(t) != 1)
        fail(name, "the longest key was not stored");

    s3_target_destroy(t);
    ok(name);
}

/* Each level of the path takes only its own methods, and a refused one
 * must not act: a DELETE of the bucket deletes nothing. */
static void test_method_not_allowed(void)
{
    const char *name = "method-not-allowed";
    struct s3_target *t = make_target(NULL);
    struct reply r;

    if (t == NULL)
        return;

    if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/keep", NULL, NULL, "x", 1))
        reply_free(&r);

    static const struct {
        const char *method;
        const char *target;
    } cases[] = {
        {"PUT", "/"},
        {"POST", "/"},
        {"PUT", "/ernic"},
        {"DELETE", "/ernic"},
        {"POST", "/ernic"},
        {"PATCH", "/ernic"},
        {"OPTIONS", "/ernic/keep"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!exec_req(t, &r, NULL, NULL, cases[i].method, cases[i].target, NULL,
                      NULL, NULL, 0)) {
            fail(name, "%s %s could not be built", cases[i].method,
                 cases[i].target);
            continue;
        }
        if (r.resp.status != 405)
            fail(name, "%s %s: status %d, want 405", cases[i].method,
                 cases[i].target, r.resp.status);
        else if (strstr(r.text, "<Code>MethodNotAllowed</Code>") == NULL)
            fail(name, "%s %s: not a MethodNotAllowed error: %s",
                 cases[i].method, cases[i].target, r.text);
        reply_free(&r);
    }

    if (s3_target_object_count(t) != 1)
        fail(name, "a refused method changed the store: %u objects",
             s3_target_object_count(t));

    s3_target_destroy(t);
    ok(name);
}

/*
 * Setting up a store with an allocation failing: the copy of the option
 * string, then each allocation s3_target_create() makes in turn.  Each is
 * reported, and leaves nothing behind.  Without entropy for the upload
 * ids the store still starts, numbering its uploads from 1.
 */
static void test_setup_alloc_failures(void)
{
    const char *name = "setup-alloc-failures";
    struct s3_target_cfg cfg;
    char err[128] = "";
    unsigned nth;

    s3_target_cfg_defaults(&cfg);
    s3_alloc_fail_arm(0);
    bool parsed = s3_target_cfg_parse(&cfg, "size=4M", err, sizeof(err));
    if (!s3_alloc_fail_disarm())
        fail(name, "cfg parse made no allocation to fail");
    else if (parsed || strstr(err, "out of memory") == NULL)
        fail(name, "cfg parse with no memory: parsed %d, \"%s\"", parsed, err);

    s3_target_cfg_defaults(&cfg);
    for (nth = 0; nth < 16; nth++) {
        err[0] = '\0';
        s3_alloc_fail_arm(nth);
        struct s3_target *t = s3_target_create(&cfg, err, sizeof(err));
        bool fired = s3_alloc_fail_disarm();
        if (!fired) {
            if (t == NULL)
                fail(name, "create failed with nothing failed: %s", err);
            s3_target_destroy(t);
            break;
        }
        if (t != NULL || strstr(err, "out of memory") == NULL)
            fail(name, "create with allocation %u failed: %p, \"%s\"", nth,
                 (void *)t, err);
        s3_target_destroy(t);
    }
    if (nth < 2)
        fail(name, "only %u create allocations could be failed, want 2", nth);

    char id[64] = "";
    s3_alloc_fail_entropy(true);
    struct s3_target *t = make_target(NULL);
    s3_alloc_fail_entropy(false);
    if (t == NULL)
        return;
    if (initiate(t, "k", id, sizeof(id), name) &&
        strcmp(id, "0000000000000001") != 0)
        fail(name,
             "with no entropy the first upload is %s, want "
             "0000000000000001",
             id);
    s3_target_destroy(t);

    ok(name);
}

enum inject_kind {
    INJECT_STORES,    /* a PUT: stored only if it reports success */
    INJECT_COMPLETES, /* a complete: the upload survives a failure */
    INJECT_READS,     /* changes nothing; only the response can fail */
};

/*
 * Each request, run with each allocation it makes failed in turn, on a
 * fresh store holding one 3000-byte object.  A failure is either reported
 * as a 500 with the store unchanged, or -- when it is the response that
 * could not be built -- leaves resp.oom set for the endpoint to act on.
 * What the store holds always agrees with the status.
 */
static void test_request_alloc_failures(void)
{
    const char *name = "request-alloc-failures";
    struct host_mem *h = calloc(1, sizeof(*h));
    char seed[3000];
    char token[S3_TOKEN_HEX_LEN + 1];

    static const struct {
        const char *what;
        enum inject_kind kind;
        const char *method;
        const char *target; /* NULL: complete the upload set up for it */
        bool rdma;
        size_t body_len;
    } cases[] = {
        {"body PUT", INJECT_STORES, "PUT", "/ernic/k", false, 100},
        {"RDMA PUT", INJECT_STORES, "PUT", "/ernic/k", true, 0},
        {"complete", INJECT_COMPLETES, "POST", NULL, false, 0},
        {"body GET", INJECT_READS, "GET", "/ernic/seed", false, 0},
        {"list", INJECT_READS, "GET", "/ernic?list-type=2", false, 0},
        {"missing key", INJECT_READS, "GET", "/ernic/absent", false, 0},
    };

    if (h == NULL) {
        fail(name, "out of memory");
        return;
    }
    memset(seed, 's', sizeof(seed));
    mint(token, 0, 4096);

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        unsigned nth;
        /* Fail allocation 0, then 1, and so on, until a run fails none:
         * that run is the request with every allocation it makes met. */
        for (nth = 0; nth < 64; nth++) {
            struct s3_target *t = make_target(NULL);
            char id[64] = "";
            char target[128];
            struct reply r;

            if (t == NULL)
                goto out;
            /* Set up with nothing armed: only the request under test may
             * have an allocation fail. */
            if (exec_req(t, &r, NULL, NULL, "PUT", "/ernic/seed", NULL, NULL,
                         seed, sizeof(seed)))
                reply_free(&r);
            snprintf(target, sizeof(target), "%s",
                     cases[c].target ? cases[c].target : "");
            if (cases[c].kind == INJECT_COMPLETES) {
                /* An upload of one 5-byte part, for the complete. */
                if (!initiate(t, "mp", id, sizeof(id), name)) {
                    s3_target_destroy(t);
                    goto out;
                }
                snprintf(target, sizeof(target),
                         "/ernic/mp?partNumber=1&uploadId=%s", id);
                if (exec_req(t, &r, NULL, NULL, "PUT", target, NULL, NULL,
                             "hello", 5))
                    reply_free(&r);
                snprintf(target, sizeof(target), "/ernic/mp?uploadId=%s", id);
            }

            /* exec_req() arms the failure around s3_target_exec() alone, so
             * its own allocation of the reply text is never the one. */
            inject = true;
            inject_nth = nth;
            bool built = exec_req(
                t, &r, cases[c].rdma ? &host_ops : NULL, h, cases[c].method,
                target, cases[c].rdma ? token : NULL, NULL,
                cases[c].body_len ? seed : NULL, cases[c].body_len);
            inject = false;
            if (!built) {
                fail(name, "%s: could not be built", cases[c].what);
                s3_target_destroy(t);
                goto out;
            }

            /* Read what the checks need, then free the reply: a check
             * below may issue another request with r. */
            int status = r.resp.status;
            bool oom = r.resp.oom;
            bool fired = inject_fired;
            reply_free(&r);

            switch (cases[c].kind) {
            case INJECT_STORES: {
                /* The seed object, and the PUT's if it was stored. */
                bool stored = s3_target_object_count(t) == 2;
                if (status == 200 && (!stored || (fired && !oom)))
                    fail(name, "%s, allocation %u: 200 with stored %d, oom %d",
                         cases[c].what, nth, stored, oom);
                if (status != 200 && (status != 500 || stored ||
                                      s3_target_bytes_used(t) != 3000))
                    fail(name, "%s, allocation %u: status %d, stored %d",
                         cases[c].what, nth, status, stored);
                break;
            }
            case INJECT_COMPLETES: {
                /* The seed object, and the assembled one if completed. */
                bool stored = s3_target_object_count(t) == 2;
                if (status == 200 && (!stored || (fired && !oom)))
                    fail(name, "%s, allocation %u: 200 with stored %d, oom %d",
                         cases[c].what, nth, stored, oom);
                if (status != 200) {
                    /* The seed's 3000 bytes and the part's 5, still held. */
                    if (status != 500 || stored ||
                        s3_target_bytes_used(t) != 3005)
                        fail(name, "%s, allocation %u: status %d, stored %d",
                             cases[c].what, nth, status, stored);
                    /* The upload is intact, so completing again works. */
                    if (exec_req(t, &r, NULL, NULL, "POST", target, NULL, NULL,
                                 NULL, 0)) {
                        if (r.resp.status != 200)
                            fail(name, "%s, allocation %u: retry gave %d",
                                 cases[c].what, nth, r.resp.status);
                        reply_free(&r);
                    }
                }
                break;
            }
            case INJECT_READS:
                if (fired && !oom)
                    fail(name, "%s, allocation %u failed unnoticed",
                         cases[c].what, nth);
                if (s3_target_object_count(t) != 1)
                    fail(name, "%s changed the store", cases[c].what);
                break;
            default:
                break;
            }
            s3_target_destroy(t);
            if (!fired)
                break;
        }
        if (nth == 0)
            fail(name, "%s: no allocation could be failed", cases[c].what);
    }

    ok(name);
out:
    free(h);
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
        fail(name, "addr 0x%" PRIx64, peer.remote_addr);
    if (peer.length != 4096)
        fail(name, "length %" PRIu64, peer.length);
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
    test_create_defaults();
    test_body_put_get();
    test_rdma_put_get();
    test_rdma_put_with_content_length();
    test_rdma_ranged_get();
    test_rdma_put_body_get();
    test_body_ranged_get();
    test_range_on_empty_object();
    test_rdma_short_transfer();
    test_rdma_errors();
    test_rdma_put_failures();
    test_rdma_get_paths();
    test_upload_table_full();
    test_complete_no_slot();
    test_bucket_too_long();
    test_head_errors();
    test_key_too_long();
    test_method_not_allowed();
    test_routing();
    test_list_objects();
    test_multipart();
    test_multipart_errors();
    test_multipart_part_rejected();
    test_overlong_upload_id();
    test_upload_id();
    test_capacity();
    test_multipart_capacity();
    test_peer_token();
    test_setup_alloc_failures();
    test_request_alloc_failures();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 target tests passed\n");
    return failures;
}
