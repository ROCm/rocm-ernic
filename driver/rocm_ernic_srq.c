// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2017 VMware, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of EITHER the GNU General Public License
 * version 2 as published by the Free Software Foundation or the BSD
 * 2-Clause License. This program is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; WITHOUT EVEN THE IMPLIED
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License version 2 for more details at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program available in the file COPYING in the main
 * directory of this source tree.
 *
 * The BSD 2-Clause License
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <asm/page.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/wait.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "rocm_ernic.h"

static inline void *get_srq_wqe(struct rocm_ernic_srq *srq, unsigned int n)
{
    return rocm_ernic_page_dir_get_ptr(
        &srq->pdir, (u64)srq->offset + (u64)n * (u32)srq->wqe_size);
}

/**
 * rocm_ernic_post_srq_recv - post receive WRs to a shared receive queue
 * @ibsrq: SRQ
 * @wr: work request list
 * @bad_wr: first failing WR
 *
 * Mirrors rocm_ernic_post_recv queue layout expected by the pvrdma
 * device server (ring prod/cons at byte offset 8 of the first page).
 */
int rocm_ernic_post_srq_recv(struct ib_srq *ibsrq, const struct ib_recv_wr *wr,
                             const struct ib_recv_wr **bad_wr)
{
    struct rocm_ernic_dev *dev = to_vdev(ibsrq->device);
    struct rocm_ernic_srq *srq = to_vsrq(ibsrq);
    unsigned long flags;
    struct rocm_ernic_rq_wqe_hdr *wqe_hdr;
    struct rocm_ernic_sge *sge;
    int ret = 0;
    int i;

    if (!srq->rq_ring || !srq->umem) {
        *bad_wr = wr;
        return -EINVAL;
    }

    spin_lock_irqsave(&srq->lock, flags);

    while (wr) {
        unsigned int tail = 0;

        if (unlikely(wr->num_sge > srq->max_gs || wr->num_sge < 0)) {
            ret = -EINVAL;
            *bad_wr = wr;
            goto out;
        }

        if (unlikely(rocm_ernic_idx_ring_has_space(srq->rq_ring, srq->wqe_cnt,
                                                   &tail) <= 0)) {
            ret = -ENOMEM;
            *bad_wr = wr;
            goto out;
        }

        wqe_hdr = (struct rocm_ernic_rq_wqe_hdr *)get_srq_wqe(srq, tail);
        wqe_hdr->wr_id = wr->wr_id;
        wqe_hdr->num_sge = wr->num_sge;
        wqe_hdr->total_len = 0;

        sge = (struct rocm_ernic_sge *)(wqe_hdr + 1);
        for (i = 0; i < wr->num_sge; i++) {
            sge->addr = wr->sg_list[i].addr;
            sge->length = wr->sg_list[i].length;
            sge->lkey = wr->sg_list[i].lkey;
            sge++;
        }

        smp_wmb();
        rocm_ernic_idx_ring_inc(&srq->rq_ring->prod_tail, srq->wqe_cnt);
        wr = wr->next;
    }

out:
    spin_unlock_irqrestore(&srq->lock, flags);

    if (!ret)
        rocm_ernic_write_uar_srq(dev,
                                 ROCM_ERNIC_UAR_SRQ_RECV | srq->srq_handle);

    return ret;
}

/**
 * rocm_ernic_query_srq - query shared receive queue
 * @ibsrq: the shared receive queue to query
 * @srq_attr: attributes to query and return to client
 *
 * @return: 0 for success, otherwise returns an errno.
 */
int rocm_ernic_query_srq(struct ib_srq *ibsrq, struct ib_srq_attr *srq_attr)
{
    struct rocm_ernic_dev *dev = to_vdev(ibsrq->device);
    struct rocm_ernic_srq *srq = to_vsrq(ibsrq);
    union rocm_ernic_cmd_req req;
    union rocm_ernic_cmd_resp rsp;
    struct rocm_ernic_cmd_query_srq *cmd = &req.query_srq;
    struct rocm_ernic_cmd_query_srq_resp *resp = &rsp.query_srq_resp;
    int ret;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.cmd = ROCM_ERNIC_CMD_QUERY_SRQ;
    cmd->srq_handle = srq->srq_handle;

    ret = rocm_ernic_cmd_post(dev, &req, &rsp, ROCM_ERNIC_CMD_QUERY_SRQ_RESP);
    if (ret < 0) {
        dev_warn(&dev->pdev->dev,
                 "could not query shared receive queue, error: %d\n", ret);
        return -EINVAL;
    }

    srq_attr->srq_limit = resp->attrs.srq_limit;
    srq_attr->max_wr = resp->attrs.max_wr;
    srq_attr->max_sge = resp->attrs.max_sge;

    return 0;
}

/**
 * rocm_ernic_create_srq - create shared receive queue
 * @ibsrq: the IB shared receive queue
 * @init_attr: shared receive queue attributes
 * @udata: user data
 *
 * @return: 0 on success, otherwise returns an errno.
 */
