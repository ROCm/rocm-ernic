/*
 * Compatibility Bridge Implementation
 *
 * This file implements the wrapper functions that isolate QEMU code from
 * our libvfio-user server: rocm_ernic_compat.h, ernic_rm.h and ernic_mesh.h.
 * Only this file includes QEMU headers.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <infiniband/verbs.h>
#include "ernic_limits.h"
#include "ernic_mesh.h"
#include "ernic_rm.h"
#include "rocm_ernic_compat.h"
/*
 * struct rocm_ernic_dev.  IWYU credits uses of it to the repeated
 * rocm_ernic_dev_t typedef in rocm_ernic_compat.h, so it would drop this.
 */
#include "rocm_ernic_internal.h" // IWYU pragma: keep

/* QEMU-derived headers, from src/qemu/ */
#include "pvrdma.h"
#include "rdma_backend.h"
#include "rdma_backend_defs.h"
#include "rdma/rdma_backend_ops.h"
#include "rdma_rm.h"
#include "rdma_rm_defs.h"
#include "rdma_utils.h"
#include "vmw_pvrdma-abi.h"

/*
 * Device Management (rocm_ernic_compat.h)
 */

pvrdma_handle_t pvrdma_device_create(const rocm_ernic_dev_t *dev,
                                     const char *backend_type_str)
{
    PVRDMADev *pvrdma;
    RdmaBackendType backend_type;

    if (!dev) {
        rdma_error_report("pvrdma_device_create: dev is NULL");
        return NULL;
    }

    /* Parse backend type and extract config */
    backend_type = rdma_backend_get_type_from_string(backend_type_str);
    if (backend_type == RDMA_BACKEND_TYPE_MAX) {
        rdma_error_report("Unknown backend '%s'", backend_type_str);
        return NULL;
    }

    /* Allocate QEMU PVRDMA device structure */
    pvrdma = calloc(1, sizeof(PVRDMADev));
    if (!pvrdma) {
        rdma_error_report("Failed to allocate PVRDMADev");
        return NULL;
    }

    pvrdma->parent_obj.vfu_ctx = dev->vfu_ctx;

    rdma_info_report("Selected RDMA backend: %s",
                     rdma_backend_type_to_string(backend_type));

    /* Store backend type and config in device for later use */
    pvrdma->backend_dev.backend_type = backend_type;

    /* Extract backend config (part after ':') from backend string, which
     * may be NULL, meaning "none" */
    const char *colon = backend_type_str ? strchr(backend_type_str, ':') : NULL;
    const char *name = colon ? (colon + 1) : NULL;

    if (name) {
        rdma_info_report("Backend config string: '%s'", name);
        pvrdma->backend_device_name = strdup(name);
        if (!pvrdma->backend_device_name) {
            rdma_error_report("Failed to allocate the backend config");
            free(pvrdma);
            return NULL;
        }
    }

    /* Defaults, until pvrdma_device_realize() replaces them all with the
     * backend's query_device() */
    pvrdma->dev_attr.max_qp = ERNIC_MAX_QP;
    pvrdma->dev_attr.max_cq = ERNIC_MAX_CQ;
    pvrdma->dev_attr.max_mr = ERNIC_MAX_MR;
    pvrdma->dev_attr.max_pd = MAX_PD;
    pvrdma->dev_attr.max_qp_rd_atom = MAX_QP_RD_ATOM;
    pvrdma->dev_attr.max_qp_init_rd_atom = MAX_QP_INIT_RD_ATOM;
    pvrdma->dev_attr.max_ah = ERNIC_MAX_AH;
    pvrdma->dev_attr.max_srq = ERNIC_MAX_SRQ;
    pvrdma->dev_attr.max_mr_size = MAX_MR_SIZE;
    pvrdma->dev_attr.max_sge =
        ERNIC_MAX_SGE; /* Required for calculations below */

    /* Calculate dynamic device capabilities (from init_dev_caps in
     * upstream QEMU's hw/rdma/vmw/pvrdma_main.c) */
    {
        size_t pg_tbl_bytes =
            PVRDMA_PG_TBL_PAGES * PAGE_SIZE * (PAGE_SIZE / sizeof(uint64_t));
        size_t wr_sz = MAX(sizeof(struct pvrdma_sq_wqe_hdr),
                           sizeof(struct pvrdma_rq_wqe_hdr));

        rdma_info_report("Calculating device capabilities:");
        rdma_info_report("  PAGE_SIZE=%zu, pg_tbl_bytes=%zu", (size_t)PAGE_SIZE,
                         pg_tbl_bytes);

        /* Calculate max_qp_wr */
        pvrdma->dev_attr.max_qp_wr =
            (int)(pg_tbl_bytes /
                      (wr_sz + sizeof(struct pvrdma_sge) *
                                   (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        /* Calculate max_cqe */
        pvrdma->dev_attr.max_cqe =
            (int)(pg_tbl_bytes / sizeof(struct pvrdma_cqe) - PAGE_SIZE);

        /* Calculate max_srq_wr */
        pvrdma->dev_attr.max_srq_wr =
            (int)(pg_tbl_bytes / ((sizeof(struct pvrdma_rq_wqe_hdr) +
                                   sizeof(struct pvrdma_sge)) *
                                  (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        rdma_info_report("  max_qp_wr=%d", pvrdma->dev_attr.max_qp_wr);
        rdma_info_report("  max_cqe=%d", pvrdma->dev_attr.max_cqe);
        rdma_info_report("  max_sge=%d", pvrdma->dev_attr.max_sge);
        rdma_info_report("  max_srq_wr=%d", pvrdma->dev_attr.max_srq_wr);
    }

    rdma_info_report("PVRDMA device created (handle=%p)", (void *)pvrdma);

    return pvrdma;
}

const char *pvrdma_backend_name(const char *backend_type_str)
{
    RdmaBackendType type = rdma_backend_get_type_from_string(backend_type_str);

    return type == RDMA_BACKEND_TYPE_MAX ? NULL
                                         : rdma_backend_type_to_string(type);
}

void pvrdma_device_destroy(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    /* Clean up resource manager */
    rdma_rm_fini(&pvrdma->rdma_dev_res);

    /* Clean up backend using the new abstraction */
    rdma_backend_fini_with_ops(&pvrdma->backend_dev);

    /* Free device names */
    free(pvrdma->backend_device_name);

    free(pvrdma);

    rdma_info_report("PVRDMA device destroyed");
}

int pvrdma_device_realize(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;
    int rc;

    if (!pvrdma) {
        return -EINVAL;
    }

    /* Initialize RDMA backend with selected backend type */
    const char *backend_config =
        pvrdma
            ->backend_device_name; /* Config extracted from --backend string */

    rc = rdma_backend_init_with_ops(
        &pvrdma->backend_dev, pvrdma->backend_dev.backend_type, backend_config);

    if (rc < 0) {
        rdma_error_report("RDMA backend initialization failed (rc=%d)", rc);
        rdma_error_report(
            "Backend type: %s",
            rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));
        return -EIO;
    }

    /* Query device capabilities from backend to populate dev_attr */
    if (pvrdma->backend_dev.backend_ops &&
        pvrdma->backend_dev.backend_ops->query_device) {
        rc = pvrdma->backend_dev.backend_ops->query_device(&pvrdma->backend_dev,
                                                           &pvrdma->dev_attr);
        if (rc < 0) {
            rdma_error_report("Backend query_device failed (rc=%d)", rc);
            return -EIO;
        }
        rdma_info_report("Backend device attributes queried: max_qp=%d, "
                         "max_cq=%d, max_pd=%d",
                         pvrdma->dev_attr.max_qp, pvrdma->dev_attr.max_cq,
                         pvrdma->dev_attr.max_pd);
    } else {
        rdma_error_report("Backend does not support query_device");
        return -ENOTSUP;
    }

    rdma_info_report(
        "RDMA backend '%s' initialized successfully",
        rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));

    /* Initialize resource manager AFTER querying device capabilities */
    rdma_info_report("pvrdma_device_realize: About to call rdma_rm_init, "
                     "pvrdma=%p, &pvrdma->rdma_dev_res=%p",
                     (void *)pvrdma, (void *)&pvrdma->rdma_dev_res);
    if (rdma_rm_init(&pvrdma->rdma_dev_res, &pvrdma->dev_attr) < 0) {
        rdma_error_report("Failed to initialize resource manager");
        return -EIO;
    }
    rdma_info_report("Resource manager initialized with dev_attr tables");

    rdma_info_report("PVRDMA device realized successfully");

    return 0;
}

/* -------------------------------------------------------------------------
 * The resource manager, for the admin queue (ernic_rm.h)
 * -------------------------------------------------------------------------
 */

int ernic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe, uint32_t *cq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_cq(&pvrdma->rdma_dev_res, &pvrdma->backend_dev, cqe,
                            cq_handle);
}

void ernic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_cq(&pvrdma->rdma_dev_res, cq_handle);
}

int ernic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_srq(&pvrdma->rdma_dev_res, pd_handle, max_wr, max_sge,
                             srq_limit, srq_handle);
}

void ernic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_srq(&pvrdma->rdma_dev_res, srq_handle);
}

