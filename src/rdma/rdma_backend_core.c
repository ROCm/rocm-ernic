/*
 * RDMA Backend Core - Multi-backend abstraction layer
 *
 * This file implements the core backend abstraction that allows
 * multiple RDMA backends (none, loopback, tcp, etc.) to coexist.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include <rdma_backend.h> /* declares rdma_backend_{init,fini}_with_ops */
#include "rdma_utils.h"
#include <errno.h>
#include <string.h>

/**
 * Backend registry
 *
 * All available backends are registered here. Backends can be selected
 * at runtime via command-line or configuration.
 */
static const RdmaBackendOps *backend_registry[RDMA_BACKEND_TYPE_MAX] = {
    [RDMA_BACKEND_TYPE_NONE] = &rdma_backend_ops_none,
    [RDMA_BACKEND_TYPE_LOOPBACK] = &rdma_backend_ops_loopback,
    [RDMA_BACKEND_TYPE_TCP] = &rdma_backend_ops_tcp,
    /*
     * The NVMe-oF controller is not a transport: it terminates the guest's
     * queue pairs inside this process, on top of the same local resource
     * management the loopback backend uses.
     */
    [RDMA_BACKEND_TYPE_NVMEOF] = &rdma_backend_ops_loopback,
    /* Nor is the S3 object store, for the same reason. */
    [RDMA_BACKEND_TYPE_S3] = &rdma_backend_ops_loopback,
};

/**
 * rdma_backend_get_ops - Get operations for a backend type
 * @type: Backend type
 *
 * Returns: Backend operations structure, or NULL if not available
 */
const RdmaBackendOps *rdma_backend_get_ops(RdmaBackendType type)
{
    if (type >= RDMA_BACKEND_TYPE_MAX) {
        rdma_error_report("Invalid backend type %u", type);
        return NULL;
    }

    if (!backend_registry[type]) {
        rdma_warn_report("Backend type %u not implemented yet", type);
        return NULL;
    }

    return backend_registry[type];
}

/**
 * rdma_backend_get_type_from_string - Parse backend type from string
 * @backend_str: String like "none", "loopback", "nvmeof:size=1G", or NULL,
 *               which means "none"
 *
 * The name is everything before the first ':', and must be one of the
 * names rdma_backend_type_to_string() returns, exactly: "s3junk" is not
 * "s3".  The server reaches it through pvrdma_backend_name(), so the server
 * and the device can't disagree about which backend a string names.
 *
 * Returns: Backend type enum, or RDMA_BACKEND_TYPE_MAX if the name is
 * unknown
 */
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str)
{
    if (!backend_str) {
        return RDMA_BACKEND_TYPE_NONE;
    }

    size_t len = strcspn(backend_str, ":");

    for (RdmaBackendType type = 0; type < RDMA_BACKEND_TYPE_MAX; type++) {
        const char *name = rdma_backend_type_to_string(type);

        if (strlen(name) == len && !strncmp(backend_str, name, len)) {
            return type;
        }
    }
    return RDMA_BACKEND_TYPE_MAX;
}

/**
 * rdma_backend_type_to_string - Convert backend type to string
 * @type: Backend type
 *
 * Returns: Human-readable backend name
 */
const char *rdma_backend_type_to_string(RdmaBackendType type)
{
    switch (type) {
    case RDMA_BACKEND_TYPE_NONE:
        return "none";
    case RDMA_BACKEND_TYPE_LOOPBACK:
        return "loopback";
    case RDMA_BACKEND_TYPE_TCP:
        return "tcp";
    case RDMA_BACKEND_TYPE_NVMEOF:
        return "nvmeof";
    case RDMA_BACKEND_TYPE_S3:
        return "s3";
    case RDMA_BACKEND_TYPE_MAX:
    default:
        return "unknown";
    }
}

/**
 * rdma_backend_init_with_ops - Initialize backend with specific ops
 * @backend_dev: Backend device structure
 * @type: Backend type to use
 * @config: Backend-specific configuration string (can be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 */
int rdma_backend_init_with_ops(RdmaBackendDev *backend_dev,
                               RdmaBackendType type, const char *config)
{
    const RdmaBackendOps *ops;
    int ret;

    ops = rdma_backend_get_ops(type);
    if (!ops) {
        rdma_error_report("Backend type %u not available", type);
        return -ENOTSUP;
    }

    rdma_info_report("Initializing RDMA backend: %s", ops->name);

    backend_dev->backend_type = type;
    backend_dev->backend_ops = ops;
    backend_dev->backend_private = NULL;

    if (ops->init) {
        ret = ops->init(backend_dev, config);
        if (ret) {
            rdma_error_report("Backend %s init failed: %d", ops->name, ret);
            backend_dev->backend_ops = NULL;
            return ret;
        }
    }

    rdma_info_report("Backend %s initialized successfully", ops->name);
    return 0;
}

/**
 * rdma_backend_fini_with_ops - Cleanup backend
 * @backend_dev: Backend device structure
 */
void rdma_backend_fini_with_ops(RdmaBackendDev *backend_dev)
{
    if (!backend_dev->backend_ops) {
        return;
    }

    rdma_info_report("Finalizing backend: %s", backend_dev->backend_ops->name);

    if (backend_dev->backend_ops->fini) {
        backend_dev->backend_ops->fini(backend_dev);
    }

    backend_dev->backend_ops = NULL;
    backend_dev->backend_private = NULL;
}
