/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * libfabric MSG endpoints (tcp provider) for the rocm-ernic mesh protocol.
 */

#include "ernic_ofi_wire.h"
#include "rdma_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if ERNIC_HAVE_LIBFABRIC

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>

#define ERNIC_OFI_CQ_SZ           4096u
/* Single fi_send per mesh message up to this size (header + payload). */
#define ERNIC_OFI_FRAMED_MAX      (64u * 1024u)
#define ERNIC_OFI_FRAMED_STACK    4096u

struct ErnicOfiListener {
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_pep *pep;
    struct fid_eq *eq;
    struct fi_info *pep_info;
};

struct ErnicOfiWire {
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_ep *ep;
    struct fid_cq *tx_cq;
    struct fid_cq *rx_cq;
    struct fid_eq *eq;
    QemuMutex lock;
    bool lock_inited;
    bool owns_fabric;
};

static void ofi_wire_lock_init(ErnicOfiWire *w)
{
    if (!w || w->lock_inited) {
        return;
    }
    qemu_mutex_init(&w->lock);
    w->lock_inited = true;
}

static void ofi_wire_lock_fini(ErnicOfiWire *w)
{
    if (!w || !w->lock_inited) {
        return;
    }
    qemu_mutex_destroy(&w->lock);
    w->lock_inited = false;
}

static int ofi_open_cqs(struct fid_domain *dom, struct fid_cq **tx_cq,
                        struct fid_cq **rx_cq)
{
    struct fi_cq_attr cq_attr = {
        .size = ERNIC_OFI_CQ_SZ,
        .format = FI_CQ_FORMAT_MSG,
    };
    int ret;

    ret = fi_cq_open(dom, &cq_attr, tx_cq, NULL);
    if (ret) {
        return ret;
    }
    ret = fi_cq_open(dom, &cq_attr, rx_cq, NULL);
    if (ret) {
        fi_close(&(*tx_cq)->fid);
        *tx_cq = NULL;
    }
    return ret;
}

static int ofi_bind_ep(struct fid_ep *ep, struct fid_cq *tx_cq,
                       struct fid_cq *rx_cq, struct fid_eq *eq)
{
    int ret;

    ret = fi_ep_bind(ep, &tx_cq->fid, FI_TRANSMIT);
    if (ret) {
        return ret;
    }
    ret = fi_ep_bind(ep, &rx_cq->fid, FI_RECV);
    if (ret) {
        return ret;
    }
    return fi_ep_bind(ep, &eq->fid, 0);
}

static int cq_drain_err(struct fid_cq *cq, const char *which)
{
    struct fi_cq_err_entry err;
    ssize_t er;

    er = fi_cq_readerr(cq, &err, 0);
    if (er != 1) {
        return -EIO;
    }
    rdma_warn_report("OFI: %s CQ error prov=%d %s", which, err.prov_errno,
                     fi_strerror(err.err));
    return 0;
}

static int cq_wait_tx(struct ErnicOfiWire *w, void *ctx, int timeout_us)
{
    struct fi_cq_msg_entry entry;

    for (;;) {
        int tw = timeout_us < 0 ? -1 : (timeout_us > 100000 ? 100000 : timeout_us);
        ssize_t n = fi_cq_sread(w->tx_cq, &entry, 1, NULL, tw);
        if (n == 1) {
            if (entry.op_context == ctx) {
                return 0;
            }
            continue;
        }
        if (n == -FI_EAGAIN) {
            return -EAGAIN;
        }
        if (n == -FI_EAVAIL) {
            if (cq_drain_err(w->tx_cq, "TX") == 0) {
                continue;
            }
            return -EIO;
        }
        if (n == -FI_ETIMEDOUT) {
            if (timeout_us >= 0) {
                timeout_us -= tw > 0 ? tw : 100000;
                if (timeout_us <= 0) {
                    return -ETIMEDOUT;
                }
                continue;
            }
            return -EAGAIN;
        }
        if (n < 0) {
            rdma_error_report("OFI: fi_cq_sread TX %zd %s", n, fi_strerror((int)-n));
            return (int)-n;
        }
    }
}

static void cq_drain_one(struct fid_cq *cq)
{
    struct fi_cq_msg_entry entry;
    ssize_t n;

    if (!cq) {
        return;
    }
    for (;;) {
        n = fi_cq_read(cq, &entry, 1);
        if (n == 1) {
            continue;
        }
        if (n == -FI_EAGAIN) {
            return;
        }
        if (n < 0) {
            rdma_info_report("OFI: cq_drain: %zd %s", n, fi_strerror((int)-n));
            return;
        }
    }
}

