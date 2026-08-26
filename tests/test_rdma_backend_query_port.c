/*
 * Unit tests for rdma_backend_query_port().
 *
 * Regression coverage for a NULL-pointer dereference: the function guarded
 * the backend-ops path with "if (backend_dev && ...)" but the verbs
 * fallback then dereferenced backend_dev->context unconditionally, so a
 * NULL backend_dev crashed instead of returning an error. The fix returns
 * -EINVAL up front.
 *
 * The backend-ops path is also exercised (via a stub vtable) to confirm the
 * non-NULL path still dispatches correctly and never reaches the verbs
 * fallback.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <infiniband/verbs.h>

#include "rdma_backend_defs.h"
#include "rdma_backend_ops.h"
#include "rdma_backend.h"

/* --- Stubs for symbols referenced by the backend TU but not provided by
 * the linked qemu-stubs, and not exercised on the query_port paths under
 * test. ---------------------------------------------------------------- */
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)dir;
    return NULL;
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;
}

/* --- Stub backend vtable ------------------------------------------------ */
static int g_stub_rc;
static int g_stub_called;

static int stub_query_port(RdmaBackendDev *backend_dev,
                           struct ibv_port_attr *attr)
{
    (void)backend_dev;
    g_stub_called++;
    if (attr) {
        memset(attr, 0, sizeof(*attr));
        attr->state = IBV_PORT_ACTIVE;
    }
    return g_stub_rc;
}

static const RdmaBackendOps stub_ops = {
    .name = "stub",
    .query_port = stub_query_port,
};

int main(void)
{
    int failures = 0;
    struct ibv_port_attr attr;

    /* 1. NULL backend_dev must return an error, not crash. */
    int rc = rdma_backend_query_port(NULL, &attr);
    if (rc != -EINVAL) {
        printf("FAIL null-backend: expected -EINVAL, got %d\n", rc);
        failures++;
    }

    /* 2. Ops path dispatches and returns the backend's success code. */
    RdmaBackendDev dev;
    memset(&dev, 0, sizeof(dev));
    dev.backend_ops = &stub_ops;
    g_stub_rc = 0;
    g_stub_called = 0;
    rc = rdma_backend_query_port(&dev, &attr);
    if (rc != 0 || g_stub_called != 1) {
        printf("FAIL ops-success: rc=%d called=%d\n", rc, g_stub_called);
        failures++;
    }

    /* 3. Ops path propagates a backend error code. */
    g_stub_rc = -13;
    g_stub_called = 0;
    rc = rdma_backend_query_port(&dev, &attr);
    if (rc != -13 || g_stub_called != 1) {
        printf("FAIL ops-error: rc=%d called=%d\n", rc, g_stub_called);
        failures++;
    }

    if (failures) {
        printf("rdma_backend_query_port: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("rdma_backend_query_port: all cases passed\n");
    return 0;
}
