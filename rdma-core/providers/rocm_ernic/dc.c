/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * rocm_ernic Dynamic Connection helpers (guest userspace).  This is not
 * libmlx5 / mlx5dv wire compatibility; applications must port explicitly.
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

#include "rocm_ernic_dc.h"
#include "rocm_ernic.h"

#define ROCM_ERNIC_WR_SEND_DC        17U
/* Must match the enum of the same name in verbs.c. */
#define ROCM_ERNIC_WR_FLAG_SIGNALED  1U
#define ROCM_ERNIC_QP_HEADER_PAGES   1
#define ROCM_ERNIC_SQ_WQE_HDR_SIZE   80
#define ROCM_ERNIC_RQ_WQE_HDR_SIZE   16
#define ROCM_ERNIC_SGE_SIZE          16
#define ROCM_ERNIC_UAR_QP_SEND       (1U << 30)

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

struct rocm_ernic_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
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
            uint32_t remote_dctn;
            uint32_t dc_access_key;
            uint32_t ah_id;
            uint32_t rsv;
            uint8_t __pad[32];
        } dc;
    } wr;
};

struct rocm_ernic_ring {
    _Atomic uint32_t prod_tail;
    _Atomic uint32_t cons_head;
};

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

static inline void uar_write32(struct rocm_ernic_qp *qp, uint32_t val)
{
    if (qp->uar_ptr) {
        volatile uint32_t *db =
            (volatile uint32_t *)((char *)qp->uar_ptr + qp->uar_qp_offset);
        *db = htole32(val);
    }
}

struct ibv_qp *rocm_ernic_dc_create_dct(struct ibv_pd *pd,
                                         struct ibv_qp_init_attr *attr,
                                         const struct rocm_ernic_dc_dct_init *dct)
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

    if (!pd || !attr || !dct || !attr->srq || !dct->access_key) {
        errno = EINVAL;
        return NULL;
    }

    if (page_size <= 0) {
        errno = EINVAL;
        return NULL;
    }

    qp = calloc(1, sizeof(*qp));
    if (!qp) {
        return NULL;
    }

    attr->qp_type = IBV_QPT_DRIVER;

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

    rq_size = (rq_depth * rq_wqe_size + page_size - 1) & ~(page_size - 1);
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
    cmd.ex_mask = ROCM_ERNIC_CREATE_QP_EX_DC;
    cmd.dc_role = ROCM_ERNIC_DC_ROLE_DCT;
    cmd.dc_port_num = dct->port_num ? dct->port_num : 1;
    cmd.dct_access_key = dct->access_key;

    ret = ibv_cmd_create_qp(pd, &qp->vqp.qp, attr, &cmd.ibv_cmd, sizeof(cmd),
                            &resp.ibv_resp, sizeof(resp));
    if (ret) {
        munmap(rq_buf, rq_size);
        munmap(sq_buf, sq_size);
        free(qp);
        errno = ret > 0 ? ret : -ret;
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
    qp->sq_ring = (struct rocm_ernic_ring *)sq_buf;
    qp->rq_ring = (struct rocm_ernic_ring *)((char *)sq_buf +
                                            sizeof(struct rocm_ernic_ring));
    qp->sq_offset = ROCM_ERNIC_QP_HEADER_PAGES * page_size;
    qp->rq_offset = 0;
    if (resp.resp_ex_mask & ROCM_ERNIC_CREATE_QP_RESP_EX_DC) {
        qp->dctn = resp.dctn;
    }

    if (resp.uar_mmap_offset) {
        qp->uar_ptr = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED,
                           pd->context->cmd_fd, resp.uar_mmap_offset);
        if (qp->uar_ptr == MAP_FAILED) {
            qp->uar_ptr = NULL;
        }
    }

    return &qp->vqp.qp;
}

