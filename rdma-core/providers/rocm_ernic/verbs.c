/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Standard verbs for the rocm_ernic provider.
 * Written against rdma-core v64.0 APIs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

#include "rocm_ernic.h"

#define ROCM_ERNIC_QP_HEADER_PAGES 1
#define ROCM_ERNIC_SQ_WQE_HDR_SIZE 80
#define ROCM_ERNIC_RQ_WQE_HDR_SIZE 16
#define ROCM_ERNIC_SGE_SIZE        16

#define ROCM_ERNIC_UAR_QP_OFFSET 0
#define ROCM_ERNIC_UAR_QP_SEND   (1U << 30)
#define ROCM_ERNIC_UAR_QP_RECV   (1U << 31)

struct rocm_ernic_ring {
    _Atomic uint32_t prod_tail;
    _Atomic uint32_t cons_head;
};

struct rocm_ernic_ring_state {
    struct rocm_ernic_ring tx;
    struct rocm_ernic_ring rx;
};

struct rocm_ernic_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct rocm_ernic_rq_wqe_hdr {
    uint64_t wr_id;
    uint32_t num_sge;
    uint32_t total_len;
};

struct rocm_ernic_sq_wqe_hdr {
    uint64_t wr_id;
    uint32_t num_sge;
    uint32_t total_len;
    uint32_t opcode;
    uint32_t send_flags;
    union {
        uint32_t imm_data;
        uint32_t invalidate_rkey;
    } ex;
    uint32_t reserved;
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint8_t reserved[4];
        } rdma;
        struct {
            uint64_t remote_addr;
            uint64_t compare_add;
            uint64_t swap;
            uint32_t rkey;
            uint32_t reserved;
        } atomic;
        struct {
            uint64_t iova_start;
            uint64_t pl_pdir_dma;
            uint32_t page_shift;
            uint32_t page_list_len;
            uint32_t length;
            uint32_t access_flags;
            uint32_t rkey;
            uint32_t reserved;
        } fast_reg;
        uint8_t _pad[48];
    } wr;
};

enum {
    ROCM_ERNIC_WR_RDMA_WRITE = 0,
    ROCM_ERNIC_WR_RDMA_WRITE_IMM = 1,
    ROCM_ERNIC_WR_SEND = 2,
    ROCM_ERNIC_WR_SEND_IMM = 3,
    ROCM_ERNIC_WR_RDMA_READ = 4,
    ROCM_ERNIC_WR_ATOMIC_CMP_SWP = 6,
    ROCM_ERNIC_WR_ATOMIC_FETCH_ADD = 7,
    ROCM_ERNIC_WR_SEND_INV = 9,
    ROCM_ERNIC_WR_LOCAL_INV = 10,
    ROCM_ERNIC_WR_REG_MR = 11,
    ROCM_ERNIC_WR_ERROR = 255,
};

enum {
    ROCM_ERNIC_WR_FLAG_SIGNALED = 1 << 0,
    ROCM_ERNIC_WR_FLAG_SOLICITED = 1 << 1,
    ROCM_ERNIC_WR_FLAG_INLINE = 1 << 2,
    ROCM_ERNIC_WR_FLAG_FENCE = 1 << 3,
};

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static size_t next_pow2(size_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 1 ? 1 : v;
}

static inline int ring_idx_valid(uint32_t idx, uint32_t max)
{
    return (idx & ~((max << 1) - 1)) == 0;
}

static inline int ring_has_space(struct rocm_ernic_ring *r, uint32_t max,
                                 uint32_t *out_tail)
{
    uint32_t tail = atomic_load(&r->prod_tail);
    uint32_t head = atomic_load(&r->cons_head);

    if (ring_idx_valid(tail, max) && ring_idx_valid(head, max)) {
        *out_tail = tail & (max - 1);
        return tail != (head ^ max);
    }
    return -1;
}

static inline void ring_inc(_Atomic uint32_t *var, uint32_t max)
{
    uint32_t idx = atomic_load(var) + 1;
    idx &= (max << 1) - 1;
    atomic_store(var, idx);
}

