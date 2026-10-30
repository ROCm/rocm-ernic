/*
 * Unit tests for the numbers and state the loopback and TCP backends keep in
 * the resource manager's backend objects.
 *
 * The backends used to store them in fields typed struct ibv_mr *,
 * struct ibv_cq *, struct ibv_qp * and struct ibv_srq *, casting numbers and
 * their own structs in and out.  They now have a number or a void * of their
 * own; these cases create, use and destroy each object through the backends'
 * operations, so a backend that reads a field another wrote shows up as a
 * wrong number here, or as a bad pointer under ASan.
 *
 * Two bugs are covered as well:
 *
 * - The loopback backend took MR lkeys from one unlocked counter shared by
 *   every device in the process, so a second device's first MR didn't get
 *   lkey 1.  Each device now has its own, under its lock.
 * - The TCP backend handed out the next QPN without checking it was free, so
 *   after the counter wrapped it gave out numbers below TCP_FIRST_QPN and
 *   numbers live QPs still had, which the resource manager refuses.  It now
 *   skips both, as the loopback backend does.
 *
 * The test links both backends and reaches the TCP backend's state through
 * rdma_backend_tcp_priv.h, building only the part of it QPs use, so the rest
 * of the backends' externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <infiniband/verbs.h>

#include "pvrdma/rdma_backend_defs.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "rdma/rdma_backend_ops.h"
#include "rdma/rdma_backend_tcp_priv.h"

/* ---- Stubs for the TUs' external symbols -------------------------------
 * Neither backend starts a thread on the paths these cases take; these exist
 * only to satisfy the linker
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
                       void *(*start_routine)(void *), void *arg)
{
    (void)thread;
    (void)name;
    (void)start_routine;
    (void)arg;
    return 0;
}
void qemu_thread_set_name(const char *name)
{
    (void)name;
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

#define CHECK(cond, ...)          \
    do {                          \
        if (!(cond)) {            \
            printf("    FAIL: "); \
            printf(__VA_ARGS__);  \
            printf("\n");         \
            failures++;           \
        }                         \
    } while (0)

static const RdmaBackendOps *const loopback = &rdma_backend_ops_loopback;
static const RdmaBackendOps *const tcp = &rdma_backend_ops_tcp;

/* A loopback device with a PD and a CQ, for its MRs and QPs */
typedef struct {
    RdmaBackendDev dev;
    RdmaBackendPD pd;
    RdmaBackendCQ cq;
} LoopbackDev;

static int loopback_dev_init(LoopbackDev *d)
{
    memset(d, 0, sizeof(*d));
    d->dev.backend_type = RDMA_BACKEND_TYPE_LOOPBACK;
    if (loopback->init(&d->dev, NULL) != 0 ||
        loopback->create_pd(&d->dev, &d->pd) != 0 ||
        loopback->create_cq(&d->dev, &d->cq, 16) != 0) {
        return -1;
    }
    return 0;
}

static void loopback_dev_fini(LoopbackDev *d)
{
    loopback->destroy_cq(&d->cq);
    loopback->fini(&d->dev);
}

/* The lkey of a new MR on @d, or 0 if it can't be created */
static uint32_t loopback_new_mr(LoopbackDev *d)
{
    RdmaBackendMR mr;
    uint32_t lkey;

    memset(&mr, 0, sizeof(mr));
    if (loopback->create_mr(&mr, &d->pd, IBV_ACCESS_LOCAL_WRITE) != 0) {
        return 0;
    }
    lkey = loopback->mr_lkey(&mr);
    loopback->destroy_mr(&mr);
    return lkey;
}

/* A TCP backend with only what its QP operations use */
static TcpBackendPrivate *tcp_priv_new(void)
{
    TcpBackendPrivate *priv = g_new0(TcpBackendPrivate, 1);

    qemu_mutex_init(&priv->lock);
    qemu_mutex_lock(&priv->lock);
    priv->next_qpn = TCP_FIRST_QPN;
    priv->qpns = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_mutex_unlock(&priv->lock);
    return priv;
}

static void tcp_priv_free_qps(TcpBackendPrivate *priv)
{
    qemu_mutex_lock(&priv->lock);
    g_hash_table_destroy(priv->qpns);
    qemu_mutex_unlock(&priv->lock);
    qemu_mutex_destroy(&priv->lock);
    g_free(priv);
}

