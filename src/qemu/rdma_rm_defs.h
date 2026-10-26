/*
 * RDMA device: Definitions of Resource Manager structures
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 *
 */

#ifndef RDMA_RM_DEFS_H
#define RDMA_RM_DEFS_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>        /* For GHashTable */
#include "qemu/thread.h" /* For QemuMutex */
#include "rdma_backend_defs.h"

#define PAGE_SIZE 4096

#define MAX_MR_SIZE         (1UL << 32)
#define MAX_QP              1024
#define MAX_SGE             32
#define MAX_CQ              2048
#define MAX_PD              1024
#define MAX_QP_RD_ATOM      16
#define MAX_QP_INIT_RD_ATOM 16
#define MAX_AH              64
#define MAX_SRQ             512

/*
 * Must stay equal to IONIC_MAX_MR in src/ionic/ionic_datapath.h, which explains
 * the value; this header is QEMU-derived and cannot include that one.  It was
 * 1024, which is exactly the 8 * 128 regions an nvme-rdma initiator
 * pre-allocates for the default eight NVMe-oF I/O queues -- leaving none for
 * the admin queue, so the connect failed on the last queue with -ENOMEM.
 */
#define MAX_MR 2048

/*
 * Number of page-table pages backing each WQ/CQ ring.
 * Upstream PVRDMA uses 1; we double it so that bidirectional
 * traffic at 64 KB+ does not exhaust MR page-table entries
 * when both send and recv rings are fully populated.
 */
#define PVRDMA_PG_TBL_PAGES 2

#define MAX_RM_TBL_NAME 16

typedef struct RdmaRmResTbl {
    char name[MAX_RM_TBL_NAME];
    /*
     * Held to change bitmap and used. rdma_res_tbl_get() reads bitmap
     * without it. It protects nothing today, since only the main thread uses
     * the tables (see RdmaDeviceResources), and locking the lookup wouldn't
     * make them safe for a second thread.
     */
    QemuMutex lock;
    unsigned long *bitmap;
    size_t tbl_sz;
    size_t res_sz;
    uint8_t *tbl;  /* tbl_sz entries of res_sz bytes */
    uint32_t used; /* number of used entries in the table */
} RdmaRmResTbl;

typedef struct RdmaRmPD {
    RdmaBackendPD backend_pd;
} RdmaRmPD;

typedef struct RdmaRmCQ {
    RdmaBackendCQ backend_cq;
} RdmaRmCQ;

/*
 * MR: a handle and the backend's MR, and nothing else. The resource manager
 * maps no memory for it; the ionic data path keeps the guest's page table
 * (ionic_datapath_register_mr()).
 */
typedef struct RdmaRmMR {
    RdmaBackendMR backend_mr;
} RdmaRmMR;

/*
 * A QP records none of the PD, CQs and SRQ it was created with, and no
 * backend keeps a pointer to them, which is why freeing one of them while
 * the QP lives leaves nothing dangling (see rdma_rm_dealloc_cq()). Adding
 * such a reference means adding reference counts to go with it.
 */
typedef struct RdmaRmQP {
    RdmaBackendQP backend_qp;
    uint8_t qp_type; /* as rdma_rm_alloc_qp() and the backends take it */
    uint32_t qpn;
    enum ibv_qp_state qp_state;
    /* The backends do not model PSNs, so the values the guest set are only
     * recoverable from here. */
    uint32_t rq_psn;
    uint32_t sq_psn;
} RdmaRmQP;

typedef struct RdmaRmSRQ {
    RdmaBackendSRQ backend_srq;
} RdmaRmSRQ;

/*
 * Only the main thread may use the resource manager. Every call comes from
 * the adminq (ionic_adminq_poll(), and ionic_adminq_release_all() at an RDMA
 * reset, both from the server's poll loop) or from device setup and
 * teardown. The TCP backend's threads and the data path never touch it.
 *
 * Nothing here relies on a lock: qp_hash has none, nor do the slots'
 * contents, and a pointer from rdma_rm_get_*() stays valid only until the
 * next free, on the same thread. Each table's lock covers only its bitmap
 * and count, inside alloc and free. Calling in from another thread would
 * need one lock over the tables, qp_hash and the slots, held for as long as
 * the caller uses the pointer it got, or reference counts on the slots.
 */
struct RdmaDeviceResources {
    RdmaRmResTbl pd_tbl;
    RdmaRmResTbl mr_tbl;
    RdmaRmResTbl qp_tbl;
    RdmaRmResTbl cq_tbl;
    RdmaRmResTbl srq_tbl;
    GHashTable *qp_hash; /* Keeps mapping between real and emulated */
};

#endif
