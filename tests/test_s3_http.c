/*
 * test_s3_http.c — unit tests for the S3 control plane's HTTP subset
 *
 * The parser is fed the byte-for-byte requests an S3 client emits, one
 * character at a time where the point is incremental arrival, so the
 * "incomplete, read more" path is exercised at every boundary rather
 * than assumed. The response builder is checked by serialising and
 * reading the result back, because Content-Length and Connection are
 * written by the serialiser and not by the caller.
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

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "s3_alloc_fail.h"
#include "s3_http.h"

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

static void test_parse_get(void)
{
    const char *name = "parse-get";
    static const char req_text[] = "GET /ernic/some/object.bin HTTP/1.1\r\n"
                                   "Host: 192.168.200.1:9000\r\n"
                                   "X-Amz-Rdma-Token: abcdef\r\n"
                                   "Range: bytes=0-4095\r\n"
                                   "\r\n";
    struct s3_http_request req;

    ssize_t n = s3_http_parse(req_text, strlen(req_text), &req);
    if (n != (ssize_t)strlen(req_text)) {
        fail(name, "consumed %zd of %zu bytes", n, strlen(req_text));
        return;
    }
    if (strcmp(req.method, "GET") != 0)
        fail(name, "method \"%s\"", req.method);
    if (strcmp(req.path, "/ernic/some/object.bin") != 0)
        fail(name, "path \"%s\"", req.path);
    if (req.query[0] != '\0')
        fail(name, "query \"%s\", want empty", req.query);
    if (!req.keep_alive)
        fail(name, "HTTP/1.1 with no Connection header is not keep-alive");

    /* Header lookup is case-insensitive in both directions. */
    const char *tok = s3_http_header_get(&req, "x-amz-rdma-token");
    if (tok == NULL || strcmp(tok, "abcdef") != 0)
        fail(name, "token header \"%s\"", tok ? tok : "(absent)");
    if (s3_http_header_get(&req, "RANGE") == NULL)
        fail(name, "uppercase lookup missed a header");
    if (s3_http_header_get(&req, "content-md5") != NULL)
        fail(name, "found a header that is not there");

    ok(name);
}

static void test_parse_incremental(void)
{
    const char *name = "parse-incremental";
    static const char req_text[] = "PUT /ernic/k HTTP/1.1\r\n"
                                   "Content-Length: 5\r\n"
                                   "\r\n"
                                   "hello";
    size_t total = strlen(req_text);
    struct s3_http_request req;

    /* Every prefix short of the whole thing must ask for more bytes. */
    for (size_t i = 0; i < total; i++) {
        ssize_t n = s3_http_parse(req_text, i, &req);
        if (n != 0) {
            fail(name, "prefix of %zu bytes returned %zd, want 0", i, n);
            return;
        }
    }

    ssize_t n = s3_http_parse(req_text, total, &req);
    if (n != (ssize_t)total) {
        fail(name, "complete request consumed %zd of %zu", n, total);
        return;
    }
    if (req.content_length != 5)
        fail(name, "content_length %zu, want 5", req.content_length);
    if (req.body_len != 5 || req.body == NULL ||
        memcmp(req.body, "hello", 5) != 0)
        fail(name, "body not delivered");

    ok(name);
}

/*
 * An RDMA PUT declares the object's length in Content-Length and then
 * writes nothing on the socket, because the bytes ride the fabric.  If
 * framing waits for them the request is never answered and the
 * connection wedges, so the rule is pinned here.
 */
static void test_parse_rdma_no_body(void)
{
    const char *name = "parse-rdma-no-body";
    static const char req_text[] = "PUT /ernic/k HTTP/1.1\r\n"
                                   "Content-Length: 1048576\r\n"
                                   "x-amz-rdma-token: 00deadbeef\r\n"
                                   "\r\n";
    size_t total = strlen(req_text);
    struct s3_http_request req;

    ssize_t n = s3_http_parse(req_text, total, &req);
    if (n != (ssize_t)total) {
        fail(name, "consumed %zd of %zu, want the whole request", n, total);
        return;
    }
    if (!req.rdma)
        fail(name, "rdma flag not set");
    else if (req.content_length != 1048576)
        fail(name, "content_length %zu, want 1048576", req.content_length);
    else if (req.body_len != 0 || req.body != NULL)
        fail(name, "body_len %zu, want no body on the socket", req.body_len);
    else
        ok(name);
}

