/*
 * Shared emulated-device lookup for the libibverbs tests.
 *
 * Devices are identified by the emulated PCI tuple, not by name.  An
 * RDMA device's name is whatever the guest's udev policy last set it
 * to, and on our guests it is rewritten twice: the kernel registers
 * ionic_%d, 60-rdma-persistent-naming.rules rewrites that to
 * rocep<bus>s<slot> via rdma_rename, and udev/99-rocm-ernic.rules
 * rewrites it again to rocm-rdma-ernic<n>.  Any list of name patterns
 * is therefore a snapshot of one moment in a three-step rename, and
 * goes stale silently -- a test that stops matching returns 77 and
 * CTest reports the skip as a pass.
 *
 * When $RDMA_DEVICE is set the caller has already identified the
 * device by name; the helper still validates that the named device is
 * the emulated tuple.  Not finding that device is a failure, never a
 * skip, because the caller has asserted it is there.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_TESTS_DEVICE_H
#define ERNIC_TESTS_DEVICE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <infiniband/verbs.h>

#define ERNIC_PCI_VENDOR_ID 0x1dd8u
#define ERNIC_PCI_DEVICE_ID 0x1002u
#define ERNIC_PCI_SUBSYSTEM_VENDOR_ID 0x1dd8u
#define ERNIC_PCI_SUBSYSTEM_DEVICE_ID 0x5400u

/* Overridable only so the lookup can be pointed at a fixture tree. */
#ifndef ERNIC_SYSFS_INFINIBAND
#define ERNIC_SYSFS_INFINIBAND "/sys/class/infiniband"
#endif

enum ernic_device_result {
    ERNIC_DEVICE_FOUND = 0,
    ERNIC_DEVICE_ABSENT,   /* nothing emulated here; caller may skip */
    ERNIC_DEVICE_MISMATCH, /* $RDMA_DEVICE named a device we cannot see */
};

static inline int ernic_read_hex_attr(const char *name, const char *suffix,
                                      unsigned int *value)
{
    char path[192];
    FILE *attr;
    int fields;

    if (!name || !suffix || !value)
        return 0;
    if (snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s/%s", name,
                 suffix) >= (int)sizeof(path))
        return 0;

    attr = fopen(path, "r");
    if (!attr)
        return 0;
    fields = fscanf(attr, "%x", value);
    fclose(attr);

    return fields == 1;
}

static inline int ernic_device_tuple_matches(const char *name)
{
    unsigned int vendor = 0;
    unsigned int device = 0;
    unsigned int subsystem_vendor = 0;
    unsigned int subsystem_device = 0;

    return ernic_read_hex_attr(name, "device/vendor", &vendor) &&
           ernic_read_hex_attr(name, "device/device", &device) &&
           ernic_read_hex_attr(name, "device/subsystem_vendor",
                               &subsystem_vendor) &&
           ernic_read_hex_attr(name, "device/subsystem_device",
                               &subsystem_device) &&
           vendor == ERNIC_PCI_VENDOR_ID &&
           device == ERNIC_PCI_DEVICE_ID &&
           subsystem_vendor == ERNIC_PCI_SUBSYSTEM_VENDOR_ID &&
           subsystem_device == ERNIC_PCI_SUBSYSTEM_DEVICE_ID;
}

static inline int ernic_device_requested_matches(const char *requested,
                                                 const char *name)
{
    if (!requested || !name)
        return 0;

    return strcmp(name, requested) == 0 && ernic_device_tuple_matches(name);
}

static inline int ernic_device_matches(const char *requested, const char *name)
{
    if (requested)
        return ernic_device_requested_matches(requested, name);

    return ernic_device_tuple_matches(name);
}

static inline const char *ernic_requested_device(void)
{
    const char *requested = getenv("RDMA_DEVICE");

    return (requested && *requested) ? requested : NULL;
}

static inline struct ibv_device *ernic_find_device(
    struct ibv_device **list, int count, enum ernic_device_result *result)
{
    const char *requested = ernic_requested_device();

    for (int i = 0; i < count; i++) {
        const char *name;

        if (!list[i])
            continue;
        name = ibv_get_device_name(list[i]);
        if (!name)
            continue;

        if (ernic_device_matches(requested, name)) {
            *result = ERNIC_DEVICE_FOUND;
            return list[i];
        }
    }

    *result = requested ? ERNIC_DEVICE_MISMATCH : ERNIC_DEVICE_ABSENT;
    return NULL;
}

static inline void ernic_report_no_device(enum ernic_device_result result)
{
    if (result == ERNIC_DEVICE_MISMATCH)
        fprintf(stderr,
                "RDMA_DEVICE=%s is not present in this guest - failing "
                "rather than skipping, because the caller asserted it "
                "exists\n",
                ernic_requested_device());
    else
        fprintf(stderr,
                "No emulated RDMA device (PCI %04x:%04x subsystem %04x:%04x) "
                "found - skipping test\n",
                ERNIC_PCI_VENDOR_ID, ERNIC_PCI_DEVICE_ID,
                ERNIC_PCI_SUBSYSTEM_VENDOR_ID, ERNIC_PCI_SUBSYSTEM_DEVICE_ID);
}

#endif /* ERNIC_TESTS_DEVICE_H */
