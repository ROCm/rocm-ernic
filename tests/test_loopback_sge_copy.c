/*
 * Unit tests for the loopback SGE copy helpers in rdma_backend_loopback.c.
 *
 * These functions walk source/destination scatter-gather lists, mapping
 * one SGE at a time and advancing through partial buffers. They previously
 * carried inner "if (idx >= num_sge) break;" guards that the enclosing
 * while-condition already made unreachable; this test exercises the
 * boundary-walking logic (single/multi/partial/mismatched SGEs) to show
 * the copy still transfers the right bytes with those dead guards removed.
 *
 * The static helpers are reached by #including the translation unit; the
 * DMA layer is stubbed with an identity mapping (an SGE addr is just a host
 * pointer), so copies run in-process under AddressSanitizer.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "rdma/rdma_backend_loopback.c"

/* ---- Stubs for the TU's external symbols -------------------------------
 * rdma_pci_dma_map is an identity mapping: the SGE "guest address" is a
 * real host pointer, so map returns it unchanged and unmap/sync are no-ops.
 */
void *rdma_pci_dma_map(void *dev, uint64_t addr, uint64_t len)
{
    (void)dev;
    (void)len;
    return (void *)(uintptr_t)addr;
}
void rdma_pci_dma_unmap(void *dev, void *buffer, uint64_t len)
{
    (void)dev;
    (void)buffer;
    (void)len;
}
int pci_dma_sync(PCIDevice *dev, uint64_t guest_addr, uint64_t len)
{
    (void)dev;
    (void)guest_addr;
    (void)len;
    return 0;
}
void *PVRDMA_DEV(void *obj)
{
    return obj;
}
PVRDMAQPStats *pvrdma_get_qp_stats(PVRDMADev *dev, uint32_t qp_handle)
{
    (void)dev;
    (void)qp_handle;
    return NULL;
}
void rdma_backend_complete_work(enum ibv_wc_status status, uint32_t vendor_err,
                                uint32_t byte_len, uint32_t qp_num,
                                enum ibv_wc_opcode opcode, void *ctx)
{
    (void)status;
    (void)vendor_err;
    (void)byte_len;
    (void)qp_num;
    (void)opcode;
    (void)ctx;
}
void error_report(const char *fmt, ...)
{
    (void)fmt;
}
void warn_report(const char *fmt, ...)
{
    (void)fmt;
}
void info_report(const char *fmt, ...)
{
    (void)fmt;
}
/* Receive-completion helpers live in pvrdma_qp_ops.c, which this test does
 * not link; the SGE-copy paths under test never depend on their effect. */
void pvrdma_queue_recv_work_completion(PVRDMADev *dev, uint32_t recv_cq_handle,
                                       uint64_t recv_guest_wr_id,
                                       uint32_t byte_len, uint32_t src_qp_num)
{
    (void)dev;
    (void)recv_cq_handle;
    (void)recv_guest_wr_id;
    (void)byte_len;
    (void)src_qp_num;
}
void pvrdma_queue_recv_imm_work_completion(
    PVRDMADev *dev, uint32_t recv_cq_handle, uint32_t recv_qp_handle,
    uint64_t recv_guest_wr_id, uint32_t byte_len, uint32_t src_qp_num,
    uint32_t imm_data)
{
    (void)dev;
    (void)recv_cq_handle;
    (void)recv_qp_handle;
    (void)recv_guest_wr_id;
    (void)byte_len;
    (void)src_qp_num;
    (void)imm_data;
}

/* ---- Test helpers ------------------------------------------------------ */

static PCIDevice *dummy_pci(void)
{
    static PCIDevice dummy;
    return &dummy;
}

/* Fill a buffer with a recognisable, position-dependent pattern. */
static void fill_seq(uint8_t *buf, size_t len, uint8_t base)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(base + i);
    }
}

/*
 * Build an SGE list over freshly-allocated exact-size buffers and seed the
 * source bytes into a linear reference stream. Returns total bytes.
 */
static uint32_t build_sges(struct ibv_sge *sges, const uint32_t *sizes,
                           uint32_t n, uint8_t *ref, int seed_source)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *buf = malloc(sizes[i] ? sizes[i] : 1);
        sges[i].addr = (uint64_t)(uintptr_t)buf;
        sges[i].length = sizes[i];
        sges[i].lkey = 0;
        if (seed_source) {
            fill_seq(buf, sizes[i], (uint8_t)(0x10 * (i + 1)));
            memcpy(ref + total, buf, sizes[i]);
        } else {
            memset(buf, 0, sizes[i]);
        }
        total += sizes[i];
    }
    return total;
}

