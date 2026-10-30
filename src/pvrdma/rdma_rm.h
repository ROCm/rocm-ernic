/*
 * RDMA device: Definitions of Resource Manager functions
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

#ifndef RDMA_RM_H
#define RDMA_RM_H

#include <stdint.h>
#include <infiniband/verbs.h>
#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_rm_defs.h"

int rdma_rm_init(RdmaDeviceResources *dev_res,
                 const struct ibv_device_attr *dev_attr);
void rdma_rm_fini(RdmaDeviceResources *dev_res);

int rdma_rm_alloc_pd(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t *pd_handle);
RdmaRmPD *rdma_rm_get_pd(RdmaDeviceResources *dev_res, uint32_t pd_handle);
void rdma_rm_dealloc_pd(RdmaDeviceResources *dev_res, uint32_t pd_handle);

int rdma_rm_alloc_mr(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     int access_flags, uint32_t *mr_handle, uint32_t *lkey,
                     uint32_t *rkey);
RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle);
void rdma_rm_dealloc_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle);

int rdma_rm_alloc_cq(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t cqe, uint32_t *cq_handle);
RdmaRmCQ *rdma_rm_get_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle);
void rdma_rm_dealloc_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle);

int rdma_rm_alloc_qp(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     uint8_t qp_type, uint32_t send_cq_handle,
                     uint32_t recv_cq_handle, uint32_t *qpn, uint8_t is_srq,
                     uint32_t srq_handle, RdmaRmQP **out_qp);
RdmaRmQP *rdma_rm_get_qp(RdmaDeviceResources *dev_res, uint32_t qpn);
int rdma_rm_modify_qp(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                      uint32_t qp_handle, uint32_t attr_mask, uint8_t sgid_idx,
                      union ibv_gid *dgid, uint32_t dqpn,
                      enum ibv_qp_state qp_state, uint32_t qkey,
                      uint32_t rq_psn, uint32_t sq_psn);
int rdma_rm_query_qp(RdmaDeviceResources *dev_res, uint32_t qp_handle,
                     struct ibv_qp_attr *attr, int attr_mask,
                     struct ibv_qp_init_attr *init_attr);
void rdma_rm_dealloc_qp(RdmaDeviceResources *dev_res, uint32_t qp_handle);

RdmaRmSRQ *rdma_rm_get_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle);
int rdma_rm_alloc_srq(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                      uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                      uint32_t *srq_handle);
int rdma_rm_query_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle,
                      struct ibv_srq_attr *srq_attr);
int rdma_rm_modify_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle,
                       struct ibv_srq_attr *srq_attr, int srq_attr_mask);
void rdma_rm_dealloc_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle);

#endif
