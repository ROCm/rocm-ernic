/*
 * Memory the server shares with its vfio-user client by a file descriptor
 *
 * An ErnicShm is an anonymous file, mapped into the server, whose
 * descriptor libvfio-user hands the client so that it can map the same
 * pages, as QEMU maps a BAR into its guest.  Its size is sealed: the client
 * holds the file too, and a file cut short under the server's mapping would
 * kill the server with SIGBUS on its next access.
 *
 * A client keeps its mapping after it disconnects, and nothing can take it
 * back, so ernic_shm_renew() gives the next client fresh pages under the
 * same descriptor number, and the old client keeps only pages nobody else
 * uses.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_SHM_H
#define ERNIC_SHM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Set up with ernic_shm_create(); touch the fields only to read them.  One
 * that is zeroed, or whose creation failed, has no memory: mem is NULL, and
 * fd means nothing.
 */
typedef struct ErnicShm {
    int fd;           /* the file, while mem isn't NULL */
    uint8_t *mem;     /* its mapping in this process, or NULL */
    size_t size;      /* in bytes */
    const char *name; /* the file's name in /proc */
} ErnicShm;

/**
 * Create zeroed shared memory, and map it
 *
 * @param shm  where to put it
 * @param name the file's name in /proc, which must outlive @p shm
 * @param size its size in bytes, a multiple of the page size
 *
 * Returns 0, or -1 with errno set, leaving @p shm with no memory.
 */
int ernic_shm_create(ErnicShm *shm, const char *name, size_t size);

/**
 * Replace the memory with new, zeroed memory of the same size
 *
 * @param shm the memory, from ernic_shm_create()
 *
 * The new file takes the old one's descriptor number, so whatever holds the
 * number (libvfio-user's region) now hands out the new file, and the
 * mapping moves: @p shm's mem changes.  Anyone else who mapped the old file
 * keeps the old pages, which nothing here uses again.
 *
 * Returns 0, or -1 with errno set, leaving @p shm as it was.
 */
int ernic_shm_renew(ErnicShm *shm);

/**
 * Unmap the memory and close the file
 *
 * @param shm the memory, which may be zeroed or have failed to be created
 *
 * Leaves @p shm with no memory.
 */
void ernic_shm_destroy(ErnicShm *shm);

#endif /* ERNIC_SHM_H */
