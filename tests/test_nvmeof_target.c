/*
 * test_nvmeof_target.c — unit tests for the in-process NVMe-oF controller
 *
 * The controller in src/nvmeof/nvmeof_target.c is deliberately transport- and
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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "byteorder.h"
#include "ernic_attributes.h"
#include "nvmeof/nvmeof.h"
#include "nvmeof/nvmeof_target.h"
#include "nvmeof/nvmeof_target_priv.h"

#define HOST_BUF_SIZE (256 * 1024)
#define HOST_BASE     0x40000000ull
#define HOST_RKEY     0x1234u

static int failures;

static void fail(const char *name, const char *fmt, ...)
    ERNIC_PRINTF_FORMAT(2, 3);

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
    ernic_put_le16(c->b + NVME_SQE_CID_OFF, cid);
}

static void cap_keyed_sgl(struct capsule *c, uint64_t addr, uint32_t len,
                          uint32_t key)
{
    uint8_t *d = c->b + NVME_SQE_DPTR_OFF;
    ernic_put_le64(d, addr);
    d[8] = (uint8_t)len;
    d[9] = (uint8_t)(len >> 8);
    d[10] = (uint8_t)(len >> 16);
    ernic_put_le32(d + 11, key);
    d[15] = NVME_SGL_TYPE_KEYED_DATA << 4;
}

static void cap_inline_sgl(struct capsule *c, const void *data, uint32_t len)
{
    uint8_t *d = c->b + NVME_SQE_DPTR_OFF;
    ernic_put_le64(d, 0);
    ernic_put_le32(d + 8, len);
    d[15] =
        (uint8_t)((NVME_SGL_TYPE_DATA_BLOCK << 4) | NVME_SGL_SUBTYPE_OFFSET);
    memcpy(c->b + NVME_SQE_SIZE, data, len);
    c->len = NVME_SQE_SIZE + len;
}

/* A data block descriptor naming a bare address: no key, not in capsule. */
static void cap_addr_sgl(struct capsule *c, uint64_t addr, uint32_t len)
{
    uint8_t *d = c->b + NVME_SQE_DPTR_OFF;
    ernic_put_le64(d, addr);
    ernic_put_le32(d + 8, len);
    d[15] = NVME_SGL_TYPE_DATA_BLOCK << 4;
}

static uint16_t rsp_status(const uint8_t *rsp)
{
    return ernic_get_le16(rsp + NVME_CQE_STATUS_OFF);
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

/*
 * Build a Connect for queue qid with its payload staged at the start of host
 * memory, so a caller can change a field before running it.
 */
static void connect_capsule(struct capsule *c, struct host_mem *h, uint16_t qid,
                            uint16_t sqsize, const char *subnqn,
                            const char *hostnqn)
{
    uint64_t addr = HOST_BASE;

    /* The CID is an opaque 16-bit tag, so wrapping it is harmless. */
    cap_init(c, NVME_ADMIN_FABRICS, (uint16_t)(0x10u + qid));
    c->b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_CONNECT;
    ernic_put_le16(c->b + NVMEOF_CONNECT_QID_OFF, qid);
    ernic_put_le16(c->b + NVMEOF_CONNECT_SQSIZE_OFF, sqsize);
    cap_keyed_sgl(c, addr, NVMEOF_CONNECT_DATA_SIZE, HOST_RKEY);
    stage_connect_data(h, addr, subnqn, hostnqn);
}

static int do_connect(struct nvmeof_queue *q, struct host_mem *h, uint16_t qid,
                      uint16_t sqsize, const char *subnqn, const char *hostnqn,
                      uint8_t *rsp)
{
    struct capsule c;

    connect_capsule(&c, h, qid, sqsize, subnqn, hostnqn);
    return nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
}

/*
 * Property Set of off to val.  Returns the status, or UINT16_MAX if the
 * target produced no response.
 */
static uint16_t prop_set(struct nvmeof_queue *q, struct host_mem *h,
                         uint16_t cid, uint32_t off, uint64_t val)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    cap_init(&c, NVME_ADMIN_FABRICS, cid);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_SET;
    ernic_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, off);
    ernic_put_le64(c.b + NVMEOF_PROP_VALUE_OFF, val);
    if (nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp) !=
        NVMEOF_EXEC_DONE) {
        return UINT16_MAX;
    }
    return rsp_status(rsp);
}

/*
 * Property Get of off, a 4-byte access unless attrib is 1, into *val.
 * Returns the status, or UINT16_MAX if the target produced no response;
 * *val is only meaningful on 0.
 */
static uint16_t prop_get(struct nvmeof_queue *q, struct host_mem *h,
                         uint16_t cid, uint32_t off, uint8_t attrib,
                         uint64_t *val)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    *val = 0;
    cap_init(&c, NVME_ADMIN_FABRICS, cid);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_GET;
    c.b[NVMEOF_PROP_ATTRIB_OFF] = attrib;
    ernic_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, off);
    if (nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp) !=
        NVMEOF_EXEC_DONE) {
        return UINT16_MAX;
    }
    *val = ernic_get_le64(rsp + NVME_CQE_RESULT_OFF);
    return rsp_status(rsp);
}

static uint16_t set_cc(struct nvmeof_queue *q, struct host_mem *h, uint16_t cid,
                       uint32_t cc)
{
    return prop_set(q, h, cid, NVMEOF_PROP_CC, cc);
}

/*
 * Read CSTS into *csts. Returns the status of the Property Get, or
 * UINT16_MAX if the target produced no response; *csts is only meaningful
 * on 0.
 */
static uint16_t get_csts(struct nvmeof_queue *q, struct host_mem *h,
                         uint16_t cid, uint32_t *csts)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    *csts = 0;
    cap_init(&c, NVME_ADMIN_FABRICS, cid);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_PROPERTY_GET;
    ernic_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_CSTS);
    if (nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp) !=
        NVMEOF_EXEC_DONE) {
        return UINT16_MAX;
    }
    *csts = ernic_get_le32(rsp + NVME_CQE_RESULT_OFF);
    return rsp_status(rsp);
}

/*
 * Get Log Page lid: numd + 1 dwords from byte offset off, into an SGL of
 * len bytes at the start of host memory.  Returns the status, or UINT16_MAX
 * if the target produced no response.
 */
static uint16_t get_log(struct nvmeof_queue *q, struct host_mem *h,
                        uint16_t cid, uint8_t lid, uint32_t numd, uint64_t off,
                        uint32_t len)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, cid);
    /* NUMDL is cdw10[31:16], NUMDU cdw11[15:0]. */
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, lid | ((numd & 0xffffu) << 16));
    ernic_put_le32(c.b + NVME_SQE_CDW11_OFF, numd >> 16);
    ernic_put_le64(c.b + NVME_SQE_CDW12_OFF, off);
    cap_keyed_sgl(&c, HOST_BASE, len, HOST_RKEY);
    if (nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp) !=
        NVMEOF_EXEC_DONE) {
        return UINT16_MAX;
    }
    return rsp_status(rsp);
}

