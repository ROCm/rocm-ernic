/*
 * QEMU compiler macros stub
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_COMPILER_H
#define QEMU_COMPILER_H

#include <stddef.h>

/* Branch prediction hints - no-ops in standalone */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/*
 * Pointer to the struct of type ``type`` that holds ``*ptr`` as its
 * ``member``.  The cast goes through void * because the result is aligned
 * for ``type`` by construction, which the compiler cannot see through the
 * char * arithmetic.
 */
#define container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#endif /* QEMU_COMPILER_H */
