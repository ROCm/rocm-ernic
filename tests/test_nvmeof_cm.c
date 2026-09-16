/*
 * test_nvmeof_cm.c — unit tests for the in-process IB CM responder
 *
 * Builds the CM MADs a guest running rdma_cm on behalf of nvme-rdma
 * actually emits -- REQ with a cma_hdr and an nvme_rdma_cm_req stacked in
 * its private data, then RTU, then DREQ -- and checks the responder's
 * replies field by field. Getting a field wrong here does not make the
 * guest fail: it makes "nvme connect" hang until it times out, which is
 * why the REP is dissected rather than just checked for success.
 *
 * The offsets are written out independently of src/nvmeof_cm.c so that a
 * typo in one does not cancel out against a matching typo in the other.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvmeof_cm.h"
#include "nvmeof_target.h"

static int failures;

static void fail(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("FAIL %-24s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures++;
}

static void check(const char *name, bool cond, const char *detail)
{
    if (!cond) {
        fail(name, "%s", detail);
    }
}

/* ------------------------------------------------------------------ */
/* Wire layout, spelled out again on purpose                          */
/* ------------------------------------------------------------------ */

#define MAD_CLASS   1
#define MAD_METHOD  3
#define MAD_ATTR_ID 16
#define MAD_TID     8

#define ATTR_REQ  0x0010
#define ATTR_REJ  0x0012
#define ATTR_REP  0x0013
#define ATTR_RTU  0x0014
#define ATTR_DREQ 0x0015
#define ATTR_DREP 0x0016

#define REQ_LOCAL_COMM_ID 24
#define REQ_SERVICE_ID    32
#define REQ_LOCAL_CA_GUID 40
#define REQ_LOCAL_QPN     56
#define REQ_LOCAL_EECN    60
#define REQ_REMOTE_EECN   64
#define REQ_STARTING_PSN  68
#define REQ_PRIVATE       164

#define REP_LOCAL_COMM_ID  24
#define REP_REMOTE_COMM_ID 28
#define REP_LOCAL_QPN      36
#define REP_STARTING_PSN   44
#define REP_PRIVATE        60

#define RTU_LOCAL_COMM_ID  24
#define RTU_REMOTE_COMM_ID 28

#define DREQ_LOCAL_COMM_ID  24
#define DREQ_REMOTE_COMM_ID 28
#define DREQ_REMOTE_QPN     32

#define REJ_REMOTE_COMM_ID 28
#define REJ_MSG_REJECTED   32
#define REJ_REASON         34
#define REJ_PRIVATE        108

#define TEST_TRADDR  0xc0a8c801u /* 192.168.200.1 */
#define TEST_PORT    4420
#define TEST_SERVICE (((uint64_t)0x0106 << 16) | TEST_PORT)

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void mad_hdr(uint8_t *m, uint16_t attr, uint64_t tid)
{
    memset(m, 0, IB_MAD_SIZE);
    m[0] = 1; /* base version */
    m[MAD_CLASS] = 0x07;
    m[2] = 2; /* class version */
    m[MAD_METHOD] = 3;
    put_be64(m + MAD_TID, tid);
    put_be16(m + MAD_ATTR_ID, attr);
}

struct req_params {
    uint32_t comm_id;
    uint32_t qpn;
    uint64_t service_id;
    uint32_t dst_ip;
    uint8_t cma_version;
    uint8_t ip_version;
    uint16_t recfmt;
    uint16_t qid;
    uint16_t hrqsize;
    uint16_t hsqsize;
    uint8_t transport_service; /* 0 = RC */
};

static void req_defaults(struct req_params *p)
{
    memset(p, 0, sizeof(*p));
    p->comm_id = 0x11223344;
    p->qpn = 0x000042;
    p->service_id = TEST_SERVICE;
    p->dst_ip = TEST_TRADDR;
    p->cma_version = 0;
    p->ip_version = 4;
    p->recfmt = 0;
    p->qid = 0;
    p->hrqsize = 32;
    p->hsqsize = 31;
}

