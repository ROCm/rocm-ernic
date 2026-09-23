/*
 *
 * Unit tests for the bitmap and polling stubs in qemu_stubs.c.
 *
 * The bitmap's storage word is unsigned long, which is 32 or 64 bits
 * depending on the platform, so the bitmap cases use sizes around that
 * width rather than fixed numbers. Each map is filled one bit at a time
 * and then emptied from the top, with find_first_zero_bit() checked after
 * every step. Built with ASan, an index that strays past the allocation
 * fails the test.
 *
 * find_first_zero_bit() must refuse a map larger than UINT32_MAX bits,
 * since its uint32_t result could not index it. That case runs in a child
 * process, which is expected to die by SIGABRT.
 *
 * The polling cases cover the two timeouts that a conversion to whole
 * milliseconds gets wrong: a negative timeout must wait indefinitely
 * rather than return at once, and a sub-millisecond timeout must wait
 * rather than round down to zero.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hw/pci/pci.h"
#include "qemu/bitmap.h"
#include "qemu/timer.h"

/* ---- The two DMA symbols the stubs TU references but does not define.
 * Nothing under test reaches them; they exist only to satisfy the link.
 */
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)dir;
    return NULL;
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;
}

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

    bitmap_free(map);
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

/* ---- Polling --------------------------------------------------------- */

static int64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

struct delayed_write {
    int fd;
    bool ok;
};

static void *write_after_delay(void *arg)
{
    struct delayed_write *dw = arg;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};

    nanosleep(&delay, NULL);
    dw->ok = write(dw->fd, "x", 1) == 1;
    return NULL;
}

static void test_poll_sub_millisecond(void)
{
    const int64_t timeout_ns = 500 * 1000; /* 0.5 ms */
    struct pollfd pfd;
    int fds[2];
    int64_t start;
    int64_t elapsed;
    int rc;

    if (pipe(fds) != 0) {
        fail("pipe failed", 0, 0);
        return;
    }

    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    start = now_ns();
    rc = qemu_poll_ns(&pfd, 1, timeout_ns);
    elapsed = now_ns() - start;

    if (rc != 0) {
        fail("sub-millisecond poll on an empty pipe did not time out", 0, 0);
    }
    if (elapsed < timeout_ns) {
        fail("sub-millisecond poll returned before its timeout", 0, 0);
    }

    close(fds[0]);
    close(fds[1]);
}

static void test_poll_negative_waits(void)
{
    struct delayed_write dw;
    struct pollfd pfd;
    pthread_t writer;
    int fds[2];
    int rc;

    if (pipe(fds) != 0) {
        fail("pipe failed", 0, 0);
        return;
    }

    dw.fd = fds[1];
    dw.ok = false;
    if (pthread_create(&writer, NULL, write_after_delay, &dw) != 0) {
        fail("pthread_create failed", 0, 0);
        close(fds[0]);
        close(fds[1]);
        return;
    }

    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    /* The pipe is empty until the writer wakes, so a poll that treats the
     * negative timeout as zero returns 0 here instead of waiting. */
    rc = qemu_poll_ns(&pfd, 1, -1);

    pthread_join(writer, NULL);

    if (!dw.ok) {
        fail("writer thread could not write to the pipe", 0, 0);
    }
    if (rc != 1 || !(pfd.revents & POLLIN)) {
        fail("negative timeout did not wait for the pipe to be readable", 0, 0);
    }

    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_bitmap_sizes();
    test_oversize_aborts();
    test_poll_sub_millisecond();
    test_poll_negative_waits();

    if (failures) {
        printf("qemu_stubs: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("qemu_stubs: all checks passed\n");
    return 0;
}
