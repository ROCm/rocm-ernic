/*
 * InfiniBand QP types that the userspace verbs headers don't name.
 *
 * QP0 (SMI) and QP1 (GSI) belong to the kernel, so <infiniband/verbs.h> has
 * no QP type for them, but the guest's kernel creates a GSI QP on the
 * device. A QP type here is an enum ibv_qp_type or one of these, which take
 * the kernel's enum ib_qp_type numbers.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_IB_H
#define ERNIC_IB_H

#include <infiniband/verbs.h>

/* Macros, not an enum: GCC's -Wenum-compare rejects the check below
 * between two enum types. */
#define ERNIC_IB_QPT_SMI 0
#define ERNIC_IB_QPT_GSI 1

_Static_assert(ERNIC_IB_QPT_SMI < IBV_QPT_RC && ERNIC_IB_QPT_GSI < IBV_QPT_RC,
               "enum ibv_qp_type numbers its types from IBV_QPT_RC up, "
               "leaving SMI's and GSI's free");

#endif /* ERNIC_IB_H */
