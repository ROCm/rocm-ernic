/*
 * s3_token.c — the x-amz-rdma-token wire format
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "byteorder.h"
#include "ernic_gid.h"
#include "net/net_headers.h"
#include "parse_int.h"
#include "s3/s3_http.h"
#include "s3/s3_token.h"

/* The largest code s3_reply_parse_code() reads, three digits */
#define S3_REPLY_CODE_MAX 999

static const char HEX[] = "0123456789abcdef";

void s3_token_encode(const struct s3_token *tok, char *out)
{
    uint8_t buf[S3_TOKEN_BIN_LEN];

    buf[S3_TOKEN_TRANSPORT_OFF] = tok->transport;
    ernic_put_le32(buf + S3_TOKEN_QP_NUM_OFF, tok->qp_num);
    memcpy(buf + S3_TOKEN_GID_OFF, tok->gid, S3_TOKEN_GID_LEN);
    ernic_put_le32(buf + S3_TOKEN_RKEY_OFF, tok->rkey);
    ernic_put_le64(buf + S3_TOKEN_REMOTE_ADDR_OFF, tok->remote_addr);
    ernic_put_le64(buf + S3_TOKEN_LENGTH_OFF, tok->length);
    buf[S3_TOKEN_PORT_NUM_OFF] = tok->port_num;
    ernic_put_le16(buf + S3_TOKEN_LID_OFF, tok->lid);

    for (size_t i = 0; i < sizeof(buf); i++) {
        out[i * 2] = HEX[(buf[i] >> 4) & 0xf];
        out[(i * 2) + 1] = HEX[buf[i] & 0xf];
    }
    out[S3_TOKEN_HEX_LEN] = '\0';
}

bool s3_token_decode(const char *hex, struct s3_token *out)
{
    if (hex == NULL || strlen(hex) != S3_TOKEN_HEX_LEN) {
        return false;
    }

    uint8_t buf[S3_TOKEN_BIN_LEN];
    if (!ernic_parse_hex(hex, sizeof(buf), buf)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->transport = buf[S3_TOKEN_TRANSPORT_OFF];
    out->qp_num = ernic_get_le32(buf + S3_TOKEN_QP_NUM_OFF);
    memcpy(out->gid, buf + S3_TOKEN_GID_OFF, S3_TOKEN_GID_LEN);
    out->rkey = ernic_get_le32(buf + S3_TOKEN_RKEY_OFF);
    out->remote_addr = ernic_get_le64(buf + S3_TOKEN_REMOTE_ADDR_OFF);
    out->length = ernic_get_le64(buf + S3_TOKEN_LENGTH_OFF);
    out->port_num = buf[S3_TOKEN_PORT_NUM_OFF];
    out->lid = ernic_get_le16(buf + S3_TOKEN_LID_OFF);
    return true;
}

bool s3_token_parse_header(const char *value, struct s3_token *out)
{
    if (value == NULL) {
        return false;
    }

    const char *c1 = strchr(value, ':');
    if (c1 == NULL) {
        return s3_token_decode(value, out);
    }

    /* "<token>:<addr>:<len>" -- both suffix fields are required once the
     * first colon is present, so a truncated header is a bad request
     * rather than a token that silently loses its overrides. */
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 == NULL) {
        return false;
    }

    if ((size_t)(c1 - value) != S3_TOKEN_HEX_LEN) {
        return false;
    }

    char hex[S3_TOKEN_HEX_LEN + 1];
    memcpy(hex, value, S3_TOKEN_HEX_LEN);
    hex[S3_TOKEN_HEX_LEN] = '\0';
    if (!s3_token_decode(hex, out)) {
        return false;
    }

    uint64_t addr = 0;
    uint64_t len = 0;
    if (!ernic_parse_hex_u64_span(c1 + 1, (size_t)(c2 - (c1 + 1)), &addr) ||
        !ernic_parse_hex_u64_span(c2 + 1, strlen(c2 + 1), &len)) {
        return false;
    }

    /* Zero means "no override": hipObject sends the token's own values
     * back in these fields when it has nothing more specific to say. */
    if (addr != 0) {
        out->remote_addr = addr;
    }
    if (len != 0) {
        out->length = len;
    }
    return true;
}