static void free_sges(struct ibv_sge *sges, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        free((void *)(uintptr_t)sges[i].addr);
    }
}

/* Concatenate destination SGE contents into a linear buffer. */
static void gather_dst(const struct ibv_sge *sges, uint32_t n, uint8_t *out)
{
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(out + off, (void *)(uintptr_t)sges[i].addr, sges[i].length);
        off += sges[i].length;
    }
}

static int check_sge_copy(const char *name, const uint32_t *src_sizes,
                          uint32_t nsrc, const uint32_t *dst_sizes,
                          uint32_t ndst)
{
    struct ibv_sge src[8], dst[8];
    uint8_t src_ref[4096] = {0};
    uint8_t dst_ref[4096] = {0};

    uint32_t src_total = build_sges(src, src_sizes, nsrc, src_ref, 1);
    uint32_t dst_total = build_sges(dst, dst_sizes, ndst, dst_ref, 0);

    uint32_t copied = 0;
    int rc = loopback_copy_sge_data(dummy_pci(), src, nsrc, dst, ndst,
                                    LOOPBACK_DATA_PATTERN_PRESERVE, &copied);

    uint32_t expect = src_total < dst_total ? src_total : dst_total;
    int fail = 0;
    if (rc != 0 || copied != expect) {
        printf("FAIL %-22s: rc=%d copied=%u expected=%u\n", name, rc, copied,
               expect);
        fail = 1;
    } else {
        gather_dst(dst, ndst, dst_ref);
        if (memcmp(src_ref, dst_ref, expect) != 0) {
            printf("FAIL %-22s: copied bytes differ from source\n", name);
            fail = 1;
        }
    }

    free_sges(src, nsrc);
    free_sges(dst, ndst);
    return fail;
}

static int check_remote_roundtrip(const char *name, const uint32_t *sizes,
                                  uint32_t n)
{
    struct ibv_sge src[8], dst[8];
    uint8_t src_ref[4096] = {0};
    uint8_t dst_ref[4096] = {0};

    uint32_t total = build_sges(src, sizes, n, src_ref, 1);
    (void)build_sges(dst, sizes, n, dst_ref, 0);

    /* remote address is an identity-mapped scratch buffer of size total */
    uint8_t *remote = calloc(total ? total : 1, 1);
    uint64_t remote_addr = (uint64_t)(uintptr_t)remote;

    uint32_t w = 0, r = 0;
    int wrc =
        loopback_copy_to_remote_addr(dummy_pci(), src, n, remote_addr, total,
                                     LOOPBACK_DATA_PATTERN_PRESERVE, &w);
    int rrc =
        loopback_copy_from_remote_addr(dummy_pci(), remote_addr, total, dst, n,
                                       LOOPBACK_DATA_PATTERN_PRESERVE, &r);

    int fail = 0;
    if (wrc != 0 || rrc != 0 || w != total || r != total) {
        printf("FAIL %-22s: write rc=%d len=%u read rc=%d len=%u expected=%u\n",
               name, wrc, w, rrc, r, total);
        fail = 1;
    } else {
        gather_dst(dst, n, dst_ref);
        if (memcmp(src_ref, dst_ref, total) != 0) {
            printf("FAIL %-22s: round-trip bytes differ\n", name);
            fail = 1;
        }
    }

    free(remote);
    free_sges(src, n);
    free_sges(dst, n);
    return fail;
}

