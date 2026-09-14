/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared emulated-device lookup for the libibverbs tests.
 *
 * Devices are identified by PCI vendor ID, not by name.  An RDMA
 * device's name is whatever the guest's udev policy last set it to,
 * and on our guests it is rewritten twice: the kernel registers
 * ionic_%d (or rocm_ernic%d on the deprecated driver),
 * 60-rdma-persistent-naming.rules rewrites that to rocep<bus>s<slot>
 * via rdma_rename, and udev/99-rocm-ernic.rules rewrites it again to
 * rocm-rdma-ernic<n>.  Any list of name patterns is therefore a
 * snapshot of one moment in a three-step rename, and goes stale
 * silently -- a test that stops matching returns 77 and CTest reports
 * the skip as a pass.  0x1022 holds at every step, and is what
 * 99-rocm-ernic.rules itself keys on.
 *
 * When $RDMA_DEVICE is set the caller has already identified the
 * device and its answer wins outright: not finding it is a failure,
 * never a skip, because the caller has asserted it is there.
 */

#ifndef ERNIC_TESTS_DEVICE_H
#define ERNIC_TESTS_DEVICE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <infiniband/verbs.h>

#define ERNIC_PCI_VENDOR_ID 0x1022u

/* Overridable only so the lookup can be pointed at a fixture tree. */
#ifndef ERNIC_SYSFS_INFINIBAND
#define ERNIC_SYSFS_INFINIBAND "/sys/class/infiniband"
#endif

enum ernic_device_result {
    ERNIC_DEVICE_FOUND = 0,
    ERNIC_DEVICE_ABSENT,   /* nothing emulated here; caller may skip */
    ERNIC_DEVICE_MISMATCH, /* $RDMA_DEVICE named a device we cannot see */
};

static inline int ernic_device_vendor_matches(const char *name)
{
    char path[160];
    FILE *attr;
    unsigned int vendor = 0;
    int fields;

    if (!name)
        return 0;
    if (snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s/device/vendor",
                 name) >= (int)sizeof(path))
        return 0;

    attr = fopen(path, "r");
    if (!attr)
        return 0;
    fields = fscanf(attr, "%x", &vendor);
    fclose(attr);

    return fields == 1 && vendor == ERNIC_PCI_VENDOR_ID;
}

static inline const char *ernic_requested_device(void)
{
    const char *requested = getenv("RDMA_DEVICE");

    return (requested && *requested) ? requested : NULL;
}

static inline struct ibv_device *ernic_find_device(struct ibv_device **list,
                                                   int count,
                                                   enum ernic_device_result *result)
{
    const char *requested = ernic_requested_device();

    for (int i = 0; i < count; i++) {
        const char *name;

        if (!list[i])
            continue;
        name = ibv_get_device_name(list[i]);
        if (!name)
            continue;

        if (requested ? strcmp(name, requested) == 0
                      : ernic_device_vendor_matches(name)) {
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
                "No emulated RDMA device (PCI vendor 0x%04x) found - "
                "skipping test\n",
                ERNIC_PCI_VENDOR_ID);
}

#endif /* ERNIC_TESTS_DEVICE_H */
