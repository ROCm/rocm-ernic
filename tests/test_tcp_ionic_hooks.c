/*
 * Unit tests for the ionic mesh hooks in rdma_backend_tcp.c.
 *
 * tcp_backend_set_ionic_recv_cb() and tcp_backend_send_ionic_v() are called
 * through the pvrdma handle whatever the backend is, and they cast
 * backend_private to TcpBackendPrivate.  On the loopback backend that block
 * is the loopback state, which is smaller, so setting the callback wrote
 * past its end; the datapath does that whenever it is destroyed.  The hooks
 * now check the backend type first, as tcp_backend_local_node_id() does.
 *
 * Each row runs a hook against a backend and checks what it returns and
 * that the private block is untouched where it should be.  A non-TCP row
 * uses a private block smaller than TcpBackendPrivate, so ASan catches a
 * write past its end.
 *
 * The functions are reached by #including the translation unit, as
 * test_tcp_gid_resolve.c does, with the same stubs.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rocm-ernic-warnings.h"

/* Pull in the code under test (including its static functions) */
#include "rdma/rdma_backend_tcp.c"

/* ---- Stubs for the TU's external symbols -------------------------------
 * None of these are reachable from the ionic hooks; they exist only to
 * satisfy the linker
 */
void error_report(const char *fmt, ...)
{
    (void)fmt;
}
void warn_report(const char *fmt, ...)
{
    (void)fmt;
}
void info_report(const char *fmt, ...)
{
    (void)fmt;
}
int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    (void)thread;
    (void)name;
    (void)start_routine;
    (void)arg;
    (void)mode;
    return 0;
}
void qemu_thread_exit(void *retval)
{
    (void)retval;
    abort();
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}
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
int pci_dma_sync(PCIDevice *dev, uint64_t guest_addr, uint64_t len)
{
    (void)dev;
    (void)guest_addr;
    (void)len;
    return 0;
}
void rdma_backend_complete_work(enum ibv_wc_status status, uint32_t vendor_err,
                                uint32_t byte_len, uint32_t qp_num,
                                enum ibv_wc_opcode opcode, void *ctx)
{
    (void)status;
    (void)vendor_err;
    (void)byte_len;
    (void)qp_num;
    (void)opcode;
    (void)ctx;
}
RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    (void)dev_res;
    (void)mr_handle;
    return NULL;
}

/* ---- Rows -------------------------------------------------------------- */

static int failures;

/* A private block for a non-TCP backend: smaller than TcpBackendPrivate. */
#define OTHER_PRIV_SIZE 16
#define FILL            0xa5

enum backend {
    BACKEND_TCP,
    BACKEND_LOOPBACK,
    BACKEND_NO_PRIVATE,
    BACKEND_NULL
};
enum hook { HOOK_SET_CB, HOOK_CLEAR_CB, HOOK_SEND };

static void recv_fn(void *opaque, uint32_t src_node, const void *data,
                    size_t len)
{
    (void)opaque;
    (void)src_node;
    (void)data;
    (void)len;
}

struct hook_case {
    const char *category;
    const char *description;
    const char *expected;
    enum backend backend;
    enum hook hook;
    int want_rc;      /* HOOK_SEND only */
    bool want_stored; /* TCP: the callback is registered afterwards */
};

static const struct hook_case cases[] = {
    {"positive", "set the callback on the TCP backend", "fn and opaque stored",
     BACKEND_TCP, HOOK_SET_CB, 0, true},
    {"positive", "clear the callback on the TCP backend", "fn and opaque NULL",
     BACKEND_TCP, HOOK_CLEAR_CB, 0, false},
    {"positive", "send to the local node on the TCP backend",
     "-EINVAL from the local-node rule, past the type check", BACKEND_TCP,
     HOOK_SEND, -EINVAL, false},
    {"negative", "set the callback on the loopback backend",
     "no-op; the loopback private block is untouched", BACKEND_LOOPBACK,
     HOOK_SET_CB, 0, false},
    {"negative", "clear the callback on the loopback backend",
     "no-op; the loopback private block is untouched", BACKEND_LOOPBACK,
     HOOK_CLEAR_CB, 0, false},
    {"negative", "send on the loopback backend",
     "-EINVAL; the loopback private block is untouched", BACKEND_LOOPBACK,
     HOOK_SEND, -EINVAL, false},
    {"corner", "set the callback with no private block", "no-op",
     BACKEND_NO_PRIVATE, HOOK_SET_CB, 0, false},
    {"corner", "send with no private block", "-EINVAL", BACKEND_NO_PRIVATE,
     HOOK_SEND, -EINVAL, false},
    {"corner", "set the callback with a NULL backend", "no-op", BACKEND_NULL,
     HOOK_SET_CB, 0, false},
    {"corner", "send with a NULL backend", "-EINVAL", BACKEND_NULL, HOOK_SEND,
     -EINVAL, false},
};

