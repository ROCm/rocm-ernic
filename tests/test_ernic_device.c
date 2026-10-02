/*
 * Fixture-backed tests for the shared emulated-device lookup.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ernic_device.h"

#ifndef ERNIC_TEST_FIXTURE_ROOT
#error "ERNIC_TEST_FIXTURE_ROOT must name the fixture directory"
#endif

#define DEVICE_NAME "rdma0"

static int make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static int write_attr(const char *attribute, unsigned int value)
{
    char path[512];
    FILE *file;
    int length;
    int result;

    length = snprintf(path, sizeof(path),
                      ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME "/device/%s",
                      attribute);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;

    file = fopen(path, "w");
    if (!file)
        return 0;
    result = fprintf(file, "%#x\n", value) >= 0;
    if (fclose(file) != 0)
        result = 0;

    return result;
}

static void remove_fixture(void)
{
    static const char *const attributes[] = {
        "vendor",
        "device",
        "subsystem_vendor",
        "subsystem_device",
    };
    char path[512];
    size_t i;

    for (i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        if (snprintf(path, sizeof(path),
                     ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME "/device/%s",
                     attributes[i]) >= (int)sizeof(path))
            continue;
        (void)unlink(path);
    }

    (void)snprintf(path, sizeof(path),
                   ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME "/device");
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), ERNIC_TEST_FIXTURE_ROOT "/class");
    (void)rmdir(path);
    (void)rmdir(ERNIC_TEST_FIXTURE_ROOT);
}

static int create_fixture(void)
{
    char path[512];
    int length;

    if (!make_dir(ERNIC_TEST_FIXTURE_ROOT))
        return 0;
    length = snprintf(path, sizeof(path), ERNIC_TEST_FIXTURE_ROOT "/class");
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;
    length = snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND);
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;
    length =
        snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME);
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;
    length = snprintf(path, sizeof(path),
                      ERNIC_SYSFS_INFINIBAND "/" DEVICE_NAME "/device");
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;

    return write_attr("vendor", ERNIC_PCI_VENDOR_ID) &&
           write_attr("device", ERNIC_PCI_DEVICE_ID) &&
           write_attr("subsystem_vendor", ERNIC_PCI_SUBSYSTEM_VENDOR_ID) &&
           write_attr("subsystem_device", ERNIC_PCI_SUBSYSTEM_DEVICE_ID);
}

int main(void)
{
    int result = EXIT_FAILURE;

    remove_fixture();
    if (!create_fixture()) {
        fprintf(stderr, "Could not create RDMA-device sysfs fixture\n");
        goto cleanup;
    }

    if (!ernic_device_tuple_matches(DEVICE_NAME) ||
        !ernic_device_matches(NULL, DEVICE_NAME)) {
        fprintf(stderr, "Expected the emulated PCI tuple to match\n");
        goto cleanup;
    }
    if (!ernic_device_matches(DEVICE_NAME, DEVICE_NAME) ||
        ernic_device_matches("another-device", DEVICE_NAME)) {
        fprintf(stderr, "Requested-device name matching is incorrect\n");
        goto cleanup;
    }

    if (!write_attr("device", ERNIC_PCI_DEVICE_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong device ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr("device", ERNIC_PCI_DEVICE_ID) ||
        !write_attr("subsystem_vendor", ERNIC_PCI_SUBSYSTEM_VENDOR_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong subsystem vendor ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr("subsystem_vendor", ERNIC_PCI_SUBSYSTEM_VENDOR_ID) ||
        !write_attr("subsystem_device", ERNIC_PCI_SUBSYSTEM_DEVICE_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong subsystem device ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr("subsystem_device", ERNIC_PCI_SUBSYSTEM_DEVICE_ID) ||
        !write_attr("vendor", ERNIC_PCI_VENDOR_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong vendor ID was accepted\n");
        goto cleanup;
    }

    result = EXIT_SUCCESS;

cleanup:
    remove_fixture();
    return result;
}
