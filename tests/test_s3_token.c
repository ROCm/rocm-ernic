/*
 * test_s3_token.c — unit tests for the x-amz-rdma-token wire format
 *
 * The encoding is a contract with a client we do not build here, so the
 * tests pin the bytes rather than the round trip: a hand-assembled
 * 44-byte image is decoded field by field, and a token encoded from
 * known fields is compared against the hex a hipObject client would have
 * put on the wire. Round-trip alone would pass happily with the fields
 * in the wrong order or the wrong endianness on both sides.
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
#include <string.h>

#include "s3_token.h"

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

/*
 * The reference image: transport RC, qp 0x00123456, an IPv4-mapped GID
 * for 192.168.200.9, rkey 0xdeadbeef, addr 0x00007f1122334000, length
 * 0x0000000000100000, port 1, lid 0.  Written out little-endian by hand
 * so the test is independent of the code it checks.
 */
static const char REF_HEX[] =
    "01"                               /* transport = RC */
    "56341200"                         /* qp_num    = 0x00123456 */
    "00000000000000000000ffffc0a8c809" /* gid       = ::ffff:192.168.200.9 */
    "efbeadde"                         /* rkey      = 0xdeadbeef */
    "00403322117f0000"                 /* addr      = 0x00007f1122334000 */
    "0000100000000000"                 /* length    = 0x100000 */
    "01"                               /* port_num  = 1 */
    "0000";                            /* lid       = 0 */

static const uint8_t REF_GID[16] = {0, 0, 0,    0,    0,    0,    0,    0,
                                    0, 0, 0xff, 0xff, 0xc0, 0xa8, 0xc8, 0x09};

static void test_decode_reference(void)
{
    const char *name = "decode-reference";
    struct s3_token t;

    if (strlen(REF_HEX) != S3_TOKEN_HEX_LEN) {
        fail(name, "reference is %zu chars, want %d", strlen(REF_HEX),
             S3_TOKEN_HEX_LEN);
        return;
    }
    if (!s3_token_decode(REF_HEX, &t)) {
        fail(name, "decode rejected the reference token");
        return;
    }
    if (t.transport != S3_TRANSPORT_RC)
        fail(name, "transport %u, want %u", t.transport, S3_TRANSPORT_RC);
    if (t.qp_num != 0x00123456u)
        fail(name, "qp_num 0x%x, want 0x123456", t.qp_num);
    if (memcmp(t.gid, REF_GID, sizeof(REF_GID)) != 0)
        fail(name, "gid mismatch");
    if (t.rkey != 0xdeadbeefu)
        fail(name, "rkey 0x%x, want 0xdeadbeef", t.rkey);
    if (t.remote_addr != 0x00007f1122334000ull)
        fail(name, "addr 0x%llx, want 0x7f1122334000",
             (unsigned long long)t.remote_addr);
    if (t.length != 0x100000ull)
        fail(name, "length 0x%llx, want 0x100000",
             (unsigned long long)t.length);
    if (t.port_num != 1)
        fail(name, "port_num %u, want 1", t.port_num);
    if (t.lid != 0)
        fail(name, "lid %u, want 0", t.lid);

    ok(name);
}

static void test_encode_reference(void)
{
    const char *name = "encode-reference";
    struct s3_token t;
    char hex[S3_TOKEN_HEX_LEN + 1];

    memset(&t, 0, sizeof(t));
    t.transport = S3_TRANSPORT_RC;
    t.qp_num = 0x00123456u;
    memcpy(t.gid, REF_GID, sizeof(REF_GID));
    t.rkey = 0xdeadbeefu;
    t.remote_addr = 0x00007f1122334000ull;
    t.length = 0x100000ull;
    t.port_num = 1;
    t.lid = 0;

    s3_token_encode(&t, hex);
    if (strcmp(hex, REF_HEX) != 0)
        fail(name, "encoded\n  %s\nwant\n  %s", hex, REF_HEX);
    else
        ok(name);
}

static void test_decode_rejects(void)
{
    const char *name = "decode-rejects";
    struct s3_token t;
    char shortened[S3_TOKEN_HEX_LEN];
    char bad[S3_TOKEN_HEX_LEN + 1];

    if (s3_token_decode(NULL, &t))
        fail(name, "accepted NULL");

    memcpy(shortened, REF_HEX, sizeof(shortened) - 1);
    shortened[sizeof(shortened) - 1] = '\0';
    if (s3_token_decode(shortened, &t))
        fail(name, "accepted a token one character short");

    memcpy(bad, REF_HEX, sizeof(bad));
    bad[17] = 'g';
    if (s3_token_decode(bad, &t))
        fail(name, "accepted a non-hex character");

    ok(name);
}

/* hipObject's formatRdmaHeaderValue(): "<token>:<addr-hex>:<len-hex>". */
static void test_parse_header_three_field(void)
{
    const char *name = "parse-header-three-field";
    struct s3_token t;
    char hdr[S3_TOKEN_HEX_LEN + 64];

    snprintf(hdr, sizeof(hdr), "%s:7f2000000000:40000", REF_HEX);
    if (!s3_token_parse_header(hdr, &t)) {
        fail(name, "rejected a well-formed three-field header");
        return;
    }
    if (t.remote_addr != 0x7f2000000000ull)
        fail(name, "addr override not applied: 0x%llx",
             (unsigned long long)t.remote_addr);
    if (t.length != 0x40000ull)
        fail(name, "length override not applied: 0x%llx",
             (unsigned long long)t.length);
    if (t.rkey != 0xdeadbeefu)
        fail(name, "overrides clobbered rkey");

    /* Zero in either field means "keep what the token said". */
    snprintf(hdr, sizeof(hdr), "%s:0:0", REF_HEX);
    if (!s3_token_parse_header(hdr, &t)) {
        fail(name, "rejected zero overrides");
        return;
    }
    if (t.remote_addr != 0x00007f1122334000ull || t.length != 0x100000ull)
        fail(name, "zero override was applied instead of ignored");

    ok(name);
}

