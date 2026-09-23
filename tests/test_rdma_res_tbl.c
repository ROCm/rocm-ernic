/*
 *
 * Unit tests for the resource-table allocator in rdma_rm.c.
 *
 * Regression coverage for an off-by-one. find_first_zero_bit() returns the
 * size it was given when no zero bit exists, so a full table yields tbl_sz
 * itself -- one past the last valid handle. The full-table guard read
 *
 *     if (*handle > tbl->tbl_sz) { reject; }
 *
 * which lets that one over-capacity value through. The code below then does
 *
 *     set_bit(*handle, tbl->bitmap);                     <- bitmap OOB write
 *     memset(tbl->tbl + *handle * tbl->res_sz, 0, res_sz) <- table OOB write
 *
 * Both land one element past the end of their allocations. Handles are
 * guest-visible (an rkey is literally an MR handle), so a guest that
 * exhausts a table drives a heap out-of-bounds write on the host.
 *
 * The two table sizes below are not interchangeable -- they exercise
 * different halves of the damage:
 *
 *   - TBL_SZ_EXACT (64) is exactly one bitmap word, so bitmap_new()
 *     allocates a single unsigned long. set_bit(64) then writes addr[1],
 *     one word past the end of that allocation. ASan catches it here.
 *
 *   - TBL_SZ_RAGGED (100) rounds up to two bitmap words, so set_bit(100)
 *     stays inside the bitmap allocation and corrupts silently -- no
 *     sanitizer report. Only the memset into tbl->tbl is out of bounds.
 *
 * Running both means the test fails on the unfixed code whether or not a
 * sanitizer is in play: the fill-to-capacity cases assert a NULL return,
 * and the ragged case's table overflow is checked against an explicit
 * guard band rather than left to ASan.
 *
 * Scope: the damage is confined to the allocator, so that is all this file
 * covers. It is tempting to also assert that the over-capacity handle never
 * resolves through rdma_res_tbl_get(), since handles are guest-visible (an
 * rkey is an MR handle). That assertion is vacuous in both directions --
 * rdma_res_tbl_get() guards with `handle < tbl->tbl_sz`, which is correct
 * and was never part of this bug, so it rejects the out-of-range handle on
 * fixed and unfixed builds alike. The over-capacity pointer reaches a
 * caller only as the allocator's return value, which the cases below check
 * directly.
 *
 * The allocator is static inline, so it is reached by #including the
 * translation unit; the resource manager's backend externs are stubbed,
 * as no path under test reaches them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pull in the code under test (including its static functions). rdma_rm.c
 * is vendored, so the warnings it raises are silenced here rather than fixed;
 * the pragmas cover only the #included code.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#ifdef __clang__
/* clang files %p with a non-void pointer under -Wpedantic, not -Wformat. */
#pragma clang diagnostic ignored "-Wformat-pedantic"
#endif
#include "hw/rdma/rdma_rm.c"
#pragma GCC diagnostic pop

#include "hw/pci/pci.h" /* declares the pci_dma_* stubs below */

/* ---- Stubs for the backend entry points rdma_rm.c calls, and for the two
 * DMA symbols the linked qemu-stubs TU references but does not define.
 * None of them are reachable from the table allocator; they exist only to
 * satisfy the link.
 */
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)dir;
    return NULL;
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;
}

int rdma_backend_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                         union ibv_gid *gid)
{
    (void)backend_dev;
    (void)ifname;
    (void)gid;
    return -1;
}
int rdma_backend_del_gid(RdmaBackendDev *backend_dev, const char *ifname,
                         union ibv_gid *gid)
{
    (void)backend_dev;
    (void)ifname;
    (void)gid;
    return -1;
}
int rdma_backend_get_gid_index(RdmaBackendDev *backend_dev, union ibv_gid *gid)
{
    (void)backend_dev;
    (void)gid;
    return -1;
}
int rdma_backend_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                            uint32_t max_wr, uint32_t max_sge,
                            uint32_t srq_limit)
{
    (void)srq;
    (void)pd;
    (void)max_wr;
    (void)max_sge;
    (void)srq_limit;
    return -1;
}
int rdma_backend_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr)
{
    (void)srq;
    (void)srq_attr;
    return -1;
}
int rdma_backend_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                            int srq_attr_mask)
{
    (void)srq;
    (void)srq_attr;
    (void)srq_attr_mask;
    return -1;
}
void rdma_backend_destroy_srq(RdmaBackendSRQ *srq, RdmaDeviceResources *dev_res)
{
    (void)srq;
    (void)dev_res;
}