struct ibv_qp *rocm_ernic_dc_create_dci(struct ibv_pd *pd,
                                       struct ibv_qp_init_attr *attr,
                                       const struct rocm_ernic_dc_dci_init *dci)
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

    (void)dci;

    if (!pd || !attr) {
        errno = EINVAL;
        return NULL;
    }

    if (page_size <= 0) {
        errno = EINVAL;
        return NULL;
    }

    qp = calloc(1, sizeof(*qp));
    if (!qp) {
        return NULL;
    }

    attr->qp_type = IBV_QPT_DRIVER;

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

    rq_size = (rq_depth * rq_wqe_size + page_size - 1) & ~(page_size - 1);
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
    cmd.ex_mask = ROCM_ERNIC_CREATE_QP_EX_DC;
    cmd.dc_role = ROCM_ERNIC_DC_ROLE_DCI;
    cmd.dc_port_num = 1;

    ret = ibv_cmd_create_qp(pd, &qp->vqp.qp, attr, &cmd.ibv_cmd, sizeof(cmd),
                            &resp.ibv_resp, sizeof(resp));
    if (ret) {
        munmap(rq_buf, rq_size);
        munmap(sq_buf, sq_size);
        free(qp);
        errno = ret > 0 ? ret : -ret;
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
    qp->sq_ring = (struct rocm_ernic_ring *)sq_buf;
    qp->rq_ring = (struct rocm_ernic_ring *)((char *)sq_buf +
                                            sizeof(struct rocm_ernic_ring));
    qp->sq_offset = ROCM_ERNIC_QP_HEADER_PAGES * page_size;
    qp->rq_offset = 0;

    if (resp.uar_mmap_offset) {
        qp->uar_ptr = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED,
                           pd->context->cmd_fd, resp.uar_mmap_offset);
        if (qp->uar_ptr == MAP_FAILED) {
            qp->uar_ptr = NULL;
        }
    }

    return &qp->vqp.qp;
}

int rocm_ernic_dc_post_send(struct ibv_qp *ibqp, uint64_t wr_id,
                            const struct ibv_sge *sg_list, int num_sge,
                            uint32_t remote_dctn, uint32_t dc_access_key,
                            uint32_t ah_id)
{
    struct rocm_ernic_qp *qp = to_rocm_ernic_qp(ibqp);
    uint32_t tail = 0;
    struct rocm_ernic_sq_wqe_hdr *wqe;
    struct rocm_ernic_sge *sge;
    int i;

    if (!qp->sq_buf || !qp->sq_ring || num_sge < 0)
        return EINVAL;
    if (qp->sq_wqe_size <= ROCM_ERNIC_SQ_WQE_HDR_SIZE ||
        (uint32_t)num_sge >
            (qp->sq_wqe_size - ROCM_ERNIC_SQ_WQE_HDR_SIZE) /
                ROCM_ERNIC_SGE_SIZE) {
        return EINVAL;
    }

    int sq_status = ring_has_space(qp->sq_ring, qp->sq_depth, &tail);

    if (sq_status <= 0) {
        return (sq_status < 0) ? EINVAL : ENOMEM;
    }

    wqe = (struct rocm_ernic_sq_wqe_hdr *)((char *)qp->sq_buf + qp->sq_offset +
                                           (size_t)tail * qp->sq_wqe_size);
    memset(wqe, 0, sizeof(*wqe));
    wqe->wr_id = wr_id;
    wqe->num_sge = (uint32_t)num_sge;
    wqe->opcode = ROCM_ERNIC_WR_SEND_DC;
    /*
     * The device only raises a completion for a WR carrying
     * the SIGNALED flag.  This entry point takes a wr_id but
     * no flags argument, so its callers are entitled to reap
     * a completion for every post; mark them all signalled.
     */
    wqe->send_flags = ROCM_ERNIC_WR_FLAG_SIGNALED;
    wqe->wr.dc.remote_dctn = remote_dctn;
    wqe->wr.dc.dc_access_key = dc_access_key;
    wqe->wr.dc.ah_id = ah_id;

    sge = (struct rocm_ernic_sge *)(wqe + 1);
    for (i = 0; i < num_sge; i++) {
        sge->addr = sg_list[i].addr;
        sge->length = sg_list[i].length;
        sge->lkey = sg_list[i].lkey;
        sge++;
    }

    __sync_synchronize();
    ring_inc(&qp->sq_ring->prod_tail, qp->sq_depth);

    uar_write32(qp, ROCM_ERNIC_UAR_QP_SEND | qp->qp_handle);
    return 0;
}

uint32_t rocm_ernic_dc_get_dctn(const struct ibv_qp *ibqp)
{
    const struct rocm_ernic_qp *qp = to_rocm_ernic_qp((struct ibv_qp *)ibqp);

    return qp->dctn;
}
