/*
 * Unit tests for the strict integer parsers in parse_int.h.
 *
 * These replace atoi(), the strto*() family, and sscanf(). Unlike them, they
 * must reject empty input, trailing garbage, and out-of-range values rather
 * than truncating or returning 0.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "parse_int.h"

struct int_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    bool expect_ok;
    int expect_val; /* checked only when expect_ok */
};

struct u32_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    bool expect_ok;
    uint32_t expect_val; /* checked only when expect_ok */
};

struct u64_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    bool expect_ok;
    uint64_t expect_val; /* checked only when expect_ok */
};

struct span_case {
    const char *name;
    const char *input;
    size_t len;
    bool expect_ok;
    uint64_t expect_val; /* checked only when expect_ok */
};

struct hex_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    size_t n;
    bool expect_ok;
    uint8_t expect[4]; /* the first n bytes, checked only when expect_ok */
};

struct mac_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    bool expect_ok;
    uint8_t expect[6]; /* checked only when expect_ok */
};

struct port_case {
    const char *name;
    const char *input;
    bool expect_ok;
    uint16_t expect_val;
};

static int run_int(const struct int_case *tc)
{
    int v = 0xDEAD;
    bool ok = ernic_parse_int(tc->input, &v);
    if (ok != tc->expect_ok) {
        printf("FAIL int %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && v != tc->expect_val) {
        printf("FAIL int %-16s: val=%d expected %d\n", tc->name, v,
               tc->expect_val);
        return 1;
    }
    return 0;
}

static int run_u32(const char *kind, bool (*parse)(const char *, uint32_t *),
                   const struct u32_case *tc)
{
    uint32_t v = 0xDEADBEEF;
    bool ok = parse(tc->input, &v);
    if (ok != tc->expect_ok) {
        printf("FAIL %s %-16s: ok=%d expected %d\n", kind, tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && v != tc->expect_val) {
        printf("FAIL %s %-16s: val=%" PRIu32 " expected %" PRIu32 "\n", kind,
               tc->name, v, tc->expect_val);
        return 1;
    }
    if (!ok && v != 0xDEADBEEF) {
        printf("FAIL %s %-16s: changed the output on failure\n", kind,
               tc->name);
        return 1;
    }
    return 0;
}

static int run_u64(const char *kind, bool (*parse)(const char *, uint64_t *),
                   const struct u64_case *tc)
{
    uint64_t v = UINT64_C(0xDEADBEEFDEADBEEF);
    bool ok = parse(tc->input, &v);
    if (ok != tc->expect_ok) {
        printf("FAIL %s %-16s: ok=%d expected %d\n", kind, tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && v != tc->expect_val) {
        printf("FAIL %s %-16s: val=%" PRIu64 " expected %" PRIu64 "\n", kind,
               tc->name, v, tc->expect_val);
        return 1;
    }
    if (!ok && v != UINT64_C(0xDEADBEEFDEADBEEF)) {
        printf("FAIL %s %-16s: changed the output on failure\n", kind,
               tc->name);
        return 1;
    }
    return 0;
}

static int run_span(const struct span_case *tc)
{
    uint64_t v = UINT64_C(0xDEADBEEFDEADBEEF);
    bool ok = ernic_parse_u64_span(tc->input, tc->len, &v);
    if (ok != tc->expect_ok) {
        printf("FAIL span %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && v != tc->expect_val) {
        printf("FAIL span %-16s: val=%" PRIu64 " expected %" PRIu64 "\n",
               tc->name, v, tc->expect_val);
        return 1;
    }
    if (!ok && v != UINT64_C(0xDEADBEEFDEADBEEF)) {
        printf("FAIL span %-16s: changed the output on failure\n", tc->name);
        return 1;
    }
    return 0;
}

static int run_hex(const struct hex_case *tc)
{
    uint8_t out[sizeof(tc->expect)];
    if (tc->n > sizeof(out)) {
        printf("FAIL hex %-16s: n=%zu is more than the case holds\n", tc->name,
               tc->n);
        return 1;
    }
    memset(out, 0xA5, sizeof(out));
    bool ok = ernic_parse_hex(tc->input, tc->n, out);
    if (ok != tc->expect_ok) {
        printf("FAIL hex %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && memcmp(out, tc->expect, tc->n) != 0) {
        printf("FAIL hex %-16s: wrong bytes\n", tc->name);
        return 1;
    }
    /* Past the n bytes on success, and anywhere on failure, it writes
     * nothing. */
    for (size_t i = ok ? tc->n : 0; i < sizeof(out); i++) {
        if (out[i] != 0xA5) {
            printf("FAIL hex %-16s: wrote byte %zu\n", tc->name, i);
            return 1;
        }
    }
    return 0;
}

static int run_mac(const struct mac_case *tc)
{
    uint8_t out[6];
    static const uint8_t untouched[6] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    memcpy(out, untouched, sizeof(out));
    bool ok = ernic_parse_mac(tc->input, out);
    if (ok != tc->expect_ok) {
        printf("FAIL mac %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && memcmp(out, tc->expect, sizeof(out)) != 0) {
        printf("FAIL mac %-16s: wrong bytes\n", tc->name);
        return 1;
    }
    if (!ok && memcmp(out, untouched, sizeof(out)) != 0) {
        printf("FAIL mac %-16s: changed the output on failure\n", tc->name);
        return 1;
    }
    return 0;
}

static int run_port(const struct port_case *tc)
{
    uint16_t p = 0xBEEF;
    bool ok = ernic_parse_port(tc->input, &p);
    if (ok != tc->expect_ok) {
        printf("FAIL port %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && p != tc->expect_val) {
        printf("FAIL port %-16s: val=%u expected %u\n", tc->name, (unsigned)p,
               (unsigned)tc->expect_val);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const struct int_case int_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"valid", "1234", true, 1234},
        {"zero", "0", true, 0},
        {"negative", "-5", true, -5},
        {"trailing-garbage", "123abc", false, 0},
        {"non-numeric", "abc", false, 0},
        {"trailing-space", "123 ", false, 0},
        {"overflow", "99999999999999999999", false, 0},
        {"int-max", "2147483647", true, INT_MAX},
    };

    /* gid_t and uid_t are 32 bits, and above INT_MAX is valid for both. */
    static const struct u32_case u32_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"zero", "0", true, 0},
        {"valid", "1234", true, 1234},
        {"leading-zeros", "007", true, 7},
        {"above-int-max", "3000000000", true, UINT32_C(3000000000)},
        {"max", "4294967295", true, UINT32_MAX},
        {"max-plus-one", "4294967296", false, 0},
        {"ull-overflow", "18446744073709551616", false, 0},
        {"negative", "-1", false, 0},
        {"plus-sign", "+5", false, 0},
        {"leading-space", " 5", false, 0},
        {"trailing-space", "5 ", false, 0},
        {"trailing-garbage", "12x", false, 0},
        {"non-numeric", "kvm", false, 0},
    };

    static const struct u32_case hex_u32_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"zero", "0", true, 0},
        {"lower", "1dd8", true, 0x1dd8},
        {"upper", "1DD8", true, 0x1dd8},
        {"max", "ffffffff", true, UINT32_MAX},
        {"leading-zeros", "00001002", true, 0x1002},
        {"nine-digits", "100000000", false, 0},
        {"prefix", "0x1dd8", false, 0},
        {"newline", "1dd8\n", false, 0},
        {"bad-digit", "1dg8", false, 0},
        {"negative", "-1", false, 0},
        {"leading-space", " 1", false, 0},
    };

    static const struct u64_case u64_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"zero", "0", true, 0},
        {"valid", "1234", true, 1234},
        {"leading-zeros", "007", true, 7},
        {"above-u32-max", "4294967296", true, UINT64_C(4294967296)},
        {"max", "18446744073709551615", true, UINT64_MAX},
        {"max-plus-one", "18446744073709551616", false, 0},
        {"way-too-large", "99999999999999999999", false, 0},
        {"negative", "-1", false, 0},
        {"plus-sign", "+5", false, 0},
        {"leading-space", " 5", false, 0},
        {"trailing-space", "5 ", false, 0},
        {"trailing-garbage", "12x", false, 0},
        {"hex", "0x10", false, 0},
    };

    static const struct span_case span_cases[] = {
        {"null", NULL, 1, false, 0},
        {"zero-length", "123", 0, false, 0},
        {"whole", "123", 3, true, 123},
        {"prefix", "123-456", 3, true, 123},
        {"stops-at-dash", "123-456", 4, false, 0},
        /* Shorter than len: the NUL fails it, and nothing past is read. */
        {"short", "12", 3, false, 0},
        {"max", "18446744073709551615", 20, true, UINT64_MAX},
        {"overflow", "18446744073709551616", 20, false, 0},
        {"sign", "-1", 2, false, 0},
    };

    static const struct u64_case size_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"bytes", "4096", true, 4096},
        {"zero", "0", true, 0},
        {"k", "4k", true, UINT64_C(4) << 10},
        {"K", "4K", true, UINT64_C(4) << 10},
        {"M", "64M", true, UINT64_C(64) << 20},
        {"G", "2G", true, UINT64_C(2) << 30},
        {"T", "1T", true, UINT64_C(1) << 40},
        {"MiB", "128MiB", true, UINT64_C(128) << 20},
        {"mb", "128mb", true, UINT64_C(128) << 20},
        {"gi", "1gi", true, UINT64_C(1) << 30},
        {"suffix-only", "M", false, 0},
        {"bad-suffix", "12Q", false, 0},
        {"double-suffix", "1MM", false, 0},
        {"bytes-suffix", "12B", false, 0},
        {"trailing-space", "1M ", false, 0},
        {"negative", "-1", false, 0},
        {"plus-sign", "+1M", false, 0},
        {"hex", "0x1000", false, 0},
        {"max-bytes", "18446744073709551615", true, UINT64_MAX},
        {"max-T", "16777215T", true, UINT64_C(16777215) << 40},
        {"overflow-T", "16777216T", false, 0},
    };

    static const struct hex_case hex_cases[] = {
        {"null", NULL, 1, false, {0}},
        {"zero-n", "ab", 0, false, {0}},
        {"one", "a5", 1, true, {0xa5}},
        {"upper", "A5", 1, true, {0xa5}},
        {"four", "0123cdEF", 4, true, {0x01, 0x23, 0xcd, 0xef}},
        {"ignores-rest", "ffzz", 1, true, {0xff}},
        {"short", "abc", 2, false, {0}},
        {"bad-digit", "0g", 1, false, {0}},
        {"late-bad-digit", "0011223g", 4, false, {0}},
        {"sign", "-1", 1, false, {0}},
        {"space", " 1", 1, false, {0}},
    };

    static const struct mac_case mac_cases[] = {
        {"null", NULL, false, {0}},
        {"empty", "", false, {0}},
        {"valid", "02:00:00:00:00:01", true, {0x02, 0, 0, 0, 0, 0x01}},
        {"mixed-case",
         "aA:bB:cC:dD:eE:fF",
         true,
         {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}},
        {"five-bytes", "02:00:00:00:00", false, {0}},
        {"seven-bytes", "02:00:00:00:00:01:02", false, {0}},
        {"trailing-garbage", "02:00:00:00:00:01x", false, {0}},
        {"trailing-colon", "02:00:00:00:00:01:", false, {0}},
        {"single-digit", "2:0:0:0:0:1", false, {0}},
        {"three-digits", "002:00:00:00:00:01", false, {0}},
        {"dashes", "02-00-00-00-00-01", false, {0}},
        {"bad-digit", "02:00:00:0g:00:01", false, {0}},
        {"space", "02:00:00: 0:00:01", false, {0}},
    };

    static const struct port_case port_cases[] = {
        {"valid", "8080", true, 8080},    {"min", "1", true, 1},
        {"max", "65535", true, 65535},    {"zero", "0", false, 0},
        {"too-large", "65536", false, 0}, {"way-too-large", "99999", false, 0},
        {"negative", "-1", false, 0},     {"garbage", "80x", false, 0},
        {"empty", "", false, 0},
    };

    int failures = 0;
    size_t ni = sizeof(int_cases) / sizeof(int_cases[0]);
    size_t nu = sizeof(u32_cases) / sizeof(u32_cases[0]);
    size_t nhu = sizeof(hex_u32_cases) / sizeof(hex_u32_cases[0]);
    size_t n64 = sizeof(u64_cases) / sizeof(u64_cases[0]);
    size_t nspan = sizeof(span_cases) / sizeof(span_cases[0]);
    size_t nsize = sizeof(size_cases) / sizeof(size_cases[0]);
    size_t nhex = sizeof(hex_cases) / sizeof(hex_cases[0]);
    size_t nmac = sizeof(mac_cases) / sizeof(mac_cases[0]);
    size_t np = sizeof(port_cases) / sizeof(port_cases[0]);
    for (size_t i = 0; i < ni; i++) {
        failures += run_int(&int_cases[i]);
    }
    for (size_t i = 0; i < nu; i++) {
        failures += run_u32("u32", ernic_parse_u32, &u32_cases[i]);
    }
    for (size_t i = 0; i < nhu; i++) {
        failures += run_u32("hex-u32", ernic_parse_hex_u32, &hex_u32_cases[i]);
    }
    for (size_t i = 0; i < n64; i++) {
        failures += run_u64("u64", ernic_parse_u64, &u64_cases[i]);
    }
    for (size_t i = 0; i < nspan; i++) {
        failures += run_span(&span_cases[i]);
    }
    for (size_t i = 0; i < nsize; i++) {
        failures += run_u64("size", ernic_parse_size, &size_cases[i]);
    }
    for (size_t i = 0; i < nhex; i++) {
        failures += run_hex(&hex_cases[i]);
    }
    for (size_t i = 0; i < nmac; i++) {
        failures += run_mac(&mac_cases[i]);
    }
    for (size_t i = 0; i < np; i++) {
        failures += run_port(&port_cases[i]);
    }

    if (failures) {
        printf("parse_int: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("parse_int: all %zu cases passed\n",
           ni + nu + nhu + n64 + nspan + nsize + nhex + nmac + np);
    return 0;
}
