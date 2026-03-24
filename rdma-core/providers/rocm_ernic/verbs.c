/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Standard verbs for the rocm_ernic provider.
 * Written against rdma-core v62.0 APIs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

#include "rocm_ernic.h"

#define ROCM_ERNIC_DEFAULT_WQE_SIZE 128
#define ROCM_ERNIC_QP_HEADER_PAGES  1

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static size_t next_pow2(size_t v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    v++;
    return v < 1 ? 1 : v;
}

int rocm_ernic_query_device(
    struct ibv_context *ctx,
    const struct ibv_query_device_ex_input *in,
    struct ibv_device_attr_ex *attr,
    size_t attr_size)
{
    struct ib_uverbs_ex_query_device_resp resp;
    size_t resp_size = sizeof(resp);

    return ibv_cmd_query_device_any(
        ctx, in, attr, attr_size, &resp, &resp_size);
}

int rocm_ernic_query_port(struct ibv_context *ctx,
                          uint8_t port,
                          struct ibv_port_attr *attr)
{
    struct ibv_query_port cmd;

    return ibv_cmd_query_port(ctx, port, attr,
                              &cmd, sizeof(cmd));
}

struct ibv_pd *rocm_ernic_alloc_pd(
    struct ibv_context *ctx)
{
    struct ibv_alloc_pd cmd;
    struct rocm_ernic_alloc_pd_resp_ex resp = {};
    struct rocm_ernic_pd *pd;

    pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;

    if (ibv_cmd_alloc_pd(ctx, &pd->ibvpd,
                         &cmd, sizeof(cmd),
                         &resp.ibv_resp,
                         sizeof(resp))) {
        free(pd);
        return NULL;
    }

    pd->pdn = resp.pdn;
    return &pd->ibvpd;
}

int rocm_ernic_dealloc_pd(struct ibv_pd *ibpd)
{
    int ret;

    ret = ibv_cmd_dealloc_pd(ibpd);
    if (ret)
        return ret;

    free(ibpd);
    return 0;
}

struct ibv_mr *rocm_ernic_reg_mr(struct ibv_pd *pd,
                                 void *addr,
                                 size_t length,
                                 uint64_t hca_va,
                                 int access)
{
    struct verbs_mr *vmr;
    struct ibv_reg_mr cmd;
    struct ib_uverbs_reg_mr_resp resp;
    int ret;

    vmr = calloc(1, sizeof(*vmr));
    if (!vmr)
        return NULL;

    ret = ibv_cmd_reg_mr(pd, addr, length, hca_va,
                         access, vmr, &cmd,
                         sizeof(cmd), &resp,
                         sizeof(resp));
    if (ret) {
        free(vmr);
        return NULL;
    }

    return &vmr->ibv_mr;
}

int rocm_ernic_dereg_mr(struct verbs_mr *vmr)
{
    int ret;

    ret = ibv_cmd_dereg_mr(vmr);
    if (ret)
        return ret;

    free(vmr);
    return 0;
}