/* ---- MR bounds / integer-overflow checks -------------------------------
 *
 * loopback_translate_addr() picks the MR containing [addr, addr+len) and
 * returns lmr->virt + (addr - guest_start), which its five callers hand
 * straight to memcpy as a source or destination. The containment test used
 * to be
 *
 *     addr >= start && addr + len <= start + lmr->length
 *
 * which has two independent wrap points, both reachable because every value
 * in it is guest-supplied and none is validated first:
 *
 *   - addr and len arrive on a posted WQE, so an addr near UINT64_MAX makes
 *     addr + len wrap down into the region while addr still compares
 *     >= start (needs a low guest_start);
 *   - guest_start arrives with the MR registration as cmd->start, so a high
 *     one makes start + lmr->length wrap down, leaving the "end" bound below
 *     start and letting a huge addr pass both halves.
 *
 * Either way a region falsely claims the address and the returned pointer
 * is lmr->virt plus a wrapped offset. This is worse than a miss: on the
 * fallback path the caller gets NULL and reports an error, whereas a false
 * claim yields a plausible-looking pointer far outside the mapping.
 *
 * These cases assert the wrapped requests are not claimed by the region.
 * The identity rdma_pci_dma_map stub above returns the guest address as a
 * host pointer, so a correct rejection is recognisable: the result is the
 * fallback value (== addr) rather than something derived from lmr->virt.
 */

#define BOUNDS_MR_LEN 4096

struct bounds_case {
    const char *name;
    uint64_t guest_start;
    size_t mr_len; /* host allocation size, so ASan bounds the region */
    uint64_t addr;
    uint64_t len;
    bool expect_contained;
};

/*
 * Register one MR directly in the global table. loopback_create_mr() takes a
 * RdmaBackendMR/PD pair and auto-assigns the handle; going straight to the
 * table keeps each case to the two fields under test.
 */
