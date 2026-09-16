/*
 * Ethernet RX Injection Implementation
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "eth_rx_inject.h"
#include "from-qemu/hw/rdma/vmw/pvrdma.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "hw/pci/pci.h"
#include "rocm_ernic_internal.h"
#include "ionic_eth_emu.h"
#include <errno.h>
#include <glib.h>

/*
 * The RDMA backends only ever hold a PVRDMADev, but the emulated NIC lives
 * on the server's device object, which hangs off the PCIDevice shim.
 */
static struct ionic_eth_emu *emu_of(PVRDMADev *dev)
{
    rocm_ernic_dev_t *srv;

    if (!dev)
        return NULL;

    srv = (rocm_ernic_dev_t *)dev->parent_obj.vfu_dev;
    return srv ? srv->ionic_emu : NULL;
}

int eth_rx_inject_frame(PVRDMADev *dev, const void *frame_data, size_t len)
{
    struct ionic_eth_emu *emu = emu_of(dev);

    if (!emu)
        return -ENODEV;

    return ionic_eth_emu_queue_rx_frame(emu, frame_data, len);
}

int eth_rx_inject_frame_mesh_blocking(PVRDMADev *dev, const void *frame_data,
                                      size_t len)
{
    struct ionic_eth_emu *emu = emu_of(dev);

    if (!emu)
        return -ENODEV;

    for (;;) {
        int r = ionic_eth_emu_queue_rx_frame(emu, frame_data, len);
        if (r != -ENOSPC)
            return r;
        /* Queue full: yield so the main loop can drain it into the guest. */
        g_usleep(50);
    }
}
