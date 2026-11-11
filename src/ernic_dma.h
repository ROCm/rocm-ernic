/*
 * ernic_dma.h - copying between host buffers and guest memory
 *
 * Every module that touches guest memory goes through these: the RDMA and
 * Ethernet devcmd paths, the admin queue, the data path and the P4 devcmds.
 * Each module creates one struct ernic_dma with its context and passes it to
 * every transfer, so the scatter/gather list libvfio-user fills in is
 * allocated once rather than on each call, which the data path makes for
 * every work request.
 *
 * A struct ernic_dma is scratch space with no lock: only one transfer may
 * use it at a time. That costs nothing, because only the thread that runs
 * the vfio-user context may DMA (libvfio-user's DMA region table is updated
 * on that thread, unlocked), and a transfer finishes before it returns.
 *
 * A function may issue memory requests only while the guest has Bus Master
 * Enable set in its Command register (PCI Express Base 6.5, 7.5.1.1.3), and
 * MSI-X messages are memory writes too. Nor may it in D3hot, the low-power
 * state the guest can put it in through the Power Management capability
 * (5.3.1.4.1). The device holds its work while it may not, and these
 * refuse every transfer, so a path that misses the check still can't reach
 * guest memory.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_DMA_H
#define ERNIC_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

/*
 * The most host mappings one transfer is mapped with at once. A guest
 * buffer is usually in one, but the guest's view is contiguous where the
 * host's need not be. A transfer that needs more is split.
 */
#define ERNIC_DMA_SGL_MAX 128

struct ernic_dma;

/* Returns NULL if memory runs out. */
struct ernic_dma *ernic_dma_create(void);

/* NULL is ignored. */
void ernic_dma_destroy(struct ernic_dma *dma);

/*
 * Copy @len bytes between guest memory at @gpa and @buf.
 *
 * @gpa and @len are checked with ernic_dma_range_ok() first, so they can
 * be passed on as the guest gave them. A zero @len maps nothing and
 * succeeds, even when ernic_dma_bus_master() says no, since it makes no
 * memory request.
 *
 * Returns 0, or -EINVAL if the range wraps the address space, -EPERM if
 * ernic_dma_bus_master() says no, or -EIO if any of it isn't guest memory
 * the device may access that way. A failed read may have filled part of
 * @buf, and a failed write may have written part of the range.
 */
int ernic_dma_read(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx, uint64_t gpa,
                   void *buf, size_t len);
int ernic_dma_write(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx, uint64_t gpa,
                    const void *buf, size_t len);

/*
 * Whether the guest has Bus Master Enable set in the function's Command
 * register, and the function in D0, as they are at this moment: the device
 * may DMA and raise MSI-X only while both are. A context with no config
 * space has no Command register to set it in, so the answer is no. Nor is
 * there a power state without a Power Management capability, so a function
 * without one, as in the unit tests, is taken to be in D0.
 *
 * The guest's config writes are stored on the thread that runs the
 * context, by libvfio-user or the server's config space callback, between
 * the device's other callbacks, so call this from that thread, at the point
 * of use: a config write and a doorbell can arrive in the same
 * vfu_run_ctx() pass.
 */
bool ernic_dma_bus_master(vfu_ctx_t *vfu_ctx);

#endif /* ERNIC_DMA_H */
