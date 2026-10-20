/*
 * QEMU paravirtual RDMA - Generic RDMA backend
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
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

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include <infiniband/verbs.h>

#include "rdma_backend.h"
#include "rdma_backend_ops.h" /* For RdmaBackendOps structure definition */

int rdma_backend_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                            uint32_t max_wr, uint32_t max_sge,
                            uint32_t srq_limit)
{
    int rc;

    if (!pd->backend_ops || !pd->backend_ops->create_srq) {
        return -EINVAL;
    }

    rc = pd->backend_ops->create_srq(srq, pd, max_wr, max_sge, srq_limit);
    if (rc) {
        return rc;
    }
    srq->backend_ops = pd->backend_ops;

    return 0;
}

int rdma_backend_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr)
{
    if (!srq->backend_ops || !srq->backend_ops->query_srq) {
        return -EINVAL;
    }

    return srq->backend_ops->query_srq(srq, srq_attr);
}

int rdma_backend_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                            int srq_attr_mask)
{
    if (!srq->backend_ops || !srq->backend_ops->modify_srq) {
        return -EINVAL;
    }

    return srq->backend_ops->modify_srq(srq, srq_attr, srq_attr_mask);
}

void rdma_backend_destroy_srq(RdmaBackendSRQ *srq)
{
    if (srq->backend_ops && srq->backend_ops->destroy_srq) {
        srq->backend_ops->destroy_srq(srq);
    }
    srq->backend_ops = NULL;
}