static uint32_t ib_to_rocm_opcode(enum ibv_wr_opcode op)
{
    switch (op) {
    case IBV_WR_RDMA_WRITE:
        return ROCM_ERNIC_WR_RDMA_WRITE;
    case IBV_WR_RDMA_WRITE_WITH_IMM:
        return ROCM_ERNIC_WR_RDMA_WRITE_IMM;
    case IBV_WR_SEND:
        return ROCM_ERNIC_WR_SEND;
    case IBV_WR_SEND_WITH_IMM:
        return ROCM_ERNIC_WR_SEND_IMM;
    case IBV_WR_RDMA_READ:
        return ROCM_ERNIC_WR_RDMA_READ;
    case IBV_WR_ATOMIC_CMP_AND_SWP:
        return ROCM_ERNIC_WR_ATOMIC_CMP_SWP;
    case IBV_WR_ATOMIC_FETCH_AND_ADD:
        return ROCM_ERNIC_WR_ATOMIC_FETCH_ADD;
    case IBV_WR_SEND_WITH_INV:
        return ROCM_ERNIC_WR_SEND_INV;
    case IBV_WR_LOCAL_INV:
        return ROCM_ERNIC_WR_LOCAL_INV;
    default:
        return ROCM_ERNIC_WR_ERROR;
    }
}

static uint32_t ib_to_rocm_send_flags(int flags)
{
    uint32_t out = 0;
    if (flags & IBV_SEND_SIGNALED)
        out |= ROCM_ERNIC_WR_FLAG_SIGNALED;
    if (flags & IBV_SEND_SOLICITED)
        out |= ROCM_ERNIC_WR_FLAG_SOLICITED;
    if (flags & IBV_SEND_INLINE)
        out |= ROCM_ERNIC_WR_FLAG_INLINE;
    if (flags & IBV_SEND_FENCE)
        out |= ROCM_ERNIC_WR_FLAG_FENCE;
    return out;
}

/* ---- Standard verbs ---- */

int rocm_ernic_query_device(struct ibv_context *ctx,
                            const struct ibv_query_device_ex_input *in,
                            struct ibv_device_attr_ex *attr, size_t attr_size)
{
    struct ib_uverbs_ex_query_device_resp resp;
    size_t resp_size = sizeof(resp);

    return ibv_cmd_query_device_any(ctx, in, attr, attr_size, &resp,
                                    &resp_size);
}

int rocm_ernic_query_port(struct ibv_context *ctx, uint8_t port,
                          struct ibv_port_attr *attr)
{
    struct ibv_query_port cmd;

    return ibv_cmd_query_port(ctx, port, attr, &cmd, sizeof(cmd));
}

struct ibv_pd *rocm_ernic_alloc_pd(struct ibv_context *ctx)
{
    struct ibv_alloc_pd cmd;
    struct rocm_ernic_alloc_pd_resp_ex resp = {};
    struct rocm_ernic_pd *pd;

    pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;

