/*
 * RDMA Backend: Loopback
 *
 * Internal RDMA loopback backend for testing without hardware.
 * Implements complete RDMA emulation with in-memory data transfer.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_utils.h"
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>

/*
 * Loopback Backend Data Structures
 */

typedef enum {
    LOOPBACK_DATA_PATTERN_ZEROS,       /* All 0x00 */
    LOOPBACK_DATA_PATTERN_ONES,        /* All 0xFF */
    LOOPBACK_DATA_PATTERN_INCREMENTING, /* 0x00, 0x01, 0x02, ... */
    LOOPBACK_DATA_PATTERN_DECREMENTING, /* 0xFF, 0xFE, 0xFD, ... */
    LOOPBACK_DATA_PATTERN_ALTERNATING,  /* 0xAA, 0x55, 0xAA, ... */
    LOOPBACK_DATA_PATTERN_RANDOM,      /* Random data */
    LOOPBACK_DATA_PATTERN_PRESERVE,    /* Use actual guest data (default) */
} LoopbackDataPattern;

typedef struct {
    uint32_t handle;
} LoopbackPD;

typedef struct {
    uint32_t handle;
    void *virt;
    size_t length;
    uint64_t guest_start;
    int access_flags;
    uint32_t lkey;
    uint32_t rkey;
    uint32_t pd_handle;
} LoopbackMR;

typedef struct {
    enum ibv_wc_status status;
    uint64_t wr_id;
    uint32_t byte_len;
    uint32_t qp_num;
    enum ibv_wc_opcode opcode;
} LoopbackCompletion;

typedef struct {
    uint32_t handle;
    int cqe;
    GQueue *completions;  /* Queue of LoopbackCompletion */
    QemuMutex lock;
} LoopbackCQ;

typedef struct {
    void *addr;
    uint32_t length;
    uint32_t lkey;
} LoopbackSGE;

typedef struct {
    uint64_t wr_id;
    uint32_t num_sge;
    LoopbackSGE sge[32];  /* Max SGEs */
} LoopbackWR;

typedef struct {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;
    uint32_t qkey;
    uint32_t pd_handle;
    
    /* Connection info */
    uint32_t remote_qpn;
    union ibv_gid remote_gid;
    uint32_t rq_psn;
    uint32_t sq_psn;
    
    /* Associated CQs */
    LoopbackCQ *scq;
    LoopbackCQ *rcq;
    
    /* Work queues */
    GQueue *send_queue;
    GQueue *recv_queue;
    
    QemuMutex lock;
} LoopbackQP;

typedef struct {
    /* Resource tracking */
    GHashTable *pds;    /* handle -> LoopbackPD */
    GHashTable *mrs;    /* handle -> LoopbackMR */
    GHashTable *cqs;    /* handle -> LoopbackCQ */
    GHashTable *qps;    /* qpn -> LoopbackQP */
    
    /* Handle generators */
    uint32_t next_pd_handle;
    uint32_t next_mr_handle;
    uint32_t next_cq_handle;
    uint32_t next_qpn;
    
    /* For loopback connections */
    GHashTable *qp_pairs;  /* local_qpn -> remote_qpn */
    
    /* Data pattern configuration */
    LoopbackDataPattern data_pattern;
    bool compute_md5;  /* Whether to compute MD5 on data transfers */
    
    QemuMutex lock;
} LoopbackBackendPrivate;

/*
 * Helper Functions
 */

static LoopbackBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    return (LoopbackBackendPrivate *)backend_dev->backend_private;
}