/* True if the first len bytes of host memory are all val. */
static bool host_bytes_are(const struct host_mem *h, size_t len, uint8_t val)
{
    for (size_t i = 0; i < len; i++) {
        if (h->buf[i] != val) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Configuration parsing                                              */
/* ------------------------------------------------------------------ */

/* As long as the Identify Controller MN and SN fields */
#define MODEL_40  "0123456789abcdefghijklmnopqrstuvwxyzABCD"
#define SERIAL_20 "0123456789ABCDEFGHIJ"

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
    {"queues=64", true, "queue count at NVMEOF_MAX_QUEUES"},
    {"model=widget,serial=SN1", true, "identify strings"},
    {"model=" MODEL_40, true, "model at its 40 characters"},
    {"serial=" SERIAL_20, true, "serial at its 20 characters"},
    {"model=" MODEL_40 "x", false, "model one character too long"},
    {"serial=" SERIAL_20 "x", false, "serial one character too long"},
    {"model=", false, "empty model"},
    {"serial=", false, "empty serial"},
    {"model=ACME \xc2\xae", false, "model not ASCII"},
    {"serial=SN\t1", false, "serial with a control character"},
    {"size=64M,bs=4096,nsid=1", true, "combination"},
    {"size", false, "missing ="},
    {"size=", false, "empty size"},
    {"size=0", false, "zero size"},
    {"size=-1", false, "negative size"},
    {"size=12Q", false, "bad size suffix"},
    {"size=0x100000", false, "hex size"},
    {"size= 1M", false, "size with a leading space"},
    {"bs=777", false, "unsupported block size"},
    {"size=1000,bs=512", false, "size not a multiple of bs"},
    {"nsid=0", false, "reserved nsid"},
    {"ip=999.1.1.1", false, "octet out of range"},
    {"ip=10.0.0", false, "short address"},
    {"ip=10.0.0.1x", false, "address with trailing text"},
    {"ip=-1.0.0.1", false, "negative octet"},
    {"port=0", false, "zero port"},
    {"port=70000", false, "port out of range"},
    {"port=0x1145", false, "hex port"},
    {"nsid=-1", false, "negative nsid"},
    {"queues=0", false, "zero queues"},
    {"queues=65", false, "one queue past NVMEOF_MAX_QUEUES"},
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
    check("config-defaults", !cfg.size_set, "the default size counts as set");

    /* Only an explicit size= overrides an existing image's size. */
    char err[128];
    if (!nvmeof_target_cfg_parse(&cfg, "bs=4096", err, sizeof(err))) {
        fail("config-size-set", "bs=4096 rejected: %s", err);
    }
    check("config-size-set", !cfg.size_set, "size counts as set without size=");
    if (!nvmeof_target_cfg_parse(&cfg, "size=64M", err, sizeof(err))) {
        fail("config-size-set", "size=64M rejected: %s", err);
    }
    check("config-size-set", cfg.size_set,
          "size= equal to the default does not count as set");

    /* A model and serial that fill their fields are kept whole. */
    nvmeof_target_cfg_defaults(&cfg);
    if (!nvmeof_target_cfg_parse(&cfg, "model=" MODEL_40 ",serial=" SERIAL_20,
                                 err, sizeof(err))) {
        fail("config-ident", "full-length model and serial rejected: %s", err);
    } else {
        check("config-ident", strcmp(cfg.model, MODEL_40) == 0,
              "a full-length model was not kept whole");
        check("config-ident", strcmp(cfg.serial, SERIAL_20) == 0,
              "a full-length serial was not kept whole");
    }

    /* A later file= must replace an earlier one without leaking. */
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
        free(h);
        nvmeof_target_destroy(t);
        return;
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
    check("connect-admin", ernic_get_le16(rsp + NVME_CQE_RESULT_OFF) == 1,
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
    ernic_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_CAP);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    uint64_t cap = ernic_get_le64(rsp + NVME_CQE_RESULT_OFF);
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
    ernic_put_le32(c.b + NVMEOF_PROP_OFFSET_OFF, NVMEOF_PROP_VS);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("prop-vs", ernic_get_le32(rsp + NVME_CQE_RESULT_OFF) == 0x00010400,
          "VS is not 1.4.0");

    /* Enable the controller and confirm CSTS.RDY follows CC.EN. */
    const uint32_t cc_en = NVME_CC_ENABLE | (6 << 16) | (4 << 20);
    uint32_t csts;
    check("prop-cc", set_cc(aq, h, 4, cc_en) == 0, "Property Set CC failed");
    check("prop-csts",
          get_csts(aq, h, 5, &csts) == 0 && (csts & NVME_CSTS_RDY) != 0,
          "CSTS.RDY did not follow CC.EN");

    /* Identify Controller. */
    memset(h->buf, 0xa5, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 6);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ctrl", rsp_status(rsp) == 0, "Identify Controller failed");
    check("id-ctrl", strcmp((char *)h->buf + 768, "nvmet-test") == 0,
          "Identify Controller subnqn mismatch");
    uint32_t sgls = ernic_get_le32(h->buf + 536);
    check("id-ctrl", (sgls & (1u << 2)) != 0,
          "keyed SGL support not advertised (nvme-rdma requires it)");
    check("id-ctrl", ernic_get_le32(h->buf + 1792) == 4,
          "ioccsz is not 4; the host would send in-capsule write data");
    check("id-ctrl", h->buf[512] == 0x66 && h->buf[513] == 0x44,
          "sqes/cqes do not pin 64/16-byte entries");
    check("id-ctrl", ernic_get_le16(h->buf + 514) != 0, "maxcmd is zero");
    check("id-ctrl", ernic_get_le32(h->buf + 516) == 1, "nn is not 1");
    check("id-ctrl", h->buf[525] == 0,
          "a RAM namespace advertises a volatile write cache");

    /* Identify Namespace: 1 MiB of 512-byte blocks. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 7);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns", ernic_get_le64(h->buf) == 2048, "nsze is not 1 MiB / 512");
    check("id-ns", h->buf[130] == 9, "LBA data size is not 2^9");

    /* Active namespace list. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 8);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS_ACTIVE_LIST);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns-list", ernic_get_le32(h->buf) == 1,
          "active namespace list does not start with nsid 1");
    check("id-ns-list", ernic_get_le32(h->buf + 4) == 0,
          "active namespace list is not zero-terminated");

    /* Namespace descriptor list must carry a well-formed UUID. */
    memset(h->buf, 0, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 9);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_NS_DESC_LIST);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-ns-desc", h->buf[0] == 3 && h->buf[1] == 16,
          "descriptor list does not lead with a 16-byte UUID");
    check("id-ns-desc",
          (h->buf[4 + 6] & 0xf0) == 0x40 && (h->buf[4 + 8] & 0xc0) == 0x80,
          "UUID version/variant bits are wrong");

    /* Unknown CNS. */
    cap_init(&c, NVME_ADMIN_IDENTIFY, 10);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, 0x55);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("id-bad-cns", rsp_sc(rsp) == NVME_SC_INVALID_FIELD,
          "unknown CNS was not rejected");

    /* Set Features: Number of Queues is clamped to what we can serve. */
    cap_init(&c, NVME_ADMIN_SET_FEATURES, 11);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_FEAT_NUM_QUEUES);
    ernic_put_le32(c.b + NVME_SQE_CDW11_OFF, 0x00ff00ff);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    uint32_t nq = ernic_get_le32(rsp + NVME_CQE_RESULT_OFF);
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
    check("cqe-cid", ernic_get_le16(rsp + NVME_CQE_CID_OFF) == 14,
          "response capsule did not echo the command id");

    /* A truncated capsule is a transport error, not a status. */
    check("short-capsule",
          nvmeof_queue_exec(aq, c.b, 32, &host_ops, h, rsp) < 0,
          "a 32-byte capsule was accepted");

    /*
     * A host shuts down by setting CC.SHN with CC.EN still set. The shutdown
     * stays reported until a reset clears CC.EN, and re-enabling after it
     * reports a ready controller that is not shut down.
     */
    check("cc-shutdown", set_cc(aq, h, 15, cc_en | NVME_CC_SHN_NORMAL) == 0,
          "Property Set CC.SHN failed");
    check("cc-shutdown",
          get_csts(aq, h, 16, &csts) == 0 &&
              (csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT,
          "CSTS.SHST did not complete for CC.SHN with CC.EN set");
    check("cc-shutdown-held", set_cc(aq, h, 17, cc_en) == 0,
          "Property Set CC failed");
    check("cc-shutdown-held",
          get_csts(aq, h, 18, &csts) == 0 &&
              (csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT,
          "CSTS.SHST was cleared by a CC write that was not a reset");
    check("cc-reset", set_cc(aq, h, 19, cc_en & ~NVME_CC_ENABLE) == 0,
          "Property Set CC failed");
    check("cc-reset", get_csts(aq, h, 20, &csts) == 0 && csts == 0,
          "CSTS is not clear after a reset");
    check("cc-reenable", set_cc(aq, h, 21, cc_en) == 0,
          "Property Set CC failed");
    check("cc-reenable",
          get_csts(aq, h, 22, &csts) == 0 && csts == NVME_CSTS_RDY,
          "CSTS is not RDY alone after re-enabling");

    /*
     * A shutdown requested with CC.EN clear is reported too, but enabling the
     * controller afterwards mustn't carry it over.
     */
    check("cc-shutdown-disabled",
          set_cc(aq, h, 23, (cc_en & ~NVME_CC_ENABLE) | NVME_CC_SHN_NORMAL) ==
              0,
          "Property Set CC.SHN failed");
    check("cc-shutdown-disabled",
          get_csts(aq, h, 24, &csts) == 0 && csts == NVME_CSTS_SHST_CMPLT,
          "CSTS.SHST did not complete for CC.SHN with CC.EN clear");
    check("cc-enable-after-shutdown", set_cc(aq, h, 25, cc_en) == 0,
          "Property Set CC failed");
    check("cc-enable-after-shutdown",
          get_csts(aq, h, 26, &csts) == 0 && csts == NVME_CSTS_RDY,
          "CSTS.SHST carried over from a shutdown while disabled");

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
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 16); /* SLBA */
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
    cap_keyed_sgl(&c, data_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-write", rsp_status(rsp) == 0, "Write failed");

    /* Read it back into a different part of host memory and compare. */
    uint64_t back_addr = HOST_BASE + 32768;
    uint8_t *back = h->buf + 32768;
    memset(back, 0, nbytes);

    cap_init(&c, NVME_CMD_READ, 21);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
    cap_keyed_sgl(&c, back_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-read", rsp_status(rsp) == 0, "Read failed");
    check("io-read", memcmp(src, back, nbytes) == 0,
          "read-after-write returned different bytes");

    /* Reading an untouched LBA must give zeros, not stale data. */
    memset(back, 0xee, 512);
    cap_init(&c, NVME_CMD_READ, 22);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 1000);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    bool zeroed = true;
    for (int i = 0; i < 512; i++) {
        zeroed = zeroed && back[i] == 0;
    }
    check("io-read-hole", zeroed, "an unwritten block did not read back zero");

    /* Write Zeroes over the middle of the pattern. */
    cap_init(&c, NVME_CMD_WRITE_ZEROES, 23);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 18);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, 1); /* two blocks */
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-write-zeroes", rsp_status(rsp) == 0, "Write Zeroes failed");

    memset(back, 0xee, nbytes);
    cap_init(&c, NVME_CMD_READ, 24);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, nblocks - 1);
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

    /* On a RAM namespace, Flush, FUA and Dataset Management are accepted
     * no-ops. */
    cap_init(&c, NVME_CMD_WRITE_ZEROES, 28);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 18);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, NVME_RW_FUA);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-fua", rsp_status(rsp) == 0, "Write Zeroes with FUA failed");

    cap_init(&c, NVME_CMD_FLUSH, 25);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-flush", rsp_status(rsp) == 0, "Flush failed");

    cap_init(&c, NVME_CMD_DSM, 26);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-dsm", rsp_status(rsp) == 0, "Dataset Management failed");

    /* Past the end of the namespace. */
    cap_init(&c, NVME_CMD_READ, 27);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 2047);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, 7); /* eight blocks, one fits */
    cap_keyed_sgl(&c, back_addr, nbytes, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-lba-range", rsp_sc(rsp) == NVME_SC_LBA_RANGE,
          "a read running off the end of the namespace was accepted");

    /* Starting beyond the last block. */
    cap_init(&c, NVME_CMD_READ, 28);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 1ull << 40);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-lba-range", rsp_sc(rsp) == NVME_SC_LBA_RANGE,
          "a read starting past the namespace was accepted");

    /* Wrong namespace. */
    cap_init(&c, NVME_CMD_READ, 29);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 7);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-nsid", rsp_sc(rsp) == NVME_SC_INVALID_NS,
          "an unknown namespace id was accepted");

    /* A bad rkey must surface as a transfer error, not a wrong-data read. */
    cap_init(&c, NVME_CMD_READ, 30);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY + 1);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-rkey", rsp_sc(rsp) == NVME_SC_DATA_XFER_ERROR,
          "a read with an unknown rkey did not report a transfer error");

    /* An SGL shorter than the command's block count is malformed. */
    cap_init(&c, NVME_CMD_WRITE, 31);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, 7);
    cap_keyed_sgl(&c, data_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-short-sgl", rsp_sc(rsp) == NVME_SC_SGL_INVALID_DATA,
          "a write with an undersized SGL was accepted");

    /* An unsupported SGL descriptor type. */
    cap_init(&c, NVME_CMD_READ, 32);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    c.b[NVME_SQE_DPTR_OFF + 15] = NVME_SGL_TYPE_SEGMENT << 4;
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-bad-sgl-type", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "an SGL segment descriptor was accepted");

    /* Reading into an in-capsule descriptor is not a thing. */
    cap_init(&c, NVME_CMD_READ, 33);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    uint8_t scratch[512] = {0};
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-read", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "a Read into an in-capsule SGL was accepted");

    /*
     * A data block that isn't in capsule has no key, so passing it to the
     * DMA ops would ask for key 0, which the data path's sge_gpa() reads as
     * a raw bus address.  Make the fake honor key 0 as sge_gpa() does, so
     * only the target's own refusal stops either direction.
     */
    h->rkey = 0;
    memset(back, 0xee, 512);
    cap_init(&c, NVME_CMD_READ, 37);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    cap_addr_sgl(&c, back_addr, 512);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-addr-sgl-read", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "a Read through an unkeyed address SGL was accepted");
    check("io-addr-sgl-read", back[0] == 0xee && back[511] == 0xee,
          "a Read through an unkeyed address SGL wrote host memory");

    cap_init(&c, NVME_CMD_WRITE, 38);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 16);
    cap_addr_sgl(&c, back_addr, 512);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-addr-sgl-write", rsp_sc(rsp) == NVME_SC_SGL_INVALID_TYPE,
          "a Write through an unkeyed address SGL was accepted");
    h->rkey = HOST_RKEY;

    /* An in-capsule write, the one direction that form makes sense. */
    for (int i = 0; i < 512; i++) {
        scratch[i] = (uint8_t)(0x40 + (i & 0x1f));
    }
    cap_init(&c, NVME_CMD_WRITE, 34);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-write", rsp_status(rsp) == 0,
          "an in-capsule Write was rejected");

    memset(back, 0, 512);
    cap_init(&c, NVME_CMD_READ, 35);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-write", memcmp(back, scratch, 512) == 0,
          "in-capsule Write data did not land in the namespace");

    /* A capsule claiming more in-capsule data than it carries. */
    cap_init(&c, NVME_CMD_WRITE, 36);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    c.len = NVME_SQE_SIZE + 16;
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-short", rsp_sc(rsp) == NVME_SC_SGL_INVALID_DATA,
          "a capsule shorter than its in-capsule SGL was accepted");

    /*
     * An offset that wraps to 0 once the 64-byte SQE is added to it, which
     * wrote the command itself to the namespace as the first of its data.
     */
    cap_init(&c, NVME_CMD_WRITE, 39);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_inline_sgl(&c, scratch, sizeof(scratch));
    ernic_put_le64(c.b + NVME_SQE_DPTR_OFF, UINT64_MAX - (NVME_SQE_SIZE - 1));
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-wrap", rsp_sc(rsp) == NVME_SC_SGL_INVALID_DATA,
          "an in-capsule SGL offset that wraps was accepted");

    memset(back, 0, 512);
    cap_init(&c, NVME_CMD_READ, 40);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 64);
    cap_keyed_sgl(&c, back_addr, 512, HOST_RKEY);
    nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
    check("io-inline-wrap", memcmp(back, scratch, 512) == 0,
          "a Write through an in-capsule SGL offset that wraps changed the "
          "namespace");

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

    /*
     * Identify Controller over a discovery association. The host cross-checks
     * CNTRLTYPE against this SUBNQN and fails the whole connect with EINVAL if
     * they disagree, so the two have to be asserted together.
     */
    memset(h->buf, 0xa5, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 39);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery", rsp_status(rsp) == 0,
          "Identify Controller on the discovery association failed");
    check("discovery", h->buf[111] == 2,
          "cntrltype does not report a discovery controller");
    check("discovery", strcmp((char *)h->buf + 768, NVMEOF_DISCOVERY_NQN) == 0,
          "Identify Controller names the subsystem, not the discovery NQN");
    check("discovery", ernic_get_le32(h->buf + 516) == 0,
          "a discovery controller must report nn as zero");

    const size_t want = NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE;
    memset(h->buf, 0xcc, want);
    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, 40);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF,
                   NVME_LOG_DISCOVERY | (uint32_t)(((want / 4) - 1) << 16));
    cap_keyed_sgl(&c, HOST_BASE, (uint32_t)want, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery", rsp_status(rsp) == 0, "Get Log Page 0x70 failed");
    check("discovery", ernic_get_le64(h->buf + NVMEOF_DISC_NUMREC_OFF) == 1,
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
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF,
                   NVME_LOG_DISCOVERY |
                       (uint32_t)(((NVMEOF_DISC_ENTRY_SIZE / 4) - 1) << 16));
    ernic_put_le64(c.b + NVME_SQE_CDW12_OFF, NVMEOF_DISC_HDR_SIZE);
    cap_keyed_sgl(&c, HOST_BASE, NVMEOF_DISC_ENTRY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery-offset", rsp_status(rsp) == 0,
          "offset Get Log Page failed");
    check("discovery-offset",
          h->buf[NVMEOF_DISC_E_TRTYPE_OFF] == NVMEOF_TRTYPE_RDMA,
          "offset read did not start at the first record");

    /*
     * Reading past the end yields zeros, not an error, however far past.
     * The header is not zero, so an offset that wraps or is truncated back
     * into the page shows: 1 << 32 is 0 in 32 bits, and 1 << 63 is
     * negative if signed.
     */
    const uint64_t past[] = {want, 2 * want, UINT64_C(1) << 32,
                             UINT64_C(1) << 63, UINT64_MAX - 3};
    for (size_t i = 0; i < sizeof(past) / sizeof(past[0]); i++) {
        memset(h->buf, 0xa5, 64);
        uint16_t st = get_log(aq, h, 43, NVME_LOG_DISCOVERY, 15, past[i], 64);
        if (st != 0 || !host_bytes_are(h, 64, 0)) {
            fail("discovery-past-end",
                 "offset %#" PRIx64 ": status %#06" PRIx16 ", or not zeros",
                 past[i], st);
        }
    }

    /* An unknown log page is rejected rather than silently zero-filled. */
    cap_init(&c, NVME_ADMIN_GET_LOG_PAGE, 42);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, 0x7e);
    cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    check("discovery", rsp_sc(rsp) == NVME_SC_INVALID_FIELD,
          "an unknown log page identifier was accepted");

    free(h);
    nvmeof_target_destroy(t);
}