    if (ibv_cmd_alloc_pd(ctx, &pd->ibvpd, &cmd, sizeof(cmd), &resp.ibv_resp,
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

struct ibv_mr *rocm_ernic_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
                                 uint64_t hca_va, int access)
{
    struct verbs_mr *vmr;
    struct ibv_reg_mr cmd;
    struct ib_uverbs_reg_mr_resp resp;
    int ret;

    vmr = calloc(1, sizeof(*vmr));
    if (!vmr)
        return NULL;

    ret = ibv_cmd_reg_mr(pd, addr, length, hca_va, access, vmr, &cmd,
                         sizeof(cmd), &resp, sizeof(resp));
    if (ret) {
        free(vmr);
        return NULL;
    }

    return &vmr->ibv_mr;
}

struct ibv_mr *rocm_ernic_reg_dmabuf_mr(
    struct ibv_pd *pd, uint64_t offset,
    size_t length, uint64_t iova, int fd,
    int access)
{
    struct verbs_mr *vmr;
    int ret;

    vmr = calloc(1, sizeof(*vmr));
    if (!vmr)
        return NULL;

    ret = ibv_cmd_reg_dmabuf_mr(
        pd, offset, length, iova, fd, access,
        vmr, NULL);
    if (ret) {
        free(vmr);
        errno = -ret;
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

struct ibv_cq *rocm_ernic_create_cq_v(struct ibv_context *ctx, int cqe,
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

    buf_size =
        4096 + align_up((size_t)cqe * sizeof(struct rocm_ernic_cqe), 4096);
    cq->buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cq->buf == MAP_FAILED) {
        free(cq);
        return NULL;
    }
    cq->buf_len = buf_size;

    cmd.buf_addr = (uintptr_t)cq->buf;
    cmd.buf_size = (uint32_t)buf_size;
    cmd.ncqe = (uint32_t)cqe;
    cmd.cqe_size = sizeof(struct rocm_ernic_cqe);

    ret =
        ibv_cmd_create_cq(ctx, cqe, ch, comp_vector, &cq->vcq.cq, &cmd.ibv_cmd,
                          sizeof(cmd), &resp.ibv_resp, sizeof(resp));
    if (ret) {
        munmap(cq->buf, buf_size);
        free(cq);
        return NULL;
    }

    cq->cqn = resp.cqn;
    cq->ncqe = resp.ncqe ? resp.ncqe : (uint32_t)cqe;
    cq->cqe_size =
        resp.cqe_size ? resp.cqe_size : sizeof(struct rocm_ernic_cqe);

    /*
     * Ring state is at the start of the buffer
     * (header page).  CQEs follow at offset 4096.
     * The rx ring (second ring struct) tracks CQ
     * completions: server is producer, user is
     * consumer.
     */
    cq->cq_rx_ring = (struct rocm_ernic_ring *)((char *)cq->buf +
                                                sizeof(struct rocm_ernic_ring));
    cq->cq_offset = 4096;

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

enum {
    ROCM_ERNIC_WC_SEND = 0,
    ROCM_ERNIC_WC_RDMA_WRITE = 1,
    ROCM_ERNIC_WC_RDMA_READ = 2,
    ROCM_ERNIC_WC_COMP_SWAP = 3,
    ROCM_ERNIC_WC_FETCH_ADD = 4,
    ROCM_ERNIC_WC_RECV = 1 << 7,
    ROCM_ERNIC_WC_RECV_RDMA_WITH_IMM = (1 << 7) + 1,
};

#define ROCM_ERNIC_UAR_CQ_OFFSET 4
#define ROCM_ERNIC_UAR_CQ_POLL   (1U << 31)

static enum ibv_wc_opcode rocm_wc_opcode(uint32_t op)
{
    switch (op) {
    case ROCM_ERNIC_WC_SEND:
        return IBV_WC_SEND;
    case ROCM_ERNIC_WC_RDMA_WRITE:
        return IBV_WC_RDMA_WRITE;
    case ROCM_ERNIC_WC_RDMA_READ:
        return IBV_WC_RDMA_READ;
    case ROCM_ERNIC_WC_COMP_SWAP:
        return IBV_WC_COMP_SWAP;
    case ROCM_ERNIC_WC_FETCH_ADD:
        return IBV_WC_FETCH_ADD;
    case ROCM_ERNIC_WC_RECV:
        return IBV_WC_RECV;
    case ROCM_ERNIC_WC_RECV_RDMA_WITH_IMM:
        return IBV_WC_RECV_RDMA_WITH_IMM;
    default:
        return IBV_WC_SEND;
    }
}

static int rocm_ernic_poll_one(struct rocm_ernic_cq *cq,
                               struct rocm_ernic_qp *qp, struct ibv_wc *wc)
{
    struct rocm_ernic_ring *ring = cq->cq_rx_ring;
    uint32_t tail, head, idx;
    struct rocm_ernic_cqe *cqe;

    if (!ring)
        return -1;

    tail = atomic_load(&ring->prod_tail);
    head = atomic_load(&ring->cons_head);

    if (!ring_idx_valid(tail, cq->ncqe) || !ring_idx_valid(head, cq->ncqe))
        return -1;

    if (tail == head) {
        if (qp && qp->uar_ptr) {
            volatile uint32_t *db =
                (volatile uint32_t *)((char *)qp->uar_ptr +
                                      ROCM_ERNIC_UAR_CQ_OFFSET);
            *db = htole32(cq->cqn | ROCM_ERNIC_UAR_CQ_POLL);
        }
        return -1;
    }

    idx = head & (cq->ncqe - 1);
    cqe =
        (struct rocm_ernic_cqe *)((char *)cq->buf + cq->cq_offset +
                                  (size_t)idx * sizeof(struct rocm_ernic_cqe));

    __sync_synchronize();

    wc->wr_id = cqe->wr_id;
    wc->status = (enum ibv_wc_status)cqe->status;
    wc->opcode = rocm_wc_opcode(cqe->opcode);
    wc->byte_len = cqe->byte_len;
    wc->imm_data = cqe->imm_data;
    wc->qp_num = (uint32_t)(cqe->qp & 0xffff);
    wc->src_qp = cqe->src_qp;
    wc->wc_flags = cqe->wc_flags;
    wc->pkey_index = cqe->pkey_index;
    wc->slid = cqe->slid;
    wc->sl = cqe->sl;
    wc->dlid_path_bits = cqe->dlid_path_bits;
    wc->vendor_err = cqe->vendor_err;

    ring_inc(&ring->cons_head, cq->ncqe);
    return 0;
}

int rocm_ernic_poll_cq_v(struct ibv_cq *ibcq, int ne, struct ibv_wc *wc)
{
    struct rocm_ernic_cq *cq = to_rocm_ernic_cq(ibcq);
    int npolled = 0;

    if (!cq->cq_rx_ring)
        return ibv_cmd_poll_cq(ibcq, ne, wc);

    for (npolled = 0; npolled < ne; npolled++) {
        if (rocm_ernic_poll_one(cq, NULL, wc + npolled))
            break;
    }
    return npolled;
}

int rocm_ernic_req_notify_cq_v(struct ibv_cq *cq, int solicited_only)
{
    return ibv_cmd_req_notify_cq(cq, solicited_only);
}

struct ibv_qp *rocm_ernic_create_qp_v(struct ibv_pd *pd,
                                      struct ibv_qp_init_attr *attr)
{
    struct rocm_ernic_qp *qp;
    struct rocm_ernic_create_qp_cmd cmd = {};
    struct rocm_ernic_create_qp_resp_ex resp = {};
    size_t sq_depth, rq_depth;
    size_t sq_wqe_size, rq_wqe_size;
    size_t sq_size, rq_size;
    void *sq_buf = NULL, *rq_buf = NULL;
    int ret;
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
        return NULL;

    qp = calloc(1, sizeof(*qp));
    if (!qp)
        return NULL;

    sq_depth = attr->cap.max_send_wr ? attr->cap.max_send_wr : 1;
    rq_depth = attr->cap.max_recv_wr ? attr->cap.max_recv_wr : 1;

    {
        uint32_t max_send_sge =
            attr->cap.max_send_sge ? attr->cap.max_send_sge : 1;
        uint32_t max_recv_sge =
            attr->cap.max_recv_sge ? attr->cap.max_recv_sge : 1;
        sq_wqe_size = next_pow2(ROCM_ERNIC_SQ_WQE_HDR_SIZE +
                                ROCM_ERNIC_SGE_SIZE * max_send_sge);
        rq_wqe_size = next_pow2(ROCM_ERNIC_RQ_WQE_HDR_SIZE +
                                ROCM_ERNIC_SGE_SIZE * max_recv_sge);
    }

    sq_size = align_up(ROCM_ERNIC_QP_HEADER_PAGES * page_size +
                           sq_depth * sq_wqe_size,
                       page_size);
    sq_buf = mmap(NULL, sq_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sq_buf == MAP_FAILED) {
        free(qp);
        return NULL;
    }

    rq_size = align_up(rq_depth * rq_wqe_size, page_size);
    rq_buf = mmap(NULL, rq_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

    ret = ibv_cmd_create_qp(pd, &qp->vqp.qp, attr, &cmd.ibv_cmd, sizeof(cmd),
                            &resp.ibv_resp, sizeof(resp));
    if (ret) {
        munmap(rq_buf, rq_size);
        munmap(sq_buf, sq_size);
        free(qp);
        return NULL;
    }

    qp->qpn = resp.qpn;
    qp->qp_handle = resp.qp_handle;
    qp->sq_depth = resp.sq_depth ? resp.sq_depth : sq_depth;
    qp->rq_depth = resp.rq_depth ? resp.rq_depth : rq_depth;
    qp->sq_wqe_size = resp.sq_wqe_size ? resp.sq_wqe_size : sq_wqe_size;
    qp->rq_wqe_size = resp.rq_wqe_size ? resp.rq_wqe_size : rq_wqe_size;
    qp->uar_qp_offset = resp.uar_qp_offset;
    qp->uar_cq_offset = resp.uar_cq_offset;

    qp->sq_buf = sq_buf;
    qp->sq_buf_size = sq_size;
    qp->rq_buf = rq_buf;
    qp->rq_buf_size = rq_size;

    /*
     * The header page contains the ring state:
     * sq ring at offset 0, rq ring at offset
     * sizeof(rocm_ernic_ring).
     */
    qp->sq_ring = (struct rocm_ernic_ring *)sq_buf;
    qp->rq_ring = (struct rocm_ernic_ring *)((char *)sq_buf +
                                             sizeof(struct rocm_ernic_ring));

    qp->sq_offset = ROCM_ERNIC_QP_HEADER_PAGES * page_size;
    qp->rq_offset = 0;

    if (resp.uar_mmap_offset) {
        qp->uar_ptr = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED,
                           pd->context->cmd_fd, resp.uar_mmap_offset);
        if (qp->uar_ptr == MAP_FAILED)
            qp->uar_ptr = NULL;
    }

    return &qp->vqp.qp;
}

int rocm_ernic_modify_qp_v(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                           int attr_mask)
{
    struct ibv_modify_qp cmd;

    return ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof(cmd));
}

int rocm_ernic_query_qp_v(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                          int attr_mask, struct ibv_qp_init_attr *ia)
{
    struct ibv_query_qp cmd;

    return ibv_cmd_query_qp(qp, attr, attr_mask, ia, &cmd, sizeof(cmd));
}

int rocm_ernic_destroy_qp_v(struct ibv_qp *ibqp)
{
    struct rocm_ernic_qp *qp = to_rocm_ernic_qp(ibqp);
    int ret;

    ret = ibv_cmd_destroy_qp(ibqp);
    if (ret)
        return ret;

    if (qp->uar_ptr) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps > 0)
            munmap(qp->uar_ptr, ps);
    }
    if (qp->sq_buf && qp->sq_buf_size)
        munmap(qp->sq_buf, qp->sq_buf_size);
    if (qp->rq_buf && qp->rq_buf_size)
        munmap(qp->rq_buf, qp->rq_buf_size);
    free(qp);
    return 0;
}

static inline void uar_write32(struct rocm_ernic_qp *qp, uint32_t val)
{
    if (qp->uar_ptr) {
        volatile uint32_t *db =
            (volatile uint32_t *)((char *)qp->uar_ptr + qp->uar_qp_offset);
        *db = htole32(val);
    }
}

int rocm_ernic_post_send_v(struct ibv_qp *ibqp, struct ibv_send_wr *wr,
                           struct ibv_send_wr **bad_wr)
{
    struct rocm_ernic_qp *qp = to_rocm_ernic_qp(ibqp);
    int ret = 0;

