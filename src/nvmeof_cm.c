/*
 * nvmeof_cm.c — in-process IB Communication Manager responder
 *
 * Field offsets below are byte offsets into the 256-byte CM MAD, counted
 * from the start of the 24-byte MAD header, and follow IBA volume 1
 * section 12.6.  The kernel writes the same layout from cm_msgs.h; the
 * "offsetNN" names in that header are relative to the end of the MAD
 * header, so CM_REQ_LOCAL_QPN below at 56 is the kernel's offset32.
 *
 * The exchange this serves is the one rdma_cm drives on behalf of
 * nvme-rdma: REQ in, REP out, RTU in, and later DREQ in, DREP out.  The
 * private data carries two stacked headers -- rdma_cm's own cma_hdr with
 * the IP addresses it resolved, then the ULP's nvme_rdma_cm_req -- and
 * both are checked, because getting either wrong shows up in the guest as
 * a connect that hangs rather than one that fails.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvmeof_cm.h"
#include "nvmeof_target.h"

/* ------------------------------------------------------------------ */
/* MAD header                                                         */
/* ------------------------------------------------------------------ */

enum {
    MAD_BASE_VERSION_OFF = 0,
    MAD_MGMT_CLASS_OFF = 1,
    MAD_CLASS_VERSION_OFF = 2,
    MAD_METHOD_OFF = 3,
    MAD_STATUS_OFF = 4,
    MAD_CLASS_SPECIFIC_OFF = 6,
    MAD_TID_OFF = 8,
    MAD_ATTR_ID_OFF = 16,
    MAD_ATTR_MOD_OFF = 20,
    MAD_HDR_SIZE = 24,
};

#define IB_MGMT_BASE_VERSION 1
#define IB_MGMT_CLASS_CM     0x07
#define IB_CM_CLASS_VERSION  2
#define IB_MGMT_METHOD_SEND  0x03

enum {
    CM_ATTR_REQ = 0x0010,
    CM_ATTR_MRA = 0x0011,
    CM_ATTR_REJ = 0x0012,
    CM_ATTR_REP = 0x0013,
    CM_ATTR_RTU = 0x0014,
    CM_ATTR_DREQ = 0x0015,
    CM_ATTR_DREP = 0x0016,
};

enum {
    /* REQ (cm_req_msg) */
    CM_REQ_LOCAL_COMM_ID = 24,
    CM_REQ_SERVICE_ID = 32,
    CM_REQ_LOCAL_CA_GUID = 40,
    CM_REQ_LOCAL_QKEY = 52,
    CM_REQ_LOCAL_QPN = 56,    /* qpn(31:8) | responder_resources(7:0) */
    CM_REQ_LOCAL_EECN = 60,   /* eecn(31:8)  | initiator_depth(7:0) */
    CM_REQ_REMOTE_EECN = 64,  /* eecn(31:8)  | resp_timeout | tst | flow */
    CM_REQ_STARTING_PSN = 68, /* psn(31:8)   | resp_timeout | retry */
    CM_REQ_PKEY = 72,
    CM_REQ_PATH_MTU = 74,    /* mtu(7:4) | rdc_exists(3) | rnr_retry(2:0) */
    CM_REQ_MAX_RETRIES = 75, /* max_cm_retries(7:4) | srq(3) */
    CM_REQ_PRIMARY_LGID = 80,
    CM_REQ_PRIMARY_RGID = 96,
    CM_REQ_PRIVATE_DATA = 164,
    CM_REQ_PRIVATE_SIZE = 92,

    /* REP (cm_rep_msg) */
    CM_REP_LOCAL_COMM_ID = 24,
    CM_REP_REMOTE_COMM_ID = 28,
    CM_REP_LOCAL_QKEY = 32,
    CM_REP_LOCAL_QPN = 36, /* qpn(31:8) */
    CM_REP_LOCAL_EECN = 40,
    CM_REP_STARTING_PSN = 44, /* psn(31:8) */
    CM_REP_RESP_RESOURCES = 48,
    CM_REP_INITIATOR_DEPTH = 49,
    CM_REP_TARGET_ACK = 50, /* delay(7:3) | failover(2:1) | flow(0) */
    CM_REP_RNR_RETRY = 51,  /* rnr_retry_count(7:5) | srq(4) */
    CM_REP_LOCAL_CA_GUID = 52,
    CM_REP_PRIVATE_DATA = 60,
    CM_REP_PRIVATE_SIZE = 196,

