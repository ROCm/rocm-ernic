/*
 * Stub for QEMU hw/hw.h
 * Provides VMState macros (we don't use state serialization)
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_HW_H
#define QEMU_HW_H

/* VMState - stubbed, we don't support state save/load */
typedef struct VMStateDescription {
    const char *name;
    /* Stubbed */
} VMStateDescription;

#endif /* QEMU_HW_H */