/* Without a token the same request must still wait for its body. */
static void test_parse_body_still_framed(void)
{
    const char *name = "parse-body-still-framed";
    static const char hdrs[] = "PUT /ernic/k HTTP/1.1\r\n"
                               "Content-Length: 5\r\n"
                               "\r\n";
    struct s3_http_request req;

    ssize_t n = s3_http_parse(hdrs, strlen(hdrs), &req);
    if (n != 0)
        fail(name, "returned %zd with no body yet, want 0", n);
    else if (req.rdma)
        fail(name, "rdma flag set without a token");
    else
        ok(name);
}

static void test_parse_pipelined(void)
{
    const char *name = "parse-pipelined";
    static const char stream[] = "DELETE /ernic/a HTTP/1.1\r\n\r\n"
                                 "GET /ernic/b HTTP/1.1\r\n\r\n";
    struct s3_http_request req;

    ssize_t n = s3_http_parse(stream, strlen(stream), &req);
    if (n <= 0 || strcmp(req.method, "DELETE") != 0) {
        fail(name, "first request: n=%zd method=%s", n, req.method);
        return;
    }

    ssize_t m = s3_http_parse(stream + n, strlen(stream) - (size_t)n, &req);
    if (m <= 0 || strcmp(req.method, "GET") != 0 ||
        strcmp(req.path, "/ernic/b") != 0)
        fail(name, "second request: n=%zd method=%s path=%s", m, req.method,
             req.path);
    else if ((size_t)(n + m) != strlen(stream))
        fail(name, "consumed %zd of %zu bytes total", n + m, strlen(stream));
    else
        ok(name);
}

static void test_parse_errors(void)
{
    const char *name = "parse-errors";
    struct s3_http_request req;

    static const char *bad[] = {
        "GET\r\n\r\n",                        /* no version */
        "GET /x\r\n\r\n",                     /* no version */
        "GET /x HTTP/1.1\r\nnocolon\r\n\r\n", /* header without a colon */
        "PUT /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "PUT /x HTTP/1.1\r\nContent-Length: 5x\r\n\r\n",
        "PUT /x HTTP/1.1\r\nContent-Length: +5\r\n\r\n",
        /* With a token no length is too long for the socket, so only the
         * parser stands between these and a length of SIZE_MAX: strtoull()
         * wrapped the first and saturated the second. */
        "PUT /x HTTP/1.1\r\nx-amz-rdma-token: t\r\n"
        "Content-Length: -1\r\n\r\n",
        "PUT /x HTTP/1.1\r\nx-amz-rdma-token: t\r\n"
        "Content-Length: 99999999999999999999\r\n\r\n",
        /* Chunked must be refused, not read as identity: the length
         * prefixes would land in the object as if they were data. */
        "PUT /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ssize_t n = s3_http_parse(bad[i], strlen(bad[i]), &req);
        if (n != -EINVAL)
            fail(name, "case %zu returned %zd, want -EINVAL", i, n);
    }

    /* A request that will never fit has to be refused, not buffered. */
    size_t huge = S3_HTTP_REQUEST_MAX + 64;
    char *big = malloc(huge);
    if (big == NULL) {
        fail(name, "out of memory");
        return;
    }
    memset(big, 'A', huge);
    ssize_t n = s3_http_parse(big, huge, &req);
    if (n != -EMSGSIZE)
        fail(name, "oversized request returned %zd, want -EMSGSIZE", n);
    free(big);

    ok(name);
}