static void build_req(uint8_t *m, const struct req_params *p)
{
    mad_hdr(m, ATTR_REQ, 0xdeadbeefcafe0001ull);
    put_be32(m + REQ_LOCAL_COMM_ID, p->comm_id);
    put_be64(m + REQ_SERVICE_ID, p->service_id);
    put_be64(m + REQ_LOCAL_CA_GUID, 0x0002c9030012abcdull);
    put_be32(m + REQ_LOCAL_QPN, (p->qpn << 8) | 4 /* responder resources */);
    put_be32(m + REQ_LOCAL_EECN, 4 /* initiator depth */);
    put_be32(m + REQ_REMOTE_EECN,
             (uint32_t)((p->transport_service & 0x3) << 1) | (20u << 3));
    put_be32(m + REQ_STARTING_PSN, 0x123456 << 8);

    uint8_t *priv = m + REQ_PRIVATE;
    priv[0] = p->cma_version;
    priv[1] = (uint8_t)(p->ip_version << 4);
    put_be16(priv + 2, TEST_PORT);
    put_be32(priv + 4 + 12, 0xc0a8c814u); /* src 192.168.200.20 */
    put_be32(priv + 20 + 12, p->dst_ip);

    uint8_t *nvme = priv + 36;
    put_le16(nvme + 0, p->recfmt);
    put_le16(nvme + 2, p->qid);
    put_le16(nvme + 4, p->hrqsize);
    put_le16(nvme + 6, p->hsqsize);
}

/* ------------------------------------------------------------------ */

static struct nvmeof_target *make_target(void)
{
    struct nvmeof_target_cfg cfg;
    char err[128] = "";

    nvmeof_target_cfg_defaults(&cfg);
    cfg.size = 1u << 20;
    struct nvmeof_target *t = nvmeof_target_create(&cfg, err, sizeof(err));
    if (t == NULL) {
        fail("setup", "target create: %s", err);
    }
    return t;
}

