/*
 *
 * Unit tests for the resource manager in rdma_rm.c: its table allocator,
 * and the paths, error paths included, of the calls built on it.
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
 * backend are thin wrappers over the allocator.
 *
 * The dealloc-locks case checks that freeing a handle changes the bitmap
 * and the count under the table's lock, as allocating one does. A no-op
 * lock guard once took the lock and released it again before the free.
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
 * The backend-succeeds and *-fails cases take a QP to RTS on a backend that
 * fails the one operation a case names. A failed create must free its slot
 * and leave nothing to destroy; a failed state transition must fail the
 * call and change neither the state nor the PSNs, which a query reports
 * from the resource manager's record rather than the backend's; and a
 * failed query must pass on the backend's error. qpn-in-use refuses a QP
 * the backend numbers as another live one. gsi-skips-backend takes a GSI
 * QP to RTS on a backend set to fail each transition, which mustn't be
 * called, and checks that a query reports it. qp-no-backend covers a QP
 * whose PD has no backend, which the resource manager numbers and keeps the
 * state of itself, QP0, which can't be modified, and QP1. bad-handles
 * passes free and out-of-range handles to every call that takes one,
 * tables-full allocates from full tables, and alloc-refused allocates
 * before rdma_rm_init() and with nowhere to put the handle. The srq-* cases
 * cover an SRQ's life, through stubs of the backend's SRQ entry points.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <infiniband/verbs.h>

#include "ernic_ib.h"
#include "pvrdma/rdma_backend.h"
#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_rm.h"
#include "pvrdma/rdma_rm_defs.h"
#include "qemu/bitmap.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_ops.h"

/* ---- Stubs for the SRQ entry points rdma_rm.c calls, which go to the
 * backend directly rather than through its ops. They return srq_rc and
 * count their calls, for the srq-* cases.
 */
static int srq_rc;
static uint32_t srq_create_calls;
static uint32_t srq_modify_calls;
static uint32_t srq_destroy_calls;

static void srq_stubs_reset(int rc)
{
    srq_rc = rc;
    srq_create_calls = 0;
    srq_modify_calls = 0;
    srq_destroy_calls = 0;
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
    srq_create_calls++;
    return srq_rc;
}
int rdma_backend_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr)
{
    (void)srq;
    (void)srq_attr;
    return srq_rc;
}
int rdma_backend_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                            int srq_attr_mask)
{
    (void)srq;
    (void)srq_attr;
    (void)srq_attr_mask;
    srq_modify_calls++;
    return srq_rc;
}
void rdma_backend_destroy_srq(RdmaBackendSRQ *srq)
{
    (void)srq;
    srq_destroy_calls++;
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

#define GUARD_BYTE 0xC7U

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

/* ---- Locking case ------------------------------------------------------
 *
 * This thread frees and reallocates the table's one entry LOCK_CYCLES
 * times, while an observer thread takes the table's lock over and over and
 * checks that the bitmap and the count agree. After each free, this thread
 * waits for the observer to take the lock and see the entry free, so every
 * cycle overlaps the observer's checks however the threads are scheduled.
 *
 * Under TSan, that makes the test fail on every run if a free changes the
 * bitmap or the count after releasing the lock: the handshake uses relaxed
 * atomics, which order nothing, so only the lock orders the free against
 * the observer's read, and TSan reports the first cycle's free as a data
 * race (halt_on_error). Without TSan, the observer catches it only if it
 * takes the lock between the two changes, so a pass there proves less.
 */
#define LOCK_CYCLES UINT32_C(10000)

typedef struct {
    RdmaRmResTbl *tbl;
    /*
     * The handshake. Relaxed throughout: the lock must be the only ordering
     * between the threads' table accesses (see above), and each thread only
     * needs to see the other's store eventually, which relaxed guarantees.
     */
    _Atomic uint32_t freed; /* cycles whose free this thread has done */
    _Atomic uint32_t seen;  /* those whose free the observer has seen */
    atomic_bool done;       /* this thread has finished every cycle */
    /* The observer's; read only after the join. */
    uint32_t mismatches; /* checks where the bitmap and count disagreed */
    uint32_t not_freed;  /* frees after which the entry was still in use */
} LockObserver;

static void *lock_observer(void *arg)
{
    LockObserver *obs = arg;
    uint32_t seen = 0;

    while (!atomic_load_explicit(&obs->done, memory_order_relaxed)) {
        /* Loaded before taking the lock, so if it shows a new free, the
         * lock is taken after that free released it. */
        uint32_t freed =
            atomic_load_explicit(&obs->freed, memory_order_relaxed);

        qemu_mutex_lock(&obs->tbl->lock);
        bool taken = test_bit(0, obs->tbl->bitmap);
        uint32_t used = obs->tbl->used;
        qemu_mutex_unlock(&obs->tbl->lock);

        if (used != (taken ? 1U : 0U)) {
            obs->mismatches++;
        }
        if (freed != seen) {
            /* The entry can't be reallocated until this is acknowledged. */
            if (taken || used != 0) {
                obs->not_freed++;
            }
            seen = freed;
            atomic_store_explicit(&obs->seen, seen, memory_order_relaxed);
        }
        sched_yield();
    }
    return NULL;
}

static bool test_dealloc_locks(void)
{
    const char *name = "dealloc-locks";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    /* Allocated: qemu_thread_create() keeps its argument in the heap */
    LockObserver *obs = malloc(sizeof(*obs));
    QemuThread thread;
    uint32_t handle = UINT32_MAX;

    if (!obs) {
        abort();
    }
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &handle) != 0) {
        abort();
    }

    obs->tbl = &dev_res.cq_tbl;
    atomic_init(&obs->freed, 0);
    atomic_init(&obs->seen, 0);
    atomic_init(&obs->done, false);
    obs->mismatches = 0;
    obs->not_freed = 0;
    if (qemu_thread_create(&thread, "lock-observer", lock_observer, obs) != 0) {
        abort();
    }

    /* The observer runs until done is set, so it acknowledges every free
     * and these waits end. */
    for (uint32_t cycle = 1; cycle <= LOCK_CYCLES; cycle++) {
        uint32_t again = UINT32_MAX;

        rdma_rm_dealloc_cq(&dev_res, handle);
        atomic_store_explicit(&obs->freed, cycle, memory_order_relaxed);
        while (atomic_load_explicit(&obs->seen, memory_order_relaxed) !=
               cycle) {
            sched_yield();
        }

        if (rdma_rm_alloc_cq(&dev_res, NULL, 1, &again) != 0 ||
            again != handle) {
            if (ok) {
                printf("FAIL %-18s: reallocating handle %u gave %u\n", name,
                       handle, again);
            }
            ok = false;
        }
    }
    atomic_store_explicit(&obs->done, true, memory_order_relaxed);
    qemu_thread_join(&thread);

    if (obs->mismatches != 0) {
        printf("FAIL %-18s: the bitmap and the count disagreed under the "
               "lock %u time(s)\n",
               name, obs->mismatches);
        ok = false;
    }
    if (obs->not_freed != 0) {
        printf("FAIL %-18s: the entry was still in use under the lock after "
               "%u of %u frees\n",
               name, obs->not_freed, LOCK_CYCLES);
        ok = false;
    }

    free(obs);
    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- MR cases ----------------------------------------------------------
 *
 * An MR in the resource manager is a table slot and the backend's MR.
 * Allocating one must hand the backend that MR and the access flags, and
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
    int access;
} MrCalls;

