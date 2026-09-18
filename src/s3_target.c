/*
 * s3_target.c — in-process S3-over-RDMA object store
 *
 * Implements the subset of the S3 REST API that an RDMA object client
 * exercises: PUT, GET, HEAD and DELETE of an object, ranged GET,
 * ListObjectsV2, and the three-call multipart upload sequence.  The
 * semantics -- status codes, error codes, ETag shape, the XML element
 * names -- follow versitygw, which is the closest thing to a readable
 * reference implementation of what real clients actually depend on.
 *
 * What makes this an S3-over-RDMA target rather than an S3 target is the
 * x-amz-rdma-token header.  When a request carries one, the object bytes
 * do not travel in the HTTP message at all: the token describes a buffer
 * the client has registered, and the target moves the payload straight
 * into or out of it through the DMA callbacks, then answers with an
 * x-amz-rdma-reply header saying how it went.  A request without a token
 * is served the ordinary way, with the payload in the body, so the same
 * store is reachable from curl.
 *
 * Objects live in malloc'd buffers.  This is an emulator backend sized
 * for tests and microbenchmarks, not a filesystem, and a flat array with
 * a linear scan keeps the whole module free of any dependency beyond
 * libc -- which is what lets the tests link it directly.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "s3_target.h"

#define S3_DEFAULT_BUCKET   "ernic"
#define S3_DEFAULT_CAPACITY (256ull << 20)
#define S3_DEFAULT_OBJECTS  256u
#define S3_DEFAULT_TRADDR   0xc0a8c801u /* 192.168.200.1 */
#define S3_DEFAULT_PORT     9000
#define S3_DEFAULT_MAX_PART (256ull << 20)

#define S3_MAX_UPLOADS   16u
#define S3_MAX_PARTS     1024u
#define S3_LIST_MAX_KEYS 1000u
#define S3_ETAG_MAX      48

#define S3_RDMA_TOKEN_HDR "x-amz-rdma-token"
#define S3_RDMA_REPLY_HDR "x-amz-rdma-reply"

struct s3_object {
    bool in_use;
    char key[S3_KEY_MAX];
    uint8_t *data;
    size_t len;
    char etag[S3_ETAG_MAX]; /* including the quotes S3 puts round it */
    time_t mtime;
};

struct s3_part {
    uint32_t number;
    uint8_t *data;
    size_t len;
    uint8_t md5[16];
};

struct s3_upload {
    bool in_use;
    char id[24];
    char key[S3_KEY_MAX];
    struct s3_part parts[S3_MAX_PARTS];
    unsigned nparts;
    size_t bytes;
};

struct s3_target {
    struct s3_target_cfg cfg;
    struct s3_target_stats stats;

    struct s3_object *objs;
    unsigned obj_cap;

    struct s3_upload uploads[S3_MAX_UPLOADS];
    uint32_t next_upload;

    uint64_t used;
};

/* ------------------------------------------------------------------ */
/* MD5, for ETags                                                     */
/* ------------------------------------------------------------------ */

/*
 * S3 clients compare ETags, so the digest has to be the real MD5 of the
 * object and not merely something stable.  This is the reference
 * algorithm from RFC 1321, written out rather than pulled from a crypto
 * library: the target must link into a unit test with no dependencies,
 * and this is the only hash it needs.
 */

struct md5_ctx {
    uint32_t state[4];
    uint64_t count;
    uint8_t buf[64];
    size_t buflen;
};

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rotl32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static void md5_block(struct md5_ctx *c, const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }

    uint32_t a = c->state[0], b = c->state[1];
    uint32_t d = c->state[3], cc = c->state[2];

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + rotl32(a + f + MD5_K[i] + m[g], MD5_S[i]);
        a = tmp;
    }

    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
}

static void md5_init(struct md5_ctx *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->count = 0;
    c->buflen = 0;
}

static void md5_update(struct md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;

    c->count += len;
    if (c->buflen > 0) {
        size_t need = 64 - c->buflen;
        size_t take = len < need ? len : need;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen < 64)
            return;
        md5_block(c, c->buf);
        c->buflen = 0;
    }
    while (len >= 64) {
        md5_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

static void md5_final(struct md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->count * 8;
    static const uint8_t pad[64] = {0x80};
    size_t padlen = (c->buflen < 56) ? (56 - c->buflen) : (120 - c->buflen);

    md5_update(c, pad, padlen);

    uint8_t tail[8];
    for (int i = 0; i < 8; i++)
        tail[i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, tail, 8);

    for (int i = 0; i < 4; i++) {
        out[i * 4] = (uint8_t)c->state[i];
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(c->state[i] >> 24);
    }
}

static void md5_buf(const void *data, size_t len, uint8_t out[16])
{
    struct md5_ctx c;
    md5_init(&c);
    md5_update(&c, data, len);
    md5_final(&c, out);
}

static void md5_hex(const uint8_t md5[16], char *out)
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = HEX[md5[i] >> 4];
        out[i * 2 + 1] = HEX[md5[i] & 0xf];
    }
    out[32] = '\0';
}

