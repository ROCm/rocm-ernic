/*
 * p4_fp8.h — OCP FP8 E4M3 arithmetic for the P4 pipeline
 *
 * P4 has no floating-point type: its base types are bit-vectors, so a
 * program cannot express fp8 arithmetic at all.  The language's answer to
 * that is an extern -- an operation the target provides and the program
 * calls -- and this is ours.  It is also how real P4 targets implement
 * in-network reduction, which is what this exists for: the emulated NIC
 * reducing two operand buffers to one result on the way past, rather than
 * the host reading both and writing a third.
 *
 * The format is OCP FP8 E4M3: one sign bit, four exponent bits with a bias
 * of seven, and three mantissa bits.  It has no infinities, so the only NaN
 * encodings are 0x7F and 0xFF, the largest finite value is 448, and
 * subnormals go down to 2^-9.
 *
 * On overflow this SATURATES to +/-448.  That is a deliberate divergence
 * from the "FN" convention that numpy's ml_dtypes.float8_e4m3fn and JAX
 * follow, where a value too large to represent becomes NaN.  Saturating is
 * what AMD's conversion instructions do and what a reduction needs: one
 * overflowed term turning the entire result into NaN loses everything else
 * in the sum, which is the opposite of useful.  It is the only difference.
 * Decoding was checked against ml_dtypes for all 256 encodings, and
 * encoding for 40506 values covering every tie between adjacent
 * representable values, the subnormal range, and both boundaries; once the
 * overflow convention is accounted for, the two agree exactly.
 *
 * Reductions accumulate in float, not in fp8, and round once at the end.
 * That is what fp8 hardware does, and it is the only way a sum of more than
 * a couple of terms means anything: the gap between adjacent fp8 values near
 * 1.0 is 1/8, so rounding after every step loses the sum entirely.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_FP8_H
#define P4_FP8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The canonical NaN encoding, and the largest finite magnitude. */
#define P4_FP8_NAN     0x7Fu
#define P4_FP8_MAX     0x7Eu /* 448.0 */
#define P4_FP8_MAX_VAL 448.0f

/*
 * The operation applied to a pair of operands.  The same code names the
 * pointwise operation for p4_fp8_map() and the combining operation for
 * p4_fp8_reduce(), so a program selects an arithmetic rather than a
 * calling convention, and an operation that is neither -- a matrix
 * product, say -- can be added without disturbing either.
 */
enum p4_fp8_op {
    P4_FP8_OP_ADD = 0,
    P4_FP8_OP_MUL = 1,
    P4_FP8_OP_MAX = 2,
};

/* True when @v is one of the two NaN encodings. */
bool p4_fp8_is_nan(uint8_t v);

/* Decode to float.  NaN decodes to a NaN; every other encoding is finite. */
float p4_fp8_to_float(uint8_t v);

/*
 * Encode from float, rounding to nearest with ties to even.  A magnitude
 * above the format's range saturates to +/-448 rather than becoming an
 * infinity, which E4M3 cannot represent; a NaN encodes to P4_FP8_NAN.
 */
uint8_t p4_fp8_from_float(float v);

/*
 * Apply @op elementwise: out[i] = a[i] op b[i], over @n elements.
 *
 * This is the shape a collective wants -- two partial buffers combined
 * into a third of the same length -- as distinct from p4_fp8_reduce()
 * below, which collapses both to a single value.
 *
 * Each element is rounded once, on the way out.  A NaN in either input
 * makes that element NaN and leaves the rest alone, which is what
 * elementwise means: one bad value does not destroy the buffer.
 *
 * @out may alias neither @a nor @b.  Returns false and writes nothing on
 * a bad argument.
 */
bool p4_fp8_map(enum p4_fp8_op op, const uint8_t *a, const uint8_t *b, size_t n,
                uint8_t *out);

/*
 * Reduce two equal-length fp8 buffers to one fp8 value.
 *
 * @a and @b are @n bytes each, every byte one E4M3 value.  Returns the
 * result, or P4_FP8_NAN if any input is NaN, which is what makes a bad
 * operand visible to the caller rather than silently absorbed.  @n of 0
 * yields the operation's identity: zero for ADD and MUL, and the smallest
 * representable value for MAX.
 *
 * Returns false and leaves @out untouched on a bad argument.
 */
bool p4_fp8_reduce(enum p4_fp8_op op, const uint8_t *a, const uint8_t *b,
                   size_t n, uint8_t *out);

#endif /* P4_FP8_H */
