/*
 * Fakes for unit tests that link ionic_eth_emu.c.
 *
 * The DMA entry points are backed by a plain buffer: a "GPA" is an offset
 * into g_mem.  The other external symbols the emulator calls are no-op
 * stubs, so a test needs no guest, VM or vfio-user socket.  It defines
 * functions, so include it once, in one translation unit.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ETH_EMU_FAKES_H
#define IONIC_ETH_EMU_FAKES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_adminq.h"
#include "ionic_datapath.h"
#include "ionic_eth_net.h"
#include "rocm_ernic_compat.h"

/* ---- Fake guest memory ------------------------------------------------- */

/*
 * Large enough for a published port_info plus the LIF info block, with the
 * two placed far enough apart that a write to one cannot reach the other.
 */
#define GUEST_MEM_SIZE 8192
static uint8_t g_mem[GUEST_MEM_SIZE];

/* DMA writes seen since the test last cleared it. */
static unsigned int g_nwrites;

/* Count mapping attempts independently of successful guest writes. */
static unsigned int g_write_attempts;

/* Refuse this many of the next DMA write mappings, as an unmapped GPA would. */
static unsigned int g_fail_writes;

/* LOG_WARNING (or worse) messages logged since the test last cleared it. */
static unsigned int g_nwarnings;

/* What ionic_eth_net_send() returns, and how often it was called. */
static int g_net_send_rc;
static unsigned int g_net_sends;

/* One frame for the next ionic_eth_net_recv(), then nothing. */
static const uint8_t *g_net_rx_frame;
static size_t g_net_rx_len;

struct fake_sg {
    uint64_t addr;
    size_t len;
};

size_t dma_sg_size(void)
{
    return sizeof(struct fake_sg);
}

int vfu_addr_to_sgl(vfu_ctx_t *vfu_ctx, vfu_dma_addr_t dma_addr, size_t len,
                    dma_sg_t *sgl, size_t max_nr_sgs, int prot)
{
    struct fake_sg *sg = (struct fake_sg *)sgl;
    uint64_t addr = (uint64_t)(uintptr_t)dma_addr;

    (void)vfu_ctx;
    if (prot & PROT_WRITE)
        g_write_attempts++;
    /* Written so a dma_addr near UINT64_MAX can't wrap past the check. */
    if (max_nr_sgs < 1 || addr > GUEST_MEM_SIZE || len > GUEST_MEM_SIZE - addr)
        return -1;
    if ((prot & PROT_WRITE) && g_fail_writes) {
        g_fail_writes--;
        return -1;
    }
    if (prot & PROT_WRITE)
        g_nwrites++;
    sg->addr = addr;
    sg->len = len;
    return 1;
}

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    const struct fake_sg *sg = (const struct fake_sg *)sgl;

    (void)vfu_ctx;
    (void)flags;
    if (cnt < 1)
        return -1;
    iov->iov_base = g_mem + sg->addr;
    iov->iov_len = sg->len;
    return 0;
}

void vfu_sgl_put(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                 size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)iov;
    (void)cnt;
}

void vfu_sgl_mark_dirty(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)cnt;
}

void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)fmt;
    if (level <= LOG_WARNING)
        g_nwarnings++;
}

int vfu_irq_trigger(vfu_ctx_t *vfu_ctx, uint32_t subindex)
{
    (void)vfu_ctx;
    (void)subindex;
    return 0;
}

void pvrdma_irq_count(pvrdma_handle_t handle)
{
    (void)handle;
}

void pvrdma_eth_bytes_count(pvrdma_handle_t handle, uint64_t bytes, bool is_tx)
{
    (void)handle;
    (void)bytes;
    (void)is_tx;
}

void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val)
{
    (void)dp;
    (void)qtype;
    (void)doorbell_val;
}

void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx, int aq_idx,
                              uint16_t p_index)
{
    (void)ctx;
    (void)aq_idx;
    (void)p_index;
}

struct ionic_eth_net *ionic_eth_net_open_tap(const char *ifname,
                                             char *out_ifname,
                                             size_t out_ifname_len)
{
    (void)ifname;
    (void)out_ifname;
    (void)out_ifname_len;
    return NULL;
}

void ionic_eth_net_close(struct ionic_eth_net *net)
{
    (void)net;
}

int ionic_eth_net_send(struct ionic_eth_net *net, const void *frame, size_t len)
{
    (void)net;
    (void)frame;
    (void)len;
    g_net_sends++;
    return g_net_send_rc;
}

ssize_t ionic_eth_net_recv(struct ionic_eth_net *net, void *buf, size_t cap)
{
    size_t len = g_net_rx_len;

    (void)net;
    if (!g_net_rx_frame || len > cap)
        return 0;
    memcpy(buf, g_net_rx_frame, len);
    g_net_rx_frame = NULL;
    return (ssize_t)len;
}

#endif /* IONIC_ETH_EMU_FAKES_H */