struct ibv_cq *rocm_ernic_create_cq_v(
    struct ibv_context *ctx, int cqe,
    struct ibv_comp_channel *ch,
    int comp_vector)
{
    struct rocm_ernic_cq *cq;
    struct rocm_ernic_create_cq_cmd cmd = {};
    struct rocm_ernic_create_cq_resp_ex resp = {};
    size_t buf_size;
    int ret;

    cq = calloc(1, sizeof(*cq));
    if (!cq)
        return NULL;

    buf_size = align_up(
        (size_t)cqe * sizeof(struct rocm_ernic_cqe),
        4096);
    cq->buf = mmap(NULL, buf_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (cq->buf == MAP_FAILED) {
        free(cq);
        return NULL;
    }
    cq->buf_len = buf_size;

    cmd.buf_addr = (uintptr_t)cq->buf;
    cmd.buf_size = (uint32_t)buf_size;
    cmd.ncqe = (uint32_t)cqe;
    cmd.cqe_size = sizeof(struct rocm_ernic_cqe);

    ret = ibv_cmd_create_cq(ctx, cqe, ch, comp_vector,
                            &cq->vcq.cq,
                            &cmd.ibv_cmd, sizeof(cmd),
                            &resp.ibv_resp,
                            sizeof(resp));
    if (ret) {
        munmap(cq->buf, buf_size);
        free(cq);
        return NULL;
    }

    cq->cqn = resp.cqn;
    cq->ncqe = resp.ncqe ? resp.ncqe : (uint32_t)cqe;
    cq->cqe_size = resp.cqe_size
                       ? resp.cqe_size
                       : sizeof(struct rocm_ernic_cqe);

    return &cq->vcq.cq;
}

int rocm_ernic_destroy_cq_v(struct ibv_cq *ibcq)
{
    struct rocm_ernic_cq *cq = to_rocm_ernic_cq(ibcq);
    int ret;

    ret = ibv_cmd_destroy_cq(ibcq);
    if (ret)
        return ret;

    if (cq->buf && cq->buf_len)
        munmap(cq->buf, cq->buf_len);
    free(cq);
    return 0;
}

int rocm_ernic_poll_cq_v(struct ibv_cq *cq, int ne,
                         struct ibv_wc *wc)
{
    return ibv_cmd_poll_cq(cq, ne, wc);
}

int rocm_ernic_req_notify_cq_v(struct ibv_cq *cq,
                               int solicited_only)
{
    return ibv_cmd_req_notify_cq(cq, solicited_only);
}

struct ibv_qp *rocm_ernic_create_qp_v(
    struct ibv_pd *pd,
    struct ibv_qp_init_attr *attr)
{
    struct rocm_ernic_qp *qp;
    struct rocm_ernic_create_qp_cmd cmd = {};
    struct rocm_ernic_create_qp_resp_ex resp = {};
    size_t sq_depth, rq_depth, sq_wqe_size, rq_wqe_size;
    size_t sq_size, rq_size;
    void *sq_buf = NULL, *rq_buf = NULL;
    int ret;

    qp = calloc(1, sizeof(*qp));
    if (!qp)
        return NULL;

    sq_depth = next_pow2(attr->cap.max_send_wr
                         ? attr->cap.max_send_wr : 1);
    rq_depth = next_pow2(attr->cap.max_recv_wr
                         ? attr->cap.max_recv_wr : 1);
    sq_wqe_size = ROCM_ERNIC_DEFAULT_WQE_SIZE;
    rq_wqe_size = ROCM_ERNIC_DEFAULT_WQE_SIZE;

    sq_size = align_up(
        ROCM_ERNIC_QP_HEADER_PAGES * 4096 +
        sq_depth * sq_wqe_size, 4096);
    sq_buf = mmap(NULL, sq_size,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    if (sq_buf == MAP_FAILED) {
        free(qp);
        return NULL;
    }

    rq_size = align_up(rq_depth * rq_wqe_size, 4096);
    rq_buf = mmap(NULL, rq_size,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    if (rq_buf == MAP_FAILED) {
        munmap(sq_buf, sq_size);
        free(qp);
        return NULL;
    }

    cmd.sbuf_addr = (uintptr_t)sq_buf;
    cmd.sbuf_size = (uint32_t)sq_size;
    cmd.sq_wqe_size = (uint32_t)sq_wqe_size;
    cmd.sq_depth = (uint32_t)sq_depth;
    cmd.rbuf_addr = (uintptr_t)rq_buf;
    cmd.rbuf_size = (uint32_t)rq_size;
    cmd.rq_wqe_size = (uint32_t)rq_wqe_size;
    cmd.rq_depth = (uint32_t)rq_depth;

    ret = ibv_cmd_create_qp(pd, &qp->vqp.qp, attr,
                            &cmd.ibv_cmd, sizeof(cmd),
                            &resp.ibv_resp,
                            sizeof(resp));
    if (ret) {
        munmap(rq_buf, rq_size);
        munmap(sq_buf, sq_size);
        free(qp);
        return NULL;
    }

    qp->qpn = resp.qpn;
    qp->qp_handle = resp.qp_handle;
    qp->sq_depth = resp.sq_depth;
    qp->rq_depth = resp.rq_depth;
    qp->sq_wqe_size = resp.sq_wqe_size;
    qp->rq_wqe_size = resp.rq_wqe_size;
    qp->uar_qp_offset = resp.uar_qp_offset;
    qp->uar_cq_offset = resp.uar_cq_offset;

    return &qp->vqp.qp;
}

int rocm_ernic_modify_qp_v(struct ibv_qp *qp,
                           struct ibv_qp_attr *attr,
                           int attr_mask)
{
    struct ibv_modify_qp cmd;

    return ibv_cmd_modify_qp(qp, attr, attr_mask,
                             &cmd, sizeof(cmd));
}

int rocm_ernic_query_qp_v(struct ibv_qp *qp,
                          struct ibv_qp_attr *attr,
                          int attr_mask,
                          struct ibv_qp_init_attr *ia)
{
    struct ibv_query_qp cmd;

    return ibv_cmd_query_qp(qp, attr, attr_mask,
                            ia, &cmd, sizeof(cmd));
}

int rocm_ernic_destroy_qp_v(struct ibv_qp *ibqp)
{
    struct rocm_ernic_qp *qp = to_rocm_ernic_qp(ibqp);
    int ret;

    ret = ibv_cmd_destroy_qp(ibqp);
    if (ret)
        return ret;

    free(qp);
    return 0;
}

int rocm_ernic_post_send_v(struct ibv_qp *qp,
                           struct ibv_send_wr *wr,
                           struct ibv_send_wr **bad_wr)
{
    return ibv_cmd_post_send(qp, wr, bad_wr);
}

int rocm_ernic_post_recv_v(struct ibv_qp *qp,
                           struct ibv_recv_wr *wr,
                           struct ibv_recv_wr **bad_wr)
{
    return ibv_cmd_post_recv(qp, wr, bad_wr);
}
