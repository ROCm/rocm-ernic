/*
 * p4_value.h — the one shape every P4 operand takes
 *
 * A value is big-endian and right-aligned in a buffer wide enough for any
 * field the loader accepts, whatever that field's own width.  Because
 * every value is the same shape, ordering two of them is one memcmp
 * whatever widths they came from, and a match unit can compare a table
 * entry's key against a packet's without either knowing the other's width.
 *
 * Header-only and inline: this is on the per-packet path, and both the
 * executor and the match units need it.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_VALUE_H
#define P4_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "p4_program.h"

/* Wide enough for any field the loader accepts. */
#define P4_VALUE_BYTES ((P4_MAX_FIELD_BITS + 7) / 8)

/*
 * A value: big-endian, right-aligned in a fixed-width buffer, so every
 * operand of every width is the same shape.  Two values are therefore
 * comparable with memcmp, and arithmetic is a byte walk from the bottom.
 */
struct p4_value {
    uint8_t b[P4_VALUE_BYTES];
};

static inline void value_zero(struct p4_value *v)
{
    memset(v->b, 0, sizeof(v->b));
}

/* Read @bits bits starting at @bit_off of @src into a value. */
static inline void bits_get(const uint8_t *src, uint32_t bit_off, uint16_t bits,
                            struct p4_value *out)
{
    value_zero(out);
    for (uint16_t i = 0; i < bits; i++) {
        uint32_t b = bit_off + i;
        uint32_t bit = ((uint32_t)src[b / 8u] >> (7u - (b % 8u))) & 1u;
        /* Bit i from the left of the field is bit (bits-1-i) from the
         * right of the value, which is right-aligned in the buffer. */
        uint32_t from_right = (uint32_t)(bits - 1u - i);
        size_t byte = P4_VALUE_BYTES - 1u - (from_right / 8u);

        if (bit)
            out->b[byte] |= (uint8_t)(1u << (from_right % 8u));
    }
}

/* Write the low @bits bits of @v at @bit_off of @dst. */
static inline void bits_set(uint8_t *dst, uint32_t bit_off, uint16_t bits,
                            const struct p4_value *v)
{
    for (uint16_t i = 0; i < bits; i++) {
        uint32_t b = bit_off + i;
        const uint8_t mask = (uint8_t)(1u << (7u - (b % 8u)));
        uint32_t from_right = (uint32_t)(bits - 1u - i);
        size_t byte = P4_VALUE_BYTES - 1u - (from_right / 8u);
        uint32_t bit = ((uint32_t)v->b[byte] >> (from_right % 8u)) & 1u;

        if (bit)
            dst[b / 8u] |= mask;
        else
            dst[b / 8u] &= (uint8_t)~mask;
    }
}

/* Both operands are right-aligned and the same width, so this orders them. */
static inline int value_cmp(const struct p4_value *a, const struct p4_value *b)
{
    return memcmp(a->b, b->b, P4_VALUE_BYTES);
}

static inline bool value_nonzero(const struct p4_value *v)
{
    for (size_t i = 0; i < P4_VALUE_BYTES; i++) {
        if (v->b[i])
            return true;
    }
    return false;
}

/* Wrap-around is what a fixed-width field does; the result is truncated to
 * the destination's width when it is stored. */
static inline void value_add(const struct p4_value *a, const struct p4_value *b,
                             struct p4_value *out)
{
    unsigned carry = 0;

    for (size_t i = P4_VALUE_BYTES; i-- > 0;) {
        unsigned sum = (unsigned)a->b[i] + b->b[i] + carry;

        out->b[i] = (uint8_t)(sum & 0xFFu);
        carry = sum >> 8;
    }
}

static inline void value_sub(const struct p4_value *a, const struct p4_value *b,
                             struct p4_value *out)
{
    unsigned borrow = 0;

    for (size_t i = P4_VALUE_BYTES; i-- > 0;) {
        unsigned d = (unsigned)a->b[i] - b->b[i] - borrow;

        out->b[i] = (uint8_t)(d & 0xFFu);
        borrow = (d >> 8) & 1u;
    }
}

static inline uint32_t value_u32(const struct p4_value *v)
{
    uint32_t x = 0;

    for (size_t i = P4_VALUE_BYTES - 4u; i < P4_VALUE_BYTES; i++)
        x = (x << 8) | v->b[i];
    return x;
}

static inline void value_set_u32(struct p4_value *v, uint32_t x)
{
    value_zero(v);
    for (size_t i = 0; i < 4u; i++)
        v->b[P4_VALUE_BYTES - 1u - i] = (uint8_t)(x >> (8u * i));
}


#endif /* P4_VALUE_H */
