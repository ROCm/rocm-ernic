/*
 * Stub for QEMU Notifier
 * PVRDMA has a shutdown_notifier field that we don't use
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_NOTIFY_H
#define QEMU_NOTIFY_H

typedef struct Notifier {
    void (*notify)(struct Notifier *notifier, void *data);
    /* Stubbed - we don't use notifiers */
} Notifier;

typedef void NotifierWithReturnFunc(struct Notifier *notifier, void *data,
                                    int *ret);

#endif /* QEMU_NOTIFY_H */
