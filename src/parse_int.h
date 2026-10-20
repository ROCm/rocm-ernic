/*
 * Strict number parsing helpers.
 *
 * Replacements for atoi(), the strto*() family, and sscanf() that reject
 * empty input, trailing garbage, and out-of-range values instead of silently
 * truncating or returning 0. Header-only so they can be unit-tested directly,
 * and so the guest-side test programs, which are copied into a VM and built
 * there on their own, can take this file with them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_PARSE_INT_H
#define ERNIC_PARSE_INT_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Parse a base-10 int: digits, optionally preceded by a '-'. Rejects
 * NULL/empty input, a leading space or '+', trailing characters, and values
 * outside [INT_MIN, INT_MAX]. On success stores the value in *out and
 * returns true; otherwise leaves *out untouched and returns false.
 */
static inline bool ernic_parse_int(const char *s, int *out)
{
    if (!s) {
        return false;
    }

    /* strtol() would skip leading space and take a '+', so check first that
     * the digits start at once, after a '-' at most. */
    const char *digits = *s == '-' ? s + 1 : s;
    if (*digits < '0' || *digits > '9') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    if (v < INT_MIN || v > INT_MAX) {
        return false;
    }

    *out = (int)v;
    return true;
}

/*
 * Parse a base-10 uint32_t in [0, UINT32_MAX]. Rejects NULL/empty input,
 * anything but digits, and values above UINT32_MAX. Unlike
 * ernic_parse_int(), it takes no '-' either: strtoull() would accept "-1"
 * and wrap it to the largest value. On success stores the value in *out and
 * returns true; otherwise leaves *out untouched and returns false.
 */
static inline bool ernic_parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s < '0' || *s > '9') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);

    if (errno != 0 || *end != '\0' || v > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)v;
    return true;
}

/*
 * Parse the len characters at s as a base-10 uint64_t, for a number that is
 * part of a longer string, such as the two ends of an HTTP byte range. Every
 * one of them must be a digit: no sign and no space. A NUL isn't a digit, so
 * a string shorter than len fails without being read past its end. Rejects
 * len 0 and values above UINT64_MAX. On success stores the value in *out and
 * returns true; otherwise leaves *out untouched and returns false.
 */
static inline bool ernic_parse_u64_span(const char *s, size_t len,
                                        uint64_t *out)
{
    if (!s || len == 0) {
        return false;
    }

    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        /* Unsigned, so a character below '0' wraps to a large value. */
        unsigned d = (unsigned)(unsigned char)s[i] - (unsigned)'0';
        if (d > 9) {
            return false;
        }
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }

    *out = v;
    return true;
}

/*
 * Parse a base-10 uint64_t. Like ernic_parse_u32(), it takes digits only, so
 * it rejects NULL/empty input, a sign, a space, trailing characters, and
 * values above UINT64_MAX. On success stores the value in *out and returns
 * true; otherwise leaves *out untouched and returns false.
 */
static inline bool ernic_parse_u64(const char *s, uint64_t *out)
{
    if (!s) {
        return false;
    }
    return ernic_parse_u64_span(s, strlen(s), out);
}

/*
 * Parse a byte count: a base-10 number of bytes, optionally followed by a
 * binary multiplier, K, M, G, or T, in either case. The multiplier may be
 * spelled KiB or KB, and so on, and means 1024 either way. Rejects anything
 * else, including a sign, a space, and a value that overflows uint64_t once
 * multiplied. On success stores the number of bytes in *out and returns
 * true; otherwise leaves *out untouched and returns false.
 */
