/*
 * Unit tests for OCP FP8 E4M3 arithmetic.
 *
 * The format has 256 encodings, so most of this is exhaustive rather than
 * sampled: every encoding is decoded, and every encoding that is not a NaN
 * is required to survive a decode/encode round trip unchanged. A format
 * that loses a value in a round trip would corrupt data silently.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "p4/p4_fp8.h"

static int failures;

/*
 * Exact comparison that does not trip -Wfloat-equal. Every value compared
 * here is exactly representable in both fp8 and float, so an exact test is
 * the correct one; the warning exists for values that are not. NaN compares
 * equal to everything under this, so NaN is always checked separately.
 */
static bool feq(float a, float b)
{
    return !(a < b) && !(a > b);
}

static void check(bool cond, const char *what)
{
    if (!cond) {
        printf("p4_fp8: FAIL %s\n", what);
        failures++;
    }
}

static void checkf(bool cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void checkf(bool cond, const char *fmt, ...)
{
    if (!cond) {
        va_list ap;
        printf("p4_fp8: FAIL ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
        failures++;
    }
}

/* The values the OCP FP8 specification pins down for E4M3. */
static void test_known_encodings(void)
{
    static const struct {
        uint8_t enc;
        float val;
        const char *name;
    } cases[] = {
        {0x00, 0.0f, "+0"},
        {0x38, 1.0f, "1.0"},
        {0xB8, -1.0f, "-1.0"},
        {0x40, 2.0f, "2.0"},
        {0x44, 3.0f, "3.0"},
        {0x48, 4.0f, "4.0"},
        {0x3C, 1.5f, "1.5"},
        {0x30, 0.5f, "0.5"},
        {0x7E, 448.0f, "max normal"},
        {0xFE, -448.0f, "min normal"},
        {0x08, 0.015625f, "smallest normal, 2^-6"},
        {0x01, 0.001953125f, "smallest subnormal, 2^-9"},
        {0x07, 0.013671875f, "largest subnormal"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float got = p4_fp8_to_float(cases[i].enc);
        checkf(feq(got, cases[i].val), "decode %s: got %g, want %g",
               cases[i].name, (double)got, (double)cases[i].val);
        checkf(p4_fp8_from_float(cases[i].val) == cases[i].enc,
               "encode %s: got 0x%02X, want 0x%02X", cases[i].name,
               p4_fp8_from_float(cases[i].val), cases[i].enc);
    }

    /* -0 decodes to zero but keeps its sign through an encode. */
    check(feq(p4_fp8_to_float(0x80), 0.0f), "-0 decodes to zero");
    check(p4_fp8_from_float(-0.0f) == 0x80, "-0 encodes with its sign");
}

static void test_nan(void)
{
    check(p4_fp8_is_nan(0x7F), "0x7F is NaN");
    check(p4_fp8_is_nan(0xFF), "0xFF is NaN");
    check(!p4_fp8_is_nan(0x7E), "0x7E is the largest finite value, not NaN");
    check(!p4_fp8_is_nan(0x00), "zero is not NaN");
    check(isnan(p4_fp8_to_float(0x7F)), "NaN decodes to a NaN");
    check(isnan(p4_fp8_to_float(0xFF)), "negative NaN decodes to a NaN");
    check(p4_fp8_from_float(NAN) == P4_FP8_NAN, "a NaN encodes to the NaN");

    /* E4M3 has no infinity, so one has to become something finite or NaN
     * rather than silently wrapping. */
    check(p4_fp8_from_float(INFINITY) == P4_FP8_MAX,
          "+inf saturates to the largest finite value");
    check(p4_fp8_from_float(-INFINITY) == (P4_FP8_MAX | 0x80u),
          "-inf saturates with its sign");
}

static void test_round_trip_exhaustive(void)
{
    for (unsigned i = 0; i < 256; i++) {
        uint8_t enc = (uint8_t)i;
        float v;
        uint8_t back;

        if (p4_fp8_is_nan(enc))
            continue;

        v = p4_fp8_to_float(enc);
        check(!isnan(v) && !isinf(v), "every non-NaN encoding is finite");

        back = p4_fp8_from_float(v);
        checkf(back == enc, "round trip 0x%02X -> %g -> 0x%02X", enc, (double)v,
               back);
    }
}

static void test_saturation_and_rounding(void)
{
    /* Above the top of the range everything saturates; the boundary is the
     * midpoint to where the next value would have been. */
    check(p4_fp8_from_float(448.0f) == 0x7E, "448 is exact");
    check(p4_fp8_from_float(456.0f) == 0x7E, "just above 448 rounds down");
    check(p4_fp8_from_float(464.0f) == 0x7E, "the midpoint saturates");
    check(p4_fp8_from_float(1.0e30f) == 0x7E, "a huge value saturates");
    check(p4_fp8_from_float(-1.0e30f) == 0xFE, "a huge negative saturates");

    /* Below the bottom it flushes to zero, keeping the sign. */
    check(p4_fp8_from_float(1.0e-30f) == 0x00, "a tiny value becomes zero");
    check(p4_fp8_from_float(-1.0e-30f) == 0x80,
          "a tiny negative becomes negative zero");

    /* Ties go to even. Between 1.0 (0x38, mantissa 0) and 1.125 (0x39,
     * mantissa 1) the midpoint is 1.0625, which must pick the even one. */
    check(p4_fp8_from_float(1.0625f) == 0x38, "a tie rounds to even, down");
    /* Between 1.125 (mantissa 1) and 1.25 (mantissa 2) the midpoint 1.1875
     * must round up, to the even mantissa. */
    check(p4_fp8_from_float(1.1875f) == 0x3A, "a tie rounds to even, up");

    /* Rounding the mantissa up has to carry into the exponent. */
    check(p4_fp8_from_float(1.9375f) == 0x40,
          "a mantissa carry increments the exponent");

    /* Rounding up out of the subnormals lands on the smallest normal. */
    check(p4_fp8_from_float(0.0150f) == 0x08,
          "a subnormal rounding up becomes the smallest normal");
}

/*
 * Elementwise. This is the shape a collective wants: two partial buffers
 * combined into a third of the same length.
 */
static void test_map(void)
{
    const uint8_t one = 0x38;  /* 1.0 */
    const uint8_t two = 0x40;  /* 2.0 */
    const uint8_t four = 0x48; /* 4.0 */
    uint8_t out[8];

    {
        const uint8_t a[] = {one, two, four, one};
        const uint8_t b[] = {one, two, one, four};

        check(p4_fp8_map(P4_FP8_OP_ADD, a, b, 4, out), "ADD succeeds");
        check(out[0] == two, "1+1 is 2");
        check(feq(p4_fp8_to_float(out[1]), 4.0f), "2+2 is 4");
        check(feq(p4_fp8_to_float(out[2]), 5.0f), "4+1 is 5");
        check(feq(p4_fp8_to_float(out[3]), 5.0f), "1+4 is 5");
    }

    {
        const uint8_t a[] = {two, four};
        const uint8_t b[] = {four, two};

        check(p4_fp8_map(P4_FP8_OP_MUL, a, b, 2, out), "MUL succeeds");
        check(feq(p4_fp8_to_float(out[0]), 8.0f), "2*4 is 8");
        check(feq(p4_fp8_to_float(out[1]), 8.0f), "4*2 is 8");

        check(p4_fp8_map(P4_FP8_OP_MAX, a, b, 2, out), "MAX succeeds");
        check(feq(p4_fp8_to_float(out[0]), 4.0f), "max(2,4) is 4");
        check(feq(p4_fp8_to_float(out[1]), 4.0f), "max(4,2) is 4");
    }

    /* A NaN spoils its own element and no other. */
    {
        const uint8_t a[] = {one, P4_FP8_NAN, one};
        const uint8_t b[] = {one, one, one};

        check(p4_fp8_map(P4_FP8_OP_ADD, a, b, 3, out), "ADD succeeds");
        check(out[0] == two, "the element before a NaN is unaffected");
        check(out[1] == P4_FP8_NAN, "the NaN element is NaN");
        check(out[2] == two, "the element after a NaN is unaffected");
    }

    /* Each element saturates on its own. */
    {
        const uint8_t a[] = {0x7E, one}; /* 448, 1.0 */
        const uint8_t b[] = {0x7E, one};

        check(p4_fp8_map(P4_FP8_OP_ADD, a, b, 2, out), "ADD succeeds");
        check(out[0] == P4_FP8_MAX, "448+448 saturates");
        check(out[1] == two, "its neighbour is untouched");
    }

    /* Bad arguments are refused before anything is written. */
    memset(out, 0xA5, sizeof(out));
    /* Deliberately out of range: a control message can carry any value in
     * the field, so the branch that rejects one has to be covered.
     * NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    check(!p4_fp8_map((enum p4_fp8_op)99, &one, &one, 1, out),
          "an unknown operation is refused");
    check(out[0] == 0xA5, "a refused operation wrote nothing");
    check(!p4_fp8_map(P4_FP8_OP_ADD, NULL, NULL, 4, out),
          "a NULL operand with a non-zero length is refused");
    check(!p4_fp8_map(P4_FP8_OP_ADD, &one, &one, 1, NULL),
          "a NULL destination is refused");
    check(p4_fp8_map(P4_FP8_OP_ADD, NULL, NULL, 0, out),
          "an empty map is accepted and does nothing");
}

int main(void)
{
    test_known_encodings();
    test_nan();
    test_round_trip_exhaustive();
    test_saturation_and_rounding();
    test_map();

    if (failures) {
        printf("p4_fp8: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("p4_fp8: all checks passed\n");
    return 0;
}
