/* This content was copied and modified from
 * the HDF5 C library's src/H5warnings.h file
 *
 * (https://github.com/HDFGroup/hdf5)
 *
 * HDF5 copyright:
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the LICENSE file, which can be found at the root of the source code       *
 * distribution tree, or in https://www.hdfgroup.org/licenses.               *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * AMD changes:
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ROCM_ERNIC_WARNINGS_H
#define ROCM_ERNIC_WARNINGS_H

/*
 * Macros for suppressing warnings
 *
 * These macros can be used to suppress compiler warnings that are difficult
 * or impossible to engineer around. By suppressing these warnings, we make
 * it easier to identify warnings created by new code and can build with
 * -Werror in CI to ensure new warnings don't get added to the code.
 *
 * USAGE:
 *
 * The macros are used in ON/OFF pairs. (i.e. ROCM_ERNIC_WARN_FOO_(ON|OFF)).
 * To suppress a warning, add the OFF macro before the offending line(s) of
 * code, and to turn it back on, add the ON macro.
 *
 *      ROCM_ERNIC_WARN_FOO_OFF
 *      ROCM_ERNIC_WARN_BAR_OFF
 *      code_that_raises_warnings();
 *      ROCM_ERNIC_WARN_BAR_ON
 *      ROCM_ERNIC_WARN_FOO_ON
 *
 * Since the warning macros work by pushing and popping diagnostic contexts,
 * warnings should be switched back on in reverse order. In practice, the ON
 * macro is just a generic pop, so it doesn't matter which one gets used or
 * in what order they get called, but using the wrong macro will make it
 * harder to reason about the code.
 *
 * Suppression macro pairs should span the minimum amount of code that
 * quiets the warning. Ideally, a single line of code. Compilers in the
 * past limited diagnostic pragmas to outside of functions, but this
 * is no longer the case in any compiler we care about.
 *
 * LIMITATIONS:
 *
 * The warning macros use compiler pragmas, which limits where the suppression
 * macros can be used. The most obvious limitation is that they often can't
 * be used inside macros.
 *
 * ADDING A NEW MACRO:
 *
 * - First, ask yourself if we really need a new warning macro. Do your best
 *   to actually correct warnings and not just suppress them. If a compiler
 *   update starts raising a bunch of new warnings we are unlikely to fix,
 *   consider shutting down the warning flag globally using the compiler
 *   flags (like -Wno-foo-warning).
 *
 * - The macro should have a helpful comment about why the warning should
 *   be suppressed and not corrected.
 *
 * - The macro name should reflect the actual problem, not the name of the
 *   compiler warning option. Names are of the form
 *   ROCM_ERNIC_WARN_<THING>_(ON|OFF).
 *
 * - Be careful with the ifdefs. clang defines __GNUC__, for example, so
 *   you can't simply check for that if you have a gcc-specific warning.
 *   Check for compiler version numbers in the macros to avoid warnings
 *   about undefined diagnostics in older compilers.
 *
 * - Add the new macro to the list in the .clang-format file.
 *
 * - Remove any macro that is no longer used.
 */

/* Macros for enabling/disabling particular gcc / clang warnings
 *
 * (see the following web-sites for more info:
 *      http://www.dbp-consulting.com/tutorials/SuppressingGCCWarnings.html
 *      http://gcc.gnu.org/onlinedocs/gcc/Diagnostic-Pragmas.html#Diagnostic-Pragmas
 *
 * _Pragma is C++11/C99 and should work with all compilers, though permitted
 * placement may vary
 *
 * "GCC" in the pragma works for both clang/llvm and gcc
 */
#define ROCM_ERNIC_WARN_JOINSTR(x, y) x y
#define ROCM_ERNIC_WARN_DO_PRAGMA(x)  _Pragma(#x)
#define ROCM_ERNIC_WARN_PRAGMA(x)     ROCM_ERNIC_WARN_DO_PRAGMA(GCC diagnostic x)

/* clang-format off */
#if (defined(__GNUC__) || defined(__clang__))
    #define ROCM_ERNIC_WARN_OFF(x) ROCM_ERNIC_WARN_PRAGMA(push) ROCM_ERNIC_WARN_PRAGMA(ignored ROCM_ERNIC_WARN_JOINSTR("-W", x))
    #define ROCM_ERNIC_WARN_ON(x)  ROCM_ERNIC_WARN_PRAGMA(pop)
#endif
/* clang-format on */

/*********************
 * SPECIFIC WARNINGS *
 *********************/

/* Suppress warnings about casting away const
 *
 * struct iovec serves readv() as well as writev(), so iov_base is not
 * const even though writev() only reads through it. Filling in an iovec
 * from a const buffer therefore needs a cast that drops the qualifier.
 */
