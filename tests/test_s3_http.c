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
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
        {404, "Not Found"},
        {500, "Internal Server Error"},
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
    test_query();
    test_response();
    test_response_header_replace();
    test_response_body_growth();
    test_reason();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 http tests passed\n");
    return failures;
}