int rocm_ernic_create_srq(struct ib_srq *ibsrq,
                          struct ib_srq_init_attr *init_attr,
                          struct ib_udata *udata)
{
    struct rocm_ernic_srq *srq = to_vsrq(ibsrq);
    struct rocm_ernic_dev *dev = to_vdev(ibsrq->device);
    union rocm_ernic_cmd_req req;
    union rocm_ernic_cmd_resp rsp;
    struct rocm_ernic_cmd_create_srq *cmd = &req.create_srq;
    struct rocm_ernic_cmd_create_srq_resp *resp = &rsp.create_srq_resp;
    struct rocm_ernic_create_srq_resp srq_resp = {};
    struct rocm_ernic_create_srq ucmd;
    struct rocm_ernic_ucontext *uctx;
    unsigned long flags;
    u32 wqe_sz;
    u32 wqe_cnt;
    size_t need;
    int ret;

    if (!udata) {
        /* No support for kernel clients. */
        dev_warn(&dev->pdev->dev,
                 "no shared receive queue support for kernel client\n");
        return -EOPNOTSUPP;
    }

    if (init_attr->srq_type != IB_SRQT_BASIC) {
        dev_warn(&dev->pdev->dev,
                 "shared receive queue type %d not supported\n",
                 init_attr->srq_type);
        return -EOPNOTSUPP;
    }

    if (init_attr->attr.max_wr < 1 || init_attr->attr.max_sge < 1 ||
        init_attr->attr.max_wr > dev->dsr->caps.max_srq_wr ||
        init_attr->attr.max_sge > dev->dsr->caps.max_srq_sge) {
        dev_warn(&dev->pdev->dev, "shared receive queue size invalid\n");
        return -EINVAL;
    }

    if (!atomic_add_unless(&dev->num_srqs, 1, dev->dsr->caps.max_srq))
        return -ENOMEM;

    spin_lock_init(&srq->lock);
    refcount_set(&srq->refcnt, 1);
    init_completion(&srq->free);

    dev_dbg(&dev->pdev->dev, "create shared receive queue from user space\n");

    if (ib_copy_from_udata(&ucmd, udata, sizeof(ucmd))) {
        ret = -EFAULT;
        goto err_srq;
    }

    srq->umem = ib_umem_get(ibsrq->device, ucmd.buf_addr, ucmd.buf_size, 0);
    if (IS_ERR(srq->umem)) {
        ret = PTR_ERR(srq->umem);
        goto err_srq;
    }

    srq->npages = ib_umem_num_dma_blocks(srq->umem, PAGE_SIZE);

    if (srq->npages < 0 || srq->npages > ROCM_ERNIC_PAGE_DIR_MAX_PAGES) {
        dev_warn(&dev->pdev->dev, "overflow pages in shared receive queue\n");
        ret = -EINVAL;
        goto err_umem;
    }

    ret = rocm_ernic_page_dir_init(dev, &srq->pdir, srq->npages, false);
    if (ret) {
        dev_warn(&dev->pdev->dev, "could not allocate page directory\n");
        goto err_umem;
    }

    rocm_ernic_page_dir_insert_umem(&srq->pdir, srq->umem, 0);

    wqe_sz = roundup_pow_of_two(sizeof(struct rocm_ernic_rq_wqe_hdr) +
                                (size_t)init_attr->attr.max_sge *
                                    sizeof(struct rocm_ernic_sge));
    wqe_cnt = roundup_pow_of_two(init_attr->attr.max_wr);
    need = (size_t)PAGE_SIZE + (size_t)wqe_cnt * (size_t)wqe_sz;

    if (ucmd.buf_size < need) {
        dev_warn(&dev->pdev->dev,
                 "SRQ user buffer too small (need %zu have %u)\n", need,
                 ucmd.buf_size);
        ret = -EINVAL;
        goto err_page_dir;
    }

    srq->wqe_size = (int)wqe_sz;
    srq->wqe_cnt = (int)wqe_cnt;
    srq->max_gs = (int)init_attr->attr.max_sge;
    srq->offset = PAGE_SIZE;
    /*
     * The SRQ buffer is user memory registered through
     * ib_umem_get, so the page directory is populated from
     * the umem and pdir->pages is never allocated (page_dir_init
     * is called with alloc_pages = false).  Dereferencing it to
     * zero the ring header therefore faults on NULL, and the
     * userspace provider already initialises the same header in
     * its own mapping of this buffer.
     *
     * rq_ring stays NULL: it is only consumed by the kernel
     * post path, and kernel SRQ clients are rejected above, so
     * rocm_ernic_post_srq_recv's !srq->rq_ring guard covers it.
     */

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.cmd = ROCM_ERNIC_CMD_CREATE_SRQ;
    cmd->srq_type = init_attr->srq_type;
    cmd->nchunks = srq->npages;
    cmd->pd_handle = to_vpd(ibsrq->pd)->pd_handle;
    cmd->attrs.max_wr = init_attr->attr.max_wr;
    cmd->attrs.max_sge = init_attr->attr.max_sge;
    cmd->attrs.srq_limit = init_attr->attr.srq_limit;
    cmd->pdir_dma = srq->pdir.dir_dma;

