/*
 * Stub for QEMU PCIDevice
 * Minimal definitions to make PVRDMA code compile with libvfio-user
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_PCI_DEVICE_H
#define QEMU_PCI_DEVICE_H

typedef struct vfu_ctx vfu_ctx_t;

/* Minimal PCIDevice structure - only fields we need */
typedef struct PCIDevice {
    vfu_ctx_t *vfu_ctx; /* libvfio-user context */
} PCIDevice;

#endif /* QEMU_PCI_DEVICE_H */