/* ---- Fixture ----------------------------------------------------------- */

/* Exactly one bitmap word: a one-past-the-end set_bit() leaves the bitmap
 * allocation entirely. */
#define TBL_SZ_EXACT 64
/* Rounds up to two bitmap words, so the same set_bit() stays inside the
 * bitmap and only the table memset goes out of bounds. */
#define TBL_SZ_RAGGED 100

#define RES_SZ     32
#define GUARD_BYTE 0xC7u

/*
 * res_tbl_init() allocates tbl->tbl itself, so a guard band cannot be placed
 * around it in the usual way. Instead the table is re-pointed at the middle
 * of a larger buffer of our own: the allocator only ever indexes off
 * tbl->tbl, so it cannot tell the difference, and the one-past-the-end
 * memset lands in a readable guard band instead of an ASan redzone. That
 * keeps the failure reportable rather than fatal, and keeps the ragged case
 * meaningful in a non-sanitized build.
 */
static void *orig_tbl;
static uint8_t *backing;
static size_t backing_len;

static void tbl_init_guarded(RdmaRmResTbl *tbl, const char *name,
                             uint32_t tbl_sz)
{
    res_tbl_init(name, tbl, tbl_sz, RES_SZ);

    /* One spare element's worth of guard on each side. */
    backing_len = (size_t)(tbl_sz + 2) * RES_SZ;
    backing = malloc(backing_len);
    if (!backing) {
        abort();
    }
    memset(backing, GUARD_BYTE, backing_len);

    orig_tbl = tbl->tbl;
    tbl->tbl = backing + RES_SZ;
}

static void tbl_destroy_guarded(RdmaRmResTbl *tbl)
{
    tbl->tbl = orig_tbl;
    res_tbl_free(tbl);
    free(backing);
    backing = NULL;
}

/* True if both guard bands still hold GUARD_BYTE. */
static bool guards_intact(const char *name)
{
    bool ok = true;

    for (size_t i = 0; i < RES_SZ; i++) {
        if (backing[i] != GUARD_BYTE) {
            printf(
                "FAIL %-18s: low guard byte %zu is 0x%02x, expected 0x%02x\n",
                name, i, backing[i], GUARD_BYTE);
            ok = false;
            break;
        }
    }
    for (size_t i = backing_len - RES_SZ; i < backing_len; i++) {
        if (backing[i] != GUARD_BYTE) {
            printf("FAIL %-18s: high guard byte %zu is 0x%02x, expected "
                   "0x%02x -- an over-capacity handle was written past the "
                   "end of the table\n",
                   name, i, backing[i], GUARD_BYTE);
            ok = false;
            break;
        }
    }
    return ok;
}

/* ---- Cases -------------------------------------------------------------
 *
 * Fill a table to capacity, then assert the next allocation is refused.
 *
 * On the unfixed code the refusal never happens: find_first_zero_bit()
 * returns tbl_sz, the `>` guard admits it, and the allocator hands back a
 * pointer one element past the end of the table having already scribbled
 * on it. Under ASan the TBL_SZ_EXACT case aborts inside set_bit() before
 * returning at all, which is also a failure -- just a louder one.
 */