static void etag_of(const void *data, size_t len, char *out)
{
    uint8_t md5[16];
    char hex[33];

    md5_buf(data, len, md5);
    md5_hex(md5, hex);
    snprintf(out, S3_ETAG_MAX, "\"%s\"", hex);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

void s3_target_cfg_defaults(struct s3_target_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->bucket, sizeof(cfg->bucket), "%s", S3_DEFAULT_BUCKET);
    cfg->capacity = S3_DEFAULT_CAPACITY;
    cfg->max_objects = S3_DEFAULT_OBJECTS;
    cfg->traddr = S3_DEFAULT_TRADDR;
    cfg->trsvcid = S3_DEFAULT_PORT;
    cfg->max_part = S3_DEFAULT_MAX_PART;
}

static bool parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (*s == '\0' || *s == '-') {
        return false;
    }
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s) {
        return false;
    }

    uint64_t mult = 1;
    switch (*end) {
    case 'k':
    case 'K':
        mult = 1ull << 10;
        end++;
        break;
    case 'm':
    case 'M':
        mult = 1ull << 20;
        end++;
        break;
    case 'g':
    case 'G':
        mult = 1ull << 30;
        end++;
        break;
    default:
        break;
    }
    if (mult != 1) {
        if (*end == 'i' || *end == 'I') {
            end++;
        }
        if (*end == 'b' || *end == 'B') {
            end++;
        }
    }
    if (*end != '\0') {
        return false;
    }
    if (v != 0 && v > UINT64_MAX / mult) {
        return false;
    }
    *out = (uint64_t)v * mult;
    return true;
}

static bool parse_ipv4(const char *s, uint32_t *out)
{
    unsigned o[4];
    char extra;

    if (sscanf(s, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &extra) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255) {
            return false;
        }
    }
    *out = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* Bucket names are DNS labels: lowercase, digits, dot and dash. */
static bool bucket_name_ok(const char *s)
{
    size_t len = strlen(s);

    if (len < 3 || len >= S3_BUCKET_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return s[0] != '-' && s[0] != '.' && s[len - 1] != '-' && s[len - 1] != '.';
}

bool s3_target_cfg_parse(struct s3_target_cfg *cfg, const char *opts, char *err,
                         size_t errlen)
{
    if (opts == NULL || *opts == '\0') {
        return true;
    }

    char *dup = strdup(opts);
    if (dup == NULL) {
        set_err(err, errlen, "out of memory");
        return false;
    }

    bool ok = true;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok != NULL && ok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            set_err(err, errlen, "option '%s' is not key=value", tok);
            ok = false;
            break;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        if (strcmp(key, "bucket") == 0) {
            if (!bucket_name_ok(val)) {
                set_err(err, errlen,
                        "bucket '%s' must be 3..%d characters of [a-z0-9.-] "
                        "not starting or ending in '.' or '-'",
                        val, S3_BUCKET_MAX - 1);
                ok = false;
            } else {
                snprintf(cfg->bucket, sizeof(cfg->bucket), "%s", val);
            }
        } else if (strcmp(key, "size") == 0) {
            if (!parse_size(val, &cfg->capacity) || cfg->capacity == 0) {
                set_err(err, errlen, "bad size '%s'", val);
                ok = false;
            }
        } else if (strcmp(key, "objects") == 0) {
            uint32_t v;
            if (!parse_u32(val, &v) || v == 0 || v > 65536) {
                set_err(err, errlen, "objects must be 1..65536 (got '%s')",
                        val);
                ok = false;
            } else {
                cfg->max_objects = v;
            }
        } else if (strcmp(key, "ip") == 0) {
            if (!parse_ipv4(val, &cfg->traddr)) {
                set_err(err, errlen, "bad IPv4 address '%s'", val);
                ok = false;
            }
        } else if (strcmp(key, "port") == 0) {
            uint32_t v;
            if (!parse_u32(val, &v) || v == 0 || v > 65535) {
                set_err(err, errlen, "bad port '%s'", val);
                ok = false;
            } else {
                cfg->trsvcid = (uint16_t)v;
            }
        } else if (strcmp(key, "maxpart") == 0) {
            uint64_t v;
            if (!parse_size(val, &v) || v == 0 || v > UINT32_MAX) {
                set_err(err, errlen, "bad maxpart '%s'", val);
                ok = false;
            } else {
                cfg->max_part = (uint32_t)v;
            }
        } else {
            set_err(err, errlen, "unknown s3 option '%s'", key);
            ok = false;
        }
    }

    free(dup);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

struct s3_target *s3_target_create(const struct s3_target_cfg *cfg, char *err,
                                   size_t errlen)
{
    struct s3_target *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    t->cfg = *cfg;
    if (t->cfg.max_objects == 0) {
        t->cfg.max_objects = S3_DEFAULT_OBJECTS;
    }
    if (t->cfg.max_part == 0) {
        t->cfg.max_part = S3_DEFAULT_MAX_PART;
    }

    t->obj_cap = t->cfg.max_objects;
    t->objs = calloc(t->obj_cap, sizeof(*t->objs));
    if (t->objs == NULL) {
        set_err(err, errlen, "out of memory");
        free(t);
        return NULL;
    }
    t->next_upload = 1;
    return t;
}

static void upload_reset(struct s3_upload *u)
{
    for (unsigned i = 0; i < u->nparts; i++) {
        free(u->parts[i].data);
    }
    memset(u, 0, sizeof(*u));
}

void s3_target_destroy(struct s3_target *t)
{
    if (t == NULL) {
        return;
    }
    for (unsigned i = 0; i < t->obj_cap; i++) {
        free(t->objs[i].data);
    }
    for (unsigned i = 0; i < S3_MAX_UPLOADS; i++) {
        if (t->uploads[i].in_use) {
            upload_reset(&t->uploads[i]);
        }
    }
    free(t->objs);
    free(t);
}

const struct s3_target_cfg *s3_target_config(const struct s3_target *t)
{
    return &t->cfg;
}

const struct s3_target_stats *s3_target_statistics(const struct s3_target *t)
{
    return &t->stats;
}

unsigned s3_target_object_count(const struct s3_target *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < t->obj_cap; i++) {
        if (t->objs[i].in_use) {
            n++;
        }
    }
    return n;
}

