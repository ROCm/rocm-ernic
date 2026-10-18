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
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ernic_device.h"

#ifndef ERNIC_TEST_FIXTURE_ROOT
#error "ERNIC_TEST_FIXTURE_ROOT must name the fixture directory"
#endif

#define DEVICE_NAME "rdma0"

static const char *const attributes[] = {
    "vendor",
    "device",
    "subsystem_vendor",
    "subsystem_device",
};

static int make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static int write_attr(const char *name, const char *attribute,
                      unsigned int value)
{
    char path[PATH_MAX];
    FILE *file;
    int length;
    int fd;
    int result;

    length = snprintf(path, sizeof(path),
                      ERNIC_SYSFS_INFINIBAND "/%s/device/%s", name, attribute);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;

    /* open() rather than fopen() so the fixture never depends on umask.
     * With _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
     * open(path, flags, mode) overload, and android-cloexec-open looks for
     * O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    fd =
        open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    /* NOLINTEND(android-cloexec-open) */
    if (fd < 0)
        return 0;
    file = fdopen(fd, "w");
    if (!file) {
        (void)close(fd);
        return 0;
    }
    result = fprintf(file, "%#x\n", value) >= 0;
    if (fclose(file) != 0)
        result = 0;

    return result;
}

static void remove_device(const char *name)
{
    char path[PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        if (snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s/device/%s",
                     name, attributes[i]) >= (int)sizeof(path))
            continue;
        (void)unlink(path);
    }

    if (snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s/device",
                 name) < (int)sizeof(path))
        (void)rmdir(path);
    if (snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s", name) <
        (int)sizeof(path))
        (void)rmdir(path);
}

static void remove_fixture(void)
{
    remove_device(DEVICE_NAME);
    (void)rmdir(ERNIC_SYSFS_INFINIBAND);
    (void)rmdir(ERNIC_TEST_FIXTURE_ROOT "/class");
    (void)rmdir(ERNIC_TEST_FIXTURE_ROOT);
}

/* The class directories that every device's fixture sits under. */
static int create_root(void)
{
    return make_dir(ERNIC_TEST_FIXTURE_ROOT) &&
           make_dir(ERNIC_TEST_FIXTURE_ROOT "/class") &&
           make_dir(ERNIC_SYSFS_INFINIBAND);
}

/* A device @name whose attributes are the emulated PCI tuple. */
static int create_device(const char *name)
{
    char path[PATH_MAX];
    int length;

    length = snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s", name);
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;
    length =
        snprintf(path, sizeof(path), ERNIC_SYSFS_INFINIBAND "/%s/device", name);
    if (length < 0 || (size_t)length >= sizeof(path) || !make_dir(path))
        return 0;

    return write_attr(name, "vendor", ERNIC_PCI_VENDOR_ID) &&
           write_attr(name, "device", ERNIC_PCI_DEVICE_ID) &&
           write_attr(name, "subsystem_vendor",
                      ERNIC_PCI_SUBSYSTEM_VENDOR_ID) &&
           write_attr(name, "subsystem_device", ERNIC_PCI_SUBSYSTEM_DEVICE_ID);
}

/* Lengths of device name, each with the emulated tuple when it fits. */
static const struct name_case {
    const char *description;
    size_t name_len;
    int create; /* build a fixture device of this name */
    int expected;
} name_cases[] = {
    {"one-character name", 1, 1, 1},
    {"name longer than the old 192-byte path buffer", 200, 1, 1},
    {"name at NAME_MAX", NAME_MAX, 1, 1},
    {"name that cannot fit in PATH_MAX is rejected, not truncated", PATH_MAX, 0,
     0},
};

static int run_name_cases(void)
{
    static char name[PATH_MAX + 1];
    int failures = 0;
    size_t i;

    for (i = 0; i < sizeof(name_cases) / sizeof(name_cases[0]); i++) {
        const struct name_case *c = &name_cases[i];
        int got;

        memset(name, 'n', c->name_len);
        name[c->name_len] = '\0';
        if (c->create && !create_device(name)) {
            fprintf(stderr, "FAIL %s: could not create the fixture\n",
                    c->description);
            failures++;
            remove_device(name);
            continue;
        }
        got = ernic_device_tuple_matches(name);
        if (got != c->expected) {
            fprintf(stderr, "FAIL %s: matched %d, expected %d\n",
                    c->description, got, c->expected);
            failures++;
        }
        if (c->create)
            remove_device(name);
    }

    return failures == 0;
}

int main(void)
{
    int result = EXIT_FAILURE;

    remove_fixture();
    if (!create_root() || !create_device(DEVICE_NAME)) {
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

    if (!write_attr(DEVICE_NAME, "device", ERNIC_PCI_DEVICE_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong device ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr(DEVICE_NAME, "device", ERNIC_PCI_DEVICE_ID) ||
        !write_attr(DEVICE_NAME, "subsystem_vendor",
                    ERNIC_PCI_SUBSYSTEM_VENDOR_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong subsystem vendor ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr(DEVICE_NAME, "subsystem_vendor",
                    ERNIC_PCI_SUBSYSTEM_VENDOR_ID) ||
        !write_attr(DEVICE_NAME, "subsystem_device",
                    ERNIC_PCI_SUBSYSTEM_DEVICE_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong subsystem device ID was accepted\n");
        goto cleanup;
    }
    if (!write_attr(DEVICE_NAME, "subsystem_device",
                    ERNIC_PCI_SUBSYSTEM_DEVICE_ID) ||
        !write_attr(DEVICE_NAME, "vendor", ERNIC_PCI_VENDOR_ID + 1) ||
        ernic_device_tuple_matches(DEVICE_NAME) ||
        ernic_device_matches(DEVICE_NAME, DEVICE_NAME)) {
        fprintf(stderr, "Wrong vendor ID was accepted\n");
        goto cleanup;
    }

    if (!run_name_cases())
        goto cleanup;

    result = EXIT_SUCCESS;

cleanup:
    remove_fixture();
    return result;
}
