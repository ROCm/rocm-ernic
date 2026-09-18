/*
 * s3_token.c — the x-amz-rdma-token wire format
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3_token.h"

static const char HEX[] = "0123456789abcdef";

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

/*
 * Unsigned throughout: on plain signed char the subtractions below are
 * signed int arithmetic, and -Wstrict-overflow=4 refuses to let gcc
 * fold the range tests without saying so.
 */
static int hex_nibble(char c)
{
    unsigned u = (unsigned char)c;

    if (u >= '0' && u <= '9')
        return (int)(u - '0');
    if (u >= 'a' && u <= 'f')
        return (int)(u - 'a' + 10u);
    if (u >= 'A' && u <= 'F')
        return (int)(u - 'A' + 10u);
    return -1;
}

void s3_token_encode(const struct s3_token *tok, char *out)
{
    uint8_t buf[S3_TOKEN_BIN_LEN];

    buf[0] = tok->transport;
    put_le32(buf + 1, tok->qp_num);
    memcpy(buf + 5, tok->gid, 16);
    put_le32(buf + 21, tok->rkey);
    put_le64(buf + 25, tok->remote_addr);
    put_le64(buf + 33, tok->length);
    buf[41] = tok->port_num;
    put_le16(buf + 42, tok->lid);

    for (size_t i = 0; i < sizeof(buf); i++) {
        out[i * 2] = HEX[(buf[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEX[buf[i] & 0xf];
    }
    out[S3_TOKEN_HEX_LEN] = '\0';
}

bool s3_token_decode(const char *hex, struct s3_token *out)
{
    if (hex == NULL || strlen(hex) != S3_TOKEN_HEX_LEN)
        return false;

    uint8_t buf[S3_TOKEN_BIN_LEN];
    for (size_t i = 0; i < sizeof(buf); i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }

    memset(out, 0, sizeof(*out));
    out->transport = buf[0];
    out->qp_num = get_le32(buf + 1);
    memcpy(out->gid, buf + 5, 16);
    out->rkey = get_le32(buf + 21);
    out->remote_addr = get_le64(buf + 25);
    out->length = get_le64(buf + 33);
    out->port_num = buf[41];
    out->lid = get_le16(buf + 42);
    return true;
}

/* strtoull on a bounded, NUL-free slice without copying it out. */
static bool parse_hex_u64(const char *p, size_t len, uint64_t *out)
{
    if (len == 0 || len > 16)
        return false;

    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        int n = hex_nibble(p[i]);
        if (n < 0)
            return false;
        v = (v << 4) | (uint64_t)n;
    }
    *out = v;
    return true;
}

bool s3_token_parse_header(const char *value, struct s3_token *out)
{
    if (value == NULL)
        return false;

    const char *c1 = strchr(value, ':');
    if (c1 == NULL)
        return s3_token_decode(value, out);

    /* "<token>:<addr>:<len>" -- both suffix fields are required once the
     * first colon is present, so a truncated header is a bad request
     * rather than a token that silently loses its overrides. */
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 == NULL)
        return false;

    if ((size_t)(c1 - value) != S3_TOKEN_HEX_LEN)
        return false;

    char hex[S3_TOKEN_HEX_LEN + 1];
    memcpy(hex, value, S3_TOKEN_HEX_LEN);
    hex[S3_TOKEN_HEX_LEN] = '\0';
    if (!s3_token_decode(hex, out))
        return false;

    uint64_t addr = 0, len = 0;
    if (!parse_hex_u64(c1 + 1, (size_t)(c2 - c1 - 1), &addr) ||
        !parse_hex_u64(c2 + 1, strlen(c2 + 1), &len))
        return false;

    /* Zero means "no override": hipObject sends the token's own values
     * back in these fields when it has nothing more specific to say. */
    if (addr != 0)
        out->remote_addr = addr;
    if (len != 0)
        out->length = len;
    return true;
}

/* Strip the trailing NUL, CR and LF a header value may arrive with. */
static size_t trim(const char *s, size_t len)
{
    while (len > 0 &&
           (s[len - 1] == '\0' || s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    return len;
}

bool s3_reply_parse_code(const char *reply, size_t len, int *http_code)
{
    if (reply == NULL)
        return false;
    len = trim(reply, len);
    if (len == 0)
        return false;

    if (len >= 2 && reply[0] == 'o' && reply[1] == 'k') {
        *http_code = 200;
        return true;
    }
    if (len >= 3 && reply[0] == 'e' && reply[1] == 'r' && reply[2] == 'r') {
        *http_code = -1;
        return true;
    }

    /* A decimal status, optionally followed by ":<peer token>". */
    size_t n = 0;
    unsigned code = 0;
    while (n < len) {
        unsigned digit = (unsigned char)reply[n] - (unsigned)'0';
        if (digit > 9)
            break;
        code = code * 10u + digit;
        if (code > 999u)
            return false;
        n++;
    }
    if (n == 0 || (n != len && reply[n] != ':'))
        return false;

    *http_code = (int)code;
    return true;
}

bool s3_reply_parse_peer(const char *reply, size_t len, struct s3_token *peer,
                         int *http_code)
{
    if (reply == NULL)
        return false;
    len = trim(reply, len);
    if (len == 0)
        return false;

    const char *colon = memchr(reply, ':', len);
    if (colon == NULL || colon == reply)
        return false;

    if (!s3_reply_parse_code(reply, (size_t)(colon - reply), http_code))
        return false;

    size_t hexlen = len - (size_t)(colon - reply) - 1;
    if (hexlen != S3_TOKEN_HEX_LEN)
        return false;

    char hex[S3_TOKEN_HEX_LEN + 1];
    memcpy(hex, colon + 1, S3_TOKEN_HEX_LEN);
    hex[S3_TOKEN_HEX_LEN] = '\0';
    return s3_token_decode(hex, peer);
}

void s3_reply_format(int http_code, const struct s3_token *peer, char *out)
{
    int n = snprintf(out, S3_REPLY_MAX, "%d:", http_code);
    s3_token_encode(peer, out + n);
}

bool s3_token_gid_ipv4(const struct s3_token *tok, char *out, size_t outlen)
{
    if (out == NULL || outlen == 0)
        return false;
    out[0] = '\0';

    /* RoCEv2 maps IPv4 into ::ffff:a.b.c.d, so the two 0xff bytes are
     * what says the last four are an address rather than a prefix. */
    if (tok->gid[10] != 0xff || tok->gid[11] != 0xff)
        return false;

    char dotted[16];
    int n = snprintf(dotted, sizeof(dotted), "%u.%u.%u.%u", tok->gid[12],
                     tok->gid[13], tok->gid[14], tok->gid[15]);
    if (n <= 0 || (size_t)n >= outlen)
        return false;

    memcpy(out, dotted, (size_t)n + 1);
    return true;
}