static void test_full_handshake(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    req_defaults(&p);
    build_req(req, &p);
    check("handshake", nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res),
          "REQ was not claimed by the responder");
    check("handshake", res.rsp_len == IB_MAD_SIZE, "REP was not produced");
    check("handshake", res.action == NVMEOF_CM_ACT_BIND,
          "REQ did not ask for a bind");

    /* MAD header of the REP. */
    check("rep-hdr", rsp[0] == 1 && rsp[MAD_CLASS] == 0x07 && rsp[2] == 2,
          "REP MAD header is not a class 7 version 2 datagram");
    check("rep-hdr", rsp[MAD_METHOD] == 3, "REP is not a SEND method");
    check("rep-hdr", get_be16(rsp + MAD_ATTR_ID) == ATTR_REP,
          "REP does not carry the REP attribute id");
    check("rep-hdr", memcmp(rsp + MAD_TID, req + MAD_TID, 8) == 0,
          "REP does not echo the request transaction id");

    /* Body. */
    check("rep-body", get_be32(rsp + REP_REMOTE_COMM_ID) == p.comm_id,
          "REP does not echo the initiator's comm id");
    uint32_t local_comm_id = get_be32(rsp + REP_LOCAL_COMM_ID);
    check("rep-body", local_comm_id != 0, "REP offers no local comm id");

    uint32_t rep_qpn = get_be32(rsp + REP_LOCAL_QPN) >> 8;
    check("rep-body", rep_qpn == res.local_qpn,
          "REP QPN does not match the reported one");
    check("rep-body", nvmeof_cm_is_target_qpn(rep_qpn),
          "REP QPN is outside the controller range");
    check("rep-body", (get_be32(rsp + REP_LOCAL_QPN) & 0xff) == 0,
          "REP QPN is not left-shifted into bits 31:8");
    check("rep-body", (get_be32(rsp + REP_STARTING_PSN) >> 8) == res.start_psn,
          "REP starting PSN does not match the reported one");

    /* nvme_rdma_cm_rep private data. */
    check("rep-priv", get_le16(rsp + REP_PRIVATE) == 0,
          "REP private data recfmt is not 1.0");
    check("rep-priv", get_le16(rsp + REP_PRIVATE + 2) == p.hrqsize,
          "REP crqsize does not echo the initiator's hrqsize");

    /* The controller queue must exist now, before any capsule arrives. */
    check("bind", nvmeof_target_find_queue(t, p.qpn) != NULL,
          "REQ did not open a controller queue for the guest QP");
    check("bind", res.guest_qpn == p.qpn, "bind names the wrong guest QP");
    check("bind", res.nvme_qid == 0, "admin REQ did not report qid 0");

    /* RTU completes the handshake. */
    uint8_t rtu[IB_MAD_SIZE];
    mad_hdr(rtu, ATTR_RTU, 0xdeadbeefcafe0001ull);
    put_be32(rtu + RTU_LOCAL_COMM_ID, p.comm_id);
    put_be32(rtu + RTU_REMOTE_COMM_ID, local_comm_id);
    nvmeof_cm_handle_mad(cm, rtu, sizeof(rtu), rsp, &res);
    check("rtu", res.action == NVMEOF_CM_ACT_ESTABLISHED,
          "RTU did not establish the connection");
    check("rtu", res.rsp_len == 0, "RTU drew a reply");
    check("rtu", res.guest_qpn == p.qpn, "RTU names the wrong guest QP");

    /* A second I/O queue on the same association. */
    struct req_params p2;
    req_defaults(&p2);
    p2.comm_id = 0x55667788;
    p2.qpn = 0x000043;
    p2.qid = 1;
    p2.hrqsize = 128;
    p2.hsqsize = 127;
    build_req(req, &p2);
    nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
    check("second-queue", res.action == NVMEOF_CM_ACT_BIND,
          "the I/O queue REQ was not accepted");
    check("second-queue", res.nvme_qid == 1, "I/O queue qid was not carried");
    check("second-queue", res.local_qpn != rep_qpn,
          "the second connection reused the first connection's QPN");
    check("second-queue", nvmeof_cm_conn_count(cm) == 2,
          "connection count is wrong");

    /* Disconnect: DREQ draws a DREP and releases the queue. */
    uint8_t dreq[IB_MAD_SIZE];
    mad_hdr(dreq, ATTR_DREQ, 0xdeadbeefcafe0009ull);
    put_be32(dreq + DREQ_LOCAL_COMM_ID, p2.comm_id);
    put_be32(dreq + DREQ_REMOTE_COMM_ID, get_be32(rsp + REP_LOCAL_COMM_ID));
    put_be32(dreq + DREQ_REMOTE_QPN, res.local_qpn << 8);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("dreq", res.rsp_len == IB_MAD_SIZE, "DREQ did not draw a DREP");
    check("dreq", get_be16(rsp + MAD_ATTR_ID) == ATTR_DREP,
          "the reply to a DREQ is not a DREP");
    check("dreq", get_be32(rsp + RTU_REMOTE_COMM_ID) == p2.comm_id,
          "DREP does not echo the initiator's comm id");
    check("dreq", res.action == NVMEOF_CM_ACT_UNBIND, "DREQ did not unbind");
    check("dreq", res.guest_qpn == p2.qpn, "DREQ unbound the wrong QP");
    check("dreq", nvmeof_cm_conn_count(cm) == 1,
          "the disconnected connection was not released");
    check("dreq", nvmeof_target_find_queue(t, p2.qpn) == NULL,
          "the controller queue outlived its connection");

    /* A DREQ for a connection we do not know still gets a DREP. */
    mad_hdr(dreq, ATTR_DREQ, 0xdeadbeefcafe000aull);
    put_be32(dreq + DREQ_REMOTE_COMM_ID, 0x0badf00d);
    put_be32(dreq + DREQ_REMOTE_QPN, 0x00999900);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("stale-dreq", res.rsp_len == IB_MAD_SIZE,
          "a stale DREQ was left unanswered");
    check("stale-dreq", res.action == NVMEOF_CM_ACT_NONE,
          "a stale DREQ unbound something");

    /* A stale RTU must be ignored, not crash or bind. */
    mad_hdr(rtu, ATTR_RTU, 1);
    put_be32(rtu + RTU_REMOTE_COMM_ID, 0x0badf00d);
    nvmeof_cm_handle_mad(cm, rtu, sizeof(rtu), rsp, &res);
    check("stale-rtu", res.action == NVMEOF_CM_ACT_NONE && res.rsp_len == 0,
          "a stale RTU was acted on");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* Rejections                                                         */
