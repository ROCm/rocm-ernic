/*
 * test_nvmeof_target.c — unit tests for the in-process NVMe-oF controller
 *
 * The controller in src/nvmeof_target.c is deliberately transport- and
 * DMA-agnostic: it is handed command capsules and a pair of callbacks and
 * hands back response capsules. That is what makes this test possible at
 * all -- it stands a plain byte array in for guest memory and drives the
 * exact capsule sequence the Linux nvme-rdma initiator emits, from
 * Fabrics Connect through Identify to a read-after-write on the
 * namespace, with no VM, no RDMA device, and no server process.
 *
 * Failures print "FAIL <case>: <detail>" and main() returns the count, so
 * a regression names itself rather than just tripping an assert.
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
#include <unistd.h>

#include "nvmeof_target.h"

#define HOST_BUF_SIZE (256 * 1024)
#define HOST_BASE     0x40000000ull
#define HOST_RKEY     0x1234u

static int failures;

static void fail(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("FAIL %-28s: ", name);
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
/* Fake host memory                                                   */
/* ------------------------------------------------------------------ */

struct host_mem {
    uint8_t buf[HOST_BUF_SIZE];
    uint32_t rkey;
    bool fail_next;
};

static bool host_range(struct host_mem *h, uint32_t key, uint64_t addr,
                       uint32_t len, size_t *off)
{
    if (h->fail_next) {
        h->fail_next = false;
        return false;
    }
    if (key != h->rkey) {
        return false;
    }
    if (addr < HOST_BASE) {
        return false;
    }
    uint64_t o = addr - HOST_BASE;
    if (o > HOST_BUF_SIZE || HOST_BUF_SIZE - o < len) {
        return false;
    }
    *off = (size_t)o;
    return true;
}

static uint32_t host_read(void *ctx, uint32_t key, uint64_t addr, void *dst,
                          uint32_t len)
{
    struct host_mem *h = ctx;
    size_t off;
    if (!host_range(h, key, addr, len, &off)) {
        return 0;
    }
    memcpy(dst, h->buf + off, len);
    return len;
}

static uint32_t host_write(void *ctx, uint32_t key, uint64_t addr,
                           const void *src, uint32_t len)
{
    struct host_mem *h = ctx;
    size_t off;
    if (!host_range(h, key, addr, len, &off)) {
        return 0;
    }
    memcpy(h->buf + off, src, len);
    return len;
}

static const struct nvmeof_dma_ops host_ops = {
    .from_host = host_read,
    .to_host = host_write,
};

/* ------------------------------------------------------------------ */
/* Capsule construction                                               */
/* ------------------------------------------------------------------ */

struct capsule {
    uint8_t b[NVME_SQE_SIZE + NVMEOF_CONNECT_DATA_SIZE];
    size_t len;
};

static void cap_init(struct capsule *c, uint8_t opcode, uint16_t cid)
{
    memset(c, 0, sizeof(*c));
    c->len = NVME_SQE_SIZE;
    c->b[NVME_SQE_OPCODE_OFF] = opcode;
    nvme_put_le16(c->b + NVME_SQE_CID_OFF, cid);
}

static void cap_keyed_sgl(struct capsule *c, uint64_t addr, uint32_t len,
                          uint32_t key)
{
    uint8_t *d = c->b + NVME_SQE_DPTR_OFF;
    nvme_put_le64(d, addr);
    d[8] = (uint8_t)len;
    d[9] = (uint8_t)(len >> 8);
    d[10] = (uint8_t)(len >> 16);
    nvme_put_le32(d + 11, key);
    d[15] = NVME_SGL_TYPE_KEYED_DATA << 4;
}

static void cap_inline_sgl(struct capsule *c, const void *data, uint32_t len)
{
    uint8_t *d = c->b + NVME_SQE_DPTR_OFF;
    nvme_put_le64(d, 0);
    nvme_put_le32(d + 8, len);
    d[15] =
        (uint8_t)((NVME_SGL_TYPE_DATA_BLOCK << 4) | NVME_SGL_SUBTYPE_OFFSET);
    memcpy(c->b + NVME_SQE_SIZE, data, len);
    c->len = NVME_SQE_SIZE + len;
}

static uint16_t rsp_status(const uint8_t *rsp)
{
    return nvme_get_le16(rsp + NVME_CQE_STATUS_OFF);
}

static uint8_t rsp_sc(const uint8_t *rsp)
{
    return (uint8_t)((rsp_status(rsp) >> 1) & 0xff);
}