static void test_parse_header_bare_and_bad(void)
{
    const char *name = "parse-header-bare-and-bad";
    struct s3_token t;
    char hdr[S3_TOKEN_HEX_LEN + 64];

    if (!s3_token_parse_header(REF_HEX, &t))
        fail(name, "rejected a bare cuObject token");

    /* One colon is a truncated three-field form, not a bare token. */
    snprintf(hdr, sizeof(hdr), "%s:7f2000000000", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a header missing its length field");

    snprintf(hdr, sizeof(hdr), "%s:zz:40000", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a non-hex address override");

    snprintf(hdr, sizeof(hdr), "deadbeef:1000:40000");
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a short token in the three-field form");

    if (s3_token_parse_header(NULL, &t))
        fail(name, "accepted NULL");

    ok(name);
}

static void test_reply_codes(void)
{
    const char *name = "reply-codes";
    int code;

    static const struct {
        const char *in;
        bool want_ok;
        int want_code;
    } cases[] = {
        {"ok", true, 200},  {"err", true, -1},      {"200", true, 200},
        {"404", true, 404}, {"204\r\n", true, 204}, {"", false, 0},
        {"2000", false, 0}, {"20x", false, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        code = 0;
        bool got = s3_reply_parse_code(cases[i].in, strlen(cases[i].in), &code);
        if (got != cases[i].want_ok) {
            fail(name, "\"%s\": parse returned %d, want %d", cases[i].in, got,
                 cases[i].want_ok);
            continue;
        }
        if (got && code != cases[i].want_code)
            fail(name, "\"%s\": code %d, want %d", cases[i].in, code,
                 cases[i].want_code);
    }

    ok(name);
}

static void test_reply_peer_round_trip(void)
{
    const char *name = "reply-peer-round-trip";
    struct s3_token peer, back;
    char reply[S3_REPLY_MAX];
    int code = 0;

    memset(&peer, 0, sizeof(peer));
    peer.transport = S3_TRANSPORT_RC;
    peer.qp_num = 0x00d00001u;
    memcpy(peer.gid, REF_GID, sizeof(REF_GID));
    peer.gid[15] = 0x01;
    peer.rkey = 0x1234u;
    peer.remote_addr = 0x9000ull;
    peer.length = 4096;
    peer.port_num = 1;

    s3_reply_format(200, &peer, reply);
    if (strlen(reply) != 4 + S3_TOKEN_HEX_LEN) {
        fail(name, "reply is %zu chars, want %d", strlen(reply),
             4 + S3_TOKEN_HEX_LEN);
        return;
    }
    if (!s3_reply_parse_peer(reply, strlen(reply), &back, &code)) {
        fail(name, "could not parse the reply we just formatted");
        return;
    }
    if (code != 200)
        fail(name, "code %d, want 200", code);
    if (memcmp(&back, &peer, sizeof(peer)) != 0)
        fail(name, "peer token did not survive the round trip");

    /* A reply with no token is a code, not a peer. */
    if (s3_reply_parse_peer("200", 3, &back, &code))
        fail(name, "accepted a bare code as a peer reply");

    ok(name);
}

static void test_gid_ipv4(void)
{
    const char *name = "gid-ipv4";
    struct s3_token t;
    char dotted[32];

    memset(&t, 0, sizeof(t));
    memcpy(t.gid, REF_GID, sizeof(REF_GID));
    if (!s3_token_gid_ipv4(&t, dotted, sizeof(dotted)))
        fail(name, "rejected an IPv4-mapped GID");
    else if (strcmp(dotted, "192.168.200.9") != 0)
        fail(name, "got \"%s\", want \"192.168.200.9\"", dotted);

    /* Native IPv6: not an error, just not a dotted quad. */
    t.gid[0] = 0xfe;
    t.gid[1] = 0x80;
    t.gid[10] = 0;
    t.gid[11] = 0;
    if (s3_token_gid_ipv4(&t, dotted, sizeof(dotted)))
        fail(name, "claimed a native IPv6 GID was IPv4-mapped");
    if (dotted[0] != '\0')
        fail(name, "left stale text in the output buffer");

    /* Too small to hold "192.168.200.9" plus its NUL. */
    memcpy(t.gid, REF_GID, sizeof(REF_GID));
    char tiny[8];
    if (s3_token_gid_ipv4(&t, tiny, sizeof(tiny)))
        fail(name, "overflowed a short output buffer");

    ok(name);
}

int main(void)
{
    test_decode_reference();
    test_encode_reference();
    test_decode_rejects();
    test_parse_header_three_field();
    test_parse_header_bare_and_bad();
    test_reply_codes();
    test_reply_peer_round_trip();
    test_gid_ipv4();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 token tests passed\n");
    return failures;
}