static bool test_fill_to_capacity(const char *name, uint32_t tbl_sz)
{
    RdmaRmResTbl tbl;
    bool ok = true;

    tbl_init_guarded(&tbl, "test", tbl_sz);

    /* Every handle up to capacity must be allocated, distinct, in range. */
    bool *seen = calloc(tbl_sz, sizeof(*seen));
    if (!seen) {
        abort();
    }
    for (uint32_t i = 0; i < tbl_sz; i++) {
        uint32_t handle = UINT32_MAX;
        void *res = rdma_res_tbl_alloc(&tbl, &handle);

        if (!res) {
            printf("FAIL %-18s: allocation %u of %u returned NULL before the "
                   "table was full\n",
                   name, i, tbl_sz);
            ok = false;
            break;
        }
        if (handle >= tbl_sz) {
            printf("FAIL %-18s: allocation %u yielded out-of-range handle %u "
                   "(table size %u)\n",
                   name, i, handle, tbl_sz);
            ok = false;
            break;
        }
        if (seen[handle]) {
            printf("FAIL %-18s: handle %u handed out twice\n", name, handle);
            ok = false;
            break;
        }
        seen[handle] = true;
    }
    free(seen);

    if (ok && tbl.used != tbl_sz) {
        printf("FAIL %-18s: used is %u after filling, expected %u\n", name,
               tbl.used, tbl_sz);
        ok = false;
    }

    /* The allocation under test: the table is full, so this must be refused.
     * handle is pre-set to a value the allocator cannot legitimately produce,
     * so a bare "it wasn't written" cannot masquerade as a valid result. */
    if (ok) {
        uint32_t handle = UINT32_MAX;
        void *res = rdma_res_tbl_alloc(&tbl, &handle);

        if (res != NULL) {
            printf("FAIL %-18s: allocation from a full table returned %p with "
                   "handle %u, expected NULL (table size %u)\n",
                   name, res, handle, tbl_sz);
            ok = false;
        }
        if (!guards_intact(name)) {
            ok = false;
        }
        if (tbl.used != tbl_sz) {
            printf("FAIL %-18s: used is %u after a refused allocation, "
                   "expected %u\n",
                   name, tbl.used, tbl_sz);
            ok = false;
        }
    }

    tbl_destroy_guarded(&tbl);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/*
 * A full table must stay usable: freeing one entry has to make exactly one
 * handle available again, and the table has to go back to refusing once it
 * is retaken. Guards against "fixing" the off-by-one by capping capacity at
 * tbl_sz - 1, which would pass the case above while quietly losing a slot.
 */
static bool test_dealloc_reopens_one_slot(void)
{
    const char *name = "dealloc-realloc";
    RdmaRmResTbl tbl;
    bool ok = true;

    tbl_init_guarded(&tbl, "test", TBL_SZ_EXACT);

    for (uint32_t i = 0; i < TBL_SZ_EXACT; i++) {
        uint32_t handle;
        if (!rdma_res_tbl_alloc(&tbl, &handle)) {
            printf("FAIL %-18s: could not fill the table (failed at %u)\n",
                   name, i);
            ok = false;
            break;
        }
    }

    /* Free a slot in the middle; the next allocation must return that exact
     * handle, since find_first_zero_bit() scans from zero. */
    const uint32_t freed = TBL_SZ_EXACT / 2;
    if (ok) {
        rdma_res_tbl_dealloc(&tbl, freed);

        uint32_t handle = UINT32_MAX;
        void *res = rdma_res_tbl_alloc(&tbl, &handle);
        if (!res) {
            printf("FAIL %-18s: allocation after freeing handle %u returned "
                   "NULL\n",
                   name, freed);
            ok = false;
        } else if (handle != freed) {
            printf("FAIL %-18s: expected the freed handle %u, got %u\n", name,
                   freed, handle);
            ok = false;
        }
    }

    /* Full again, so refused again. */
    if (ok) {
        uint32_t handle = UINT32_MAX;
        if (rdma_res_tbl_alloc(&tbl, &handle) != NULL) {
            printf("FAIL %-18s: table refilled to capacity still allocated "
                   "(handle %u)\n",
                   name, handle);
            ok = false;
        }
        if (!guards_intact(name)) {
            ok = false;
        }
    }

    tbl_destroy_guarded(&tbl);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

int main(void)
{
    int failures = 0;

    if (!test_fill_to_capacity("full-exact-word", TBL_SZ_EXACT)) {
        failures++;
    }
    if (!test_fill_to_capacity("full-ragged-word", TBL_SZ_RAGGED)) {
        failures++;
    }
    if (!test_dealloc_reopens_one_slot()) {
        failures++;
    }

    if (failures) {
        printf("rdma_res_tbl: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("rdma_res_tbl: all cases passed\n");
    return 0;
}