static void cq_drain(struct ErnicOfiWire *w)
{
    if (!w) {
        return;
    }
    cq_drain_one(w->tx_cq);
    cq_drain_one(w->rx_cq);
}

static int cq_wait_rx_len(struct ErnicOfiWire *w, void *ctx, size_t *out_len,
                          size_t expect_len, int timeout_us)
{
    struct fi_cq_msg_entry entry;

    for (;;) {
        int tw = timeout_us < 0 ? -1 : (timeout_us > 100000 ? 100000 : timeout_us);
        ssize_t n = fi_cq_sread(w->rx_cq, &entry, 1, NULL, tw);
        if (n == 1) {
            if (entry.op_context != ctx) {
                continue;
            }
            if (out_len) {
                *out_len = entry.len;
            }
            if (expect_len > 0 && (size_t)entry.len != expect_len) {
                rdma_error_report("OFI: RX len %zu want %zu", (size_t)entry.len,
                                  expect_len);
                return -EIO;
            }
            return 0;
        }
        if (n == -FI_EAGAIN) {
            return -EAGAIN;
        }
        if (n == -FI_EAVAIL) {
            if (cq_drain_err(w->rx_cq, "RX") == 0) {
                continue;
            }
            return -EIO;
        }
        if (n == -FI_ETIMEDOUT) {
            if (timeout_us >= 0) {
                timeout_us -= tw > 0 ? tw : 100000;
                if (timeout_us <= 0) {
                    return -ETIMEDOUT;
                }
                continue;
            }
            return -EAGAIN;
        }
        if (n < 0) {
            rdma_error_report("OFI: fi_cq_sread RX %zd %s", n, fi_strerror((int)-n));
            return (int)-n;
        }
    }
}

static int cq_wait_rx(struct ErnicOfiWire *w, void *ctx, size_t expect_len,
                      int timeout_us)
{
    size_t got = 0;

    return cq_wait_rx_len(w, ctx, expect_len > 0 ? &got : NULL, expect_len,
                          timeout_us);
}

static int eq_wait(struct fid_eq *eq, uint32_t *event, void *buf, size_t blen,
                   int timeout_us)
{
    int timeout_ms;

    if (timeout_us < 0) {
        timeout_ms = -1;
    } else {
        timeout_ms = timeout_us / 1000;
        if (timeout_ms == 0 && timeout_us > 0) {
            timeout_ms = 1;
        }
    }

    ssize_t n = fi_eq_sread(eq, event, buf, blen, timeout_ms, 0);
    if (n < 0) {
        if (n == -FI_EAGAIN || n == -FI_ETIMEDOUT) {
            return -EAGAIN;
        }
        rdma_error_report("OFI: fi_eq_sread %zd %s", n, fi_strerror((int)-n));
        return (int)-n;
    }
    return 0;
}

static int ofi_wire_send_exact_nl(ErnicOfiWire *w, const void *buf, size_t len)
{
    void *ctx;
    int ret;
    int to_us = len > (1u << 20) ? 120000000 : 12000000;

    ctx = (void *)((uintptr_t)buf ^ (uintptr_t)len);
    ret = fi_send(w->ep, buf, len, NULL, 0, ctx);
    if (ret && ret != -FI_EAGAIN) {
        rdma_error_report("OFI: fi_send %d %s", ret, fi_strerror(-ret));
        return -ret;
    }
    return cq_wait_tx(w, ctx, to_us);
}

static int ofi_wire_recv_exact_nl(ErnicOfiWire *w, void *buf, size_t len,
                                  int timeout_us)
{
    void *ctx;
    int ret;

    ctx = (void *)((uintptr_t)buf ^ ((uintptr_t)len << 1u));
    ret = fi_recv(w->ep, buf, len, NULL, 0, ctx);
    if (ret) {
        rdma_error_report("OFI: fi_recv %d %s", ret, fi_strerror(-ret));
        return -ret;
    }
    ret = cq_wait_rx_len(w, ctx, NULL, len, timeout_us);
    if (ret == -ETIMEDOUT) {
        fi_cancel(&w->ep->fid, ctx);
        struct fi_cq_err_entry err;
        (void)fi_cq_readerr(w->rx_cq, &err, 0);
    }
    return ret;
}

