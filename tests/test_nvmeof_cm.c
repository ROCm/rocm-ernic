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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "byteorder.h"
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
#define ATTR_MRA  0x0011
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

#define MRA_REMOTE_COMM_ID  28
#define DREP_LOCAL_COMM_ID  24
#define DREP_REMOTE_COMM_ID 28

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

static void mad_hdr(uint8_t *m, uint16_t attr, uint64_t tid)
{
    memset(m, 0, IB_MAD_SIZE);
    m[0] = 1; /* base version */
    m[MAD_CLASS] = 0x07;
    m[2] = 2; /* class version */
    m[MAD_METHOD] = 3;
    ernic_put_be64(m + MAD_TID, tid);
    ernic_put_be16(m + MAD_ATTR_ID, attr);
}

struct req_params {
    uint32_t comm_id;
    uint32_t qpn;
    uint64_t service_id;
    uint32_t dst_ip;     /* with ip_version 4 */
    uint8_t dst_ip6[16]; /* with ip_version 6 */
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
    ernic_put_be32(m + REQ_LOCAL_COMM_ID, p->comm_id);
    ernic_put_be64(m + REQ_SERVICE_ID, p->service_id);
    ernic_put_be64(m + REQ_LOCAL_CA_GUID, 0x0002c9030012abcdull);
    ernic_put_be32(m + REQ_LOCAL_QPN,
                   (p->qpn << 8) | 4 /* responder resources */);
    ernic_put_be32(m + REQ_LOCAL_EECN, 4 /* initiator depth */);
    ernic_put_be32(m + REQ_REMOTE_EECN,
                   (uint32_t)((p->transport_service & 0x3) << 1) | (20u << 3));
    ernic_put_be32(m + REQ_STARTING_PSN, 0x123456 << 8);

    uint8_t *priv = m + REQ_PRIVATE;
    priv[0] = p->cma_version;
    priv[1] = (uint8_t)(p->ip_version << 4);
    ernic_put_be16(priv + 2, TEST_PORT);
    ernic_put_be32(priv + 4 + 12, 0xc0a8c814u); /* src 192.168.200.20 */
    if (p->ip_version == 6) {
        memcpy(priv + 20, p->dst_ip6, 16);
    } else {
        ernic_put_be32(priv + 20 + 12, p->dst_ip);
    }

    uint8_t *nvme = priv + 36;
    ernic_put_le16(nvme + 0, p->recfmt);
    ernic_put_le16(nvme + 2, p->qid);
    ernic_put_le16(nvme + 4, p->hrqsize);
    ernic_put_le16(nvme + 6, p->hsqsize);
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
    check("rep-hdr", ernic_get_be16(rsp + MAD_ATTR_ID) == ATTR_REP,
          "REP does not carry the REP attribute id");
    check("rep-hdr", memcmp(rsp + MAD_TID, req + MAD_TID, 8) == 0,
          "REP does not echo the request transaction id");

    /* Body. */
    check("rep-body", ernic_get_be32(rsp + REP_REMOTE_COMM_ID) == p.comm_id,
          "REP does not echo the initiator's comm id");
    uint32_t local_comm_id = ernic_get_be32(rsp + REP_LOCAL_COMM_ID);
    check("rep-body", local_comm_id != 0, "REP offers no local comm id");

    uint32_t rep_qpn = ernic_get_be32(rsp + REP_LOCAL_QPN) >> 8;
    check("rep-body", rep_qpn == res.local_qpn,
          "REP QPN does not match the reported one");
    check("rep-body", nvmeof_cm_is_target_qpn(rep_qpn),
          "REP QPN is outside the controller range");
    check("rep-body", (ernic_get_be32(rsp + REP_LOCAL_QPN) & 0xff) == 0,
          "REP QPN is not left-shifted into bits 31:8");
    check("rep-body",
          (ernic_get_be32(rsp + REP_STARTING_PSN) >> 8) == res.start_psn,
          "REP starting PSN does not match the reported one");

