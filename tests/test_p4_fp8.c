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

#include "p4_fp8.h"

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

static void test_reduce(void)
{
    uint8_t out;
    const uint8_t one = 0x38;  /* 1.0 */
    const uint8_t two = 0x40;  /* 2.0 */
    const uint8_t four = 0x48; /* 4.0 */

    /* ADD reduces both buffers: sum of every a[i] + b[i]. */
    {
        const uint8_t a[] = {one, one, one, one};
        const uint8_t b[] = {one, one, one, one};
        check(p4_fp8_reduce(P4_FP8_OP_ADD, a, b, 4, &out), "ADD succeeds");
        check(out == 0x50, "1+1 over four elements is 8.0");
    }

    /* MUL is a dot product. */
    {
        const uint8_t a[] = {one, two, four};
        const uint8_t b[] = {two, two, one};
        /* 1*2 + 2*2 + 4*1 = 10 */
        check(p4_fp8_reduce(P4_FP8_OP_MUL, a, b, 3, &out), "MUL succeeds");
        check(feq(p4_fp8_to_float(out), 10.0f), "the dot product is 10.0");
    }

    {
        const uint8_t a[] = {one, four};
        const uint8_t b[] = {two, one};
        check(p4_fp8_reduce(P4_FP8_OP_MAX, a, b, 2, &out), "MAX succeeds");
        check(feq(p4_fp8_to_float(out), 4.0f), "the maximum is 4.0");
    }

    /* Accumulating in float, not fp8, is what makes a long sum meaningful:
     * 0.5 added 16 times is 8, but rounding to fp8 after each step would
     * stall long before that. */
    {
        uint8_t a[16], b[16];
        memset(a, 0x28, sizeof(a)); /* 0.25 */
        memset(b, 0x28, sizeof(b));
        check(p4_fp8_reduce(P4_FP8_OP_ADD, a, b, 16, &out), "ADD succeeds");
        check(feq(p4_fp8_to_float(out), 8.0f),
              "sixteen 0.25+0.25 pairs sum to 8.0");
    }

    /* A NaN operand must reach the caller, not vanish into the sum. */
    {
        const uint8_t a[] = {one, P4_FP8_NAN};
        const uint8_t b[] = {one, one};
        check(p4_fp8_reduce(P4_FP8_OP_ADD, a, b, 2, &out), "ADD succeeds");
        check(out == P4_FP8_NAN, "a NaN operand makes the result NaN");
    }

    /* An overflowing sum saturates rather than wrapping. */
    {
        uint8_t a[8], b[8];
        memset(a, 0x7E, sizeof(a)); /* 448 */
        memset(b, 0x7E, sizeof(b));
        check(p4_fp8_reduce(P4_FP8_OP_ADD, a, b, 8, &out), "ADD succeeds");
        check(out == P4_FP8_MAX, "an overflowing sum saturates");
    }

    /* Identities for an empty reduction. */
    check(p4_fp8_reduce(P4_FP8_OP_ADD, NULL, NULL, 0, &out) && out == 0x00,
          "an empty ADD is zero");
    check(p4_fp8_reduce(P4_FP8_OP_MUL, NULL, NULL, 0, &out) && out == 0x00,
          "an empty MUL is zero");

    /* Bad arguments are refused rather than read. */
    check(!p4_fp8_reduce(P4_FP8_OP_ADD, NULL, NULL, 4, &out),
          "a NULL operand with a non-zero length is refused");
    check(!p4_fp8_reduce(P4_FP8_OP_ADD, &one, &one, 1, NULL),
          "a NULL result is refused");
    /* Deliberately out of range: a control message can carry any value in
     * the field, so the default branch that rejects one has to be covered.
     * NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    check(!p4_fp8_reduce((enum p4_fp8_op)99, &one, &one, 1, &out),
          "an unknown operation is refused");
}

/*
 * Elementwise. This is the shape a collective wants: two partial buffers
 * combined into a third of the same length, rather than collapsed to one
 * value the way p4_fp8_reduce() does.
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

    /*
     * A NaN spoils its own element and no other. That is the difference
     * from a reduction, where one NaN has to poison the result because
     * there is only one.
     */
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


/* One little-endian fp16 or bf16 element, for the conversion tests. */
static void put16(uint8_t *dst, uint16_t raw)
{
    dst[0] = (uint8_t)(raw & 0xFFu);
    dst[1] = (uint8_t)(raw >> 8);
}

/*
 * fp16 -> fp8.  The decode half was checked against numpy for all 65536
 * encodings; what is pinned here is the behaviour the conversion adds on
 * top -- the narrowing conventions, which are the same ones the rest of
 * the file keeps.
 */
static void test_convert_fp16(void)
{
    static const struct {
        uint16_t in;
        uint8_t want;
        const char *what;
    } cases[] = {
        {0x3C00u, 0x38u, "1.0"},
        {0x4000u, 0x40u, "2.0"},
        {0x4200u, 0x44u, "3.0"},
        {0x4400u, 0x48u, "4.0"},
        {0x4600u, 0x4Cu, "6.0"},
        {0xBC00u, 0xB8u, "-1.0"},
        {0x0000u, 0x00u, "+0"},
        {0x8000u, 0x80u, "-0"},
        {0x5C00u, 0x78u, "256.0, exactly representable"},
        {0x5F00u, P4_FP8_MAX, "448.0, the largest finite value"},
        /* E4M3 has no infinities, so these saturate rather than becoming
         * NaN: one overflowed term must not destroy a whole reduction. */
        {0x5F80u, P4_FP8_MAX, "480 saturates"},
        {0x7BFFu, P4_FP8_MAX, "65504, fp16's largest, saturates"},
        {0x7C00u, P4_FP8_MAX, "+infinity saturates"},
        {0xFC00u, (uint8_t)(0x80u | P4_FP8_MAX), "-infinity saturates"},
        {0x7E00u, P4_FP8_NAN, "NaN stays NaN"},
        {0x1800u, 0x01u, "2^-9, the smallest fp8 subnormal"},
        {0x0001u, 0x00u, "fp16's smallest subnormal flushes"},
    };
    uint8_t in[2], got;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        put16(in, cases[i].in);
        check(p4_fp8_convert(P4_FP8_SRC_FP16, in, 1, &got),
              "p4_fp8_convert refused a well-formed element");
        checkf(got == cases[i].want, "fp16 %s: got %#x, want %#x",
               cases[i].what, got, cases[i].want);
    }
}