static void run_case(const struct hook_case *c)
{
    RdmaBackendDev dev;
    RdmaBackendDev *devp = &dev;
    TcpBackendPrivate *tcp = NULL;
    uint8_t *other = NULL;
    static const uint8_t hdr[8] = {1};
    int opaque_target = 0;
    int rc = 0;
    bool ok = true;
    char got[160] = "";

    memset(&dev, 0, sizeof(dev));
    switch (c->backend) {
    case BACKEND_TCP:
        tcp = g_new0(TcpBackendPrivate, 1);
        qemu_mutex_init(&tcp->lock);
        tcp->local_node_id = 1;
        if (c->hook == HOOK_CLEAR_CB) {
            tcp->ionic_recv_fn = recv_fn;
            tcp->ionic_recv_opaque = &opaque_target;
        }
        dev.backend_type = RDMA_BACKEND_TYPE_TCP;
        dev.backend_private = tcp;
        break;
    case BACKEND_LOOPBACK:
        other = g_malloc(OTHER_PRIV_SIZE);
        memset(other, FILL, OTHER_PRIV_SIZE);
        dev.backend_type = RDMA_BACKEND_TYPE_LOOPBACK;
        dev.backend_private = other;
        break;
    case BACKEND_NO_PRIVATE:
        dev.backend_type = RDMA_BACKEND_TYPE_TCP;
        break;
    case BACKEND_NULL:
        devp = NULL;
        break;
    }

    switch (c->hook) {
    case HOOK_SET_CB:
        tcp_backend_set_ionic_recv_cb(devp, recv_fn, &opaque_target);
        break;
    case HOOK_CLEAR_CB:
        tcp_backend_set_ionic_recv_cb(devp, NULL, NULL);
        break;
    case HOOK_SEND:
        rc = tcp_backend_send_ionic_v(devp, 1, hdr, sizeof(hdr), NULL, 0);
        if (rc != c->want_rc) {
            ok = false;
            snprintf(got, sizeof(got), "rc %d (want %d)", rc, c->want_rc);
        }
        break;
    }

    if (tcp && c->hook != HOOK_SEND) {
        bool stored = tcp->ionic_recv_fn == recv_fn &&
                      tcp->ionic_recv_opaque == &opaque_target;
        bool cleared = !tcp->ionic_recv_fn && !tcp->ionic_recv_opaque;

        if (c->want_stored ? !stored : !cleared) {
            ok = false;
            snprintf(got, sizeof(got), "fn %s, opaque %s",
                     tcp->ionic_recv_fn ? "set" : "NULL",
                     tcp->ionic_recv_opaque ? "set" : "NULL");
        }
    }
    for (size_t i = 0; other && i < OTHER_PRIV_SIZE; i++) {
        if (other[i] != FILL) {
            ok = false;
            snprintf(got, sizeof(got), "private byte %zu changed to 0x%02x", i,
                     other[i]);
            break;
        }
    }

    if (ok) {
        printf("ok   %s: %s\n", c->category, c->description);
    } else {
        printf("FAIL %s: %s; expected: %s\n     got: %s\n", c->category,
               c->description, c->expected, got);
        failures++;
    }

    if (tcp) {
        qemu_mutex_destroy(&tcp->lock);
        g_free(tcp);
    }
    g_free(other);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    if (failures)
        printf("tcp_ionic_hooks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