    /* RTU / DREP (cm_rtu_msg, cm_drep_msg): identical first eight bytes */
    CM_RTU_LOCAL_COMM_ID = 24,
    CM_RTU_REMOTE_COMM_ID = 28,
    CM_DREP_LOCAL_COMM_ID = CM_RTU_LOCAL_COMM_ID,
    CM_DREP_REMOTE_COMM_ID = CM_RTU_REMOTE_COMM_ID,

    /* DREQ (cm_dreq_msg) */
    CM_DREQ_LOCAL_COMM_ID = 24,
    CM_DREQ_REMOTE_COMM_ID = 28,
    CM_DREQ_REMOTE_QPN = 32, /* qpn(31:8) */

    /* REJ (cm_rej_msg) */
    CM_REJ_LOCAL_COMM_ID = 24,
    CM_REJ_REMOTE_COMM_ID = 28,
    CM_REJ_MSG_REJECTED = 32, /* message_rejected(7:6) */
    CM_REJ_INFO_LENGTH = 33,  /* reject_info_length(7:1) */
    CM_REJ_REASON = 34,
    CM_REJ_ARI = 36,
    CM_REJ_PRIVATE_DATA = 108,
};

enum {
    IB_CM_REJ_NO_QP = 1,
    IB_CM_REJ_NO_RESOURCES = 3,
    IB_CM_REJ_UNSUPPORTED = 5,
    IB_CM_REJ_INVALID_COMM_ID = 6,
    IB_CM_REJ_INVALID_SERVICE_ID = 8,
    IB_CM_REJ_INVALID_TRANSPORT_TYPE = 9,
    IB_CM_REJ_CONSUMER_DEFINED = 28,
};

enum {
    /* rdma_cm's private-data header (struct cma_hdr), ahead of the ULP's. */
    CMA_HDR_VERSION_OFF = 0,
    CMA_HDR_IP_VERSION_OFF = 1,
    CMA_HDR_PORT_OFF = 2,
    CMA_HDR_SRC_ADDR_OFF = 4,
    CMA_HDR_DST_ADDR_OFF = 20,
    CMA_HDR_SIZE = 36,

    /* struct nvme_rdma_cm_req / _cm_rep / _cm_rej */
    NVME_RDMA_CM_REQ_RECFMT_OFF = 0,
    NVME_RDMA_CM_REQ_QID_OFF = 2,
    NVME_RDMA_CM_REQ_HRQSIZE_OFF = 4,
    NVME_RDMA_CM_REQ_HSQSIZE_OFF = 6,
    NVME_RDMA_CM_REQ_SIZE = 32,

    NVME_RDMA_CM_REP_RECFMT_OFF = 0,
    NVME_RDMA_CM_REP_CRQSIZE_OFF = 2,
    NVME_RDMA_CM_REP_SIZE = 32,
};

/*
 * Both ULP payloads sit behind the cma_hdr in the CM private data, so the
 * private data has to be big enough for either. These are all constants, so
 * the check belongs to the build rather than to the REQ path.
 */
_Static_assert(CMA_HDR_SIZE + NVME_RDMA_CM_REQ_SIZE <= CM_REQ_PRIVATE_SIZE,
               "CM REQ private data cannot hold cma_hdr + nvme_rdma_cm_req");
_Static_assert(CMA_HDR_SIZE + NVME_RDMA_CM_REP_SIZE <= CM_REQ_PRIVATE_SIZE,
               "CM REQ private data cannot hold cma_hdr + nvme_rdma_cm_rep");

enum {
    NVME_RDMA_CM_FMT_1_0 = 0x0,
    NVME_RDMA_CM_INVALID_LEN = 0x01,
    NVME_RDMA_CM_INVALID_RECFMT = 0x02,
    NVME_RDMA_CM_INVALID_QID = 0x03,
    NVME_RDMA_CM_INVALID_HSQSIZE = 0x04,
    NVME_RDMA_CM_INVALID_HRQSIZE = 0x05,
    NVME_RDMA_CM_NO_RSC = 0x06,
};