static MrCalls mr_calls;

static int rec_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    (void)backend_dev;
    (void)pd;
    return 0;
}

static int rec_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, int access)
{
    (void)pd;
    mr_calls.create_calls++;
    mr_calls.created = mr;
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
                         RdmaBackendSRQ *srq)
{
    (void)qp;
    (void)qp_type;
    (void)pd;
    (void)scq;
    (void)rcq;
    (void)srq;
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
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t qpn;
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
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                         &qpn, 0, 0, NULL) != 0) {
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

/* ---- Backend failure cases ---------------------------------------------
 *
 * A backend whose operations all succeed but the one a case names, which
 * fails with BACKEND_ERR. Each operation records that it ran, so a case can
 * tell which one the resource manager stopped at.
 */
typedef enum {
    OP_NONE,
    OP_CREATE_PD,
    OP_CREATE_CQ,
    OP_CREATE_QP,
    OP_QP_INIT,
    OP_QP_RTR,
    OP_QP_RTS,
    OP_QUERY_QP,
} BackendOp;

#define BACKEND_ERR (-ENODEV)

static BackendOp fail_op;    /* the operation that fails */
static BackendOp last_op;    /* the last one that ran */
static uint32_t cq_destroys; /* destroy_cq calls */
static uint32_t qp_destroys; /* destroy_qp calls */

static int op_ran(BackendOp op)
{
    last_op = op;
    return op == fail_op ? BACKEND_ERR : 0;
}

static int fop_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    (void)backend_dev;
    (void)pd;
    return op_ran(OP_CREATE_PD);
}

static int fop_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                         uint32_t cqe)
{
    (void)backend_dev;
    (void)cq;
    (void)cqe;
    return op_ran(OP_CREATE_CQ);
}

static void fop_destroy_cq(RdmaBackendCQ *cq)
{
    (void)cq;
    cq_destroys++;
}

static int fop_create_qp(RdmaBackendQP *qp, uint8_t qp_type, RdmaBackendPD *pd,
                         RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                         RdmaBackendSRQ *srq)
{
    (void)qp;
    (void)qp_type;
    (void)pd;
    (void)scq;
    (void)rcq;
    (void)srq;
    return op_ran(OP_CREATE_QP);
}

static void fop_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *res)
{
    (void)qp;
    (void)res;
    qp_destroys++;
}

static int fop_qp_state_init(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                             uint8_t qp_type, uint32_t qkey)
{
    (void)backend_dev;
    (void)qp;
    (void)qp_type;
    (void)qkey;
    return op_ran(OP_QP_INIT);
}

static int fop_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                            uint8_t qp_type, uint8_t sgid_idx,
                            union ibv_gid *dgid, uint32_t dqpn, uint32_t rq_psn,
                            uint32_t qkey, bool qkey_set)
{
    (void)backend_dev;
    (void)qp;
    (void)qp_type;
    (void)sgid_idx;
    (void)dgid;
    (void)dqpn;
    (void)rq_psn;
    (void)qkey;
    (void)qkey_set;
    return op_ran(OP_QP_RTR);
}

