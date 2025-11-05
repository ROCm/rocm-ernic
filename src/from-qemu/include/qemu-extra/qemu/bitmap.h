#ifndef QEMU_BITMAP_H
#define QEMU_BITMAP_H

#include <stddef.h>

/*
 * QEMU Bitmap Operations Stubs
 */

unsigned long *bitmap_new(int nbits);
void bitmap_free(unsigned long *bitmap);
void set_bit(int nr, unsigned long *addr);
void clear_bit(int nr, unsigned long *addr);
int test_bit(int nr, const unsigned long *addr);
int find_first_zero_bit(const unsigned long *addr, unsigned long size);

#endif /* QEMU_BITMAP_H */
