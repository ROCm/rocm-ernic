/*
 *
 * Unit tests for the bitmap and thread stubs in qemu_stubs.c.
 *
 * The bitmap's storage word is unsigned long, which is 32 or 64 bits
 * depending on the platform, so the cases use sizes around that width
 * rather than fixed numbers. Each map is filled one bit at a time and then
 * emptied from the top, with find_first_zero_bit() checked after every
 * step. Built with ASan, an index that strays past the allocation fails
 * the test.
 *
 * find_first_zero_bit() must refuse a map larger than UINT32_MAX bits,
 * since its uint32_t result could not index it. That case runs in a child
 * process, which is expected to die by SIGABRT.
 *
 * qemu_thread_create() must start each thread with every signal blocked
 * except those a fault raises, so the process's signals go to the main
 * thread, and must leave its caller's own mask as it was.  The thread must
 * already have its name when its start routine runs, cut short to what the
 * kernel keeps, and qemu_thread_set_name() must rename the calling thread.
 *
 * The mutex wrappers in qemu/thread.h must abort when a lock is misused,
 * each case in a child process like the oversize map: destroying a locked
 * mutex in every build, and, without NDEBUG, where mutexes are
 * error-checking, unlocking an unlocked mutex or locking one the thread
 * already holds. Under TSan these are skipped, since TSan reports the
 * misuse first. The results the wrappers treat as normal must not abort:
 * trylock on a mutex another thread holds, and a timed wait whose deadline
 * has passed.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

/* pthread_getname_np() is a GNU extension */
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

#include "qemu/bitmap.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"

static int failures;

static void fail(const char *what, uint32_t nbits, uint32_t bit)
{
    printf("FAIL %s (nbits=%" PRIu32 ", bit=%" PRIu32 ")\n", what, nbits, bit);
    failures++;
}

/* ---- Bitmap ---------------------------------------------------------- */

static void test_bitmap_size(uint32_t nbits)
{
    unsigned long *map = bitmap_new(nbits);

    if (!map) {
        fail("bitmap_new returned NULL", nbits, 0);
        return;
    }

    /* The resource manager passes the size as a size_t. */
    size_t size = nbits;

    if (find_first_zero_bit(map, size) != 0) {
        fail("empty map: first zero is not bit 0", nbits, 0);
    }

    for (uint32_t i = 0; i < nbits; i++) {
        if (test_bit(i, map)) {
            fail("bit set before set_bit", nbits, i);
        }
        set_bit(i, map);
        if (!test_bit(i, map)) {
            fail("bit clear after set_bit", nbits, i);
        }
        if (find_first_zero_bit(map, size) != i + 1) {
            fail("first zero is not the next bit", nbits, i);
        }
    }

    /* Full: every bit is set, so the result is the size itself. */
    if (find_first_zero_bit(map, size) != nbits) {
        fail("full map: first zero is not the size", nbits, nbits);
    }

    for (uint32_t i = nbits; i-- > 0;) {
        clear_bit(i, map);
        if (test_bit(i, map)) {
            fail("bit set after clear_bit", nbits, i);
        }
        if (i > 0 && !test_bit(i - 1, map)) {
            fail("clear_bit cleared the bit below", nbits, i);
        }
        if (find_first_zero_bit(map, size) != i) {
            fail("first zero is not the bit just cleared", nbits, i);
        }
    }

    /* The resource manager frees its maps with g_free(). */
    g_free(map);
}

static void test_bitmap_sizes(void)
{
    const uint32_t word = CHAR_BIT * sizeof(unsigned long);
    const uint32_t sizes[] = {
        0, 1, word - 1, word, word + 1, 2 * word, 2 * word + 1,
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        test_bitmap_size(sizes[i]);
    }
}

/*
 * Runs misuse in a child process, which must die by SIGABRT. An alarm ends
 * a child that hangs instead, such as one deadlocked on a mutex that doesn't
 * check its owner, which then fails the check.
 */