static int ofi_wire_recv_chunk_nl(ErnicOfiWire *w, void *buf, size_t buf_len,
                                  size_t *got_len, int timeout_us)
{
    void *ctx;
    int ret;

    if (!got_len || buf_len == 0) {
        return -EINVAL;
    }

    ctx = (void *)((uintptr_t)buf ^ ((uintptr_t)buf_len << 2u));
    ret = fi_recv(w->ep, buf, buf_len, NULL, 0, ctx);
    if (ret) {
        rdma_error_report("OFI: fi_recv %d %s", ret, fi_strerror(-ret));
        return -ret;
    }
    ret = cq_wait_rx_len(w, ctx, got_len, 0, timeout_us);
    if (ret == -ETIMEDOUT) {
        fi_cancel(&w->ep->fid, ctx);
        struct fi_cq_err_entry err;
        (void)fi_cq_readerr(w->rx_cq, &err, 0);
    }
    return ret;
}

int ernic_ofi_wire_send_exact(ErnicOfiWire *w, const void *buf, size_t len)
{
    int ret;

    if (!w || !buf) {
        return -EINVAL;
    }

    qemu_mutex_lock(&w->lock);
    ret = ofi_wire_send_exact_nl(w, buf, len);
    qemu_mutex_unlock(&w->lock);
    return ret;
}

int ernic_ofi_wire_recv_exact(ErnicOfiWire *w, void *buf, size_t len, int timeout_us)
{
    int ret;

    if (!w || !buf) {
        return -EINVAL;
    }

    qemu_mutex_lock(&w->lock);
    ret = ofi_wire_recv_exact_nl(w, buf, len, timeout_us);
    qemu_mutex_unlock(&w->lock);
    return ret;
}

ErnicOfiListener *ernic_ofi_listener_open(uint16_t port)
{
    struct fi_info *hints = fi_allocinfo();
    struct fi_info *fi = NULL;
    ErnicOfiListener *lst = NULL;
    char svc[32];
    int ret;

    if (!hints) {
        return NULL;
    }

    hints->ep_attr->type = FI_EP_MSG;
    hints->caps = FI_MSG;
    hints->addr_format = FI_SOCKADDR_IN;
    /* Provider comes from FI_PROVIDER env (e.g. tcp); do not set prov_name
     * to a string literal — fi_freeinfo(hints) would corrupt the heap. */

    snprintf(svc, sizeof(svc), "%u", port);
    ret = fi_getinfo(fi_version(), NULL, svc, FI_SOURCE | FI_SOCKADDR, hints,
                     &fi);
    fi_freeinfo(hints);
    if (ret) {
        rdma_error_report("OFI: fi_getinfo(listen %u): %d %s", port, ret,
                          fi_strerror(-ret));
        return NULL;
    }

    lst = calloc(1, sizeof(*lst));
    if (!lst) {
        fi_freeinfo(fi);
        return NULL;
    }
    lst->pep_info = fi;

    ret = fi_fabric(fi->fabric_attr, &lst->fabric, NULL);
    if (ret) {
        goto fail;
    }

    struct fi_eq_attr eq_attr = { .size = 256 };
    ret = fi_eq_open(lst->fabric, &eq_attr, &lst->eq, NULL);
    if (ret) {
        goto fail;
    }

    /* Passive EP is bound to the fabric, not the domain (fi_setup). */
    ret = fi_passive_ep(lst->fabric, fi, &lst->pep, NULL);
    if (ret) {
        goto fail;
    }
    ret = fi_pep_bind(lst->pep, &lst->eq->fid, 0);
    if (ret) {
        goto fail;
    }
    if (fi->src_addr && fi->src_addrlen > 0) {
        ret = fi_setname(&lst->pep->fid, fi->src_addr, fi->src_addrlen);
        if (ret) {
            goto fail;
        }
    }
    ret = fi_listen(lst->pep);
    if (ret) {
        goto fail;
    }

    /* Domain is created per accepted connection from cm_entry.info. */
    rdma_info_report("OFI: listening on port %u", port);
    return lst;

fail:
    rdma_error_report("OFI: listener failed: %d %s", ret, fi_strerror(-ret));
    if (lst) {
        if (lst->pep) {
            fi_close(&lst->pep->fid);
        }
        if (lst->eq) {
            fi_close(&lst->eq->fid);
        }
        if (lst->domain) {
            fi_close(&lst->domain->fid);
        }
        if (lst->fabric) {
            fi_close(&lst->fabric->fid);
        }
        if (lst->pep_info) {
            fi_freeinfo(lst->pep_info);
        }
        free(lst);
    } else if (fi) {
        fi_freeinfo(fi);
    }
    return NULL;
}

