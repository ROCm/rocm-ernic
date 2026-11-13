/*
 * Unit tests for the memory the server shares with its client (ernic_shm.h).
 *
 * A second mapping of the descriptor stands in for the client's, so a test
 * can tell whether the two see the same memory.  The program is linked with
 * --wrap for each call the module makes that can fail (tests/CMakeLists.txt),
 * so a case can fail one and check that nothing is left half made: the
 * memory as it was, the errno of the failure, and no descriptor leaked.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ernic_shm.h"
#include "rocm-ernic-warnings.h"

#define SHM_SIZE ((size_t)64 * 1024)

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* The call the next failure is injected into, if any */
enum fail_call {
    FAIL_NONE,
    FAIL_MEMFD,
    FAIL_FTRUNCATE,
    FAIL_SEAL,
    FAIL_MMAP,
    FAIL_DUP2,
};

static enum fail_call fail_next;

/* Fail the call if it's the one armed, which disarms it */
static bool should_fail(enum fail_call call)
{
    if (fail_next != call) {
        return false;
    }
    fail_next = FAIL_NONE;
    errno = ENOSPC; /* something none of them fails with here */
    return true;
}

/*
 * The names are --wrap's, so they are reserved identifiers by necessity, and
 * they can't be static, since the linker has to find them.
 */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
int __real_ernic_gnu_memfd_create(const char *name);
int __real_ernic_gnu_seal_size(int fd);
int __real_ftruncate(int fd, off_t length);
void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset);
int __real_dup2(int oldfd, int newfd);
int __wrap_ernic_gnu_memfd_create(const char *name);
int __wrap_ernic_gnu_seal_size(int fd);
int __wrap_ftruncate(int fd, off_t length);
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset);
int __wrap_dup2(int oldfd, int newfd);

int __wrap_ernic_gnu_memfd_create(const char *name)
{
    return should_fail(FAIL_MEMFD) ? -1 : __real_ernic_gnu_memfd_create(name);
}

int __wrap_ernic_gnu_seal_size(int fd)
{
    return should_fail(FAIL_SEAL) ? -1 : __real_ernic_gnu_seal_size(fd);
}

int __wrap_ftruncate(int fd, off_t length)
{
    return should_fail(FAIL_FTRUNCATE) ? -1 : __real_ftruncate(fd, length);
}

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset)
{
    return should_fail(FAIL_MMAP)
               ? MAP_FAILED
               : __real_mmap(addr, length, prot, flags, fd, offset);
}

int __wrap_dup2(int oldfd, int newfd)
{
    return should_fail(FAIL_DUP2) ? -1 : __real_dup2(oldfd, newfd);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

/* How many descriptors the process has open, or -1 */
static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;

    if (!d) {
        return -1;
    }
    while (readdir(d)) {
        n++;
    }
    (void)closedir(d);
    return n;
}

/* Map @fd as the client does, or NULL */
static uint8_t *map_fd(int fd)
{
    void *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    return p == MAP_FAILED ? NULL : p;
}

/* Whether @mem's first @len bytes are all zero */
static bool all_zero(const uint8_t *mem, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (mem[i] != 0) {
            return false;
        }
    }
    return true;
}

static void test_create(void)
{
    ErnicShm shm;
    uint8_t *client;

    if (ernic_shm_create(&shm, "ernic-shm-test", SHM_SIZE) != 0 || !shm.mem) {
        check(false, "create");
        return;
    }
    check(shm.fd >= 0 && shm.size == SHM_SIZE, "create: fields");
    check(all_zero(shm.mem, SHM_SIZE), "create: zeroed");
    check((fcntl(shm.fd, F_GETFD) & FD_CLOEXEC) != 0, "create: close-on-exec");

    /* Nobody holding the file can change its size */
    check(ftruncate(shm.fd, (off_t)SHM_SIZE / 2) != 0, "create: can shrink");
    check(ftruncate(shm.fd, (off_t)SHM_SIZE * 2) != 0, "create: can grow");

    client = map_fd(shm.fd);
    if (client) {
        client[100] = 0x5a;
        shm.mem[200] = 0xa5;
        check(shm.mem[100] == 0x5a && client[200] == 0xa5,
              "create: a second mapping sees the same memory");
        (void)munmap(client, SHM_SIZE);
    } else {
        check(false, "create: a second mapping");
    }

    ernic_shm_destroy(&shm);
    check(shm.fd == -1 && !shm.mem && shm.size == 0, "destroy: fields");
    ernic_shm_destroy(&shm); /* again, which does nothing */
}

static void test_create_bad_size(void)
{
    ErnicShm shm;

    errno = 0;
    check(ernic_shm_create(&shm, "ernic-shm-test", 0) == -1 && errno == EINVAL,
          "create of 0 bytes refused with EINVAL");
    check(!shm.mem, "create of 0 bytes: no memory");
    errno = 0;
    check(ernic_shm_create(&shm, "ernic-shm-test", (size_t)INT64_MAX + 1) ==
                  -1 &&
              errno == EINVAL,
          "create of more than off_t holds refused with EINVAL");
}

