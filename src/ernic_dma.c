/*
 * ernic_dma.c - copying between host buffers and guest memory
 *
 * See ernic_dma.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_dma.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* PROT_READ, PROT_WRITE */
#include <sys/uio.h>

#include "dma_range.h"

struct ernic_dma {
    /* ERNIC_DMA_SGL_MAX entries; dma_sg_t's size is known only at run
     * time, from dma_sg_size(). */
    dma_sg_t *sg;
    struct iovec iov[ERNIC_DMA_SGL_MAX];
};

struct ernic_dma *ernic_dma_create(void)
{
    struct ernic_dma *dma = calloc(1, sizeof(*dma));
    if (!dma)
        return NULL;

    dma->sg = calloc(ERNIC_DMA_SGL_MAX, dma_sg_size());
    if (!dma->sg) {
        free(dma);
        return NULL;
    }
    return dma;
}

void ernic_dma_destroy(struct ernic_dma *dma)
{
    if (!dma)
        return;
    free(dma->sg);
    free(dma);
}

/*
 * The transfer behind ernic_dma_read() and ernic_dma_write(): exactly one of
 * @rbuf, the destination of a read, and @wbuf, the source of a write, is
 * non-NULL.
 */
static int dma_xfer(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx, uint64_t gpa,
                    uint8_t *rbuf, const uint8_t *wbuf, size_t len)
{
    size_t done = 0;
    int cnt;

    if (!ernic_dma_range_ok(gpa, len))
        return -EINVAL;
    /* libvfio-user maps a zero length to no entries, and vfu_sgl_get()
     * can't take none. */
    if (!len)
        return 0;

    cnt = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, dma->sg,
                          ERNIC_DMA_SGL_MAX, wbuf ? PROT_WRITE : PROT_READ);
    if (cnt < -1 && len > 1) {
        /*
         * -(entries needed) - 1, with errno 0: more host mappings than
         * there's room for. Map each half on its own. A span of n bytes
         * needs at most n entries, so halving ends by the time the pieces
         * are ERNIC_DMA_SGL_MAX bytes long, and the list is free again
         * before each half is mapped.
         */
        size_t half = len / 2;
        int rc = dma_xfer(dma, vfu_ctx, gpa, rbuf, wbuf, half);
        if (rc < 0)
            return rc;
        return dma_xfer(dma, vfu_ctx, gpa + half, rbuf ? rbuf + half : NULL,
                        wbuf ? wbuf + half : NULL, len - half);
    }
    if (cnt <= 0)
        return -EIO;

    size_t nsg = (size_t)cnt;

    if (vfu_sgl_get(vfu_ctx, dma->sg, dma->iov, nsg, 0) < 0)
        return -EIO;

    for (size_t i = 0; i < nsg && done < len; i++) {
        size_t n = dma->iov[i].iov_len;

        if (n > len - done)
            n = len - done;
        if (wbuf)
            memcpy(dma->iov[i].iov_base, wbuf + done, n);
        else
            memcpy(rbuf + done, dma->iov[i].iov_base, n);
        done += n;
    }

    if (wbuf)
        vfu_sgl_mark_dirty(vfu_ctx, dma->sg, nsg);
    vfu_sgl_put(vfu_ctx, dma->sg, dma->iov, nsg);
    return done == len ? 0 : -EIO;
}

int ernic_dma_read(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx, uint64_t gpa,
                   void *buf, size_t len)
{
    return dma_xfer(dma, vfu_ctx, gpa, buf, NULL, len);
}

int ernic_dma_write(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx, uint64_t gpa,
                    const void *buf, size_t len)
{
    return dma_xfer(dma, vfu_ctx, gpa, NULL, buf, len);
}