void ernic_ofi_listener_close(ErnicOfiListener *lst)
{
    if (!lst) {
        return;
    }
    if (lst->pep) {
        fi_close(&lst->pep->fid);
    }
    if (lst->eq) {
        fi_close(&lst->eq->fid);
    }
    if (lst->domain) {
        fi_close(&lst->domain->fid);
    }
    if (lst->fabric) {
        fi_close(&lst->fabric->fid);
    }
    if (lst->pep_info) {
        fi_freeinfo(lst->pep_info);
    }
    free(lst);
}

ErnicOfiWire *ernic_ofi_listener_accept(ErnicOfiListener *lst, int timeout_us)
{
    uint32_t event;
    struct fi_eq_cm_entry cm_entry;
    ErnicOfiWire *w = NULL;
    struct fid_ep *ep = NULL;
    int ret;

    if (!lst) {
        return NULL;
    }

    memset(&cm_entry, 0, sizeof(cm_entry));
    ret = eq_wait(lst->eq, &event, &cm_entry, sizeof(cm_entry), timeout_us);
    if (ret) {
        return NULL;
    }
    if (event != FI_CONNREQ) {
        rdma_error_report("OFI: EQ event %u (want CONNREQ)", event);
        return NULL;
    }

    w = calloc(1, sizeof(*w));
    if (!w) {
        if (cm_entry.info) {
            fi_freeinfo(cm_entry.info);
        }
        return NULL;
    }
    w->fabric = lst->fabric;
    w->eq = lst->eq;
    w->owns_fabric = false;

    ret = fi_domain(lst->fabric, cm_entry.info, &w->domain, NULL);
    if (ret) {
        free(w);
        if (cm_entry.info) {
            fi_freeinfo(cm_entry.info);
        }
        return NULL;
    }

    ret = fi_endpoint(w->domain, cm_entry.info, &ep, NULL);
    if (ret) {
        fi_close(&w->domain->fid);
        free(w);
        if (cm_entry.info) {
            fi_freeinfo(cm_entry.info);
        }
        return NULL;
    }
    w->ep = ep;

    ret = ofi_open_cqs(w->domain, &w->tx_cq, &w->rx_cq);
    if (ret) {
        goto fail_ep;
    }

    ret = ofi_bind_ep(ep, w->tx_cq, w->rx_cq, lst->eq);
    if (ret) {
        goto fail;
    }
    ret = fi_enable(ep);
    if (ret) {
        goto fail;
    }
    ret = fi_accept(ep, NULL, 0);
    if (ret) {
        goto fail;
    }

    memset(&cm_entry, 0, sizeof(cm_entry));
    ret = eq_wait(lst->eq, &event, &cm_entry, sizeof(cm_entry), 60000000);
    if (ret || event != FI_CONNECTED) {
        rdma_error_report("OFI: accept wait ev=%u ret=%d", event, ret);
        goto fail;
    }

    if (cm_entry.info) {
        fi_freeinfo(cm_entry.info);
    }

    cq_drain(w);
    ofi_wire_lock_init(w);

    rdma_info_report("OFI: accepted connection");
    return w;

fail:
    rdma_error_report("OFI: accept failed: %d %s", ret, fi_strerror(-ret));
    if (cm_entry.info) {
        fi_freeinfo(cm_entry.info);
    }
fail_ep:
    if (w) {
        if (w->tx_cq) {
            fi_close(&w->tx_cq->fid);
            w->tx_cq = NULL;
        }
        if (w->rx_cq) {
            fi_close(&w->rx_cq->fid);
            w->rx_cq = NULL;
        }
    }
    if (ep) {
        fi_close(&ep->fid);
    }
    if (w && w->domain) {
        fi_close(&w->domain->fid);
        w->domain = NULL;
    }
    free(w);
    return NULL;
}

