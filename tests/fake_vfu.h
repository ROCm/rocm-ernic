/*
 * fake_vfu.h — an in-process stand-in for the libvfio-user DMA and
 * interrupt calls the device code makes
 *
 * The ionic emulator reaches the guest through seven libvfio-user entry
 * points: dma_sg_size(), vfu_addr_to_sgl(), vfu_sgl_get(), vfu_sgl_put(),
 * vfu_sgl_mark_dirty(), vfu_irq_trigger() and vfu_log(). fake_vfu.c
 * defines all seven, so a test that links it instead of libvfio-user runs
 * the real device code with no client, no socket and no VM.
 *
 * Guest memory is a set of separately allocated regions laid end to end in
 * guest physical address (GPA) space, the way a VM's RAM arrives in several
 * DMA regions. A transfer that crosses a region boundary therefore needs
 * more than one scatter/gather entry and lands in host memory that isn't
 * contiguous, which is what the real library does too. Address 0 is never
 * mapped.
 *
 * Interrupts are counted per vector rather than delivered.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef FAKE_VFU_H
#define FAKE_VFU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

/* GPA of the first byte of guest memory. */
#define FAKE_VFU_GPA_BASE UINT64_C(0x10000000)

/* Vectors whose interrupts are counted; higher ones fail vfu_irq_trigger(). */
#define FAKE_VFU_MAX_IRQS 64

/*
 * Create a context with @nregions regions of @region_size bytes each, all
 * zeroed. Returns NULL if either is 0 or memory runs out.
 */
vfu_ctx_t *fake_vfu_create(size_t region_size, size_t nregions);

/*
 * Free the context and its guest memory. Returns the number of
 * scatter/gather entries that were got with vfu_sgl_get() and never put
 * back, which is a mapping leak in the code under test if it isn't 0.
 */
size_t fake_vfu_destroy(vfu_ctx_t *ctx);

/*
 * Allocate @len bytes of guest memory aligned to @align (a power of two),
 * inside a single region. Returns the GPA, or 0 if no region has room.
 * Nothing is ever freed; the memory goes away with the context.
 */
uint64_t fake_vfu_alloc(vfu_ctx_t *ctx, size_t len, size_t align);

/*
 * Allocate @len bytes that cross a region boundary, @before of them in the
 * first region. Returns the GPA, or 0 if there's no room.
 */
uint64_t fake_vfu_alloc_straddling(vfu_ctx_t *ctx, size_t len, size_t before);

/* Copy between guest memory and a test buffer. Return false if any part of
 * [gpa, gpa + len) isn't guest memory. They may cross region boundaries. */
bool fake_vfu_read(vfu_ctx_t *ctx, uint64_t gpa, void *buf, size_t len);
bool fake_vfu_write(vfu_ctx_t *ctx, uint64_t gpa, const void *buf, size_t len);

/* Interrupts raised on @vec since the context was created. */
unsigned fake_vfu_irq_count(vfu_ctx_t *ctx, uint32_t vec);

/* Calls to vfu_addr_to_sgl() so far, failed ones included. */
unsigned long fake_vfu_map_count(vfu_ctx_t *ctx);

/*
 * Make the @n'th call to vfu_addr_to_sgl() from now fail with ENOENT, as
 * the real library does for an address outside every DMA region; 1 is the
 * next call. Only that one call fails. 0 cancels a pending failure.
 */
void fake_vfu_fail_nth_map(vfu_ctx_t *ctx, unsigned long n);

/*
 * Make the @n'th call to vfu_sgl_get() from now fail with EFAULT, getting
 * nothing, as the real library does for a DMA region the client didn't
 * share a file descriptor for; 1 is the next call. Only that one call
 * fails. 0 cancels a pending failure.
 */
void fake_vfu_fail_nth_get(vfu_ctx_t *ctx, unsigned long n);

/* Scatter/gather entries currently got and not yet put back. */
size_t fake_vfu_mapped(vfu_ctx_t *ctx);

#endif /* FAKE_VFU_H */