static int fop_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type, uint32_t sq_psn,
                            uint32_t qkey, bool qkey_set)
{
    (void)qp;
    (void)qp_type;
    (void)sq_psn;
    (void)qkey;
    (void)qkey_set;
    return op_ran(OP_QP_RTS);
}

static int fop_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                        int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    (void)qp;
    (void)attr_mask;
    memset(attr, 0, sizeof(*attr));
    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
    }
    return op_ran(OP_QUERY_QP);
}

/*
 * No qpn, so the resource manager numbers QPs by their handles, and no
 * get_backend_gid_index, so RTR passes the guest's GID index on as it is.
 */
static const RdmaBackendOps failing_ops = {
    .name = "failing",
    .create_pd = fop_create_pd,
    .create_cq = fop_create_cq,
    .destroy_cq = fop_destroy_cq,
    .create_qp = fop_create_qp,
    .destroy_qp = fop_destroy_qp,
    .qp_state_init = fop_qp_state_init,
    .qp_state_rtr = fop_qp_state_rtr,
    .qp_state_rts = fop_qp_state_rts,
    .query_qp = fop_query_qp,
};

/*
 * What the resource manager returns with @op failing: success for none, the
 * backend's error for a query, and -EIO for the rest
 */
static int failure_rc(BackendOp op)
{
    if (op == OP_NONE) {
        return 0;
    }
    if (op == OP_QUERY_QP) {
        return BACKEND_ERR;
    }
    return -EIO;
}

/*
 * Make a PD, a CQ and an RC QP on the failing backend, take the QP through
 * INIT and RTR to RTS, and query it, with @op failing. Everything before
 * @op must succeed and nothing after it run. A failed create is -EIO and
 * leaves its table as it was, with nothing for the backend to destroy; a
 * failed state transition is -EIO; and a failed query is passed on as the
 * backend's error. What was made is then destroyed once each.
 */