/*
 * A percent escape is two hex digits, and the path is decoded on parse, so
 * a broken escape in it fails the request rather than being passed through
 * as literal text the store would then take for part of the key.
 */
static void test_parse_bad_escapes(void)
{
    const char *name = "parse-bad-escapes";
    struct s3_http_request req;

    static const char *bad[] = {
        "GET /ernic/a% HTTP/1.1\r\n\r\n",     /* no digits */
        "GET /ernic/a%4 HTTP/1.1\r\n\r\n",    /* one digit */
        "GET /ernic/a%zz HTTP/1.1\r\n\r\n",   /* not hex */
        "GET /ernic/a%g0 HTTP/1.1\r\n\r\n",   /* first digit not hex */
        "GET /ernic/a%0g HTTP/1.1\r\n\r\n",   /* second digit not hex */
        "GET /ernic/a%00b HTTP/1.1\r\n\r\n",  /* a NUL smuggled into the key */
        "GET /ernic/a%4?x=1 HTTP/1.1\r\n\r\n" /* cut short by the query */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ssize_t n = s3_http_parse(bad[i], strlen(bad[i]), &req);
        if (n != -EINVAL)
            fail(name, "case %zu returned %zd, want -EINVAL", i, n);
    }

    /* An escape that ends the path exactly is whole, not cut short. */
    static const char edge[] = "GET /ernic/a%41 HTTP/1.1\r\n\r\n";
    ssize_t n = s3_http_parse(edge, strlen(edge), &req);
    if (n != (ssize_t)strlen(edge))
        fail(name, "an escape ending the path returned %zd", n);
    else if (strcmp(req.path, "/ernic/aA") != 0)
        fail(name, "path \"%s\", want \"/ernic/aA\"", req.path);

    /* The query is decoded on lookup, so a bad escape there fails the
     * lookup and leaves the request itself alone. */
    static const char query[] = "GET /ernic?prefix=a%2 HTTP/1.1\r\n\r\n";
    char val[64];
    if (s3_http_parse(query, strlen(query), &req) <= 0)
        fail(name, "a bad escape in the query failed the whole request");
    else if (s3_http_query_get(&req, "prefix", val, sizeof(val)))
        fail(name, "a bad escape in the query decoded as \"%s\"", val);

    ok(name);
}

/* HTTP/1.0 closes after each response unless told otherwise, and this
 * server never keeps it open. */
static void test_parse_http10(void)
{
    const char *name = "parse-http10";
    struct s3_http_request req;

    static const char *closing[] = {
        "GET /ernic HTTP/1.0\r\n\r\n",
        "GET /ernic HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        "GET /ernic HTTP/1.1\r\nConnection: Close\r\n\r\n",
    };

    for (size_t i = 0; i < sizeof(closing) / sizeof(closing[0]); i++) {
        ssize_t n = s3_http_parse(closing[i], strlen(closing[i]), &req);
        if (n != (ssize_t)strlen(closing[i]))
            fail(name, "case %zu returned %zd", i, n);
        else if (req.keep_alive)
            fail(name, "case %zu is keep-alive, want close", i);
    }

    ok(name);
}

/* Lines may end in a bare LF, the header block included, and a body
 * follows the blank line as it would after CRLF. */
static void test_parse_lf_only(void)
{
    const char *name = "parse-lf-only";
    static const char text[] = "PUT /ernic/k HTTP/1.1\n"
                               "Content-Length: 5\n"
                               "\n"
                               "hello"
                               "GET /ernic/k HTTP/1.1\n\n";
    struct s3_http_request req;

    ssize_t n = s3_http_parse(text, strlen(text), &req);
    if (n != (ssize_t)(strlen(text) - strlen("GET /ernic/k HTTP/1.1\n\n")))
        fail(name, "consumed %zd bytes, want the PUT and its body", n);
    else if (strcmp(req.method, "PUT") != 0 ||
             strcmp(req.path, "/ernic/k") != 0)
        fail(name, "request line read as %s %s", req.method, req.path);
    else if (req.body_len != 5 || memcmp(req.body, "hello", 5) != 0)
        fail(name, "body not delivered");
    else
        ok(name);
}