/*
 * Lay out the 1024-byte Connect payload in host memory and point the
 * command's keyed SGL at it, the way nvme-rdma does for the admin queue.
 */
static void stage_connect_data(struct host_mem *h, uint64_t addr,
                               const char *subnqn, const char *hostnqn)
{
    uint8_t *p = h->buf + (addr - HOST_BASE);
    memset(p, 0, NVMEOF_CONNECT_DATA_SIZE);
    memcpy(p + NVMEOF_CONNECT_HOSTID_OFF, "0123456789abcdef", 16);
    snprintf((char *)p + NVMEOF_CONNECT_SUBNQN_OFF, 256, "%s", subnqn);
    snprintf((char *)p + NVMEOF_CONNECT_HOSTNQN_OFF, 256, "%s", hostnqn);
}

static int do_connect(struct nvmeof_queue *q, struct host_mem *h, uint16_t qid,
                      uint16_t sqsize, const char *subnqn, const char *hostnqn,
                      uint8_t *rsp)
{
    struct capsule c;
    uint64_t addr = HOST_BASE;

    cap_init(&c, NVME_ADMIN_FABRICS, 0x10 + qid);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_CONNECT;
    nvme_put_le16(c.b + NVMEOF_CONNECT_QID_OFF, qid);
    nvme_put_le16(c.b + NVMEOF_CONNECT_SQSIZE_OFF, sqsize);
    cap_keyed_sgl(&c, addr, NVMEOF_CONNECT_DATA_SIZE, HOST_RKEY);
    stage_connect_data(h, addr, subnqn, hostnqn);

    return nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
}

/* ------------------------------------------------------------------ */
/* Configuration parsing                                              */
/* ------------------------------------------------------------------ */

static const struct {
    const char *opts;
    bool ok;
    const char *what;
} cfg_cases[] = {
    {NULL, true, "null options"},
    {"", true, "empty options"},
    {"size=64M", true, "size with M suffix"},
    {"size=1G", true, "size with G suffix"},
    {"size=128MiB", true, "size with MiB suffix"},
    {"size=1048576", true, "bare byte count"},
    {"bs=4096", true, "4k blocks"},
    {"nqn=nvmet-test", true, "explicit nqn"},
    {"ip=10.0.0.1,port=4421", true, "address override"},
    {"queues=4", true, "queue count"},
    {"model=widget,serial=SN1", true, "identify strings"},
    {"size=64M,bs=4096,nsid=1", true, "combination"},
    {"size", false, "missing ="},
    {"size=", false, "empty size"},
    {"size=0", false, "zero size"},
    {"size=-1", false, "negative size"},
    {"size=12Q", false, "bad size suffix"},
    {"bs=777", false, "unsupported block size"},
    {"size=1000,bs=512", false, "size not a multiple of bs"},
    {"nsid=0", false, "reserved nsid"},
    {"ip=999.1.1.1", false, "octet out of range"},
    {"ip=10.0.0", false, "short address"},
    {"port=0", false, "zero port"},
    {"port=70000", false, "port out of range"},
    {"queues=0", false, "zero queues"},
    {"queues=65", false, "too many queues"},
    {"nonsense=1", false, "unknown key"},
};

/*
 * The wire status word is the status field shifted left over the phase tag,
 * so DNR lands at 0x8000 and 0x4000 is the More bit.  Getting that backwards
 * is invisible to every other check here, which only ever looks at SC -- and
 * to the host it means a permanent failure gets retried.
 */
static void test_status_encoding(void)
{
    uint16_t s = nvme_status(NVME_SCT_GENERIC, NVME_SC_LBA_RANGE, 1);

    if ((s & 0x8000u) == 0)
        fail("status dnr", "DNR not set in %#x", s);
    if (s & 0x4000u)
        fail("status more", "More bit set in %#x", s);
    if (((s >> 1) & 0xffu) != NVME_SC_LBA_RANGE)
        fail("status sc", "SC is %#x in %#x", (s >> 1) & 0xffu, s);
    if (((s >> 9) & 0x7u) != NVME_SCT_GENERIC)
        fail("status sct", "SCT is %#x in %#x", (s >> 9) & 0x7u, s);
    if (s & 1u)
        fail("status phase", "phase bit set in %#x", s);

    uint16_t n = nvme_status(NVME_SCT_GENERIC, NVME_SC_LBA_RANGE, 0);
    if (n & 0xc000u)
        fail("status nodnr", "DNR or More set without dnr in %#x", n);
}

