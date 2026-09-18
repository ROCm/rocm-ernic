/*
 * s3_http.h — the sliver of HTTP/1.1 an S3 control plane needs
 *
 * Buffers in, buffers out.  Nothing here opens a socket or knows what a
 * queue pair is, which is what lets the request parser and the response
 * builder be exercised under CTest with string literals, and what lets
 * the same code sit behind the in-process TCP stack in s3_tcp.c.
 *
 * The subset is deliberate.  An S3-over-RDMA control plane carries
 * request lines, headers and small XML bodies; the object payload never
 * appears here, because that is what the RDMA data plane is for.  So
 * there is no chunked transfer coding, no trailers, no compression and
 * no pipelining beyond "parse one request, then tell the caller how many
 * bytes you consumed".
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef S3_HTTP_H
#define S3_HTTP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define S3_HTTP_MAX_HEADERS 24
#define S3_HTTP_NAME_MAX    64
#define S3_HTTP_VALUE_MAX   512
#define S3_HTTP_PATH_MAX    1024
#define S3_HTTP_QUERY_MAX   512

/*
 * The largest request we will hold before giving up on a client.  A
 * control-plane request is a few hundred bytes; anything approaching
 * this is a client that has lost the plot, and refusing it is what keeps
 * a connection's receive buffer bounded.
 */
#define S3_HTTP_REQUEST_MAX 16384

struct s3_http_header {
    char name[S3_HTTP_NAME_MAX]; /* lowercased on parse */
    char value[S3_HTTP_VALUE_MAX];
};

struct s3_http_request {
    char method[16];
    char path[S3_HTTP_PATH_MAX];   /* percent-decoded, query removed */
    char query[S3_HTTP_QUERY_MAX]; /* raw, still percent-encoded */
    struct s3_http_header hdr[S3_HTTP_MAX_HEADERS];
    unsigned nhdr;
    size_t content_length;
    bool keep_alive;
    const uint8_t *body; /* points into the caller's buffer */
    size_t body_len;
};

/*
 * Parse one request out of @buf.
 *
 * Returns the number of bytes consumed, 0 when the request is still
 * incomplete and more should be read, or a negative errno: -EMSGSIZE
 * when it exceeds S3_HTTP_REQUEST_MAX, -EINVAL when it is malformed.
 * On success @req->body points into @buf and stays valid only as long as
 * @buf does.
 */
ssize_t s3_http_parse(const void *buf, size_t len, struct s3_http_request *req);

/* Header lookup, case-insensitive.  NULL when absent. */
const char *s3_http_header_get(const struct s3_http_request *req,
                               const char *name);

/*
 * Look up one key in the query string.  A valueless key ("?uploads")
 * yields an empty string, which is why presence is the return value
 * rather than a non-empty @out.
 */
bool s3_http_query_get(const struct s3_http_request *req, const char *key,
                       char *out, size_t outlen);

/* -------------------------------------------------------------------------
 * Responses
 * -------------------------------------------------------------------------
 */

struct s3_http_response {
    int status;
    struct s3_http_header hdr[S3_HTTP_MAX_HEADERS];
    unsigned nhdr;
    uint8_t *body;
    size_t body_len;
    size_t body_cap;
    bool keep_alive;
    bool oom; /* sticky: an allocation failed somewhere along the way */
};

void s3_http_response_init(struct s3_http_response *resp, int status);
void s3_http_response_free(struct s3_http_response *resp);

/* Set (or replace) a header.  Silently sets resp->oom if there is no room. */
void s3_http_response_header(struct s3_http_response *resp, const char *name,
                             const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Append to the body. */
void s3_http_response_write(struct s3_http_response *resp, const void *data,
                            size_t len);
void s3_http_response_printf(struct s3_http_response *resp, const char *fmt,
                             ...) __attribute__((format(printf, 2, 3)));

/*
 * Render status line, headers and body into one freshly allocated
 * buffer.  Content-Length is written here, so callers never set it.
 * Returns NULL on allocation failure; the caller frees the result.
 */
uint8_t *s3_http_response_serialize(const struct s3_http_response *resp,
                                    size_t *out_len);

/* Reason phrase for a status, "Unknown" for one we do not name. */
const char *s3_http_reason(int status);

#endif /* S3_HTTP_H */
