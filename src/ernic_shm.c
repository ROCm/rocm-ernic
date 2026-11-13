/*
 * Memory the server shares with its vfio-user client by a file descriptor.
 * See ernic_shm.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ernic_gnu.h"

/* The size goes to ftruncate() as an off_t, which is 64 bits on 64-bit
 * Linux, the only platform the project supports */
_Static_assert(sizeof(off_t) == sizeof(int64_t), "off_t is 64 bits");

/* Close @fd, keeping the errno of the failure that led here */
static void close_keep_errno(int fd)
{
    int err = errno;

    (void)close(fd);
    errno = err;
}

int ernic_shm_create(ErnicShm *shm, const char *name, size_t size)
{
    void *mem;
    int fd;

    shm->fd = -1;
    shm->mem = NULL;
    shm->size = 0;
    shm->name = name;

    if (size == 0 || size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }

    fd = ernic_gnu_memfd_create(name);
    if (fd < 0) {
        return -1;
    }
    /* A new file's pages read as zeros */
    if (ftruncate(fd, (off_t)size) != 0 || ernic_gnu_seal_size(fd) != 0) {
        close_keep_errno(fd);
        return -1;
    }
    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close_keep_errno(fd);
        return -1;
    }

    shm->fd = fd;
    shm->mem = mem;
    shm->size = size;
    return 0;
}

int ernic_shm_renew(ErnicShm *shm)
{
    ErnicShm fresh;

    if (ernic_shm_create(&fresh, shm->name, shm->size) != 0) {
        return -1;
    }

    /*
     * dup2() closes the old file's descriptor and puts the new file there in
     * one step, so the number never names nothing.  It clears close-on-exec,
     * which the server wants on every descriptor it holds; setting it can't
     * fail, since the descriptor was just made.
     */
    if (dup2(fresh.fd, shm->fd) < 0) {
        ernic_shm_destroy(&fresh);
        return -1;
    }
    (void)fcntl(shm->fd, F_SETFD, FD_CLOEXEC);
    (void)close(fresh.fd);

    (void)munmap(shm->mem, shm->size);
    shm->mem = fresh.mem;
    return 0;
}

void ernic_shm_destroy(ErnicShm *shm)
{
    /* Set up exactly when it's mapped; a zeroed one has descriptor 0, which
     * isn't its to close */
    if (shm->mem) {
        (void)munmap(shm->mem, shm->size);
        (void)close(shm->fd);
    }
    shm->fd = -1;
    shm->mem = NULL;
    shm->size = 0;
}
