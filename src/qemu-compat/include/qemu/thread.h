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
#include <time.h>

/* QemuMutex - wrapper around pthread_mutex */
typedef struct QemuMutex {
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

/* Mutex operations */
static inline void qemu_mutex_init(QemuMutex *mutex)
{
    pthread_mutex_init(&mutex->lock, NULL);
    mutex->initialized = true;
}

static inline void qemu_mutex_destroy(QemuMutex *mutex)
{
    if (mutex->initialized) {
        pthread_mutex_destroy(&mutex->lock);
        mutex->initialized = false;
    }
}

static inline void qemu_mutex_lock(QemuMutex *mutex)
{
    pthread_mutex_lock(&mutex->lock);
}

static inline int qemu_mutex_trylock(QemuMutex *mutex)
{
    return pthread_mutex_trylock(&mutex->lock);
}

static inline void qemu_mutex_unlock(QemuMutex *mutex)
{
    pthread_mutex_unlock(&mutex->lock);
}

/* QEMU_LOCK_GUARD - automatic lock/unlock using C99 for-loop scope */
#define QEMU_LOCK_GUARD(lock)                             \
    for (int _guard = (qemu_mutex_lock(lock), 1); _guard; \
         _guard = 0, qemu_mutex_unlock(lock))

/* Thread creation and management */
int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg);
void qemu_thread_join(QemuThread *thread);

/*
 * Condition variable operations.  As in QEMU, timed waits are measured
 * against CLOCK_MONOTONIC, so a change to the wall clock neither cuts a wait
 * short nor stretches it.
 */
static inline void qemu_cond_init(QemuCond *cond)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cond->cond, &attr);
    pthread_condattr_destroy(&attr);
}

static inline void qemu_cond_destroy(QemuCond *cond)
{
    pthread_cond_destroy(&cond->cond);
}

static inline void qemu_cond_signal(QemuCond *cond)
{
    pthread_cond_signal(&cond->cond);
}

/*
 * Wait until signalled or until deadline, a CLOCK_MONOTONIC time.  Returns
 * false once the deadline has passed.  Like pthread_cond_wait(), it can also
 * return without a signal, so callers recheck their condition in a loop.
 */
static inline bool qemu_cond_timedwait_until(QemuCond *cond, QemuMutex *mutex,
                                             const struct timespec *deadline)
{
    return pthread_cond_timedwait(&cond->cond, &mutex->lock, deadline) !=
           ETIMEDOUT;
}

#endif /* QEMU_THREAD_H */