/*
 * Each field fits a fixed buffer.  A request line part that does not fit
 * fails the request, and a header that does not fit is dropped like one
 * past the header limit; at one byte less, each fits.  Header values are
 * trimmed of the spaces and tabs around them.
 */
static void test_parse_field_limits(void)
{
    const char *name = "parse-field-limits";
    struct s3_http_request req;
    char *text = malloc(4096);
    char *fill = malloc(1024);

    if (text == NULL || fill == NULL) {
        fail(name, "out of memory");
        goto out;
    }

    static const char *empty[] = {"\r\n\r\n", "\n\n"};
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        ssize_t n = s3_http_parse(empty[i], strlen(empty[i]), &req);
        if (n != -EINVAL)
            fail(name, "empty request line %zu returned %zd", i, n);
    }

    /* method[16]: 15 characters and the NUL. */
    for (size_t len = 15; len <= 16; len++) {
        memset(fill, 'M', len);
        fill[len] = '\0';
        int w = snprintf(text, 4096, "%s / HTTP/1.1\r\n\r\n", fill);
        ssize_t n = s3_http_parse(text, (size_t)w, &req);
        if (len == 15 && n != w)
            fail(name, "a %zu character method returned %zd", len, n);
        if (len == 16 && n != -EINVAL)
            fail(name, "a %zu character method returned %zd", len, n);
    }

    /* The raw query: S3_HTTP_QUERY_MAX - 1 characters and the NUL. */
    for (size_t len = S3_HTTP_QUERY_MAX - 1; len <= S3_HTTP_QUERY_MAX; len++) {
        memset(fill, 'q', len);
        fill[len] = '\0';
        int w = snprintf(text, 4096, "GET /ernic?%s HTTP/1.1\r\n\r\n", fill);
        ssize_t n = s3_http_parse(text, (size_t)w, &req);
        bool fits = len < S3_HTTP_QUERY_MAX;
        if (fits && n != w)
            fail(name, "a %zu byte query returned %zd", len, n);
        if (!fits && n != -EINVAL)
            fail(name, "a %zu byte query returned %zd", len, n);
    }

    /* Header names and values: one byte short of the buffer, then full. */
    static const struct {
        const char *what;
        size_t cap;
        bool is_name;
    } fields[] = {
        {"name", S3_HTTP_NAME_MAX, true},
        {"value", S3_HTTP_VALUE_MAX, false},
    };
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        for (size_t len = fields[f].cap - 1; len <= fields[f].cap; len++) {
            memset(fill, 'h', len);
            fill[len] = '\0';
            char line[S3_HTTP_VALUE_MAX + 16];
            if (fields[f].is_name)
                snprintf(line, sizeof(line), "%s: v", fill);
            else
                snprintf(line, sizeof(line), "X-Long: %s", fill);
            int w = snprintf(
                text, 4096, "GET / HTTP/1.1\r\n%s\r\nX-After: 1\r\n\r\n", line);
            ssize_t n = s3_http_parse(text, (size_t)w, &req);
            bool fits = len < fields[f].cap;
            if (n != w) {
                fail(name, "a %zu byte header %s returned %zd", len,
                     fields[f].what, n);
                continue;
            }
            if (req.nhdr != (fits ? 2u : 1u))
                fail(name, "a %zu byte header %s left %u headers, want %u", len,
                     fields[f].what, req.nhdr, fits ? 2u : 1u);
            if (s3_http_header_get(&req, "x-after") == NULL)
                fail(name, "a %zu byte header %s lost the next header", len,
                     fields[f].what);
        }
    }

    static const char ws[] = "GET / HTTP/1.1\r\nX-Pad: \t value  with space \t "
                             "\r\nX-Empty:  \t\r\n\r\n";
    if (s3_http_parse(ws, strlen(ws), &req) != (ssize_t)strlen(ws)) {
        fail(name, "the whitespace request did not parse");
    } else {
        const char *v = s3_http_header_get(&req, "x-pad");
        const char *e = s3_http_header_get(&req, "x-empty");
        if (v == NULL || strcmp(v, "value  with space") != 0)
            fail(name, "trimmed value \"%s\"", v ? v : "(absent)");
        if (e == NULL || e[0] != '\0')
            fail(name, "a blank value read as \"%s\"", e ? e : "(absent)");
    }

    ok(name);
