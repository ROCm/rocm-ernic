/*
 *  RDMA device: Definitions of Backend Device functions
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

#ifndef RDMA_BACKEND_H
#define RDMA_BACKEND_H

#include <stdint.h>

#include "pvrdma/rdma_backend_defs.h"

struct ibv_srq_attr;

/* Multi-backend abstraction functions */
const RdmaBackendOps *rdma_backend_get_ops(RdmaBackendType type);
/* RDMA_BACKEND_TYPE_MAX for an unknown name; see rdma_backend_core.c */
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str);
const char *rdma_backend_type_to_string(RdmaBackendType type);
int rdma_backend_init_with_ops(RdmaBackendDev *backend_dev,
                               RdmaBackendType type, const char *config);
void rdma_backend_fini_with_ops(RdmaBackendDev *backend_dev);

int rdma_backend_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                            uint32_t max_wr, uint32_t max_sge,
                            uint32_t srq_limit);
int rdma_backend_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr);
int rdma_backend_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                            int srq_attr_mask);
void rdma_backend_destroy_srq(RdmaBackendSRQ *srq);

#endif
