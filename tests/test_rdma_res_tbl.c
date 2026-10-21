/*
 *
 * Unit tests for the resource manager in rdma_rm.c, mostly its table
 * allocator.
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
 * Scope: the damage is confined to the allocator, so that is all the table
 * cases cover. It is tempting to also assert that the over-capacity handle
 * never resolves through rdma_res_tbl_get(), since handles are guest-visible
 * (an rkey is an MR handle). That assertion is vacuous in both directions --
 * rdma_res_tbl_get() guards with `handle < tbl->tbl_sz`, which is correct
 * and was never part of this bug, so it rejects the out-of-range handle on
 * fixed and unfixed builds alike. The over-capacity slot reaches a caller
 * only through the allocator's result, which the cases below check
 * directly.
 *
 * The allocator is static inline in rdma_rm.c, so it is reached through
 * the resource manager's public API instead: rdma_rm_init() sizes the CQ
 * table as max_cq, and rdma_rm_alloc_cq() and rdma_rm_dealloc_cq() given no
 * backend are thin wrappers over the allocator. The resource manager's
 * backend externs are stubbed, as no path under test reaches them.
 *
 * The init-* cases cover rdma_rm_init() itself: it must reject device
 * limits that would size a table wrongly (a negative limit), and when a
 * table can't be allocated it must free the tables it had already built and
 * leave nothing for rdma_rm_fini() to free twice.
 * g_try_malloc() and g_try_malloc0_n() are defined here so a case can make
 * a given table's entry or bitmap allocation fail.
 *
 * The mr-* cases cover an MR's life in the resource manager, with no backend
 * and with a stub one that records its calls: the MR owns no memory, so the
 * backend must be given none, and freeing the MR, once or twice, must
 * destroy the backend's MR exactly once and free its slot.
 *
 * The gid-index-* cases cover the GID index used at RTR, which the backend
 * maps from the guest's and returns as an int. It must be range-checked
 * before it is narrowed to the uint8_t qp_state_rtr() takes, so 200 is
 * passed on, and 256 and an error are refused.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <infiniband/verbs.h>

#include "rdma_backend.h"
#include "rdma_backend_defs.h"
#include "rdma_backend_ops.h"
#include "rdma_rm.h"
#include "rdma_rm_defs.h"

/* ---- Stubs for the backend entry points rdma_rm.c calls. None of them are
 * reachable from the table allocator; they exist only to satisfy the link.
 */
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
void rdma_backend_destroy_srq(RdmaBackendSRQ *srq)
{
    (void)srq;
}

/* ---- Allocation failure injection -------------------------------------
 *
 * res_tbl_init() allocates each table's entries with g_try_malloc(), and
 * its bitmap through bitmap_new(), whose g_try_new0() calls
 * g_try_malloc0_n(). Nothing else the test reaches calls either. These
 * definitions take the place of glib's, so a case can fail either
 * allocation for a given table. g_free() frees glib's allocations with
 * free(), so it frees these too.
 */
#define NO_FAILURE UINT32_MAX

/* The calls to one allocation function, and which of them to fail. */
typedef struct {
    uint32_t calls;
    uint32_t fail_at; /* 0-based, or NO_FAILURE */
} AllocInjector;

static AllocInjector entry_allocs = {0, NO_FAILURE};
static AllocInjector bitmap_allocs = {0, NO_FAILURE};

static void alloc_injectors_reset(void)
{
    entry_allocs = (AllocInjector){0, NO_FAILURE};
    bitmap_allocs = (AllocInjector){0, NO_FAILURE};
}

static bool alloc_should_fail(AllocInjector *inj)
{
    return inj->calls++ == inj->fail_at;
}

gpointer g_try_malloc(gsize n_bytes)
{
    if (alloc_should_fail(&entry_allocs)) {
        return NULL;
    }
    /* Like glib's, return NULL for zero bytes. */
    return n_bytes ? malloc(n_bytes) : NULL;
}

