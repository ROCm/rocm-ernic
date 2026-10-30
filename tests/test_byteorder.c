/*
 * Unit tests for the byte-order accessors in byteorder.h.
 *
 * The server and most of the tests read and build wire data through these,
 * so a mistake in one would be made identically on both sides and every
 * other test would still pass. Each case is pinned here against a byte image
 * written out by hand, at an unaligned offset, with guard bytes either side
 * so a store that strays past its field is caught too.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "byteorder.h"

#define GUARD 0xa5U
#define OFF   3U /* unaligned for every width */

struct bo_case {
    const char *name;
    unsigned int bits; /* 16, 32 or 64 */
    bool big;          /* big-endian, else little-endian */
    uint64_t value;
    uint8_t bytes[8]; /* the first bits / 8 are the field */
};

static uint64_t get(const struct bo_case *tc, const uint8_t *p)
{
    switch (tc->bits) {
    case 16:
        return tc->big ? ernic_get_be16(p) : ernic_get_le16(p);
    case 32:
        return tc->big ? ernic_get_be32(p) : ernic_get_le32(p);
    default:
        return tc->big ? ernic_get_be64(p) : ernic_get_le64(p);
    }
}

static void put(const struct bo_case *tc, uint8_t *p)
{
    switch (tc->bits) {
    case 16:
        if (tc->big) {
            ernic_put_be16(p, (uint16_t)tc->value);
        } else {
            ernic_put_le16(p, (uint16_t)tc->value);
        }
        break;
    case 32:
        if (tc->big) {
            ernic_put_be32(p, (uint32_t)tc->value);
        } else {
            ernic_put_le32(p, (uint32_t)tc->value);
        }
        break;
    default:
        if (tc->big) {
            ernic_put_be64(p, tc->value);
        } else {
            ernic_put_le64(p, tc->value);
        }
        break;
    }
}

static int run_case(const struct bo_case *tc)
{
    size_t n = tc->bits / 8U;
    uint8_t buf[OFF + sizeof(tc->bytes) + OFF];
    uint8_t want[sizeof(buf)];
    uint64_t got;

    if (tc->bits != 16 && tc->bits != 32 && tc->bits != 64) {
        printf("FAIL     %-18s: no %u-bit accessors\n", tc->name, tc->bits);
        return 1;
    }

    /* put: exactly the field's bytes, in order, and nothing either side. */
    memset(want, GUARD, sizeof(want));
    memcpy(want + OFF, tc->bytes, n);
    memset(buf, GUARD, sizeof(buf));
    put(tc, buf + OFF);
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != want[i]) {
            printf("FAIL put %-18s: byte %zu is %#04x, expected %#04x\n",
                   tc->name, i, buf[i], want[i]);
            return 1;
        }
    }

    /* get: the value back from the same image, guards unread. */
    memset(buf, GUARD, sizeof(buf));
    memcpy(buf + OFF, tc->bytes, n);
    got = get(tc, buf + OFF);
    if (got != tc->value) {
        printf("FAIL get %-18s: %#" PRIx64 ", expected %#" PRIx64 "\n",
               tc->name, got, tc->value);
        return 1;
    }
    return 0;
}

int main(void)
{
    /* For each width and order: a value whose bytes all differ, so any
     * swap or misplacement shows; zero; all ones; and only the top bit,
     * which a shift done in a signed int instead of the field's own type
     * would get wrong. */
    static const struct bo_case cases[] = {
        {"le16-pattern", 16, false, 0x0102U, {0x02, 0x01}},
        {"le16-zero", 16, false, 0, {0x00, 0x00}},
        {"le16-ones", 16, false, 0xffffU, {0xff, 0xff}},
        {"le16-top-bit", 16, false, 0x8000U, {0x00, 0x80}},
        {"be16-pattern", 16, true, 0x0102U, {0x01, 0x02}},
        {"be16-zero", 16, true, 0, {0x00, 0x00}},
        {"be16-ones", 16, true, 0xffffU, {0xff, 0xff}},
        {"be16-top-bit", 16, true, 0x8000U, {0x80, 0x00}},
        {"le32-pattern", 32, false, 0x01020304U, {0x04, 0x03, 0x02, 0x01}},
        {"le32-zero", 32, false, 0, {0x00, 0x00, 0x00, 0x00}},
        {"le32-ones", 32, false, 0xffffffffU, {0xff, 0xff, 0xff, 0xff}},
        {"le32-top-bit", 32, false, 0x80000000U, {0x00, 0x00, 0x00, 0x80}},
        {"be32-pattern", 32, true, 0x01020304U, {0x01, 0x02, 0x03, 0x04}},
        {"be32-zero", 32, true, 0, {0x00, 0x00, 0x00, 0x00}},
        {"be32-ones", 32, true, 0xffffffffU, {0xff, 0xff, 0xff, 0xff}},
        {"be32-top-bit", 32, true, 0x80000000U, {0x80, 0x00, 0x00, 0x00}},
        {"le64-pattern",
         64,
         false,
         UINT64_C(0x0102030405060708),
         {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}},
        {"le64-zero", 64, false, 0, {0}},
        {"le64-ones",
         64,
         false,
         UINT64_MAX,
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
        {"le64-top-bit",
         64,
         false,
         UINT64_C(0x8000000000000000),
         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}},
        {"le64-low-half-top",
         64,
         false,
         UINT64_C(0x0000000080000000),
         {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00}},
        {"be64-pattern",
         64,
         true,
         UINT64_C(0x0102030405060708),
         {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}},
        {"be64-zero", 64, true, 0, {0}},
        {"be64-ones",
         64,
         true,
         UINT64_MAX,
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
        {"be64-top-bit",
         64,
         true,
         UINT64_C(0x8000000000000000),
         {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {"be64-low-half-top",
         64,
         true,
         UINT64_C(0x0000000080000000),
         {0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00}},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    for (size_t i = 0; i < n; i++) {
        failures += run_case(&cases[i]);
    }

    if (failures) {
        printf("byteorder: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("byteorder: all %zu cases passed\n", n);
    return 0;
}