    ret = rocm_ernic_cmd_post(dev, &req, &rsp, ROCM_ERNIC_CMD_CREATE_SRQ_RESP);
    if (ret < 0) {
        dev_warn(&dev->pdev->dev,
                 "could not create shared receive queue, error: %d\n", ret);
        goto err_page_dir;
    }

    srq->srq_handle = resp->srqn;
    srq_resp.srqn = resp->srqn;
    uctx = rdma_udata_to_drv_context(udata, struct rocm_ernic_ucontext,
                                     ibucontext);
    srq_resp.uar_mmap_offset = (u64)uctx->uar.pfn << PAGE_SHIFT;

    spin_lock_irqsave(&dev->srq_tbl_lock, flags);
    dev->srq_tbl[srq->srq_handle % dev->dsr->caps.max_srq] = srq;
    spin_unlock_irqrestore(&dev->srq_tbl_lock, flags);

    if (udata && udata->outlen >= sizeof(srq_resp)) {
        if (ib_copy_to_udata(udata, &srq_resp, sizeof(srq_resp))) {
            dev_warn(&dev->pdev->dev, "failed to copy back udata\n");
            rocm_ernic_destroy_srq(&srq->ibsrq, udata);
            return -EINVAL;
        }
    }

    return 0;

err_page_dir:
    rocm_ernic_page_dir_cleanup(dev, &srq->pdir);
err_umem:
    ib_umem_release(srq->umem);
err_srq:
    atomic_dec(&dev->num_srqs);

    return ret;
}

static void rocm_ernic_free_srq(struct rocm_ernic_dev *dev,
                                struct rocm_ernic_srq *srq)
{
    unsigned long flags;

    spin_lock_irqsave(&dev->srq_tbl_lock, flags);
    dev->srq_tbl[srq->srq_handle % dev->dsr->caps.max_srq] = NULL;
    spin_unlock_irqrestore(&dev->srq_tbl_lock, flags);

    if (refcount_dec_and_test(&srq->refcnt))
        complete(&srq->free);
    wait_for_completion(&srq->free);

    /* There is no support for kernel clients, so this is safe. */
    ib_umem_release(srq->umem);

    rocm_ernic_page_dir_cleanup(dev, &srq->pdir);

    atomic_dec(&dev->num_srqs);
}

/**
 * rocm_ernic_destroy_srq - destroy shared receive queue
 * @srq: the shared receive queue to destroy
 * @udata: user data or null for kernel object
 *
 * @return: 0 for success.
 */
int rocm_ernic_destroy_srq(struct ib_srq *srq, struct ib_udata *udata)
{
    struct rocm_ernic_srq *vsrq = to_vsrq(srq);
    union rocm_ernic_cmd_req req;
    struct rocm_ernic_cmd_destroy_srq *cmd = &req.destroy_srq;
    struct rocm_ernic_dev *dev = to_vdev(srq->device);
    int ret;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.cmd = ROCM_ERNIC_CMD_DESTROY_SRQ;
    cmd->srq_handle = vsrq->srq_handle;

    ret = rocm_ernic_cmd_post(dev, &req, NULL, 0);
    if (ret < 0)
        dev_warn(&dev->pdev->dev,
                 "destroy shared receive queue failed, error: %d\n", ret);

    rocm_ernic_free_srq(dev, vsrq);
    return 0;
}

/**
 * rocm_ernic_modify_srq - modify shared receive queue attributes
 * @ibsrq: the shared receive queue to modify
 * @attr: the shared receive queue's new attributes
 * @attr_mask: attributes mask
 * @udata: user data
 *
 * @returns 0 on success, otherwise returns an errno.
 */
int rocm_ernic_modify_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr,
                          enum ib_srq_attr_mask attr_mask,
                          struct ib_udata *udata)
{
    struct rocm_ernic_srq *vsrq = to_vsrq(ibsrq);
    union rocm_ernic_cmd_req req;
    struct rocm_ernic_cmd_modify_srq *cmd = &req.modify_srq;
    struct rocm_ernic_dev *dev = to_vdev(ibsrq->device);
    int ret;

    /* Only support SRQ limit. */
    if (!(attr_mask & IB_SRQ_LIMIT))
        return -EINVAL;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.cmd = ROCM_ERNIC_CMD_MODIFY_SRQ;
    cmd->srq_handle = vsrq->srq_handle;
    cmd->attrs.srq_limit = attr->srq_limit;
    cmd->attr_mask = attr_mask;

    ret = rocm_ernic_cmd_post(dev, &req, NULL, 0);
    if (ret < 0) {
        dev_warn(&dev->pdev->dev,
                 "could not modify shared receive queue, error: %d\n", ret);

        return -EINVAL;
    }

    return ret;
}