static inline bool ernic_parse_size(const char *s, uint64_t *out)
{
    if (!s) {
        return false;
    }

    size_t ndigits = strspn(s, "0123456789");
    uint64_t v;
    if (!ernic_parse_u64_span(s, ndigits, &v)) {
        return false;
    }

    const char *p = s + ndigits;
    unsigned shift = 0;
    switch (*p) {
    case 'k':
    case 'K':
        shift = 10;
        break;
    case 'm':
    case 'M':
        shift = 20;
        break;
    case 'g':
    case 'G':
        shift = 30;
        break;
    case 't':
    case 'T':
        shift = 40;
        break;
    default:
        break;
    }
    if (shift != 0) {
        p++;
        if (*p == 'i' || *p == 'I') {
            p++;
        }
        if (*p == 'b' || *p == 'B') {
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    if (v > (UINT64_MAX >> shift)) {
        return false;
    }

    *out = v << shift;
    return true;
}

/*
 * The value of hex digit c, or -1 if c isn't one. Unsigned throughout: on a
 * signed char the subtractions would be signed int arithmetic, and GCC's
 * -Wstrict-overflow reports folding the range checks around them.
 */
static inline int ernic_hex_digit(char c)
{
    unsigned u = (unsigned char)c;

    if (u >= '0' && u <= '9') {
        return (int)(u - '0');
    }
    if (u >= 'a' && u <= 'f') {
        return (int)(u - 'a' + 10u);
    }
    if (u >= 'A' && u <= 'F') {
        return (int)(u - 'A' + 10u);
    }
    return -1;
}

/*
 * Parse a hexadecimal uint32_t: one to eight hex digits, in either case, with
 * no "0x" prefix, sign, or space. Rejects NULL/empty input and anything else.
 * On success stores the value in *out and returns true; otherwise leaves
 * *out untouched and returns false.
 */
static inline bool ernic_parse_hex_u32(const char *s, uint32_t *out)
{
    if (!s) {
        return false;
    }
    size_t len = strlen(s);
    if (len == 0 || len > 8) {
        return false;
    }

    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        int d = ernic_hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }

    *out = v;
    return true;
}

/*
 * Decode the 2 * n hex digits at s, in either case, into the n bytes at out,
 * the first digit of each pair being the high half of its byte. Every one of
 * them must be a hex digit. Like ernic_parse_u64_span(), it doesn't look at
 * what follows them, and fails on a shorter string without reading past its
 * end. On success returns true; otherwise leaves out untouched and returns
 * false.
 */
static inline bool ernic_parse_hex(const char *s, size_t n, uint8_t *out)
{
    if (!s || n == 0 || n > SIZE_MAX / 2) {
        return false;
    }

    /* Check every digit first, so a failure writes nothing. */
    for (size_t i = 0; i < 2 * n; i++) {
        if (ernic_hex_digit(s[i]) < 0) {
            return false;
        }
    }
    for (size_t i = 0; i < n; i++) {
        unsigned hi = (unsigned)ernic_hex_digit(s[2 * i]);
        unsigned lo = (unsigned)ernic_hex_digit(s[2 * i + 1]);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/*
 * Parse a MAC address written as six pairs of hex digits separated by
 * colons, XX:XX:XX:XX:XX:XX, in either case. Rejects anything else, such as
 * a single-digit pair, another separator, or trailing characters. On success
 * stores the six bytes in out and returns true; otherwise leaves out
 * untouched and returns false.
 */
static inline bool ernic_parse_mac(const char *s, uint8_t out[6])
{
    uint8_t mac[6];

    if (!s) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        const char *pair = s + 3 * i;
        if (!ernic_parse_hex(pair, 1, &mac[i])) {
            return false;
        }
        if (pair[2] != (i < 5 ? ':' : '\0')) {
            return false;
        }
    }

    memcpy(out, mac, sizeof(mac));
    return true;
}

/*
 * Parse a TCP port in [1, 65535]: digits only. Returns true on success
 * (value in *out), false for anything out of range, zero, or not just
 * digits, including a sign or a space.
 */
static inline bool ernic_parse_port(const char *s, uint16_t *out)
{
    int v;
    if (!ernic_parse_int(s, &v)) {
        return false;
    }
    if (v < 1 || v > 65535) {
        return false;
    }

    *out = (uint16_t)v;
    return true;
}

#endif /* ERNIC_PARSE_INT_H */
