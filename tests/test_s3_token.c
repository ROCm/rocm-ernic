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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "s3/s3_token.h"

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

/*
 * The image above is a literal, so its length is fixed when this file is
 * compiled rather than when the test runs: check it here, where a hand-edit
 * that drops or doubles a byte stops the build instead of failing a run.
 * sizeof counts the NUL, hence the -1.
 */
_Static_assert(sizeof(REF_HEX) - 1 == S3_TOKEN_HEX_LEN,
               "the reference token image is not S3_TOKEN_HEX_LEN chars");

static const uint8_t REF_GID[16] = {0, 0, 0,    0,    0,    0,    0,    0,
                                    0, 0, 0xff, 0xff, 0xc0, 0xa8, 0xc8, 0x09};

static void test_decode_reference(void)
{
    const char *name = "decode-reference";
    struct s3_token t;

    if (!s3_token_decode(REF_HEX, &t)) {
        fail(name, "decode rejected the reference token");
        return;
    }
    if (t.transport != S3_TRANSPORT_RC)
        fail(name, "transport %u, want %u", t.transport,
             (unsigned)S3_TRANSPORT_RC);
    if (t.qp_num != 0x00123456u)
        fail(name, "qp_num 0x%x, want 0x123456", t.qp_num);
    if (memcmp(t.gid, REF_GID, sizeof(REF_GID)) != 0)
        fail(name, "gid mismatch");
    if (t.rkey != 0xdeadbeefu)
        fail(name, "rkey 0x%x, want 0xdeadbeef", t.rkey);
    if (t.remote_addr != 0x00007f1122334000ull)
        fail(name, "addr 0x%" PRIx64 ", want 0x7f1122334000", t.remote_addr);
    if (t.length != 0x100000ull)
        fail(name, "length 0x%" PRIx64 ", want 0x100000", t.length);
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
        fail(name, "accepted a non-hex low nibble");

    memcpy(bad, REF_HEX, sizeof(bad));
    bad[16] = 'g';
    if (s3_token_decode(bad, &t))
        fail(name, "accepted a non-hex high nibble");

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
        fail(name, "addr override not applied: 0x%" PRIx64, t.remote_addr);
    if (t.length != 0x40000ull)
        fail(name, "length override not applied: 0x%" PRIx64, t.length);
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

    /* A token of the right length that isn't hex. */
    snprintf(hdr, sizeof(hdr), "%.*sg:1000:40000", S3_TOKEN_HEX_LEN - 1,
             REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a non-hex token in the three-field form");

    snprintf(hdr, sizeof(hdr), "%s::40000", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted an empty address override");

    snprintf(hdr, sizeof(hdr), "%s:1000:", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted an empty length override");

    snprintf(hdr, sizeof(hdr), "%s:1000:4z", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a non-hex length override");

    /* 64 bits is 16 hex digits; a 17th would be lost off the top. */
    snprintf(hdr, sizeof(hdr), "%s:10000000000000000:40000", REF_HEX);
    if (s3_token_parse_header(hdr, &t))
        fail(name, "accepted a 17-digit address override");

    snprintf(hdr, sizeof(hdr), "%s:ffffffffffffffff:FFFFFFFFFFFFFFFF", REF_HEX);
    if (!s3_token_parse_header(hdr, &t))
        fail(name, "rejected 16-digit overrides");
    else if (t.remote_addr != UINT64_MAX || t.length != UINT64_MAX)
        fail(name, "16-digit overrides read as 0x%" PRIx64 " and 0x%" PRIx64,
             t.remote_addr, t.length);

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
        {"ok", true, 200},
        {"err", true, -1},
        {"200", true, 200},
        {"404", true, 404},
        {"204\r\n", true, 204},
        {"", false, 0},
        {"2000", false, 0},
        {"20x", false, 0},
        /* A code with the peer token behind it, as parse_peer() sees. */
        {"404:", true, 404},
        {"\r\n", false, 0},
        {"abc", false, 0},
        {":200", false, 0},
        /* Near misses of "ok" and "err" are read as codes, and fail. */
        {"o", false, 0},
        {"ox", false, 0},
        {"ex", false, 0},
        {"erx", false, 0},
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

    /* The NUL a header value may arrive with is trimmed like CR and LF. */
    code = 0;
    if (!s3_reply_parse_code("201", 4, &code) || code != 201)
        fail(name, "\"201\" with its NUL: code %d, want 201", code);

    if (s3_reply_parse_code(NULL, 3, &code))
        fail(name, "accepted NULL");

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

static void test_reply_peer_rejects(void)
{
    const char *name = "reply-peer-rejects";
    char hex[S3_TOKEN_HEX_LEN + 1];
    char reply[S3_REPLY_MAX + 8];
    struct s3_token back;
    int code = 0;

    memcpy(hex, REF_HEX, sizeof(hex));

    if (s3_reply_parse_peer(NULL, 10, &back, &code))
        fail(name, "accepted NULL");
    if (s3_reply_parse_peer("\r\n", 2, &back, &code))
        fail(name, "accepted a reply of only CR LF");

    snprintf(reply, sizeof(reply), ":%s", hex);
    if (s3_reply_parse_peer(reply, strlen(reply), &back, &code))
        fail(name, "accepted a token with no code");

    snprintf(reply, sizeof(reply), "2x:%s", hex);
    if (s3_reply_parse_peer(reply, strlen(reply), &back, &code))
        fail(name, "accepted a code that isn't a number");

    snprintf(reply, sizeof(reply), "200:%.*s", S3_TOKEN_HEX_LEN - 1, hex);
    if (s3_reply_parse_peer(reply, strlen(reply), &back, &code))
        fail(name, "accepted a token one character short");

    snprintf(reply, sizeof(reply), "200:%s0", hex);
    if (s3_reply_parse_peer(reply, strlen(reply), &back, &code))
        fail(name, "accepted a token one character long");

    hex[0] = 'x';
    snprintf(reply, sizeof(reply), "200:%s", hex);
    if (s3_reply_parse_peer(reply, strlen(reply), &back, &code))
        fail(name, "accepted a token that isn't hex");

    ok(name);
}

/*
 * Two limits bound the code s3_reply_format() can send, and they are not
 * the same.  The parser reads back at most three digits, so only 0-999
 * survives the trip and anything else goes out as 500.  The buffer is
 * looser: S3_REPLY_MAX leaves seven bytes ahead of the token, room for
 * six digits (or a sign and five) and the colon, so a code past that
 * would have run the token off the end.  The cases sit on both sides of
 * each limit, plus INT_MIN and INT_MAX.
 *
 * The reply is written into a larger buffer whose tail holds a known
 * pattern, so an overrun past S3_REPLY_MAX fails here in any build rather
 * than only under ASan.  INT_MIN, the longest, would overrun by five.
 */
static void test_reply_format_codes(void)
{
    const char *name = "reply-format-codes";
    enum { GUARD = 16 };

    static const struct {
        int in;
        int want;
    } cases[] = {
        /* Sent as given. */
        {0, 0},
        {1, 1},
        {99, 99},
        {100, 100},
        {200, 200},
        {999, 999},
        /* Would fit the buffer, but not the parser. */
        {-1, 500},
        {1000, 500},
        {-99999, 500},
        {999999, 500},
        /* Would fit neither. */
        {-100000, 500},
        {1000000, 500},
        {INT_MIN, 500},
        {INT_MAX, 500},
    };

    struct s3_token peer, back;
    char hex[S3_TOKEN_HEX_LEN + 1];

    if (!s3_token_decode(REF_HEX, &peer)) {
        fail(name, "decode rejected the reference token");
        return;
    }
    s3_token_encode(&peer, hex);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int in = cases[i].in;
        char buf[S3_REPLY_MAX + GUARD];
        char want[sizeof(buf)];
        int code = 0;

        memset(buf, '#', sizeof(buf));
        s3_reply_format(in, &peer, buf);

        bool overran = false;
        for (size_t j = S3_REPLY_MAX; j < sizeof(buf); j++)
            overran = overran || buf[j] != '#';
        if (overran) {
            fail(name, "code %d: wrote past S3_REPLY_MAX", in);
            continue;
        }
        /* Comparing the NUL too means @buf is a string once this passes. */
        snprintf(want, sizeof(want), "%d:%s", cases[i].want, hex);
        if (memcmp(buf, want, strlen(want) + 1) != 0) {
            fail(name, "code %d: formatted\n  %.*s\nwant\n  %s", in,
                 S3_REPLY_MAX, buf, want);
            continue;
        }

        if (!s3_reply_parse_peer(buf, strlen(buf), &back, &code)) {
            fail(name, "code %d: could not parse \"%s\"", in, buf);
            continue;
        }
        if (code != cases[i].want)
            fail(name, "code %d: read back as %d, want %d", in, code,
                 cases[i].want);
        if (memcmp(&back, &peer, sizeof(peer)) != 0)
            fail(name, "code %d: peer token did not survive the round trip",
                 in);
    }

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

    /* Both 0xff bytes are needed, not just the first. */
    t.gid[11] = 0;
    if (s3_token_gid_ipv4(&t, dotted, sizeof(dotted)))
        fail(name, "took ::ff00:c0a8:c809 for an IPv4-mapped GID");

    memcpy(t.gid, REF_GID, sizeof(REF_GID));
    if (s3_token_gid_ipv4(&t, NULL, sizeof(dotted)))
        fail(name, "accepted a NULL output buffer");
    dotted[0] = 'Z';
    if (s3_token_gid_ipv4(&t, dotted, 0))
        fail(name, "accepted an output buffer of length 0");
    if (dotted[0] != 'Z')
        fail(name, "wrote into an output buffer of length 0");

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
    test_reply_peer_rejects();
    test_reply_format_codes();
    test_gid_ipv4();

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("\nall s3 token tests passed\n");
    return failures;
}
