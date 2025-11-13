/*
 * RDMA Backend Core - Multi-backend abstraction layer
 *
 * This file implements the core backend abstraction that allows
 * multiple RDMA backends (none, loopback, verbs, etc.) to coexist.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_utils.h"
#include <string.h>

/* Forward declarations of backend implementations */
extern const RdmaBackendOps rdma_backend_ops_none;
extern const RdmaBackendOps rdma_backend_ops_loopback;
/* extern const RdmaBackendOps rdma_backend_ops_verbs; */ /* TODO */

/**
 * Backend registry
 *
 * All available backends are registered here. Backends can be selected
 * at runtime via command-line or configuration.
 */
static const RdmaBackendOps *backend_registry[RDMA_BACKEND_TYPE_MAX] = {
    [RDMA_BACKEND_TYPE_NONE] = &rdma_backend_ops_none,
    [RDMA_BACKEND_TYPE_LOOPBACK] = &rdma_backend_ops_loopback,
    [RDMA_BACKEND_TYPE_VERBS] = NULL, /* TODO: &rdma_backend_ops_verbs */
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
        rdma_error_report("Invalid backend type %d", type);
        return NULL;
    }

    if (!backend_registry[type]) {
        rdma_warn_report("Backend type %d not implemented yet", type);
        return NULL;
    }

    return backend_registry[type];
}

/**
 * rdma_backend_get_type_from_string - Parse backend type from string
 * @backend_str: String like "none", "loopback", "verbs:mlx5_0"
 *
 * Returns: Backend type enum
 */
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str)
{
    if (!backend_str || !strcmp(backend_str, "none")) {
        return RDMA_BACKEND_TYPE_NONE;
    }

    if (!strncmp(backend_str, "loopback", 8)) {
        /* Match "loopback" or "loopback:..." */
        return RDMA_BACKEND_TYPE_LOOPBACK;
    }

    if (!strncmp(backend_str, "verbs:", 6) || !strcmp(backend_str, "verbs")) {
        return RDMA_BACKEND_TYPE_VERBS;
    }

    rdma_warn_report("Unknown backend '%s', using 'none'", backend_str);
    return RDMA_BACKEND_TYPE_NONE;
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
    case RDMA_BACKEND_TYPE_VERBS:
        return "verbs";
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
        rdma_error_report("Backend type %d not available", type);
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
