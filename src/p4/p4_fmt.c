/*
 * p4_fmt.c — one encoder and one decoder for every format
 *
 * See p4_fmt.h for the conventions.  Everything here works on float32 bit
 * patterns rather than on arithmetic, so no libm is needed and a NaN
 * keeps its payload through a widening conversion.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>

#include "p4/p4_fmt.h"

#define F32_SIGN_MASK 0x80000000U
#define F32_ABS_MASK  0x7FFFFFFFU
#define F32_EXP_SHIFT 23
#define F32_MAN_MASK  0x007FFFFFU
#define F32_MAN_BITS  23
#define F32_BIAS      127
#define F32_INF_BITS  0x7F800000U
#define F32_QNAN_BITS 0x7FC00000U

/*
 * What a format is.  Sign is always one bit, so the width is
 * 1 + exp_bits + man_bits and is not stored twice.
 *
 * has_inf and has_nan are separate because E4M3 has the second without
 * the first: it spends the encoding an infinity would have taken on a
 * finite value, and keeps only the all-ones significand for NaN.
 */
struct fmt_desc {
    uint8_t exp_bits;
    uint8_t man_bits;
    int16_t bias;
    bool has_inf;
    bool has_nan;
    const char *name;
};

static const struct fmt_desc fmt_table[P4_FMT_COUNT] = {
    [P4_FMT_FP32] = {8, 23, 127, true, true, "fp32"},
    [P4_FMT_FP16] = {5, 10, 15, true, true, "fp16"},
    [P4_FMT_BF16] = {8, 7, 127, true, true, "bf16"},
    [P4_FMT_FP8_E4M3] = {4, 3, 7, false, true, "fp8_e4m3"},
    [P4_FMT_FP8_E5M2] = {5, 2, 15, true, true, "fp8_e5m2"},
    [P4_FMT_FP4_E2M1] = {2, 1, 1, false, false, "fp4_e2m1"},
};

bool p4_fmt_valid(enum p4_fmt f)
{
    /*
     * The whole value, not a narrowed copy of it.  Taking a uint8_t here
     * meant a caller's (enum p4_fmt)256 validated as format 0 and then
     * indexed fmt_table[] with 256 -- checking a range after narrowing
     * into it rather than before.
     *
     * Compared as unsigned so a negative value, which an enum may hold,
     * fails rather than passing a signed test.
     */
    return (unsigned)f < (unsigned)P4_FMT_COUNT;
}

unsigned p4_fmt_bits(enum p4_fmt f)
{
    const struct fmt_desc *d;

    if (!p4_fmt_valid(f)) {
        return 0;
    }
    d = &fmt_table[f];
    return 1U + d->exp_bits + d->man_bits;
}

const char *p4_fmt_name(enum p4_fmt f)
{
    return p4_fmt_valid(f) ? fmt_table[f].name : "?";
}

size_t p4_fmt_bytes(enum p4_fmt f, size_t n)
{
    unsigned bits = p4_fmt_bits(f);

    if (bits == 0) {
        return 0;
    }
    /* Only a 4-bit format can land mid-byte, and only on an odd count. */
    if (((n * bits) % 8U) != 0U) {
        return 0;
    }
    return n * bits / 8U;
}

size_t p4_fmt_elems(enum p4_fmt f, size_t bytes)
{
    unsigned bits = p4_fmt_bits(f);

    if (bits == 0) {
        return 0;
    }
    if (((bytes * 8U) % bits) != 0U) {
        return 0;
    }
    return bytes * 8U / bits;
}

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

/* The raw encoding of element @i, right-aligned in a uint32_t. */
static uint32_t fmt_load(enum p4_fmt f, const uint8_t *base, size_t i)
{
    switch (p4_fmt_bits(f)) {
    case 4:
        /* Two to a byte, low nibble first. */
        return (i & 1U) ? (uint32_t)(base[i / 2U] >> 4)
                        : (uint32_t)(base[i / 2U] & 0x0FU);
    case 8:
        return base[i];
    case 16:
        return (uint32_t)base[2U * i] | ((uint32_t)base[(2U * i) + 1U] << 8);
    default:
        return (uint32_t)base[4U * i] | ((uint32_t)base[(4U * i) + 1U] << 8) |
               ((uint32_t)base[(4U * i) + 2U] << 16) |
               ((uint32_t)base[(4U * i) + 3U] << 24);
    }
}

