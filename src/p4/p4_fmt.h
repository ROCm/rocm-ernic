/*
 * p4_fmt.h — the floating-point formats the P4 conversion extern moves
 * between
 *
 * The fp8 arithmetic in p4_fp8.h is one format doing sums.  This is the
 * other half of what a reduction engine needs: getting data into and out
 * of that format, in either direction, because a collective's buffers
 * arrive as whatever the framework above it chose.
 *
 * Every format here is (sign, exponent, significand) with an exponent
 * bias, so one descriptor describes all of them and one encoder and one
 * decoder serve all of them.  That is deliberate: six hand-written codecs
 * is six places for a rounding or subnormal bug to hide, and the
 * conversion tests cross-check the generic path for E4M3 against
 * p4_fp8_from_float(), which was validated against ml_dtypes.
 *
 * Two conventions, and they follow from what a format can represent
 * rather than from taste:
 *
 *  - A magnitude too large for the destination becomes an infinity if the
 *    format has one, and saturates to the largest finite magnitude if it
 *    does not.  Saturating is what AMD's conversion instructions do and
 *    what a reduction needs: one overflowed term turning into an infinity
 *    that then poisons a sum loses everything else in it.
 *  - A NaN stays NaN if the format has a NaN encoding.  FP4 E2M1 has
 *    neither NaN nor infinity -- the OCP microscaling format spends all
 *    sixteen encodings on finite values -- so a NaN saturates there too,
 *    sign preserved.  That is lossy and cannot be otherwise; a format with
 *    no NaN cannot carry one.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_FMT_H
#define P4_FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The formats, as the wire values a table entry carries.  Numbered
 * explicitly because a guest's program names them and the numbering is
 * therefore ABI.  New formats are appended; none is ever renumbered.
 */
enum p4_fmt {
    P4_FMT_FP32 = 0,     /* IEEE 754 binary32:  1-8-23  */
    P4_FMT_FP16 = 1,     /* IEEE 754 binary16:  1-5-10  */
    P4_FMT_BF16 = 2,     /* bfloat16:           1-8-7   */
    P4_FMT_FP8_E4M3 = 3, /* OCP FP8 E4M3:       1-4-3, no infinity */
    P4_FMT_FP8_E5M2 = 4, /* OCP FP8 E5M2:       1-5-2   */
    P4_FMT_FP4_E2M1 = 5, /* OCP MXFP4 E2M1:     1-2-1, no infinity or NaN */
    P4_FMT_COUNT
};

/*
 * True when @f is a format this target has.
 *
 * Takes the enum rather than a byte: the value has to be checked before
 * anything narrows it, or a wider bad value validates as a narrow good
 * one and then indexes the table with the original.
 */
bool p4_fmt_valid(enum p4_fmt f);

/*
 * Bits one element occupies.  4 for FP4, which is the only one narrower
 * than a byte and so the only one whose elements are packed two to a
 * byte: element i is in byte i / 2, in the low nibble when i is even.
 * That is the order a little-endian host writes a packed array in, and
 * the order ROCm's own FP4 buffers use.
 */
unsigned p4_fmt_bits(enum p4_fmt f);

/* The format's name, for diagnostics. */
const char *p4_fmt_name(enum p4_fmt f);

/*
 * How many bytes @n elements of @f occupy, or 0 if that is not a whole
 * number of bytes -- which only an odd @n of a 4-bit format can be.
 */
size_t p4_fmt_bytes(enum p4_fmt f, size_t n);

/*
 * How many elements of @f fit exactly in @bytes, or 0 when @bytes is not
 * a whole number of them.
 */
size_t p4_fmt_elems(enum p4_fmt f, size_t bytes);

/* Element @i of the packed array at @base, as a float. */
float p4_fmt_decode(enum p4_fmt f, const uint8_t *base, size_t i);

/*
 * Write @v as element @i of the packed array at @base.  For FP4 this is a
 * read-modify-write of the byte, so the other nibble is preserved.
 */
void p4_fmt_encode(enum p4_fmt f, float v, uint8_t *base, size_t i);

/*
 * Convert @n elements from @src format at @in to @dst format at @out.
 *
 * Either direction: @dst may be wider than @src, narrower, or the same.
 * @out may not alias @in.  Returns false and writes nothing on a bad
 * argument, including a length that is not a whole number of bytes in
 * either format.
 */
bool p4_fmt_convert(enum p4_fmt src, enum p4_fmt dst, const uint8_t *in,
                    size_t n, uint8_t *out);

#endif /* P4_FMT_H */
