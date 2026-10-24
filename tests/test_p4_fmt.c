/*
 * The format layer: one encoder and one decoder for every format the
 * conversion extern moves between.
 *
 * The strongest checks here are the exhaustive ones, because a
 * floating-point codec fails on the cases nobody picks by hand -- the
 * subnormal boundary, the rounding tie, the exponent that carries. Three
 * of them:
 *
 *  - E4M3 against p4_fp8.c, which was itself validated against
 *    ml_dtypes. All 256 decodings and a wide encode sweep must agree
 *    exactly, so the generic path inherits that validation.
 *  - Every encoding of every format decoded and re-encoded. Only a NaN
 *    may come back different, and only by collapsing to the canonical
 *    one.
 *  - FP4 E2M1's whole value set, which is small enough to write down and
 *    is fixed by the OCP microscaling specification.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "p4_fmt.h"
#include "p4_fp8.h"

static int failures;

static void check(bool cond, const char *what)
{
    if (!cond) {
        printf("p4_fmt: FAIL %s\n", what);
        failures++;
    }
}

static void checkf(bool cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void checkf(bool cond, const char *fmt, ...)
{
    if (!cond) {
        va_list ap;

        printf("p4_fmt: FAIL ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
        failures++;
    }
}

static uint32_t bits_of(float v)
{
    uint32_t b;

    memcpy(&b, &v, sizeof(b));
    return b;
}

static bool is_nan(float v)
{
    return v != v;
}

/* The formats narrower than 32 bits, which are the ones worth sweeping
 * exhaustively; fp32 is the identity. */
static const enum p4_fmt narrow[] = {P4_FMT_FP16, P4_FMT_BF16, P4_FMT_FP8_E4M3,
                                     P4_FMT_FP8_E5M2, P4_FMT_FP4_E2M1};

/* Write raw encoding @raw as element 0 of a packed array. */
static void put_raw(enum p4_fmt f, uint8_t *buf, uint32_t raw)
{
    memset(buf, 0, 4);
    switch (p4_fmt_bits(f)) {
    case 4:
    case 8:
        buf[0] = (uint8_t)raw;
        break;
    default:
        buf[0] = (uint8_t)(raw & 0xFFu);
        buf[1] = (uint8_t)(raw >> 8);
        break;
    }
}

static uint32_t get_raw(enum p4_fmt f, const uint8_t *buf)
{
    switch (p4_fmt_bits(f)) {
    case 4:
        return buf[0] & 0x0Fu;
    case 8:
        return buf[0];
    default:
        return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
    }
}

/*
 * The generic path must reproduce p4_fp8.c exactly for E4M3. That file
 * was checked against ml_dtypes for all 256 decodings and 40506 encodings;
 * agreeing with it is how this one inherits that.
 */
static void test_e4m3_matches_fp8(void)
{
    unsigned bad = 0;

    for (unsigned i = 0; i < 256; i++) {
        uint8_t raw = (uint8_t)i;
        float a = p4_fmt_decode(P4_FMT_FP8_E4M3, &raw, 0);
        float b = p4_fp8_to_float(raw);

        if (!((is_nan(a) && is_nan(b)) || bits_of(a) == bits_of(b)))
            bad++;
    }
    checkf(bad == 0, "E4M3 decode disagrees with p4_fp8.c for %u encodings",
           bad);

    /*
     * A stride that is coprime with the obvious powers of two, so the
     * sweep lands on exponents and significands rather than marching
     * through one exponent's worth of mantissas.
     */
    bad = 0;
    for (uint64_t u = 0; u < (UINT64_C(1) << 32); u += 1021u) {
        float v;
        uint32_t raw = (uint32_t)u;
        uint8_t got = 0;

        memcpy(&v, &raw, sizeof(v));
        p4_fmt_encode(P4_FMT_FP8_E4M3, v, &got, 0);
        if (got != p4_fp8_from_float(v))
            bad++;
    }
    checkf(bad == 0, "E4M3 encode disagrees with p4_fp8.c for %u floats", bad);
}