static void expect_abort(void (*misuse)(void), const char *what)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork failed\n", what);
        failures++;
        return;
    }
    if (pid == 0) {
        /* The abort message is expected; keep it out of the test log. */
        if (!freopen("/dev/null", "w", stderr)) {
            _exit(2);
        }
        alarm(30);
        misuse();
        _exit(0);
    }

    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: waitpid failed\n", what);
        failures++;
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        printf("FAIL %s did not abort\n", what);
        failures++;
    }
}

static void oversize_map(void)
{
    unsigned long word = 0;

    (void)find_first_zero_bit(&word, (uint64_t)UINT32_MAX + 1);
}

/* ---- Threads --------------------------------------------------------- */

/* The signal mask the thread found itself started with. */
static void *record_mask(void *arg)
{
    pthread_sigmask(SIG_BLOCK, NULL, (sigset_t *)arg);
    return NULL;
}

static void check_blocked(const sigset_t *mask, int signo, bool want,
                          const char *whose)
{
    if ((sigismember(mask, signo) == 1) != want) {
        printf("FAIL %s: signal %d is %sblocked\n", whose, signo,
               want ? "not " : "");
        failures++;
    }
}

static void test_thread_signal_mask(void)
{
    /* Blocked in the thread.  SIGUSR2 is also blocked in the caller, below,
     * so it has to stay that way there. */
    static const int blocked[] = {SIGINT,  SIGTERM, SIGHUP,  SIGQUIT, SIGPIPE,
                                  SIGUSR1, SIGUSR2, SIGALRM, SIGCHLD};
    /* Raised by a fault, so left unblocked. */
    static const int faults[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
    sigset_t caller;
    sigset_t before;
    sigset_t after;
    sigset_t in_thread;
    QemuThread thread;

    sigemptyset(&caller);
    sigaddset(&caller, SIGUSR2);
    pthread_sigmask(SIG_SETMASK, &caller, &before);

    sigemptyset(&in_thread);
    if (qemu_thread_create(&thread, "record-mask", record_mask, &in_thread) !=
        0) {
        printf("FAIL qemu_thread_create\n");
        failures++;
        pthread_sigmask(SIG_SETMASK, &before, NULL);
        return;
    }
    qemu_thread_join(&thread);
    pthread_sigmask(SIG_SETMASK, &before, &after);

    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        check_blocked(&in_thread, blocked[i], true, "new thread");
    }
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        check_blocked(&in_thread, faults[i], false, "new thread");
    }

    /* The caller's mask is the one it had before. */
    check_blocked(&after, SIGUSR2, true, "caller");
    check_blocked(&after, SIGINT, false, "caller");
    check_blocked(&after, SIGTERM, false, "caller");
}

/* The name the thread has, into arg, a QEMU_THREAD_NAME_LEN buffer. */
static void *record_name(void *arg)
{
    if (pthread_getname_np(pthread_self(), arg, QEMU_THREAD_NAME_LEN) != 0) {
        g_strlcpy(arg, "(unknown)", QEMU_THREAD_NAME_LEN);
    }
    return NULL;
}

/* Renames itself, then records the name it has. */
static void *rename_self(void *arg)
{
    qemu_thread_set_name("renamed-thread-too-long");
    return record_name(arg);
}

static void check_thread_name(const char *given, void *(*fn)(void *),
                              const char *want)
{
    char name[QEMU_THREAD_NAME_LEN] = "";
    QemuThread thread;

    if (qemu_thread_create(&thread, given, fn, name) != 0) {
        printf("FAIL qemu_thread_create %s\n", given);
        failures++;
        return;
    }
    qemu_thread_join(&thread);
    if (strcmp(name, want) != 0) {
        printf("FAIL thread started as \"%s\" is called \"%s\", not \"%s\"\n",
               given, name, want);
        failures++;
    }
}

static void test_thread_names(void)
{
    check_thread_name("tcp-accept", record_name, "tcp-accept");
    /* The longest name kept whole, then names a byte and more past it */
    check_thread_name("tcp-send-123456", record_name, "tcp-send-123456");
    check_thread_name("tcp-send-1234567", record_name, "tcp-send-123456");
    check_thread_name("tcp-send-4294967295", record_name, "tcp-send-429496");
    check_thread_name("tcp-recv-new", rename_self, "renamed-thread-");
}

/* ---- Mutexes --------------------------------------------------------- */