uint64_t s3_target_bytes_used(const struct s3_target *t)
{
    return t->used;
}

void s3_target_peer_token(const struct s3_target *t, uint32_t rkey,
                          uint64_t addr, uint64_t length, struct s3_token *out)
{
    memset(out, 0, sizeof(*out));
    out->transport = S3_TRANSPORT_RC;
    out->qp_num = S3_TARGET_QPN_BASE | 1u;
    out->gid[10] = 0xff;
    out->gid[11] = 0xff;
    out->gid[12] = (uint8_t)(t->cfg.traddr >> 24);
    out->gid[13] = (uint8_t)(t->cfg.traddr >> 16);
    out->gid[14] = (uint8_t)(t->cfg.traddr >> 8);
    out->gid[15] = (uint8_t)t->cfg.traddr;
    out->rkey = rkey;
    out->remote_addr = addr;
    out->length = length;
    out->port_num = 1;
}

/* ------------------------------------------------------------------ */
/* Object table                                                       */
/* ------------------------------------------------------------------ */

static struct s3_object *obj_find(struct s3_target *t, const char *key)
{
    for (unsigned i = 0; i < t->obj_cap; i++) {
        if (t->objs[i].in_use && strcmp(t->objs[i].key, key) == 0) {
            return &t->objs[i];
        }
    }
    return NULL;
}

static struct s3_object *obj_alloc(struct s3_target *t)
{
    for (unsigned i = 0; i < t->obj_cap; i++) {
        if (!t->objs[i].in_use) {
            return &t->objs[i];
        }
    }
    return NULL;
}

static void obj_release(struct s3_target *t, struct s3_object *o)
{
    t->used -= o->len;
    free(o->data);
    memset(o, 0, sizeof(*o));
}

/*
 * Install @data (ownership transferred) at @key, replacing whatever was
 * there.  Returns NULL when there is no room, having freed nothing: the
 * caller still owns @data and reports the right S3 error.
 */
