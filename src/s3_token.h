/*
 * s3_token.h — the x-amz-rdma-token wire format
 *
 * S3 over RDMA splits the object protocol in two: an ordinary HTTP
 * control plane that names the object, and an RDMA data plane that moves
 * its bytes.  The two are tied together by a token the client mints over
 * one of its registered buffers and hands to the server in the
 * x-amz-rdma-token request header; the server answers in
 * x-amz-rdma-reply, optionally with a token of its own.
 *
 * The encoding here is byte-for-byte the one NVIDIA cuObject v1.2.0 uses
 * and ROCm/hipObject implements in src/rdma/token.cpp, so a token minted
 * by hipObject decodes here and a reply minted here parses there.  Every
 * multi-byte field is little-endian on the wire regardless of host byte
 * order, and the whole 44-byte structure travels as 88 lowercase hex
 * characters.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef S3_TOKEN_H
#define S3_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire layout, in order:
 *
 *   [0]      transport
 *   [1:4]    qp_num
 *   [5:20]   gid
 *   [21:24]  rkey
 *   [25:32]  remote_addr
 *   [33:40]  length
 *   [41]     port_num
 *   [42:43]  lid
 */
#define S3_TOKEN_BIN_LEN 44
#define S3_TOKEN_HEX_LEN (S3_TOKEN_BIN_LEN * 2)

/*
 * Transport byte.  cuObject requires Dynamic Connection on ConnectX;
 * hipObject and this emulator speak Reliable Connection.  The byte is
 * what tells the two apart, so it is checked rather than assumed.
 */
enum {
    S3_TRANSPORT_DC = 0x00,
    S3_TRANSPORT_RC = 0x01,
};

struct s3_token {
    uint32_t qp_num;
    uint8_t gid[16];
    uint32_t rkey;
    uint64_t remote_addr;
    uint64_t length;
    uint8_t transport;
    uint8_t port_num;
    uint16_t lid;
};

/*
 * Encode @tok as S3_TOKEN_HEX_LEN lowercase hex characters plus a NUL.
 * @out must have room for S3_TOKEN_HEX_LEN + 1 bytes.
 */
void s3_token_encode(const struct s3_token *tok, char *out);

/* Decode exactly S3_TOKEN_HEX_LEN hex characters.  Anything else fails. */
bool s3_token_decode(const char *hex, struct s3_token *out);

/*
 * Parse a complete x-amz-rdma-token header value.
 *
 * hipObject sends the three-field form its formatRdmaHeaderValue()
 * builds -- "<token-hex>:<addr-hex>:<len-hex>" -- where the trailing
 * pair overrides the address and length carried inside the token.  A
 * bare token with neither suffix is also accepted, because cuObject
 * clients send that.  A zero in either override means "no override",
 * which is what hipObject itself does with them.
 */
bool s3_token_parse_header(const char *value, struct s3_token *out);

/*
 * The x-amz-rdma-reply header.
 *
 * Three forms are in use.  The legacy cuObject tags "ok" and "err" map
 * to 200 and -1; a bare decimal is an HTTP status; and "<code>:<hex>"
 * carries the server's own token so an RC client can point its queue
 * pair at the peer that just served it.
 */
bool s3_reply_parse_code(const char *reply, size_t len, int *http_code);
bool s3_reply_parse_peer(const char *reply, size_t len, struct s3_token *peer,
                         int *http_code);

/*
 * Format "<code>:<token-hex>".  @out must have room for
 * S3_TOKEN_HEX_LEN + 8 bytes; S3_REPLY_MAX is that size.
 */
#define S3_REPLY_MAX (S3_TOKEN_HEX_LEN + 8)
void s3_reply_format(int http_code, const struct s3_token *peer, char *out);

/*
 * Render the IPv4 address embedded in an IPv4-mapped RoCEv2 GID as a
 * dotted quad.  Returns false when the GID is not IPv4-mapped, which is
 * not an error -- it just means the peer is on native IPv6.
 */
bool s3_token_gid_ipv4(const struct s3_token *tok, char *out, size_t outlen);

#endif /* S3_TOKEN_H */
