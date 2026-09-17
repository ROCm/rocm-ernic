/*
 * Stub for QEMU Device Model
 * Minimal definitions for PVRDMA
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_QDEV_CORE_H
#define QEMU_QDEV_CORE_H

#include <stdint.h>

/* Minimal DeviceState - PVRDMA doesn't use it directly */
struct DeviceState {
    /* Stubbed - only here to satisfy struct member requirements */
    int dummy;
};
typedef struct DeviceState DeviceState;

/* Minimal DeviceClass - PVRDMA doesn't use it directly */
struct DeviceClass {
    /* Stubbed - only here to satisfy struct member requirements */
    int dummy;
};
typedef struct DeviceClass DeviceClass;

/* Device macros - stubbed */
#define DEVICE(obj) ((DeviceState *)(obj))

#endif /* QEMU_QDEV_CORE_H */