/* rdma_cm builds service IDs as (RDMA_PS_TCP << 16) | port. */
#define RDMA_PS_TCP 0x0106

/* ------------------------------------------------------------------ */
/* Byte helpers                                                       */
/* ------------------------------------------------------------------ */

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_be64(const uint8_t *p)
{
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------------------ */
/* Connection table                                                   */
/* ------------------------------------------------------------------ */

#define NVMEOF_CM_MAX_CONN 66

struct nvmeof_cm_conn {
    bool in_use;
    bool established;
    uint32_t remote_comm_id;
    uint32_t local_comm_id;
    uint32_t guest_qpn;
    uint32_t local_qpn;
    uint32_t start_psn;
    uint16_t nvme_qid;
};

struct nvmeof_cm {
    struct nvmeof_target *target;
    uint32_t traddr;
    uint16_t trsvcid;
    uint32_t next_comm_id;
    uint32_t next_qpn;
    struct nvmeof_cm_conn conn[NVMEOF_CM_MAX_CONN];
};

struct nvmeof_cm *nvmeof_cm_create(struct nvmeof_target *target,
                                   uint32_t traddr, uint16_t trsvcid)
{
    struct nvmeof_cm *cm = calloc(1, sizeof(*cm));
    if (cm == NULL) {
        return NULL;
    }
    cm->target = target;
    cm->traddr = traddr;
    cm->trsvcid = trsvcid;
    cm->next_comm_id = 0x51000001u;
    cm->next_qpn = NVMEOF_CM_QPN_BASE + 1;
    return cm;
}

void nvmeof_cm_destroy(struct nvmeof_cm *cm)
{
    free(cm);
}

unsigned nvmeof_cm_conn_count(const struct nvmeof_cm *cm)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NVMEOF_CM_MAX_CONN; i++) {
        n += cm->conn[i].in_use ? 1 : 0;
    }
    return n;
}

static struct nvmeof_cm_conn *conn_by_local_id(struct nvmeof_cm *cm,
                                               uint32_t local_comm_id)
{
    for (unsigned i = 0; i < NVMEOF_CM_MAX_CONN; i++) {
        if (cm->conn[i].in_use && cm->conn[i].local_comm_id == local_comm_id) {
            return &cm->conn[i];
        }
    }
    return NULL;
}

static struct nvmeof_cm_conn *conn_by_guest_qpn(struct nvmeof_cm *cm,
                                                uint32_t guest_qpn)
{
    for (unsigned i = 0; i < NVMEOF_CM_MAX_CONN; i++) {
        if (cm->conn[i].in_use && cm->conn[i].guest_qpn == guest_qpn) {
            return &cm->conn[i];
        }
    }
    return NULL;
}

static struct nvmeof_cm_conn *conn_alloc(struct nvmeof_cm *cm)
{
    for (unsigned i = 0; i < NVMEOF_CM_MAX_CONN; i++) {
        if (!cm->conn[i].in_use) {
            memset(&cm->conn[i], 0, sizeof(cm->conn[i]));
            cm->conn[i].in_use = true;
            return &cm->conn[i];
        }
    }
    return NULL;
}

static void conn_free(struct nvmeof_cm *cm, struct nvmeof_cm_conn *c)
{
    if (cm->target != NULL) {
        nvmeof_target_close_queue(cm->target, c->guest_qpn);
    }
    memset(c, 0, sizeof(*c));
}

void nvmeof_cm_drop_qp(struct nvmeof_cm *cm, uint32_t guest_qpn)
{
    struct nvmeof_cm_conn *c = conn_by_guest_qpn(cm, guest_qpn);
    if (c != NULL) {
        conn_free(cm, c);
    }
}

/* ------------------------------------------------------------------ */
/* MAD construction                                                   */
/* ------------------------------------------------------------------ */

