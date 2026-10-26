/*
 * Stub for QEMU threading API
 * RDMA backend uses mutexes for thread safety
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_THREAD_H
#define QEMU_THREAD_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qemu/clang-tsa.h" // IWYU pragma: export

/*
 * QemuMutex - wrapper around pthread_mutex, and a capability for clang's
 * thread-safety analysis (see qemu/clang-tsa.h)
 */
typedef struct TSA_CAPABILITY("mutex") QemuMutex {
    pthread_mutex_t lock;
    bool initialized;
} QemuMutex;

/* QemuCond - wrapper around pthread_cond */
typedef struct QemuCond {
    pthread_cond_t cond;
} QemuCond;

/* QemuThread - wrapper around pthread_t */
typedef struct QemuThread {
    pthread_t thread;
} QemuThread;

/*
 * Reports that the pthread function what failed with err, then aborts.  The
 * wrappers here call it whenever a pthread call fails, other than for the
 * results they document as normal: a failure means a bug, such as unlocking
 * a mutex the thread doesn't hold, or corrupted memory, so carrying on isn't
 * safe.
 *
 * It's defined here, with the wrappers, so that a program using them needs
 * nothing more than this header.  It writes to stderr itself rather than
 * through error_report(), which a quieter log level would silence.
 */
static inline _Noreturn void qemu_thread_fatal(int err, const char *what)
{
    fprintf(stderr, "%s failed: %s\n", what, strerror(err));
    abort();
}

/* Aborts through qemu_thread_fatal() unless err, what's result, is 0 */
static inline void qemu_thread_check(int err, const char *what)
{
    if (err != 0) {
        qemu_thread_fatal(err, what);
    }
}

/*
 * Mutex operations
 *
 * Without NDEBUG, the macro that also turns off assert(), a mutex is
 * error-checking (PTHREAD_MUTEX_ERRORCHECK): a thread locking a mutex it
 * already holds, or unlocking one it doesn't hold, gets an error, so the
 * wrappers abort rather than deadlock or carry on.  A release build, which
 * runs the per-packet path, keeps the default mutex, which checks neither.
 */
static inline void qemu_mutex_init(QemuMutex *mutex)
{
#ifdef NDEBUG
    qemu_thread_check(pthread_mutex_init(&mutex->lock, NULL),
                      "pthread_mutex_init");
#else
    pthread_mutexattr_t attr;

    qemu_thread_check(pthread_mutexattr_init(&attr), "pthread_mutexattr_init");
    qemu_thread_check(
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK),
        "pthread_mutexattr_settype");
    qemu_thread_check(pthread_mutex_init(&mutex->lock, &attr),
                      "pthread_mutex_init");
    qemu_thread_check(pthread_mutexattr_destroy(&attr),
                      "pthread_mutexattr_destroy");
#endif
    mutex->initialized = true;
}

/* Destroying a locked mutex fails, and so aborts, in every build */
static inline void qemu_mutex_destroy(QemuMutex *mutex)
{
    if (mutex->initialized) {
        qemu_thread_check(pthread_mutex_destroy(&mutex->lock),
                          "pthread_mutex_destroy");
        mutex->initialized = false;
    }
}

static inline void qemu_mutex_lock(QemuMutex *mutex)
    TSA_ACQUIRE(mutex) TSA_NO_TSA
{
    qemu_thread_check(pthread_mutex_lock(&mutex->lock), "pthread_mutex_lock");
}

/*
 * Returns 0 once it holds mutex, or EBUSY if mutex is locked, whether by
 * another thread or by this one
 */
static inline int qemu_mutex_trylock(QemuMutex *mutex)
    TSA_TRY_ACQUIRE(0, mutex) TSA_NO_TSA
{
    int err = pthread_mutex_trylock(&mutex->lock);

    if (err != EBUSY) {
        qemu_thread_check(err, "pthread_mutex_trylock");
    }
    return err;
}

static inline void qemu_mutex_unlock(QemuMutex *mutex)
    TSA_RELEASE(mutex) TSA_NO_TSA
{
    qemu_thread_check(pthread_mutex_unlock(&mutex->lock),
                      "pthread_mutex_unlock");
}

/*
 * Thread creation and management.  qemu_thread_create() returns
 * pthread_create()'s result, since a thread can fail to start for want of
 * resources (EAGAIN); qemu_thread_join() aborts if pthread_join() fails.
 */
int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg);
void qemu_thread_join(QemuThread *thread);

/*
 * Condition variable operations.  As in QEMU, timed waits are measured
 * against CLOCK_MONOTONIC, so a change to the wall clock neither cuts a wait
 * short nor stretches it.  If the clock can't be set, the wrapper aborts
 * rather than measure CLOCK_MONOTONIC deadlines against the wall clock.
 * Any other failure aborts too, such as waiting without holding mutex.
 */
static inline void qemu_cond_init(QemuCond *cond)
{
    pthread_condattr_t attr;

    qemu_thread_check(pthread_condattr_init(&attr), "pthread_condattr_init");
    qemu_thread_check(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC),
                      "pthread_condattr_setclock");
    qemu_thread_check(pthread_cond_init(&cond->cond, &attr),
                      "pthread_cond_init");
    qemu_thread_check(pthread_condattr_destroy(&attr),
                      "pthread_condattr_destroy");
}

static inline void qemu_cond_destroy(QemuCond *cond)
{
    qemu_thread_check(pthread_cond_destroy(&cond->cond),
                      "pthread_cond_destroy");
}

static inline void qemu_cond_signal(QemuCond *cond)
{
    qemu_thread_check(pthread_cond_signal(&cond->cond), "pthread_cond_signal");
}

/*
 * Wait until signalled.  Like pthread_cond_wait(), it can also return without
 * a signal, so callers recheck their condition in a loop.
 */
static inline void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
    TSA_REQUIRES(mutex)
{
    qemu_thread_check(pthread_cond_wait(&cond->cond, &mutex->lock),
                      "pthread_cond_wait");
}

/*
 * Wait until signalled or until deadline, a CLOCK_MONOTONIC time.  Returns
 * false once the deadline has passed.  Like pthread_cond_wait(), it can also
 * return without a signal, so callers recheck their condition in a loop.
 */
static inline bool qemu_cond_timedwait_until(QemuCond *cond, QemuMutex *mutex,
                                             const struct timespec *deadline)
    TSA_REQUIRES(mutex)
{
    int err = pthread_cond_timedwait(&cond->cond, &mutex->lock, deadline);

    if (err == ETIMEDOUT) {
        return false;
    }
    qemu_thread_check(err, "pthread_cond_timedwait");
    return true;
}

#endif /* QEMU_THREAD_H */