int ernic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_srq_attr attr = {.srq_limit = srq_limit};

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    /*
     * A limit of 0 disables the low-watermark and is a legal value that the
     * guest driver accepts, but rdma_rm_modify_srq() rejects it outright
     * (rdma_rm.c: IBV_SRQ_LIMIT with srq_limit == 0 is -EINVAL). There is
     * nothing for the backend to hold for "no limit", and the authoritative
     * copy lives in the adminq's SRQ map, so succeed here rather than
     * turning a valid ibv_modify_srq() into an error.
     */
    if (srq_limit == 0)
        return 0;
    /* IBV_SRQ_LIMIT only: the depth is fixed at create time, and the guest
     * driver's modify path carries nothing else (ionic_admin_modify_srq is
     * qid + low_wqes_limit). */
    return rdma_rm_modify_srq(&pvrdma->rdma_dev_res, srq_handle, &attr,
                              IBV_SRQ_LIMIT);
}

int ernic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge, uint32_t *srq_limit)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_srq_attr attr = {0};
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    ret = rdma_rm_query_srq(&pvrdma->rdma_dev_res, srq_handle, &attr);
    if (ret)
        return ret;

    if (max_wr)
        *max_wr = attr.max_wr;
    if (max_sge)
        *max_sge = attr.max_sge;
    if (srq_limit)
        *srq_limit = attr.srq_limit;
    return 0;
}