/* ------------------------------------------------------------------ */
/* Connect, properties, features, log pages and Abort                 */
/* ------------------------------------------------------------------ */

#define FIXTURE_HOSTNQN "nqn.2014-08.org.nvmexpress:uuid:fixture"
#define FIXTURE_KATO    15000u /* ms; not the host's 5 s default */

/* A target on "size=1M,queues=4" and its admin queue, on handle 1. */
struct admin_fixture {
    struct host_mem *h;
    struct nvmeof_target *t;
    struct nvmeof_queue *aq;
};

static void admin_teardown(struct admin_fixture *f)
{
    free(f->h);
    nvmeof_target_destroy(f->t);
    memset(f, 0, sizeof(*f));
}

/* Returns false, having failed name and freed everything, on any error. */
static bool admin_setup(struct admin_fixture *f, const char *name)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    memset(f, 0, sizeof(*f));
    f->h = calloc(1, sizeof(*f->h));
    f->t = make_target("size=1M,queues=4");
    if (f->h == NULL || f->t == NULL) {
        fail(name, "setup failed");
        admin_teardown(f);
        return false;
    }
    f->h->rkey = HOST_RKEY;

    f->aq = nvmeof_target_open_queue(f->t, 1);
    if (f->aq == NULL) {
        fail(name, "admin queue not opened");
        admin_teardown(f);
        return false;
    }
    connect_capsule(&c, f->h, 0, 31, "nvmet-test", FIXTURE_HOSTNQN);
    ernic_put_le32(c.b + NVMEOF_CONNECT_KATO_OFF, FIXTURE_KATO);
    if (nvmeof_queue_exec(f->aq, c.b, c.len, &host_ops, f->h, rsp) !=
            NVMEOF_EXEC_DONE ||
        rsp_status(rsp) != 0) {
        fail(name, "admin Connect failed");
        admin_teardown(f);
        return false;
    }
    return true;
}

