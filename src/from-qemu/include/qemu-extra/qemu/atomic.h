#ifndef QEMU_ATOMIC_H
#define QEMU_ATOMIC_H

#include <stdint.h>

/*
 * QEMU Atomic Operations Stubs
 * Note: In QEMU these work with many types, but for standalone we keep it simple
 */

/* Implementation functions */
void __qatomic_set_impl(int *ptr, int val);
int __qatomic_read_impl(const int *ptr);
void __qatomic_inc_impl(int *ptr);
void __qatomic_dec_impl(int *ptr);
void __qatomic_add_impl(int *ptr, int val);
void __qatomic_sub_impl(int *ptr, int val);

/* Generic macros that work with any integer pointer type */
#define qatomic_inc(ptr) __qatomic_inc_impl((int *)(ptr))
#define qatomic_dec(ptr) __qatomic_dec_impl((int *)(ptr))
#define qatomic_add(ptr, val) __qatomic_add_impl((int *)(ptr), (val))
#define qatomic_sub(ptr, val) __qatomic_sub_impl((int *)(ptr), (val))
#define qatomic_set(ptr, val) __qatomic_set_impl((int *)(ptr), (val))
#define qatomic_read(ptr) __qatomic_read_impl((const int *)(ptr))

#endif /* QEMU_ATOMIC_H */

