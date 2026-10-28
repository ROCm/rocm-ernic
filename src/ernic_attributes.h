/*
 * ernic_attributes.h - compiler attributes C11 has no spelling for
 *
 * Each macro expands to a GCC and Clang attribute, and to nothing for
 * other compilers, where the code is the same without it. What C11 can
 * say itself is written directly: _Alignas, _Noreturn, _Static_assert.
 * Clang's thread-safety annotations are in qemu/clang-tsa.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_ATTRIBUTES_H
#define ERNIC_ATTRIBUTES_H

/*
 * ERNIC_PRINTF_FORMAT(fmt, args) - the function's parameter fmt (counting
 * from 1) is a printf format string, and its arguments start at parameter
 * args, so -Wformat checks each call as it does printf()'s. args is 0 for
 * a function that takes a va_list.
 *
 * The attribute's names are the reserved spellings, so a macro named
 * printf or format can't break it.
 */
#if defined(__GNUC__)
#define ERNIC_PRINTF_FORMAT(fmt, args) \
    __attribute__((__format__(__printf__, fmt, args)))
#else
#define ERNIC_PRINTF_FORMAT(fmt, args)
#endif

#endif /* ERNIC_ATTRIBUTES_H */