static struct s3_object *obj_store(struct s3_target *t, const char *key,
                                   uint8_t *data, size_t len, const char *etag)
{
    struct s3_object *o = obj_find(t, key);
    uint64_t freed = (o != NULL) ? o->len : 0;

    if (t->used - freed + len > t->cfg.capacity) {
        return NULL;
    }
    if (o == NULL) {
        o = obj_alloc(t);
        if (o == NULL) {
            return NULL;
        }
        snprintf(o->key, sizeof(o->key), "%s", key);
    } else {
        free(o->data);
    }

    t->used = t->used - freed + len;
    o->in_use = true;
    o->data = data;
    o->len = len;
    o->mtime = time(NULL);
    if (etag != NULL) {
        snprintf(o->etag, sizeof(o->etag), "%s", etag);
    } else {
        etag_of(data, len, o->etag);
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Multipart uploads                                                  */
/* ------------------------------------------------------------------ */

static struct s3_upload *upload_find(struct s3_target *t, const char *id)
{
    for (unsigned i = 0; i < S3_MAX_UPLOADS; i++) {
        if (t->uploads[i].in_use && strcmp(t->uploads[i].id, id) == 0) {
            return &t->uploads[i];
        }
    }
    return NULL;
}

static struct s3_upload *upload_alloc(struct s3_target *t, const char *key)
{
    for (unsigned i = 0; i < S3_MAX_UPLOADS; i++) {
        struct s3_upload *u = &t->uploads[i];
        if (u->in_use) {
            continue;
        }
        memset(u, 0, sizeof(*u));
        u->in_use = true;
        snprintf(u->id, sizeof(u->id), "%08x%08x", t->next_upload++,
                 (unsigned)(uintptr_t)u & 0xffffffffu);
        snprintf(u->key, sizeof(u->key), "%s", key);
        return u;
    }
    return NULL;
}

static struct s3_part *part_slot(struct s3_upload *u, uint32_t number)
{
    for (unsigned i = 0; i < u->nparts; i++) {
        if (u->parts[i].number == number) {
            return &u->parts[i];
        }
    }
    if (u->nparts >= S3_MAX_PARTS) {
        return NULL;
    }
    struct s3_part *p = &u->parts[u->nparts++];
    p->number = number;
    return p;
}

static int part_cmp(const void *a, const void *b)
{
    const struct s3_part *pa = a, *pb = b;
    return (pa->number > pb->number) - (pa->number < pb->number);
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

static void xml_escape(struct s3_http_response *resp, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '&':
            s3_http_response_write(resp, "&amp;", 5);
            break;
        case '<':
            s3_http_response_write(resp, "&lt;", 4);
            break;
        case '>':
            s3_http_response_write(resp, "&gt;", 4);
            break;
        case '"':
            s3_http_response_write(resp, "&quot;", 6);
            break;
        default:
            s3_http_response_write(resp, p, 1);
            break;
        }
    }
}

static void iso8601(time_t t, char *out, size_t outlen)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, outlen, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
}

static void s3_error(struct s3_target *t, struct s3_http_response *resp,
                     int status, const char *code, const char *message,
                     const char *resource)
{
    t->stats.errors++;
    resp->status = status;
    resp->body_len = 0;
    s3_http_response_header(resp, "Content-Type", "application/xml");
    s3_http_response_printf(
        resp,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>%s"
        "</Code><Message>",
        code);
    xml_escape(resp, message);
    s3_http_response_write(resp, "</Message><Resource>", 20);
    xml_escape(resp, resource != NULL ? resource : "/");
    s3_http_response_write(resp, "</Resource></Error>\n", 20);
}

/*
 * Every RDMA-carrying request answers in x-amz-rdma-reply, success or
 * not: a client that posted a token is waiting on that header to learn
 * whether its buffer was touched, and gets no other signal.
 */
static void rdma_reply(struct s3_target *t, struct s3_http_response *resp,
                       int code, const struct s3_token *req_tok, uint64_t bytes)
{
    struct s3_token peer;
    char buf[S3_REPLY_MAX];

    s3_target_peer_token(t, req_tok->rkey, req_tok->remote_addr, bytes, &peer);
    s3_reply_format(code, &peer, buf);
    s3_http_response_header(resp, S3_RDMA_REPLY_HDR, "%s", buf);
    s3_http_response_header(resp, "x-amz-rdma-bytes", "%" PRIu64, bytes);
}

/* ------------------------------------------------------------------ */
/* Request routing                                                    */
/* ------------------------------------------------------------------ */

/*
 * Split "/bucket/key/with/slashes" into its two parts.  @bucket is empty
 * for "/" and @key is empty for "/bucket" -- the caller distinguishes a
 * service request from a bucket request from an object request on that
 * basis alone, which is exactly how the S3 API is specified.
 */
static void split_path(const char *path, char *bucket, size_t bucketlen,
                       const char **key)
{
    const char *p = path;

    while (*p == '/') {
        p++;
    }
    const char *slash = strchr(p, '/');
    size_t blen = slash ? (size_t)(slash - p) : strlen(p);
    if (blen >= bucketlen) {
        blen = bucketlen - 1;
    }
    memcpy(bucket, p, blen);
    bucket[blen] = '\0';
    *key = slash ? slash + 1 : "";
}

/* "bytes=<first>-<last>", the only Range form S3 answers. */
static bool parse_range(const char *v, size_t size, size_t *off, size_t *len)
{
    unsigned long long first, last;
    char *end;

    if (strncasecmp(v, "bytes=", 6) != 0) {
        return false;
    }
    v += 6;

    if (*v == '-') { /* suffix range: the final N bytes */
        errno = 0;
        last = strtoull(v + 1, &end, 10);
        if (errno != 0 || end == v + 1 || *end != '\0' || last == 0) {
            return false;
        }
        if (last > size) {
            last = size;
        }
        *off = size - (size_t)last;
        *len = (size_t)last;
        return true;
    }

    errno = 0;
    first = strtoull(v, &end, 10);
    if (errno != 0 || end == v || *end != '-') {
        return false;
    }
    if (first >= size) {
        return false;
    }
    v = end + 1;
    if (*v == '\0') {
        last = size - 1;
    } else {
        errno = 0;
        last = strtoull(v, &end, 10);
        if (errno != 0 || end == v || *end != '\0') {
            return false;
        }
        if (last >= size) {
            last = size - 1;
        }
        if (last < first) {
            return false;
        }
    }
    *off = (size_t)first;
    *len = (size_t)(last - first + 1);
    return true;
}

