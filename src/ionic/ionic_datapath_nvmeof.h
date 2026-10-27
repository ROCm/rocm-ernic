/*
 * ionic_datapath_nvmeof.h — attach the NVMe-oF target to the data path
 *
 * ionic_datapath_nvmeof.c implements these.  ionic_device.c calls them when
 * --backend names nvmeof; the rest of the device doesn't need them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_NVMEOF_H
#define IONIC_DATAPATH_NVMEOF_H

#include <stdbool.h>
#include <stddef.h>

#include "ernic_limits.h"

struct ionic_datapath;
struct nvmeof_target_cfg;

/*
 * Memory-region budget.
 *
 * A stock nvme-rdma initiator runs with register_always, so it pre-allocates
 * a pool of IONIC_MR_PER_QUEUE regions for every queue it creates -- admin
 * queue included -- at connect time rather than per I/O.  An N-queue
 * controller therefore needs (N + 1) * IONIC_MR_PER_QUEUE regions to be
 * reachable before the first capsule moves.
 *
 * The device holds ERNIC_MAX_MR (ernic_limits.h), which is not sized for
 * the maximum 64 queues: dp_reg_mr() walks the whole table on every fast
 * registration -- that is, on every NVMe command -- so the limit is also a
 * per-I/O cost, and it covers the default 8 queues with headroom without
 * doubling that walk again. ionic_datapath_nvmeof_cfg_check() rejects a
 * queue count that would not fit.
 */
#define IONIC_MR_PER_QUEUE 128

/* Largest max_queues that fits the budget, admin queue included. */
#define IONIC_MAX_MR_QUEUES ((int)(ERNIC_MAX_MR / IONIC_MR_PER_QUEUE) - 1)

/*
 * Attach the in-process NVMe-oF controller.  Once attached the data path
 * answers IB CM MADs on the guest's GSI QP and serves NVMe command capsules
 * sent to the QP numbers that handshake hands out, so a guest running stock
 * `nvme connect -t rdma` reaches a target without a second node.
 *
 * Returns false and fills @err on failure, including for a configuration
 * ionic_datapath_nvmeof_cfg_check() refuses.  The data path owns the
 * controller from then on and tears it down in ionic_datapath_destroy().
 */
bool ionic_datapath_attach_nvmeof(struct ionic_datapath *dp,
                                  const struct nvmeof_target_cfg *cfg,
                                  char *err, size_t errlen);

/*
 * Whether this device can serve the NVMe-oF configuration @cfg: no more
 * queues than IONIC_MAX_MR_QUEUES, since a host registers
 * IONIC_MR_PER_QUEUE memory regions for each.  Returns false and fills @err
 * if not.  For checking the options at startup, before a guest attaches.
 */
bool ionic_datapath_nvmeof_cfg_check(const struct nvmeof_target_cfg *cfg,
                                     char *err, size_t errlen);

#endif /* IONIC_DATAPATH_NVMEOF_H */