/* ------------------------------------------------------------------ */

static void run_rej(struct nvmeof_cm *cm, struct nvmeof_target *t,
                    const struct req_params *p, const char *what,
                    uint16_t reason, uint16_t nvme_sts)
{
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;

    build_req(req, p);
    if (!nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res)) {
        fail("reject", "%s: REQ was not claimed", what);
        return;
    }
    if (res.rsp_len != IB_MAD_SIZE) {
        fail("reject", "%s: no reply produced", what);
        return;
    }
    if (get_be16(rsp + MAD_ATTR_ID) != ATTR_REJ) {
        fail("reject", "%s: accepted instead of rejected", what);
        return;
    }
    if (res.action != NVMEOF_CM_ACT_NONE) {
        fail("reject", "%s: a rejected REQ still bound a queue", what);
    }
    if (nvmeof_target_find_queue(t, p->qpn) != NULL) {
        fail("reject", "%s: a rejected REQ left a controller queue behind",
             what);
    }
    if (get_be32(rsp + REJ_REMOTE_COMM_ID) != p->comm_id) {
        fail("reject", "%s: REJ does not echo the initiator's comm id", what);
    }
    if ((rsp[REJ_MSG_REJECTED] >> 6) != 0) {
        fail("reject", "%s: REJ does not name the REQ as the rejected message",
             what);
    }
    uint16_t got = get_be16(rsp + REJ_REASON);
    if (got != reason) {
        fail("reject", "%s: reason %u, expected %u", what, got, reason);
    }
    if (reason == 28 && get_le16(rsp + REJ_PRIVATE + 2) != nvme_sts) {
        fail("reject", "%s: nvme status 0x%02x, expected 0x%02x", what,
             get_le16(rsp + REJ_PRIVATE + 2), nvme_sts);
    }
}

