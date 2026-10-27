/*
 * p4_fp8.c — OCP FP8 E4M3 arithmetic
 *
 * The conversions are integer bit manipulation on the float32
 * representation rather than calls into libm. That keeps the result
 * independent of the floating-point rounding mode -- these values go on a
 * wire and into guest memory, so the same inputs have to give the same
 * byte on every host -- and it means this file adds no link dependency.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>

#include "p4/p4_fp8.h"

#define FP8_SIGN_MASK 0x80u
#define FP8_EXP_MASK  0x78u
#define FP8_MAN_MASK  0x07u
#define FP8_EXP_SHIFT 3
#define FP8_BIAS      7
#define FP8_EXP_MAX   15

#define F32_SIGN_MASK 0x80000000u
#define F32_ABS_MASK  0x7FFFFFFFu
#define F32_EXP_SHIFT 23
#define F32_MAN_MASK  0x007FFFFFu
#define F32_BIAS      127
#define F32_INF_BITS  0x7F800000u
#define F32_NAN_BITS  0x7FC00000u

/*
 * Bit patterns of the two thresholds, so the comparisons are integer and
 * need no libm. 0x43E80000 is 464.0, the midpoint between the largest
 * representable value (448) and where the next one would have been, so
 * anything at or above it saturates. 0x3A800000 is 2^-10, half the
 * smallest subnormal, below which everything flushes to zero.
 */
#define F32_SATURATE_BITS 0x43E80000u
#define F32_FLUSH_BITS    0x3A800000u

/* Shift from a 24-bit significand down to the fp8's four. */
#define FP8_NORMAL_SHIFT 20