static void test_config(void)
{
    for (size_t i = 0; i < sizeof(cfg_cases) / sizeof(cfg_cases[0]); i++) {
        struct nvmeof_target_cfg cfg;
        char err[128] = "";

        nvmeof_target_cfg_defaults(&cfg);
        bool ok =
            nvmeof_target_cfg_parse(&cfg, cfg_cases[i].opts, err, sizeof(err));
        if (ok != cfg_cases[i].ok) {
            fail("config", "%s: expected %s, got %s (%s)", cfg_cases[i].what,
                 cfg_cases[i].ok ? "accept" : "reject",
                 ok ? "accept" : "reject", err);
        }
        if (!ok && err[0] == '\0') {
            fail("config", "%s: rejected with no message", cfg_cases[i].what);
        }
        free(cfg.file);
    }

    /* Defaults must line up with what the ansible nvmeof_setup role uses. */
    struct nvmeof_target_cfg cfg;
    nvmeof_target_cfg_defaults(&cfg);
    check("config-defaults", strcmp(cfg.subnqn, "nvmet-test") == 0,
          "default subnqn is not nvmet-test");
    check("config-defaults", cfg.trsvcid == 4420, "default port is not 4420");
    check("config-defaults", cfg.size == (64u << 20),
          "default size is not 64M");
    check("config-defaults", cfg.block_size == 512, "default bs is not 512");

    /* A later file= must replace an earlier one without leaking. */
    char err[128];
    nvmeof_target_cfg_defaults(&cfg);
    if (!nvmeof_target_cfg_parse(&cfg, "file=/tmp/a,file=/tmp/b", err,
                                 sizeof(err))) {
        fail("config-file", "repeated file= rejected: %s", err);
    } else {
        check("config-file",
              cfg.file != NULL && strcmp(cfg.file, "/tmp/b") == 0,
              "repeated file= did not take the last value");
    }
    free(cfg.file);
}

/* ------------------------------------------------------------------ */
/* Handshake and admin command set                                    */
/* ------------------------------------------------------------------ */

static struct nvmeof_target *make_target(const char *opts)
{
    struct nvmeof_target_cfg cfg;
    char err[128] = "";

    nvmeof_target_cfg_defaults(&cfg);
    if (!nvmeof_target_cfg_parse(&cfg, opts, err, sizeof(err))) {
        fail("make-target", "parse '%s': %s", opts ? opts : "", err);
        return NULL;
    }
    struct nvmeof_target *t = nvmeof_target_create(&cfg, err, sizeof(err));
    if (t == NULL) {
        fail("make-target", "create '%s': %s", opts ? opts : "", err);
    }
    free(cfg.file);
    return t;
}

