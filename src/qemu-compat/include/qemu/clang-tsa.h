/*
 * Clang thread-safety analysis annotations
 *
 * Named after QEMU's include/qemu/clang-tsa.h, which these follow.  They let
 * clang check, at compile time, that a lock is held where the code says it
 * must be: QemuMutex is a capability, its lock and unlock functions acquire
 * and release it, and the analysis (-Wthread-safety, set for clang builds in
 * ErnicClangCompilerOptions.cmake) follows it through each function.
 *
 *   TSA_REQUIRES(m)  on a function: its caller holds m
 *   TSA_EXCLUDES(m)  on a function: its caller does not hold m, because the
 *                    function takes it, or waits for a thread that does
 *   TSA_GUARDED_BY(m) on a field: read or written only with m held
 *   TSA_ACQUIRED_AFTER(m) on a lock: taken, if with m, only while m is held
 *
 * Every macro expands to nothing for other compilers.  The field annotations
 * also expand to nothing before clang 23: in C, clang 18 can't name another
 * field of the struct in one at all, and clang 19 to 21 take the name as a
 * lone variable rather than a field of the object the code locks, so they
 * would report correctly locked code.  The analysis ties a field to its lock
 * only if the lock is declared first, so a struct declares its locks before
 * the fields they guard.
 *
 * Code the analysis can't follow, such as the initialisation of an object
 * no other thread can see yet, or a read the field's comment explains is
 * safe without the lock, is wrapped in ROCM_ERNIC_WARN_THREAD_SAFETY_OFF and
 * _ON (shared/rocm-ernic-warnings.h), around as little as possible.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_CLANG_TSA_H
#define QEMU_CLANG_TSA_H

#ifdef __clang__
#define TSA_ATTR(x) __attribute__((x))
#else
#define TSA_ATTR(x)
#endif

#if defined(__clang__) && __clang_major__ >= 23
#define TSA_FIELD_ATTR(x) __attribute__((x))
#else
#define TSA_FIELD_ATTR(x)
#endif

#define TSA_CAPABILITY(x) TSA_ATTR(capability(x))
#define TSA_REQUIRES(...) TSA_ATTR(requires_capability(__VA_ARGS__))
#define TSA_EXCLUDES(...) TSA_ATTR(locks_excluded(__VA_ARGS__))
#define TSA_ACQUIRE(...)  TSA_ATTR(acquire_capability(__VA_ARGS__))
#define TSA_RELEASE(...)  TSA_ATTR(release_capability(__VA_ARGS__))
/* The function returns ret when it has acquired the capability */
#define TSA_TRY_ACQUIRE(ret, ...) \
    TSA_ATTR(try_acquire_capability(ret, __VA_ARGS__))
/*
 * Only for the lock functions themselves, which call pthreads, whose own
 * functions aren't annotated.
 */
#define TSA_NO_TSA TSA_ATTR(no_thread_safety_analysis)

#define TSA_GUARDED_BY(x)       TSA_FIELD_ATTR(guarded_by(x))
#define TSA_ACQUIRED_AFTER(...) TSA_FIELD_ATTR(acquired_after(__VA_ARGS__))

#endif /* QEMU_CLANG_TSA_H */
