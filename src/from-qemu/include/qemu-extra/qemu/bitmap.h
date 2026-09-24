/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_BITMAP_H
#define QEMU_BITMAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * QEMU Bitmap Operations Stubs
 *
 * Bit indices and the bitmap size are uint32_t, the type of the resource
 * handles these bitmaps track. The storage word is unsigned long because
 * the resource manager's RdmaRmResTbl declares its bitmap that way; the
 * implementation measures the word's width rather than assuming it.
 *
 * find_first_zero_bit() takes its size as uint64_t so that callers holding
 * the size in a size_t pass it without narrowing. It aborts if the size
 * exceeds UINT32_MAX, since such a map cannot be indexed by the uint32_t
 * it returns.
 */

_Static_assert(SIZE_MAX <= UINT64_MAX,
               "a size_t bitmap size must convert to uint64_t without loss");

unsigned long *bitmap_new(uint32_t nbits);
void bitmap_free(unsigned long *bitmap);
void set_bit(uint32_t nr, unsigned long *addr);
void clear_bit(uint32_t nr, unsigned long *addr);
bool test_bit(uint32_t nr, const unsigned long *addr);
uint32_t find_first_zero_bit(const unsigned long *addr, uint64_t size);

#endif /* QEMU_BITMAP_H */