static void test_handshake(void)
{
    struct host_mem *h = calloc(1, sizeof(*h));
    struct nvmeof_target *t = make_target("size=1M,bs=512");
    uint8_t rsp[NVME_CQE_SIZE];
    struct capsule c;

    if (t == NULL || h == NULL) {
        free(h);
        nvmeof_target_destroy(t);
        return;
    }
    h->rkey = HOST_RKEY;

    struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 100);
    check("handshake", aq != NULL, "admin queue not opened");
    if (aq == NULL) {
        goto out;
    }

    /* An I/O command before Connect must be refused, not executed. */
    cap_init(&c, NVME_CMD_READ, 1);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("pre-connect", rsp_sc(rsp) == NVME_SC_INVALID_OPCODE,
          "command before Connect was not rejected");

    /* Connect, admin queue. */
    do_connect(aq, h, 0, 31, "nvmet-test", "nqn.2014-08.org.nvmexpress:uuid:t",
               rsp);
    check("connect-admin", rsp_status(rsp) == 0, "admin Connect failed");
    check("connect-admin", nvme_get_le16(rsp + NVME_CQE_RESULT_OFF) == 1,
          "Connect did not return cntlid 1");
    check("connect-admin", nvmeof_queue_connected(aq), "queue not marked live");

    /* A second Connect on a live queue is a protocol error. */
    do_connect(aq, h, 0, 31, "nvmet-test", "nqn.2014-08.org.nvmexpress:uuid:t",
               rsp);
    check("connect-twice", rsp_sc(rsp) == NVMEOF_SC_CONNECT_CTRL_BUSY,
          "duplicate Connect was accepted");

    /* Wrong subsystem NQN. */
    struct nvmeof_queue *bad = nvmeof_target_open_queue(t, 199);
    do_connect(bad, h, 0, 31, "nqn.does.not.exist", "host", rsp);
    check("connect-badnqn", rsp_sc(rsp) == NVMEOF_SC_CONNECT_INVALID_PARAM,
          "Connect to an unknown subsystem was accepted");
    nvmeof_target_close_queue(t, 199);

    /* Property Get CAP: 8-byte attrib, MQES must match queue_depth - 1. */
    cap_init(&c, NVME_ADMIN_FABRICS, 2);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_GET;
    c.b[NVMEOF_PROP_ATTRIB_OFF] = 1;
    nvme_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_CAP);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    uint64_t cap = nvme_get_le64(rsp + NVME_CQE_RESULT_OFF);
    check("prop-cap", rsp_status(rsp) == 0, "Property Get CAP failed");
    check("prop-cap", (cap & 0xffff) == 127, "MQES is not queue_depth - 1");
    check("prop-cap", (cap >> 37) & 1,
          "CAP.CSS does not advertise the NVM set");

    /* CAP with a 4-byte access is invalid. */
    c.b[NVMEOF_PROP_ATTRIB_OFF] = 0;
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("prop-cap", rsp_sc(rsp) == NVME_SC_INVALID_FIELD,
          "32-bit CAP read was accepted");

    /* Property Get VS. */
    cap_init(&c, NVME_ADMIN_FABRICS, 3);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_GET;
    nvme_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_VS);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("prop-vs", nvme_get_le32(rsp + NVME_CQE_RESULT_OFF) == 0x00010400,
          "VS is not 1.4.0");

    /* Enable the controller and confirm CSTS.RDY follows CC.EN. */
    cap_init(&c, NVME_ADMIN_FABRICS, 4);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_SET;
    nvme_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_CC);
    nvme_put_le64(c.b + NVMEOF_PROP_VALUE_OFF,
                  NVME_CC_ENABLE | (6 << 16) | (4 << 20));
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("prop-cc", rsp_status(rsp) == 0, "Property Set CC failed");

    cap_init(&c, NVME_ADMIN_FABRICS, 5);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_GET;
    nvme_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_CSTS);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("prop-csts", nvme_get_le32(rsp + NVME_CQE_RESULT_OFF) & NVME_CSTS_RDY,
          "CSTS.RDY did not follow CC.EN");

    /* Identify Controller. */
    memset(h->buf, 0xa5, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 6);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ctrl", rsp_status(rsp) == 0, "Identify Controller failed");
    check("id-ctrl", strcmp((char *)h->buf + 768, "nvmet-test") == 0,
          "Identify Controller subnqn mismatch");
    uint32_t sgls = nvme_get_le32(h->buf + 536);
    check("id-ctrl", (sgls & (1u << 2)) != 0,
          "keyed SGL support not advertised (nvme-rdma requires it)");
    check("id-ctrl", nvme_get_le32(h->buf + 1792) == 4,
          "ioccsz is not 4; the host would send in-capsule write data");
    check("id-ctrl", h->buf[512] == 0x66 && h->buf[513] == 0x44,
          "sqes/cqes do not pin 64/16-byte entries");
    check("id-ctrl", nvme_get_le16(h->buf + 514) != 0, "maxcmd is zero");
    check("id-ctrl", nvme_get_le32(h->buf + 516) == 1, "nn is not 1");

    /* Identify Namespace: 1 MiB of 512-byte blocks. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 7);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns", nvme_get_le64(h->buf) == 2048, "nsze is not 1 MiB / 512");
    check("id-ns", h->buf[130] == 9, "LBA data size is not 2^9");

    /* Active namespace list. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 8);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS_ACTIVE_LIST);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns-list", nvme_get_le32(h->buf) == 1,
          "active namespace list does not start with nsid 1");
    check("id-ns-list", nvme_get_le32(h->buf + 4) == 0,
          "active namespace list is not zero-terminated");

    /* Namespace descriptor list must carry a well-formed UUID. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 9);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS_DESC_LIST);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns-desc", h->buf[0] == 3 && h->buf[1] == 16,
          "descriptor list does not lead with a 16-byte UUID");
    check("id-ns-desc",
          (h->buf[4 + 6] & 0xf0) == 0x40 && (h->buf[4 + 8] & 0xc0) == 0x80,
          "UUID version/variant bits are wrong");

    /* Unknown CNS. */
    cap_init(&c, NVME_ADMIN_IDENTIFY, 10);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, 0x55);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-bad-cns", rsp_sc(rsp) == NVME_SC_INVALID_FIELD,
          "unknown CNS was not rejected");

    /* Set Features: Number of Queues is clamped to what we can serve. */
    cap_init(&c, NVME_ADMIN_SET_FEATURES, 11);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_FEAT_NUM_QUEUES);
    nvme_put_le32(c.b + NVME_SQE_CDW11_OFF, 0x00ff00ff);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    uint32_t nq = nvme_get_le32(rsp + NVME_CQE_RESULT_OFF);
    check("set-num-queues", rsp_status(rsp) == 0, "Set Features failed");
    check("set-num-queues", (nq & 0xffff) == 7 && (nq >> 16) == 7,
          "queue count was not clamped to the configured maximum");

    /* Keep Alive. */
    cap_init(&c, NVME_ADMIN_KEEP_ALIVE, 12);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("keep-alive", rsp_status(rsp) == 0, "Keep Alive failed");

    /* Async Event Request must be held, not completed. */
    cap_init(&c, NVME_ADMIN_ASYNC_EVENT, 13);
    int rc = nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("async-event", rc == NVMEOF_EXEC_HELD,
          "Async Event Request was completed instead of held");

    /* Unknown admin opcode. */
    cap_init(&c, 0xfe, 14);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("bad-admin-op", rsp_sc(rsp) == NVME_SC_INVALID_OPCODE,
          "unknown admin opcode was not rejected");

    /* The response capsule must echo the command identifier. */
    check("cqe-cid", nvme_get_le16(rsp + NVME_CQE_CID_OFF) == 14,
          "response capsule did not echo the command id");

    /* A truncated capsule is a transport error, not a status. */
    check("short-capsule",
          nvmeof_queue_exec(aq, c.b, 32, &host_ops, h, rsp) < 0,
          "a 32-byte capsule was accepted");

