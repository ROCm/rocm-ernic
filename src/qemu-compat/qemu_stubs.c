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
 * pthread_sigmask() fails only for an invalid first argument, so its result
 * is not checked.
 */
int qemu_thread_create(QemuThread *thread, void *(*start_routine)(void *),
                       void *arg)
{
    sigset_t set;
    sigset_t oldset;
    int ret;

    sigfillset(&set);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    sigdelset(&set, SIGFPE);
    sigdelset(&set, SIGILL);

    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    ret = pthread_create(&thread->thread, NULL, start_routine, arg);
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    return ret;
}

void qemu_thread_join(QemuThread *thread)
{
    pthread_join(thread->thread, NULL);
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
