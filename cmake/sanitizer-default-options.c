/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Default run-time options for the sanitizers
 *
 * ErnicSanitizers.cmake adds this file to every program that it builds
 * with a sanitizer. Each sanitizer's runtime calls its function, if the
 * program defines it, when the program starts. The options in the
 * ASAN_OPTIONS, UBSAN_OPTIONS, and TSAN_OPTIONS environment variables are
 * applied after these, so they can override them.
 *
 * A program that needs other AddressSanitizer defaults can define
 * ERNIC_ASAN_EXTRA_OPTIONS as a string of options that starts with ':'.
 * They're applied after the ones below, so they can override them.
 *
 * The runtimes choose these functions' names, which are reserved
 * identifiers.
 */

#include "rocm-ernic-warnings.h"

#ifndef ERNIC_ASAN_EXTRA_OPTIONS
#define ERNIC_ASAN_EXTRA_OPTIONS ""
#endif

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF

/* AddressSanitizer
 *
 *   - detect_invalid_pointer_pairs=2 turns on GCC's pointer-compare and
 *     pointer-subtract checks, including for null pointers.
 *   - detect_stack_use_after_return=1 finds uses of a function's local
 *     variables after it returns.
 *   - strict_string_checks=1 checks that the strings passed to the C
 *     library's string functions are terminated.
 */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_invalid_pointer_pairs=2:"
           "detect_stack_use_after_return=1:"
           "strict_string_checks=1" ERNIC_ASAN_EXTRA_OPTIONS;
}

/* UndefinedBehaviorSanitizer
 *
 *   - print_stacktrace=1 shows where each report came from.
 */
const char *__ubsan_default_options(void);
const char *__ubsan_default_options(void)
{
    return "print_stacktrace=1";
}

/* ThreadSanitizer
 *
 *   - halt_on_error=1 stops the program at the first report, as the other
 *     sanitizers do. Otherwise it runs on, and only its exit status (66)
 *     shows that there was a report.
 *   - second_deadlock_stack=1 shows where both locks in a possible
 *     deadlock were taken, not just one.
 */
const char *__tsan_default_options(void);
const char *__tsan_default_options(void)
{
    return "halt_on_error=1:"
           "second_deadlock_stack=1";
}

ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