static void mad_reply_hdr(uint8_t *rsp, const uint8_t *req, uint16_t attr_id)
{
    memset(rsp, 0, IB_MAD_SIZE);
    rsp[MAD_BASE_VERSION_OFF] = IB_MGMT_BASE_VERSION;
    rsp[MAD_MGMT_CLASS_OFF] = IB_MGMT_CLASS_CM;
    rsp[MAD_CLASS_VERSION_OFF] = IB_CM_CLASS_VERSION;
    rsp[MAD_METHOD_OFF] = IB_MGMT_METHOD_SEND;
    /* The transaction ID ties the reply to the request that caused it. */
    memcpy(rsp + MAD_TID_OFF, req + MAD_TID_OFF, 8);
    put_be16(rsp + MAD_ATTR_ID_OFF, attr_id);
}

static size_t build_rej(uint8_t *rsp, const uint8_t *req, uint8_t msg_rejected,
                        uint16_t reason, uint16_t nvme_sts)
{
    mad_reply_hdr(rsp, req, CM_ATTR_REJ);
    /* A REJ for a REQ has no local comm ID of its own to offer yet. */
    put_be32(rsp + CM_REJ_LOCAL_COMM_ID, 0);
    put_be32(rsp + CM_REJ_REMOTE_COMM_ID, get_be32(req + CM_REQ_LOCAL_COMM_ID));
    rsp[CM_REJ_MSG_REJECTED] = (uint8_t)(msg_rejected << 6);
    rsp[CM_REJ_INFO_LENGTH] = 0;
    put_be16(rsp + CM_REJ_REASON, reason);

    if (reason == IB_CM_REJ_CONSUMER_DEFINED) {
        /* nvme-rdma decodes this as struct nvme_rdma_cm_rej. */
        put_le16(rsp + CM_REJ_PRIVATE_DATA, NVME_RDMA_CM_FMT_1_0);
        put_le16(rsp + CM_REJ_PRIVATE_DATA + 2, nvme_sts);
    }
    return IB_MAD_SIZE;
}

/* ------------------------------------------------------------------ */
/* REQ                                                                */
/* ------------------------------------------------------------------ */

static size_t handle_req(struct nvmeof_cm *cm, const uint8_t *req, uint8_t *rsp,
                         struct nvmeof_cm_result *res)
{
    uint64_t service_id = get_be64(req + CM_REQ_SERVICE_ID);
    uint32_t remote_comm_id = get_be32(req + CM_REQ_LOCAL_COMM_ID);
    uint32_t guest_qpn = get_be32(req + CM_REQ_LOCAL_QPN) >> 8;
    uint8_t responder_resources = req[CM_REQ_LOCAL_QPN + 3];
    uint8_t initiator_depth = req[CM_REQ_LOCAL_EECN + 3];
    uint8_t transport_service = (req[CM_REQ_REMOTE_EECN + 3] >> 1) & 0x3;
    const uint8_t *priv = req + CM_REQ_PRIVATE_DATA;

    /* Only reliable connected service reaches an NVMe-oF controller. */
    if (transport_service != 0) {
        return build_rej(rsp, req, 0, IB_CM_REJ_INVALID_TRANSPORT_TYPE, 0);
    }

    if ((service_id >> 16) != RDMA_PS_TCP ||
        (service_id & 0xffff) != cm->trsvcid) {
        return build_rej(rsp, req, 0, IB_CM_REJ_INVALID_SERVICE_ID, 0);
    }

    /*
     * rdma_cm puts the addresses it resolved in front of the ULP's own
     * private data. Checking the destination catches a connect aimed at
     * an address this target does not answer for, which otherwise looks
     * to the guest like a silent hang.
     */
    if (priv[CMA_HDR_VERSION_OFF] != 0) {
        return build_rej(rsp, req, 0, IB_CM_REJ_UNSUPPORTED, 0);
    }
    uint8_t ip_version = priv[CMA_HDR_IP_VERSION_OFF] >> 4;
    if (ip_version == 4) {
        uint32_t dst = get_be32(priv + CMA_HDR_DST_ADDR_OFF + 12);
        if (dst != cm->traddr) {
            return build_rej(rsp, req, 0, IB_CM_REJ_INVALID_SERVICE_ID, 0);
        }
    }