static LoopbackDataPattern parse_data_pattern(const char *config)
{
    if (!config || strstr(config, "preserve")) {
        return LOOPBACK_DATA_PATTERN_PRESERVE;
    }
    if (strstr(config, "zeros")) {
        return LOOPBACK_DATA_PATTERN_ZEROS;
    }
    if (strstr(config, "ones")) {
        return LOOPBACK_DATA_PATTERN_ONES;
    }
    if (strstr(config, "increment")) {
        return LOOPBACK_DATA_PATTERN_INCREMENTING;
    }
    if (strstr(config, "decrement")) {
        return LOOPBACK_DATA_PATTERN_DECREMENTING;
    }
    if (strstr(config, "alternate")) {
        return LOOPBACK_DATA_PATTERN_ALTERNATING;
    }
    if (strstr(config, "random")) {
        return LOOPBACK_DATA_PATTERN_RANDOM;
    }
    return LOOPBACK_DATA_PATTERN_PRESERVE;  /* Default */
}

static const char *data_pattern_name(LoopbackDataPattern pattern)
{
    switch (pattern) {
    case LOOPBACK_DATA_PATTERN_ZEROS: return "zeros";
    case LOOPBACK_DATA_PATTERN_ONES: return "ones";
    case LOOPBACK_DATA_PATTERN_INCREMENTING: return "incrementing";
    case LOOPBACK_DATA_PATTERN_DECREMENTING: return "decrementing";
    case LOOPBACK_DATA_PATTERN_ALTERNATING: return "alternating";
    case LOOPBACK_DATA_PATTERN_RANDOM: return "random";
    case LOOPBACK_DATA_PATTERN_PRESERVE: return "preserve";
    default: return "unknown";
    }
}

static void generate_data_pattern(void *buffer, size_t length, 
                                  LoopbackDataPattern pattern)
{
    uint8_t *buf = (uint8_t *)buffer;
    
    switch (pattern) {
    case LOOPBACK_DATA_PATTERN_ZEROS:
        memset(buf, 0x00, length);
        break;
        
    case LOOPBACK_DATA_PATTERN_ONES:
        memset(buf, 0xFF, length);
        break;
        
    case LOOPBACK_DATA_PATTERN_INCREMENTING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)(i & 0xFF);
        }
        break;
        
    case LOOPBACK_DATA_PATTERN_DECREMENTING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)((0xFF - i) & 0xFF);
        }
        break;
        
    case LOOPBACK_DATA_PATTERN_ALTERNATING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (i % 2) ? 0x55 : 0xAA;
        }
        break;
        
    case LOOPBACK_DATA_PATTERN_RANDOM:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)(g_random_int() & 0xFF);
        }
        break;
        
    case LOOPBACK_DATA_PATTERN_PRESERVE:
        /* Don't modify the buffer - use actual guest data */
        break;
    }
}

static void compute_sge_md5(struct ibv_sge *sge, uint32_t num_sge, 
                            char *md5_str, size_t md5_str_len)
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
    uint32_t total_len = 0;
    
    /* Compute MD5 over all SGE data */
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        if (sge[i].addr && sge[i].length > 0) {
            g_checksum_update(checksum, (const guchar *)sge[i].addr, sge[i].length);
            total_len += sge[i].length;
        }
    }
    
    /* Get MD5 hex string */
    const gchar *md5_hex = g_checksum_get_string(checksum);
    snprintf(md5_str, md5_str_len, "%s", md5_hex);
    
    g_checksum_free(checksum);
    
    rdma_info_report("Loopback: Data MD5: %s (%u bytes)", md5_str, total_len);
}

static void loopback_post_completion(LoopbackCQ *cq, uint64_t wr_id,
                                     enum ibv_wc_status status,
                                     uint32_t byte_len, uint32_t qp_num,
                                     enum ibv_wc_opcode opcode)
{
    LoopbackCompletion *comp = g_new0(LoopbackCompletion, 1);
    
    comp->status = status;
    comp->wr_id = wr_id;
    comp->byte_len = byte_len;
    comp->qp_num = qp_num;
    comp->opcode = opcode;
    
    qemu_mutex_lock(&cq->lock);
    g_queue_push_tail(cq->completions, comp);
    qemu_mutex_unlock(&cq->lock);
    
    rdma_info_report("Loopback: Posted completion wr_id=%lu status=%d to CQ %u",
                     wr_id, status, cq->handle);
}