int ernic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_pd(&pvrdma->rdma_dev_res, &pvrdma->backend_dev,
                            pd_handle);
}

void ernic_rm_dealloc_pd(pvrdma_handle_t handle, uint32_t pd_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_pd(&pvrdma->rdma_dev_res, pd_handle);
}

int ernic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    RdmaRmQP *out_qp = NULL;
    return rdma_rm_alloc_qp(&pvrdma->rdma_dev_res, pd_handle, qp_type,
                            max_send_wr, 16, send_cq_handle, max_recv_wr, 16,
                            recv_cq_handle, qpn, 0, 0, &out_qp);
}

void ernic_rm_dealloc_qp(pvrdma_handle_t handle, uint32_t qpn)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_qp(&pvrdma->rdma_dev_res, qpn);
}

int ernic_rm_alloc_mr(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint32_t access_flags, uint32_t *mr_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    uint32_t lkey = 0, rkey = 0;
    return rdma_rm_alloc_mr(&pvrdma->rdma_dev_res, pd_handle, (int)access_flags,
                            mr_handle, &lkey, &rkey);
}

void ernic_rm_dealloc_mr(pvrdma_handle_t handle, uint32_t mr_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_mr(&pvrdma->rdma_dev_res, mr_handle);
}

int ernic_rm_modify_qp(pvrdma_handle_t handle, uint32_t qpn, uint32_t attr_mask,
                       uint8_t type_state, uint32_t sq_psn, uint32_t rq_psn,
                       uint32_t qkey_dest_qpn, const uint8_t *dest_gid_16bytes)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    /* Decode ionic type_state: bits[3:0]=to_state, bits[7:4]=from_state.
     * Map ionic QP state values to ibv_qp_state.  ionic uses the same
     * numeric encoding as IB (RESET=0, INIT=1, RTR=2, RTS=3, SQD=4,
     * SQE=5, ERROR=6) so a direct cast is safe for the common path. */
    enum ibv_qp_state to_state = (enum ibv_qp_state)(type_state & 0x0fu);

    /* Build a minimal dgid from the raw bytes provided by the caller. */
    union ibv_gid dgid;
    if (dest_gid_16bytes)
        memcpy(&dgid, dest_gid_16bytes, sizeof(dgid));
    else
        memset(&dgid, 0, sizeof(dgid));

    /* dest QPN sits in the lower 24 bits of qkey_dest_qpn when sending UD. */
    uint32_t dqpn = qkey_dest_qpn & 0x00ffffffu;
    uint32_t qkey = qkey_dest_qpn >> 24;

    return rdma_rm_modify_qp(&pvrdma->rdma_dev_res, &pvrdma->backend_dev, qpn,
                             attr_mask, 0, /* sgid_idx: use default GID */
                             &dgid, dqpn, to_state, qkey, rq_psn, sq_psn);
}