    if (!qp->sq_buf || !qp->sq_ring) {
        *bad_wr = wr;
        return EINVAL;
    }

    while (wr) {
        uint32_t tail = 0;
        struct rocm_ernic_sq_wqe_hdr *wqe;
        struct rocm_ernic_sge *sge;
        int i;

        int sq_status = ring_has_space(qp->sq_ring, qp->sq_depth, &tail);
        if (sq_status <= 0) {
            *bad_wr = wr;
            ret = (sq_status < 0) ? EINVAL : ENOMEM;
            break;
        }

        wqe = (struct rocm_ernic_sq_wqe_hdr *)((char *)qp->sq_buf +
                                               qp->sq_offset +
                                               (size_t)tail * qp->sq_wqe_size);
        memset(wqe, 0, sizeof(*wqe));

        wqe->wr_id = wr->wr_id;
        wqe->num_sge = wr->num_sge;
        wqe->opcode = ib_to_rocm_opcode(wr->opcode);
        wqe->send_flags = ib_to_rocm_send_flags(wr->send_flags);

        if (wr->opcode == IBV_WR_SEND_WITH_IMM ||
            wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
            wqe->ex.imm_data = wr->imm_data;

        if (wr->opcode == IBV_WR_RDMA_WRITE ||
            wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM ||
            wr->opcode == IBV_WR_RDMA_READ) {
            wqe->wr.rdma.remote_addr = wr->wr.rdma.remote_addr;
            wqe->wr.rdma.rkey = wr->wr.rdma.rkey;
        }

        sge = (struct rocm_ernic_sge *)(wqe + 1);
        for (i = 0; i < wr->num_sge; i++) {
            sge->addr = wr->sg_list[i].addr;
            sge->length = wr->sg_list[i].length;
            sge->lkey = wr->sg_list[i].lkey;
            sge++;
        }

        __sync_synchronize();
        ring_inc(&qp->sq_ring->prod_tail, qp->sq_depth);
        wr = wr->next;
    }

    uar_write32(qp, ROCM_ERNIC_UAR_QP_SEND | qp->qp_handle);
    return ret;
}

int rocm_ernic_post_recv_v(struct ibv_qp *ibqp, struct ibv_recv_wr *wr,
                           struct ibv_recv_wr **bad_wr)
{
    struct rocm_ernic_qp *qp = to_rocm_ernic_qp(ibqp);
    int ret = 0;

