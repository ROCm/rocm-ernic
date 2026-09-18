/*
 * s3_http.c — the sliver of HTTP/1.1 an S3 control plane needs
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3_http.h"

static char lower(char c)
{
    return (char)tolower((unsigned char)c);
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower(*a) != lower(*b))
            return false;
    }
    return *a == *b;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/*
 * Percent-decode @src into @dst.  '+' is left alone: it is a legal
 * literal in an object key and only means space in a form body, which
 * this control plane never carries.
 */
static bool percent_decode(const char *src, size_t srclen, char *dst,
                           size_t dstcap)
{
    size_t o = 0;
    for (size_t i = 0; i < srclen; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= srclen)
                return false;
            int hi = hex_nibble(src[i + 1]);
            int lo = hex_nibble(src[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0')
            return false; /* a NUL smuggled in through %00 */
        if (o + 1 >= dstcap)
            return false;
        dst[o++] = c;
    }
    dst[o] = '\0';
    return true;
}

static bool copy_bounded(char *dst, size_t dstcap, const char *src, size_t len)
{
    if (len + 1 > dstcap)
        return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* Find the end of the header block, i.e. the first CRLFCRLF or LFLF. */
static const char *find_header_end(const char *buf, size_t len, size_t *hdr_len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            *hdr_len = i + 2;
            return buf + i + 2;
        }
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *hdr_len = i + 4;
            return buf + i + 4;
        }
    }
    return NULL;
}

/* Advance past one line, returning its length without the terminator. */
static const char *next_line(const char *p, const char *end, size_t *linelen)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (nl == NULL)
        return NULL;
    size_t n = (size_t)(nl - p);
    if (n > 0 && p[n - 1] == '\r')
        n--;
    *linelen = n;
    return nl + 1;
}

ssize_t s3_http_parse(const void *buf, size_t len, struct s3_http_request *req)
{
    const char *base = buf;
    size_t hdr_len = 0;

    if (len == 0)
        return 0;

    const char *body = find_header_end(base, len, &hdr_len);
    if (body == NULL)
        return len >= S3_HTTP_REQUEST_MAX ? -EMSGSIZE : 0;

    memset(req, 0, sizeof(*req));
    req->keep_alive =
        true; /* HTTP/1.1 default; the request line may lower it */

    const char *p = base;
    const char *hdr_end = base + hdr_len;
    size_t linelen = 0;

    /* Request line: METHOD SP request-target SP HTTP-version */
    p = next_line(p, hdr_end, &linelen);
    if (p == NULL || linelen == 0)
        return -EINVAL;

    const char *line = base;
    const char *sp1 = memchr(line, ' ', linelen);
    if (sp1 == NULL)
        return -EINVAL;
    const char *sp2 = memchr(sp1 + 1, ' ', linelen - (size_t)(sp1 - line) - 1);
    if (sp2 == NULL)
        return -EINVAL;

    if (!copy_bounded(req->method, sizeof(req->method), line,
                      (size_t)(sp1 - line)))
        return -EINVAL;
    for (char *m = req->method; *m; m++)
        *m = (char)toupper((unsigned char)*m);

    const char *target = sp1 + 1;
    size_t target_len = (size_t)(sp2 - target);
    const char *qmark = memchr(target, '?', target_len);
    size_t path_len = qmark ? (size_t)(qmark - target) : target_len;

    if (!percent_decode(target, path_len, req->path, sizeof(req->path)))
        return -EINVAL;
    if (qmark != NULL && !copy_bounded(req->query, sizeof(req->query),
                                       qmark + 1, target_len - path_len - 1))
        return -EINVAL;

    /* "HTTP/1.0" defaults to close, and nothing older is served at all. */
    size_t ver_len = linelen - (size_t)(sp2 + 1 - line);
    if (ver_len >= 8 && !memcmp(sp2 + 1, "HTTP/1.0", 8))
        req->keep_alive = false;

    /* Header lines.  A header we have no room for is dropped rather than
     * failing the request: the ones this server acts on are few and come
     * first in every client that matters, and a 431 for a stray
     * X-Amz-Trace header would be a worse answer than ignoring it. */
    while (p < hdr_end) {
        const char *hline = p;
        p = next_line(p, hdr_end, &linelen);
        if (p == NULL)
            break;
        if (linelen == 0)
            break; /* the blank line that ends the block */

        const char *colon = memchr(hline, ':', linelen);
        if (colon == NULL)
            return -EINVAL;

        size_t nlen = (size_t)(colon - hline);
        const char *v = colon + 1;
        size_t vlen = linelen - nlen - 1;
        while (vlen > 0 && (*v == ' ' || *v == '\t')) {
            v++;
            vlen--;
        }
        while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
            vlen--;

        if (req->nhdr >= S3_HTTP_MAX_HEADERS)
            continue;
        struct s3_http_header *h = &req->hdr[req->nhdr];
        if (!copy_bounded(h->name, sizeof(h->name), hline, nlen) ||
            !copy_bounded(h->value, sizeof(h->value), v, vlen))
            continue;
        for (char *n = h->name; *n; n++)
            *n = lower(*n);
        req->nhdr++;
    }

    const char *cl = s3_http_header_get(req, "content-length");
    if (cl != NULL) {
        char *end = NULL;
        unsigned long long v = strtoull(cl, &end, 10);
        if (end == cl || *end != '\0' || v > S3_HTTP_REQUEST_MAX)
            return -EINVAL;
        req->content_length = (size_t)v;
    }

    /* Chunked bodies are refused rather than mis-parsed as identity: a
     * silent misread would hand the object store a length prefix as if
     * it were data. */
    const char *te = s3_http_header_get(req, "transfer-encoding");
    if (te != NULL && !ieq(te, "identity"))
        return -EINVAL;

    const char *conn = s3_http_header_get(req, "connection");
    if (conn != NULL && ieq(conn, "close"))
        req->keep_alive = false;

    size_t have = len - hdr_len;
    if (have < req->content_length)
        return hdr_len + req->content_length > S3_HTTP_REQUEST_MAX ? -EMSGSIZE
                                                                   : 0;

    req->body = req->content_length ? (const uint8_t *)body : NULL;
    req->body_len = req->content_length;
    return (ssize_t)(hdr_len + req->content_length);
}