/* Strip the trailing NUL, CR and LF a header value may arrive with. */
static size_t trim(const char *s, size_t len)
{
    while (len > 0 &&
           (s[len - 1] == '\0' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        len--;
    }
    return len;
}

bool s3_reply_parse_code(const char *reply, size_t len, int *http_code)
{
    if (reply == NULL) {
        return false;
    }
    len = trim(reply, len);
    if (len == 0) {
        return false;
    }

    if (len >= 2 && reply[0] == 'o' && reply[1] == 'k') {
        *http_code = S3_HTTP_OK;
        return true;
    }
    if (len >= 3 && reply[0] == 'e' && reply[1] == 'r' && reply[2] == 'r') {
        *http_code = -1;
        return true;
    }

    /* A decimal status, optionally followed by ":<peer token>". */
    const char *colon = memchr(reply, ':', len);
    size_t n = colon != NULL ? (size_t)(colon - reply) : len;
    uint64_t code;
    if (!ernic_parse_u64_span(reply, n, &code) || code > S3_REPLY_CODE_MAX) {
        return false;
    }

    *http_code = (int)code;
    return true;
}

bool s3_reply_parse_peer(const char *reply, size_t len, struct s3_token *peer,
                         int *http_code)
{
    if (reply == NULL) {
        return false;
    }
    len = trim(reply, len);
    if (len == 0) {
        return false;
    }

    const char *colon = memchr(reply, ':', len);
    if (colon == NULL || colon == reply) {
        return false;
    }

    if (!s3_reply_parse_code(reply, (size_t)(colon - reply), http_code)) {
        return false;
    }

    size_t hexlen = len - (size_t)(colon - reply) - 1;
    if (hexlen != S3_TOKEN_HEX_LEN) {
        return false;
    }

    char hex[S3_TOKEN_HEX_LEN + 1];
    memcpy(hex, colon + 1, S3_TOKEN_HEX_LEN);
    hex[S3_TOKEN_HEX_LEN] = '\0';
    return s3_token_decode(hex, peer);
}

void s3_reply_format(int http_code, const struct s3_token *peer, char *out)
{
    /* Room for the longest code s3_reply_parse_code() reads back, its
     * colon, the token and the NUL. */
    _Static_assert(S3_REPLY_MAX >= 3 + 1 + S3_TOKEN_HEX_LEN + 1,
                   "S3_REPLY_MAX cannot hold a reply");

    /* Any other code would be unreadable to the client, and a long
     * enough one would run the token off the end of @out.  A caller
     * passing one is a server bug, which is what 500 says. */
    if (http_code < 0 || http_code > S3_REPLY_CODE_MAX) {
        http_code = S3_HTTP_INTERNAL_ERROR;
    }

    int n = snprintf(out, S3_REPLY_MAX, "%d:", http_code);
    s3_token_encode(peer, out + n);
}

bool s3_token_gid_ipv4(const struct s3_token *tok, char *out, size_t outlen)
{
    if (out == NULL || outlen == 0) {
        return false;
    }
    out[0] = '\0';

    /* RoCEv2 maps IPv4 into ::ffff:a.b.c.d, so the two 0xff bytes are
     * what says the last four are an address rather than a prefix. */
    if (tok->gid[ERNIC_GID_V4_OFF - 2] != 0xff ||
        tok->gid[ERNIC_GID_V4_OFF - 1] != 0xff) {
        return false;
    }

    char dotted[IP_ADDR_STR_LEN];
    ip_addr_str(ernic_get_be32(tok->gid + ERNIC_GID_V4_OFF), dotted);
    size_t n = strlen(dotted);
    if (n >= outlen) {
        return false;
    }

    memcpy(out, dotted, n + 1);
    return true;
}