/*
 * Connect status codes are command-specific and permanent, so compare the
 * whole status word: SC alone would pass a generic error, or one without DNR.
 */
static void check_connect_reject(const char *name, const struct nvmeof_queue *q,
                                 const uint8_t *rsp, uint8_t sc,
                                 const char *detail)
{
    uint16_t want = nvme_status(NVME_SCT_CMD_SPECIFIC, sc, 1);

    if (rsp_status(rsp) != want) {
        fail(name, "%s: status %#06" PRIx16 ", want %#06" PRIx16, detail,
             rsp_status(rsp), want);
    }
    if (nvmeof_queue_connected(q)) {
        fail(name, "%s: the queue was left connected", detail);
    }
}

static void test_connect_reject(void)
{
    struct host_mem *h = calloc(1, sizeof(*h));
    struct nvmeof_target *t = make_target("size=1M,queues=4");
    uint8_t rsp[NVME_CQE_SIZE];
    struct capsule c;
    const char *hostnqn = "nqn.2014-08.org.nvmexpress:uuid:reject";
    struct nvmeof_queue *q;

    if (t == NULL || h == NULL) {
        fail("connect-reject", "setup failed");
        free(h);
        nvmeof_target_destroy(t);
        return;
    }
    h->rkey = HOST_RKEY;

    struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
    if (aq == NULL) {
        fail("connect-reject", "admin queue not opened");
        goto out;
    }

    /* Only record format 0 exists. */
    static const uint16_t recfmts[] = {1, 0xffff};
    for (size_t i = 0; i < sizeof(recfmts) / sizeof(recfmts[0]); i++) {
        connect_capsule(&c, h, 0, 31, "nvmet-test", hostnqn);
        ernic_put_le16(c.b + NVMEOF_CONNECT_RECFMT_OFF, recfmts[i]);
        nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
        check_connect_reject("connect-recfmt", aq, rsp,
                             NVMEOF_SC_CONNECT_FORMAT,
                             "a nonzero record format");
    }

    /*
     * SQSIZE is 0-based, and a 1-entry queue is invalid, so 0 is refused;
     * so is anything past CAP.MQES, queue_depth - 1.
     */
    static const uint16_t sqsizes[] = {0, 128, 0xffff};
    for (size_t i = 0; i < sizeof(sqsizes) / sizeof(sqsizes[0]); i++) {
        do_connect(aq, h, 0, sqsizes[i], "nvmet-test", hostnqn, rsp);
        check_connect_reject("connect-sqsize", aq, rsp,
                             NVMEOF_SC_CONNECT_INVALID_PARAM,
                             "an out-of-range SQSIZE");
    }

    /* An I/O queue needs an association to join. */
    q = nvmeof_target_open_queue(t, 2);
    if (q == NULL) {
        fail("connect-qid", "queue not opened");
        goto out;
    }
    do_connect(q, h, 1, 31, "nvmet-test", hostnqn, rsp);
    check_connect_reject("connect-qid", q, rsp, NVMEOF_SC_CONNECT_INVALID_PARAM,
                         "an I/O queue before the admin queue");

    check("connect-reject", nvmeof_target_stats(t)->connects == 0,
          "a rejected Connect was counted as a connection");

    /* None of that left state behind: the largest SQSIZE is accepted. */
    do_connect(aq, h, 0, 127, "nvmet-test", hostnqn, rsp);
    check("connect-sqsize-max", rsp_status(rsp) == 0,
          "an admin Connect with SQSIZE = MQES was refused");

    /* The admin Connect reset the queue table, so open the I/O queue again. */
    q = nvmeof_target_open_queue(t, 2);
    if (q == NULL) {
        fail("connect-qid", "queue not opened");
        goto out;
    }

    /* QID is 1-based for I/O queues, up to the configured count. */
    static const uint16_t qids[] = {5, 0xffff};
    for (size_t i = 0; i < sizeof(qids) / sizeof(qids[0]); i++) {
        do_connect(q, h, qids[i], 31, "nvmet-test", hostnqn, rsp);
        check_connect_reject("connect-qid", q, rsp,
                             NVMEOF_SC_CONNECT_INVALID_PARAM,
                             "a QID past the configured queue count");
    }
    do_connect(q, h, 1, 0, "nvmet-test", hostnqn, rsp);
    check_connect_reject("connect-sqsize", q, rsp,
                         NVMEOF_SC_CONNECT_INVALID_PARAM,
                         "an I/O queue with SQSIZE 0");

    /* The rejected queue can still connect, as the last queue. */
    do_connect(q, h, 4, 31, "nvmet-test", hostnqn, rsp);
    check("connect-qid-max", rsp_status(rsp) == 0,
          "a Connect for the last I/O queue was refused");
    check("connect-qid-max", nvmeof_queue_id(q) == 4, "queue id is not 4");
    check("connect-reject", nvmeof_target_stats(t)->connects == 2,
          "connect count is wrong");

out:
    free(h);
    nvmeof_target_destroy(t);
}