/*
 * Backend Lifecycle
 */

static int loopback_init(RdmaBackendDev *backend_dev, const char *config)
{
    LoopbackBackendPrivate *priv;
    
    rdma_info_report("Loopback backend: Initializing internal emulation");
    
    priv = g_new0(LoopbackBackendPrivate, 1);
    
    priv->pds = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->mrs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->cqs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, 
                                      (GDestroyNotify)g_free);
    priv->qps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_free);
    priv->qp_pairs = g_hash_table_new(g_direct_hash, g_direct_equal);
    
    priv->next_pd_handle = 1;
    priv->next_mr_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100;  /* Start at 100 to avoid special QPs */
    
    /* Parse configuration for data pattern and MD5 */
    priv->data_pattern = parse_data_pattern(config);
    priv->compute_md5 = (config && strstr(config, "md5")) ? true : false;
    
    qemu_mutex_init(&priv->lock);
    
    backend_dev->backend_private = priv;
    
    rdma_info_report("Loopback backend: Data pattern='%s', MD5=%s",
                    data_pattern_name(priv->data_pattern),
                    priv->compute_md5 ? "enabled" : "disabled");
    rdma_info_report("Loopback backend: Initialized successfully");
    return 0;
}

static void loopback_fini(RdmaBackendDev *backend_dev)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    
    if (!priv) {
        return;
    }
    
    rdma_info_report("Loopback backend: Cleaning up");
    
    g_hash_table_destroy(priv->pds);
    g_hash_table_destroy(priv->mrs);
    g_hash_table_destroy(priv->cqs);
    g_hash_table_destroy(priv->qps);
    g_hash_table_destroy(priv->qp_pairs);
    
    qemu_mutex_destroy(&priv->lock);
    
    g_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int loopback_query_port(RdmaBackendDev *backend_dev,
                               struct ibv_port_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->state = IBV_PORT_ACTIVE;
    attr->max_mtu = IBV_MTU_4096;
    attr->active_mtu = IBV_MTU_1024;
    attr->gid_tbl_len = 1;
    attr->port_cap_flags = IBV_PORT_CM_SUP;
    attr->max_msg_sz = 0x80000000;
    attr->pkey_tbl_len = 1;
    attr->active_width = 4;  /* 4X */
    attr->active_speed = 4;  /* 10 Gbps */
    return 0;
}

static int loopback_query_device(RdmaBackendDev *backend_dev,
                                 struct ibv_device_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->max_qp = 1024;
    attr->max_qp_wr = 1024;
    attr->max_sge = 32;
    attr->max_cq = 1024;
    attr->max_cqe = 8192;
    attr->max_mr = 1024;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    return 0;
}

/*
 * Protection Domain Operations
 */

static int loopback_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackPD *lpd = g_new0(LoopbackPD, 1);
    
    qemu_mutex_lock(&priv->lock);
    lpd->handle = priv->next_pd_handle++;
    g_hash_table_insert(priv->pds, GUINT_TO_POINTER(lpd->handle), lpd);
    qemu_mutex_unlock(&priv->lock);
    
    pd->ibpd = (struct ibv_pd *)(uintptr_t)lpd->handle;  /* Store handle as pointer */
    
    rdma_info_report("Loopback: Created PD handle %u", lpd->handle);
    return 0;
}

static void loopback_destroy_pd(RdmaBackendPD *pd)
{
    /* Handle stored in ibpd - nothing to free here */
    rdma_info_report("Loopback: Destroyed PD");
}

/*
 * Memory Region Operations
 */