/*
 * Decode every encoding and encode it back. Only a NaN may change, and
 * only into the canonical one -- every finite value, zeros and
 * infinities included, must be exactly itself again.
 */
static void test_round_trip_exhaustive(void)
{
    for (size_t k = 0; k < sizeof(narrow) / sizeof(narrow[0]); k++) {
        enum p4_fmt f = narrow[k];
        unsigned n = 1u << p4_fmt_bits(f);
        unsigned finite_bad = 0, nan_bad = 0;

        for (unsigned i = 0; i < n; i++) {
            uint8_t in[4], out[4];
            float v;

            put_raw(f, in, i);
            v = p4_fmt_decode(f, in, 0);
            memset(out, 0, sizeof(out));
            p4_fmt_encode(f, v, out, 0);

            if (is_nan(v)) {
                /* Allowed to canonicalise, but it must still be a NaN. */
                if (!is_nan(p4_fmt_decode(f, out, 0)))
                    nan_bad++;
            } else if (get_raw(f, out) != i) {
                finite_bad++;
            }
        }
        checkf(finite_bad == 0,
               "%s: %u of %u non-NaN encodings did not round-trip",
               p4_fmt_name(f), finite_bad, n);
        checkf(nan_bad == 0, "%s: %u NaN encodings did not stay NaN",
               p4_fmt_name(f), nan_bad);
    }
}

/*
 * FP4 E2M1's entire value set, which the OCP microscaling specification
 * fixes: no infinity, no NaN, all sixteen encodings spent on finite
 * values. This is the format the 4-bit AMD data paths use.
 */
static void test_fp4_value_set(void)
{
    static const float want[16] = {0.0f,  0.5f,  1.0f,  1.5f,  2.0f,  3.0f,
                                   4.0f,  6.0f,  -0.0f, -0.5f, -1.0f, -1.5f,
                                   -2.0f, -3.0f, -4.0f, -6.0f};

    for (unsigned i = 0; i < 16; i++) {
        uint8_t raw = (uint8_t)i;
        float got = p4_fmt_decode(P4_FMT_FP4_E2M1, &raw, 0);

        checkf(bits_of(got) == bits_of(want[i]),
               "fp4 encoding %#x is %g, want %g", i, (double)got,
               (double)want[i]);
    }

    /* No NaN encoding, so a NaN cannot survive: it saturates like an
     * overflow, keeping its sign.  Lossy, and unavoidably so. */
    {
        uint8_t out = 0;
        float nan_v =
            p4_fmt_decode(P4_FMT_FP16, (const uint8_t[]){0x00, 0x7E}, 0);

        check(is_nan(nan_v), "the fp16 NaN fixture is not a NaN");
        p4_fmt_encode(P4_FMT_FP4_E2M1, nan_v, &out, 0);
        checkf(out == 0x7u, "fp4 NaN gave %#x, want the largest finite (0x7)",
               out);
    }
    /* And an overflow saturates rather than becoming an infinity. */
    {
        uint8_t out = 0;

        p4_fmt_encode(P4_FMT_FP4_E2M1, 1000.0f, &out, 0);
        checkf(out == 0x7u, "fp4 1000 gave %#x, want 6.0 (0x7)", out);
        p4_fmt_encode(P4_FMT_FP4_E2M1, -1000.0f, &out, 0);
        checkf(out == 0xFu, "fp4 -1000 gave %#x, want -6.0 (0xF)", out);
    }
}

/*
 * Two elements to a byte, low nibble first. Getting the order wrong
 * reverses every pair, which an all-one-value buffer would hide.
 */
