/*
 * RDMA device: Debug utilities
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
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

#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include "qemu/error-report.h"

#define rdma_error_report(fmt, ...) \
    error_report("%s: " fmt, "rdma", ##__VA_ARGS__)
#define rdma_warn_report(fmt, ...) \
    warn_report("%s: " fmt, "rdma", ##__VA_ARGS__)
#define rdma_info_report(fmt, ...) \
    info_report("%s: " fmt, "rdma", ##__VA_ARGS__)

#endif