out:
    free(fill);
    free(text);
}

/*
 * A response header value that does not fit is not cut short: the
 * builder sets oom, and once it is set the body takes no more writes and
 * the serialiser produces nothing.
 */
static void test_response_value_too_long(void)
{
    const char *name = "response-value-too-long";
    struct s3_http_response resp;
    char value[S3_HTTP_VALUE_MAX + 1];

    s3_http_response_init(&resp, 200);
    memset(value, 'v', S3_HTTP_VALUE_MAX - 1);
    value[S3_HTTP_VALUE_MAX - 1] = '\0';
    s3_http_response_header(&resp, "X-Fits", "%s", value);
    if (resp.oom)
        fail(name, "a %d byte value set oom", S3_HTTP_VALUE_MAX - 1);

    s3_http_response_write(&resp, "abc", 3);
    s3_http_response_write(&resp, "ignored", 0);
    if (resp.body_len != 3)
        fail(name, "body_len %zu after writing 3 bytes", resp.body_len);

    value[S3_HTTP_VALUE_MAX - 1] = 'v';
    value[S3_HTTP_VALUE_MAX] = '\0';
    s3_http_response_header(&resp, "X-Long", "%s", value);
    if (!resp.oom)
        fail(name, "a %d byte value did not set oom", S3_HTTP_VALUE_MAX);

    s3_http_response_write(&resp, "def", 3);
    if (resp.body_len != 3)
        fail(name, "a write after oom changed body_len to %zu", resp.body_len);

    size_t len = 0;
    uint8_t *wire = s3_http_response_serialize(&resp, &len);
    if (wire != NULL)
        fail(name, "serialised a response with a truncated header");
    free(wire);
    s3_http_response_free(&resp);

    ok(name);
}

/*
 * A format that fails is an error, not empty output.  In the C locale,
 * which this program never leaves, a wide character outside ASCII has no
 * conversion, so formatting it makes vsnprintf() return -1.
 */
static void test_response_format_error(void)
{
    const char *name = "response-format-error";
    static const wchar_t bad[] = L"\x80";
    struct s3_http_response resp;

    s3_http_response_init(&resp, 200);
    s3_http_response_printf(&resp, "%ls", bad);
    if (!resp.oom)
        fail(name, "a failed body format did not set oom");
    else if (resp.body_len != 0)
        fail(name, "a failed body format wrote %zu bytes", resp.body_len);
    s3_http_response_free(&resp);

    s3_http_response_init(&resp, 200);
    s3_http_response_header(&resp, "X-Bad", "%ls", bad);
    if (!resp.oom)
        fail(name, "a failed header format did not set oom");
    s3_http_response_free(&resp);

    ok(name);
}

/*
 * Every allocation the response builder makes, failed in turn: a format
 * too long for the stack buffer, the body growing twice, and the
 * serialised copy.  Any one failing leaves no response, never a partial
 * one, and the response can still be freed.
 */
