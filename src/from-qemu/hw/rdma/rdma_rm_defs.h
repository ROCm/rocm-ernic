/*
 * RDMA device: Definitions of Resource Manager structures
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

#ifndef RDMA_RM_DEFS_H
#define RDMA_RM_DEFS_H

#include <glib.h> /* For GHashTable */
#include "rdma_backend_defs.h"

#define PAGE_SIZE 4096

#define MAX_PORTS           1 /* Do not change - we support only one port */
#define MAX_PORT_GIDS       255
#define MAX_GIDS            MAX_PORT_GIDS
#define MAX_PORT_PKEYS      1
#define MAX_PKEYS           MAX_PORT_PKEYS
#define MAX_UCS             512
#define MAX_MR_SIZE         (1UL << 32)
#define MAX_QP              1024
#define MAX_SGE             32
#define MAX_CQ              2048
#define MAX_MR              1024
#define MAX_PD              1024
#define MAX_QP_RD_ATOM      16
#define MAX_QP_INIT_RD_ATOM 16
#define MAX_AH              64
#define MAX_SRQ             512

/* Paravirt queue pair types for rocm_ernic Dynamic Connection (software DC) */
#define ROCM_ERNIC_PVRDMA_QPT_DCT 240U
#define ROCM_ERNIC_PVRDMA_QPT_DCI 241U
#define ROCM_ERNIC_DC_ROLE_NONE   0U
#define ROCM_ERNIC_DC_ROLE_DCT    1U
#define ROCM_ERNIC_DC_ROLE_DCI    2U

/*
 * Number of page-table pages backing each WQ/CQ ring.
 * Upstream PVRDMA uses 1; we double it so that bidirectional
 * traffic at 64 KB+ does not exhaust MR page-table entries
 * when both send and recv rings are fully populated.
 */
#define PVRDMA_PG_TBL_PAGES 2

#define MAX_RM_TBL_NAME          16
#define MAX_CONSEQ_EMPTY_POLL_CQ 4096 /* considered as error above this */

typedef struct RdmaRmResTbl {
    char name[MAX_RM_TBL_NAME];
    QemuMutex lock;
    unsigned long *bitmap;
    size_t tbl_sz;
    size_t res_sz;
    void *tbl;
    uint32_t used; /* number of used entries in the table */
} RdmaRmResTbl;

typedef struct RdmaRmPD {
    RdmaBackendPD backend_pd;
    uint32_t ctx_handle;
} RdmaRmPD;

typedef enum CQNotificationType {
    CNT_CLEAR,
    CNT_ARM,
    CNT_SET,
} CQNotificationType;

typedef struct RdmaRmCQ {
    RdmaBackendCQ backend_cq;
    void *opaque;
    CQNotificationType notify;
} RdmaRmCQ;

/* MR (DMA region) */
typedef struct RdmaRmMR {
    RdmaBackendMR backend_mr;
    void *virt;
    uint64_t start;
    size_t length;
    uint32_t pd_handle;
    uint32_t lkey;
    uint32_t rkey;
} RdmaRmMR;

typedef struct RdmaRmUC {
    uint64_t uc_handle;
} RdmaRmUC;

/* Per-direction WQE processing state for incremental batch processing */
typedef struct RdmaRmQPWqeProcessingState {
    bool send_processing_active;
    uint32_t send_wqes_processed;
    bool recv_processing_active;
    uint32_t recv_wqes_processed;
} RdmaRmQPWqeProcessingState;

typedef struct RdmaRmQP {
    RdmaBackendQP backend_qp;
    void *opaque;
    uint32_t qp_type;
    uint32_t qpn;
    uint32_t send_cq_handle;
    uint32_t recv_cq_handle;
    enum ibv_qp_state qp_state;
    uint8_t is_srq;
    RdmaRmQPWqeProcessingState wqe_state;
    _Atomic uint32_t send_in_flight;
    uint8_t dc_role;
    uint8_t dc_pad[3];
    uint32_t dctn;
    uint64_t dct_access_key;
    uint32_t bound_srq_handle;
} RdmaRmQP;

/* WQE processing state for SRQ incremental batch processing */
typedef struct RdmaRmSRQWqeProcessingState {
    bool processing_active;  /* Is this SRQ currently processing WQEs? */
    uint32_t wqes_processed; /* Count for this batch */
} RdmaRmSRQWqeProcessingState;

typedef struct RdmaRmSRQ {
    RdmaBackendSRQ backend_srq;
    uint32_t recv_cq_handle;
    void *opaque;
    RdmaRmSRQWqeProcessingState wqe_state; /* WQE processing state */
} RdmaRmSRQ;

typedef struct RdmaRmGid {
    union ibv_gid gid;
    int backend_gid_index;
} RdmaRmGid;

typedef struct RdmaRmPort {
    RdmaRmGid gid_tbl[MAX_PORT_GIDS];
    enum ibv_port_state state;
} RdmaRmPort;

typedef struct RdmaRmStats {
    uint64_t tx;
    uint64_t tx_len;
    uint64_t tx_err;
    uint64_t rx_bufs;
    uint64_t rx_bufs_len;
    uint64_t rx_bufs_err;
    uint64_t rx_srq;
    uint64_t completions;
    uint64_t mad_tx;
    uint64_t mad_tx_err;
    uint64_t mad_rx;
    uint64_t mad_rx_err;
    uint64_t mad_rx_bufs;
    uint64_t mad_rx_bufs_err;
    uint64_t poll_cq_from_bk;
    uint64_t poll_cq_from_guest;
    uint64_t poll_cq_from_guest_empty;
    uint64_t poll_cq_ppoll_to;
    uint32_t missing_cqe;
} RdmaRmStats;

struct RdmaDeviceResources {
    RdmaRmPort port;
    RdmaRmResTbl pd_tbl;
    RdmaRmResTbl mr_tbl;
    RdmaRmResTbl uc_tbl;
    RdmaRmResTbl qp_tbl;
    RdmaRmResTbl cq_tbl;
    RdmaRmResTbl cqe_ctx_tbl;
    RdmaRmResTbl srq_tbl;
    GHashTable *qp_hash;  /* Keeps mapping between real and emulated */
    GHashTable *dct_hash; /* DCT number -> RdmaRmQP (DC targets) */
    uint32_t next_dctn;
    QemuMutex dc_lock;
    QemuMutex lock;
    RdmaRmStats stats;
};

#endif