gpointer g_try_malloc0_n(gsize n_blocks, gsize n_block_bytes)
{
    if (alloc_should_fail(&bitmap_allocs)) {
        return NULL;
    }
    /* Like glib's, return NULL for zero bytes. calloc() checks the product
     * for overflow, as glib does. */
    if (n_blocks == 0 || n_block_bytes == 0) {
        return NULL;
    }
    return calloc(n_blocks, n_block_bytes);
}

/* ---- Fixture ----------------------------------------------------------- */

/* Exactly one bitmap word: a one-past-the-end set_bit() leaves the bitmap
 * allocation entirely. */
#define TBL_SZ_EXACT 64
/* Rounds up to two bitmap words, so the same set_bit() stays inside the
 * bitmap and only the table memset goes out of bounds. */
#define TBL_SZ_RAGGED 100

#define GUARD_BYTE 0xC7u

/*
 * rdma_rm_init() allocates the table itself, so a guard band cannot be
 * placed around it in the usual way. Instead the table is re-pointed at the
 * middle of a larger buffer of our own: the allocator only ever indexes off
 * tbl->tbl, so it cannot tell the difference, and the one-past-the-end
 * write lands in a readable guard band instead of an ASan redzone. That
 * keeps the failure reportable rather than fatal, and keeps the ragged case
 * meaningful in a non-sanitized build. The original table is put back
 * before rdma_rm_fini() frees it.
 */
static RdmaDeviceResources dev_res;
static void *orig_tbl;
static uint8_t *backing;
static size_t backing_len;
static size_t res_sz;

static RdmaRmResTbl *tbl_init_guarded(uint32_t tbl_sz)
{
    /* Every other table gets one entry, so none is created empty. */
    struct ibv_device_attr attr = {
        .max_qp = 1,
        .max_cq = (int)tbl_sz,
        .max_mr = 1,
        .max_pd = 1,
        .max_srq = 1,
    };
    RdmaRmResTbl *tbl = &dev_res.cq_tbl;

    memset(&dev_res, 0, sizeof(dev_res));
    if (rdma_rm_init(&dev_res, &attr) != 0 || tbl->tbl_sz != tbl_sz) {
        abort();
    }
    res_sz = tbl->res_sz;

    /* One spare element's worth of guard on each side. */
    backing_len = (size_t)(tbl_sz + 2) * res_sz;
    backing = malloc(backing_len);
    if (!backing) {
        abort();
    }
    memset(backing, GUARD_BYTE, backing_len);

    orig_tbl = tbl->tbl;
    tbl->tbl = backing + res_sz;
    return tbl;
}

static void tbl_destroy_guarded(RdmaRmResTbl *tbl)
{
    tbl->tbl = orig_tbl;
    rdma_rm_fini(&dev_res);
    free(backing);
    backing = NULL;
}