int ernic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    ret = rdma_rm_query_qp(&pvrdma->rdma_dev_res, qpn, &attr,
                           IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                               IBV_QP_ACCESS_FLAGS | IBV_QP_RQ_PSN |
                               IBV_QP_SQ_PSN,
                           &init_attr);
    if (ret)
        return ret;

    if (state)
        *state = (uint8_t)attr.qp_state;
    if (path_mtu)
        *path_mtu = (uint8_t)attr.path_mtu;
    if (dest_qpn)
        *dest_qpn = attr.dest_qp_num;
    if (access_flags)
        *access_flags = (uint32_t)attr.qp_access_flags;
    if (rq_psn)
        *rq_psn = attr.rq_psn;
    if (sq_psn)
        *sq_psn = attr.sq_psn;
    return 0;
}

/* -------------------------------------------------------------------------
 * The TCP mesh, for the data path (ernic_mesh.h)
 * -------------------------------------------------------------------------
 */

uint32_t ernic_mesh_local_node(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return UINT32_MAX;
    return tcp_backend_local_node_id(&pvrdma->backend_dev);
}

uint32_t ernic_mesh_node_from_gid(pvrdma_handle_t handle,
                                  const uint8_t *dest_gid_16bytes)
{
    PVRDMADev *pvrdma = handle;
    union ibv_gid dgid;

    if (!pvrdma)
        return UINT32_MAX;

    if (!dest_gid_16bytes)
        return tcp_backend_node_from_gid(&pvrdma->backend_dev, NULL);

    memcpy(&dgid, dest_gid_16bytes, sizeof(dgid));
    return tcp_backend_node_from_gid(&pvrdma->backend_dev, &dgid);
}

int ernic_mesh_claim_gid(pvrdma_handle_t handle, const uint8_t *gid_16bytes,
                         bool add)
{
    PVRDMADev *pvrdma = handle;
    union ibv_gid gid;

    if (!pvrdma || !gid_16bytes)
        return -EINVAL;

    memcpy(&gid, gid_16bytes, sizeof(gid));
    return tcp_backend_claim_gid(&pvrdma->backend_dev, &gid, add);
}

int ernic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return -EINVAL;
    return tcp_backend_send_ionic_v(&pvrdma->backend_dev, dst_node, hdr,
                                    hdr_len, body, body_len, tag, reply);
}

bool ernic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return true;
    return tcp_backend_ionic_has_room(&pvrdma->backend_dev, dst_node, len);
}

void ernic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return;
    tcp_backend_ionic_cancel(&pvrdma->backend_dev, tag, mask);
}

void ernic_mesh_set_recv_cb(pvrdma_handle_t handle, ernic_mesh_recv_fn fn,
                            void *opaque)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return;
    tcp_backend_set_ionic_recv_cb(&pvrdma->backend_dev, (tcp_ionic_recv_fn)fn,
                                  opaque);
}