    /* nvme_rdma_cm_rep private data. */
    check("rep-priv", ernic_get_le16(rsp + REP_PRIVATE) == 0,
          "REP private data recfmt is not 1.0");
    check("rep-priv", ernic_get_le16(rsp + REP_PRIVATE + 2) == p.hrqsize,
          "REP crqsize does not echo the initiator's hrqsize");

    /* The controller queue must exist now, before any capsule arrives. */
    check("bind", nvmeof_target_find_queue(t, p.qpn) != NULL,
          "REQ did not open a controller queue for the guest QP");
    check("bind", res.guest_qpn == p.qpn, "bind names the wrong guest QP");
    check("bind", res.nvme_qid == 0, "admin REQ did not report qid 0");

    /* RTU completes the handshake. */
    uint8_t rtu[IB_MAD_SIZE];
    mad_hdr(rtu, ATTR_RTU, 0xdeadbeefcafe0001ull);
    ernic_put_be32(rtu + RTU_LOCAL_COMM_ID, p.comm_id);
    ernic_put_be32(rtu + RTU_REMOTE_COMM_ID, local_comm_id);
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
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, p2.comm_id);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID,
                   ernic_get_be32(rsp + REP_LOCAL_COMM_ID));
    ernic_put_be32(dreq + DREQ_REMOTE_QPN, res.local_qpn << 8);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("dreq", res.rsp_len == IB_MAD_SIZE, "DREQ did not draw a DREP");
    check("dreq", ernic_get_be16(rsp + MAD_ATTR_ID) == ATTR_DREP,
          "the reply to a DREQ is not a DREP");
    check("dreq", ernic_get_be32(rsp + RTU_REMOTE_COMM_ID) == p2.comm_id,
          "DREP does not echo the initiator's comm id");
    check("dreq", res.action == NVMEOF_CM_ACT_UNBIND, "DREQ did not unbind");
    check("dreq", res.guest_qpn == p2.qpn, "DREQ unbound the wrong QP");
    check("dreq", nvmeof_cm_conn_count(cm) == 1,
          "the disconnected connection was not released");
    check("dreq", nvmeof_target_find_queue(t, p2.qpn) == NULL,
          "the controller queue outlived its connection");

    /* A DREQ for a connection we do not know still gets a DREP. */
    mad_hdr(dreq, ATTR_DREQ, 0xdeadbeefcafe000aull);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, 0x0badf00d);
    ernic_put_be32(dreq + DREQ_REMOTE_QPN, 0x00999900);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("stale-dreq", res.rsp_len == IB_MAD_SIZE,
          "a stale DREQ was left unanswered");
    check("stale-dreq", res.action == NVMEOF_CM_ACT_NONE,
          "a stale DREQ unbound something");

    /* A stale RTU must be ignored, not crash or bind. */
    mad_hdr(rtu, ATTR_RTU, 1);
    ernic_put_be32(rtu + RTU_REMOTE_COMM_ID, 0x0badf00d);
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
    if (ernic_get_be16(rsp + MAD_ATTR_ID) != ATTR_REJ) {
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
    if (ernic_get_be32(rsp + REJ_REMOTE_COMM_ID) != p->comm_id) {
        fail("reject", "%s: REJ does not echo the initiator's comm id", what);
    }
    if ((rsp[REJ_MSG_REJECTED] >> 6) != 0) {
        fail("reject", "%s: REJ does not name the REQ as the rejected message",
             what);
    }
    uint16_t got = ernic_get_be16(rsp + REJ_REASON);
    if (got != reason) {
        fail("reject", "%s: reason %u, expected %u", what, got, reason);
    }
    if (reason == 28 && ernic_get_le16(rsp + REJ_PRIVATE + 2) != nvme_sts) {
        fail("reject", "%s: nvme status 0x%02x, expected 0x%02x", what,
             ernic_get_le16(rsp + REJ_PRIVATE + 2), nvme_sts);
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

    /* A base version other than 1. */
    mad_hdr(m, ATTR_REQ, 1);
    m[0] = 2;
    check("foreign", !nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a base version 2 MAD was claimed");

    /* Missing arguments are refused, and the result still cleared. */
    mad_hdr(m, ATTR_REQ, 1);
    memset(&res, 0xa5, sizeof(res));
    check("bad-args", !nvmeof_cm_handle_mad(NULL, m, sizeof(m), rsp, &res),
          "a NULL responder was used");
    check("bad-args", res.action == NVMEOF_CM_ACT_NONE && res.rsp_len == 0,
          "a refused MAD left a stale result");
    check("bad-args", !nvmeof_cm_handle_mad(cm, NULL, IB_MAD_SIZE, rsp, &res),
          "a NULL MAD was read");
    check("bad-args", !nvmeof_cm_handle_mad(cm, m, sizeof(m), NULL, &res),
          "a MAD was answered into a NULL buffer");
    check("bad-args", nvmeof_cm_conn_count(cm) == 0,
          "a refused MAD left a connection behind");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* The other CM messages                                              */
/* ------------------------------------------------------------------ */

/* Send a REQ for @p and return the local comm ID from the REP, or 0. */
static uint32_t connect_req(struct nvmeof_cm *cm, const struct req_params *p,
                            struct nvmeof_cm_result *res)
{
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];

    build_req(req, p);
    if (!nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, res) ||
        ernic_get_be16(rsp + MAD_ATTR_ID) != ATTR_REP) {
        return 0;
    }
    return ernic_get_be32(rsp + REP_LOCAL_COMM_ID);
}

static void test_other_messages(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    uint8_t m[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    req_defaults(&p);
    uint32_t local_comm_id = connect_req(cm, &p, &res);
    check("other-setup", local_comm_id != 0, "the REQ was not accepted");

    /* MRA and DREP are claimed, change nothing, and draw no reply. */
    mad_hdr(m, ATTR_MRA, 2);
    ernic_put_be32(m + MRA_REMOTE_COMM_ID, local_comm_id);
    check("mra", nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "an MRA was not claimed");
    check("mra", res.action == NVMEOF_CM_ACT_NONE && res.rsp_len == 0,
          "an MRA was acted on or answered");

    mad_hdr(m, ATTR_DREP, 3);
    ernic_put_be32(m + DREP_REMOTE_COMM_ID, local_comm_id);
    check("drep", nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a DREP was not claimed");
    check("drep", res.action == NVMEOF_CM_ACT_NONE && res.rsp_len == 0,
          "a DREP was acted on or answered");
    check("drep", nvmeof_cm_conn_count(cm) == 1,
          "a DREP released the connection");

    /* A REP is for an active side, which the target never is: REJ it,
     * naming the REP (message_rejected 1) and reason 5, unsupported. */
    mad_hdr(m, ATTR_REP, 0x0123456789abcdefull);
    ernic_put_be32(m + REP_LOCAL_COMM_ID, 0x77665544);
    check("rep", nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a REP was not claimed");
    check("rep", res.rsp_len == IB_MAD_SIZE, "a REP drew no reply");
    check("rep", ernic_get_be16(rsp + MAD_ATTR_ID) == ATTR_REJ,
          "the reply to a REP is not a REJ");
    check("rep", memcmp(rsp + MAD_TID, m + MAD_TID, 8) == 0,
          "the REJ does not echo the REP's transaction id");
    check("rep", (rsp[REJ_MSG_REJECTED] >> 6) == 1,
          "the REJ does not name the REP as the rejected message");
    check("rep", ernic_get_be16(rsp + REJ_REASON) == 5,
          "the REJ does not give reason 5, unsupported");
    check("rep", ernic_get_be32(rsp + REJ_REMOTE_COMM_ID) == 0x77665544,
          "the REJ does not echo the sender's comm id");
    check("rep", res.action == NVMEOF_CM_ACT_NONE, "a REP was acted on");

    /* A REJ for a connection we do not know is claimed and ignored. */
    mad_hdr(m, ATTR_REJ, 4);
    ernic_put_be32(m + REJ_REMOTE_COMM_ID, 0x0badf00d);
    check("stale-rej", nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a stale REJ was not claimed");
    check("stale-rej", res.action == NVMEOF_CM_ACT_NONE && res.rsp_len == 0,
          "a stale REJ was acted on or answered");
    check("stale-rej", nvmeof_cm_conn_count(cm) == 1,
          "a stale REJ released a connection");

    /* The guest refusing our REP tears the connection down. */
    mad_hdr(m, ATTR_REJ, 5);
    ernic_put_be32(m + REJ_REMOTE_COMM_ID, local_comm_id);
    check("rej", nvmeof_cm_handle_mad(cm, m, sizeof(m), rsp, &res),
          "a REJ was not claimed");
    check("rej", res.action == NVMEOF_CM_ACT_UNBIND, "a REJ did not unbind");
    check("rej", res.guest_qpn == p.qpn, "a REJ unbound the wrong QP");
    check("rej", res.rsp_len == 0, "a REJ was answered");
    check("rej", nvmeof_cm_conn_count(cm) == 0,
          "a REJ left the connection behind");
    check("rej", nvmeof_target_find_queue(t, p.qpn) == NULL,
          "a REJ left the controller queue behind");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/*
 * The target listens on one IPv4 address, so a REQ whose cma_hdr is
 * anything but IPv4 naming that address is for a listener it doesn't have,
 * and is refused with reason 8, invalid service ID, as rdma_cm refuses
 * one. Once the target can have an IPv6 address, an IPv6 REQ naming it
 * will be answered, but these still won't be: none names it.
 */
static void test_ip_versions(void)
{
    static const uint8_t ula[16] = {0xfd, 0, 0, 0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0, 0, 0, 1};
    /* ::ffff:192.168.200.1, the target's address mapped into IPv6. */
    static const uint8_t mapped[16] = {0, 0, 0,    0,    0,    0,    0,    0,
                                       0, 0, 0xff, 0xff, 0xc0, 0xa8, 0xc8, 1};
    /* The target's address laid out as an IPv4 cma_hdr has it. */
    static const uint8_t padded[16] = {0, 0, 0, 0, 0,    0,    0,    0,
                                       0, 0, 0, 0, 0xc0, 0xa8, 0xc8, 1};
    static const struct {
        const char *what;
        uint8_t ip_version;
        const uint8_t *dst6;
    } rows[] = {
        {"IPv6 to another address", 6, ula},
        {"IPv6 to the target's IPv4-mapped address", 6, mapped},
        {"IPv6 with the target's address in IPv4 layout", 6, padded},
        {"IP version 0", 0, NULL},
        {"IP version 5", 5, NULL},
    };
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        req_defaults(&p);
        p.ip_version = rows[i].ip_version;
        if (rows[i].dst6 != NULL) {
            memcpy(p.dst_ip6, rows[i].dst6, sizeof(p.dst_ip6));
        }
        run_rej(cm, t, &p, rows[i].what, 8, 0);
    }
    check("ip-version", nvmeof_cm_conn_count(cm) == 0,
          "a refused REQ left a connection behind");

    nvmeof_cm_destroy(cm);
    nvmeof_target_destroy(t);
}

/*
 * Send a DREQ whose remote comm ID matches nothing, so the responder has
 * to go by the controller QPN @qpn and the guest's comm ID.
 */
static void dreq_by_qpn(struct nvmeof_cm *cm, uint32_t guest_comm_id,
                        uint32_t qpn, struct nvmeof_cm_result *res)
{
    uint8_t dreq[IB_MAD_SIZE], rsp[IB_MAD_SIZE];

    mad_hdr(dreq, ATTR_DREQ, 0x53);
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, guest_comm_id);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, 0x0badf00d);
    ernic_put_be32(dreq + DREQ_REMOTE_QPN, qpn << 8);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, res);
}

/*
 * A DREQ whose remote comm ID matches nothing is matched by its remote
 * QPN instead. That is the QPN of the DREQ's recipient: the controller's,
 * from our REP, not the guest's own. The guest's comm ID has to match
 * too, since a controller QPN is reused once the QPNs wrap.
 */
static void test_dreq_by_qpn(void)
{
    struct nvmeof_target *t = make_target();
    struct nvmeof_cm *cm = nvmeof_cm_create(t, TEST_TRADDR, TEST_PORT);
    uint8_t dreq[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params p;

    if (t == NULL || cm == NULL) {
        nvmeof_cm_destroy(cm);
        nvmeof_target_destroy(t);
        return;
    }

    req_defaults(&p);
    uint32_t local_comm_id = connect_req(cm, &p, &res);
    uint32_t local_qpn = res.local_qpn;
    check("dreq-qpn", local_comm_id != 0, "the REQ was not accepted");
    check("dreq-qpn", local_qpn != p.qpn,
          "the controller QPN equals the guest's, so the test proves nothing");

    /* The guest's own QPN names no controller QP. */
    mad_hdr(dreq, ATTR_DREQ, 0x51);
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, p.comm_id);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, 0x0badf00d);
    ernic_put_be32(dreq + DREQ_REMOTE_QPN, p.qpn << 8);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("dreq-qpn", res.rsp_len == IB_MAD_SIZE,
          "a DREQ naming the guest's QPN was left unanswered");
    check("dreq-qpn", res.action == NVMEOF_CM_ACT_NONE,
          "a DREQ naming the guest's QPN unbound it");
    check("dreq-qpn", nvmeof_cm_conn_count(cm) == 1,
          "a DREQ naming the guest's QPN released the connection");

    /* The controller's QPN with another connection's guest comm ID. */
    dreq_by_qpn(cm, p.comm_id + 1, local_qpn, &res);
    check("dreq-qpn", res.rsp_len == IB_MAD_SIZE,
          "a DREQ with another guest comm id was left unanswered");
    check("dreq-qpn", res.action == NVMEOF_CM_ACT_NONE,
          "a DREQ with another guest comm id unbound the QP");
    check("dreq-qpn", nvmeof_cm_conn_count(cm) == 1,
          "a DREQ with another guest comm id released the connection");

    /* The controller's QPN does. */
    mad_hdr(dreq, ATTR_DREQ, 0x52);
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, p.comm_id);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, 0x0badf00d);
    ernic_put_be32(dreq + DREQ_REMOTE_QPN, local_qpn << 8);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("dreq-qpn", res.action == NVMEOF_CM_ACT_UNBIND,
          "a DREQ naming the controller's QPN did not unbind");
    check("dreq-qpn", res.guest_qpn == p.qpn && res.local_qpn == local_qpn,
          "a DREQ naming the controller's QPN unbound the wrong QP");
    check("dreq-qpn", ernic_get_be32(rsp + DREP_LOCAL_COMM_ID) == local_comm_id,
          "the DREP does not carry the connection's comm id");
    check("dreq-qpn", nvmeof_cm_conn_count(cm) == 0,
          "the connection was not released");
    check("dreq-qpn", nvmeof_target_find_queue(t, p.qpn) == NULL,
          "the controller queue outlived its connection");

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
        p.qid = (uint16_t)(i % (cfg->max_queues + 1u));
        build_req(req, &p);
        nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
        if (ernic_get_be16(rsp + MAD_ATTR_ID) == ATTR_REP) {
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

/*
 * A responder with no target has no queue limit of its own, so the only
 * limit is its connection table: once that is full, a REQ is refused with
 * reason 3, no resources, rather than overrunning it.
 */
static void test_table_full(void)
{
    struct nvmeof_cm *cm = nvmeof_cm_create(NULL, TEST_TRADDR, TEST_PORT);
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params p;
    unsigned accepted = 0;
    uint16_t reason = 0;
    uint32_t first_comm_id = 0;

    if (cm == NULL) {
        fail("setup", "nvmeof_cm_create failed");
        return;
    }

    for (unsigned i = 0; i < 1000; i++) {
        req_defaults(&p);
        p.comm_id = 0x2000 + i;
        p.qpn = 0x200 + i;
        build_req(req, &p);
        nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
        if (ernic_get_be16(rsp + MAD_ATTR_ID) != ATTR_REP) {
            reason = ernic_get_be16(rsp + REJ_REASON);
            break;
        }
        if (accepted++ == 0) {
            first_comm_id = ernic_get_be32(rsp + REP_LOCAL_COMM_ID);
        }
    }
    check("table-full", accepted > 0 && accepted < 1000,
          "the connection table never filled");
    check("table-full", reason == 3,
          "a REQ beyond the table was not refused for want of resources");
    check("table-full", res.action == NVMEOF_CM_ACT_NONE,
          "a refused REQ still bound a queue");
    check("table-full", nvmeof_cm_conn_count(cm) == accepted,
          "connection count does not match the accepted REQs");

    /* A DREQ frees a slot, with no target queue to close... */
    uint8_t dreq[IB_MAD_SIZE];
    mad_hdr(dreq, ATTR_DREQ, 7);
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, 0x2000);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, first_comm_id);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("table-full", res.action == NVMEOF_CM_ACT_UNBIND,
          "a DREQ did not unbind");
    check("table-full", res.guest_qpn == 0x200, "a DREQ unbound the wrong QP");
    check("table-full", nvmeof_cm_conn_count(cm) == accepted - 1,
          "a DREQ did not free a slot");

    /* ...which the refused REQ can now have. */
    build_req(req, &p);
    nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
    check("table-full", ernic_get_be16(rsp + MAD_ATTR_ID) == ATTR_REP,
          "a REQ was refused after a slot was freed");

    nvmeof_cm_destroy(cm);
}