static void test_properties(void)
{
    struct admin_fixture f;
    if (!admin_setup(&f, "prop")) {
        return;
    }
    const uint16_t bad_field =
        nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    const uint16_t bad_opcode =
        nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
    const uint32_t cc_en = NVME_CC_ENABLE | (6u << 16) | (4u << 20);
    uint64_t v;

    /* A new association starts with the controller disabled. */
    check("prop-cc-initial",
          prop_get(f.aq, f.h, 1, NVMEOF_PROP_CC, 0, &v) == 0 && v == 0,
          "CC is not zero after Connect");

    /* CC reads back what was written, all of it, not just CC.EN. */
    check("prop-cc-roundtrip", set_cc(f.aq, f.h, 2, cc_en) == 0,
          "Property Set CC failed");
    check("prop-cc-roundtrip",
          prop_get(f.aq, f.h, 3, NVMEOF_PROP_CC, 0, &v) == 0 && v == cc_en,
          "Property Get CC did not return the value written");
    check("prop-cc-roundtrip",
          set_cc(f.aq, f.h, 4, cc_en | NVME_CC_SHN_NORMAL) == 0 &&
              prop_get(f.aq, f.h, 5, NVMEOF_PROP_CC, 0, &v) == 0 &&
              v == (cc_en | NVME_CC_SHN_NORMAL),
          "Property Get CC did not return CC.SHN");
    check("prop-cc-roundtrip", set_cc(f.aq, f.h, 6, cc_en) == 0,
          "Property Set CC failed");

    /* CC is 4 bytes; the upper half of the value field is not part of it. */
    check("prop-cc-width",
          prop_set(f.aq, f.h, 7, NVMEOF_PROP_CC,
                   (UINT64_C(0xdeadbeef) << 32) | cc_en) == 0 &&
              prop_get(f.aq, f.h, 8, NVMEOF_PROP_CC, 0, &v) == 0 && v == cc_en,
          "Property Set CC kept bits past its 4 bytes");

    /* CC is the only writable property, and a refused write changes none. */
    const uint32_t ro[] = {NVMEOF_PROP_CAP, NVMEOF_PROP_VS, NVMEOF_PROP_CSTS,
                           NVMEOF_PROP_NSSR, 0x18};
    for (size_t i = 0; i < sizeof(ro) / sizeof(ro[0]); i++) {
        uint16_t st = prop_set(f.aq, f.h, 9, ro[i], 0);
        if (st != bad_field) {
            fail("prop-set-ro",
                 "Property Set of %#" PRIx32 ": status %#06" PRIx16
                 ", want %#06" PRIx16,
                 ro[i], st, bad_field);
        }
    }
    check("prop-set-ro",
          prop_get(f.aq, f.h, 10, NVMEOF_PROP_CC, 0, &v) == 0 && v == cc_en,
          "a refused Property Set changed CC");

    /* Offsets that aren't a property the controller implements. */
    const uint32_t unknown[] = {NVMEOF_PROP_NSSR, 0x04, 0x18, 0xffffffffu};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        uint16_t st = prop_get(f.aq, f.h, 11, unknown[i], 0, &v);
        if (st != bad_field) {
            fail("prop-get-unknown",
                 "Property Get of %#" PRIx32 ": status %#06" PRIx16
                 ", want %#06" PRIx16,
                 unknown[i], st, bad_field);
        }
    }

    /* A queue that hasn't connected gets no property access. */
    struct nvmeof_queue *q = nvmeof_target_open_queue(f.t, 2);
    if (q == NULL) {
        fail("prop-pre-connect", "queue not opened");
    } else {
        check("prop-pre-connect",
              prop_get(q, f.h, 12, NVMEOF_PROP_CC, 0, &v) == bad_opcode,
              "Property Get on an unconnected queue was accepted");
        check("prop-pre-connect",
              prop_set(q, f.h, 13, NVMEOF_PROP_CC, 0) == bad_opcode,
              "Property Set on an unconnected queue was accepted");
        check("prop-pre-connect",
              prop_get(f.aq, f.h, 14, NVMEOF_PROP_CC, 0, &v) == 0 && v == cc_en,
              "Property Set on an unconnected queue changed CC");
    }

    /* A Fabrics command type the controller doesn't implement. */
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];
    cap_init(&c, NVME_ADMIN_FABRICS, 15);
    c.b[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_AUTH_SEND;
    nvmeof_queue_exec(f.aq, c.b, c.len, &host_ops, f.h, rsp);
    check("prop-bad-fctype", rsp_status(rsp) == bad_opcode,
          "an unknown Fabrics command type was accepted");

    admin_teardown(&f);
}

/*
 * Get or Set Features fid with cdw11; the result dword goes to *result.
 * Returns the status, or UINT16_MAX if the target produced no response.
 */
static uint16_t features(struct nvmeof_queue *q, struct host_mem *h,
                         uint8_t opcode, uint16_t cid, uint8_t fid,
                         uint32_t cdw11, uint32_t *result)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    *result = 0;
    cap_init(&c, opcode, cid);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, fid);
    ernic_put_le32(c.b + NVME_SQE_CDW11_OFF, cdw11);
    if (nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp) !=
        NVMEOF_EXEC_DONE) {
        return UINT16_MAX;
    }
    *result = ernic_get_le32(rsp + NVME_CQE_RESULT_OFF);
    return rsp_status(rsp);
}

static void test_features(void)
{
    struct admin_fixture f;
    if (!admin_setup(&f, "features")) {
        return;
    }
    const uint16_t bad_field =
        nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    uint32_t r;

    /*
     * Number of Queues: both counts are 0-based.  Before the host sets it,
     * it is every configured queue.
     */
    check("get-num-queues",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 1, NVME_FEAT_NUM_QUEUES,
                   0, &r) == 0 &&
              r == (3u | (3u << 16)),
          "Number of Queues before Set Features is not the configured 4");

    /* A request within the limit is granted as asked, each count its own. */
    check("set-num-queues-fit",
          features(f.aq, f.h, NVME_ADMIN_SET_FEATURES, 2, NVME_FEAT_NUM_QUEUES,
                   1u | (2u << 16), &r) == 0 &&
              r == (1u | (2u << 16)),
          "a Number of Queues within the limit was not granted as asked");

    /* Without separate SQs and CQs, the smaller count is what's usable. */
    check("get-num-queues",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 3, NVME_FEAT_NUM_QUEUES,
                   0, &r) == 0 &&
              r == (1u | (1u << 16)),
          "Get Features did not report the smaller of the granted counts");

    /* The most a host may ask for is clamped.  That's 0xfffe, 65535
     * queues: 0xffff is reserved. */
    check("set-num-queues-clamp",
          features(f.aq, f.h, NVME_ADMIN_SET_FEATURES, 4, NVME_FEAT_NUM_QUEUES,
                   0xfffefffeu, &r) == 0 &&
              r == (3u | (3u << 16)),
          "Number of Queues was not clamped to the configured 4");
    check("set-num-queues-clamp",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 5, NVME_FEAT_NUM_QUEUES,
                   0, &r) == 0 &&
              r == (3u | (3u << 16)),
          "Get Features did not report the clamped count");

    /* Keep Alive Timer: Connect's KATO, until Set Features changes it. */
    check("get-kato",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 6, NVME_FEAT_KATO, 0,
                   &r) == 0 &&
              r == FIXTURE_KATO,
          "Get Features KATO did not report Connect's KATO");
    check("set-kato",
          features(f.aq, f.h, NVME_ADMIN_SET_FEATURES, 7, NVME_FEAT_KATO, 30000,
                   &r) == 0 &&
              r == 30000,
          "Set Features KATO failed");
    check("set-kato",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 8, NVME_FEAT_KATO, 0,
                   &r) == 0 &&
              r == 30000,
          "Get Features KATO did not report the value set");

    /* Asynchronous Event Configuration is accepted; no event is sent. */
    check("set-async-event",
          features(f.aq, f.h, NVME_ADMIN_SET_FEATURES, 9, NVME_FEAT_ASYNC_EVENT,
                   0x100, &r) == 0 &&
              r == 0x100,
          "Set Features Asynchronous Event Configuration failed");
    check("get-async-event",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 10,
                   NVME_FEAT_ASYNC_EVENT, 0, &r) == 0,
          "Get Features Asynchronous Event Configuration failed");

    /* Arbitration, Power Management, and a vendor-specific feature. */
    const uint8_t unknown[] = {0x01, 0x02, 0xc0};
    for (size_t i = 0; i < sizeof(unknown); i++) {
        uint16_t st =
            features(f.aq, f.h, NVME_ADMIN_SET_FEATURES, 11, unknown[i], 0, &r);
        if (st != bad_field) {
            fail("set-features-unknown",
                 "FID %#" PRIx8 ": status %#06" PRIx16 ", want %#06" PRIx16,
                 unknown[i], st, bad_field);
        }
        st =
            features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 12, unknown[i], 0, &r);
        if (st != bad_field) {
            fail("get-features-unknown",
                 "FID %#" PRIx8 ": status %#06" PRIx16 ", want %#06" PRIx16,
                 unknown[i], st, bad_field);
        }
    }

    /* An unsupported Set changed nothing. */
    check("set-features-unknown",
          features(f.aq, f.h, NVME_ADMIN_GET_FEATURES, 13, NVME_FEAT_KATO, 0,
                   &r) == 0 &&
              r == 30000,
          "an unsupported Set Features changed KATO");

    admin_teardown(&f);
}

