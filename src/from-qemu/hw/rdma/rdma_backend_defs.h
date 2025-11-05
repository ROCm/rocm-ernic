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

/* Stub for rdmacm-mux types (MAD handling not used) */
/* Forward declarations */
struct ibv_mad_hdr;
struct ib_user_mad {
    uint32_t agent_id;
    uint32_t status;
    uint32_t timeout_ms;
    uint32_t retries;
    uint8_t  data[0];
};

/* RDMA constants */
#define RDMA_MAX_PRIVATE_DATA 224

/* Stub RDMA CM MUX opcodes and message types */
#define RDMACM_MUX_OP_CODE_REG    1
#define RDMACM_MUX_OP_CODE_UNREG  2
#define RDMACM_MUX_OP_CODE_MAD    3

#define RDMACM_MUX_MSG_TYPE_REQ   1
#define RDMACM_MUX_MSG_TYPE_RESP  2

#define RDMACM_MUX_ERR_CODE_OK    0

struct RdmaCmMuxMsg {
    struct {
        uint32_t msg_type;
        uint32_t msg_len;
        uint32_t op_code;  /* Operation code for GID registration */
        int32_t err_code;  /* Error code for responses */
        union ibv_gid sgid;  /* Source GID */
    } hdr;
    uint32_t umad_len;
    char umad[256];  /* Simplified - real struct is larger */
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
    RdmaBackendThread comp_thread;
    PCIDevice *dev;
    RdmaDeviceResources *rdma_dev_res;
    struct ibv_device *ib_dev;
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    uint8_t port_num;
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
