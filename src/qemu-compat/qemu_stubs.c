/*
 * QEMU Utility Function Stubs for Standalone PVRDMA Device
 *
 * This file provides minimal stub implementations of QEMU utility functions
 * needed by the PVRDMA device code. These stubs allow the QEMU PVRDMA code
 * to link and run in a standalone libvfio-user environment.
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <glib.h>

/*
 * The headers declaring the stubs defined below.  Including them is what
 * checks each definition against the prototype the rest of the tree sees.
 */
#include "qemu/thread.h"
#include "qemu/bitmap.h"

#include "ernic_gnu.h"

/*
 * Thread Operations
 */

/*
 * A new thread starts with its creator's signal mask, so the mask is set to
 * block every signal around pthread_create() and then put back, as QEMU's own
 * qemu_thread_create() does.  The process's signals, such as SIGINT and
 * SIGTERM, then go to the main thread rather than a worker, and never
 * interrupt a worker's system call with EINTR.
 *
 * The signals a fault raises in the thread that caused it stay unblocked:
 * POSIX leaves undefined what happens when SIGSEGV, SIGBUS, SIGFPE, or SIGILL
 * is raised while blocked, and Linux then kills the process without running a
 * handler, such as AddressSanitizer's.  sigfillset() also takes in SIGKILL
 * and SIGSTOP, which pthread_sigmask() ignores, as it does glibc's internal
 * signals.
 *
 * pthread_sigmask() fails only for an invalid first argument, so a failure
 * there is a bug, and aborts.
 *
 * The thread names itself, in qemu_thread_start(), rather than being named
 * by its creator: glibc names another thread by writing to its file in
 * /proc, which takes a descriptor the process may have run out of, but names
 * the calling thread with prctl().
 */

/* What qemu_thread_start() needs, from g_new() and freed by the thread */
typedef struct QemuThreadStart {
    void *(*start_routine)(void *);
    void *arg;
    char name[QEMU_THREAD_NAME_LEN];
} QemuThreadStart;

static void *qemu_thread_start(void *opaque)
{
    QemuThreadStart start = *(QemuThreadStart *)opaque;

    g_free(opaque);
    qemu_thread_set_name(start.name);
    return start.start_routine(start.arg);
}

int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg)
{
    QemuThreadStart *start = g_new0(QemuThreadStart, 1);
    sigset_t set;
    sigset_t oldset;
    int ret;

    start->start_routine = start_routine;
    start->arg = arg;
    /* Cut short to what the kernel keeps */
    g_strlcpy(start->name, name, sizeof(start->name));

    sigfillset(&set);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    sigdelset(&set, SIGFPE);
    sigdelset(&set, SIGILL);

    qemu_thread_check(pthread_sigmask(SIG_SETMASK, &set, &oldset),
                      "pthread_sigmask");
    ret = pthread_create(&thread->thread, NULL, qemu_thread_start, start);
    qemu_thread_check(pthread_sigmask(SIG_SETMASK, &oldset, NULL),
                      "pthread_sigmask");

    if (ret != 0) {
        g_free(start);
    }
    return ret;
}

/*
 * pthread_setname_np() refuses a name longer than the kernel keeps
 * (ERANGE), so it is cut short first.  Naming the calling thread can't
 * otherwise fail on Linux, and a thread without a name works the same, so
 * the result is ignored.
 */
void qemu_thread_set_name(const char *name)
{
    char buf[QEMU_THREAD_NAME_LEN];

    g_strlcpy(buf, name, sizeof(buf));
    (void)ernic_gnu_pthread_setname_np(pthread_self(), buf);
}

void qemu_thread_join(QemuThread *thread)
{
    qemu_thread_check(pthread_join(thread->thread, NULL), "pthread_join");
}

/*
 * Bit Manipulation
 */

/* Bits in one storage word. The word is unsigned long, whose width varies
 * by platform, so it is measured rather than assumed. */
#define BITMAP_WORD_BITS (CHAR_BIT * sizeof(unsigned long))

/*
 * Returns a zeroed map, or NULL if allocation fails. The map is allocated
 * with GLib because the resource manager releases it with g_free().
 */
unsigned long *bitmap_new(uint32_t nbits)
{
    /* Round up by testing the remainder: nbits + BITMAP_WORD_BITS - 1
     * can overflow when size_t is 32 bits. */
    size_t words = nbits / BITMAP_WORD_BITS;
    if (nbits % BITMAP_WORD_BITS != 0) {
        words++;
    }

    /* g_try_new0() returns NULL for a zero count, which callers would
     * take for an allocation failure. An empty map gets one word. */
    if (words == 0) {
        words = 1;
    }

    return g_try_new0(unsigned long, words);
}

void set_bit(uint32_t nr, unsigned long *addr)
{
    addr[nr / BITMAP_WORD_BITS] |= 1UL << (nr % BITMAP_WORD_BITS);
}

void clear_bit(uint32_t nr, unsigned long *addr)
{
    addr[nr / BITMAP_WORD_BITS] &= ~(1UL << (nr % BITMAP_WORD_BITS));
}

bool test_bit(uint32_t nr, const unsigned long *addr)
{
    return (addr[nr / BITMAP_WORD_BITS] & (1UL << (nr % BITMAP_WORD_BITS))) !=
           0;
}

/*
 * Returns the index of the first clear bit, or size if every bit is set.
 */
uint32_t find_first_zero_bit(const unsigned long *addr, uint64_t size)
{
    if (size > UINT32_MAX) {
        fprintf(stderr,
                "find_first_zero_bit: a %" PRIu64 "-bit map cannot be "
                "indexed by uint32_t\n",
                size);
        abort();
    }

    /* The check above bounds size, so this conversion is exact. */
    uint32_t nbits = (uint32_t)size;

    for (uint32_t i = 0; i < nbits; i++) {
        if (!test_bit(i, addr)) {
            return i;
        }
    }
    return nbits;
}
