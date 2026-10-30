/*
 * SipHash-2-4, a keyed hash for short inputs
 *
 * See ernic_siphash.h.  This follows the paper's description and the
 * reference implementation's structure: two compression rounds per
 * 8-byte block, four finalization rounds.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stddef.h>
#include <stdint.h>

#include "byteorder.h"
#include "ernic_siphash.h"

static uint64_t rotl64(uint64_t x, unsigned b)
{
    /* b is always one of SipRound's constants, 13 to 32, so neither shift
     * is by 0 or 64. */
    return (x << b) | (x >> (64U - b));
}

static void sipround(uint64_t v[4])
{
    v[0] += v[1];
    v[1] = rotl64(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl64(v[0], 32);
    v[2] += v[3];
    v[3] = rotl64(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = rotl64(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = rotl64(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl64(v[2], 32);
}

static void compress(uint64_t v[4], uint64_t m)
{
    v[3] ^= m;
    sipround(v);
    sipround(v);
    v[0] ^= m;
}

uint64_t ernic_siphash24(const uint8_t key[ERNIC_SIPHASH_KEY_LEN],
                         const void *data, size_t len)
{
    const uint8_t *p = data;
    uint64_t k0 = ernic_get_le64(key);
    uint64_t k1 = ernic_get_le64(key + 8);
    uint64_t v[4] = {
        k0 ^ UINT64_C(0x736f6d6570736575), /* "somepseu" */
        k1 ^ UINT64_C(0x646f72616e646f6d), /* "dorandom" */
        k0 ^ UINT64_C(0x6c7967656e657261), /* "lygenera" */
        k1 ^ UINT64_C(0x7465646279746573), /* "tedbytes" */
    };
    size_t whole = len - (len % 8);

    for (size_t i = 0; i < whole; i += 8) {
        compress(v, ernic_get_le64(p + i));
    }

    /* The last block: the 0 to 7 bytes left over, little-endian, with the
     * input's length (mod 256) in the top byte. */
    uint64_t last = (uint64_t)(len & 0xff) << 56;
    for (size_t i = 0; i < len % 8; i++) {
        last |= (uint64_t)p[whole + i] << (8 * i);
    }
    compress(v, last);

    v[2] ^= 0xff;
    for (int i = 0; i < 4; i++) {
        sipround(v);
    }
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}