static void tcp_set_next_qpn(TcpBackendPrivate *priv, uint32_t qpn)
{
    qemu_mutex_lock(&priv->lock);
    priv->next_qpn = qpn;
    qemu_mutex_unlock(&priv->lock);
}

static guint tcp_live_qpns(TcpBackendPrivate *priv)
{
    guint n;

    qemu_mutex_lock(&priv->lock);
    n = g_hash_table_size(priv->qpns);
    qemu_mutex_unlock(&priv->lock);
    return n;
}

/* The QPN of a new RC QP on @cq's device, or 0 if it can't be created */
static uint32_t tcp_new_qp(RdmaBackendQP *qp, RdmaBackendCQ *cq)
{
    memset(qp, 0, sizeof(*qp));
    if (tcp->create_qp(qp, IBV_QPT_RC, NULL, cq, cq, NULL) != 0) {
        return 0;
    }
    return tcp->qpn(qp);
}

/* ---- Cases ------------------------------------------------------------- */

/* Each loopback device numbers its MRs from 1, whatever another has made */
static void test_loopback_mr_lkeys_per_device(void)
{
    LoopbackDev a;
    LoopbackDev b;

    printf("  loopback MR lkeys are per device\n");

    if (loopback_dev_init(&a) != 0 || loopback_dev_init(&b) != 0) {
        CHECK(0, "loopback init, create_pd or create_cq failed");
        return;
    }

    uint32_t a1 = loopback_new_mr(&a);
    uint32_t a2 = loopback_new_mr(&a);
    uint32_t b1 = loopback_new_mr(&b);

    CHECK(a1 == 1 && a2 == 2,
          "device A's lkeys %" PRIu32 ", %" PRIu32 ", expected 1, 2", a1, a2);
    CHECK(b1 == 1, "device B's first lkey %" PRIu32 ", expected 1", b1);

    loopback_dev_fini(&b);
    loopback_dev_fini(&a);
}

/* A loopback QP and SRQ keep their state through every operation */
static void test_loopback_qp_and_srq(void)
{
    LoopbackDev d;
    RdmaBackendQP qp;
    RdmaBackendSRQ srq;
    struct ibv_qp_attr attr;
    struct ibv_srq_attr sattr;

    printf("  loopback QP and SRQ state\n");

    if (loopback_dev_init(&d) != 0) {
        CHECK(0, "loopback init, create_pd or create_cq failed");
        return;
    }

    memset(&qp, 0, sizeof(qp));
    CHECK(loopback->create_qp(&qp, IBV_QPT_RC, &d.pd, &d.cq, &d.cq, NULL) == 0,
          "create_qp failed");
    CHECK(qp.backend_dev == &d.dev, "the QP doesn't name its device");
    CHECK(loopback->qpn(&qp) >= 100, "QPN %" PRIu32 " is a special QP's",
          loopback->qpn(&qp));
    CHECK(loopback->qp_state_init(&d.dev, &qp, IBV_QPT_RC, 0) == 0,
          "qp_state_init failed");
    CHECK(loopback->query_qp(&qp, &attr, 0, NULL) == 0 &&
              attr.qp_state == IBV_QPS_INIT,
          "query_qp doesn't report INIT");
    CHECK(loopback->qp_state_rts(&qp, IBV_QPT_RC, 0, 0, false) == 0,
          "qp_state_rts failed");
    loopback->destroy_qp(&qp, NULL);
    CHECK(qp.priv == NULL, "destroy_qp left the QP's state behind");
    CHECK(loopback->qpn(&qp) == 0, "a destroyed QP still has a QPN");

    memset(&srq, 0, sizeof(srq));
    CHECK(loopback->create_srq(&srq, &d.pd, 64, 2, 8) == 0,
          "create_srq failed");
    memset(&sattr, 0, sizeof(sattr));
    sattr.srq_limit = 16;
    CHECK(loopback->modify_srq(&srq, &sattr, IBV_SRQ_LIMIT) == 0,
          "modify_srq failed");
    memset(&sattr, 0, sizeof(sattr));
    CHECK(loopback->query_srq(&srq, &sattr) == 0 && sattr.max_wr == 64 &&
              sattr.max_sge == 2 && sattr.srq_limit == 16,
          "query_srq returned %u, %u, %u, expected 64, 2, 16", sattr.max_wr,
          sattr.max_sge, sattr.srq_limit);
    loopback->destroy_srq(&srq);
    CHECK(srq.priv == NULL, "destroy_srq left the SRQ's state behind");
    CHECK(loopback->query_srq(&srq, &sattr) != 0,
          "query_srq accepted a destroyed SRQ");

    loopback_dev_fini(&d);
}