out:
    free(h);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* I/O                                                                */
/* ------------------------------------------------------------------ */

static void test_io(void)
{
    struct host_mem *h = calloc(1, sizeof(*h));
    struct nvmeof_target *t = make_target("size=1M,bs=512");
    uint8_t rsp[NVME_CQE_SIZE];
    struct capsule c;
    const char *hostnqn = "nqn.2014-08.org.nvmexpress:uuid:io";

    if (t == NULL || h == NULL) {
        free(h);
        nvmeof_target_destroy(t);
        return;
    }
    h->rkey = HOST_RKEY;

    struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 100);
    do_connect(aq, h, 0, 31, "nvmet-test", hostnqn, rsp);

    /* An I/O queue Connect from a different host must be refused. */
    struct nvmeof_queue *impostor = nvmeof_target_open_queue(t, 198);
    do_connect(impostor, h, 1, 31, "nvmet-test", "nqn.someone.else", rsp);
    check("io-connect-host", rsp_sc(rsp) == NVMEOF_SC_CONNECT_INVALID_HOST,
          "an I/O queue from a foreign host NQN was accepted");
    nvmeof_target_close_queue(t, 198);

    struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 101);
    do_connect(ioq, h, 1, 31, "nvmet-test", hostnqn, rsp);
    check("io-connect", rsp_status(rsp) == 0, "I/O queue Connect failed");
    check("io-connect", nvmeof_queue_id(ioq) == 1, "queue id is not 1");

    /* Write eight blocks of a recognisable pattern. */
    const uint32_t nblocks = 8;
    const uint32_t nbytes = nblocks * 512;
    uint64_t data_addr = HOST_BASE + 8192;
    uint8_t *src = h->buf + 8192;
    for (uint32_t i = 0; i < nbytes; i++) {
        src[i] = (uint8_t)(i * 7 + 3);
    }

    cap_init(&c, NVME_CMD_WRITE, 20);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 16); /* SLBA */
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
    cap_keyed_sgl(&c, data_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-write", rsp_status(rsp) == 0, "Write failed");

    /* Read it back into a different part of host memory and compare. */
    uint64_t back_addr = HOST_BASE + 32768;
    uint8_t *back = h->buf + 32768;
    memset(back, 0, nbytes);

    cap_init(&c, NVME_CMD_READ, 21);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
    cap_keyed_sgl(&c, back_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-read", rsp_status(rsp) == 0, "Read failed");
    check("io-read", memcmp(src, back, nbytes) == 0,
          "read-after-write returned different bytes");

    /* Reading an untouched LBA must give zeros, not stale data. */
    memset(back, 0xee, 512);
    cap_init(&c, NVME_CMD_READ, 22);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 1000);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    bool zeroed = true;
    for (int i = 0; i < 512; i++) {
        zeroed = zeroed && back[i] == 0;
    }
    check("io-read-hole", zeroed, "an unwritten block did not read back zero");

    /* Write Zeroes over the middle of the pattern. */
    cap_init(&c, NVME_CMD_WRITE_ZEROES, 23);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 18);
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, 1); /* two blocks */
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-write-zeroes", rsp_status(rsp) == 0, "Write Zeroes failed");

    memset(back, 0xee, nbytes);
    cap_init(&c, NVME_CMD_READ, 24);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
    cap_keyed_sgl(&c, back_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-write-zeroes", memcmp(back, src, 1024) == 0,
          "Write Zeroes disturbed the blocks before it");
    zeroed = true;
    for (int i = 1024; i < 2048; i++) {
        zeroed = zeroed && back[i] == 0;
    }
    check("io-write-zeroes", zeroed, "Write Zeroes left non-zero bytes");
    check("io-write-zeroes",
          memcmp(back + 2048, src + 2048, nbytes - 2048) == 0,
          "Write Zeroes disturbed the blocks after it");

    /* Flush and Dataset Management are accepted no-ops. */
    cap_init(&c, NVME_CMD_FLUSH, 25);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-flush", rsp_status(rsp) == 0, "Flush failed");

    cap_init(&c, NVME_CMD_DSM, 26);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-dsm", rsp_status(rsp) == 0, "Dataset Management failed");

    /* Past the end of the namespace. */
    cap_init(&c, NVME_CMD_READ, 27);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 2047);
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, 7); /* eight blocks, one fits */
    cap_keyed_sgl(&c, back_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-lba-range", rsp_sc(rsp) == NVME_SC_LBA_RANGE,
          "a read running off the end of the namespace was accepted");

    /* Starting beyond the last block. */
    cap_init(&c, NVME_CMD_READ, 28);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 1ull << 40);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-lba-range", rsp_sc(rsp) == NVME_SC_LBA_RANGE,
          "a read starting past the namespace was accepted");

    /* Wrong namespace. */
    cap_init(&c, NVME_CMD_READ, 29);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 7);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-nsid", rsp_sc(rsp) == NVME_SC_INVALID_NS,
          "an unknown namespace id was accepted");

    /* A bad rkey must surface as a transfer error, not a wrong-data read. */
    cap_init(&c, NVME_CMD_READ, 30);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY + 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-rkey", rsp_sc(rsp) == NVME_SC_DATA_XFER_ERROR,
          "a read with an unknown rkey did not report a transfer error");

    /* An SGL shorter than the command's block count is malformed. */
    cap_init(&c, NVME_CMD_WRITE, 31);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le32(c.b + NVME_SQE_CDW12_OFF, 7);
    cap_keyed_sgl(&c, data_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-short-sgl", rsp_sc(rsp) == NVME_SC_SGL_INVALID_DATA,
          "a write with an undersized SGL was accepted");

    /* An unsupported SGL descriptor type. */
    cap_init(&c, NVME_CMD_READ, 32);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    c.b[NVME_SQE_DPTR_OFF + 15] = NVME_SGL_TYPE_SEGMENT << 4;
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-sgl-type", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "an SGL segment descriptor was accepted");

    /* Reading into an in-capsule descriptor is not a thing. */
    cap_init(&c, NVME_CMD_READ, 33);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    uint8_t scratch[512] = {0};
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-read", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "a Read into an in-capsule SGL was accepted");

    /* An in-capsule write, the one direction that form makes sense. */
    for (int i = 0; i < 512; i++) {
        scratch[i] = (uint8_t)(0x40 + (i & 0x1f));
    }
    cap_init(&c, NVME_CMD_WRITE, 34);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-write", rsp_status(rsp) == 0,
          "an in-capsule Write was rejected");

    memset(back, 0, 512);
    cap_init(&c, NVME_CMD_READ, 35);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-write", memcmp(back, scratch, 512) == 0,
          "in-capsule Write data did not land in the namespace");

    /* A capsule claiming more in-capsule data than it carries. */
    cap_init(&c, NVME_CMD_WRITE, 36);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    c.len = NVME_SQE_SIZE + 16;
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-short", rsp_sc(rsp) == NVME_SC_SGL_INVALID_DATA,
          "a capsule shorter than its in-capsule SGL was accepted");

    /* Statistics should reflect the traffic we just generated. */
    const struct nvmeof_target_stats *st = nvmeof_target_stats(t);
    check("stats", st->connects == 2, "connect count is wrong");
    check("stats", st->io_cmds > 0, "no I/O commands counted");
    check("stats", st->read_bytes >= nbytes, "read bytes not counted");
    check("stats", st->write_bytes >= nbytes, "write bytes not counted");
    check("stats", st->errors > 0, "rejected commands were not counted");

    /* Closing the admin queue tears the whole association down. */
    check("teardown", nvmeof_target_queue_count(t) == 2,
          "unexpected queue count before teardown");
    nvmeof_target_close_queue(t, 100);
    check("teardown", nvmeof_target_queue_count(t) == 0,
          "closing the admin queue left I/O queues behind");

    free(h);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* Discovery                                                          */
