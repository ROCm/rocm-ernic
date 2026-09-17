/*
 * QEMU compiler macros stub
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_COMPILER_H
#define QEMU_COMPILER_H

/* Branch prediction hints - no-ops in standalone */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#endif /* QEMU_COMPILER_H */