/*
 * The responder's QPNs come from NVMEOF_CM_QPN_BASE + 1 up to the top of
 * the range nvmeof_cm_is_target_qpn() accepts, then start again from the
 * bottom rather than handing out a QPN the data path would not route to
 * the target. A repeat REQ for one guest QP replaces its connection, so
 * the table never fills on the way round.
 */
static void test_qpn_wrap(void)
{
    struct nvmeof_cm *cm = nvmeof_cm_create(NULL, TEST_TRADDR, TEST_PORT);
    /* A QPN is 24 bits; the bits below the mask are free. */
    const uint32_t last =
        NVMEOF_CM_QPN_BASE | (~NVMEOF_CM_QPN_MASK & 0x00ffffffu);
    uint8_t req[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params p;
    uint32_t want = NVMEOF_CM_QPN_BASE + 1;

    if (cm == NULL) {
        fail("setup", "nvmeof_cm_create failed");
        return;
    }

    req_defaults(&p);
    for (;;) {
        p.comm_id++;
        build_req(req, &p);
        nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
        if (res.action != NVMEOF_CM_ACT_BIND || res.local_qpn != want) {
            fail("qpn-wrap", "QPN 0x%06x, expected 0x%06x", res.local_qpn,
                 want);
            break;
        }
        if (want == last) {
            break;
        }
        want++;
    }

    /* One more: past the top of the range, back to the bottom. */
    p.comm_id++;
    build_req(req, &p);
    nvmeof_cm_handle_mad(cm, req, sizeof(req), rsp, &res);
    check("qpn-wrap", res.local_qpn == NVMEOF_CM_QPN_BASE + 1,
          "the QPN after the top of the range is not the first again");
    check("qpn-wrap", nvmeof_cm_is_target_qpn(res.local_qpn),
          "the QPN after the top of the range is not a target QPN");
    check("qpn-wrap", nvmeof_cm_conn_count(cm) == 1,
          "repeat REQs for one QP left more than one connection");

    nvmeof_cm_destroy(cm);
}

/*
 * After the QPNs wrap, a QPN a live connection still holds is skipped:
 * connection A stays up while guest QP B reconnects all the way round, and
 * B is never given A's QPN, so a DREQ for B can't close A. Once A is closed
 * and B has been given A's old QPN, a stale DREQ for A, replayed, doesn't
 * close B.
 */
static void test_qpn_wrap_live(void)
{
    struct nvmeof_cm *cm = nvmeof_cm_create(NULL, TEST_TRADDR, TEST_PORT);
    uint8_t dreq[IB_MAD_SIZE], rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    struct req_params a, b;
    bool wrapped = false, reused = false;

    if (cm == NULL) {
        fail("setup", "nvmeof_cm_create failed");
        return;
    }

    req_defaults(&a);
    a.comm_id = 0xa0000000;
    a.qpn = 0x10;
    uint32_t a_comm_id = connect_req(cm, &a, &res);
    uint32_t a_qpn = res.local_qpn;
    check("qpn-wrap-live", a_comm_id != 0, "A's REQ was not accepted");

    /* B reconnects until its QPN wraps round past A's. */
    req_defaults(&b);
    b.comm_id = 0xb0000000;
    b.qpn = 0x20;
    uint32_t prev = a_qpn;
    for (unsigned i = 0; i < 0x20000 && !wrapped; i++) {
        b.comm_id++;
        if (connect_req(cm, &b, &res) == 0) {
            fail("qpn-wrap-live", "B's REQ %u was refused", i);
            break;
        }
        if (res.local_qpn == a_qpn) {
            fail("qpn-wrap-live", "B was given A's QPN 0x%06x", a_qpn);
            break;
        }
        wrapped = res.local_qpn < prev;
        prev = res.local_qpn;
    }
    check("qpn-wrap-live", wrapped, "B's QPNs never wrapped");
    check("qpn-wrap-live", res.local_qpn == a_qpn + 1,
          "after the wrap, B was not given the QPN after A's");

    /* A DREQ for B by QPN closes B, not A. */
    dreq_by_qpn(cm, b.comm_id, res.local_qpn, &res);
    check("qpn-wrap-live",
          res.action == NVMEOF_CM_ACT_UNBIND && res.guest_qpn == b.qpn,
          "a DREQ for B by its QPN did not close B");
    check("qpn-wrap-live", nvmeof_cm_conn_count(cm) == 1,
          "a DREQ for B by its QPN left the wrong connections");

    /* Close A, then reconnect B until it is given A's old QPN. */
    mad_hdr(dreq, ATTR_DREQ, 0x54);
    ernic_put_be32(dreq + DREQ_LOCAL_COMM_ID, a.comm_id);
    ernic_put_be32(dreq + DREQ_REMOTE_COMM_ID, a_comm_id);
    nvmeof_cm_handle_mad(cm, dreq, sizeof(dreq), rsp, &res);
    check("qpn-wrap-live",
          res.action == NVMEOF_CM_ACT_UNBIND && res.guest_qpn == a.qpn,
          "A's DREQ did not close A");
    for (unsigned i = 0; i < 0x20000 && !reused; i++) {
        b.comm_id++;
        if (connect_req(cm, &b, &res) == 0) {
            fail("qpn-wrap-live", "B's REQ %u was refused", i);
            break;
        }
        reused = res.local_qpn == a_qpn;
    }
    check("qpn-wrap-live", reused, "B was never given A's old QPN");

    /* A's DREQ again, missing on the comm id: B's isn't A's guest comm id. */
    dreq_by_qpn(cm, a.comm_id, a_qpn, &res);
    check("qpn-wrap-live", res.rsp_len == IB_MAD_SIZE,
          "a replayed DREQ for A was left unanswered");
    check("qpn-wrap-live", res.action == NVMEOF_CM_ACT_NONE,
          "a replayed DREQ for A closed B, which has A's old QPN");
    check("qpn-wrap-live", nvmeof_cm_conn_count(cm) == 1,
          "a replayed DREQ for A released a connection");

    nvmeof_cm_destroy(cm);
}

int main(void)
{
    test_full_handshake();
    test_rejections();
    test_foreign_mads();
    test_other_messages();
    test_ip_versions();
    test_dreq_by_qpn();
    test_reconnect();
    test_table_full();
    test_qpn_wrap();
    test_qpn_wrap_live();

    if (failures == 0) {
        printf("PASS nvmeof-cm\n");
    } else {
        printf("%d nvmeof-cm check(s) failed\n", failures);
    }
    return failures;
}
