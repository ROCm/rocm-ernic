/*
 * Unit tests for SipHash-2-4 (ernic_siphash.h).
 *
 * The expected values are the reference implementation's 64 test vectors
 * (key 00 01 .. 0f, message 00 01 .. of each length from 0 to 63 bytes),
 * read as little-endian numbers. They cover every length of the final
 * partial block and inputs of up to seven whole blocks. Each message is
 * hashed from a heap buffer of exactly its length, so a sanitizer sees a
 * read past its end, and the empty one from NULL, which the API allows.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ernic_siphash.h"

static const uint64_t vectors[64] = {
    UINT64_C(0x726fdb47dd0e0e31), UINT64_C(0x74f839c593dc67fd),
    UINT64_C(0x0d6c8009d9a94f5a), UINT64_C(0x85676696d7fb7e2d),
    UINT64_C(0xcf2794e0277187b7), UINT64_C(0x18765564cd99a68d),
    UINT64_C(0xcbc9466e58fee3ce), UINT64_C(0xab0200f58b01d137),
    UINT64_C(0x93f5f5799a932462), UINT64_C(0x9e0082df0ba9e4b0),
    UINT64_C(0x7a5dbbc594ddb9f3), UINT64_C(0xf4b32f46226bada7),
    UINT64_C(0x751e8fbc860ee5fb), UINT64_C(0x14ea5627c0843d90),
    UINT64_C(0xf723ca908e7af2ee), UINT64_C(0xa129ca6149be45e5),
    UINT64_C(0x3f2acc7f57c29bdb), UINT64_C(0x699ae9f52cbe4794),
    UINT64_C(0x4bc1b3f0968dd39c), UINT64_C(0xbb6dc91da77961bd),
    UINT64_C(0xbed65cf21aa2ee98), UINT64_C(0xd0f2cbb02e3b67c7),
    UINT64_C(0x93536795e3a33e88), UINT64_C(0xa80c038ccd5ccec8),
    UINT64_C(0xb8ad50c6f649af94), UINT64_C(0xbce192de8a85b8ea),
    UINT64_C(0x17d835b85bbb15f3), UINT64_C(0x2f2e6163076bcfad),
    UINT64_C(0xde4daaaca71dc9a5), UINT64_C(0xa6a2506687956571),
    UINT64_C(0xad87a3535c49ef28), UINT64_C(0x32d892fad841c342),
    UINT64_C(0x7127512f72f27cce), UINT64_C(0xa7f32346f95978e3),
    UINT64_C(0x12e0b01abb051238), UINT64_C(0x15e034d40fa197ae),
    UINT64_C(0x314dffbe0815a3b4), UINT64_C(0x027990f029623981),
    UINT64_C(0xcadcd4e59ef40c4d), UINT64_C(0x9abfd8766a33735c),
    UINT64_C(0x0e3ea96b5304a7d0), UINT64_C(0xad0c42d6fc585992),
    UINT64_C(0x187306c89bc215a9), UINT64_C(0xd4a60abcf3792b95),
    UINT64_C(0xf935451de4f21df2), UINT64_C(0xa9538f0419755787),
    UINT64_C(0xdb9acddff56ca510), UINT64_C(0xd06c98cd5c0975eb),
    UINT64_C(0xe612a3cb9ecba951), UINT64_C(0xc766e62cfcadaf96),
    UINT64_C(0xee64435a9752fe72), UINT64_C(0xa192d576b245165a),
    UINT64_C(0x0a8787bf8ecb74b2), UINT64_C(0x81b3e73d20b49b6f),
    UINT64_C(0x7fa8220ba3b2ecea), UINT64_C(0x245731c13ca42499),
    UINT64_C(0xb78dbfaf3a8d83bd), UINT64_C(0xea1ad565322a1a0b),
    UINT64_C(0x60e61c23a3795013), UINT64_C(0x6606d7e446282b93),
    UINT64_C(0x6ca4ecb15c5f91e1), UINT64_C(0x9f626da15c9625f3),
    UINT64_C(0xe51b38608ef25f57), UINT64_C(0x958a324ceb064572),
};

int main(void)
{
    uint8_t key[ERNIC_SIPHASH_KEY_LEN];
    int failures = 0;

    for (size_t i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;

    for (size_t len = 0; len < sizeof(vectors) / sizeof(vectors[0]); len++) {
        uint8_t *msg = NULL;

        if (len > 0) {
            msg = malloc(len);
            if (msg == NULL) {
                printf("FAIL %zu bytes: out of memory\n", len);
                return 1;
            }
            for (size_t i = 0; i < len; i++)
                msg[i] = (uint8_t)i;
        }

        uint64_t got = ernic_siphash24(key, msg, len);
        free(msg);
        if (got != vectors[len]) {
            printf("FAIL %zu bytes: %#018" PRIx64 ", want %#018" PRIx64 "\n",
                   len, got, vectors[len]);
            failures++;
        }
    }

    if (failures)
        printf("\n%d failure(s)\n", failures);
    else
        printf("all siphash tests passed\n");
    return failures;
}