/*
 * Past the top of the counter, the TCP backend skips the special QPs' numbers
 * and any a live QP still has, and a destroyed QP's number is free again.
 */
static void test_tcp_qpn_wrap(void)
{
    TcpBackendPrivate *priv = tcp_priv_new();
    RdmaBackendDev dev;
    RdmaBackendCQ cq;
    RdmaBackendQP first;
    RdmaBackendQP top;
    RdmaBackendQP wrapped;
    RdmaBackendQP again;

    printf("  TCP QPNs skip special and live QPs after wrapping\n");

    memset(&dev, 0, sizeof(dev));
    dev.backend_type = RDMA_BACKEND_TYPE_TCP;
    dev.backend_private = priv;
    memset(&cq, 0, sizeof(cq));
    CHECK(tcp->create_cq(&dev, &cq, 16) == 0, "create_cq failed");

    uint32_t n = tcp_new_qp(&first, &cq);
    CHECK(n == TCP_FIRST_QPN, "first QPN %" PRIu32 ", expected %u", n,
          TCP_FIRST_QPN);
    CHECK(first.backend_dev == &dev, "the QP doesn't name its device");

    tcp_set_next_qpn(priv, UINT32_MAX);
    n = tcp_new_qp(&top, &cq);
    CHECK(n == UINT32_MAX, "QPN %" PRIu32 ", expected %" PRIu32, n, UINT32_MAX);
    n = tcp_new_qp(&wrapped, &cq);
    CHECK(n == TCP_FIRST_QPN + 1,
          "QPN after wrapping %" PRIu32 ", expected %u (%u is live)", n,
          TCP_FIRST_QPN + 1, TCP_FIRST_QPN);
    CHECK(tcp_live_qpns(priv) == 3, "%u live QPNs, expected 3",
          tcp_live_qpns(priv));

    tcp->destroy_qp(&first, NULL);
    tcp_set_next_qpn(priv, TCP_FIRST_QPN);
    n = tcp_new_qp(&again, &cq);
    CHECK(n == TCP_FIRST_QPN, "QPN %" PRIu32 ", expected the destroyed QP's %u",
          n, TCP_FIRST_QPN);

    tcp->destroy_qp(&again, NULL);
    tcp->destroy_qp(&wrapped, NULL);
    tcp->destroy_qp(&top, NULL);
    CHECK(tcp_live_qpns(priv) == 0, "%u QPNs left after destroying every QP",
          tcp_live_qpns(priv));

    tcp->destroy_cq(&cq);
    tcp_priv_free_qps(priv);
}

/* A TCP SRQ keeps its state through every operation */
static void test_tcp_srq(void)
{
    RdmaBackendSRQ srq;
    struct ibv_srq_attr sattr;

    printf("  TCP SRQ state\n");

    memset(&srq, 0, sizeof(srq));
    CHECK(tcp->create_srq(&srq, NULL, 32, 4, 2) == 0, "create_srq failed");
    memset(&sattr, 0, sizeof(sattr));
    sattr.srq_limit = 5;
    CHECK(tcp->modify_srq(&srq, &sattr, IBV_SRQ_LIMIT) == 0,
          "modify_srq failed");
    memset(&sattr, 0, sizeof(sattr));
    CHECK(tcp->query_srq(&srq, &sattr) == 0 && sattr.max_wr == 32 &&
              sattr.max_sge == 4 && sattr.srq_limit == 5,
          "query_srq returned %u, %u, %u, expected 32, 4, 5", sattr.max_wr,
          sattr.max_sge, sattr.srq_limit);
    tcp->destroy_srq(&srq);
    CHECK(srq.priv == NULL, "destroy_srq left the SRQ's state behind");
    CHECK(tcp->query_srq(&srq, &sattr) != 0,
          "query_srq accepted a destroyed SRQ");
}

int main(void)
{
    printf("RDMA backend handle unit tests\n");

    test_loopback_mr_lkeys_per_device();
    test_loopback_qp_and_srq();
    test_tcp_qpn_wrap();
    test_tcp_srq();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
