/*
 * nvmeof_target_priv.h — NVMe-oF controller internals, for its unit test
 *
 * Only nvmeof_target.c and tests/test_nvmeof_target.c include this.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_TARGET_PRIV_H
#define NVMEOF_TARGET_PRIV_H

#include <stddef.h>

#include "nvmeof_target.h"

/* Writes a range of a file-backed namespace back to its file; msync()'s
 * signature, and msync() unless a test replaces it. */
typedef int (*nvmeof_sync_fn)(void *addr, size_t len, int flags);

/*
 * Replace what Flush and FUA writes call to reach stable storage.  A RAM
 * namespace never calls it.  Restarting a target over the same image can't
 * tell a synchronous writeback from none (both read the host page cache),
 * so this is how the test sees the calls, and makes them fail.
 */
void nvmeof_target_set_sync(struct nvmeof_target *t, nvmeof_sync_fn fn);

#endif /* NVMEOF_TARGET_PRIV_H */