ErnicOfiWire *ernic_ofi_wire_connect(const char *host, uint16_t port)
{
    struct fi_info *hints = fi_allocinfo();
    struct fi_info *fi = NULL;
    ErnicOfiWire *w = NULL;
    char svc[32];
    int ret;

    if (!hints) {
        return NULL;
    }

    hints->ep_attr->type = FI_EP_MSG;
    hints->caps = FI_MSG;
    hints->addr_format = FI_SOCKADDR_IN;

    snprintf(svc, sizeof(svc), "%u", port);
    ret = fi_getinfo(fi_version(), host, svc, FI_SOCKADDR, hints, &fi);
    fi_freeinfo(hints);
    if (ret) {
        rdma_error_report("OFI: fi_getinfo(%s:%u): %d %s", host, port, ret,
                          fi_strerror(-ret));
        return NULL;
    }

    w = calloc(1, sizeof(*w));
    if (!w) {
        fi_freeinfo(fi);
        return NULL;
    }
    w->owns_fabric = true;

    ret = fi_fabric(fi->fabric_attr, &w->fabric, NULL);
    if (ret) {
        goto fail;
    }
    ret = fi_domain(w->fabric, fi, &w->domain, NULL);
    if (ret) {
        goto fail;
    }

    struct fi_eq_attr eq_attr = { .size = 256 };
    ret = fi_eq_open(w->fabric, &eq_attr, &w->eq, NULL);
    if (ret) {
        goto fail;
    }

    ret = fi_endpoint(w->domain, fi, &w->ep, NULL);
    if (ret) {
        goto fail;
    }

    ret = ofi_open_cqs(w->domain, &w->tx_cq, &w->rx_cq);
    if (ret) {
        goto fail;
    }

    ret = ofi_bind_ep(w->ep, w->tx_cq, w->rx_cq, w->eq);
    if (ret) {
        goto fail;
    }
    ret = fi_enable(w->ep);
    if (ret) {
        goto fail;
    }

    ret = fi_connect(w->ep, fi->dest_addr, NULL, 0);
    if (ret) {
        goto fail;
    }

    uint32_t event;
    struct fi_eq_cm_entry cm_entry;
    memset(&cm_entry, 0, sizeof(cm_entry));
    ret = eq_wait(w->eq, &event, &cm_entry, sizeof(cm_entry), 60000000);
    if (ret || event != FI_CONNECTED) {
        rdma_error_report("OFI: connect wait ev=%u ret=%d", event, ret);
        goto fail;
    }

    cq_drain(w);
    ofi_wire_lock_init(w);

    rdma_info_report("OFI: connected to %s:%u", host, port);
    fi_freeinfo(fi);
    return w;

fail:
    if (fi) {
        fi_freeinfo(fi);
    }
    ernic_ofi_wire_close(w);
    return NULL;
}

void ernic_ofi_wire_close(ErnicOfiWire *w)
{
    if (!w) {
        return;
    }
    ofi_wire_lock_fini(w);
    if (w->ep) {
        fi_close(&w->ep->fid);
        w->ep = NULL;
    }
    if (w->tx_cq) {
        fi_close(&w->tx_cq->fid);
        w->tx_cq = NULL;
    }
    if (w->rx_cq) {
        fi_close(&w->rx_cq->fid);
        w->rx_cq = NULL;
    }
    if (w->eq && w->owns_fabric) {
        fi_close(&w->eq->fid);
        w->eq = NULL;
    }
    if (w->domain && w->owns_fabric) {
        fi_close(&w->domain->fid);
        w->domain = NULL;
    }
    if (w->fabric && w->owns_fabric) {
        fi_close(&w->fabric->fid);
        w->fabric = NULL;
    }
    free(w);
}

int ernic_ofi_wire_send_framed(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                               const void *payload, size_t payload_len)
{
    int ret;

    if (!w || !hdr_net) {
        return -EINVAL;
    }

    /*
     * FI_MSG delivers one fi_send as one fi_recv.  The mesh protocol
     * uses a fixed header recv then payload recv; hold the wire lock
     * across both sends so no other message interleaves on this EP.
     */
    qemu_mutex_lock(&w->lock);
    ret = ofi_wire_send_exact_nl(w, hdr_net, sizeof(*hdr_net));
    if (!ret && payload_len > 0 && payload) {
        ret = ofi_wire_send_exact_nl(w, payload, payload_len);
    }
    qemu_mutex_unlock(&w->lock);
    return ret;
}

