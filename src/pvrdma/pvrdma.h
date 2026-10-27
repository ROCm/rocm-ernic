/*
 * QEMU VMWARE paravirtual RDMA device definitions
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

#ifndef PVRDMA_PVRDMA_H
#define PVRDMA_PVRDMA_H

#include <infiniband/verbs.h>

#include "hw/pci/pci_device.h"

#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_rm_defs.h"

/* The device's statistics are the server's (ernic_stats.h). */
struct PVRDMADev {
    PCIDevice parent_obj;
    struct ibv_device_attr dev_attr;
    char *backend_device_name;
    RdmaBackendDev backend_dev;
    RdmaDeviceResources rdma_dev_res;
};
typedef struct PVRDMADev PVRDMADev;

#endif