static void test_fp4_packing(void)
{
    uint8_t buf[2] = {0, 0};
    uint8_t out[2] = {0xFF, 0xFF};

    p4_fmt_encode(P4_FMT_FP4_E2M1, 1.0f, buf, 0);  /* 0x2, low nibble */
    p4_fmt_encode(P4_FMT_FP4_E2M1, -6.0f, buf, 1); /* 0xF, high nibble */
    checkf(buf[0] == 0xF2u, "fp4 packing gave %#x, want 0xf2", buf[0]);

    /* Writing one element must leave the other alone. */
    out[0] = 0xA5u;
    p4_fmt_encode(P4_FMT_FP4_E2M1, 0.0f, out, 1);
    checkf((out[0] & 0x0Fu) == 0x05u,
           "writing the high nibble disturbed the low one: %#x", out[0]);

    checkf(p4_fmt_bytes(P4_FMT_FP4_E2M1, 2) == 1,
           "two fp4 elements are a byte");
    checkf(p4_fmt_bytes(P4_FMT_FP4_E2M1, 3) == 0,
           "an odd fp4 element count is not a whole number of bytes");
    checkf(p4_fmt_elems(P4_FMT_FP4_E2M1, 1) == 2, "a byte is two fp4 elements");
    checkf(p4_fmt_elems(P4_FMT_FP16, 3) == 0,
           "three bytes is not a whole number of fp16 elements");
}

/* Widening and narrowing are the same call with the pair reversed. */
static void test_both_directions(void)
{
    /* 1.0, 2.0, 4.0, 6.0 in fp4: encodings 2, 4, 6, 7. */
    static const uint8_t fp4_in[2] = {0x42u, 0x76u};
    static const float want[4] = {1.0f, 2.0f, 4.0f, 6.0f};
    uint8_t wide[4 * 4], back[2];
    float got;

    /* fp4 -> fp32, the widest widening there is. */
    check(p4_fmt_convert(P4_FMT_FP4_E2M1, P4_FMT_FP32, fp4_in, 4, wide),
          "fp4 to fp32 was refused");
    for (unsigned i = 0; i < 4; i++) {
        got = p4_fmt_decode(P4_FMT_FP32, wide, i);
        checkf(bits_of(got) == bits_of(want[i]),
               "fp4 to fp32 element %u is %g, want %g", i, (double)got,
               (double)want[i]);
    }

    /* And back again: every fp4 value is exactly representable in fp32,
     * so this is the identity. */
    check(p4_fmt_convert(P4_FMT_FP32, P4_FMT_FP4_E2M1, wide, 4, back),
          "fp32 to fp4 was refused");
    check(memcmp(back, fp4_in, sizeof(back)) == 0,
          "fp4 -> fp32 -> fp4 is not the identity");

    /* fp4 -> fp16 widens too, and fp16 holds every fp4 value exactly. */
    {
        uint8_t half[4 * 2];

        check(p4_fmt_convert(P4_FMT_FP4_E2M1, P4_FMT_FP16, fp4_in, 4, half),
              "fp4 to fp16 was refused");
        for (unsigned i = 0; i < 4; i++) {
            got = p4_fmt_decode(P4_FMT_FP16, half, i);
            checkf(bits_of(got) == bits_of(want[i]),
                   "fp4 to fp16 element %u is %g, want %g", i, (double)got,
                   (double)want[i]);
        }
    }

    /* bf16 -> fp8 E5M2 narrows between two formats that both have
     * infinities, so an overflow becomes one rather than saturating. */
    {
        uint8_t big[2], small[1];

        /* bf16 3.0e38, which E5M2 cannot hold (its max is 57344). */
        big[0] = 0x0Eu;
        big[1] = 0x7Fu;
        check(p4_fmt_convert(P4_FMT_BF16, P4_FMT_FP8_E5M2, big, 1, small),
              "bf16 to E5M2 was refused");
        got = p4_fmt_decode(P4_FMT_FP8_E5M2, small, 0);
        check(got > 0.0f && !(got < 1.0f / 0.0f) && !is_nan(got),
              "an overflow into E5M2, which has infinities, did not become "
              "one");
    }
}