#if defined(__clang__) || defined(__GNUC__)
#define ROCM_ERNIC_WARN_CAST_AWAY_CONST_OFF ROCM_ERNIC_WARN_OFF("cast-qual")
#define ROCM_ERNIC_WARN_CAST_AWAY_CONST_ON  ROCM_ERNIC_WARN_ON("cast-qual")
#else
#define ROCM_ERNIC_WARN_CAST_AWAY_CONST_OFF
#define ROCM_ERNIC_WARN_CAST_AWAY_CONST_ON
#endif

/* Suppress warnings about self-referential macros
 *
 * Some system headers define a macro that expands to a name spelled the
 * same as itself (glibc's sa_handler, for example), which clang reports
 * as a recursive macro expansion wherever the name is used.
 */
#if defined(__clang__)
#define ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_OFF \
    ROCM_ERNIC_WARN_OFF("disabled-macro-expansion")
#define ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_ON \
    ROCM_ERNIC_WARN_ON("disabled-macro-expansion")
#else
#define ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_OFF
#define ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_ON
#endif

/* Suppress warnings about reserved identifiers
 *
 * Sanitizer runtimes look up hooks such as __asan_default_options() by
 * name, so defining one requires a reserved identifier.
 */
#if defined(__clang__)
#define ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF \
    ROCM_ERNIC_WARN_OFF("reserved-identifier")
#define ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON \
    ROCM_ERNIC_WARN_ON("reserved-identifier")
#else
#define ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
#define ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON
#endif

/* The following warnings are raised by vendored QEMU code that a test
 * #includes to reach its static functions. That code is not ours to fix,
 * so these should only ever surround vendored code.
 */

/* Suppress warnings about implicit conversions that may change a value */
#if defined(__clang__) || defined(__GNUC__)
#define ROCM_ERNIC_WARN_IMPLICIT_CONVERSION_OFF \
    ROCM_ERNIC_WARN_OFF("conversion")
#define ROCM_ERNIC_WARN_IMPLICIT_CONVERSION_ON ROCM_ERNIC_WARN_ON("conversion")
#else
#define ROCM_ERNIC_WARN_IMPLICIT_CONVERSION_OFF
#define ROCM_ERNIC_WARN_IMPLICIT_CONVERSION_ON
#endif

/* Suppress warnings about implicit conversions that may change a sign */
#if defined(__clang__) || defined(__GNUC__)
#define ROCM_ERNIC_WARN_SIGN_CONVERSION_OFF \
    ROCM_ERNIC_WARN_OFF("sign-conversion")
#define ROCM_ERNIC_WARN_SIGN_CONVERSION_ON ROCM_ERNIC_WARN_ON("sign-conversion")
#else
#define ROCM_ERNIC_WARN_SIGN_CONVERSION_OFF
#define ROCM_ERNIC_WARN_SIGN_CONVERSION_ON
#endif

/* Suppress warnings about printf-style arguments that don't match their
 * conversion specifiers
 */
#if defined(__clang__) || defined(__GNUC__)
#define ROCM_ERNIC_WARN_FORMAT_MISMATCH_OFF ROCM_ERNIC_WARN_OFF("format")
#define ROCM_ERNIC_WARN_FORMAT_MISMATCH_ON  ROCM_ERNIC_WARN_ON("format")
#else
#define ROCM_ERNIC_WARN_FORMAT_MISMATCH_OFF
#define ROCM_ERNIC_WARN_FORMAT_MISMATCH_ON
#endif

/* Suppress warnings about passing a non-void pointer to %p
 *
 * clang reports this under -Wformat-pedantic, not -Wformat.
 */
#if defined(__clang__)
#define ROCM_ERNIC_WARN_FORMAT_NONVOID_POINTER_OFF \
    ROCM_ERNIC_WARN_OFF("format-pedantic")
#define ROCM_ERNIC_WARN_FORMAT_NONVOID_POINTER_ON \
    ROCM_ERNIC_WARN_ON("format-pedantic")
#else
#define ROCM_ERNIC_WARN_FORMAT_NONVOID_POINTER_OFF
#define ROCM_ERNIC_WARN_FORMAT_NONVOID_POINTER_ON
#endif

/* Suppress warnings about arithmetic on void pointers */
#if defined(__clang__) || defined(__GNUC__)
#define ROCM_ERNIC_WARN_VOID_POINTER_ARITH_OFF \
    ROCM_ERNIC_WARN_OFF("pointer-arith")
#define ROCM_ERNIC_WARN_VOID_POINTER_ARITH_ON \
    ROCM_ERNIC_WARN_ON("pointer-arith")
#else
#define ROCM_ERNIC_WARN_VOID_POINTER_ARITH_OFF
#define ROCM_ERNIC_WARN_VOID_POINTER_ARITH_ON
#endif

#endif /* ROCM_ERNIC_WARNINGS_H */