static uint32_t float_bits(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static float bits_float(uint32_t bits)
{
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/*
 * Shift @sig right by @shift, rounding to nearest with ties to even. The
 * result can be one larger than @sig >> @shift, which is the carry the
 * callers have to absorb.
 */
static uint32_t rne_shift(uint32_t sig, unsigned shift)
{
    uint32_t result, rem, half;

    if (shift == 0)
        return sig;
    /* Every caller bounds the shift well below this; the guard is here so
     * the shifts below can never be by the width of the type. */
    if (shift >= 32)
        return 0;

    result = sig >> shift;
    half = 1u << (shift - 1);
    rem = sig & ((1u << shift) - 1u);

    if (rem > half || (rem == half && (result & 1u) != 0u))
        result++;

    return result;
}

bool p4_fp8_is_nan(uint8_t v)
{
    return (v & (uint8_t)~FP8_SIGN_MASK) == P4_FP8_NAN;
}

float p4_fp8_to_float(uint8_t v)
{
    const uint32_t sign = (v & FP8_SIGN_MASK) ? F32_SIGN_MASK : 0u;
    const uint32_t exp = (v & FP8_EXP_MASK) >> FP8_EXP_SHIFT;
    uint32_t man = v & FP8_MAN_MASK;

    if (p4_fp8_is_nan(v))
        return bits_float(F32_NAN_BITS);

    if (exp == 0) {
        unsigned msb;

        if (man == 0)
            return bits_float(sign);

        /*
         * A subnormal is man * 2^-9, which float32 represents as a normal.
         * Shifting the leading one of the mantissa up to the implicit
         * position gives the exponent to use with it.
         */
        msb = (man >= 4u) ? 2u : (man >= 2u ? 1u : 0u);
        return bits_float(
            sign | ((uint32_t)((int32_t)msb - 9 + F32_BIAS) << F32_EXP_SHIFT) |
            ((man << (F32_EXP_SHIFT - msb)) & F32_MAN_MASK));
    }

    /* The larger bias first: exp is unsigned, and may be below FP8_BIAS. */
    return bits_float(sign | ((exp + F32_BIAS - FP8_BIAS) << F32_EXP_SHIFT) |
                      (man << FP8_NORMAL_SHIFT));
}

uint8_t p4_fp8_from_float(float v)
{
    const uint32_t bits = float_bits(v);
    const uint32_t mag = bits & F32_ABS_MASK;
    const uint8_t sign = (bits & F32_SIGN_MASK) ? (uint8_t)FP8_SIGN_MASK : 0u;
    int32_t field;
    uint32_t sig, q;
    unsigned shift;

    if (mag > F32_INF_BITS)
        return P4_FP8_NAN;

    /* Infinity lands here too: E4M3 has none, so it saturates. */
    if (mag >= F32_SATURATE_BITS)
        return (uint8_t)(sign | P4_FP8_MAX);

    if (mag < F32_FLUSH_BITS)
        return sign;

    /* Restore the implicit leading one. Anything that reached here is a
     * float32 normal, since a float32 subnormal is far below the flush
     * threshold. */
    sig = (mag & F32_MAN_MASK) | (1u << F32_EXP_SHIFT);
    field = (int32_t)(mag >> F32_EXP_SHIFT) - F32_BIAS + FP8_BIAS;

    if (field < 1) {
        /* Subnormal: the exponent field is pinned at zero and the extra
         * exponent range is paid for out of the mantissa. */
        shift = (unsigned)(FP8_NORMAL_SHIFT + 1 - field);
        q = rne_shift(sig, shift);
        if (q >= 8u) {
            /* Rounded up out of the subnormals onto the smallest normal. */
            return (uint8_t)(sign | (1u << FP8_EXP_SHIFT));
        }
        return (uint8_t)(sign | (q & FP8_MAN_MASK));
    }

    q = rne_shift(sig, FP8_NORMAL_SHIFT);
    if (q >= 16u) {
        /* The mantissa carried out of [8, 16), which is an exponent step. */
        q = 8u;
        field++;
    }

    /* The top exponent field stops at a mantissa of six: seven is NaN. */
    if (field > FP8_EXP_MAX || (field == FP8_EXP_MAX && q > 14u))
        return (uint8_t)(sign | P4_FP8_MAX);

    return (uint8_t)(sign | ((uint32_t)field << FP8_EXP_SHIFT) |
                     ((q - 8u) & FP8_MAN_MASK));
}

/* Combine one pair of operands. Shared so map and reduce cannot drift. */
static bool fp8_combine(enum p4_fp8_op op, float fa, float fb, float *out)
{
    switch (op) {
    case P4_FP8_OP_ADD:
        *out = fa + fb;
        return true;
    case P4_FP8_OP_MUL:
        *out = fa * fb;
        return true;
    case P4_FP8_OP_MAX:
        *out = (fa > fb) ? fa : fb;
        return true;
    default:
        return false;
    }
}

bool p4_fp8_map(enum p4_fp8_op op, const uint8_t *a, const uint8_t *b, size_t n,
                uint8_t *out)
{
    float probe;

    if (!out)
        return false;
    if (n != 0 && (!a || !b))
        return false;
    /* Reject an unknown operation before writing anything, so a bad one
     * cannot leave a partly transformed buffer behind. */
    if (!fp8_combine(op, 0.0f, 0.0f, &probe))
        return false;

    for (size_t i = 0; i < n; i++) {
        float r;

        /* Elementwise, so a NaN spoils its own element and no other. */
        if (p4_fp8_is_nan(a[i]) || p4_fp8_is_nan(b[i])) {
            out[i] = P4_FP8_NAN;
            continue;
        }
        (void)fp8_combine(op, p4_fp8_to_float(a[i]), p4_fp8_to_float(b[i]), &r);
        out[i] = p4_fp8_from_float(r);
    }
    return true;
}

bool p4_fp8_reduce(enum p4_fp8_op op, const uint8_t *a, const uint8_t *b,
                   size_t n, uint8_t *out)
{
    float acc;

    if (!out)
        return false;
    if (n != 0 && (!a || !b))
        return false;

    switch (op) {
    case P4_FP8_OP_ADD:
    case P4_FP8_OP_MUL:
        acc = 0.0f;
        break;
    case P4_FP8_OP_MAX:
        acc = -P4_FP8_MAX_VAL;
        break;
    default:
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        float fa, fb;

        /* A NaN anywhere in either operand makes the whole result NaN, so
         * it reaches the caller rather than being absorbed by the sum. */
        if (p4_fp8_is_nan(a[i]) || p4_fp8_is_nan(b[i])) {
            *out = P4_FP8_NAN;
            return true;
        }

        fa = p4_fp8_to_float(a[i]);
        fb = p4_fp8_to_float(b[i]);

        switch (op) {
        case P4_FP8_OP_ADD:
            acc += fa + fb;
            break;
        case P4_FP8_OP_MUL:
            acc += fa * fb;
            break;
        case P4_FP8_OP_MAX:
            if (fa > acc)
                acc = fa;
            if (fb > acc)
                acc = fb;
            break;
        default:
            return false;
        }
    }

    *out = p4_fp8_from_float(acc);
    return true;
}