static void test_rejections(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    req_defaults(&p);
    p.service_id = ((uint64_t)0x0106 << 16) | 4421;
    run_rej(cm, t, &p, "wrong service port", 8, 0);

    req_defaults(&p);
    p.service_id = ((uint64_t)0x0108 << 16) | TEST_PORT;
    run_rej(cm, t, &p, "wrong protocol", 8, 0);

    req_defaults(&p);
    p.dst_ip = 0x0a000001; /* 10.0.0.1 */
    run_rej(cm, t, &p, "wrong destination ip", 8, 0);

    req_defaults(&p);
    p.transport_service = 1; /* unreliable connected */
    run_rej(cm, t, &p, "unreliable transport", 9, 0);

    req_defaults(&p);
    p.cma_version = 9;
    run_rej(cm, t, &p, "bad cma version", 5, 0);

    req_defaults(&p);
    p.recfmt = 7;
    run_rej(cm, t, &p, "bad recfmt", 28, 0x02);

    req_defaults(&p);
    p.qid = 4000;
    run_rej(cm, t, &p, "bad qid", 28, 0x03);

    req_defaults(&p);
    p.hsqsize = 0;
    run_rej(cm, t, &p, "zero hsqsize", 28, 0x04);

    req_defaults(&p);
    p.hrqsize = 0;
    run_rej(cm, t, &p, "zero hrqsize", 28, 0x05);

    check("reject", nvmeof_cm_conn_count(cm) == 0,
          "a rejected REQ left a connection behind");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* MADs the responder should not touch                                */
/* ------------------------------------------------------------------ */

static void test_foreign_mads(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    uint8_t m[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    /* Subnet administration, not communication management. */
    mad_hdr(m, ATTR_REQ, 1);
    m[MAD_CLASS] = 0x03;
    check("foreign", !nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "an SA MAD was claimed by the CM responder");

    /* A CM MAD with an unexpected method. */
    mad_hdr(m, ATTR_REQ, 1);
    m[MAD_METHOD] = 0x01; /* GET */
    check("foreign", !nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a CM GET was claimed");

    /* An unknown CM attribute. */
    mad_hdr(m, 0x0019 /* LAP */, 1);
    check("foreign", !nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a LAP was claimed");

    /* A short datagram is not a MAD at all. */
    mad_hdr(m, ATTR_REQ, 1);
    check("foreign", !nvmeof_cm_handle_mad(cm, m, 64, rsp, &res),
          "a 64-byte datagram was treated as a MAD");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* Reconnect and exhaustion                                           */
/* ------------------------------------------------------------------ */

static void test_reconnect(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res, res2;
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    req_defaults(&p);
    build_req(req, &p);
    nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);

    /* A repeat REQ for the same QP replaces the connection, not adds one. */
    p.comm_id = 0x99887766;
    build_req(req, &p);
    nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res2);
    check("reconnect", res2.action == NVMEOF_CM_ACT_BIND,
          "a repeat REQ was refused");
    check("reconnect", nvmeof_cm_conn_count(cm) == 1,
          "a repeat REQ left two connections for one QP");
    check("reconnect", res2.local_qpn != res.local_qpn,
          "a repeat REQ reused the stale controller QPN");
    check("reconnect", nvmeof_target_find_queue(t, p.qpn) != NULL,
          "a repeat REQ left the controller without a queue");

    /* Dropping the QP outside the CM exchange releases everything. */
    nvmeof_cm_drop_qp(cm, p.qpn);
    check("drop-qp", nvmeof_cm_conn_count(cm) == 0,
          "dropping a QP left its connection behind");
    check("drop-qp", nvmeof_target_find_queue(t, p.qpn) == NULL,
          "dropping a QP left its controller queue behind");
    nvmeof_cm_drop_qp(cm, 0xdead); /* unknown handle must be harmless */

    /*
     * The target only has room for max_queues + 1 queues, so a guest
     * opening more must be refused rather than overrunning the table.
     */
    const struct nvmeof_target_cfg *cfg = nvmeof_target_config(t);
    unsigned accepted = 0;
    for (unsigned i = 0; i < (unsigned)cfg->max_queues + 4; i++) {
        req_defaults(&p);
        p.comm_id = 0x1000 + i;
        p.qpn = 0x100 + i;
        p.qid = (uint16_t)(i % (cfg->max_queues + 1));
        build_req(req, &p);
        nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
        if (get_be16(rsp + MAD_ATTR_ID) == ATTR_REP) {
            accepted++;
        }
    }
    check("exhaustion", accepted == (unsigned)cfg->max_queues + 1,
          "the controller accepted more queues than it has room for");
    check("exhaustion", nvmeof_cm_conn_count(cm) == accepted,
          "connection count does not match the accepted queues");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

int main(void)
{
    test_full_handshake();
    test_rejections();
    test_foreign_mads();
    test_reconnect();

    if (failures == 0) {
        printf("PASS nvmeof-cm\n");
    } else {
        printf("%d nvmeof-cm check(s) failed\n", failures);
    }
    return failures;
}