static bool test_backend_fails(const char *name, BackendOp op)
{
    /* Each transition, with the PSN it sets */
    static const struct {
        enum ibv_qp_state state;
        uint32_t mask;
        uint32_t rq_psn;
        uint32_t sq_psn;
    } steps[] = {
        {IBV_QPS_INIT, IBV_QP_STATE, 0, 0},
        {IBV_QPS_RTR, IBV_QP_STATE | IBV_QP_RQ_PSN, 0x11, 0},
        {IBV_QPS_RTS, IBV_QP_STATE | IBV_QP_SQ_PSN, 0, 0x22},
    };
    bool ok = true;
    RdmaBackendDev backend_dev;
    struct ibv_device_attr a = one_each;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    union ibv_gid dgid;
    uint32_t pd_handle = UINT32_MAX;
    uint32_t cq_handle = UINT32_MAX;
    uint32_t qpn = UINT32_MAX;
    const int expected_rc = failure_rc(op);
    const BackendOp expected_last = op == OP_NONE ? OP_QUERY_QP : op;
    const bool have_cq = op == OP_NONE || op > OP_CREATE_CQ;
    const bool have_qp = op == OP_NONE || op > OP_CREATE_QP;

    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &failing_ops;
    memset(&dgid, 0, sizeof(dgid));
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    fail_op = op;
    last_op = OP_NONE;
    cq_destroys = 0;
    qp_destroys = 0;
    if (rdma_rm_init(&dev_res, &a) != 0) {
        abort();
    }

    int rc = rdma_rm_alloc_pd(&dev_res, &backend_dev, &pd_handle);
    if (rc == 0) {
        rc = rdma_rm_alloc_cq(&dev_res, &backend_dev, 1, &cq_handle);
    }
    if (rc == 0) {
        rc = rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle,
                              cq_handle, &qpn, 0, 0, NULL);
    }
    for (size_t i = 0; rc == 0 && i < sizeof(steps) / sizeof(steps[0]); i++) {
        rc = rdma_rm_modify_qp(&dev_res, &backend_dev, qpn, steps[i].mask, 0,
                               &dgid, 0, steps[i].state, 0, steps[i].rq_psn,
                               steps[i].sq_psn);
    }
    if (rc == 0) {
        rc = rdma_rm_query_qp(&dev_res, qpn, &attr, IBV_QP_STATE, &init_attr);
    }

    if (rc != expected_rc || last_op != expected_last) {
        printf("FAIL %-18s: returned %d after backend operation %d, expected "
               "%d after %d\n",
               name, rc, (int)last_op, expected_rc, (int)expected_last);
        ok = false;
    }
    if (dev_res.pd_tbl.used != (op == OP_CREATE_PD ? 0U : 1U) ||
        dev_res.cq_tbl.used != (have_cq ? 1U : 0U) ||
        dev_res.qp_tbl.used != (have_qp ? 1U : 0U) ||
        g_hash_table_size(dev_res.qp_hash) != (have_qp ? 1U : 0U)) {
        printf("FAIL %-18s: %u PD(s), %u CQ(s), %u QP(s) and %u QPN(s) "
               "allocated\n",
               name, dev_res.pd_tbl.used, dev_res.cq_tbl.used,
               dev_res.qp_tbl.used, g_hash_table_size(dev_res.qp_hash));
        ok = false;
    }
    if (cq_destroys != 0 || qp_destroys != 0) {
        printf("FAIL %-18s: %u CQ(s) and %u QP(s) destroyed before being "
               "freed\n",
               name, cq_destroys, qp_destroys);
        ok = false;
    }

    /*
     * A failed transition changes neither the state nor the PSNs, and the
     * query reports the state the resource manager recorded, not the
     * backend's: this one's query answers RESET whatever it is asked.
     */
    if (have_qp) {
        const bool rtr = op == OP_NONE || op > OP_QP_RTR;
        const bool rts = op == OP_NONE || op > OP_QP_RTS;
        const uint32_t expected_rq_psn = rtr ? 0x11U : 0U;
        const uint32_t expected_sq_psn = rts ? 0x22U : 0U;
        enum ibv_qp_state expected_state = IBV_QPS_RESET;

        if (rts) {
            expected_state = IBV_QPS_RTS;
        } else if (rtr) {
            expected_state = IBV_QPS_RTR;
        } else if (op > OP_QP_INIT) {
            expected_state = IBV_QPS_INIT;
        }

        fail_op = OP_NONE;
        rc = rdma_rm_query_qp(&dev_res, qpn, &attr, IBV_QP_STATE, &init_attr);
        if (rc != 0 || attr.qp_state != expected_state ||
            attr.cur_qp_state != expected_state ||
            attr.rq_psn != expected_rq_psn || attr.sq_psn != expected_sq_psn) {
            printf("FAIL %-18s: query returned %d with state %d and PSNs "
                   "%#x/%#x, expected 0 with %d and %#x/%#x\n",
                   name, rc, (int)attr.qp_state, attr.rq_psn, attr.sq_psn,
                   (int)expected_state, expected_rq_psn, expected_sq_psn);
            ok = false;
        }
    }

    if (have_qp) {
        rdma_rm_dealloc_qp(&dev_res, qpn);
    }
    if (have_cq) {
        rdma_rm_dealloc_cq(&dev_res, cq_handle);
    }
    if (cq_destroys != (have_cq ? 1U : 0U) ||
        qp_destroys != (have_qp ? 1U : 0U)) {
        printf("FAIL %-18s: freeing destroyed %u CQ(s) and %u QP(s)\n", name,
               cq_destroys, qp_destroys);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/*
 * A backend that gives every QP the same number, as the none backend once
 * did. The second QP must be refused, and its backend QP destroyed, rather
 * than replace the first in the QPN table, where the first could then be
 * neither found nor freed.
 */
static uint32_t fop_same_qpn(const RdmaBackendQP *qp)
{
    (void)qp;
    return 7;
}

static const RdmaBackendOps same_qpn_ops = {
    .name = "same-qpn",
    .create_pd = fop_create_pd,
    .create_cq = fop_create_cq,
    .destroy_cq = fop_destroy_cq,
    .create_qp = fop_create_qp,
    .destroy_qp = fop_destroy_qp,
    .qpn = fop_same_qpn,
};

static bool test_qpn_in_use(void)
{
    const char *name = "qpn-in-use";
    bool ok = true;
    RdmaBackendDev backend_dev;
    struct ibv_device_attr a = one_each;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t first;
    uint32_t second = UINT32_MAX;

    a.max_qp = 2;
    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &same_qpn_ops;
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    fail_op = OP_NONE;
    qp_destroys = 0;
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, &backend_dev, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, &backend_dev, 1, &cq_handle) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                         &first, 0, 0, NULL) != 0) {
        abort();
    }
    RdmaRmQP *qp = rdma_rm_get_qp(&dev_res, first);

    int rc = rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle,
                              cq_handle, &second, 0, 0, NULL);
    if (rc != -EIO || qp_destroys != 1 || dev_res.qp_tbl.used != 1) {
        printf("FAIL %-18s: a second QPN %u returned %d, destroyed %u "
               "backend QP(s) and left %u QP(s), expected %d, 1 and 1\n",
               name, second, rc, qp_destroys, dev_res.qp_tbl.used, -EIO);
        ok = false;
    }
    if (!qp || rdma_rm_get_qp(&dev_res, first) != qp) {
        printf("FAIL %-18s: QPN %u no longer names the first QP\n", name,
               first);
        ok = false;
    }

    rdma_rm_dealloc_qp(&dev_res, first);
    if (qp_destroys != 2 || dev_res.qp_tbl.used != 0) {
        printf("FAIL %-18s: freeing the first QP destroyed %u in all and "
               "left %u\n",
               name, qp_destroys, dev_res.qp_tbl.used);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/*
 * GSI's transitions aren't passed to the backend, so a GSI QP reaches RTS
 * on the failing backend even with each transition's backend operation set
 * to fail, and a query reports the state and PSNs its modifications set.
 */
static bool test_gsi_skips_backend(void)
{
    static const struct {
        enum ibv_qp_state state;
        BackendOp op; /* the backend operation set to fail */
    } steps[] = {
        {IBV_QPS_INIT, OP_QP_INIT},
        {IBV_QPS_RTR, OP_QP_RTR},
        {IBV_QPS_RTS, OP_QP_RTS},
    };
    const char *name = "gsi-skips-backend";
    bool ok = true;
    RdmaBackendDev backend_dev;
    struct ibv_device_attr a = one_each;
    struct ibv_qp_attr attr;
    union ibv_gid dgid;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t gsi;

    memset(&backend_dev, 0, sizeof(backend_dev));
    backend_dev.backend_ops = &failing_ops;
    memset(&dgid, 0, sizeof(dgid));
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    fail_op = OP_NONE;
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, &backend_dev, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, &backend_dev, 1, &cq_handle) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, ERNIC_IB_QPT_GSI, cq_handle,
                         cq_handle, &gsi, 0, 0, NULL) != 0) {
        abort();
    }

    /* Each step sets new PSNs, so the query shows the last step's */
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        fail_op = steps[i].op;
        last_op = OP_NONE;
        int rc = rdma_rm_modify_qp(&dev_res, &backend_dev, gsi,
                                   IBV_QP_STATE | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN,
                                   0, &dgid, 0, steps[i].state, 0,
                                   (uint32_t)i + 0x10U, (uint32_t)i + 0x20U);
        if (rc != 0 || last_op != OP_NONE) {
            printf("FAIL %-18s: transition %zu returned %d after backend "
                   "operation %d, expected 0 after none\n",
                   name, i, rc, (int)last_op);
            ok = false;
        }
    }

    fail_op = OP_NONE;
    int rc = rdma_rm_query_qp(&dev_res, gsi, &attr, IBV_QP_STATE, NULL);
    if (rc != 0 || attr.qp_state != IBV_QPS_RTS ||
        attr.cur_qp_state != IBV_QPS_RTS || attr.rq_psn != 0x12U ||
        attr.sq_psn != 0x22U) {
        printf("FAIL %-18s: query returned %d with state %d and PSNs "
               "%#x/%#x, expected 0 with %d and 0x12/0x22\n",
               name, rc, (int)attr.qp_state, attr.rq_psn, attr.sq_psn,
               IBV_QPS_RTS);
        ok = false;
    }

    rdma_rm_dealloc_qp(&dev_res, gsi);
    rdma_rm_dealloc_cq(&dev_res, cq_handle);
    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- QPs with no backend -----------------------------------------------
 *
 * A PD made with no backend gives its QPs none. The resource manager then
 * numbers a QP by its handle, keeps its state and PSNs itself, and answers
 * a query from them.
 */