    if (!qp->rq_buf || !qp->rq_ring) {
        *bad_wr = wr;
        return EINVAL;
    }

    while (wr) {
        uint32_t tail = 0;
        struct rocm_ernic_rq_wqe_hdr *wqe;
        struct rocm_ernic_sge *sge;
        int i;

        int rq_status = ring_has_space(qp->rq_ring, qp->rq_depth, &tail);
        if (rq_status <= 0) {
            *bad_wr = wr;
            ret = (rq_status < 0) ? EINVAL : ENOMEM;
            break;
        }

        wqe = (struct rocm_ernic_rq_wqe_hdr *)((char *)qp->rq_buf +
                                               qp->rq_offset +
                                               (size_t)tail * qp->rq_wqe_size);

        wqe->wr_id = wr->wr_id;
        wqe->num_sge = wr->num_sge;
        wqe->total_len = 0;

        sge = (struct rocm_ernic_sge *)(wqe + 1);
        for (i = 0; i < wr->num_sge; i++) {
            sge->addr = wr->sg_list[i].addr;
            sge->length = wr->sg_list[i].length;
            sge->lkey = wr->sg_list[i].lkey;
            sge++;
        }

        __sync_synchronize();
        ring_inc(&qp->rq_ring->prod_tail, qp->rq_depth);
        wr = wr->next;
    }

    __sync_synchronize();
    uar_write32(qp, ROCM_ERNIC_UAR_QP_RECV | qp->qp_handle);

    return ret;
}