/* True if both guard bands still hold GUARD_BYTE. */
static bool guards_intact(const char *name)
{
    bool ok = true;

    for (size_t i = 0; i < res_sz; i++) {
        if (backing[i] != GUARD_BYTE) {
            printf(
                "FAIL %-18s: low guard byte %zu is 0x%02x, expected 0x%02x\n",
                name, i, backing[i], GUARD_BYTE);
            ok = false;
            break;
        }
    }
    for (size_t i = backing_len - res_sz; i < backing_len; i++) {
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
 * slot one element past the end of the table having already scribbled on
 * it. Under ASan the TBL_SZ_EXACT case aborts inside set_bit() before
 * returning at all, which is also a failure -- just a louder one.
 */
static bool test_fill_to_capacity(const char *name, uint32_t tbl_sz)
{
    bool ok = true;
    RdmaRmResTbl *tbl = tbl_init_guarded(tbl_sz);

    /* Every handle up to capacity must be allocated, distinct, in range. */
    bool *seen = calloc(tbl_sz, sizeof(*seen));
    if (!seen) {
        abort();
    }
    for (uint32_t i = 0; i < tbl_sz; i++) {
        uint32_t handle = UINT32_MAX;
        int rc = rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle);

        if (rc != 0) {
            printf("FAIL %-18s: allocation %u of %u returned %d before the "
                   "table was full\n",
                   name, i, tbl_sz, rc);
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

    if (ok && tbl->used != tbl_sz) {
        printf("FAIL %-18s: used is %u after filling, expected %u\n", name,
               tbl->used, tbl_sz);
        ok = false;
    }

    /* The allocation under test: the table is full, so this must be refused.
     * handle is pre-set to a value the allocator cannot legitimately produce,
     * so a bare "it wasn't written" cannot masquerade as a valid result. */
    if (ok) {
        uint32_t handle = UINT32_MAX;
        int rc = rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle);

        if (rc != -ENOMEM) {
            printf("FAIL %-18s: allocation from a full table returned %d with "
                   "handle %u, expected %d (table size %u)\n",
                   name, rc, handle, -ENOMEM, tbl_sz);
            ok = false;
        }
        if (!guards_intact(name)) {
            ok = false;
        }
        if (tbl->used != tbl_sz) {
            printf("FAIL %-18s: used is %u after a refused allocation, "
                   "expected %u\n",
                   name, tbl->used, tbl_sz);
            ok = false;
        }
    }

    tbl_destroy_guarded(tbl);

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
    bool ok = true;
    RdmaRmResTbl *tbl = tbl_init_guarded(TBL_SZ_EXACT);

    for (uint32_t i = 0; i < TBL_SZ_EXACT; i++) {
        uint32_t handle;
        if (rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle) != 0) {
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
        rdma_rm_dealloc_cq(&dev_res, freed);

        uint32_t handle = UINT32_MAX;
        int rc = rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle);
        if (rc != 0) {
            printf("FAIL %-18s: allocation after freeing handle %u returned "
                   "%d\n",
                   name, freed, rc);
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
        if (rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle) != -ENOMEM) {
            printf("FAIL %-18s: table refilled to capacity still allocated "
                   "(handle %u)\n",
                   name, handle);
            ok = false;
        }
        if (!guards_intact(name)) {
            ok = false;
        }
    }

    tbl_destroy_guarded(tbl);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- rdma_rm_init() cases ---------------------------------------------- */

/* The tables rdma_rm_init() builds, in the order it builds them. */
static RdmaRmResTbl *const rm_tables[] = {
    &dev_res.pd_tbl, &dev_res.cq_tbl,  &dev_res.mr_tbl,
    &dev_res.qp_tbl, &dev_res.srq_tbl,
};
#define RM_TABLE_COUNT ((uint32_t)(sizeof(rm_tables) / sizeof(rm_tables[0])))

/* One entry in every table. */
static const struct ibv_device_attr one_each = {
    .max_qp = 1,
    .max_cq = 1,
    .max_mr = 1,
    .max_pd = 1,
    .max_srq = 1,
};

/* True if rdma_rm_init() left nothing allocated in dev_res. */
static bool rm_init_left_nothing(const char *name)
{
    bool ok = true;

    for (uint32_t i = 0; i < RM_TABLE_COUNT; i++) {
        if (rm_tables[i]->bitmap || rm_tables[i]->tbl) {
            printf("FAIL %-18s: table %s is still allocated\n", name,
                   rm_tables[i]->name);
            ok = false;
        }
    }
    if (dev_res.qp_hash) {
        printf("FAIL %-18s: the QP hash table is still allocated\n", name);
        ok = false;
    }
    return ok;
}

/*
 * Device limits that would size a table wrongly must be refused before
 * anything is allocated. rdma_rm_fini() runs afterwards, as the server's
 * teardown does after a failed init, and must find nothing to free.
 */
static bool test_init_rejects(const char *name,
                              const struct ibv_device_attr *attr,
                              int expected_rc)
{
    bool ok = true;
    struct ibv_device_attr a = *attr;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();

    int rc = rdma_rm_init(&dev_res, &a);
    if (rc != expected_rc) {
        printf("FAIL %-18s: rdma_rm_init() returned %d, expected %d\n", name,
               rc, expected_rc);
        ok = false;
    }
    if (entry_allocs.calls != 0 || bitmap_allocs.calls != 0) {
        printf("FAIL %-18s: %u table(s) and %u bitmap(s) allocated before "
               "the limits were refused\n",
               name, entry_allocs.calls, bitmap_allocs.calls);
        ok = false;
    }
    if (!rm_init_left_nothing(name)) {
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/*
 * Fail each table's entry allocation in turn, or each table's bitmap
 * allocation. rdma_rm_init() must report it and free what it had already
 * built, including, when the bitmap fails, the same table's entries (LSan
 * and valgrind report anything it doesn't), and rdma_rm_fini() afterwards
 * must not free any of it again (ASan and valgrind report that).
 */
static bool test_init_unwinds(const char *name, bool fail_bitmap)
{
    bool ok = true;

    for (uint32_t fail_at = 0; fail_at < RM_TABLE_COUNT; fail_at++) {
        struct ibv_device_attr a = one_each;
        /* The failing table's entries are allocated before its bitmap. */
        uint32_t expected_entries = fail_at + 1;
        uint32_t expected_bitmaps = fail_bitmap ? fail_at + 1 : fail_at;

        memset(&dev_res, 0, sizeof(dev_res));
        alloc_injectors_reset();
        if (fail_bitmap) {
            bitmap_allocs.fail_at = fail_at;
        } else {
            entry_allocs.fail_at = fail_at;
        }

        int rc = rdma_rm_init(&dev_res, &a);

        if (rc != -ENOMEM) {
            printf("FAIL %-18s: with table %u failed, rdma_rm_init() "
                   "returned %d, expected %d\n",
                   name, fail_at, rc, -ENOMEM);
            ok = false;
        }
        if (entry_allocs.calls != expected_entries ||
            bitmap_allocs.calls != expected_bitmaps) {
            printf("FAIL %-18s: with table %u failed, rdma_rm_init() "
                   "allocated %u table(s) and %u bitmap(s), expected %u and "
                   "%u\n",
                   name, fail_at, entry_allocs.calls, bitmap_allocs.calls,
                   expected_entries, expected_bitmaps);
            ok = false;
        }
        if (!rm_init_left_nothing(name)) {
            ok = false;
        }

        rdma_rm_fini(&dev_res);
    }

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/*
 * A successful init allocates each table's entries and bitmap once, so
 * test_init_unwinds() reaches every one of them. Zero limits are valid:
 * g_try_malloc() returns NULL for an empty table, and that must not be
 * taken for a failure.
 */
static bool test_init_succeeds(const char *name,
                               const struct ibv_device_attr *attr)
{
    bool ok = true;
    struct ibv_device_attr a = *attr;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();

    int rc = rdma_rm_init(&dev_res, &a);
    if (rc != 0) {
        printf("FAIL %-18s: rdma_rm_init() returned %d\n", name, rc);
        ok = false;
    } else if (entry_allocs.calls != RM_TABLE_COUNT ||
               bitmap_allocs.calls != RM_TABLE_COUNT) {
        printf("FAIL %-18s: rdma_rm_init() allocated %u table(s) and %u "
               "bitmap(s), expected %u of each\n",
               name, entry_allocs.calls, bitmap_allocs.calls, RM_TABLE_COUNT);
        ok = false;
    }

    if (rc == 0) {
        rdma_rm_fini(&dev_res);
    }

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- MR cases ----------------------------------------------------------
 *
 * An MR in the resource manager is a table slot and the backend's MR. It
 * owns no memory, so allocating one must hand the backend no address, and
 * freeing one must destroy the backend's MR exactly once and free the slot,
 * however often it is called.
 */

/* What the recording backend's MR operations were called with. */
typedef struct {
    uint32_t create_calls;
    uint32_t destroy_calls;
    int create_rc; /* what create_mr returns */
    const RdmaBackendMR *created;
    const RdmaBackendMR *destroyed;
    const void *addr;
    size_t length;
    uint64_t guest_start;
    int access;
} MrCalls;

static MrCalls mr_calls;

static int rec_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    (void)backend_dev;
    (void)pd;
    return 0;
}

static int rec_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                         size_t length, uint64_t guest_start, int access)
{
    (void)pd;
    mr_calls.create_calls++;
    mr_calls.created = mr;
    mr_calls.addr = addr;
    mr_calls.length = length;
    mr_calls.guest_start = guest_start;
    mr_calls.access = access;
    return mr_calls.create_rc;
}

static void rec_destroy_mr(RdmaBackendMR *mr)
{
    mr_calls.destroy_calls++;
    mr_calls.destroyed = mr;
}

/* No mr_lkey, so the resource manager uses the handle as the lkey. */
static const RdmaBackendOps recording_ops = {
    .name = "recording",
    .create_pd = rec_create_pd,
    .create_mr = rec_create_mr,
    .destroy_mr = rec_destroy_mr,
};

#define MR_ACCESS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)

/* Init with one entry in every table and allocate a PD, with the recording
 * backend or none. */
static uint32_t mr_setup(RdmaBackendDev *backend_dev)
{
    struct ibv_device_attr a = one_each;
    uint32_t pd_handle;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    memset(&mr_calls, 0, sizeof(mr_calls));
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, backend_dev, &pd_handle) != 0) {
        abort();
    }
    return pd_handle;
}

static bool test_mr_lifecycle(const char *name, bool with_backend)
{
    bool ok = true;
    RdmaBackendDev backend_dev;

    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &recording_ops;
    uint32_t pd_handle = mr_setup(with_backend ? &backend_dev : NULL);
    const uint32_t expected_calls = with_backend ? 1 : 0;

    uint32_t handle = UINT32_MAX;
    uint32_t lkey = UINT32_MAX;
    uint32_t rkey = UINT32_MAX;
    int rc =
        rdma_rm_alloc_mr(&dev_res, pd_handle, MR_ACCESS, &handle, &lkey, &rkey);
    RdmaRmMR *mr = rc == 0 ? rdma_rm_get_mr(&dev_res, handle) : NULL;

    if (rc != 0 || !mr) {
        printf("FAIL %-18s: rdma_rm_alloc_mr() returned %d\n", name, rc);
        ok = false;
    } else if (lkey != handle || rkey != handle) {
        printf("FAIL %-18s: lkey %u and rkey %u, expected the handle %u\n",
               name, lkey, rkey, handle);
        ok = false;
    }

    if (ok && mr_calls.create_calls != expected_calls) {
        printf("FAIL %-18s: create_mr called %u time(s), expected %u\n", name,
               mr_calls.create_calls, expected_calls);
        ok = false;
    }
    if (ok && with_backend) {
        if (mr_calls.created != &mr->backend_mr) {
            printf("FAIL %-18s: create_mr was not given the MR's backend MR\n",
                   name);
            ok = false;
        }
        if (mr_calls.addr || mr_calls.length != 0 ||
            mr_calls.guest_start != 0) {
            printf("FAIL %-18s: create_mr was given memory (addr %p, length "
                   "%zu, guest_start %" PRIu64 "), but an MR has none\n",
                   name, mr_calls.addr, mr_calls.length, mr_calls.guest_start);
            ok = false;
        }
        if (mr_calls.access != MR_ACCESS) {
            printf("FAIL %-18s: create_mr was given access %#x, expected "
                   "%#x\n",
                   name, (unsigned int)mr_calls.access,
                   (unsigned int)MR_ACCESS);
            ok = false;
        }
    }

    /* Free it twice: the second call must find nothing to free. */
    if (ok) {
        rdma_rm_dealloc_mr(&dev_res, handle);
        rdma_rm_dealloc_mr(&dev_res, handle);

        if (mr_calls.destroy_calls != expected_calls) {
            printf("FAIL %-18s: destroy_mr called %u time(s), expected %u\n",
                   name, mr_calls.destroy_calls, expected_calls);
            ok = false;
        } else if (with_backend && mr_calls.destroyed != &mr->backend_mr) {
            printf("FAIL %-18s: destroy_mr was not given the MR's backend "
                   "MR\n",
                   name);
            ok = false;
        }
        if (rdma_rm_get_mr(&dev_res, handle)) {
            printf("FAIL %-18s: handle %u still resolves after being freed\n",
                   name, handle);
            ok = false;
        }
        if (dev_res.mr_tbl.used != 0) {
            printf("FAIL %-18s: used is %u after freeing the only MR\n", name,
                   dev_res.mr_tbl.used);
            ok = false;
        }
    }

    /* The table has one entry, so the slot must be reusable. */
    if (ok) {
        uint32_t again = UINT32_MAX;
        rc = rdma_rm_alloc_mr(&dev_res, pd_handle, MR_ACCESS, &again, &lkey,
                              &rkey);
        if (rc != 0 || again != handle) {
            printf("FAIL %-18s: reallocating returned %d with handle %u, "
                   "expected 0 with handle %u\n",
                   name, rc, again, handle);
            ok = false;
        }
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* A backend that can't create the MR must leave the slot free and have
 * nothing to destroy. */
static bool test_mr_backend_fails(void)
{
    const char *name = "mr-backend-fails";
    bool ok = true;
    RdmaBackendDev backend_dev;

    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &recording_ops;
    uint32_t pd_handle = mr_setup(&backend_dev);
    mr_calls.create_rc = -EINVAL;

    uint32_t handle = UINT32_MAX;
    uint32_t lkey;
    uint32_t rkey;
    int rc =
        rdma_rm_alloc_mr(&dev_res, pd_handle, MR_ACCESS, &handle, &lkey, &rkey);

    if (rc != -EIO) {
        printf("FAIL %-18s: rdma_rm_alloc_mr() returned %d, expected %d\n",
               name, rc, -EIO);
        ok = false;
    }
    if (dev_res.mr_tbl.used != 0) {
        printf("FAIL %-18s: used is %u after a failed allocation\n", name,
               dev_res.mr_tbl.used);
        ok = false;
    }
    if (mr_calls.destroy_calls != 0) {
        printf("FAIL %-18s: destroy_mr called for an MR never created\n", name);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- GID index cases ---------------------------------------------------
 *
 * Moving a QP to RTR asks the backend to map the guest's GID index to its
 * own, as an int, and passes the result on to qp_state_rtr() as a uint8_t.
 * The result has to be range-checked before it is narrowed: an index of 128
 * or more is valid, and one above UINT8_MAX would otherwise be truncated to
 * a different GID.
 */

/* What get_backend_gid_index returns, and what qp_state_rtr was given. */
static int gid_index_result;
static uint32_t rtr_calls;
static uint8_t rtr_sgid_idx;

static int gid_create_qp(RdmaBackendQP *qp, uint8_t qp_type, RdmaBackendPD *pd,
                         RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                         RdmaBackendSRQ *srq, uint32_t max_send_wr,
                         uint32_t max_recv_wr, uint32_t max_send_sge,
                         uint32_t max_recv_sge)
{
    (void)qp;
    (void)qp_type;
    (void)pd;
    (void)scq;
    (void)rcq;
    (void)srq;
    (void)max_send_wr;
    (void)max_recv_wr;
    (void)max_send_sge;
    (void)max_recv_sge;
    return 0;
}

static int gid_get_backend_gid_index(RdmaBackendDev *backend_dev, int sgid_idx)
{
    (void)backend_dev;
    (void)sgid_idx;
    return gid_index_result;
}

static int gid_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                            uint8_t qp_type, uint8_t sgid_idx,
                            union ibv_gid *dgid, uint32_t dqpn, uint32_t rq_psn,
                            uint32_t qkey, bool qkey_set)
{
    (void)backend_dev;
    (void)qp;
    (void)qp_type;
    (void)dgid;
    (void)dqpn;
    (void)rq_psn;
    (void)qkey;
    (void)qkey_set;
    rtr_calls++;
    rtr_sgid_idx = sgid_idx;
    return 0;
}

static const RdmaBackendOps gid_ops = {
    .name = "gid-index",
    .create_pd = rec_create_pd,
    .create_qp = gid_create_qp,
    .get_backend_gid_index = gid_get_backend_gid_index,
    .qp_state_rtr = gid_qp_state_rtr,
};

/*
 * Move a fresh RC QP to RTR with the backend mapping its GID index to
 * backend_idx. RTR must succeed and pass backend_idx on when it fits a
 * uint8_t, and fail with -EIO, never reaching qp_state_rtr(), when it
 * doesn't.
 */
static bool test_gid_index(const char *name, int backend_idx)
{
    bool ok = true;
    RdmaBackendDev backend_dev;
    struct ibv_device_attr a = one_each;
    union ibv_gid dgid;
    uint32_t pd_handle, cq_handle, qpn;
    const bool fits = backend_idx >= 0 && backend_idx <= UINT8_MAX;

    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &gid_ops;
    memset(&dgid, 0, sizeof(dgid));
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    gid_index_result = backend_idx;
    rtr_calls = 0;
    rtr_sgid_idx = 0;

    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, &backend_dev, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &cq_handle) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, 1, 1, cq_handle, 1, 1,
                         cq_handle, &qpn, 0, 0, NULL) != 0) {
        abort();
    }

    int rc = rdma_rm_modify_qp(&dev_res, &backend_dev, qpn, IBV_QP_STATE, 3,
                               &dgid, 0, IBV_QPS_RTR, 0, 0, 0);

    if (fits) {
        if (rc != 0 || rtr_calls != 1 || rtr_sgid_idx != backend_idx) {
            printf("FAIL %-18s: RTR returned %d with qp_state_rtr called %u "
                   "time(s) with sgid_idx %u, expected 0, once, with %d\n",
                   name, rc, rtr_calls, rtr_sgid_idx, backend_idx);
            ok = false;
        }
    } else if (rc != -EIO || rtr_calls != 0) {
        printf("FAIL %-18s: RTR returned %d with qp_state_rtr called %u "
               "time(s) (sgid_idx %u), expected %d and never\n",
               name, rc, rtr_calls, rtr_sgid_idx, -EIO);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

int main(void)
{
    int failures = 0;
    struct ibv_device_attr attr;

    if (!test_fill_to_capacity("full-exact-word", TBL_SZ_EXACT)) {
        failures++;
    }
    if (!test_fill_to_capacity("full-ragged-word", TBL_SZ_RAGGED)) {
        failures++;
    }
    if (!test_dealloc_reopens_one_slot()) {
        failures++;
    }

    if (!test_init_succeeds("init-one-each", &one_each)) {
        failures++;
    }
    memset(&attr, 0, sizeof(attr));
    if (!test_init_succeeds("init-all-zero", &attr)) {
        failures++;
    }

    attr = one_each;
    attr.max_mr = -1;
    if (!test_init_rejects("init-negative-mr", &attr, -EINVAL)) {
        failures++;
    }

    if (!test_init_unwinds("init-unwind-table", false)) {
        failures++;
    }
    if (!test_init_unwinds("init-unwind-bitmap", true)) {
        failures++;
    }

    if (!test_mr_lifecycle("mr-no-backend", false)) {
        failures++;
    }
    if (!test_mr_lifecycle("mr-backend", true)) {
        failures++;
    }
    if (!test_mr_backend_fails()) {
        failures++;
    }

    if (!test_gid_index("gid-index-200", 200)) {
        failures++;
    }
    if (!test_gid_index("gid-index-256", 256)) {
        failures++;
    }
    if (!test_gid_index("gid-index-error", -EINVAL)) {
        failures++;
    }

    if (failures) {
        printf("rdma_res_tbl: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("rdma_res_tbl: all cases passed\n");
    return 0;
}