static bool test_qp_no_backend(void)
{
    const char *name = "qp-no-backend";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    union ibv_gid dgid;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t qpn;

    a.max_qp = 3;
    memset(&dgid, 0, sizeof(dgid));
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, NULL, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &cq_handle) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                         &qpn, 0, 0, NULL) != 0) {
        abort();
    }

    RdmaRmQP *qp = rdma_rm_get_qp(&dev_res, qpn);
    if (!qp || qp->qpn != qpn) {
        printf("FAIL %-18s: QPN %u doesn't name the QP with that handle\n",
               name, qpn);
        ok = false;
    }

    /* PSNs are 24 bits; the bits above are dropped. */
    int rc = rdma_rm_modify_qp(
        &dev_res, NULL, qpn, IBV_QP_STATE | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN, 0,
        &dgid, 0, IBV_QPS_RTR, 0, UINT32_C(0x12345678), UINT32_C(0x9abcdef0));
    if (rc != 0) {
        printf("FAIL %-18s: RTR returned %d\n", name, rc);
        ok = false;
    }

    memset(&attr, 0xff, sizeof(attr));
    memset(&init_attr, 0xff, sizeof(init_attr));
    rc = rdma_rm_query_qp(&dev_res, qpn, &attr, IBV_QP_STATE, &init_attr);
    if (rc != 0 || attr.qp_state != IBV_QPS_RTR ||
        attr.cur_qp_state != IBV_QPS_RTR || attr.rq_psn != 0x345678U ||
        attr.sq_psn != 0xbcdef0U || init_attr.qp_type != IBV_QPT_RC) {
        printf("FAIL %-18s: query returned %d with state %d/%d, PSNs %#x/%#x "
               "and type %d, expected 0 with %d/%d, 0x345678/0xbcdef0 and "
               "%d\n",
               name, rc, (int)attr.qp_state, (int)attr.cur_qp_state,
               attr.rq_psn, attr.sq_psn, (int)init_attr.qp_type, IBV_QPS_RTR,
               IBV_QPS_RTR, IBV_QPT_RC);
        ok = false;
    }

    /* The init attributes are optional. */
    rc = rdma_rm_query_qp(&dev_res, qpn, &attr, IBV_QP_STATE, NULL);
    if (rc != 0 || attr.qp_state != IBV_QPS_RTR) {
        printf("FAIL %-18s: query with no init attributes returned %d with "
               "state %d\n",
               name, rc, (int)attr.qp_state);
        ok = false;
    }

    /* QP0 can't be modified, and QP1's modifications are recorded. */
    uint32_t smi;
    uint32_t gsi;
    if (rdma_rm_alloc_qp(&dev_res, pd_handle, ERNIC_IB_QPT_SMI, cq_handle,
                         cq_handle, &smi, 0, 0, NULL) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, ERNIC_IB_QPT_GSI, cq_handle,
                         cq_handle, &gsi, 0, 0, NULL) != 0) {
        abort();
    }
    rc = rdma_rm_modify_qp(&dev_res, NULL, smi, IBV_QP_STATE, 0, &dgid, 0,
                           IBV_QPS_RTS, 0, 0, 0);
    if (rc != -EPERM) {
        printf("FAIL %-18s: modifying QP0 returned %d, expected %d\n", name, rc,
               -EPERM);
        ok = false;
    }
    rc = rdma_rm_modify_qp(&dev_res, NULL, gsi, IBV_QP_STATE | IBV_QP_SQ_PSN, 0,
                           &dgid, 0, IBV_QPS_RTS, 0, 0, 5);
    if (rc != 0 ||
        rdma_rm_query_qp(&dev_res, gsi, &attr, IBV_QP_STATE, NULL) != 0 ||
        attr.qp_state != IBV_QPS_RTS || attr.sq_psn != 5) {
        printf("FAIL %-18s: modifying QP1 returned %d and left state %d and "
               "SQ PSN %u, expected 0 and RTS, 5\n",
               name, rc, (int)attr.qp_state, attr.sq_psn);
        ok = false;
    }

    /* Freed twice, the second finds nothing. */
    rdma_rm_dealloc_qp(&dev_res, qpn);
    rdma_rm_dealloc_qp(&dev_res, qpn);
    rdma_rm_dealloc_qp(&dev_res, smi);
    rdma_rm_dealloc_qp(&dev_res, gsi);
    if (dev_res.qp_tbl.used != 0 || g_hash_table_size(dev_res.qp_hash) != 0) {
        printf("FAIL %-18s: %u QP(s) and %u QPN(s) left after freeing every "
               "QP\n",
               name, dev_res.qp_tbl.used, g_hash_table_size(dev_res.qp_hash));
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- Bad handles and full tables ---------------------------------------
 *
 * Handles come from the guest. One that names nothing, whether in range but
 * free or past the end of its table, must be refused by every call that
 * takes it, and ignored by every free.
 */
static bool test_bad_handles(void)
{
    const char *name = "bad-handles";
    static const uint32_t bad[] = {1, 2, UINT32_MAX};
    bool ok = true;
    struct ibv_device_attr a = {
        .max_qp = 2,
        .max_cq = 2,
        .max_mr = 2,
        .max_pd = 2,
        .max_srq = 2,
    };
    struct ibv_qp_attr attr;
    struct ibv_srq_attr srq_attr;
    union ibv_gid dgid;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t handle;
    uint32_t lkey;
    uint32_t rkey;

    memset(&dgid, 0, sizeof(dgid));
    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.max_wr = 1;
    srq_attr.srq_limit = 1;
    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    srq_stubs_reset(0);
    /* Handle 0 of the PD and CQ tables is taken, so 1 is free and in range,
     * and 2 is past the end. */
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, NULL, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &cq_handle) != 0 ||
        pd_handle != 0 || cq_handle != 0) {
        abort();
    }

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const uint32_t h = bad[i];
        int rcs[] = {
            rdma_rm_alloc_mr(&dev_res, h, MR_ACCESS, &handle, &lkey, &rkey),
            rdma_rm_alloc_qp(&dev_res, h, IBV_QPT_RC, cq_handle, cq_handle,
                             &handle, 0, 0, NULL),
            rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, h, cq_handle,
                             &handle, 0, 0, NULL),
            rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, h,
                             &handle, 0, 0, NULL),
            rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle,
                             cq_handle, &handle, 1, h, NULL),
            rdma_rm_modify_qp(&dev_res, NULL, h, IBV_QP_STATE, 0, &dgid, 0,
                              IBV_QPS_INIT, 0, 0, 0),
            rdma_rm_query_qp(&dev_res, h, &attr, IBV_QP_STATE, NULL),
            rdma_rm_alloc_srq(&dev_res, h, 1, 1, 1, &handle),
            rdma_rm_query_srq(&dev_res, h, &srq_attr),
            rdma_rm_modify_srq(&dev_res, h, &srq_attr, IBV_SRQ_LIMIT),
        };

        for (size_t j = 0; j < sizeof(rcs) / sizeof(rcs[0]); j++) {
            if (rcs[j] != -EINVAL) {
                printf("FAIL %-18s: call %zu with handle %u returned %d, "
                       "expected %d\n",
                       name, j, h, rcs[j], -EINVAL);
                ok = false;
            }
        }
        if (rdma_rm_get_qp(&dev_res, h)) {
            printf("FAIL %-18s: QPN %u names a QP\n", name, h);
            ok = false;
        }

        rdma_rm_dealloc_mr(&dev_res, h);
        rdma_rm_dealloc_cq(&dev_res, h);
        rdma_rm_dealloc_qp(&dev_res, h);
        rdma_rm_dealloc_srq(&dev_res, h);
    }

    if (dev_res.pd_tbl.used != 1 || dev_res.cq_tbl.used != 1 ||
        dev_res.mr_tbl.used != 0 || dev_res.qp_tbl.used != 0 ||
        dev_res.srq_tbl.used != 0 || g_hash_table_size(dev_res.qp_hash) != 0) {
        printf("FAIL %-18s: %u PD(s), %u CQ(s), %u MR(s), %u QP(s), %u "
               "SRQ(s) and %u QPN(s) allocated, expected 1, 1 and none\n",
               name, dev_res.pd_tbl.used, dev_res.cq_tbl.used,
               dev_res.mr_tbl.used, dev_res.qp_tbl.used, dev_res.srq_tbl.used,
               g_hash_table_size(dev_res.qp_hash));
        ok = false;
    }
    if (srq_create_calls != 0 || srq_modify_calls != 0 ||
        srq_destroy_calls != 0) {
        printf("FAIL %-18s: the backend was asked about an SRQ\n", name);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* With one entry in every table, the second PD, MR, QP and SRQ must be
 * refused. (The CQ table is filled by the full-* cases.) */
static bool test_tables_full(void)
{
    const char *name = "tables-full";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t srq_handle;
    uint32_t handle;
    uint32_t lkey;
    uint32_t rkey;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    srq_stubs_reset(0);
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, NULL, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &cq_handle) != 0 ||
        rdma_rm_alloc_mr(&dev_res, pd_handle, MR_ACCESS, &handle, &lkey,
                         &rkey) != 0 ||
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                         &handle, 0, 0, NULL) != 0 ||
        rdma_rm_alloc_srq(&dev_res, pd_handle, 1, 1, 1, &srq_handle) != 0) {
        abort();
    }

    int rcs[] = {
        rdma_rm_alloc_pd(&dev_res, NULL, &handle),
        rdma_rm_alloc_mr(&dev_res, pd_handle, MR_ACCESS, &handle, &lkey, &rkey),
        rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                         &handle, 0, 0, NULL),
        rdma_rm_alloc_srq(&dev_res, pd_handle, 1, 1, 1, &handle),
    };
    for (size_t i = 0; i < sizeof(rcs) / sizeof(rcs[0]); i++) {
        if (rcs[i] != -ENOMEM) {
            printf("FAIL %-18s: allocation %zu from a full table returned %d, "
                   "expected %d\n",
                   name, i, rcs[i], -ENOMEM);
            ok = false;
        }
    }
    if (srq_create_calls != 1) {
        printf("FAIL %-18s: the backend was asked for %u SRQs, expected 1\n",
               name, srq_create_calls);
        ok = false;
    }

    rdma_rm_dealloc_srq(&dev_res, srq_handle);
    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* A table rdma_rm_init() hasn't built, or nowhere to put the handle, is
 * refused rather than dereferenced. */