static bool have_token(const struct s3_http_request *req, struct s3_token *tok,
                       bool *malformed)
{
    const char *v = s3_http_header_get(req, S3_RDMA_TOKEN_HDR);

    *malformed = false;
    if (v == NULL) {
        return false;
    }
    if (!s3_token_parse_header(v, tok)) {
        *malformed = true;
        return false;
    }
    return true;
}

static void handle_list_buckets(struct s3_target *t,
                                struct s3_http_response *resp)
{
    char when[32];
    iso8601(0, when, sizeof(when));

    s3_http_response_header(resp, "Content-Type", "application/xml");
    s3_http_response_printf(
        resp,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ListAllMyBucketsResult "
        "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Owner><ID>ernic</ID><DisplayName>rocm-ernic</DisplayName></Owner>"
        "<Buckets><Bucket><Name>%s</Name><CreationDate>%s</CreationDate>"
        "</Bucket></Buckets></ListAllMyBucketsResult>\n",
        t->cfg.bucket, when);
}

static void handle_list_objects(struct s3_target *t,
                                const struct s3_http_request *req,
                                struct s3_http_response *resp)
{
    char prefix[S3_KEY_MAX] = "";
    char maxkeys[24] = "";
    unsigned long limit = S3_LIST_MAX_KEYS;

    s3_http_query_get(req, "prefix", prefix, sizeof(prefix));
    if (s3_http_query_get(req, "max-keys", maxkeys, sizeof(maxkeys)) &&
        maxkeys[0] != '\0') {
        char *end;
        unsigned long v = strtoul(maxkeys, &end, 10);
        if (*end == '\0' && v < limit) {
            limit = v;
        }
    }

    /* Two passes: KeyCount and IsTruncated have to precede Contents. */
    unsigned matched = 0;
    for (unsigned i = 0; i < t->obj_cap; i++) {
        struct s3_object *o = &t->objs[i];
        if (o->in_use && strncmp(o->key, prefix, strlen(prefix)) == 0) {
            matched++;
        }
    }
    unsigned listed = matched < limit ? matched : (unsigned)limit;

    s3_http_response_header(resp, "Content-Type", "application/xml");
    s3_http_response_printf(
        resp,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Name>%s</Name><Prefix>",
        t->cfg.bucket);
    xml_escape(resp, prefix);
    s3_http_response_printf(resp,
                            "</Prefix><KeyCount>%u</KeyCount>"
                            "<MaxKeys>%lu</MaxKeys>"
                            "<IsTruncated>%s</IsTruncated>",
                            listed, limit, matched > listed ? "true" : "false");

    unsigned n = 0;
    for (unsigned i = 0; i < t->obj_cap && n < listed; i++) {
        struct s3_object *o = &t->objs[i];
        if (!o->in_use || strncmp(o->key, prefix, strlen(prefix)) != 0) {
            continue;
        }
        char when[32];
        iso8601(o->mtime, when, sizeof(when));
        s3_http_response_write(resp, "<Contents><Key>", 15);
        xml_escape(resp, o->key);
        s3_http_response_printf(resp,
                                "</Key><LastModified>%s</LastModified>"
                                "<ETag>&quot;%.*s&quot;</ETag>"
                                "<Size>%zu</Size>"
                                "<StorageClass>STANDARD</StorageClass>"
                                "</Contents>",
                                when, (int)strlen(o->etag) - 2, o->etag + 1,
                                o->len);
        n++;
    }
    s3_http_response_write(resp, "</ListBucketResult>\n", 20);
}

static void handle_head(struct s3_target *t, const char *key,
                        struct s3_http_response *resp)
{
    struct s3_object *o = obj_find(t, key);

    if (o == NULL) {
        s3_error(t, resp, 404, "NoSuchKey", "The specified key does not exist.",
                 key);
        resp->body_len = 0; /* a HEAD carries no body, not even an error one */
        return;
    }
    char when[32];
    iso8601(o->mtime, when, sizeof(when));
    s3_http_response_header(resp, "Content-Type", "application/octet-stream");
    s3_http_response_header(resp, "ETag", "%s", o->etag);
    s3_http_response_header(resp, "Last-Modified", "%s", when);
    s3_http_response_header(resp, "x-amz-content-length", "%zu", o->len);
    s3_http_response_header(resp, "Accept-Ranges", "bytes");
}

