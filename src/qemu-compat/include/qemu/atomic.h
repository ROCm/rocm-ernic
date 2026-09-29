/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_ATOMIC_H
#define QEMU_ATOMIC_H

#include <stdint.h>

/*
 * QEMU Atomic Operations Stubs
 * Note: In QEMU these work with many types, but for standalone we keep it
 * simple
 */

/* Implementation functions */
void qatomic_set_impl(int *ptr, int val);
int qatomic_read_impl(const int *ptr);
void qatomic_inc_impl(int *ptr);
void qatomic_dec_impl(int *ptr);
void qatomic_add_impl(int *ptr, int val);
void qatomic_sub_impl(int *ptr, int val);

/* Generic macros that work with any integer pointer type */
#define qatomic_inc(ptr)      qatomic_inc_impl((int *)(ptr))
#define qatomic_dec(ptr)      qatomic_dec_impl((int *)(ptr))
#define qatomic_add(ptr, val) qatomic_add_impl((int *)(ptr), (val))
#define qatomic_sub(ptr, val) qatomic_sub_impl((int *)(ptr), (val))
#define qatomic_set(ptr, val) qatomic_set_impl((int *)(ptr), (val))
#define qatomic_read(ptr)     qatomic_read_impl((const int *)(ptr))

#endif /* QEMU_ATOMIC_H */