/* ------------------------------------------------------------------ */

static void test_discovery(void)
{
    struct host_mem *h = calloc(1, sizeof(*h));
    struct nvmeof_target *t = make_target("size=1M,ip=192.168.200.1,port=4420");
    uint8_t rsp[NVME_CQE_SIZE];
    struct capsule c;

    if (t == NULL || h == NULL) {
        free(h);
        nvmeof_target_destroy(t);
        return;
    }
    h->rkey = HOST_RKEY;

    struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
    do_connect(aq, h, 0, 31, NVMEOF_DISCOVERY_NQN, "nqn.host", rsp);
    check("discovery", rsp_status(rsp) == 0,
          "Connect to the discovery subsystem failed");

    const size_t want = NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE;
    memset(h->buf, 0xcc, want);
    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, 40);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF,
                  NVME_LOG_DISCOVERY | (uint32_t)(((want / 4) - 1) << 16));
    cap_keyed_sgl(&c, HOST_BASE, (uint32_t)want, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery", rsp_status(rsp) == 0, "Get Log Page 0x70 failed");
    check("discovery", nvme_get_le64(h->buf + NVMEOF_DISC_NUMREC_OFF) == 1,
          "discovery log does not report exactly one record");

    const uint8_t *e = h->buf + NVMEOF_DISC_HDR_SIZE;
    check("discovery", e[NVMEOF_DISC_E_TRTYPE_OFF] == NVMEOF_TRTYPE_RDMA,
          "discovery record is not RDMA");
    check("discovery", e[NVMEOF_DISC_E_ADRFAM_OFF] == NVMEOF_ADRFAM_IPV4,
          "discovery record is not IPv4");
    check("discovery",
          strcmp((const char *)e + NVMEOF_DISC_E_SUBNQN_OFF, "nvmet-test") == 0,
          "discovery record advertises the wrong subnqn");
    check("discovery",
          strcmp((const char *)e + NVMEOF_DISC_E_TRADDR_OFF, "192.168.200.1") ==
              0,
          "discovery record advertises the wrong traddr");
    check("discovery",
          strcmp((const char *)e + NVMEOF_DISC_E_TRSVCID_OFF, "4420") == 0,
          "discovery record advertises the wrong trsvcid");
    check("discovery", e[NVMEOF_DISC_E_TSAS_OFF + 2] == NVMEOF_RDMA_CMS_RDMA_CM,
          "discovery record does not name RDMA_CM");

    /* The host reads the header first, then re-reads with an offset. */
    memset(h->buf, 0, want);
    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, 41);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF,
                  NVME_LOG_DISCOVERY |
                      (uint32_t)(((NVMEOF_DISC_ENTRY_SIZE / 4) - 1) << 16));
    nvme_put_le64(c.b + NVME_SQE_CDW12_OFF, NVMEOF_DISC_HDR_SIZE);
    cap_keyed_sgl(&c, HOST_BASE, NVMEOF_DISC_ENTRY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery-offset", rsp_status(rsp) == 0,
          "offset Get Log Page failed");
    check("discovery-offset",
          h->buf[NVMEOF_DISC_E_TRTYPE_OFF] == NVMEOF_TRTYPE_RDMA,
          "offset read did not start at the first record");

    /* An unknown log page is rejected rather than silently zero-filled. */
    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, 42);
    nvme_put_le32(c.b + NVME_SQE_CDW10_OFF, 0x7e);
    cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery", rsp_sc(rsp) == NVME_SC_INVALID_FIELD,
          "an unknown log page identifier was accepted");

    free(h);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* File-backed namespace                                              */
