/*
 * The RDMA core's resource manager, for the ionic admin queue
 *
 * Thin wrappers that call rdma_rm_* in src/pvrdma/ on the admin queue's
 * behalf, through the opaque handle from rocm_ernic_compat.h, so that the
 * admin queue never sees the core's types (PVRDMADev, RdmaDeviceResources,
 * RdmaBackendDev). rocm_ernic_compat.c implements them.
 *
 * Those that return int return 0 on success, -errno on failure.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_RM_H
#define ERNIC_RM_H

#include <stdint.h>

#include "rocm_ernic_compat.h"

/**
 * ernic_rm_alloc_cq - Allocate a completion queue via rdma_rm
 * @param handle pvrdma device handle (must be realized)
 * @param cqe number of CQ entries
 * @param cq_handle output: allocated CQ handle
 */
int ernic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe,
                      uint32_t *cq_handle);

/**
 * ernic_rm_dealloc_cq - Free a completion queue
 */
void ernic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle);

/**
 * ernic_rm_alloc_srq - Allocate a shared receive queue
 * @param handle pvrdma device handle (must be realized)
 * @param pd_handle protection domain the SRQ belongs to
 * @param max_wr max receive work requests
 * @param max_sge max scatter/gather entries per work request
 * @param srq_limit low-watermark at which the device raises an SRQ_LIMIT event
 * @param srq_handle output: allocated SRQ handle
 */
int ernic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle);

/**
 * ernic_rm_dealloc_srq - Free a shared receive queue
 */
void ernic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle);

/**
 * ernic_rm_modify_srq - Change an SRQ's low-watermark
 */
int ernic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit);

/**
 * ernic_rm_query_srq - Read back an SRQ's attributes
 */
int ernic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge,
                       uint32_t *srq_limit);

/**
 * ernic_rm_alloc_pd - Allocate a protection domain
 */
int ernic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle);

/**
 * ernic_rm_dealloc_pd - Free a protection domain, once nothing uses it
 */
void ernic_rm_dealloc_pd(pvrdma_handle_t handle, uint32_t pd_handle);

/**
 * ernic_rm_alloc_qp - Allocate a queue pair
 * @param handle pvrdma device handle
 * @param pd_handle protection domain handle
 * @param qp_type QP type (enum ibv_qp_type, or ERNIC_IB_QPT_GSI)
 * @param max_send_wr max send work requests
 * @param max_recv_wr max recv work requests
 * @param send_cq_handle CQ handle for send completions
 * @param recv_cq_handle CQ handle for recv completions
 * @param qpn output: QP number assigned
 */
int ernic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn);

/**
 * ernic_rm_dealloc_qp - Free a queue pair
 */
void ernic_rm_dealloc_qp(pvrdma_handle_t handle, uint32_t qpn);

/**
 * ernic_rm_alloc_mr - Allocate a memory region
 * @param handle pvrdma device handle
 * @param pd_handle protection domain
 * @param access_flags MR access flags
 * @param mr_handle output handle
 */
int ernic_rm_alloc_mr(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint32_t access_flags, uint32_t *mr_handle);

/**
 * ernic_rm_dealloc_mr - Free a memory region
 */
void ernic_rm_dealloc_mr(pvrdma_handle_t handle, uint32_t mr_handle);

/**
 * ernic_rm_modify_qp - Modify a queue pair's state
 * @param handle pvrdma device handle
 * @param qpn QP number (handle)
 * @param attr_mask IB attr mask (big-endian u32 from WQE, we convert)
 * @param type_state ionic type_state byte: bits[3:0]=to_state,
 *                   bits[7:4]=from_state
 * @param sq_psn SQ starting PSN
 * @param rq_psn RQ starting PSN
 * @param qkey_dest_qpn Q-Key / destination QPN (combined le32)
 * @param dest_gid_16bytes 16-byte destination GID (raw bytes, zeroed if not
 *                         RC)
 */
int ernic_rm_modify_qp(pvrdma_handle_t handle, uint32_t qpn, uint32_t attr_mask,
                       uint8_t type_state, uint32_t sq_psn, uint32_t rq_psn,
                       uint32_t qkey_dest_qpn, const uint8_t *dest_gid_16bytes);

/**
 * ernic_rm_query_qp - Read a queue pair's attributes back out of rdma_rm
 * @param handle pvrdma device handle
 * @param qpn QP number (handle)
 * @param state ibv_qp_state the device holds for the QP
 * @param path_mtu ibv_mtu enum
 * @param dest_qpn peer QPN, 0 when the backend tracks none
 * @param access_flags ibv_access_flags bitmask
 * @param rq_psn receive PSN the guest last set, 0 if it never set one
 * @param sq_psn send PSN the guest last set, 0 if it never set one
 *
 * Any out parameter may be NULL.  Returns 0 on success, -errno on failure.
 */
int ernic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn);

#endif /* ERNIC_RM_H */