const char *s3_http_header_get(const struct s3_http_request *req,
                               const char *name)
{
    for (unsigned i = 0; i < req->nhdr; i++) {
        if (ieq(req->hdr[i].name, name))
            return req->hdr[i].value;
    }
    return NULL;
}

bool s3_http_query_get(const struct s3_http_request *req, const char *key,
                       char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = req->query;

    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seglen);
        size_t nlen = eq ? (size_t)(eq - p) : seglen;

        if (nlen == klen && !strncasecmp(p, key, klen)) {
            if (out != NULL && outlen > 0) {
                const char *v = eq ? eq + 1 : "";
                size_t vlen = eq ? seglen - nlen - 1 : 0;
                if (!percent_decode(v, vlen, out, outlen))
                    return false;
            }
            return true;
        }
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    if (out != NULL && outlen > 0)
        out[0] = '\0';
    return false;
}

/* -------------------------------------------------------------------------
 * Responses
 * -------------------------------------------------------------------------
 */

void s3_http_response_init(struct s3_http_response *resp, int status)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = status;
    resp->keep_alive = true;
}

void s3_http_response_free(struct s3_http_response *resp)
{
    free(resp->body);
    resp->body = NULL;
    resp->body_len = resp->body_cap = 0;
}

void s3_http_response_header(struct s3_http_response *resp, const char *name,
                             const char *fmt, ...)
{
    struct s3_http_header *h = NULL;

    for (unsigned i = 0; i < resp->nhdr; i++) {
        if (ieq(resp->hdr[i].name, name)) {
            h = &resp->hdr[i];
            break;
        }
    }
    if (h == NULL) {
        if (resp->nhdr >= S3_HTTP_MAX_HEADERS) {
            resp->oom = true;
            return;
        }
        h = &resp->hdr[resp->nhdr++];
    }

    snprintf(h->name, sizeof(h->name), "%s", name);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(h->value, sizeof(h->value), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(h->value))
        resp->oom = true;
}

void s3_http_response_write(struct s3_http_response *resp, const void *data,
                            size_t len)
{
    if (resp->oom || len == 0)
        return;

    if (resp->body_len + len > resp->body_cap) {
        size_t cap = resp->body_cap ? resp->body_cap : 256;
        while (cap < resp->body_len + len)
            cap *= 2;
        uint8_t *nb = realloc(resp->body, cap);
        if (nb == NULL) {
            resp->oom = true;
            return;
        }
        resp->body = nb;
        resp->body_cap = cap;
    }
    memcpy(resp->body + resp->body_len, data, len);
    resp->body_len += len;
}

void s3_http_response_printf(struct s3_http_response *resp, const char *fmt,
                             ...)
{
    char stack[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        resp->oom = true;
        return;
    }
    if ((size_t)n < sizeof(stack)) {
        s3_http_response_write(resp, stack, (size_t)n);
        return;
    }

    char *heap = malloc((size_t)n + 1);
    if (heap == NULL) {
        resp->oom = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s3_http_response_write(resp, heap, (size_t)n);
    free(heap);
}

const char *s3_http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 416:
        return "Range Not Satisfiable";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 507:
        return "Insufficient Storage";
    default:
        return "Unknown";
    }
}

uint8_t *s3_http_response_serialize(const struct s3_http_response *resp,
                                    size_t *out_len)
{
    if (resp->oom)
        return NULL;

    size_t cap = 128 + resp->body_len;
    for (unsigned i = 0; i < resp->nhdr; i++)
        cap += strlen(resp->hdr[i].name) + strlen(resp->hdr[i].value) + 4;

    uint8_t *out = malloc(cap);
    if (out == NULL)
        return NULL;

    int off = snprintf((char *)out, cap, "HTTP/1.1 %d %s\r\n", resp->status,
                       s3_http_reason(resp->status));
    for (unsigned i = 0; i < resp->nhdr; i++)
        off += snprintf((char *)out + off, cap - (size_t)off, "%s: %s\r\n",
                        resp->hdr[i].name, resp->hdr[i].value);

    /* Content-Length and Connection are ours, not the caller's: getting
     * either wrong desynchronises a keep-alive connection for good. */
    off += snprintf((char *)out + off, cap - (size_t)off,
                    "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                    resp->body_len, resp->keep_alive ? "keep-alive" : "close");

    if (resp->body_len)
        memcpy(out + off, resp->body, resp->body_len);

    *out_len = (size_t)off + resp->body_len;
    return out;
}