    const uint8_t *nvme = priv + CMA_HDR_SIZE;
    uint16_t recfmt = get_le16(nvme + NVME_RDMA_CM_REQ_RECFMT_OFF);
    uint16_t qid = get_le16(nvme + NVME_RDMA_CM_REQ_QID_OFF);
    uint16_t hrqsize = get_le16(nvme + NVME_RDMA_CM_REQ_HRQSIZE_OFF);
    uint16_t hsqsize = get_le16(nvme + NVME_RDMA_CM_REQ_HSQSIZE_OFF);

    if (recfmt != NVME_RDMA_CM_FMT_1_0) {
        return build_rej(rsp, req, 0, IB_CM_REJ_CONSUMER_DEFINED,
                         NVME_RDMA_CM_INVALID_RECFMT);
    }
    if (hsqsize == 0) {
        return build_rej(rsp, req, 0, IB_CM_REJ_CONSUMER_DEFINED,
                         NVME_RDMA_CM_INVALID_HSQSIZE);
    }
    if (hrqsize == 0) {
        return build_rej(rsp, req, 0, IB_CM_REJ_CONSUMER_DEFINED,
                         NVME_RDMA_CM_INVALID_HRQSIZE);
    }

    const struct nvmeof_target_cfg *cfg =
        cm->target != NULL ? nvmeof_target_config(cm->target) : NULL;
    if (cfg != NULL && qid > cfg->max_queues) {
        return build_rej(rsp, req, 0, IB_CM_REJ_CONSUMER_DEFINED,
                         NVME_RDMA_CM_INVALID_QID);
    }

    /*
     * A REQ for a QP we already track is a retransmission or a stale
     * connection; drop the old state and honour the new one rather than
     * leaving the guest to time out.
     */
    struct nvmeof_cm_conn *old = conn_by_guest_qpn(cm, guest_qpn);
    if (old != NULL) {
        conn_free(cm, old);
    }

    struct nvmeof_cm_conn *c = conn_alloc(cm);
    if (c == NULL) {
        return build_rej(rsp, req, 0, IB_CM_REJ_NO_RESOURCES, 0);
    }

    if (cm->target != NULL &&
        nvmeof_target_open_queue(cm->target, guest_qpn) == NULL) {
        conn_free(cm, c);
        return build_rej(rsp, req, 0, IB_CM_REJ_CONSUMER_DEFINED,
                         NVME_RDMA_CM_NO_RSC);
    }

    c->remote_comm_id = remote_comm_id;
    c->local_comm_id = cm->next_comm_id++;
    c->guest_qpn = guest_qpn;
    c->local_qpn = cm->next_qpn++;
    if (!nvmeof_cm_is_target_qpn(c->local_qpn)) {
        cm->next_qpn = NVMEOF_CM_QPN_BASE + 1;
        c->local_qpn = cm->next_qpn++;
    }
    c->start_psn = (c->local_comm_id ^ 0x5a5a5au) & 0xffffffu;
    c->nvme_qid = qid;

    /* Build the REP. */
    mad_reply_hdr(rsp, req, CM_ATTR_REP);
    put_be32(rsp + CM_REP_LOCAL_COMM_ID, c->local_comm_id);
    put_be32(rsp + CM_REP_REMOTE_COMM_ID, c->remote_comm_id);
    put_be32(rsp + CM_REP_LOCAL_QKEY, 0);
    put_be32(rsp + CM_REP_LOCAL_QPN, c->local_qpn << 8);
    put_be32(rsp + CM_REP_STARTING_PSN, c->start_psn << 8);
    rsp[CM_REP_RESP_RESOURCES] = initiator_depth;
    rsp[CM_REP_INITIATOR_DEPTH] = responder_resources;
    rsp[CM_REP_TARGET_ACK] = (uint8_t)(14u << 3); /* target ack delay */
    rsp[CM_REP_RNR_RETRY] = (uint8_t)(7u << 5);   /* infinite RNR retry */
    put_be64(rsp + CM_REP_LOCAL_CA_GUID,
             get_be64(req + CM_REQ_LOCAL_CA_GUID) ^ 0x1dd800000000ull);