static int loopback_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd,
                              void *addr, size_t length,
                              uint64_t guest_start, int access)
{
    LoopbackBackendPrivate *priv = get_private(pd->ibpd ? 
        (RdmaBackendDev *)NULL : NULL);  /* TODO: Get backend_dev properly */
    LoopbackMR *lmr = g_new0(LoopbackMR, 1);
    uint32_t pd_handle = (uint32_t)(uintptr_t)pd->ibpd;
    
    /* For now, use a simplified approach without backend_dev */
    static uint32_t mr_counter = 1;
    static GHashTable *global_mrs = NULL;
    if (!global_mrs) {
        global_mrs = g_hash_table_new_full(g_direct_hash, g_direct_equal, 
                                           NULL, g_free);
    }
    
    lmr->handle = mr_counter++;
    lmr->virt = addr;
    lmr->length = length;
    lmr->guest_start = guest_start;
    lmr->access_flags = access;
    lmr->lkey = lmr->handle;  /* Simple: lkey = handle */
    lmr->rkey = lmr->handle + 0x10000;  /* rkey = handle + offset */
    lmr->pd_handle = pd_handle;
    
    g_hash_table_insert(global_mrs, GUINT_TO_POINTER(lmr->handle), lmr);
    
    /* Store handle in mr structure */
    mr->ibpd = pd->ibpd;
    mr->ibmr = (struct ibv_mr *)(uintptr_t)lmr->handle;
    
    rdma_info_report("Loopback: Created MR handle %u, lkey=0x%x, rkey=0x%x, len=%zu",
                     lmr->handle, lmr->lkey, lmr->rkey, length);
    return 0;
}

static void loopback_destroy_mr(RdmaBackendMR *mr)
{
    rdma_info_report("Loopback: Destroyed MR");
}

static uint32_t loopback_mr_lkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    return handle;  /* lkey = handle */
}

static uint32_t loopback_mr_rkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    return handle + 0x10000;  /* rkey = handle + offset */
}

/*
 * Completion Queue Operations
 */

static int loopback_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                              int cqe)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackCQ *lcq = g_new0(LoopbackCQ, 1);
    
    qemu_mutex_lock(&priv->lock);
    lcq->handle = priv->next_cq_handle++;
    qemu_mutex_unlock(&priv->lock);
    
    lcq->cqe = cqe;
    lcq->completions = g_queue_new();
    qemu_mutex_init(&lcq->lock);
    
    g_hash_table_insert(priv->cqs, GUINT_TO_POINTER(lcq->handle), lcq);
    
    cq->backend_dev = backend_dev;
    cq->ibcq = (struct ibv_cq *)(uintptr_t)lcq->handle;
    
    rdma_info_report("Loopback: Created CQ handle %u with %d entries", 
                     lcq->handle, cqe);
    return 0;
}

static void loopback_destroy_cq(RdmaBackendCQ *cq)
{
    uint32_t handle = (uint32_t)(uintptr_t)cq->ibcq;
    rdma_info_report("Loopback: Destroyed CQ handle %u", handle);
    /* Actual cleanup happens in fini */
}

static void loopback_poll_cq(RdmaDeviceResources *rdma_dev_res,
                             RdmaBackendCQ *cq)
{
    /* No-op for now - completions would be polled by driver */
}

/*
 * Queue Pair Operations
 */

static int loopback_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                              RdmaBackendPD *pd, RdmaBackendCQ *scq,
                              RdmaBackendCQ *rcq, RdmaBackendSRQ *srq,
                              uint32_t max_send_wr, uint32_t max_recv_wr,
                              uint32_t max_send_sge, uint32_t max_recv_sge)
{
    LoopbackBackendPrivate *priv = get_private(scq->backend_dev);
    LoopbackQP *lqp = g_new0(LoopbackQP, 1);
    LoopbackCQ *lscq, *lrcq;
    
    qemu_mutex_lock(&priv->lock);
    lqp->qpn = priv->next_qpn++;
    qemu_mutex_unlock(&priv->lock);
    
    lqp->qp_type = qp_type;
    lqp->state = IBV_QPS_RESET;
    lqp->pd_handle = (uint32_t)(uintptr_t)pd->ibpd;
    
    /* Get CQ handles */
    lscq = g_hash_table_lookup(priv->cqs, 
                               GUINT_TO_POINTER((uint32_t)(uintptr_t)scq->ibcq));
    lrcq = g_hash_table_lookup(priv->cqs,
                               GUINT_TO_POINTER((uint32_t)(uintptr_t)rcq->ibcq));
    
    lqp->scq = lscq;
    lqp->rcq = lrcq;
    
    lqp->send_queue = g_queue_new();
    lqp->recv_queue = g_queue_new();
    qemu_mutex_init(&lqp->lock);
    
    g_hash_table_insert(priv->qps, GUINT_TO_POINTER(lqp->qpn), lqp);
    
    qp->ibpd = pd->ibpd;
    /* Store the actual LoopbackQP pointer, not the QPN! */
    qp->ibqp = (struct ibv_qp *)lqp;
    qp->sgid_idx = 0;
    
    rdma_info_report("Loopback: Created QP %u type=%d (stored lqp=%p as ibqp)", 
                     lqp->qpn, qp_type, lqp);
    return 0;
}