static void handle_get(struct s3_target *t, const struct s3_http_request *req,
                       const char *key, const struct s3_dma_ops *dma,
                       void *dma_ctx, struct s3_http_response *resp)
{
    struct s3_token tok;
    bool malformed;
    bool rdma = have_token(req, &tok, &malformed);

    if (malformed) {
        s3_error(t, resp, 400, "InvalidArgument",
                 "x-amz-rdma-token is not a valid RDMA token.", key);
        return;
    }

    struct s3_object *o = obj_find(t, key);
    if (o == NULL) {
        s3_error(t, resp, 404, "NoSuchKey", "The specified key does not exist.",
                 key);
        if (rdma) {
            rdma_reply(t, resp, 404, &tok, 0);
        }
        return;
    }

    size_t off = 0, len = o->len;
    bool ranged = false;
    const char *range = s3_http_header_get(req, "range");
    if (range != NULL) {
        if (!parse_range(range, o->len, &off, &len)) {
            s3_error(t, resp, 416, "InvalidRange",
                     "The requested range is not satisfiable.", key);
            if (rdma) {
                rdma_reply(t, resp, 416, &tok, 0);
            }
            return;
        }
        ranged = true;
    }

    if (!rdma) {
        resp->status = ranged ? 206 : 200;
        s3_http_response_header(resp, "Content-Type",
                                "application/octet-stream");
        s3_http_response_header(resp, "ETag", "%s", o->etag);
        s3_http_response_header(resp, "Accept-Ranges", "bytes");
        if (ranged) {
            s3_http_response_header(resp, "Content-Range", "bytes %zu-%zu/%zu",
                                    off, off + len - 1, o->len);
        }
        s3_http_response_write(resp, o->data + off, len);
        t->stats.gets++;
        t->stats.read_bytes += len;
        return;
    }

    if (dma == NULL || dma->to_host == NULL) {
        s3_error(t, resp, 501, "NotImplemented",
                 "This endpoint has no RDMA data plane attached.", key);
        rdma_reply(t, resp, 501, &tok, 0);
        return;
    }

    /* The token's length is the client's buffer, not a request for that
     * many bytes: a short object is a short transfer, not an error. */
    if (tok.length != 0 && len > tok.length) {
        len = (size_t)tok.length;
    }
    if (len > t->cfg.max_part) {
        s3_error(t, resp, 400, "EntityTooLarge",
                 "The transfer exceeds the configured maximum.", key);
        rdma_reply(t, resp, 400, &tok, 0);
        return;
    }

    uint32_t moved = 0;
    if (len > 0) {
        moved = dma->to_host(dma_ctx, tok.rkey, tok.remote_addr, o->data + off,
                             (uint32_t)len);
    }
    if (moved != len) {
        s3_error(t, resp, 500, "InternalError",
                 "The RDMA write to the client buffer did not complete.", key);
        rdma_reply(t, resp, 500, &tok, moved);
        return;
    }

    resp->status = ranged ? 206 : 200;
    s3_http_response_header(resp, "ETag", "%s", o->etag);
    s3_http_response_header(resp, "Accept-Ranges", "bytes");
    if (ranged) {
        s3_http_response_header(resp, "Content-Range", "bytes %zu-%zu/%zu", off,
                                off + len - 1, o->len);
    }
    rdma_reply(t, resp, resp->status, &tok, len);
    t->stats.gets++;
    t->stats.read_bytes += len;
}