static void test_response_alloc_failures(void)
{
    const char *name = "response-alloc-failures";
    char big[600];
    unsigned nth;

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    for (nth = 0; nth < 64; nth++) {
        struct s3_http_response resp;
        size_t len = 0;

        s3_http_response_init(&resp, 200);
        s3_alloc_fail_arm(nth);
        s3_http_response_printf(&resp, "<%s>", big);
        s3_http_response_write(&resp, big, sizeof(big) - 1);
        uint8_t *wire = s3_http_response_serialize(&resp, &len);
        bool fired = s3_alloc_fail_disarm();

        if (!fired) {
            if (wire == NULL)
                fail(name, "with no allocation failed, nothing serialised");
            else if (len < 2 * sizeof(big))
                fail(name, "serialised %zu bytes, short of the body", len);
            free(wire);
            s3_http_response_free(&resp);
            break;
        }
        if (wire != NULL)
            fail(name, "allocation %u failed, but a response serialised", nth);
        free(wire);
        s3_http_response_free(&resp);
    }

    /* The printf buffer, two body reallocs, and the serialised copy. */
    if (nth < 4)
        fail(name, "only %u allocations could be failed, want 4", nth);
    else
        ok(name);
}

/*
 * A request's headers past S3_HTTP_MAX_HEADERS are dropped, not refused,
 * and a response's are refused: there the builder sets oom, and the
 * serialiser then produces nothing rather than a response missing a
 * header its caller asked for.
 */
static void test_too_many_headers(void)
{
    const char *name = "too-many-headers";
    struct s3_http_request req;
    char text[4096];
    size_t at = 0;

    int w = snprintf(text, sizeof(text), "GET /ernic HTTP/1.1\r\n");
    if (w < 0 || (size_t)w >= sizeof(text)) {
        fail(name, "request line does not fit");
        return;
    }
    at = (size_t)w;
    for (unsigned i = 0; i < S3_HTTP_MAX_HEADERS; i++) {
        w = snprintf(text + at, sizeof(text) - at, "X-Pad-%u: %u\r\n", i, i);
        if (w < 0 || (size_t)w >= sizeof(text) - at) {
            fail(name, "headers do not fit");
            return;
        }
        at += (size_t)w;
    }
    w = snprintf(text + at, sizeof(text) - at, "X-Late: 1\r\n\r\n");
    if (w < 0 || (size_t)w >= sizeof(text) - at) {
        fail(name, "last header does not fit");
        return;
    }
    at += (size_t)w;

    ssize_t n = s3_http_parse(text, at, &req);
    if (n != (ssize_t)at)
        fail(name, "request with too many headers returned %zd, want %zu", n,
             at);
    else if (req.nhdr != S3_HTTP_MAX_HEADERS)
        fail(name, "kept %u headers, want %d", req.nhdr, S3_HTTP_MAX_HEADERS);
    else if (s3_http_header_get(&req, "x-late") != NULL)
        fail(name, "a header past the limit was kept");
    else if (s3_http_header_get(&req, "x-pad-0") == NULL ||
             s3_http_header_get(&req, "x-pad-23") == NULL)
        fail(name, "a header within the limit was dropped");

    struct s3_http_response resp;
    s3_http_response_init(&resp, 200);
    for (unsigned i = 0; i < S3_HTTP_MAX_HEADERS; i++) {
        char hname[16];
        snprintf(hname, sizeof(hname), "X-Pad-%u", i);
        s3_http_response_header(&resp, hname, "%u", i);
    }
    /* Replacing a header needs no new slot, so a full table allows it. */
    s3_http_response_header(&resp, "x-pad-0", "replaced");
    if (resp.oom)
        fail(name, "replacing a header in a full table set oom");
    else if (strcmp(resp.hdr[0].value, "replaced") != 0)
        fail(name, "header 0 is \"%s\", want \"replaced\"", resp.hdr[0].value);

    s3_http_response_header(&resp, "X-Late", "1");
    if (!resp.oom)
        fail(name, "a header past the limit did not set oom");
    if (resp.nhdr != S3_HTTP_MAX_HEADERS)
        fail(name, "response has %u headers, want %d", resp.nhdr,
             S3_HTTP_MAX_HEADERS);

    size_t len = 0;
    uint8_t *wire = s3_http_response_serialize(&resp, &len);
    if (wire != NULL)
        fail(name, "serialised a response that lost a header");
    free(wire);
    s3_http_response_free(&resp);

    ok(name);
}