/* ------------------------------------------------------------------ */

static void write_one_block(struct nvmeof_queue *q, struct host_mem *h,
                            uint64_t slba, const uint8_t *data)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    memcpy(h->buf, data, 512);
    cap_init(&c, NVME_CMD_WRITE, 50);
    nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, slba);
    cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
    if (rsp_status(rsp) != 0) {
        fail("file-backing", "write failed with status 0x%04x",
             rsp_status(rsp));
    }
}

static void test_file_backing(void)
{
    char path[] = "/tmp/ernic-nvmeof-testXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fail("file-backing", "mkstemp failed");
        return;
    }
    close(fd);

    struct host_mem *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        unlink(path);
        return;
    }
    h->rkey = HOST_RKEY;

    uint8_t pattern[512];
    for (int i = 0; i < 512; i++) {
        pattern[i] = (uint8_t)(i ^ 0x5a);
    }

    char opts[256];
    snprintf(opts, sizeof(opts), "size=1M,file=%s", path);

    /* First instance: connect, write one block, shut down. */
    struct nvmeof_target *t = make_target(opts);
    if (t != NULL) {
        uint8_t rsp[NVME_CQE_SIZE];
        struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp);
        struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 2);
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp);
        write_one_block(ioq, h, 3, pattern);

        struct capsule c;
        cap_init(&c, NVME_CMD_FLUSH, 51);
        nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
        nvmeof_target_destroy(t);
    }

    /* Second instance over the same file must see the block. */
    t = make_target(opts);
    if (t != NULL) {
        uint8_t rsp[NVME_CQE_SIZE];
        struct capsule c;
        struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp);
        struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 2);
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp);

        memset(h->buf, 0, 512);
        cap_init(&c, NVME_CMD_READ, 52);
        nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 3);
        cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
        nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
        check("file-backing", rsp_status(rsp) == 0, "read back failed");
        check("file-backing", memcmp(h->buf, pattern, 512) == 0,
              "file-backed namespace did not survive a restart");
        nvmeof_target_destroy(t);
    }

    /* A RAM namespace, by contrast, starts empty every time. */
    t = make_target("size=1M");
    if (t != NULL) {
        uint8_t rsp[NVME_CQE_SIZE];
        struct capsule c;
        struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp);
        struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 2);
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp);

        memset(h->buf, 0xff, 512);
        cap_init(&c, NVME_CMD_READ, 53);
        nvme_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        nvme_put_le64(c.b + NVME_SQE_CDW10_OFF, 3);
        cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
        nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
        bool zeroed = true;
        for (int i = 0; i < 512; i++) {
            zeroed = zeroed && h->buf[i] == 0;
        }
        check("ram-backing", zeroed, "a fresh RAM namespace was not zero");
        nvmeof_target_destroy(t);
    }

    unlink(path);
    free(h);
}