static bool test_alloc_refused(void)
{
    const char *name = "alloc-refused";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    uint32_t handle;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    int rc_no_table = rdma_rm_alloc_pd(&dev_res, NULL, &handle);

    if (rdma_rm_init(&dev_res, &a) != 0) {
        abort();
    }
    int rc_no_handle = rdma_rm_alloc_cq(&dev_res, NULL, 1, NULL);

    if (rc_no_table != -ENOMEM || rc_no_handle != -ENOMEM) {
        printf("FAIL %-18s: allocating before init returned %d, and with no "
               "handle %d, expected %d\n",
               name, rc_no_table, rc_no_handle, -ENOMEM);
        ok = false;
    }
    if (dev_res.cq_tbl.used != 0) {
        printf("FAIL %-18s: allocating with no handle took a CQ\n", name);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* ---- SRQ cases ---------------------------------------------------------
 *
 * The resource manager refuses a modification asking for a zero limit or
 * depth before the backend sees it, binds a QP only to an SRQ that exists,
 * and destroys the backend's SRQ once, however often it is freed.
 */
static bool test_srq_lifecycle(void)
{
    const char *name = "srq-lifecycle";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    struct ibv_srq_attr srq_attr;
    uint32_t pd_handle;
    uint32_t cq_handle;
    uint32_t srq_handle;
    uint32_t qpn;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    srq_stubs_reset(0);
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, NULL, &pd_handle) != 0 ||
        rdma_rm_alloc_cq(&dev_res, NULL, 1, &cq_handle) != 0) {
        abort();
    }

    int rc = rdma_rm_alloc_srq(&dev_res, pd_handle, 4, 1, 2, &srq_handle);
    if (rc != 0 || srq_create_calls != 1) {
        printf("FAIL %-18s: allocating returned %d with %u backend call(s)\n",
               name, rc, srq_create_calls);
        ok = false;
    }

    memset(&srq_attr, 0, sizeof(srq_attr));
    rc = rdma_rm_query_srq(&dev_res, srq_handle, &srq_attr);
    if (rc != 0) {
        printf("FAIL %-18s: query returned %d\n", name, rc);
        ok = false;
    }

    /* srq_attr is all zero: a zero limit and a zero depth. */
    int rc_limit =
        rdma_rm_modify_srq(&dev_res, srq_handle, &srq_attr, IBV_SRQ_LIMIT);
    int rc_max_wr =
        rdma_rm_modify_srq(&dev_res, srq_handle, &srq_attr, IBV_SRQ_MAX_WR);
    if (rc_limit != -EINVAL || rc_max_wr != -EINVAL || srq_modify_calls != 0) {
        printf("FAIL %-18s: a zero limit returned %d and a zero depth %d, "
               "with %u backend call(s), expected %d, %d and none\n",
               name, rc_limit, rc_max_wr, srq_modify_calls, -EINVAL, -EINVAL);
        ok = false;
    }
    srq_attr.srq_limit = 3;
    rc = rdma_rm_modify_srq(&dev_res, srq_handle, &srq_attr, IBV_SRQ_LIMIT);
    if (rc != 0 || srq_modify_calls != 1) {
        printf("FAIL %-18s: a limit of 3 returned %d with %u backend "
               "call(s)\n",
               name, rc, srq_modify_calls);
        ok = false;
    }

    rc = rdma_rm_alloc_qp(&dev_res, pd_handle, IBV_QPT_RC, cq_handle, cq_handle,
                          &qpn, 1, srq_handle, NULL);
    if (rc != 0) {
        printf("FAIL %-18s: a QP on the SRQ returned %d\n", name, rc);
        ok = false;
    } else {
        rdma_rm_dealloc_qp(&dev_res, qpn);
    }

    rdma_rm_dealloc_srq(&dev_res, srq_handle);
    rdma_rm_dealloc_srq(&dev_res, srq_handle);
    if (srq_destroy_calls != 1 || dev_res.srq_tbl.used != 0) {
        printf("FAIL %-18s: freeing twice destroyed %u SRQ(s) and left %u\n",
               name, srq_destroy_calls, dev_res.srq_tbl.used);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* A backend that can't create the SRQ must leave the slot free and have
 * nothing to destroy. */
static bool test_srq_backend_fails(void)
{
    const char *name = "srq-backend-fails";
    bool ok = true;
    struct ibv_device_attr a = one_each;
    uint32_t pd_handle;
    uint32_t srq_handle = UINT32_MAX;

    memset(&dev_res, 0, sizeof(dev_res));
    alloc_injectors_reset();
    srq_stubs_reset(BACKEND_ERR);
    if (rdma_rm_init(&dev_res, &a) != 0 ||
        rdma_rm_alloc_pd(&dev_res, NULL, &pd_handle) != 0) {
        abort();
    }

    int rc = rdma_rm_alloc_srq(&dev_res, pd_handle, 4, 1, 2, &srq_handle);
    if (rc != -EIO || dev_res.srq_tbl.used != 0 ||
        rdma_rm_get_srq(&dev_res, srq_handle)) {
        printf("FAIL %-18s: returned %d and left %u SRQ(s), expected %d and "
               "none\n",
               name, rc, dev_res.srq_tbl.used, -EIO);
        ok = false;
    }
    if (srq_destroy_calls != 0) {
        printf("FAIL %-18s: destroy_srq called for an SRQ never created\n",
               name);
        ok = false;
    }

    rdma_rm_fini(&dev_res);

    if (ok) {
        printf("ok   %-18s\n", name);
    }
    return ok;
}

/* Run every case, each printing its own ok or FAIL line, rather than
 * stopping at the first failure; exit 1 if any failed. */
int main(void)
{
    int failures = 0;
    struct ibv_device_attr attr;

    /* The table allocator: a full table of each bitmap shape (see the top
     * of the file), a free from a full table, and the lock around a free. */
    if (!test_fill_to_capacity("full-exact-word", TBL_SZ_EXACT)) {
        failures++;
    }
    if (!test_fill_to_capacity("full-ragged-word", TBL_SZ_RAGGED)) {
        failures++;
    }
    if (!test_dealloc_reopens_one_slot()) {
        failures++;
    }

    if (!test_dealloc_locks()) {
        failures++;
    }

    /* rdma_rm_init(): limits it accepts, zero included, a negative limit
     * it refuses, and a failed table or bitmap allocation unwound. */
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

    /* MRs with and without a backend, and a backend that can't make one. */
    if (!test_mr_lifecycle("mr-no-backend", false)) {
        failures++;
    }
    if (!test_mr_lifecycle("mr-backend", true)) {
        failures++;
    }
    if (!test_mr_backend_fails()) {
        failures++;
    }

    /* The backend's GID index at RTR: one that fits a uint8_t, one that
     * doesn't, and the backend's error. */
    if (!test_gid_index("gid-index-200", 200)) {
        failures++;
    }
    if (!test_gid_index("gid-index-256", 256)) {
        failures++;
    }
    if (!test_gid_index("gid-index-error", -EINVAL)) {
        failures++;
    }

    /* A QP from creation through RTS and a query, with each backend
     * operation on the way failing in turn, then none. */
    static const struct {
        const char *name;
        BackendOp op;
    } backend_cases[] = {
        {"backend-succeeds", OP_NONE},     {"create-pd-fails", OP_CREATE_PD},
        {"create-cq-fails", OP_CREATE_CQ}, {"create-qp-fails", OP_CREATE_QP},
        {"qp-init-fails", OP_QP_INIT},     {"qp-rtr-fails", OP_QP_RTR},
        {"qp-rts-fails", OP_QP_RTS},       {"query-qp-fails", OP_QUERY_QP},
    };
    for (size_t i = 0; i < sizeof(backend_cases) / sizeof(backend_cases[0]);
         i++) {
        if (!test_backend_fails(backend_cases[i].name, backend_cases[i].op)) {
            failures++;
        }
    }

    /* QPs the backend gives a duplicate number, skips (GSI), or doesn't
     * have, then guest handles that name nothing and tables that are full
     * or missing. */
    if (!test_qpn_in_use()) {
        failures++;
    }
    if (!test_gsi_skips_backend()) {
        failures++;
    }
    if (!test_qp_no_backend()) {
        failures++;
    }
    if (!test_bad_handles()) {
        failures++;
    }
    if (!test_tables_full()) {
        failures++;
    }
    if (!test_alloc_refused()) {
        failures++;
    }
    /* SRQs: limits, binding a QP, double free, and a failing backend. */
    if (!test_srq_lifecycle()) {
        failures++;
    }
    if (!test_srq_backend_fails()) {
        failures++;
    }

    if (failures) {
        printf("rdma_res_tbl: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("rdma_res_tbl: all cases passed\n");
    return 0;
}