/*
 * s3_http_response_printf() formats into a 512-byte stack buffer first
 * and goes to the heap only for longer output.  The output lengths either
 * side of that boundary, 511 bytes and 512 with the two the format adds,
 * are where an off-by-one would lose or add a byte.
 */
static void test_response_printf_large(void)
{
    const char *name = "response-printf-large";
    static const size_t lengths[] = {509, 510, 511, 4000};
    char *text = malloc(4001);

    if (text == NULL) {
        fail(name, "out of memory");
        return;
    }

    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size_t want = lengths[i];
        struct s3_http_response resp;

        for (size_t j = 0; j < want; j++)
            text[j] = (char)('a' + j % 26);
        text[want] = '\0';

        s3_http_response_init(&resp, 200);
        s3_http_response_printf(&resp, "<%s>", text);

        if (resp.oom)
            fail(name, "%zu bytes: builder ran out of memory", want);
        else if (resp.body_len != want + 2)
            fail(name, "%zu bytes: body_len %zu, want %zu", want, resp.body_len,
                 want + 2);
        else if (resp.body[0] != '<' || resp.body[want + 1] != '>' ||
                 memcmp(resp.body + 1, text, want) != 0)
            fail(name, "%zu bytes: body is not what was formatted", want);
        s3_http_response_free(&resp);
    }

    free(text);
    ok(name);
}

static void test_query(void)
{
    const char *name = "query";
    static const char req_text[] =
        "GET /ernic?list-type=2&prefix=a%2Fb%20c&max-keys=10&uploads "
        "HTTP/1.1\r\n\r\n";
    struct s3_http_request req;
    char val[64];

    if (s3_http_parse(req_text, strlen(req_text), &req) <= 0) {
        fail(name, "parse failed");
        return;
    }
    if (strcmp(req.path, "/ernic") != 0)
        fail(name, "query was not split off the path: \"%s\"", req.path);

    if (!s3_http_query_get(&req, "list-type", val, sizeof(val)) ||
        strcmp(val, "2") != 0)
        fail(name, "list-type = \"%s\"", val);

    /* Percent-decoding happens on lookup, not on parse. */
    if (!s3_http_query_get(&req, "prefix", val, sizeof(val)) ||
        strcmp(val, "a/b c") != 0)
        fail(name, "prefix = \"%s\", want \"a/b c\"", val);

    /* A valueless key is present with an empty value. */
    if (!s3_http_query_get(&req, "uploads", val, sizeof(val)))
        fail(name, "valueless key reported absent");
    else if (val[0] != '\0')
        fail(name, "valueless key yielded \"%s\"", val);

    if (s3_http_query_get(&req, "delimiter", val, sizeof(val)))
        fail(name, "found a key that is not there");

    /* "max-keys" must not be found by a prefix match on "max". */
    if (s3_http_query_get(&req, "max", val, sizeof(val)))
        fail(name, "prefix-matched \"max\" against \"max-keys\"");

    ok(name);
}

static void test_response(void)
{
    const char *name = "response";
    struct s3_http_response resp;

    s3_http_response_init(&resp, 200);
    resp.keep_alive = true;
    s3_http_response_header(&resp, "ETag", "\"%s\"", "0123456789abcdef");
    s3_http_response_header(&resp, "x-amz-rdma-reply", "200");
    s3_http_response_printf(&resp, "<Body count=\"%d\"/>", 3);

    size_t len = 0;
    uint8_t *wire = s3_http_response_serialize(&resp, &len);
    if (wire == NULL) {
        fail(name, "serialize returned NULL");
        s3_http_response_free(&resp);
        return;
    }

    char *text = malloc(len + 1);
    if (text == NULL) {
        fail(name, "out of memory");
        free(wire);
        s3_http_response_free(&resp);
        return;
    }
    memcpy(text, wire, len);
    text[len] = '\0';

    if (strncmp(text, "HTTP/1.1 200 OK\r\n", 17) != 0)
        fail(name, "status line: %.20s", text);
    if (strstr(text, "\r\nETag: \"0123456789abcdef\"\r\n") == NULL)
        fail(name, "ETag header missing or malformed");
    if (strstr(text, "\r\nContent-Length: 17\r\n") == NULL)
        fail(name, "Content-Length not written by the serialiser");
    if (strstr(text, "\r\nConnection: keep-alive\r\n") == NULL)
        fail(name, "Connection: keep-alive missing");
    if (strstr(text, "\r\n\r\n<Body count=\"3\"/>") == NULL)
        fail(name, "body missing or not separated from the headers");

    free(text);
    free(wire);
    s3_http_response_free(&resp);

    ok(name);
}