    uint8_t *rep_priv = rsp + CM_REP_PRIVATE_DATA;
    put_le16(rep_priv + NVME_RDMA_CM_REP_RECFMT_OFF, NVME_RDMA_CM_FMT_1_0);
    put_le16(rep_priv + NVME_RDMA_CM_REP_CRQSIZE_OFF, hrqsize);

    res->action = NVMEOF_CM_ACT_BIND;
    res->guest_qpn = c->guest_qpn;
    res->local_qpn = c->local_qpn;
    res->start_psn = c->start_psn;
    res->nvme_qid = c->nvme_qid;
    return IB_MAD_SIZE;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

bool nvmeof_cm_handle_mad(struct nvmeof_cm *cm, const void *mad, size_t len,
                          void *rsp_buf, struct nvmeof_cm_result *res)
{
    const uint8_t *m = mad;
    uint8_t *rsp = rsp_buf;

    memset(res, 0, sizeof(*res));

    if (cm == NULL || m == NULL || rsp == NULL || len < IB_MAD_SIZE) {
        return false;
    }
    if (m[MAD_MGMT_CLASS_OFF] != IB_MGMT_CLASS_CM ||
        m[MAD_BASE_VERSION_OFF] != IB_MGMT_BASE_VERSION ||
        m[MAD_METHOD_OFF] != IB_MGMT_METHOD_SEND) {
        return false;
    }

    uint16_t attr = get_be16(m + MAD_ATTR_ID_OFF);
    struct nvmeof_cm_conn *c;

    switch (attr) {
    case CM_ATTR_REQ:
        res->rsp_len = handle_req(cm, m, rsp, res);
        return true;

    case CM_ATTR_RTU:
        /* The initiator's remote_comm_id is the ID we handed it in the REP. */
        c = conn_by_local_id(cm, get_be32(m + CM_RTU_REMOTE_COMM_ID));
        if (c == NULL) {
            return true; /* nothing to reply to a stale RTU */
        }
        c->established = true;
        res->action = NVMEOF_CM_ACT_ESTABLISHED;
        res->guest_qpn = c->guest_qpn;
        res->local_qpn = c->local_qpn;
        res->nvme_qid = c->nvme_qid;
        return true;

    case CM_ATTR_DREQ:
        c = conn_by_local_id(cm, get_be32(m + CM_DREQ_REMOTE_COMM_ID));
        if (c == NULL) {
            c = conn_by_guest_qpn(cm, get_be32(m + CM_DREQ_REMOTE_QPN) >> 8);
        }
        mad_reply_hdr(rsp, m, CM_ATTR_DREP);
        put_be32(rsp + CM_DREP_LOCAL_COMM_ID, c != NULL ? c->local_comm_id : 0);
        put_be32(rsp + CM_DREP_REMOTE_COMM_ID,
                 get_be32(m + CM_DREQ_LOCAL_COMM_ID));
        res->rsp_len = IB_MAD_SIZE;
        if (c != NULL) {
            res->action = NVMEOF_CM_ACT_UNBIND;
            res->guest_qpn = c->guest_qpn;
            res->local_qpn = c->local_qpn;
            res->nvme_qid = c->nvme_qid;
            conn_free(cm, c);
        }
        return true;

    case CM_ATTR_DREP:
        /* We never send a DREQ, so a DREP is only ever an echo. */
        return true;

    case CM_ATTR_REJ:
        c = conn_by_local_id(cm, get_be32(m + CM_REJ_REMOTE_COMM_ID));
        if (c != NULL) {
            res->action = NVMEOF_CM_ACT_UNBIND;
            res->guest_qpn = c->guest_qpn;
            res->local_qpn = c->local_qpn;
            res->nvme_qid = c->nvme_qid;
            conn_free(cm, c);
        }
        return true;

    case CM_ATTR_MRA:
        /* Acknowledges that our REP arrived; no state change and no reply. */
        return true;

    case CM_ATTR_REP:
        /* Only an active side receives these, and we are never that. */
        res->rsp_len = build_rej(rsp, m, 1, IB_CM_REJ_UNSUPPORTED, 0);
        return true;

    default:
        return false;
    }
}