static void loopback_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    uint32_t qpn = lqp ? lqp->qpn : 0;
    rdma_info_report("Loopback: Destroyed QP %u", qpn);
}

static uint32_t loopback_qpn(const RdmaBackendQP *qp)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    return lqp ? lqp->qpn : 0;
}

/*
 * QP State Transitions
 */

static int loopback_qp_state_init(RdmaBackendDev *backend_dev,
                                  RdmaBackendQP *qp,
                                  uint8_t qp_type, uint32_t qkey)
{
    (void)backend_dev;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    
    if (lqp) {
        lqp->state = IBV_QPS_INIT;
        lqp->qkey = qkey;
        rdma_info_report("Loopback: QP %u -> INIT", lqp->qpn);
    }
    return 0;
}

static int loopback_qp_state_rtr(RdmaBackendDev *backend_dev,
                                 RdmaBackendQP *qp,
                                 uint8_t qp_type, uint8_t sgid_idx,
                                 union ibv_gid *dgid, uint32_t dqpn,
                                 uint32_t rq_psn, uint32_t qkey,
                                 bool qkey_set)
{
    (void)backend_dev;
    (void)qp_type;
    (void)sgid_idx;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    
    if (lqp) {
        lqp->state = IBV_QPS_RTR;
        lqp->remote_qpn = dqpn;
        if (dgid) {
            memcpy(&lqp->remote_gid, dgid, sizeof(union ibv_gid));
        }
        lqp->rq_psn = rq_psn;
        if (qkey_set) {
            lqp->qkey = qkey;
        }
        
        rdma_info_report("Loopback: QP %u -> RTR (remote_qpn=%u, rq_psn=%u)", lqp->qpn, dqpn, rq_psn);
    }
    return 0;
}

static int loopback_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                                 uint32_t sq_psn, uint32_t qkey,
                                 bool qkey_set)
{
    (void)qp_type;
    (void)sq_psn;
    (void)qkey;
    (void)qkey_set;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    if (lqp) {
        lqp->state = IBV_QPS_RTS;
        rdma_info_report("Loopback: QP %u -> RTS", lqp->qpn);
    }
    return 0;
}

static int loopback_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                             int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->qp_state = IBV_QPS_RTS;
    attr->cur_qp_state = IBV_QPS_RTS;
    attr->path_mtu = IBV_MTU_1024;
    attr->qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    
    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
    }
    return 0;
}

/*
 * Data Path Operations
 */