static void handle_put(struct s3_target *t, const struct s3_http_request *req,
                       const char *key, const struct s3_dma_ops *dma,
                       void *dma_ctx, struct s3_http_response *resp)
{
    struct s3_token tok;
    bool malformed;
    bool rdma = have_token(req, &tok, &malformed);

    if (malformed) {
        s3_error(t, resp, 400, "InvalidArgument",
                 "x-amz-rdma-token is not a valid RDMA token.", key);
        return;
    }

    size_t len;
    uint8_t *buf;

    if (rdma) {
        if (dma == NULL || dma->from_host == NULL) {
            s3_error(t, resp, 501, "NotImplemented",
                     "This endpoint has no RDMA data plane attached.", key);
            rdma_reply(t, resp, 501, &tok, 0);
            return;
        }
        /* Content-Length wins when the client set one, because a token
         * minted over a large reusable buffer says nothing about how much
         * of it this particular object occupies. */
        len =
            req->content_length > 0 ? req->content_length : (size_t)tok.length;
        if (len == 0) {
            s3_error(t, resp, 411, "MissingContentLength",
                     "An RDMA PUT needs a length in the token or the header.",
                     key);
            rdma_reply(t, resp, 411, &tok, 0);
            return;
        }
        if (len > t->cfg.max_part || (tok.length != 0 && len > tok.length)) {
            s3_error(t, resp, 400, "EntityTooLarge",
                     "The transfer exceeds the client buffer or the "
                     "configured maximum.",
                     key);
            rdma_reply(t, resp, 400, &tok, 0);
            return;
        }
        buf = malloc(len);
        if (buf == NULL) {
            s3_error(t, resp, 500, "InternalError", "Out of memory.", key);
            rdma_reply(t, resp, 500, &tok, 0);
            return;
        }
        uint32_t moved = dma->from_host(dma_ctx, tok.rkey, tok.remote_addr, buf,
                                        (uint32_t)len);
        if (moved != len) {
            free(buf);
            s3_error(t, resp, 500, "InternalError",
                     "The RDMA read from the client buffer did not complete.",
                     key);
            rdma_reply(t, resp, 500, &tok, moved);
            return;
        }
    } else {
        len = req->body_len;
        buf = malloc(len > 0 ? len : 1);
        if (buf == NULL) {
            s3_error(t, resp, 500, "InternalError", "Out of memory.", key);
            return;
        }
        if (len > 0) {
            memcpy(buf, req->body, len);
        }
    }

    /* A part upload holds its bytes aside until the upload completes. */
    char upload_id[32] = "";
    char part_str[16] = "";
    if (s3_http_query_get(req, "uploadId", upload_id, sizeof(upload_id))) {
        struct s3_upload *u = upload_find(t, upload_id);
        uint32_t number = 0;

        if (!s3_http_query_get(req, "partNumber", part_str, sizeof(part_str)) ||
            !parse_u32(part_str, &number) || number == 0 ||
            number > S3_MAX_PARTS) {
            free(buf);
            s3_error(t, resp, 400, "InvalidPart", "partNumber must be 1..1024.",
                     key);
            if (rdma) {
                rdma_reply(t, resp, 400, &tok, 0);
            }
            return;
        }
        if (u == NULL || strcmp(u->key, key) != 0) {
            free(buf);
            s3_error(t, resp, 404, "NoSuchUpload",
                     "The specified multipart upload does not exist.", key);
            if (rdma) {
                rdma_reply(t, resp, 404, &tok, 0);
            }
            return;
        }
        struct s3_part *p = part_slot(u, number);
        if (p == NULL || t->used + u->bytes - p->len + len > t->cfg.capacity) {
            free(buf);
            s3_error(t, resp, 507, "EntityTooLarge",
                     "The store has no room for this part.", key);
            if (rdma) {
                rdma_reply(t, resp, 507, &tok, 0);
            }
            return;
        }
        u->bytes = u->bytes - p->len + len;
        free(p->data);
        p->data = buf;
        p->len = len;
        md5_buf(buf, len, p->md5);

        char hex[33];
        md5_hex(p->md5, hex);
        s3_http_response_header(resp, "ETag", "\"%s\"", hex);
        if (rdma) {
            rdma_reply(t, resp, 200, &tok, len);
        }
        t->stats.puts++;
        t->stats.write_bytes += len;
        return;
    }

    struct s3_object *o = obj_store(t, key, buf, len, NULL);
    if (o == NULL) {
        free(buf);
        s3_error(t, resp, 507, "EntityTooLarge",
                 "The store is full or has no free object slot.", key);
        if (rdma) {
            rdma_reply(t, resp, 507, &tok, 0);
        }
        return;
    }

    s3_http_response_header(resp, "ETag", "%s", o->etag);
    if (rdma) {
        rdma_reply(t, resp, 200, &tok, len);
    }
    t->stats.puts++;
    t->stats.write_bytes += len;
}

static void handle_delete(struct s3_target *t,
                          const struct s3_http_request *req, const char *key,
                          struct s3_http_response *resp)
{
    char upload_id[32] = "";

    if (s3_http_query_get(req, "uploadId", upload_id, sizeof(upload_id))) {
        struct s3_upload *u = upload_find(t, upload_id);
        if (u == NULL) {
            s3_error(t, resp, 404, "NoSuchUpload",
                     "The specified multipart upload does not exist.", key);
            return;
        }
        upload_reset(u);
        resp->status = 204;
        return;
    }

    struct s3_object *o = obj_find(t, key);
    if (o != NULL) {
        obj_release(t, o);
        t->stats.deletes++;
    }
    /* S3 deletes are idempotent: a missing key is still a 204. */
    resp->status = 204;
}

static void handle_initiate(struct s3_target *t, const char *key,
                            struct s3_http_response *resp)
{
    struct s3_upload *u = upload_alloc(t, key);

    if (u == NULL) {
        s3_error(t, resp, 503, "SlowDown",
                 "Too many multipart uploads are in flight.", key);
        return;
    }
    s3_http_response_header(resp, "Content-Type", "application/xml");
    s3_http_response_printf(resp,
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<InitiateMultipartUploadResult "
                            "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                            "<Bucket>%s</Bucket><Key>",
                            t->cfg.bucket);
    xml_escape(resp, key);
    s3_http_response_printf(resp,
                            "</Key><UploadId>%s</UploadId>"
                            "</InitiateMultipartUploadResult>\n",
                            u->id);
}

static void handle_complete(struct s3_target *t, const char *key,
                            const char *upload_id,
                            struct s3_http_response *resp)
{
    struct s3_upload *u = upload_find(t, upload_id);