/* Arguments that must be refused rather than guessed at. */
static void test_bad_arguments(void)
{
    uint8_t in[8] = {0}, out[8] = {0};

    check(!p4_fmt_convert((enum p4_fmt)P4_FMT_COUNT, P4_FMT_FP32, in, 4, out),
          "an unknown source format was accepted");
    check(!p4_fmt_convert(P4_FMT_FP32, (enum p4_fmt)0x55u, in, 4, out),
          "an unknown destination format was accepted");
    check(!p4_fmt_convert(P4_FMT_FP32, P4_FMT_FP4_E2M1, NULL, 4, out),
          "a NULL source was accepted");
    check(!p4_fmt_convert(P4_FMT_FP32, P4_FMT_FP4_E2M1, in, 4, NULL),
          "a NULL destination was accepted");
    /* An odd count of a 4-bit format is half a byte at one end. */
    check(!p4_fmt_convert(P4_FMT_FP4_E2M1, P4_FMT_FP32, in, 3, out),
          "an odd fp4 element count was accepted");
    check(p4_fmt_convert(P4_FMT_FP32, P4_FMT_FP4_E2M1, in, 0, out),
          "an empty conversion was refused");

    check(!p4_fmt_valid(P4_FMT_COUNT), "P4_FMT_COUNT is not a format");
    check(p4_fmt_valid(P4_FMT_FP4_E2M1), "fp4 should be a valid format");
    checkf(p4_fmt_bits(P4_FMT_FP32) == 32 && p4_fmt_bits(P4_FMT_FP16) == 16 &&
               p4_fmt_bits(P4_FMT_BF16) == 16 &&
               p4_fmt_bits(P4_FMT_FP8_E4M3) == 8 &&
               p4_fmt_bits(P4_FMT_FP8_E5M2) == 8 &&
               p4_fmt_bits(P4_FMT_FP4_E2M1) == 4,
           "a format reported the wrong width");
}

/*
 * The conventions, stated as values rather than left to the exhaustive
 * sweeps: a format with infinities overflows into one, a format without
 * saturates, and a NaN survives wherever there is an encoding for it.
 */
static void test_overflow_conventions(void)
{
    uint8_t out[4];

    /* fp32 -> fp16: fp16 has infinities. */
    p4_fmt_encode(P4_FMT_FP16, 1.0e30f, out, 0);
    checkf(get_raw(P4_FMT_FP16, out) == 0x7C00u,
           "fp32 1e30 to fp16 gave %#x, want +infinity",
           get_raw(P4_FMT_FP16, out));

    /* fp32 -> E4M3: no infinity, so it saturates to 448. */
    p4_fmt_encode(P4_FMT_FP8_E4M3, 1.0e30f, out, 0);
    checkf(out[0] == P4_FP8_MAX, "fp32 1e30 to E4M3 gave %#x, want %#x", out[0],
           P4_FP8_MAX);

    /* An infinity narrowing into E4M3 saturates for the same reason. */
    p4_fmt_encode(P4_FMT_FP8_E4M3, 1.0f / 0.0f, out, 0);
    checkf(out[0] == P4_FP8_MAX, "+infinity to E4M3 gave %#x, want %#x", out[0],
           P4_FP8_MAX);

    /* A NaN keeps being one where the format can say so. */
    {
        float nan_v = 0.0f / 0.0f;

        p4_fmt_encode(P4_FMT_FP8_E4M3, nan_v, out, 0);
        check(is_nan(p4_fmt_decode(P4_FMT_FP8_E4M3, out, 0)),
              "a NaN did not survive into E4M3");
        p4_fmt_encode(P4_FMT_FP16, nan_v, out, 0);
        check(is_nan(p4_fmt_decode(P4_FMT_FP16, out, 0)),
              "a NaN did not survive into fp16");
    }
}

int main(void)
{
    test_e4m3_matches_fp8();
    test_round_trip_exhaustive();
    test_fp4_value_set();
    test_fp4_packing();
    test_both_directions();
    test_bad_arguments();
    test_overflow_conventions();

    if (failures) {
        printf("p4_fmt: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("p4_fmt: all checks passed\n");
    return 0;
}
