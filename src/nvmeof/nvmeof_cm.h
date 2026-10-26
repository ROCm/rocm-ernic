/*
 * nvmeof_cm.h — in-process IB Communication Manager responder
 *
 * The guest's nvme-rdma initiator reaches a target through rdma_cm, which
 * means an IB CM exchange over GSI/QP1 before a single capsule moves.
 * There is no remote node here to run that exchange, so this module is
 * the passive side: it consumes the CM MADs the guest sends and produces
 * the MADs it expects back, terminating REQ/RTU/DREQ locally and telling
 * the caller which guest QP to wire to which controller queue.
 *
 * Like nvmeof_target.c it touches neither guest memory nor the wire: MADs
 * in, MADs out, so the whole handshake is exercised in CTest.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_CM_H
#define NVMEOF_CM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An IB management datagram is always 256 bytes. */
#define IB_MAD_SIZE 256

/* A received UD datagram is preceded by a 40-byte global route header. */
#define IB_GRH_SIZE 40

/*
 * QP numbers handed out for the controller side of each connection.
 * They have to be 24-bit values the guest will accept in MODIFY_QP, and
 * they have to be distinguishable from the guest's own QPs, which the
 * ionic driver allocates from zero upwards.
 */
#define NVMEOF_CM_QPN_BASE 0x00c00000u
#define NVMEOF_CM_QPN_MASK 0x00ff0000u

static inline bool nvmeof_cm_is_target_qpn(uint32_t qpn)
{
    return (qpn & NVMEOF_CM_QPN_MASK) == NVMEOF_CM_QPN_BASE;
}

struct nvmeof_target;
struct nvmeof_cm;

enum nvmeof_cm_action {
    NVMEOF_CM_ACT_NONE = 0,
    NVMEOF_CM_ACT_BIND,        /* REQ accepted: attach guest_qpn to a queue */
    NVMEOF_CM_ACT_ESTABLISHED, /* RTU seen: the connection is usable */
    NVMEOF_CM_ACT_UNBIND,      /* DREQ seen, or REQ rejected after binding */
};

struct nvmeof_cm_result {
    enum nvmeof_cm_action action;
    uint32_t guest_qpn; /* initiator QP from the REQ */
    uint32_t local_qpn; /* controller QP offered in the REP */
    uint32_t start_psn; /* PSN the controller starts sending on */
    uint16_t nvme_qid;  /* 0 for the admin queue */
    size_t rsp_len;     /* bytes written to rsp; 0 means send nothing */
};

struct nvmeof_cm *nvmeof_cm_create(struct nvmeof_target *target,
                                   uint32_t traddr, uint16_t trsvcid);
void nvmeof_cm_destroy(struct nvmeof_cm *cm);

/*
 * Handle one CM MAD sent by the guest on QP1. mad points at the 256-byte
 * datagram with no GRH. rsp must have room for IB_MAD_SIZE bytes.
 *
 * Returns true if the MAD was a CM MAD this responder owns (whether it
 * accepted or rejected it), false if it should be left alone -- a MAD for
 * another management class, or a CM attribute the passive side never
 * receives.
 */
bool nvmeof_cm_handle_mad(struct nvmeof_cm *cm, const void *mad, size_t len,
                          void *rsp, struct nvmeof_cm_result *res);

/* Forget a connection whose guest QP has gone away without a DREQ. */
void nvmeof_cm_drop_qp(struct nvmeof_cm *cm, uint32_t guest_qpn);

/* Number of connections the responder currently tracks. */
unsigned nvmeof_cm_conn_count(const struct nvmeof_cm *cm);

#endif /* NVMEOF_CM_H */