/* Each call create makes fails in turn, leaving nothing behind */
static void test_create_fails(void)
{
    static const struct {
        enum fail_call call;
        const char *name;
    } calls[] = {
        {FAIL_MEMFD, "create: memfd_create fails"},
        {FAIL_FTRUNCATE, "create: ftruncate fails"},
        {FAIL_SEAL, "create: sealing fails"},
        {FAIL_MMAP, "create: mmap fails"},
    };

    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        ErnicShm shm;
        int before = open_fds();
        int rc;

        fail_next = calls[i].call;
        errno = 0;
        rc = ernic_shm_create(&shm, "ernic-shm-test", SHM_SIZE);
        check(rc == -1 && errno == ENOSPC && !shm.mem && fail_next == FAIL_NONE,
              calls[i].name);
        check(open_fds() == before, calls[i].name);
        ernic_shm_destroy(&shm);
    }
}

static void test_renew(void)
{
    ErnicShm shm;
    uint8_t *old_client;
    uint8_t *client;
    uint8_t *old_mem;
    int fd;

    if (ernic_shm_create(&shm, "ernic-shm-test", SHM_SIZE) != 0) {
        check(false, "renew: create");
        return;
    }
    fd = shm.fd;
    old_mem = shm.mem;
    old_client = map_fd(fd);
    shm.mem[0] = 1;
    (void)fcntl(fd, F_SETFD, 0);

    check(ernic_shm_renew(&shm) == 0, "renew");
    check(shm.fd == fd, "renew: the same descriptor number");
    check(shm.mem != old_mem && shm.size == SHM_SIZE, "renew: new mapping");
    check(all_zero(shm.mem, SHM_SIZE), "renew: zeroed");
    check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "renew: close-on-exec");
    check(ftruncate(fd, (off_t)SHM_SIZE / 2) != 0, "renew: can shrink");

    /* The old client keeps the old memory, which is no longer the device's */
    if (old_client) {
        check(old_client[0] == 1, "renew: the old mapping keeps its memory");
        old_client[1] = 2;
        check(shm.mem[1] == 0, "renew: the old mapping reaches the new memory");
        (void)munmap(old_client, SHM_SIZE);
    } else {
        check(false, "renew: an old client's mapping");
    }

    /* The next client maps the new memory through the same number */
    client = map_fd(fd);
    if (client) {
        client[3] = 3;
        check(shm.mem[3] == 3, "renew: a new mapping sees the new memory");
        (void)munmap(client, SHM_SIZE);
    } else {
        check(false, "renew: a new client's mapping");
    }
    ernic_shm_destroy(&shm);
}

/* A renewal that fails leaves the memory as it was, and nothing open */
static void test_renew_fails(void)
{
    static const struct {
        enum fail_call call;
        const char *name;
    } calls[] = {
        {FAIL_MEMFD, "renew: memfd_create fails"},
        {FAIL_MMAP, "renew: mmap fails"},
        {FAIL_DUP2, "renew: dup2 fails"},
    };
    ErnicShm shm;

    if (ernic_shm_create(&shm, "ernic-shm-test", SHM_SIZE) != 0) {
        check(false, "renew fails: create");
        return;
    }
    shm.mem[7] = 7;

    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        ErnicShm before = shm;
        int fds = open_fds();
        uint8_t *client;

        fail_next = calls[i].call;
        errno = 0;
        check(ernic_shm_renew(&shm) == -1 && errno == ENOSPC &&
                  fail_next == FAIL_NONE,
              calls[i].name);
        check(shm.fd == before.fd && shm.mem == before.mem &&
                  shm.size == before.size && shm.mem[7] == 7,
              calls[i].name);
        check(open_fds() == fds, calls[i].name);

        /* The descriptor still names the memory */
        client = map_fd(shm.fd);
        check(client && client[7] == 7, calls[i].name);
        if (client) {
            (void)munmap(client, SHM_SIZE);
        }
    }
    ernic_shm_destroy(&shm);
}

/* A zeroed one has descriptor 0, which destroying it mustn't close */
static void test_destroy_zeroed(void)
{
    ErnicShm shm;
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    memset(&shm, 0, sizeof(shm));
    shm.fd = fd;
    ernic_shm_destroy(&shm);
    check(fd >= 0 && fcntl(fd, F_GETFD) != -1,
          "destroy with no memory closes no descriptor");
    if (fd >= 0) {
        (void)close(fd);
    }
}

int main(void)
{
    test_create();
    test_create_bad_size();
    test_create_fails();
    test_renew();
    test_renew_fails();
    test_destroy_zeroed();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
    } else {
        printf("all ernic_shm tests passed\n");
    }
    return failures ? 1 : 0;
}
