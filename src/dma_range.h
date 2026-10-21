/*
 * dma_range.h - the bounds check every guest DMA range has to pass
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_DMA_RANGE_H
#define ERNIC_DMA_RANGE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * ernic_dma_range_ok() - does [@gpa, @gpa + @len) stay inside the address
 *                        space?
 *
 * Every guest DMA address and length reaches us from a queue descriptor, a
 * work request or an admin command, so both are attacker-controlled, and
 * the sum of the two is what the host's bounds checks are made against.
 * libvfio-user's fast path in dma_addr_to_sgl() compares
 * ``dma_addr + len <= iov_end(region)``: that addition is on uint64_t, so
 * a length large enough to wrap it turns the comparison into a test
 * against a small number, which a legitimately mapped region passes. The
 * translation that comes back then describes the full, enormous length,
 * and the memcpy() behind it runs off the end of the mapping.
 *
 * Unsigned wrap-around is defined behaviour, so no sanitizer reports it;
 * the check has to be written out. Compare against what is left rather
 * than adding, so the check cannot wrap itself.
 *
 * Call this before every vfu_addr_to_sgl(), on the values as they arrived.
 * A zero length passes, which is what the callers want: they either return
 * early or ask libvfio-user for nothing.
 */
static inline bool ernic_dma_range_ok(uint64_t gpa, uint64_t len)
{
    return gpa <= UINT64_MAX - len;
}

#endif /* ERNIC_DMA_RANGE_H */