static void fmt_store(enum p4_fmt f, uint8_t *base, size_t i, uint32_t raw)
{
    switch (p4_fmt_bits(f)) {
    case 4:
        /* Read-modify-write: the other element in this byte is not ours. */
        if (i & 1U) {
            base[i / 2U] =
                (uint8_t)((base[i / 2U] & 0x0FU) | ((raw & 0x0FU) << 4));
        } else {
            base[i / 2U] = (uint8_t)((base[i / 2U] & 0xF0U) | (raw & 0x0FU));
        }
        break;
    case 8:
        base[i] = (uint8_t)raw;
        break;
    case 16:
        base[2U * i] = (uint8_t)(raw & 0xFFU);
        base[(2U * i) + 1U] = (uint8_t)((raw >> 8) & 0xFFU);
        break;
    default:
        base[4U * i] = (uint8_t)(raw & 0xFFU);
        base[(4U * i) + 1U] = (uint8_t)((raw >> 8) & 0xFFU);
        base[(4U * i) + 2U] = (uint8_t)((raw >> 16) & 0xFFU);
        base[(4U * i) + 3U] = (uint8_t)((raw >> 24) & 0xFFU);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* decode                                                              */
/* ------------------------------------------------------------------ */

/*
 * The float32 bits of @sign * @sig * 2^@e, exactly.
 *
 * Every format here has fewer significand bits than float32, so no
 * rounding is ever needed -- but the result is not always a float32
 * normal.  bf16 shares float32's exponent bias, so ITS subnormals are
 * float32 subnormals too, and building one by normalising into an
 * exponent field runs that field off the bottom.  fp16, E4M3, E5M2 and
 * E2M1 subnormals all sit inside float32's normal range, which is why
 * only bf16 showed it.
 */
static float make_f32(uint32_t sign, uint32_t sig, int32_t e)
{
    unsigned p = 0;
    int32_t field;

    if (sig == 0) {
        return bits_float(sign);
    }

    /* Put the significand's leading one at bit 23, so the value reads as
     * 1.f * 2^(e + 23).  A loop rather than a count-leading-zeros
     * builtin, which is a GNU extension. */
    while ((sig >> p) > 1U) {
        p++;
    }
    if (p < F32_MAN_BITS) {
        sig <<= (F32_MAN_BITS - p);
        e -= (int32_t)(F32_MAN_BITS - p);
    } else if (p > F32_MAN_BITS) {
        sig >>= (p - F32_MAN_BITS);
        e += (int32_t)(p - F32_MAN_BITS);
    }

    field = e + (int32_t)F32_MAN_BITS + F32_BIAS;
    if (field >= 1) {
        return bits_float(sign | ((uint32_t)field << F32_EXP_SHIFT) |
                          (sig & F32_MAN_MASK));
    }

    /* A float32 subnormal.  Shifting cannot lose a bit: the formats that
     * reach here have at most 7 significant bits and float32 keeps 23. */
    {
        uint32_t shift = (uint32_t)(1 - field);

        if (shift > F32_MAN_BITS) {
            return bits_float(sign);
        }
        return bits_float(sign | (sig >> shift));
    }
}

static float fmt_decode_raw(const struct fmt_desc *d, uint32_t raw)
{
    const uint32_t man_mask = (1U << d->man_bits) - 1U;
    const uint32_t exp_mask = (1U << d->exp_bits) - 1U;
    const uint32_t sign =
        ((raw >> (d->exp_bits + d->man_bits)) & 1U) ? F32_SIGN_MASK : 0U;
    const uint32_t exp = (raw >> d->man_bits) & exp_mask;
    const uint32_t man = raw & man_mask;

    if (exp == exp_mask) {
        if (d->has_inf) {
            /* The payload moves up with the significand, so a NaN stays a
             * NaN rather than quietly becoming an infinity. */
            return bits_float(sign | F32_INF_BITS |
                              (man << (F32_MAN_BITS - d->man_bits)));
        }
        if (d->has_nan && man == man_mask) {
            return bits_float(sign | F32_QNAN_BITS);
        }
        /* Otherwise this exponent holds ordinary finite values. */
    }

    /*
     * Both cases as significand * 2^exponent, which make_f32() turns into
     * whatever float32 shape the value needs.  A subnormal is the same
     * expression with the implicit leading one absent and the exponent
     * pinned one above its field.
     */
    if (exp == 0) {
        return make_f32(sign, man, 1 - d->bias - (int32_t)d->man_bits);
    }

    return make_f32(sign, (1U << d->man_bits) | man,
                    (int32_t)exp - d->bias - (int32_t)d->man_bits);
}

float p4_fmt_decode(enum p4_fmt f, const uint8_t *base, size_t i)
{
    if (!p4_fmt_valid(f) || !base) {
        return 0.0F;
    }
    if (f == P4_FMT_FP32) {
        return bits_float(fmt_load(f, base, i)); /* identity */
    }
    return fmt_decode_raw(&fmt_table[f], fmt_load(f, base, i));
}

/* ------------------------------------------------------------------ */
/* encode                                                              */
/* ------------------------------------------------------------------ */

/*
 * Thresholds a format's encoder compares against, derived from the
 * descriptor once rather than per element.
 */
struct fmt_enc {
    const struct fmt_desc *d;
    uint32_t max_raw; /* largest finite encoding, without its sign */
    uint32_t nan_raw; /* NaN encoding, or max_raw when there is none */
    uint32_t inf_raw; /* infinity encoding, or max_raw when there is none */
    uint32_t saturate_f32; /* |v| at or above this leaves the finite range */
};

static void fmt_enc_init(struct fmt_enc *e, const struct fmt_desc *d)
{
    const uint32_t man_mask = (1U << d->man_bits) - 1U;
    const uint32_t exp_mask = (1U << d->exp_bits) - 1U;
    uint32_t max_exp;
    uint32_t max_man;
    float max_v;
    float step;

    e->d = d;

    if (d->has_inf) {
        /* The all-ones exponent is infinity and NaN, so the largest
         * finite value is one exponent below it. */
        max_exp = exp_mask - 1U;
        max_man = man_mask;
        e->inf_raw = exp_mask << d->man_bits;
        e->nan_raw = e->inf_raw | (1U << (d->man_bits - 1U));
    } else if (d->has_nan) {
        /* E4M3: the all-ones exponent is finite except for the all-ones
         * significand, which is the only NaN. */
        max_exp = exp_mask;
        max_man = man_mask - 1U;
        e->nan_raw = (exp_mask << d->man_bits) | man_mask;
        e->inf_raw = (max_exp << d->man_bits) | max_man;
    } else {
        /* E2M1: every encoding is a finite number. */
        max_exp = exp_mask;
        max_man = man_mask;
        e->nan_raw = (max_exp << d->man_bits) | max_man;
        e->inf_raw = e->nan_raw;
    }
    e->max_raw = (max_exp << d->man_bits) | max_man;

    max_v = fmt_decode_raw(d, e->max_raw);
    /* The step at the largest exponent, which is what the round-to-
     * nearest boundary above max_v is half of. */
    step = bits_float(
        (uint32_t)((int32_t)max_exp - d->bias - d->man_bits + F32_BIAS)
        << F32_EXP_SHIFT);
    e->saturate_f32 = float_bits(max_v + (step * 0.5F));
}

/* Shift @v right by @n, rounding to nearest with ties to even. */
static uint32_t rne_shift(uint32_t v, unsigned n)
{
    uint32_t keep;
    uint32_t half;
    uint32_t rest;

    if (n == 0) {
        return v;
    }
    if (n >= 32) {
        return 0;
    }
    keep = v >> n;
    half = (v >> (n - 1U)) & 1U;
    rest = v & ((1U << (n - 1U)) - 1U);
    if (half && (rest || (keep & 1U))) {
        keep++;
    }
    return keep;
}

/*
 * Round @v into @e's format.
 *
 * Done as significand * 2^exponent rather than by shifting the float32
 * fields in place, for the same reason make_f32() exists: a float32
 * subnormal has no implicit leading one, and a destination whose own
 * subnormals reach below float32's normal range -- bf16, which shares
 * float32's bias -- needs both ends handled.  Getting that wrong flushed
 * every bf16 subnormal to zero.
 */
static uint32_t fmt_encode_raw(const struct fmt_enc *e, float v)
{
    const struct fmt_desc *d = e->d;
    const uint32_t bits = float_bits(v);
    const uint32_t mag = bits & F32_ABS_MASK;
    const uint32_t sign_bit =
        (bits & F32_SIGN_MASK) ? (1U << (d->exp_bits + d->man_bits)) : 0U;
    const uint32_t man_mask = (1U << d->man_bits) - 1U;
    /* Exponent of the least significant bit of the smallest subnormal. */
    const int32_t emin = 1 - d->bias - (int32_t)d->man_bits;
    uint32_t sig;
    uint32_t q;
    int32_t src_e;
    int32_t msb = 0;
    int32_t unbiased;
    int32_t field;
    int32_t shift;

    if (mag > F32_INF_BITS) {
        /*
         * A NaN.  It encodes to the format's canonical quiet NaN with no
         * sign, which is what p4_fp8_from_float() has always done and
         * what keeps one convention in the tree rather than two; the sign
         * of a NaN carries no meaning.  A format with no NaN encoding
         * cannot carry it at all, so it saturates like an overflow does,
         * and there the sign IS meaningful and is kept -- see p4_fmt.h.
         */
        return d->has_nan ? e->nan_raw : (sign_bit | e->max_raw);
    }
    if (mag >= e->saturate_f32) {
        /* Out of the finite range, infinity included: an infinity, or the
         * largest finite value for a format that has none. */
        return sign_bit | (d->has_inf ? e->inf_raw : e->max_raw);
    }
    if (mag == 0) {
        return sign_bit;
    }

    if ((mag >> F32_EXP_SHIFT) == 0) {
        /* float32 subnormal: no implicit leading one. */
        sig = mag;
        src_e = -126 - (int32_t)F32_MAN_BITS;
    } else {
        sig = (mag & F32_MAN_MASK) | (1U << F32_EXP_SHIFT);
        src_e =
            (int32_t)(mag >> F32_EXP_SHIFT) - F32_BIAS - (int32_t)F32_MAN_BITS;
    }

    while ((sig >> msb) > 1U) {
        msb++;
    }
    unbiased = src_e + msb;
    field = unbiased + d->bias;

    /*
     * A normal keeps man_bits + 1 significant bits; a subnormal is
     * instead pinned to the smallest representable step.  A negative
     * shift is a widening conversion and is exact.
     */
    shift = (field >= 1) ? (msb - (int32_t)d->man_bits) : (emin - src_e);
    if (shift > 0) {
        q = rne_shift(sig, (unsigned)shift);
    } else {
        q = sig << (unsigned)(-shift);
    }

    if (field < 1) {
        /* Subnormal, or rounded up out of the subnormals onto the
         * smallest normal -- which is the same bit pattern either way. */
        return sign_bit | q;
    }
    if (q > (man_mask | (man_mask + 1U))) {
        /* Rounding carried into the exponent. */
        q >>= 1;
        field++;
    }
    if ((((uint32_t)field << d->man_bits) | (q & man_mask)) > e->max_raw) {
        return sign_bit | (d->has_inf ? e->inf_raw : e->max_raw);
    }
    return sign_bit | ((uint32_t)field << d->man_bits) | (q & man_mask);
}

void p4_fmt_encode(enum p4_fmt f, float v, uint8_t *base, size_t i)
{
    struct fmt_enc e;

    if (!p4_fmt_valid(f) || !base) {
        return;
    }
    if (f == P4_FMT_FP32) {
        fmt_store(f, base, i, float_bits(v)); /* identity */
        return;
    }
    fmt_enc_init(&e, &fmt_table[f]);
    fmt_store(f, base, i, fmt_encode_raw(&e, v));
}

bool p4_fmt_convert(enum p4_fmt src, enum p4_fmt dst, const uint8_t *in,
                    size_t n, uint8_t *out)
{
    struct fmt_enc e;
    bool dst_is_f32;

    if (!in || !out) {
        return false;
    }
    if (!p4_fmt_valid(src) || !p4_fmt_valid(dst)) {
        return false;
    }
    /* A half-byte at either end is not an array of either format. */
    if (p4_fmt_bytes(src, n) == 0 && n != 0) {
        return false;
    }
    if (p4_fmt_bytes(dst, n) == 0 && n != 0) {
        return false;
    }

    dst_is_f32 = (dst == P4_FMT_FP32);
    if (!dst_is_f32) {
        fmt_enc_init(&e, &fmt_table[dst]);
    }

    for (size_t i = 0; i < n; i++) {
        float v = p4_fmt_decode(src, in, i);

        fmt_store(dst, out, i,
                  dst_is_f32 ? float_bits(v) : fmt_encode_raw(&e, v));
    }
    return true;
}
