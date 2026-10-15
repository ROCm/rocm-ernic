/*
 * fake_vfu.c — an in-process stand-in for the libvfio-user DMA and
 * interrupt calls the device code makes
 *
 * See fake_vfu.h. The semantics follow libvfio-user's lib/dma.c closely
 * enough that the device code's error handling sees what it would see
 * against a real client: an address outside guest memory fails with
 * ENOENT, and a span that needs more scatter/gather entries than the
 * caller has room for returns -(entries needed) - 1 with errno set to 0.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "fake_vfu.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "parse_int.h"

/* One scatter/gather entry: a span that lies inside a single region. */
struct dma_sg {
    size_t region;
    size_t offset;
    size_t len;
};

struct vfu_ctx {
    uint8_t **regions;
    size_t nregions;
    size_t region_size;

    /* Bump allocator: the next free byte, as an offset from the base GPA. */
    uint64_t next_free;

    unsigned irq_count[FAKE_VFU_MAX_IRQS];

    unsigned long map_calls;
    /* vfu_addr_to_sgl() call that fails, counted like map_calls; 0 = none */
    unsigned long fail_map_at;

    /* Entries got with vfu_sgl_get() and not yet put back */
    size_t mapped;

    /* vfu_log() prints messages at this syslog level or more urgent; -1
     * prints nothing. From ERNIC_FAKE_VFU_LOG_LEVEL. */
    int log_level;
};

vfu_ctx_t *fake_vfu_create(size_t region_size, size_t nregions)
{
    if (!region_size || !nregions || region_size > SIZE_MAX / nregions)
        return NULL;

    vfu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->regions = calloc(nregions, sizeof(*ctx->regions));
    if (!ctx->regions) {
        free(ctx);
        return NULL;
    }
    ctx->nregions = nregions;
    ctx->region_size = region_size;
    for (size_t i = 0; i < nregions; i++) {
        ctx->regions[i] = calloc(1, region_size);
        if (!ctx->regions[i]) {
            fake_vfu_destroy(ctx);
            return NULL;
        }
    }

    ctx->log_level = -1;
    const char *lvl = getenv("ERNIC_FAKE_VFU_LOG_LEVEL");
    if (lvl && !ernic_parse_int(lvl, &ctx->log_level))
        fprintf(stderr, "fake_vfu: ignoring ERNIC_FAKE_VFU_LOG_LEVEL=%s\n",
                lvl);
    return ctx;
}

size_t fake_vfu_destroy(vfu_ctx_t *ctx)
{
    if (!ctx)
        return 0;

    size_t leaked = ctx->mapped;

    for (size_t i = 0; i < ctx->nregions; i++)
        free(ctx->regions[i]);
    free(ctx->regions);
    free(ctx);
    return leaked;
}

static uint64_t mem_size(const vfu_ctx_t *ctx)
{
    return (uint64_t)ctx->region_size * ctx->nregions;
}

/*
 * Offset of [gpa, gpa + len) from the base, or false if any of it is
 * outside guest memory. Checked without forming gpa + len, which a caller
 * passing a guest-chosen address can make wrap.
 */
static bool gpa_offset(const vfu_ctx_t *ctx, uint64_t gpa, uint64_t len,
                       uint64_t *off)
{
    if (gpa < FAKE_VFU_GPA_BASE)
        return false;

    uint64_t o = gpa - FAKE_VFU_GPA_BASE;
    if (o > mem_size(ctx) || len > mem_size(ctx) - o)
        return false;
    *off = o;
    return true;
}

uint64_t fake_vfu_alloc(vfu_ctx_t *ctx, size_t len, size_t align)
{
    if (!ctx || !len || len > ctx->region_size || !align ||
        (align & (align - 1)))
        return 0;

    uint64_t off = (ctx->next_free + align - 1) & ~(uint64_t)(align - 1);

    /* Move to the next region rather than straddle one. */
    if (off % ctx->region_size + len > ctx->region_size)
        off = (off / ctx->region_size + 1) * ctx->region_size;
    if (off > mem_size(ctx) || len > mem_size(ctx) - off)
        return 0;

    ctx->next_free = off + len;
    return FAKE_VFU_GPA_BASE + off;
}

uint64_t fake_vfu_alloc_straddling(vfu_ctx_t *ctx, size_t len, size_t before)
{
    if (!ctx || !before || before >= len || before > ctx->region_size)
        return 0;

    /* The first region boundary at least @before bytes past the free
     * pointer. */
    uint64_t boundary = (ctx->next_free + before + ctx->region_size - 1) /
                        ctx->region_size * ctx->region_size;
    uint64_t off = boundary - before;

    if (boundary > mem_size(ctx) || len - before > mem_size(ctx) - boundary)
        return 0;

    ctx->next_free = off + len;
    return FAKE_VFU_GPA_BASE + off;
}