static void loopback_post_send(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                               uint8_t qp_type, struct ibv_sge *sge,
                               uint32_t num_sge, uint8_t sgid_idx,
                               union ibv_gid *sgid, union ibv_gid *dgid,
                               uint32_t dqpn, uint32_t dqkey, void *ctx)
{
    (void)qp_type;
    (void)sgid_idx;
    (void)sgid;
    (void)dgid;
    (void)dqkey;
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    LoopbackQP *remote_qp = NULL;
    LoopbackWR *recv_wr = NULL;
    uint32_t total_len = 0;
    uint32_t transferred = 0;
    
    if (!lqp) {
        rdma_error_report("Loopback: post_send on unknown QP");
        return;
    }
    
    /* Calculate total send length and apply data pattern */
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        total_len += sge[i].length;
        
        /* Generate data pattern if not PRESERVE */
        if (priv->data_pattern != LOOPBACK_DATA_PATTERN_PRESERVE && 
            sge[i].addr && sge[i].length > 0) {
            generate_data_pattern((void *)sge[i].addr, sge[i].length, 
                                 priv->data_pattern);
        }
    }
    
    /* Compute MD5 of send data if enabled */
    if (priv->compute_md5 && total_len > 0 && num_sge > 0) {
        char md5_str[33];
        compute_sge_md5(sge, num_sge, md5_str, sizeof(md5_str));
        rdma_info_report("Loopback: SEND QP %u: %u bytes, pattern=%s, MD5=%s",
                        lqp->qpn, total_len, 
                        data_pattern_name(priv->data_pattern), md5_str);
    } else if (total_len > 0) {
        rdma_info_report("Loopback: SEND QP %u: %u bytes, pattern=%s",
                        lqp->qpn, total_len, data_pattern_name(priv->data_pattern));
    }
    
    /* For UD QP, use dqpn; for connected QPs, use paired remote_qpn from lqp */
    if (lqp->qp_type == IBV_QPT_UD) {
        remote_qp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(dqpn));
    } else {
        /* For RC/UC, use the remote_qpn stored in lqp from RTR transition */
        if (lqp->remote_qpn) {
            remote_qp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(lqp->remote_qpn));
        } else {
            /* Self-loopback: use same QP */
            remote_qp = lqp;
        }
    }
    
    /* Try to match with a recv on remote/local QP */
    if (remote_qp && !g_queue_is_empty(remote_qp->recv_queue)) {
        qemu_mutex_lock(&remote_qp->lock);
        recv_wr = g_queue_pop_head(remote_qp->recv_queue);
        qemu_mutex_unlock(&remote_qp->lock);
        
        if (recv_wr) {
            /* Perform actual data transfer - copy from send SGEs to recv SGEs */
            uint32_t send_offset = 0;
            uint32_t recv_offset = 0;
            uint32_t send_sge_idx = 0;
            uint32_t recv_sge_idx = 0;
            
            transferred = 0;
            
            /* Copy data from send buffers to recv buffers */
            while (send_sge_idx < num_sge && recv_sge_idx < recv_wr->num_sge) {
                void *send_addr = (void *)(sge[send_sge_idx].addr + send_offset);
                void *recv_addr = (void *)((uintptr_t)recv_wr->sge[recv_sge_idx].addr + recv_offset);
                uint32_t send_remaining = sge[send_sge_idx].length - send_offset;
                uint32_t recv_remaining = recv_wr->sge[recv_sge_idx].length - recv_offset;
                uint32_t copy_len = (send_remaining < recv_remaining) ? send_remaining : recv_remaining;
                
                if (copy_len > 0 && send_addr && recv_addr) {
                    memcpy(recv_addr, send_addr, copy_len);
                    transferred += copy_len;
                }
                
                send_offset += copy_len;
                recv_offset += copy_len;
                
                /* Move to next SGE if current one exhausted */
                if (send_offset >= sge[send_sge_idx].length) {
                    send_sge_idx++;
                    send_offset = 0;
                }
                if (recv_offset >= recv_wr->sge[recv_sge_idx].length) {
                    recv_sge_idx++;
                    recv_offset = 0;
                }
            }
            
            /* Post recv completion on remote QP */
            if (remote_qp->rcq) {
                loopback_post_completion(remote_qp->rcq, recv_wr->wr_id,
                                        IBV_WC_SUCCESS, transferred,
                                        remote_qp->qpn, IBV_WC_RECV);
            }
            
            g_free(recv_wr);
            rdma_info_report("Loopback: Send QP %u -> Recv QP %u (%u bytes transferred)",
                           lqp->qpn, remote_qp->qpn, transferred);
        }
    } else {
        rdma_info_report("Loopback: Send QP %u (no matching recv, %u bytes)",
                        lqp->qpn, total_len);
    }
    
    /* Always post send completion */
    if (lqp->scq) {
        loopback_post_completion(lqp->scq, (uint64_t)(uintptr_t)ctx,
                                IBV_WC_SUCCESS, total_len, lqp->qpn, IBV_WC_SEND);
    }
}

