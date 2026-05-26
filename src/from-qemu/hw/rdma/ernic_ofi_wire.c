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

#define ERNIC_OFI_CQ_SZ 4096u

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
    struct fid_cq *cq;
    struct fid_eq *eq;
    bool owns_fabric;
};

static int cq_wait_tx(struct ErnicOfiWire *w, void *ctx, int timeout_us)
{
    struct fi_cq_msg_entry entry;

    for (;;) {
        int tw = timeout_us < 0 ? -1 : (timeout_us > 100000 ? 100000 : timeout_us);
        ssize_t n = fi_cq_sread(w->cq, &entry, 1, NULL, tw);
        if (n == 1) {
            if (entry.op_context == ctx) {
                return 0;
            }
            rdma_error_report("OFI: unexpected TX CQ context");
            return -EIO;
        }
        if (n == -FI_EAGAIN) {
            return -EAGAIN;
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

static int cq_wait_rx(struct ErnicOfiWire *w, void *ctx, size_t expect_len,
                      int timeout_us)
{
    struct fi_cq_msg_entry entry;

    for (;;) {
        int tw = timeout_us < 0 ? -1 : (timeout_us > 100000 ? 100000 : timeout_us);
        ssize_t n = fi_cq_sread(w->cq, &entry, 1, NULL, tw);
        if (n == 1) {
            if (entry.op_context != ctx) {
                rdma_error_report("OFI: unexpected RX CQ context");
                return -EIO;
            }
            if ((size_t)entry.len != expect_len) {
                rdma_error_report("OFI: RX len %zu want %zu", (size_t)entry.len,
                                  expect_len);
                return -EIO;
            }
            return 0;
        }
        if (n == -FI_EAGAIN) {
            return -EAGAIN;
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

static int eq_wait(struct fid_eq *eq, uint32_t *event, void *buf, size_t blen,
                   int timeout_us)
{
    ssize_t n = fi_eq_sread(eq, event, buf, blen, timeout_us);
    if (n < 0) {
        if (n == -FI_EAGAIN || n == -FI_ETIMEDOUT) {
            return -EAGAIN;
        }
        rdma_error_report("OFI: fi_eq_sread %zd %s", n, fi_strerror((int)-n));
        return (int)-n;
    }
    return 0;
}

int ernic_ofi_wire_send_exact(ErnicOfiWire *w, const void *buf, size_t len)
{
    void *ctx;
    int ret;
    int to_us = len > (1u << 20) ? 120000000 : 12000000;

    if (!w || !buf) {
        return -EINVAL;
    }

    ctx = (void *)((uintptr_t)buf ^ (uintptr_t)len);
    ret = fi_send(w->ep, buf, len, NULL, 0, ctx);
    if (ret && ret != -FI_EAGAIN) {
        rdma_error_report("OFI: fi_send %d %s", ret, fi_strerror(-ret));
        return -ret;
    }
    ret = cq_wait_tx(w, ctx, to_us);
    return ret;
}

int ernic_ofi_wire_recv_exact(ErnicOfiWire *w, void *buf, size_t len, int timeout_us)
{
    void *ctx;
    int ret;

    if (!w || !buf) {
        return -EINVAL;
    }

    ctx = (void *)((uintptr_t)buf ^ ((uintptr_t)len << 1u));
    ret = fi_recv(w->ep, buf, len, NULL, 0, ctx);
    if (ret) {
        rdma_error_report("OFI: fi_recv %d %s", ret, fi_strerror(-ret));
        return -ret;
    }
    ret = cq_wait_rx(w, ctx, len, timeout_us);
    if (ret == -ETIMEDOUT) {
        fi_cancel(&w->ep->fid, ctx);
        struct fi_cq_err_entry err;
        (void)fi_cq_readerr(w->cq, &err, 0);
    }
    return ret;
}

ErnicOfiListener *ernic_ofi_listener_open(uint16_t port)
{
    struct fi_info *hints = fi_allocinfo();
    struct fi_info *fi = NULL;
    ErnicOfiListener *lst;
    char svc[32];
    int ret;

    if (!hints) {
        return NULL;
    }

    hints->ep_attr->type = FI_EP_MSG;
    hints->caps = FI_MSG;
    hints->addr_format = FI_SOCKADDR_IN;

    snprintf(svc, sizeof(svc), "%u", port);
    ret = fi_getinfo(fi_version(), NULL, svc, FI_SOURCE, hints, &fi);
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
    ret = fi_domain(lst->fabric, fi, &lst->domain, NULL);
    if (ret) {
        goto fail;
    }

    struct fi_eq_attr eq_attr = { .size = 256 };
    ret = fi_eq_open(lst->fabric, &eq_attr, &lst->eq, NULL);
    if (ret) {
        goto fail;
    }

    ret = fi_passive_ep(lst->domain, fi, &lst->pep, NULL);
    if (ret) {
        goto fail;
    }
    ret = fi_pep_bind(lst->pep, &lst->eq->fid, 0);
    if (ret) {
        goto fail;
    }
    ret = fi_listen(lst->pep);
    if (ret) {
        goto fail;
    }

    rdma_info_report("OFI: listening on port %u", port);
    return lst;

fail:
    rdma_error_report("OFI: listener failed: %d %s", ret, fi_strerror(-ret));
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
    fi_freeinfo(fi);
    free(lst);
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
    struct fid_cq *cq = NULL;
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
        return NULL;
    }
    w->fabric = lst->fabric;
    w->domain = lst->domain;
    w->eq = lst->eq;
    w->owns_fabric = false;

    ret = fi_endpoint(lst->domain, cm_entry.info, &ep, NULL);
    if (ret) {
        free(w);
        return NULL;
    }
    w->ep = ep;

    struct fi_cq_attr cq_attr = { .size = ERNIC_OFI_CQ_SZ };
    ret = fi_cq_open(lst->domain, &cq_attr, &cq, NULL);
    if (ret) {
        goto fail_ep;
    }
    w->cq = cq;

    ret = fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV);
    if (ret) {
        goto fail;
    }
    ret = fi_ep_bind(ep, &lst->eq->fid, 0);
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

    rdma_info_report("OFI: accepted connection");
    return w;

fail:
    rdma_error_report("OFI: accept failed: %d %s", ret, fi_strerror(-ret));
fail_ep:
    if (cq) {
        fi_close(&cq->fid);
    }
    if (ep) {
        fi_close(&ep->fid);
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
    ret = fi_getinfo(fi_version(), host, svc, 0, hints, &fi);
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

    struct fi_cq_attr cq_attr = { .size = ERNIC_OFI_CQ_SZ };
    ret = fi_cq_open(w->domain, &cq_attr, &w->cq, NULL);
    if (ret) {
        goto fail;
    }

    ret = fi_ep_bind(w->ep, &w->cq->fid, FI_TRANSMIT | FI_RECV);
    if (ret) {
        goto fail;
    }
    ret = fi_ep_bind(w->ep, &w->eq->fid, 0);
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

    fi_freeinfo(fi);
    fi = NULL;
    rdma_info_report("OFI: connected to %s:%u", host, port);
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
    if (w->ep) {
        fi_close(&w->ep->fid);
        w->ep = NULL;
    }
    if (w->cq) {
        fi_close(&w->cq->fid);
        w->cq = NULL;
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

    ret = ernic_ofi_wire_send_exact(w, hdr_net, sizeof(*hdr_net));
    if (ret) {
        return ret;
    }
    if (payload_len > 0 && payload) {
        ret = ernic_ofi_wire_send_exact(w, payload, payload_len);
    }
    return ret;
}

int ernic_ofi_wire_send_eth_nonblock(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                                     const void *payload, size_t payload_len)
{
    uint8_t buf[sizeof(TcpMsgHeader) + 2048];
    void *ctx = (void *)((uintptr_t)buf ^ 0xfeedu);
    int ret;

    if (!w || !hdr_net || payload_len > 2048u) {
        return -EINVAL;
    }

    memcpy(buf, hdr_net, sizeof(TcpMsgHeader));
    memcpy(buf + sizeof(TcpMsgHeader), payload, payload_len);

    ret = fi_send(w->ep, buf, sizeof(TcpMsgHeader) + payload_len, NULL, 0, ctx);
    if (ret == -FI_EAGAIN) {
        return -EAGAIN;
    }
    if (ret) {
        return -ret;
    }
    ret = cq_wait_tx(w, ctx, 2000);
    return ret ? -EAGAIN : 0;
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
