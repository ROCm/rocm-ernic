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
 * thread, and must leave its caller's own mask as it was.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>

#include "qemu/bitmap.h"
#include "qemu/thread.h"

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

static void test_oversize_aborts(void)
{
    unsigned long word = 0;
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        fail("fork failed", 0, 0);
        return;
    }
    if (pid == 0) {
        /* The abort message is expected; keep it out of the test log. */
        if (!freopen("/dev/null", "w", stderr)) {
            _exit(2);
        }
        (void)find_first_zero_bit(&word, (uint64_t)UINT32_MAX + 1);
        _exit(0);
    }

    if (waitpid(pid, &status, 0) != pid) {
        fail("waitpid failed", 0, 0);
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fail("oversize map did not abort", 0, 0);
    }
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
    if (qemu_thread_create(&thread, record_mask, &in_thread) != 0) {
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

int main(void)
{
    test_bitmap_sizes();
    test_oversize_aborts();
    test_thread_signal_mask();

    if (failures) {
        printf("qemu_stubs: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("qemu_stubs: all checks passed\n");
    return 0;
}
