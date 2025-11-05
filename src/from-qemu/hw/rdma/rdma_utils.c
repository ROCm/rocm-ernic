/*
 * QEMU paravirtual RDMA - Generic RDMA backend
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
#include <inttypes.h>

/* GLib types for queue/list management */
#include <glib.h>

#include "hw/pci/pci_device.h"
#include "rdma_utils.h"

/* DMA functions moved to vfu_compat_bridge.c for libvfio-user implementation */

void rdma_protected_gqueue_init(RdmaProtectedGQueue *list)
{
    qemu_mutex_init(&list->lock);
    list->list = g_queue_new();
}

void rdma_protected_gqueue_destroy(RdmaProtectedGQueue *list)
{
    if (list->list) {
        g_queue_free_full(list->list, g_free);
        qemu_mutex_destroy(&list->lock);
        list->list = NULL;
    }
}

void rdma_protected_gqueue_append_int64(RdmaProtectedGQueue *list,
                                        int64_t value)
{
    qemu_mutex_lock(&list->lock);
    g_queue_push_tail(list->list, g_memdup(&value, sizeof(value)));
    qemu_mutex_unlock(&list->lock);
}

int64_t rdma_protected_gqueue_pop_int64(RdmaProtectedGQueue *list)
{
    int64_t *valp;
    int64_t val;

    qemu_mutex_lock(&list->lock);

    valp = g_queue_pop_head(list->list);
    qemu_mutex_unlock(&list->lock);

    if (!valp) {
        return -ENOENT;
    }

    val = *valp;
    g_free(valp);
    return val;
}

void rdma_protected_gslist_init(RdmaProtectedGSList *list)
{
    qemu_mutex_init(&list->lock);
}

void rdma_protected_gslist_destroy(RdmaProtectedGSList *list)
{
    if (list->list) {
        g_slist_free(list->list);
        qemu_mutex_destroy(&list->lock);
        list->list = NULL;
    }
}

void rdma_protected_gslist_append_int32(RdmaProtectedGSList *list,
                                        int32_t value)
{
    qemu_mutex_lock(&list->lock);
    list->list = g_slist_prepend(list->list, GINT_TO_POINTER(value));
    qemu_mutex_unlock(&list->lock);
}

void rdma_protected_gslist_remove_int32(RdmaProtectedGSList *list,
                                        int32_t value)
{
    qemu_mutex_lock(&list->lock);
    list->list = g_slist_remove(list->list, GINT_TO_POINTER(value));
    qemu_mutex_unlock(&list->lock);
}
