/*
 * QEMU paravirtual RDMA - Device rings
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* Minimal includes instead of qemu/osdep.h */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include "hw/pci/pci.h"
#include "hw/rdma/rdma.h" /* For rdma_pci_dma_map declaration */
/* #include "cpu.h" - Not needed for PVRDMA */
#include "qemu/cutils.h"
#include "qemu/atomic.h"
#include "glib-compat.h"


#include "../rdma_utils.h"
#include "pvrdma_dev_ring.h"
#include "pvrdma.h"

int pvrdma_ring_init(PvrdmaRing *ring, const char *name, PCIDevice *dev,
                     PvrdmaRingState *ring_state, uint32_t max_elems,
                     size_t elem_sz, dma_addr_t *tbl, uint32_t npages)
{
    int i;
    int rc = 0;

    pstrcpy(ring->name, MAX_RING_NAME_SZ, name);
    ring->dev = dev;
    ring->ring_state = ring_state;
    ring->max_elems = max_elems;
    ring->elem_sz = elem_sz;
    /* TODO: Give a moment to think if we want to redo driver settings
    qatomic_set(&ring->ring_state->prod_tail, 0);
    qatomic_set(&ring->ring_state->cons_head, 0);
    */
    ring->npages = npages;
    ring->pages = g_new0(void *, npages);

    rdma_info_report(">>> pvrdma_ring_init: sizeof(void*)=%zu, sizeof(unsigned "
                     "long)=%zu, sizeof(uintptr_t)=%zu",
                     sizeof(void *), sizeof(unsigned long), sizeof(uintptr_t));
    rdma_info_report(
        ">>> pvrdma_ring_init: ring=%p, allocating pages array at %p", ring,
        ring->pages);

    for (i = 0; i < npages; i++) {
        void *mapped_addr;

        if (!tbl[i]) {
            rdma_error_report("npages=%d but tbl[%d] is NULL", npages, i);
            continue;
        }

        mapped_addr = rdma_pci_dma_map(dev, tbl[i], PAGE_SIZE);
        printf("DIRECT_PRINTF: page[%d] mapped_addr=%p (uintptr=%#lx)\n", i,
               mapped_addr, (uintptr_t)mapped_addr);
        fflush(stdout);
        rdma_info_report(">>> pvrdma_ring_init: page[%d]: guest=0x%lx -> "
                         "mapped_addr=%p (as lx=%#lx)",
                         i, tbl[i], mapped_addr, (unsigned long)mapped_addr);

        ring->pages[i] = mapped_addr;
        printf("DIRECT_PRINTF: page[%d] STORED ring->pages[%d]=%p "
               "(uintptr=%#lx)\n",
               i, i, ring->pages[i], (uintptr_t)ring->pages[i]);
        fflush(stdout);
        rdma_info_report(">>> pvrdma_ring_init: page[%d]: STORED "
                         "ring->pages[%d]=%p (as lx=%#lx)",
                         i, i, ring->pages[i], (unsigned long)ring->pages[i]);

        if (!ring->pages[i]) {
            rc = -ENOMEM;
            rdma_error_report("Failed to map to page %d in ring %s", i, name);
            goto out_free;
        }
        /* NOTE: Don't memset guest-owned memory in vfio-user model.
         * The guest driver initializes its own pages. In QEMU's internal
         * model this worked because QEMU has direct RAM access, but in
         * vfio-user we only get a mapped pointer that may not be writable. */
        /* memset(ring->pages[i], 0, PAGE_SIZE); */
    }

    goto out;

out_free:
    while (i--) {
        rdma_pci_dma_unmap(dev, ring->pages[i], PAGE_SIZE);
    }
    g_free(ring->pages);

out:
    return rc;
}

void *pvrdma_ring_next_elem_read(PvrdmaRing *ring)
{
    unsigned int idx, offset;
    unsigned int page_idx;
    void *result;
    const uint32_t tail = qatomic_read(&ring->ring_state->prod_tail);
    const uint32_t head = qatomic_read(&ring->ring_state->cons_head);

    rdma_info_report(">>> pvrdma_ring_next_elem_read: "
                     "ring=%p tail=%u head=%u "
                     "max=%u ring_state=%p",
                     ring, tail, head, ring->max_elems,
                     (void *)ring->ring_state);

    if (tail & ~((ring->max_elems << 1) - 1) ||
        head & ~((ring->max_elems << 1) - 1) || tail == head) {
        return NULL;
    }

    idx = head & (ring->max_elems - 1);
    offset = idx * ring->elem_sz;
    page_idx = offset / PAGE_SIZE;

    if (page_idx >= ring->npages) {
        rdma_error_report("ring %s: page_idx %u >= npages %u",
                          ring->name, page_idx, ring->npages);
        return NULL;
    }

    if (!ring->pages[page_idx]) {
        rdma_error_report("ring %s: pages[%u] is NULL",
                          ring->name, page_idx);
        return NULL;
    }

    rdma_info_report(
        ">>> pvrdma_ring_next_elem_read: idx=%u, offset=%u, page_idx=%u", idx,
        offset, page_idx);
    rdma_info_report(
        ">>> pvrdma_ring_next_elem_read: ring->pages[%u]=%p (as lx=%#lx)",
        page_idx, ring->pages[page_idx], (unsigned long)ring->pages[page_idx]);

    result = ring->pages[page_idx] + (offset % PAGE_SIZE);
    rdma_info_report(
        ">>> pvrdma_ring_next_elem_read: returning %p (as lx=%#lx)", result,
        (unsigned long)result);

    return result;
}

void pvrdma_ring_read_inc(PvrdmaRing *ring)
{
    uint32_t idx = qatomic_read(&ring->ring_state->cons_head);

    idx = (idx + 1) & ((ring->max_elems << 1) - 1);
    qatomic_set(&ring->ring_state->cons_head, idx);
}

void *pvrdma_ring_next_elem_write(PvrdmaRing *ring)
{
    unsigned int idx, offset;
    const uint32_t tail = qatomic_read(&ring->ring_state->prod_tail);
    const uint32_t head = qatomic_read(&ring->ring_state->cons_head);

    if (tail & ~((ring->max_elems << 1) - 1) ||
        head & ~((ring->max_elems << 1) - 1) ||
        tail == (head ^ ring->max_elems)) {
        rdma_error_report("CQ is full");
        return NULL;
    }

    idx = tail & (ring->max_elems - 1);
    offset = idx * ring->elem_sz;
    unsigned int page_idx = offset / PAGE_SIZE;

    if (page_idx >= ring->npages) {
        rdma_error_report("ring %s: write page_idx %u >= npages %u",
                          ring->name, page_idx, ring->npages);
        return NULL;
    }

    if (!ring->pages[page_idx]) {
        rdma_error_report("ring %s: write pages[%u] is NULL",
                          ring->name, page_idx);
        return NULL;
    }

    return ring->pages[page_idx] + (offset % PAGE_SIZE);
}

void pvrdma_ring_write_inc(PvrdmaRing *ring)
{
    uint32_t idx = qatomic_read(&ring->ring_state->prod_tail);

    idx = (idx + 1) & ((ring->max_elems << 1) - 1);
    qatomic_set(&ring->ring_state->prod_tail, idx);
}

void pvrdma_ring_free(PvrdmaRing *ring)
{
    if (!ring) {
        return;
    }

    if (!ring->pages) {
        return;
    }

    while (ring->npages--) {
        rdma_pci_dma_unmap(ring->dev, ring->pages[ring->npages], PAGE_SIZE);
    }

    g_free(ring->pages);
    ring->pages = NULL;
}