int ernic_ofi_wire_recv_framed(ErnicOfiWire *w, TcpMsgHeader *hdr_net,
                               void **payload_out, int hdr_timeout_us)
{
    uint8_t buf[ERNIC_OFI_FRAMED_MAX];
    uint32_t plen;
    size_t got;
    size_t hdr_sz = sizeof(*hdr_net);
    size_t in_chunk;
    int ret;

    if (!w || !hdr_net || !payload_out) {
        return -EINVAL;
    }
    *payload_out = NULL;

    qemu_mutex_lock(&w->lock);
    ret = ofi_wire_recv_chunk_nl(w, buf, sizeof(buf), &got, hdr_timeout_us);
    if (ret) {
        qemu_mutex_unlock(&w->lock);
        return ret == -ETIMEDOUT ? -EAGAIN : ret;
    }
    if (got < hdr_sz) {
        qemu_mutex_unlock(&w->lock);
        rdma_error_report("OFI: short mesh read %zu < %zu", got, hdr_sz);
        return -EIO;
    }

    memcpy(hdr_net, buf, hdr_sz);
    plen = ntohl(hdr_net->msg_len);
    in_chunk = got > hdr_sz ? got - hdr_sz : 0;

    if (plen > 0) {
        void *payload = malloc(plen);
        if (!payload) {
            qemu_mutex_unlock(&w->lock);
            return -ENOMEM;
        }
        if (in_chunk >= plen) {
            memcpy(payload, buf + hdr_sz, plen);
        } else {
            if (in_chunk > 0) {
                memcpy(payload, buf + hdr_sz, in_chunk);
            }
            ret = ofi_wire_recv_exact_nl(w, (char *)payload + in_chunk,
                                         plen - in_chunk, 600000000);
            if (ret) {
                free(payload);
                qemu_mutex_unlock(&w->lock);
                return ret == -ETIMEDOUT ? -EAGAIN : ret;
            }
        }
        *payload_out = payload;
    } else if (in_chunk > 0) {
        rdma_warn_report("OFI: %zu trailing bytes after zero-length msg",
                         in_chunk);
    }
    qemu_mutex_unlock(&w->lock);
    return 0;
}

void ernic_ofi_wire_free_framed_payload(void *payload)
{
    free(payload);
}

int ernic_ofi_wire_send_eth_nonblock(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                                     const void *payload, size_t payload_len)
{
    return ernic_ofi_wire_send_framed(w, hdr_net, payload, payload_len);
}

#else /* !ERNIC_HAVE_LIBFABRIC */

struct ErnicOfiListener {
    int dummy;
};
struct ErnicOfiWire {
    int dummy;
};

ErnicOfiListener *ernic_ofi_listener_open(uint16_t port)
{
    (void)port;
    return NULL;
}

void ernic_ofi_listener_close(ErnicOfiListener *lst)
{
    free(lst);
}

ErnicOfiWire *ernic_ofi_listener_accept(ErnicOfiListener *lst, int timeout_us)
{
    (void)lst;
    (void)timeout_us;
    return NULL;
}

ErnicOfiWire *ernic_ofi_wire_connect(const char *host, uint16_t port)
{
    (void)host;
    (void)port;
    return NULL;
}

void ernic_ofi_wire_close(ErnicOfiWire *w)
{
    free(w);
}

int ernic_ofi_wire_send_exact(ErnicOfiWire *w, const void *buf, size_t len)
{
    (void)w;
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int ernic_ofi_wire_recv_exact(ErnicOfiWire *w, void *buf, size_t len, int timeout_us)
{
    (void)w;
    (void)buf;
    (void)len;
    (void)timeout_us;
    return -ENOSYS;
}

int ernic_ofi_wire_send_framed(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                               const void *payload, size_t payload_len)
{
    (void)w;
    (void)hdr_net;
    (void)payload;
    (void)payload_len;
    return -ENOSYS;
}

int ernic_ofi_wire_recv_framed(ErnicOfiWire *w, TcpMsgHeader *hdr_net,
                               void **payload_out, int hdr_timeout_us)
{
    (void)w;
    (void)hdr_net;
    (void)payload_out;
    (void)hdr_timeout_us;
    return -ENOSYS;
}

void ernic_ofi_wire_free_framed_payload(void *payload)
{
    free(payload);
}

int ernic_ofi_wire_send_eth_nonblock(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                                     const void *payload, size_t payload_len)
{
    (void)w;
    (void)hdr_net;
    (void)payload;
    (void)payload_len;
    return -EAGAIN;
}

#endif /* ERNIC_HAVE_LIBFABRIC */