static LoopbackMR *bounds_register_mr(void *virt, uint64_t guest_start,
                                      size_t length)
{
    LoopbackMR *lmr = g_new0(LoopbackMR, 1);

    if (!global_mrs_table) {
        global_mrs_table =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
    lmr->handle = 0xB001;
    lmr->virt = virt;
    lmr->length = length;
    lmr->guest_start = guest_start;
    lmr->lkey = lmr->handle;
    lmr->rkey = lmr->handle + 0x10000;
    g_hash_table_insert(global_mrs_table, GUINT_TO_POINTER(lmr->handle), lmr);
    return lmr;
}

static void bounds_clear_mrs(void)
{
    if (global_mrs_table) {
        g_hash_table_destroy(global_mrs_table);
        global_mrs_table = NULL;
    }
}

static int check_translate_bounds(const struct bounds_case *tc)
{
    uint8_t *region = malloc(tc->mr_len);
    LoopbackMR *lmr;
    void *got;
    int fail = 0;

    if (!region) {
        printf("FAIL %-24s: allocation failed\n", tc->name);
        return 1;
    }
    memset(region, 0xC7, tc->mr_len);

    bounds_clear_mrs();
    lmr = bounds_register_mr(region, tc->guest_start, tc->mr_len);

    /* Cross-check the predicate directly as well as through the lookup, so
     * a failure says whether the test or the walk is at fault. */
    if (loopback_mr_contains(lmr, tc->addr, tc->len) != tc->expect_contained) {
        printf("FAIL %-24s: contains() said %s for start=0x%" PRIx64
               " addr=0x%" PRIx64 " len=0x%" PRIx64 ", expected %s\n",
               tc->name, tc->expect_contained ? "no" : "yes", tc->guest_start,
               tc->addr, tc->len, tc->expect_contained ? "yes" : "no");
        fail = 1;
    }

    got = loopback_translate_addr(dummy_pci(), tc->addr, tc->len);

    if (tc->expect_contained) {
        void *want = region + (tc->addr - tc->guest_start);
        if (got != want) {
            printf("FAIL %-24s: in-bounds translate gave %p, expected %p\n",
                   tc->name, got, want);
            fail = 1;
        }
    } else {
        /* Must not resolve through the region. The identity stub means a
         * correct rejection falls back to the address itself. */
        void *fallback = (void *)(uintptr_t)tc->addr;
        if (got != fallback) {
            long long delta = (long long)((uint8_t *)got - region);
            printf("FAIL %-24s: wrapped request resolved to %p, which is "
                   "region%+lld (region is %p, %zu bytes); expected the "
                   "fallback %p\n",
                   tc->name, got, delta, (void *)region, tc->mr_len, fallback);
            fail = 1;
        }
    }

    bounds_clear_mrs();
    free(region);
    if (!fail) {
        printf("PASS %-24s: %s\n", tc->name,
               tc->expect_contained ? "in-bounds translate correct"
                                    : "wrapped request not claimed by MR");
    }
    return fail;
}

int main(void)
{
    int failures = 0;

    /* loopback_copy_sge_data: single, multi, partial, mismatched */
    failures += check_sge_copy("single->single", (uint32_t[]){64}, 1,
                               (uint32_t[]){64}, 1);
    failures +=
        check_sge_copy("multi-src->single-dst", (uint32_t[]){10, 20, 30}, 3,
                       (uint32_t[]){60}, 1);
    failures += check_sge_copy("single-src->multi-dst", (uint32_t[]){60}, 1,
                               (uint32_t[]){10, 20, 30}, 3);
    failures += check_sge_copy("mismatched-boundaries", (uint32_t[]){10, 20}, 2,
                               (uint32_t[]){5, 5, 20}, 3);
    failures += check_sge_copy("more-src-than-dst", (uint32_t[]){40, 40}, 2,
                               (uint32_t[]){40}, 1);
    failures += check_sge_copy("more-dst-than-src", (uint32_t[]){40}, 1,
                               (uint32_t[]){40, 40}, 2);

    /* remote copy helpers (RDMA write/read paths) */
    failures += check_remote_roundtrip("remote-single", (uint32_t[]){100}, 1);
    failures +=
        check_remote_roundtrip("remote-multi", (uint32_t[]){16, 48, 80}, 3);

    /*
     * loopback_translate_addr MR bounds.
     *
     * GS_LOW is small enough that GS_LOW - 80 underflows past zero. GS_HIGH
     * sits 99 below UINT64_MAX so that guest_start + length overflows for
     * any region bigger than 100 bytes.
     *
     * The two wrap points need different region sizes to be exploitable,
     * which is why mr_len is per-case rather than fixed. Wrap point 2 only
     * escapes the mapping when the *offset* lands outside it, and
     * addr >= guest_start already caps the offset at 99 here -- so a 4 KiB
     * region would swallow it (offset 50 of 4096 is genuinely in bounds and
     * must be accepted), while a 16-byte region does not.
     */
    {
        const uint64_t gs_low = 0x1000;
        const uint64_t gs_tiny = 16; /* below the 80-byte wrap target */
        const uint64_t gs_high = UINT64_MAX - 99;
        const size_t big = BOUNDS_MR_LEN;
        const size_t small = 16;
        const struct bounds_case bounds[] = {
            /* Ordinary accepts, to catch a fix that rejects everything */
            {"bounds-whole-region", gs_low, big, gs_low, big, true},
            {"bounds-interior", gs_low, big, gs_low + 64, 128, true},
            {"bounds-last-byte", gs_low, big, gs_low + big - 1, 1, true},
            /* High guest_start, but the offset really is inside the region:
             * the overflow-safe form must still accept it. */
            {"high-start-in-bounds", gs_high, big, gs_high + 50, 50, true},

            /* Ordinary rejects */
            {"bounds-one-below", gs_low, big, gs_low - 1, 1, false},
            {"bounds-one-past", gs_low, big, gs_low + big, 1, false},
            {"bounds-too-long", gs_low, big, gs_low, big + 1, false},

            /* Wrap point 1: addr + len wraps past 0 back into the region.
             * gs_tiny is below the 80-byte target offset, so gs_tiny - 80
             * underflows to a huge value that still compares >= start --
             * with gs_low the subtraction stays positive and the ordinary
             * addr >= start half would catch it, testing nothing new. */
            {"wrap-addr-underflow", gs_tiny, big, gs_tiny - 80, 96, false},
            {"wrap-addr-max", gs_low, big, UINT64_MAX, 1, false},
            {"wrap-len-max", gs_low, big, gs_low, UINT64_MAX, false},

            /* Wrap point 2: guest_start + length wraps, so the computed
             * "end" is below start and the upper bound is vacuous. The TCP
             * sites share this one -- guest_start and mr->start are the same
             * unvalidated cmd->start from the MR registration -- and
             * test_high_start_region() in test_tcp_mr_bounds.c asserts the
             * same property against tcp_mr_range_ok(). */
            {"wrap-end-past-region", gs_high, small, gs_high + 50, 50, false},
            {"wrap-end-at-max", gs_high, small, UINT64_MAX, 1, false},
        };

        for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
            failures += check_translate_bounds(&bounds[i]);
        }
    }

    if (failures) {
        printf("loopback_sge_copy: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("loopback_sge_copy: all checks passed\n");
    return 0;
}