/*
 * Each of these misuses a lock on purpose, for expect_abort(), so the
 * thread-safety analysis is off for them. TSan would report the misuse and
 * halt before the wrapper could abort, so they aren't built under it.
 */
#ifndef ERNIC_THREAD_SANITIZED
ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
/* pthread_mutex_destroy() refuses a locked mutex of any type. */
static void destroy_locked(void)
{
    QemuMutex mutex;

    qemu_mutex_init(&mutex);
    qemu_mutex_lock(&mutex);
    qemu_mutex_destroy(&mutex);
}

/* Only an error-checking mutex, made without NDEBUG, refuses these. */
#ifndef NDEBUG
static void unlock_unlocked(void)
{
    QemuMutex mutex;

    qemu_mutex_init(&mutex);
    qemu_mutex_unlock(&mutex);
}

static void relock_held(void)
{
    QemuMutex mutex;

    qemu_mutex_init(&mutex);
    qemu_mutex_lock(&mutex);
    qemu_mutex_lock(&mutex);
}
#endif
ROCM_ERNIC_WARN_THREAD_SAFETY_ON
#endif

static void test_mutex_misuse_aborts(void)
{
#ifdef ERNIC_THREAD_SANITIZED
    printf("skip mutex misuse: TSan reports it before the wrappers can\n");
#else
    expect_abort(destroy_locked, "destroying a locked mutex");
#ifdef NDEBUG
    printf("skip unlocking an unlocked mutex, and relocking a held one: "
           "NDEBUG mutexes don't check their owner\n");
#else
    expect_abort(unlock_unlocked, "unlocking an unlocked mutex");
    expect_abort(relock_held, "relocking a held mutex");
#endif
#endif
}

typedef struct {
    QemuMutex *mutex;
    int ret;
} TrylockArg;

static void *trylock_once(void *opaque)
{
    TrylockArg *t = opaque;
    int ret = qemu_mutex_trylock(t->mutex);

    if (ret == 0) {
        qemu_mutex_unlock(t->mutex);
    }
    t->ret = ret;
    return NULL;
}

/* EBUSY is trylock's normal answer for a held mutex, not a failure. */
static void test_trylock_busy(void)
{
    QemuMutex mutex;
    QemuThread thread;
    TrylockArg t = {.mutex = &mutex, .ret = -1};

    qemu_mutex_init(&mutex);
    qemu_mutex_lock(&mutex);
    if (qemu_thread_create(&thread, "trylock-once", trylock_once, &t) != 0) {
        printf("FAIL qemu_thread_create\n");
        failures++;
    } else {
        qemu_thread_join(&thread);
        if (t.ret != EBUSY) {
            printf("FAIL trylock of a held mutex returned %d, not EBUSY\n",
                   t.ret);
            failures++;
        }
    }
    qemu_mutex_unlock(&mutex);
    qemu_mutex_destroy(&mutex);
}

/* ETIMEDOUT is a timed wait's normal answer, not a failure. */
static void test_timedwait_deadline_passed(void)
{
    QemuMutex mutex;
    QemuCond cond;
    struct timespec now;

    qemu_mutex_init(&mutex);
    qemu_cond_init(&cond);
    clock_gettime(CLOCK_MONOTONIC, &now);

    qemu_mutex_lock(&mutex);
    if (qemu_cond_timedwait_until(&cond, &mutex, &now)) {
        printf("FAIL timed wait past its deadline reported a wakeup\n");
        failures++;
    }
    qemu_mutex_unlock(&mutex);

    qemu_cond_destroy(&cond);
    qemu_mutex_destroy(&mutex);
}

int main(void)
{
    /* The children that abort run before any thread has: glibc keeps a
     * joined thread's stack for reuse, and a child that aborts can't free
     * its copy, which valgrind then reports as possibly lost. */
    test_bitmap_sizes();
    expect_abort(oversize_map, "oversize map");
    test_mutex_misuse_aborts();
    test_thread_signal_mask();
    test_thread_names();
    test_trylock_busy();
    test_timedwait_deadline_passed();

    if (failures) {
        printf("qemu_stubs: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("qemu_stubs: all checks passed\n");
    return 0;
}
