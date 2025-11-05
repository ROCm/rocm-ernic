/*
 * RDMA device: Debug utilities
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include <glib.h>  /* For GQueue, GSList */
#include "qemu/thread.h"  /* For QemuMutex */
#include "qemu/error-report.h"
#include "sysemu/dma.h"

/* Forward declarations */
typedef struct PCIDevice PCIDevice;

#define rdma_error_report(fmt, ...) \
    error_report("%s: " fmt, "rdma", ## __VA_ARGS__)
#define rdma_warn_report(fmt, ...) \
    warn_report("%s: " fmt, "rdma", ## __VA_ARGS__)
#define rdma_info_report(fmt, ...) \
    info_report("%s: " fmt, "rdma", ## __VA_ARGS__)

/* Optional debug tracing - define RDMA_DEBUG to enable */
#ifdef RDMA_DEBUG
#define rdma_debug_report(fmt, ...) \
    fprintf(stderr, "rdma-debug: " fmt "\n", ## __VA_ARGS__)
#else
#define rdma_debug_report(fmt, ...) \
    do { } while (0)
#endif

typedef struct RdmaProtectedGQueue {
    QemuMutex lock;
    GQueue *list;
} RdmaProtectedGQueue;

typedef struct RdmaProtectedGSList {
    QemuMutex lock;
    GSList *list;
} RdmaProtectedGSList;

/* DMA functions moved to vfu_compat_bridge.c - now using pci_dma_* directly */
void rdma_protected_gqueue_init(RdmaProtectedGQueue *list);
void rdma_protected_gqueue_destroy(RdmaProtectedGQueue *list);
void rdma_protected_gqueue_append_int64(RdmaProtectedGQueue *list,
                                        int64_t value);
int64_t rdma_protected_gqueue_pop_int64(RdmaProtectedGQueue *list);
void rdma_protected_gslist_init(RdmaProtectedGSList *list);
void rdma_protected_gslist_destroy(RdmaProtectedGSList *list);
void rdma_protected_gslist_append_int32(RdmaProtectedGSList *list,
                                        int32_t value);
void rdma_protected_gslist_remove_int32(RdmaProtectedGSList *list,
                                        int32_t value);

static inline void addrconf_addr_eui48(uint8_t *eui, const char *addr)
{
    memcpy(eui, addr, 3);
    eui[3] = 0xFF;
    eui[4] = 0xFE;
    memcpy(eui + 5, addr + 3, 3);
    eui[0] ^= 2;
}

#endif