static void test_log_page(void)
{
    struct admin_fixture f;
    if (!admin_setup(&f, "log-page")) {
        return;
    }
    const uint16_t bad_field =
        nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);

    /*
     * The error log, which the host reads at startup, is empty.  Only what
     * was asked for is written.
     */
    memset(f.h->buf, 0xa5, 128);
    check("log-error", get_log(f.aq, f.h, 1, NVME_LOG_ERROR, 15, 0, 64) == 0,
          "Get Log Page for the error log failed");
    check("log-error", host_bytes_are(f.h, 64, 0),
          "the error log is not empty");
    check("log-error", f.h->buf[64] == 0xa5,
          "Get Log Page wrote past the length asked for");

    /* Every page the controller keeps is a whole, empty 4 KiB. */
    const uint8_t lids[] = {NVME_LOG_ERROR, NVME_LOG_SMART, NVME_LOG_FW_SLOT,
                            NVME_LOG_CHANGED_NS, NVME_LOG_CMD_EFFECTS};
    for (size_t i = 0; i < sizeof(lids); i++) {
        memset(f.h->buf, 0xa5, 4096);
        uint16_t st = get_log(f.aq, f.h, 2, lids[i], 1023, 0, 4096);
        if (st != 0 || !host_bytes_are(f.h, 4096, 0)) {
            fail("log-pages",
                 "log %#" PRIx8 ": status %#06" PRIx16
                 ", or not 4 KiB of zeros",
                 lids[i], st);
        }
    }

    /* A read that starts inside the page and runs off its end. */
    memset(f.h->buf, 0xa5, 64);
    check("log-offset-tail",
          get_log(f.aq, f.h, 4, NVME_LOG_ERROR, 15, 4096 - 16, 64) == 0 &&
              host_bytes_are(f.h, 16, 0),
          "a read running off the end of the log failed");

    /*
     * The transfer is the smaller of NUMD and the SGL, and is capped at
     * 1 MiB.  NUMD can ask for 16 GiB and a keyed SGL can name 16 MiB, so
     * the cap only applies when both are past it.
     */
    const uint32_t mib_numd = (1u << 20) / 4 - 1;
    check("log-size-cap",
          get_log(f.aq, f.h, 5, NVME_LOG_ERROR, UINT32_MAX, 0, 4096) == 0,
          "a NUMD larger than the SGL was not cut to the SGL");
    check("log-size-cap",
          get_log(f.aq, f.h, 6, NVME_LOG_ERROR, mib_numd, 0, 1u << 20) == 0,
          "a 1 MiB Get Log Page was refused");
    check("log-size-cap",
          get_log(f.aq, f.h, 7, NVME_LOG_ERROR, UINT32_MAX, 0, 1u << 20) == 0,
          "a NUMD past 1 MiB was not cut to a 1 MiB SGL");
    check("log-size-cap",
          get_log(f.aq, f.h, 8, NVME_LOG_ERROR, mib_numd + 1, 0,
                  (1u << 20) + 4) == bad_field,
          "a Get Log Page of 1 MiB + 4 was accepted");
    check("log-size-cap",
          get_log(f.aq, f.h, 9, NVME_LOG_ERROR, UINT32_MAX, 0, 0xfffffcu) ==
              bad_field,
          "a Get Log Page of 16 MiB was accepted");

    admin_teardown(&f);
}

/* Abort never finds a command to abort, and says so. */
static void test_abort(void)
{
    struct admin_fixture f;
    if (!admin_setup(&f, "abort")) {
        return;
    }
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    /* The one command a controller holds is an Async Event Request. */
    cap_init(&c, NVME_ADMIN_ASYNC_EVENT, 0x1234);
    check("abort",
          nvmeof_queue_exec(f.aq, c.b, c.len, &host_ops, f.h, rsp) ==
              NVMEOF_EXEC_HELD,
          "Async Event Request was not held");

    /* cdw10: SQID 0 in bits 15:0, the CID in bits 31:16. */
    const uint16_t cids[] = {0x1234, 0x4321};
    for (size_t i = 0; i < sizeof(cids) / sizeof(cids[0]); i++) {
        cap_init(&c, NVME_ADMIN_ABORT, (uint16_t)(1 + i));
        ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, (uint32_t)cids[i] << 16);
        memset(rsp, 0xa5, sizeof(rsp));
        int rc = nvmeof_queue_exec(f.aq, c.b, c.len, &host_ops, f.h, rsp);
        if (rc != NVMEOF_EXEC_DONE || rsp_status(rsp) != 0) {
            fail("abort",
                 "Abort of CID %#" PRIx16 ": rc %d, status %#06" PRIx16,
                 cids[i], rc, rsp_status(rsp));
        }
        /* Dword 0 bit 0 set: the command was not aborted. */
        if (ernic_get_le32(rsp + NVME_CQE_RESULT_OFF) != 1) {
            fail("abort",
                 "Abort of CID %#" PRIx16 ": result %#" PRIx32 ", want 1",
                 cids[i], ernic_get_le32(rsp + NVME_CQE_RESULT_OFF));
        }
        if (ernic_get_le16(rsp + NVME_CQE_CID_OFF) != (uint16_t)(1 + i)) {
            fail("abort",
                 "Abort of CID %#" PRIx16 ": response is not for the Abort",
                 cids[i]);
        }
    }

    admin_teardown(&f);
}

/* ------------------------------------------------------------------ */
/* File-backed namespace                                              */
/* ------------------------------------------------------------------ */

static void write_one_block(struct nvmeof_queue *q, struct host_mem *h,
                            uint64_t slba, uint32_t cdw12, const uint8_t *data)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    memcpy(h->buf, data, 512);
    cap_init(&c, NVME_CMD_WRITE, 50);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, slba);
    ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, cdw12);
    cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
    if (rsp_status(rsp) != 0) {
        fail("file-backing", "write failed with status 0x%04x",
             rsp_status(rsp));
    }
}

/* Read one block into the start of host memory; returns the status. */
static uint16_t read_one_block(struct nvmeof_queue *q, struct host_mem *h,
                               uint64_t slba)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    memset(h->buf, 0, 512);
    cap_init(&c, NVME_CMD_READ, 52);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, slba);
    cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
    return rsp_status(rsp);
}

/* Identify Controller's vwc byte, or -1 if Identify failed. */
static int read_vwc(struct nvmeof_queue *aq, struct host_mem *h)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    memset(h->buf, 0xa5, NVME_IDENTIFY_SIZE);
    cap_init(&c, NVME_ADMIN_IDENTIFY, 54);
    ernic_put_le32(c.b + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    cap_keyed_sgl(&c, HOST_BASE, NVME_IDENTIFY_SIZE, HOST_RKEY);
    nvmeof_queue_exec(aq, c.b, c.len, &host_ops, h, rsp);
    return rsp_status(rsp) == 0 ? h->buf[525] : -1;
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
    uint8_t fua_pattern[512];
    for (int i = 0; i < 512; i++) {
        pattern[i] = (uint8_t)(i ^ 0x5a);
        fua_pattern[i] = (uint8_t)(i ^ 0xc3);
    }

    char opts[256];
    snprintf(opts, sizeof(opts), "size=1M,file=%s", path);

    /* First instance: connect, write two blocks, shut down. */
    struct nvmeof_target *t = make_target(opts);
    if (t != NULL) {
        uint8_t rsp[NVME_CQE_SIZE];
        struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp);
        struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 2);
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp);

        /* Without a volatile write cache, the host sends neither Flush nor
         * FUA, and a guest's fsync() never reaches the image. */
        int vwc = read_vwc(aq, h);
        check("file-vwc", vwc == 0x05,
              "a file-backed namespace does not advertise a volatile write "
              "cache that needs the namespace's own NSID to flush");

        write_one_block(ioq, h, 3, 0, pattern);
        write_one_block(ioq, h, 4, NVME_RW_FUA, fua_pattern);

        struct capsule c;
        cap_init(&c, NVME_CMD_WRITE_ZEROES, 53);
        ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 5);
        ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, NVME_RW_FUA);
        nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
        check("file-fua", rsp_status(rsp) == 0, "Write Zeroes with FUA failed");

        cap_init(&c, NVME_CMD_FLUSH, 51);
        ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        nvmeof_queue_exec(ioq, c.b, c.len, &host_ops, h, rsp);
        check("file-flush", rsp_status(rsp) == 0, "Flush failed");
        nvmeof_target_destroy(t);
    }

    /* Second instance over the same file, without size=, must keep the
     * image's size and see both blocks.  That shows the writes reach the
     * file, not that they reach stable storage, which the host page cache
     * hides here; test_sync() checks that. */
    snprintf(opts, sizeof(opts), "file=%s", path);
    t = make_target(opts);
    if (t != NULL) {
        uint8_t rsp[NVME_CQE_SIZE];
        struct nvmeof_queue *aq = nvmeof_target_open_queue(t, 1);
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp);
        struct nvmeof_queue *ioq = nvmeof_target_open_queue(t, 2);
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp);

        check("file-backing", nvmeof_target_config(t)->size == (1u << 20),
              "reopening an image without size= did not keep its size");
        check("file-backing", read_one_block(ioq, h, 3) == 0,
              "read back failed");
        check("file-backing", memcmp(h->buf, pattern, 512) == 0,
              "a write did not reach the image");
        check("file-fua", read_one_block(ioq, h, 4) == 0,
              "FUA read back failed");
        check("file-fua", memcmp(h->buf, fua_pattern, 512) == 0,
              "a FUA write did not reach the image");
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
        ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
        ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, 3);
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
/* Writeback for Flush, FUA and shutdown                              */
/* ------------------------------------------------------------------ */