/* A header set twice must be replaced, not duplicated. */
static void test_response_header_replace(void)
{
    const char *name = "response-header-replace";
    struct s3_http_response resp;

    s3_http_response_init(&resp, 404);
    s3_http_response_header(&resp, "Content-Type", "text/plain");
    s3_http_response_header(&resp, "content-type", "application/xml");

    unsigned seen = 0;
    for (unsigned i = 0; i < resp.nhdr; i++)
        if (strcasecmp(resp.hdr[i].name, "content-type") == 0)
            seen++;

    if (seen != 1)
        fail(name, "Content-Type appears %u times, want 1", seen);
    else if (strcmp(resp.hdr[0].value, "application/xml") != 0)
        fail(name, "value \"%s\", want the second one", resp.hdr[0].value);
    else
        ok(name);

    s3_http_response_free(&resp);
}

/* Growing past the initial body capacity must not lose or corrupt bytes. */
static void test_response_body_growth(void)
{
    const char *name = "response-body-growth";
    struct s3_http_response resp;
    const unsigned chunks = 400;

    s3_http_response_init(&resp, 200);
    for (unsigned i = 0; i < chunks; i++)
        s3_http_response_printf(&resp, "<K>%04u</K>", i);

    if (resp.oom) {
        fail(name, "builder ran out of memory");
        s3_http_response_free(&resp);
        return;
    }
    if (resp.body_len != chunks * 11) {
        fail(name, "body_len %zu, want %u", resp.body_len, chunks * 11);
        s3_http_response_free(&resp);
        return;
    }
    for (unsigned i = 0; i < chunks; i++) {
        char want[16];
        snprintf(want, sizeof(want), "<K>%04u</K>", i);
        if (memcmp(resp.body + i * 11, want, 11) != 0) {
            fail(name, "chunk %u corrupted across a realloc", i);
            s3_http_response_free(&resp);
            return;
        }
    }

    s3_http_response_free(&resp);
    ok(name);
}

static void test_reason(void)
{
    const char *name = "reason";

    static const struct {
        int status;
        const char *want;
    } cases[] = {
        {200, "OK"},
        {204, "No Content"},
        {206, "Partial Content"},
        {400, "Bad Request"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {411, "Length Required"},
        {413, "Payload Too Large"},
        {416, "Range Not Satisfiable"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {503, "Service Unavailable"},
        {507, "Insufficient Storage"},
        {0, "Unknown"},
        {-1, "Unknown"},
        {599, "Unknown"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (strcmp(s3_http_reason(cases[i].status), cases[i].want) != 0)
            fail(name, "%d -> \"%s\", want \"%s\"", cases[i].status,
                 s3_http_reason(cases[i].status), cases[i].want);

    ok(name);
}

int main(void)
{
    test_parse_get();
    test_parse_incremental();
    test_parse_rdma_no_body();
    test_parse_body_still_framed();
    test_parse_pipelined();
    test_parse_errors();
    test_parse_bad_escapes();
    test_parse_http10();
    test_parse_lf_only();
    test_parse_field_limits();
    test_too_many_headers();
    test_response_value_too_long();
    test_response_format_error();
    test_response_alloc_failures();
    test_query();
    test_response();
    test_response_header_replace();
    test_response_body_growth();
    test_response_printf_large();
    test_reason();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 http tests passed\n");
    return failures;
}