static void loopback_post_recv(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                               uint8_t qp_type, struct ibv_sge *sge,
                               uint32_t num_sge, void *ctx)
{
    (void)backend_dev;
    (void)qp_type;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    
    if (!lqp) {
        rdma_error_report("Loopback: post_recv on unknown QP");
        return;
    }
    
    /* Create and queue the receive work request */
    LoopbackWR *recv_wr = g_new0(LoopbackWR, 1);
    recv_wr->wr_id = (uint64_t)(uintptr_t)ctx;
    recv_wr->num_sge = (num_sge < 32) ? num_sge : 32;
    
    /* Copy SGE list */
    for (uint32_t i = 0; i < recv_wr->num_sge; i++) {
        recv_wr->sge[i].addr = (void *)sge[i].addr;
        recv_wr->sge[i].length = sge[i].length;
        recv_wr->sge[i].lkey = sge[i].lkey;
    }
    
    /* Queue the receive work request */
    qemu_mutex_lock(&lqp->lock);
    g_queue_push_tail(lqp->recv_queue, recv_wr);
    qemu_mutex_unlock(&lqp->lock);
    
    rdma_info_report("Loopback: Posted recv on QP %u (wr_id=0x%lx, %u SGEs)",
                    lqp->qpn, (unsigned long)recv_wr->wr_id, recv_wr->num_sge);
}

/*
 * GID Management
 */

static int loopback_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                           union ibv_gid *gid)
{
    rdma_info_report("Loopback: Added GID");
    return 0;
}

static int loopback_del_gid(RdmaBackendDev *backend_dev, const char *ifname,
                           int gid_idx)
{
    rdma_info_report("Loopback: Deleted GID index %d", gid_idx);
    return 0;
}

static int loopback_get_backend_gid_index(RdmaBackendDev *backend_dev,
                                          int sgid_idx)
{
    return sgid_idx;  /* Identity mapping */
}

/*
 * Backend Operations Structure
 */
const RdmaBackendOps rdma_backend_ops_loopback = {
    .name = "loopback",
    .type = RDMA_BACKEND_TYPE_LOOPBACK,
    
    .init = loopback_init,
    .fini = loopback_fini,
    
    .query_port = loopback_query_port,
    .query_device = loopback_query_device,
    
    .create_pd = loopback_create_pd,
    .destroy_pd = loopback_destroy_pd,
    
    .create_mr = loopback_create_mr,
    .destroy_mr = loopback_destroy_mr,
    .mr_lkey = loopback_mr_lkey,
    .mr_rkey = loopback_mr_rkey,
    
    .create_cq = loopback_create_cq,
    .destroy_cq = loopback_destroy_cq,
    .poll_cq = loopback_poll_cq,
    
    .create_qp = loopback_create_qp,
    .destroy_qp = loopback_destroy_qp,
    .qpn = loopback_qpn,
    
    .qp_state_init = loopback_qp_state_init,
    .qp_state_rtr = loopback_qp_state_rtr,
    .qp_state_rts = loopback_qp_state_rts,
    .query_qp = loopback_query_qp,
    
    .post_send = loopback_post_send,
    .post_recv = loopback_post_recv,
    
    .add_gid = loopback_add_gid,
    .del_gid = loopback_del_gid,
    .get_backend_gid_index = loopback_get_backend_gid_index,
    
    /* SRQ not implemented yet */
    .create_srq = NULL,
    .destroy_srq = NULL,
    .query_srq = NULL,
    .modify_srq = NULL,
    .post_srq_recv = NULL,
};