/* bf16 is float32's top half, so its exponent range is far wider than
 * E4M3's and the saturating convention does most of the work. */
static void test_convert_bf16(void)
{
    static const struct {
        uint16_t in;
        uint8_t want;
        const char *what;
    } cases[] = {
        {0x3F80u, 0x38u, "1.0"},
        {0x4000u, 0x40u, "2.0"},
        {0x4080u, 0x48u, "4.0"},
        {0xBF80u, 0xB8u, "-1.0"},
        {0x43E0u, P4_FP8_MAX, "448.0"},
        {0x7F80u, P4_FP8_MAX, "+infinity saturates"},
        {0x0000u, 0x00u, "+0"},
    };
    uint8_t in[2], got;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        put16(in, cases[i].in);
        check(p4_fp8_convert(P4_FP8_SRC_BF16, in, 1, &got),
              "p4_fp8_convert refused a well-formed bf16 element");
        checkf(got == cases[i].want, "bf16 %s: got %#x, want %#x",
               cases[i].what, got, cases[i].want);
    }
}

/*
 * A whole buffer, and the arguments the conversion must refuse.  The
 * elementwise property matters here for the same reason it does for the
 * map: one bad element must not disturb its neighbours.
 */
static void test_convert_buffer(void)
{
    uint8_t in[8 * 2], out[8];
    static const uint16_t raw[8] = {0x3C00u, 0x7E00u, 0x4000u, 0x7C00u,
                                    0x4400u, 0x0000u, 0xBC00u, 0x4600u};
    static const uint8_t want[8] = {0x38u, P4_FP8_NAN, 0x40u, P4_FP8_MAX,
                                    0x48u, 0x00u,      0xB8u, 0x4Cu};

    for (size_t i = 0; i < 8; i++)
        put16(in + 2u * i, raw[i]);

    check(p4_fp8_convert(P4_FP8_SRC_FP16, in, 8, out),
          "p4_fp8_convert refused a well-formed buffer");
    for (size_t i = 0; i < 8; i++)
        checkf(out[i] == want[i], "buffer element %zu: got %#x, want %#x", i,
               out[i], want[i]);

    /* A NaN and an infinity in the middle left everything else alone,
     * which the per-element checks above have just shown. */

    check(!p4_fp8_convert((enum p4_fp8_src_fmt)0x55u, in, 8, out),
          "an unknown source format was accepted");
    check(!p4_fp8_convert(P4_FP8_SRC_FP16, NULL, 8, out),
          "a NULL source was accepted");
    check(!p4_fp8_convert(P4_FP8_SRC_FP16, in, 8, NULL),
          "a NULL destination was accepted");
    check(p4_fp8_convert(P4_FP8_SRC_FP16, in, 0, out),
          "an empty conversion was refused");
}

int main(void)
{
    test_known_encodings();
    test_nan();
    test_round_trip_exhaustive();
    test_saturation_and_rounding();
    test_reduce();
    test_map();
    test_convert_fp16();
    test_convert_bf16();
    test_convert_buffer();

    if (failures) {
        printf("p4_fp8: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("p4_fp8: all checks passed\n");
    return 0;
}