/* What the target asked of sync_spy() since the last sync_reset(). */
static struct {
    unsigned calls;
    uint8_t *addr;
    size_t len;
    int flags;
    bool fail; /* fail every call with EIO */
} sync_log;

static void sync_reset(bool fail)
{
    memset(&sync_log, 0, sizeof(sync_log));
    sync_log.fail = fail;
}

static int sync_spy(void *addr, size_t len, int flags)
{
    sync_log.calls++;
    sync_log.addr = addr;
    sync_log.len = len;
    sync_log.flags = flags;
    if (sync_log.fail) {
        errno = EIO;
        return -1;
    }
    return msync(addr, len, flags);
}

/* Run one I/O command on block slba (ignored by Flush); returns its status.
 * Read and Write move one block through the start of host memory. */
static uint16_t io_one(struct nvmeof_queue *q, struct host_mem *h,
                       uint8_t opcode, uint64_t slba, uint32_t cdw12)
{
    struct capsule c;
    uint8_t rsp[NVME_CQE_SIZE];

    cap_init(&c, opcode, 60);
    ernic_put_le32(c.b + NVME_SQE_NSID_OFF, 1);
    if (opcode != NVME_CMD_FLUSH) {
        ernic_put_le64(c.b + NVME_SQE_CDW10_OFF, slba);
        ernic_put_le32(c.b + NVME_SQE_CDW12_OFF, cdw12);
    }
    if (opcode == NVME_CMD_READ || opcode == NVME_CMD_WRITE) {
        cap_keyed_sgl(&c, HOST_BASE, 512, HOST_RKEY);
    }
    nvmeof_queue_exec(q, c.b, c.len, &host_ops, h, rsp);
    return rsp_status(rsp);
}

/* Open a target on opts with sync_spy() in place of msync(), and connect
 * an admin and an I/O queue.  Returns the I/O queue, or NULL. */
static struct nvmeof_queue *sync_target(const char *opts, struct host_mem *h,
                                        struct nvmeof_target **out)
{
    uint8_t rsp[NVME_CQE_SIZE];

    *out = make_target(opts);
    if (*out == NULL) {
        return NULL;
    }
    nvmeof_target_set_sync(*out, sync_spy);

    /* The admin Connect starts a new association, which resets the queue
     * table, so the I/O queue is opened after it. */
    struct nvmeof_queue *aq = nvmeof_target_open_queue(*out, 1);
    if (aq == NULL ||
        do_connect(aq, h, 0, 31, "nvmet-test", "nqn.host", rsp) !=
            NVMEOF_EXEC_DONE ||
        rsp_status(rsp) != 0) {
        fail("sync-setup", "admin Connect to '%s' failed", opts);
        return NULL;
    }
    struct nvmeof_queue *ioq = nvmeof_target_open_queue(*out, 2);
    if (ioq == NULL ||
        do_connect(ioq, h, 1, 31, "nvmet-test", "nqn.host", rsp) !=
            NVMEOF_EXEC_DONE ||
        rsp_status(rsp) != 0) {
        fail("sync-setup", "I/O queue Connect to '%s' failed", opts);
        return NULL;
    }
    return ioq;
}

/*
 * Restarting over the same image reads the host page cache, so it can't
 * tell writeback from none.  Watch the calls instead: Flush, FUA and a
 * shutdown must ask for a synchronous writeback of what they cover, and
 * Flush and FUA fail if it does.
 */