static bool copy(vfu_ctx_t *ctx, uint64_t gpa, void *buf, size_t len,
                 bool to_guest)
{
    uint64_t off;

    if (!ctx || !gpa_offset(ctx, gpa, len, &off))
        return false;

    size_t done = 0;
    while (done < len) {
        size_t region = (size_t)(off / ctx->region_size);
        size_t roff = (size_t)(off % ctx->region_size);
        size_t n = ctx->region_size - roff;

        if (n > len - done)
            n = len - done;
        if (to_guest)
            memcpy(ctx->regions[region] + roff, (const uint8_t *)buf + done, n);
        else
            memcpy((uint8_t *)buf + done, ctx->regions[region] + roff, n);
        done += n;
        off += n;
    }
    return true;
}

bool fake_vfu_read(vfu_ctx_t *ctx, uint64_t gpa, void *buf, size_t len)
{
    return copy(ctx, gpa, buf, len, false);
}

bool fake_vfu_write(vfu_ctx_t *ctx, uint64_t gpa, const void *buf, size_t len)
{
    /* copy() only reads from buf when copying to the guest. */
    return copy(ctx, gpa, (void *)(uintptr_t)buf, len, true);
}

unsigned fake_vfu_irq_count(vfu_ctx_t *ctx, uint32_t vec)
{
    return ctx && vec < FAKE_VFU_MAX_IRQS ? ctx->irq_count[vec] : 0;
}

unsigned long fake_vfu_map_count(vfu_ctx_t *ctx)
{
    return ctx ? ctx->map_calls : 0;
}

void fake_vfu_fail_nth_map(vfu_ctx_t *ctx, unsigned long n)
{
    if (ctx)
        ctx->fail_map_at = n ? ctx->map_calls + n : 0;
}

size_t fake_vfu_mapped(vfu_ctx_t *ctx)
{
    return ctx ? ctx->mapped : 0;
}

/* ------------------------------------------------------------------------
 * The libvfio-user entry points
 * ------------------------------------------------------------------------
 */

size_t dma_sg_size(void)
{
    return sizeof(struct dma_sg);
}

int vfu_addr_to_sgl(vfu_ctx_t *vfu_ctx, vfu_dma_addr_t dma_addr, size_t len,
                    dma_sg_t *sgl, size_t max_nr_sgs, int prot)
{
    uint64_t gpa = (uint64_t)(uintptr_t)dma_addr;
    uint64_t off;

    (void)prot;

    vfu_ctx->map_calls++;
    if (vfu_ctx->fail_map_at && vfu_ctx->map_calls == vfu_ctx->fail_map_at) {
        vfu_ctx->fail_map_at = 0;
        errno = ENOENT;
        return -1;
    }

    if (!gpa_offset(vfu_ctx, gpa, len, &off)) {
        errno = ENOENT;
        return -1;
    }

    size_t cnt = 0;
    while (len > 0) {
        size_t roff = (size_t)(off % vfu_ctx->region_size);
        size_t n = vfu_ctx->region_size - roff;

        if (n > len)
            n = len;
        if (cnt < max_nr_sgs)
            sgl[cnt] = (struct dma_sg){
                .region = (size_t)(off / vfu_ctx->region_size),
                .offset = roff,
                .len = n,
            };
        cnt++;
        off += n;
        len -= n;
    }

    errno = 0;
    if (cnt > max_nr_sgs)
        return -(int)cnt - 1;
    return (int)cnt;
}

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    if (flags != 0 || cnt == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < cnt; i++) {
        if (sgl[i].region >= vfu_ctx->nregions ||
            sgl[i].offset > vfu_ctx->region_size ||
            sgl[i].len > vfu_ctx->region_size - sgl[i].offset) {
            errno = EINVAL;
            return -1;
        }
    }
    for (size_t i = 0; i < cnt; i++) {
        iov[i].iov_base = vfu_ctx->regions[sgl[i].region] + sgl[i].offset;
        iov[i].iov_len = sgl[i].len;
    }
    vfu_ctx->mapped += cnt;
    return 0;
}

void vfu_sgl_put(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                 size_t cnt)
{
    (void)sgl;
    (void)iov;

    if (cnt > vfu_ctx->mapped) {
        /* More put back than was ever got: the caller has lost track of
         * its mappings, which no later check would catch. */
        fprintf(stderr,
                "fake_vfu: vfu_sgl_put() of %zu entries with %zu "
                "mapped\n",
                cnt, vfu_ctx->mapped);
        abort();
    }
    vfu_ctx->mapped -= cnt;
}

void vfu_sgl_mark_dirty(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)cnt;
}

int vfu_irq_trigger(vfu_ctx_t *vfu_ctx, uint32_t subindex)
{
    if (subindex >= FAKE_VFU_MAX_IRQS) {
        errno = EINVAL;
        return -1;
    }
    vfu_ctx->irq_count[subindex]++;
    return 0;
}

void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    va_list ap;

    if (!vfu_ctx || level > vfu_ctx->log_level)
        return;

    va_start(ap, fmt);
    fprintf(stderr, "vfu[%d]: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