/* ------------------------------------------------------------------ */
/* Queue table limits                                                 */
/* ------------------------------------------------------------------ */

static void test_queue_table(void)
{
    struct nvmeof_target *t = make_target("size=1M,queues=2");
    if (t == NULL) {
        return;
    }

    /* max_queues I/O queues plus the admin queue. */
    check("queue-table", nvmeof_target_open_queue(t, 1) != NULL, "queue 1");
    check("queue-table", nvmeof_target_open_queue(t, 2) != NULL, "queue 2");
    check("queue-table", nvmeof_target_open_queue(t, 3) != NULL, "queue 3");
    check("queue-table", nvmeof_target_open_queue(t, 4) == NULL,
          "the queue table grew past its configured size");

    /* Opening a live handle again returns the same queue, not a new slot. */
    check("queue-table",
          nvmeof_target_open_queue(t, 2) == nvmeof_target_find_queue(t, 2),
          "reopening a live handle allocated a second slot");
    check("queue-table", nvmeof_target_queue_count(t) == 3,
          "queue count is wrong");

    nvmeof_target_close_queue(t, 2);
    check("queue-table", nvmeof_target_find_queue(t, 2) == NULL,
          "a closed queue is still findable");
    check("queue-table", nvmeof_target_open_queue(t, 5) != NULL,
          "a closed slot was not reused");

    /* Closing an unknown handle must be harmless. */
    nvmeof_target_close_queue(t, 999);

    nvmeof_target_destroy(t);
}

int main(void)
{
    test_status_encoding();
    test_config();
    test_handshake();
    test_io();
    test_discovery();
    test_file_backing();
    test_queue_table();

    if (failures == 0) {
        printf("PASS nvmeof-target\n");
    } else {
        printf("%d nvmeof-target check(s) failed\n", failures);
    }
    return failures;
}
