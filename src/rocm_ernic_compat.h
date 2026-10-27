/*
 * Compatibility Bridge: RDMA Device ↔ libvfio-user
 *
 * The interface through which the device front end (the ionic emulation and
 * the server) reaches the RDMA core in src/qemu/, so that the front end
 * doesn't include the core's headers. This header creates and destroys the
 * core and names it with an opaque handle. What the front end does with the
 * handle is in three narrower headers, each included only where it's used:
 *
 *   ernic_rm.h     the resource manager, for the admin queue
 *   ernic_mesh.h   the TCP mesh, for the data path
 *   ernic_stats.h  counters and the stats file
 *
 * rocm_ernic_compat.c implements all four.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_COMPAT_H
#define ROCM_ERNIC_COMPAT_H

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;

/*
 * Opaque handle to RDMA device
 *
 * This hides the QEMU PVRDMADev structure from our code: the struct is
 * incomplete here, and only the files that include pvrdma.h
 * (rocm_ernic_compat.c and src/rdma/pvrdma_stats.c) can see its members.
 * Unlike a void *, the compiler diagnoses any other pointer passed where a
 * handle belongs.
 *
 * The typedef alone would declare the struct, but without a separate
 * declaration IWYU asks this header to include pvrdma.h.
 */
struct PVRDMADev; // IWYU pragma: keep
typedef struct PVRDMADev *pvrdma_handle_t;

/*
 * RDMA Device Management
 */

/**
 * pvrdma_backend_name - Name the backend a --backend string selects
 * @backend_type_str: e.g. "loopback" or "nvmeof:size=1G" (may be NULL,
 *                    meaning "none")
 *
 * The name is everything before the first ':', and must match exactly.
 *
 * Returns: "none", "loopback", "tcp", "nvmeof" or "s3", or NULL if the name
 * is none of them
 */
const char *pvrdma_backend_name(const char *backend_type_str);

/**
 * pvrdma_device_create - Create and initialize RDMA device
 * @dev: Parent rocm_ernic device structure
 * @backend_type_str: Backend and its config, as pvrdma_backend_name() takes
 *
 * Returns: Opaque handle to RDMA device, or NULL on failure, including an
 * unknown backend
 */
pvrdma_handle_t pvrdma_device_create(const rocm_ernic_dev_t *dev,
                                     const char *backend_type_str);

/**
 * pvrdma_device_destroy - Clean up and destroy PVRDMA device
 * @handle: Device handle from pvrdma_device_create()
 */
void pvrdma_device_destroy(pvrdma_handle_t handle);

/**
 * pvrdma_device_realize - Complete device initialization
 * @handle: Device handle
 *
 * Initializes RDMA backend, loads device attributes, etc.
 * Returns: 0 on success, -errno on failure
 */
int pvrdma_device_realize(pvrdma_handle_t handle);

#endif /* ROCM_ERNIC_COMPAT_H */