static void test_sync(void)
{
    char path[] = "/tmp/ernic-nvmeof-syncXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fail("sync", "mkstemp failed");
        return;
    }
    close(fd);

    struct host_mem *h = calloc(1, sizeof(*h));
    long page = sysconf(_SC_PAGESIZE);
    if (h == NULL || page <= 0) {
        fail("sync", "setup failed");
        free(h);
        unlink(path);
        return;
    }
    h->rkey = HOST_RKEY;

    const uint16_t fault = nvme_status(NVME_SCT_MEDIA, NVME_SC_WRITE_FAULT, 1);
    const uint8_t fua_ops[] = {NVME_CMD_WRITE, NVME_CMD_WRITE_ZEROES};

    char opts[256];
    snprintf(opts, sizeof(opts), "size=1M,file=%s", path);
    struct nvmeof_target *t;
    struct nvmeof_queue *ioq = sync_target(opts, h, &t);
    if (ioq != NULL) {
        /* Flush writes back the whole namespace. */
        sync_reset(false);
        check("sync-flush", io_one(ioq, h, NVME_CMD_FLUSH, 0, 0) == 0,
              "Flush failed");
        check("sync-flush",
              sync_log.calls == 1 && sync_log.flags == MS_SYNC &&
                  sync_log.len == (1u << 20),
              "Flush did not write the namespace back synchronously");
        uint8_t *base = sync_log.addr;

        /* FUA writes back the pages the write touched.  Block 9 doesn't
         * start a page, so the range has to be rounded down to one. */
        const uint64_t off = 9 * 512;
        const uint64_t start = off - off % (uint64_t)page;
        for (size_t i = 0; i < sizeof(fua_ops); i++) {
            sync_reset(false);
            check("sync-fua", io_one(ioq, h, fua_ops[i], 9, NVME_RW_FUA) == 0,
                  "a FUA write failed");
            check("sync-fua",
                  sync_log.calls == 1 && sync_log.flags == MS_SYNC &&
                      sync_log.addr == base + start &&
                      sync_log.len == off + 512 - start,
                  "a FUA write did not write its pages back synchronously");
        }

        /* Without FUA, and for a Read with it, nothing is written back. */
        sync_reset(false);
        io_one(ioq, h, NVME_CMD_WRITE, 9, 0);
        io_one(ioq, h, NVME_CMD_WRITE_ZEROES, 9, 0);
        io_one(ioq, h, NVME_CMD_READ, 9, NVME_RW_FUA);
        check("sync-no-fua", sync_log.calls == 0,
              "a command without FUA, or a Read, waited for writeback");

        /* A failed writeback fails the command, and says not to retry. */
        sync_reset(true);
        check("sync-fail", io_one(ioq, h, NVME_CMD_FLUSH, 0, 0) == fault,
              "a failed writeback did not fail Flush with Write Fault, DNR");
        for (size_t i = 0; i < sizeof(fua_ops); i++) {
            check("sync-fail",
                  io_one(ioq, h, fua_ops[i], 9, NVME_RW_FUA) == fault,
                  "a failed writeback did not fail a FUA write with Write "
                  "Fault, DNR");
        }
        check("sync-fail", sync_log.calls == 3,
              "not every command tried to write back");

        /*
         * A shutdown, requested with CC.EN set or clear, writes back the
         * whole namespace once.  A reset, enabling the controller, and
         * repeating the notification don't.
         */
        struct nvmeof_queue *aq = nvmeof_target_find_queue(t, 1);
        const uint32_t cc = (6u << 16) | (4u << 20);
        const uint32_t ens[] = {NVME_CC_ENABLE, 0};
        uint32_t csts;
        for (size_t i = 0; i < sizeof(ens) / sizeof(ens[0]); i++) {
            const uint32_t shn = cc | ens[i] | NVME_CC_SHN_NORMAL;
            sync_reset(false);
            check("sync-shutdown",
                  set_cc(aq, h, 70, cc) == 0 &&
                      set_cc(aq, h, 71, cc | ens[i]) == 0 &&
                      sync_log.calls == 0,
                  "a reset or enable failed, or wrote back");
            check("sync-shutdown",
                  set_cc(aq, h, 72, shn) == 0 && set_cc(aq, h, 73, shn) == 0,
                  "Property Set CC.SHN failed");
            check("sync-shutdown",
                  sync_log.calls == 1 && sync_log.flags == MS_SYNC &&
                      sync_log.addr == base && sync_log.len == (1u << 20),
                  "a shutdown did not write the namespace back once, "
                  "synchronously");
            check("sync-shutdown",
                  get_csts(aq, h, 74, &csts) == 0 &&
                      (csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT,
                  "the shutdown was not reported complete");
        }

        /* A failed writeback is only logged: the shutdown still completes. */
        sync_reset(true);
        check("sync-shutdown-fail",
              set_cc(aq, h, 75, cc) == 0 &&
                  set_cc(aq, h, 76, cc | NVME_CC_ENABLE | NVME_CC_SHN_NORMAL) ==
                      0,
              "a failed writeback failed Property Set CC.SHN");
        check("sync-shutdown-fail",
              sync_log.calls == 1 && get_csts(aq, h, 77, &csts) == 0 &&
                  (csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT,
              "a failed writeback kept the shutdown from completing");
    }
    nvmeof_target_destroy(t);

    /* A RAM namespace has nothing to write back. */
    ioq = sync_target("size=1M", h, &t);
    if (ioq != NULL) {
        sync_reset(true);
        check("sync-ram", io_one(ioq, h, NVME_CMD_FLUSH, 0, 0) == 0,
              "Flush on a RAM namespace failed");
        for (size_t i = 0; i < sizeof(fua_ops); i++) {
            check("sync-ram", io_one(ioq, h, fua_ops[i], 9, NVME_RW_FUA) == 0,
                  "a FUA write on a RAM namespace failed");
        }
        struct nvmeof_queue *aq = nvmeof_target_find_queue(t, 1);
        uint32_t csts;
        check("sync-ram",
              set_cc(aq, h, 78, NVME_CC_ENABLE | NVME_CC_SHN_NORMAL) == 0 &&
                  get_csts(aq, h, 79, &csts) == 0 &&
                  (csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT,
              "a shutdown of a RAM namespace did not complete");
        check("sync-ram", sync_log.calls == 0,
              "a RAM namespace tried to write back");
    }
    nvmeof_target_destroy(t);

    unlink(path);
    free(h);
}

/* Parse opts and create a target, leaving any error in err. */
static struct nvmeof_target *try_target(const char *opts, char *err,
                                        size_t errlen)
{
    struct nvmeof_target_cfg cfg;
    struct nvmeof_target *t = NULL;

    nvmeof_target_cfg_defaults(&cfg);
    if (nvmeof_target_cfg_parse(&cfg, opts, err, errlen)) {
        t = nvmeof_target_create(&cfg, err, errlen);
    }
    free(cfg.file);
    return t;
}

/* Size of the open file, or UINT64_MAX if fstat() fails. */
static uint64_t fd_size(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        return UINT64_MAX;
    }
    return (uint64_t)st.st_size;
}

/*
 * An image may hold an earlier run's data, so the size a target settles on
 * must never cost the image its end.
 */
static void test_image_size(void)
{
    char path[] = "/tmp/ernic-nvmeof-sizeXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fail("image-size", "mkstemp failed");
        return;
    }

    const uint64_t def = UINT64_C(64) << 20;
    char opts[256];
    char err[256] = "";
    struct nvmeof_target *t;

    /* An empty image is sized to the default. */
    snprintf(opts, sizeof(opts), "file=%s", path);
    t = try_target(opts, err, sizeof(err));
    if (t == NULL) {
        fail("image-new", "an empty image was refused: %s", err);
    } else {
        check("image-new", nvmeof_target_config(t)->size == def,
              "an empty image was not given the default size");
    }
    nvmeof_target_destroy(t);
    check("image-new", fd_size(fd) == def,
          "an empty image was not grown to the default size");

    /* A size= smaller than the image is refused, and the image kept. */
    snprintf(opts, sizeof(opts), "size=1M,file=%s", path);
    t = try_target(opts, err, sizeof(err));
    check("image-shrink", t == NULL,
          "a size= smaller than the image was taken");
    check("image-shrink", strstr(err, "would truncate") != NULL,
          "refusing to shrink the image did not say why");
    nvmeof_target_destroy(t);
    check("image-shrink", fd_size(fd) == def, "a refused size= truncated it");

    /* A larger one grows it. */
    snprintf(opts, sizeof(opts), "size=128M,file=%s", path);
    t = try_target(opts, err, sizeof(err));
    if (t == NULL) {
        fail("image-grow", "a size= larger than the image was refused: %s",
             err);
    } else {
        check("image-grow", nvmeof_target_config(t)->size == 2 * def,
              "growing the image did not change the namespace size");
    }
    nvmeof_target_destroy(t);
    check("image-grow", fd_size(fd) == 2 * def, "the image was not grown");

    /* Without size=, an image that isn't whole blocks is refused rather
     * than having its tail cut off or hidden. */
    if (ftruncate(fd, (off_t)def + 100) != 0) {
        fail("image-partial", "cannot resize %s", path);
    } else {
        snprintf(opts, sizeof(opts), "file=%s", path);
        t = try_target(opts, err, sizeof(err));
        check("image-partial", t == NULL,
              "an image of a partial block was taken without size=");
        check("image-partial", strstr(err, "not a multiple of bs") != NULL,
              "refusing a partial block did not say why");
        nvmeof_target_destroy(t);
        check("image-partial", fd_size(fd) == def + 100,
              "a refused image was resized");
    }

    /* A size= past what an off_t holds is refused as such, rather than
     * handed to ftruncate() as a negative length. 8388608T is 2^63. */
    snprintf(opts, sizeof(opts), "size=8388608T,file=%s", path);
    t = try_target(opts, err, sizeof(err));
    check("image-too-large", t == NULL, "a size= of 2^63 bytes was taken");
    check("image-too-large", strstr(err, "too large for a file") != NULL,
          "refusing a size= of 2^63 bytes did not say why");
    nvmeof_target_destroy(t);

    /* Something that isn't a regular file can't be sized or mapped as one. */
    t = try_target("file=/dev/null", err, sizeof(err));
    check("image-not-file", t == NULL, "/dev/null was taken as an image");
    check("image-not-file", strstr(err, "not a regular file") != NULL,
          "refusing /dev/null did not say why");
    nvmeof_target_destroy(t);

    /* An image that can't be grown is refused.  The file size limit makes
     * ftruncate() fail with EFBIG, once SIGXFSZ isn't killing us for it. */
    struct rlimit old_lim;
    if (ftruncate(fd, 0) != 0 || getrlimit(RLIMIT_FSIZE, &old_lim) != 0) {
        fail("image-no-room", "cannot set up %s", path);
    } else {
        struct rlimit lim = old_lim;
        lim.rlim_cur = 1u << 20;
        void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
        if (old_handler == SIG_ERR || setrlimit(RLIMIT_FSIZE, &lim) != 0) {
            fail("image-no-room", "cannot limit the file size");
        } else {
            snprintf(opts, sizeof(opts), "size=2M,file=%s", path);
            t = try_target(opts, err, sizeof(err));
            setrlimit(RLIMIT_FSIZE, &old_lim);
            check("image-no-room", t == NULL,
                  "an image past the file size limit was taken");
            check("image-no-room", strstr(err, "cannot size") != NULL,
                  "failing to grow the image did not say why");
            nvmeof_target_destroy(t);
        }
        if (old_handler != SIG_ERR) {
            signal(SIGXFSZ, old_handler);
        }
    }

    close(fd);
    unlink(path);
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
    test_connect_reject();
    test_properties();
    test_features();
    test_log_page();
    test_abort();
    test_file_backing();
    test_sync();
    test_image_size();
    test_queue_table();

    if (failures == 0) {
        printf("PASS nvmeof-target\n");
    } else {
        printf("%d nvmeof-target check(s) failed\n", failures);
    }
    return failures;
}
