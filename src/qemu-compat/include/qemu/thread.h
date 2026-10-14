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

/* QemuRecMutex - recursive mutex */
typedef struct QemuRecMutex {
    pthread_mutex_t lock;
} QemuRecMutex;

/* QemuEvent - simple event for thread signaling */
typedef struct QemuEvent {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool value;
} QemuEvent;

/* QemuThread - wrapper around pthread_t */
typedef struct QemuThread {
    pthread_t thread;
} QemuThread;

/* Function pointers for mutex operations */
typedef void (*QemuMutexLockFunc)(QemuMutex *m, const char *f, int l);
typedef int (*QemuMutexTrylockFunc)(QemuMutex *m, const char *f, int l);
typedef void (*QemuCondWaitFunc)(QemuCond *c, QemuMutex *m, const char *f,
                                 int l);
typedef bool (*QemuCondTimedWaitFunc)(QemuCond *c, QemuMutex *m, int ms,
                                      const char *f, int l);

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

static inline void qemu_mutex_lock_impl(QemuMutex *mutex, const char *file,
                                        int line)
{
    (void)file;
    (void)line;
    pthread_mutex_lock(&mutex->lock);
}

static inline int qemu_mutex_trylock_impl(QemuMutex *mutex, const char *file,
                                          int line)
{
    (void)file;
    (void)line;
    return pthread_mutex_trylock(&mutex->lock);
}

static inline void qemu_mutex_unlock_impl(QemuMutex *mutex, const char *file,
                                          int line)
{
    (void)file;
    (void)line;
    pthread_mutex_unlock(&mutex->lock);
}

/* Thread creation flags */
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

/* Lock guard helper macro - simplified */
#define WITH_QEMU_LOCK_GUARD(lock)                        \
    for (int _guard = (qemu_mutex_lock(lock), 1); _guard; \
         _guard = 0, qemu_mutex_unlock(lock))

/* Simplified macros without file/line tracking */
#define qemu_mutex_lock(m)    qemu_mutex_lock_impl(m, __FILE__, __LINE__)
#define qemu_mutex_trylock(m) qemu_mutex_trylock_impl(m, __FILE__, __LINE__)
#define qemu_mutex_unlock(m)  qemu_mutex_unlock_impl(m, __FILE__, __LINE__)

/* QEMU_LOCK_GUARD - automatic lock/unlock using C99 for-loop scope */
#define QEMU_LOCK_GUARD(lock) WITH_QEMU_LOCK_GUARD(lock)

/* Thread creation and management */
int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode);
_Noreturn void qemu_thread_exit(void *retval);
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

static inline void qemu_cond_broadcast(QemuCond *cond)
{
    pthread_cond_broadcast(&cond->cond);
}

static inline void qemu_cond_wait_impl(QemuCond *cond, QemuMutex *mutex,
                                       const char *file, int line)
{
    (void)file;
    (void)line;
    pthread_cond_wait(&cond->cond, &mutex->lock);
}

#define qemu_cond_wait(c, m) qemu_cond_wait_impl(c, m, __FILE__, __LINE__)

/*
 * Wait until signalled or until deadline, a CLOCK_MONOTONIC time.  Returns
 * false once the deadline has passed.  Like qemu_cond_wait(), it can also
 * return without a signal, so callers recheck their condition in a loop.
 */
static inline bool qemu_cond_timedwait_until_impl(
    QemuCond *cond, QemuMutex *mutex, const struct timespec *deadline,
    const char *file, int line)
{
    (void)file;
    (void)line;
    return pthread_cond_timedwait(&cond->cond, &mutex->lock, deadline) !=
           ETIMEDOUT;
}

#define qemu_cond_timedwait_until(c, m, d) \
    qemu_cond_timedwait_until_impl(c, m, d, __FILE__, __LINE__)

/* Thread-safety analysis attributes - no-ops for GCC */
#define TSA_NO_TSA
#define TSA_GUARDED_BY(x)
#define TSA_REQUIRES(x)

#endif /* QEMU_THREAD_H */
