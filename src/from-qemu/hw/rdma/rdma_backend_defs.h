/*
 *  RDMA device: Definitions of Backend Device structures
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

#ifndef RDMA_BACKEND_DEFS_H
#define RDMA_BACKEND_DEFS_H

#include "qemu/thread.h"
#include "chardev/char-fe.h"
#include <infiniband/verbs.h>
/* MAD multiplexer not needed for standalone operation */
/* #include "contrib/rdmacm-mux/rdmacm-mux.h" */
#include "rdma_utils.h"

/* Forward declare backend ops (defined in rdma_backend_ops.h) */
typedef struct RdmaBackendOps RdmaBackendOps;

/**
 * Backend Types
 * Defined here to avoid circular dependency
 */
typedef enum {
    RDMA_BACKEND_TYPE_NONE,      /* No backend - minimal stubs */
    RDMA_BACKEND_TYPE_LOOPBACK,  /* Internal loopback emulation */
    RDMA_BACKEND_TYPE_VERBS,     /* libibverbs hardware backend */
    RDMA_BACKEND_TYPE_MAX
} RdmaBackendType;

/* Stub for rdmacm-mux types (MAD handling not used) */
/* Forward declarations */
struct ibv_mad_hdr;

/* RDMA constants */
#define RDMA_MAX_PRIVATE_DATA 224

/* Stub RDMA CM MUX opcodes and message types */
#define RDMACM_MUX_OP_CODE_REG   1
#define RDMACM_MUX_OP_CODE_UNREG 2
#define RDMACM_MUX_OP_CODE_MAD   3

#define RDMACM_MUX_MSG_TYPE_REQ  1
#define RDMACM_MUX_MSG_TYPE_RESP 2

#define RDMACM_MUX_ERR_CODE_OK 0

/* ib_user_mad stub - simplified structure */
struct ib_user_mad {
    uint32_t agent_id;
    uint32_t status;
    uint32_t timeout_ms;
    uint32_t retries;
    struct {
        uint32_t qpn;
        uint32_t qkey;
        uint16_t lid;
        uint8_t sl;
        uint8_t path_bits;
        uint8_t grh_present;
        uint8_t gid_index;
        uint8_t hop_limit;
        uint8_t traffic_class;
        uint8_t gid[16];
        uint32_t flow_label;
        uint16_t pkey_index;
        uint8_t reserved[6];
    } addr;
    uint8_t data[0];
};

/* Forward declare backend_umad (defined in rdma_backend.c) */
struct backend_umad {
    struct ib_user_mad hdr;
    char mad[RDMA_MAX_PRIVATE_DATA];
};

struct RdmaCmMuxMsg {
    struct {
        uint32_t msg_type;
        uint32_t msg_len;
        uint32_t op_code;   /* Operation code for GID registration */
        int32_t err_code;   /* Error code for responses */
        union ibv_gid sgid; /* Source GID */
    } hdr;
    uint32_t umad_len;
    struct backend_umad umad; /* MAD message data */
};
typedef struct RdmaCmMuxMsg RdmaCmMuxMsg;

struct RdmaCmMux {
    CharBackend *chr_be;
    int can_receive;
};
typedef struct RdmaCmMux RdmaCmMux;

typedef struct RdmaDeviceResources RdmaDeviceResources;

typedef struct RdmaBackendThread {
    QemuThread thread;
    bool run; /* Set by thread manager to let thread know it should exit */
    bool is_running; /* Set by the thread to report its status */
} RdmaBackendThread;

typedef struct RdmaBackendDev {
    /* Backend abstraction */
    RdmaBackendType backend_type;
    const RdmaBackendOps *backend_ops;
    void *backend_private;  /* Backend-specific data */
    
    /* Common fields */
    RdmaBackendThread comp_thread;
    PCIDevice *dev;
    RdmaDeviceResources *rdma_dev_res;
    uint8_t port_num;
    
    /* Verbs-specific fields (kept for verbs backend) */
    struct ibv_device *ib_dev;
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    RdmaProtectedGQueue recv_mads_list;
    RdmaCmMux rdmacm_mux;
} RdmaBackendDev;

typedef struct RdmaBackendPD {
    struct ibv_pd *ibpd;
} RdmaBackendPD;

typedef struct RdmaBackendMR {
    struct ibv_pd *ibpd;
    struct ibv_mr *ibmr;
} RdmaBackendMR;

typedef struct RdmaBackendCQ {
    RdmaBackendDev *backend_dev;
    struct ibv_cq *ibcq;
} RdmaBackendCQ;

typedef struct RdmaBackendQP {
    struct ibv_pd *ibpd;
    struct ibv_qp *ibqp;
    uint8_t sgid_idx;
    RdmaProtectedGSList cqe_ctx_list;
} RdmaBackendQP;

typedef struct RdmaBackendSRQ {
    struct ibv_srq *ibsrq;
    RdmaProtectedGSList cqe_ctx_list;
} RdmaBackendSRQ;

#endif
