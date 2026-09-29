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
 *
 * The unevaluated comparison makes the compiler diagnose a ``ptr`` whose
 * type does not match ``member``, which the casts alone would accept.
 * QEMU does this with a GNU statement expression and typeof; a comparison
 * inside sizeof does the same in standard C11.  A void * ``ptr`` still
 * passes, as it compares equal to any object pointer.
 */
#define container_of(ptr, type, member)           \
    ((void)sizeof((ptr) == &((type *)0)->member), \
     (type *)(void *)((char *)(ptr) - offsetof(type, member)))

#endif /* QEMU_COMPILER_H */