    if (u == NULL || strcmp(u->key, key) != 0) {
        s3_error(t, resp, 404, "NoSuchUpload",
                 "The specified multipart upload does not exist.", key);
        return;
    }
    if (u->nparts == 0) {
        s3_error(t, resp, 400, "InvalidRequest",
                 "A multipart upload must have at least one part.", key);
        return;
    }

    qsort(u->parts, u->nparts, sizeof(u->parts[0]), part_cmp);

    uint8_t *buf = malloc(u->bytes > 0 ? u->bytes : 1);
    if (buf == NULL) {
        s3_error(t, resp, 500, "InternalError", "Out of memory.", key);
        return;
    }

    /* The ETag of a multipart object is the digest of the concatenated
     * part digests, suffixed with the part count.  Clients check it. */
    struct md5_ctx outer;
    md5_init(&outer);
    size_t at = 0;
    for (unsigned i = 0; i < u->nparts; i++) {
        memcpy(buf + at, u->parts[i].data, u->parts[i].len);
        at += u->parts[i].len;
        md5_update(&outer, u->parts[i].md5, 16);
    }

    uint8_t digest[16];
    char hex[33], etag[S3_ETAG_MAX];
    md5_final(&outer, digest);
    md5_hex(digest, hex);
    snprintf(etag, sizeof(etag), "\"%s-%u\"", hex, u->nparts);

    struct s3_object *o = obj_store(t, key, buf, at, etag);
    if (o == NULL) {
        free(buf);
        s3_error(t, resp, 507, "EntityTooLarge",
                 "The store is full or has no free object slot.", key);
        return;
    }
    upload_reset(u);

    s3_http_response_header(resp, "Content-Type", "application/xml");
    s3_http_response_printf(resp,
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<CompleteMultipartUploadResult "
                            "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                            "<Bucket>%s</Bucket><Key>",
                            t->cfg.bucket);
    xml_escape(resp, key);
    s3_http_response_printf(resp,
                            "</Key><ETag>&quot;%.*s&quot;</ETag>"
                            "</CompleteMultipartUploadResult>\n",
                            (int)strlen(o->etag) - 2, o->etag + 1);
}

void s3_target_exec(struct s3_target *t, const struct s3_http_request *req,
                    const struct s3_dma_ops *dma, void *dma_ctx,
                    struct s3_http_response *resp)
{
    char bucket[S3_BUCKET_MAX];
    const char *key;

    s3_http_response_init(resp, 200);
    resp->keep_alive = req->keep_alive;
    s3_http_response_header(resp, "Server", "rocm-ernic/s3");
    s3_http_response_header(resp, "x-amz-request-id", "%016" PRIx64,
                            (uint64_t)++t->stats.requests);

    split_path(req->path, bucket, sizeof(bucket), &key);

    if (strlen(key) >= S3_KEY_MAX) {
        s3_error(t, resp, 400, "KeyTooLongError", "The key is too long.",
                 req->path);
        return;
    }

    bool is_get = strcmp(req->method, "GET") == 0;
    bool is_head = strcmp(req->method, "HEAD") == 0;

    if (bucket[0] == '\0') {
        if (is_get || is_head) {
            handle_list_buckets(t, resp);
            if (is_head) {
                resp->body_len = 0;
            }
        } else {
            s3_error(t, resp, 405, "MethodNotAllowed",
                     "That method is not allowed on the service endpoint.",
                     "/");
        }
        return;
    }

    if (strcmp(bucket, t->cfg.bucket) != 0) {
        s3_error(t, resp, 404, "NoSuchBucket",
                 "The specified bucket does not exist.", bucket);
        return;
    }

    if (key[0] == '\0') {
        if (is_head) {
            return; /* bucket exists; HEAD says so with an empty 200 */
        }
        if (is_get) {
            handle_list_objects(t, req, resp);
            return;
        }
        s3_error(t, resp, 405, "MethodNotAllowed",
                 "That method is not allowed on a bucket.", bucket);
        return;
    }

    if (is_get) {
        handle_get(t, req, key, dma, dma_ctx, resp);
    } else if (is_head) {
        handle_head(t, key, resp);
    } else if (strcmp(req->method, "PUT") == 0) {
        handle_put(t, req, key, dma, dma_ctx, resp);
    } else if (strcmp(req->method, "DELETE") == 0) {
        handle_delete(t, req, key, resp);
    } else if (strcmp(req->method, "POST") == 0) {
        char upload_id[32] = "";
        if (s3_http_query_get(req, "uploads", NULL, 0)) {
            handle_initiate(t, key, resp);
        } else if (s3_http_query_get(req, "uploadId", upload_id,
                                     sizeof(upload_id))) {
            handle_complete(t, key, upload_id, resp);
        } else {
            s3_error(t, resp, 400, "InvalidRequest",
                     "POST needs ?uploads or ?uploadId.", key);
        }
    } else {
        s3_error(t, resp, 405, "MethodNotAllowed",
                 "That method is not allowed on an object.", key);
    }
}
