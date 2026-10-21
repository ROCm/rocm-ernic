/*
 * ionic_harness.c — a userspace model of the ionic driver, driving the
 * real device code in-process
 *
 * See ionic_harness.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ionic_harness.h"

#include <endian.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "fake_vfu.h"
#include "ionic_adminq.h"
#include "ionic_datapath.h"
#include "ionic_eth_emu.h"
#include "ionic_rdma_devcmd.h"
#include "nvmeof_target.h"
#include "rocm_ernic_compat.h"

/* Guest memory: 32 regions of 1 MiB */
#define IH_REGION_SIZE (1u << 20)
#define IH_NREGIONS    32u

/* BAR0 register map (union ionic_dev_regs) */
#define BAR0_DEVCMD       0x0800u
#define BAR0_DEVCMD_DONE  (BAR0_DEVCMD + 0x04u)
#define BAR0_DEVCMD_CMD   (BAR0_DEVCMD + 0x08u)
#define BAR0_DEVCMD_COMP  (BAR0_DEVCMD + 0x48u)
#define BAR0_DEVCMD_DATA  (BAR0_DEVCMD + 0x88u)
#define BAR0_INTR_CTRL    0x2000u
#define BAR0_INTR_STRIDE  32u
#define BAR0_INTR_MASK    4u
#define IONIC_DEV_SIG     0x44455649u /* "DEVI" */
#define DEVCMD_DATA_BYTES 1912u

/* Ring geometry the harness asks for */
#define ETH_AQ_DEPTH_LOG2 4
#define EQ_DEPTH_LOG2     8
#define AQ_DEPTH_LOG2     6
#define ACQ_DEPTH_LOG2    6
#define QP_DEPTH_LOG2     4
#define WQE_STRIDE_LOG2   7 /* 128 bytes: six SGEs, or 96 bytes inline */
#define CQE_STRIDE_LOG2   5

/* Admin WQEs are 64-byte strides behind a 4-byte header */
#define ADMIN_STRIDE  64u
#define ADMIN_HDR_LEN 4u

/* Where WQE fields sit (struct ionic_v1_wqe) */
#define WQE_PLD_OFF 32u

/* RoCE v2's UDP port */
#define ROCE_UDP_PORT 4791

static bool fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static bool fail(const char *fmt, ...)
{
    va_list ap;

    fputs("harness: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return false;
}

static int fail_rc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* fail() for the functions that report failure as -1 */
static int fail_rc(const char *fmt, ...)
{
    va_list ap;

    fputs("harness: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

/* ---- Byte order helpers ------------------------------------------------ */

static void put_le16(uint8_t *p, uint16_t v)
{
    v = htole16(v);
    memcpy(p, &v, 2);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    v = htole32(v);
    memcpy(p, &v, 4);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    v = htole64(v);
    memcpy(p, &v, 8);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    v = htobe16(v);
    memcpy(p, &v, 2);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    v = htobe32(v);
    memcpy(p, &v, 4);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    v = htobe64(v);
    memcpy(p, &v, 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    uint16_t v;

    memcpy(&v, p, 2);
    return le16toh(v);
}

static uint32_t get_le32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return le32toh(v);
}

static uint16_t get_be16(const uint8_t *p)
{
    uint16_t v;

    memcpy(&v, p, 2);
    return be16toh(v);
}

static uint32_t get_be32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return be32toh(v);
}

/* The wire splits 64-bit RDMA addresses and operands into be32 halves. */
static void put_be32_pair(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* ---- Rings ------------------------------------------------------------- */

static uint32_t ring_depth(const struct ih_ring *r)
{
    return 1u << r->depth_log2;
}

static uint64_t ring_slot(const struct ih_ring *r, uint32_t idx)
{
    return r->gpa + ((uint64_t)(idx % ring_depth(r)) << r->stride_log2);
}

/*
 * The colour a completion at the consumer index must carry to be new. Every
 * ring here starts with colour 1 and flips it on each wrap.
 */
static bool ring_color(const struct ih_ring *r)
{
    return (r->cons / ring_depth(r)) % 2 == 0;
}

static bool ring_alloc(struct ih *h, struct ih_ring *r, uint8_t depth_log2,
                       uint8_t stride_log2)
{
    memset(r, 0, sizeof(*r));
    r->depth_log2 = depth_log2;
    r->stride_log2 = stride_log2;
    r->gpa = fake_vfu_alloc(h->vfu, (size_t)1 << (depth_log2 + stride_log2),
                            IH_PAGE_SIZE);
    return r->gpa != 0 || fail("out of guest memory for a ring");
}

/* ---- Device lifecycle -------------------------------------------------- */

static bool attach_nvmeof(struct ih *h, const char *backend)
{
    struct nvmeof_target_cfg cfg;
    char err[256] = "";
    const char *opts = strchr(backend, ':');

    nvmeof_target_cfg_defaults(&cfg);
    bool ok =
        nvmeof_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                sizeof(err)) &&
        ionic_datapath_attach_nvmeof(h->dev->ionic_dp, &cfg, err, sizeof(err));
    free(cfg.file);
    return ok || fail("nvmeof backend: %s", err);
}

bool ih_create(struct ih *h, const char *backend)
{
    memset(h, 0, sizeof(*h));

    h->vfu = fake_vfu_create(IH_REGION_SIZE, IH_NREGIONS);
    h->dev = calloc(1, sizeof(*h->dev));
    if (!h->vfu || !h->dev) {
        ih_destroy(h);
        return fail("out of memory");
    }

    rocm_ernic_dev_t *dev = h->dev;
    dev->vfu_ctx = h->vfu;
    dev->backend_type_str = strdup(backend);
    dev->backend_port_num = 1;

    dev->pvrdma_handle = pvrdma_device_create(dev, backend, NULL, NULL, 1);
    if (!dev->pvrdma_handle) {
        ih_destroy(h);
        return fail("pvrdma_device_create(%s) failed", backend);
    }
    if (pvrdma_device_realize(dev->pvrdma_handle) < 0) {
        ih_destroy(h);
        return fail("pvrdma_device_realize(%s) failed", backend);
    }

    /* The rest is the server's ionic_device_init(). */
    dev->ionic_emu = ionic_eth_emu_create(h->vfu, IONIC_BAR2_DB_SIZE);
    dev->ionic_rdma = ionic_rdma_devcmd_create(h->vfu);
    if (dev->ionic_emu)
        dev->ionic_dp = ionic_datapath_create(h->vfu, dev->ionic_emu);
    if (!dev->ionic_emu || !dev->ionic_rdma || !dev->ionic_dp) {
        ih_destroy(h);
        return fail("out of memory for the ionic layer");
    }
    ionic_eth_emu_set_pvrdma(dev->ionic_emu, dev->pvrdma_handle);
    ionic_eth_emu_register_rdma_handler(
        dev->ionic_emu, ionic_rdma_devcmd_dispatch, dev->ionic_rdma);
    ionic_rdma_devcmd_set_eth_emu(dev->ionic_rdma, dev->ionic_emu);

    if (!strncmp(backend, "nvmeof", 6) && !attach_nvmeof(h, backend)) {
        ih_destroy(h);
        return false;
    }
    ionic_eth_emu_register_datapath(dev->ionic_emu, dev->ionic_dp);

    h->port_info = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    h->lif_info = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    h->scratch = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    return true;
}

bool ih_destroy(struct ih *h)
{
    rocm_ernic_dev_t *dev = h->dev;

    if (dev) {
        /*
         * The server's ionic_device_fini(), then its pvrdma teardown, both
         * before the context goes, as in the server: the device's teardown
         * puts back any mappings through it.
         */
        ionic_datapath_destroy(dev->ionic_dp);
        ionic_rdma_devcmd_destroy(dev->ionic_rdma);
        ionic_eth_emu_destroy(dev->ionic_emu);
        pvrdma_device_destroy(dev->pvrdma_handle);
        free(dev->backend_type_str);
        free(dev);
    }

    size_t leaked = fake_vfu_destroy(h->vfu);
    memset(h, 0, sizeof(*h));
    return leaked == 0 ||
           fail("%zu scatter/gather entries left mapped at teardown", leaked);
}

void ih_poll(struct ih *h)
{
    rocm_ernic_dev_t *dev = h->dev;

    /* The body of the server's main loop, minus vfu_run_ctx(). */
    ionic_eth_emu_poll_rx(dev->ionic_emu);

    struct ionic_adminq_ctx *aqctx =
        ionic_rdma_devcmd_get_adminq_ctx(dev->ionic_rdma);
    if (aqctx) {
        ionic_adminq_set_pvrdma(aqctx, dev->pvrdma_handle);
        ionic_eth_emu_register_adminq(dev->ionic_emu, aqctx);
        ionic_adminq_set_datapath(aqctx, dev->ionic_dp);
        ionic_rdma_devcmd_set_datapath(dev->ionic_rdma, dev->ionic_dp);
        ionic_adminq_poll(aqctx, h->vfu);
    }
    ionic_datapath_set_pvrdma(dev->ionic_dp, dev->pvrdma_handle);
    ionic_datapath_poll(dev->ionic_dp);
}

/* ---- Registers and doorbells ------------------------------------------- */

void ih_bar0_read(struct ih *h, uint32_t off, void *buf, size_t len)
{
    pvrdma_bar0_mmio_count(h->dev->pvrdma_handle, false);
    if (ionic_eth_emu_bar0_access(h->dev->ionic_emu, buf, len, off, false) !=
        (ssize_t)len) {
        fail("BAR0 read of %zu bytes at %#x refused", len, off);
        memset(buf, 0xff, len);
    }
}

void ih_bar0_write(struct ih *h, uint32_t off, const void *buf, size_t len)
{
    uint8_t tmp[64];

    if (len > sizeof(tmp)) {
        fail("BAR0 write of %zu bytes is larger than the harness allows", len);
        return;
    }
    memcpy(tmp, buf, len);
    pvrdma_bar0_mmio_count(h->dev->pvrdma_handle, true);
    if (ionic_eth_emu_bar0_access(h->dev->ionic_emu, (char *)tmp, len, off,
                                  true) != (ssize_t)len)
        fail("BAR0 write of %zu bytes at %#x refused", len, off);
}

uint32_t ih_bar0_read32(struct ih *h, uint32_t off)
{
    uint8_t b[4];

    ih_bar0_read(h, off, b, 4);
    return get_le32(b);
}

void ih_bar0_write32(struct ih *h, uint32_t off, uint32_t val)
{
    uint8_t b[4];

    put_le32(b, val);
    ih_bar0_write(h, off, b, 4);
}

void ih_intr_mask(struct ih *h, uint32_t vec, bool masked)
{
    ih_bar0_write32(h, BAR0_INTR_CTRL + vec * BAR0_INTR_STRIDE + BAR0_INTR_MASK,
                    masked ? 1u : 0u);
}

void ih_doorbell(struct ih *h, int qtype, uint32_t qid, uint8_t ring,
                 uint16_t p_index)
{
    /* struct ionic_doorbell: le16 p_index, u8 ring, u8 qid_lo, le16 qid_hi */
    uint64_t val = (uint64_t)p_index | (uint64_t)ring << 16 |
                   (uint64_t)(qid & 0xffu) << 24 |
                   (uint64_t)((qid >> 8) & 0xffffu) << 32;
    uint8_t b[8];

    put_le64(b, val);
    pvrdma_uar_mmio_count(h->dev->pvrdma_handle, true);
    /* The kernel's doorbell page is page 0, one u64 slot per qtype. */
    if (ionic_eth_emu_bar2_access(h->dev->ionic_emu, (char *)b, sizeof(b),
                                  (loff_t)qtype * 8, true) != sizeof(b))
        fail("doorbell write for qtype %d refused", qtype);
}

int ih_devcmd(struct ih *h, const uint8_t cmd[64], uint8_t comp[16], void *data,
              size_t data_len)
{
    if (data_len > DEVCMD_DATA_BYTES)
        return fail_rc("devcmd data window is only %u bytes",
                       DEVCMD_DATA_BYTES);

    /* ionic_dev_cmd_go(): command, clear done, ring. */
    ih_bar0_write(h, BAR0_DEVCMD_CMD, cmd, 64);
    ih_bar0_write32(h, BAR0_DEVCMD_DONE, 0);
    ih_bar0_write32(h, BAR0_DEVCMD, 1);

    if (!(ih_bar0_read32(h, BAR0_DEVCMD_DONE) & 1u)) {
        fail("devcmd %u never set done", cmd[0]);
        return -1;
    }
    ih_bar0_read(h, BAR0_DEVCMD_COMP, comp, 16);
    for (size_t off = 0; off < data_len; off += 64) {
        size_t n = data_len - off < 64 ? data_len - off : 64;
        ih_bar0_read(h, BAR0_DEVCMD_DATA + (uint32_t)off, (uint8_t *)data + off,
                     n);
    }
    return comp[0];
}

int ih_eth_admin(struct ih *h, const uint8_t cmd[64], uint8_t comp[16])
{
    struct ih_ring *q = &h->eth_aq;
    struct ih_ring *cq = &h->eth_acq;
    uint32_t slot = q->prod % ring_depth(q);

    if (!fake_vfu_write(h->vfu, ring_slot(q, q->prod), cmd, 64))
        return fail_rc("eth adminq write failed");
    q->prod++;
    ih_doorbell(h, IH_QTYPE_ETH_ADMINQ, 0, 0,
                (uint16_t)(q->prod % ring_depth(q)));

    /* struct ionic_admin_comp: status, rsvd, le16 comp_index, ... colour in
     * the top bit of the last byte */
    if (!fake_vfu_read(h->vfu, ring_slot(cq, cq->cons), comp, 16))
        return fail_rc("eth admin CQ read failed");
    if (((comp[15] & 0x80u) != 0) != ring_color(cq)) {
        fail("eth adminq command %u drew no completion", cmd[0]);
        return -1;
    }
    cq->cons++;
    if (get_le16(comp + 2) != slot) {
        fail("eth admin completion for slot %u, expected %u",
             get_le16(comp + 2), slot);
        return -1;
    }
    return comp[0];
}

/*
 * The next event off the EQ ring itself, ignoring anything an admin wait
 * has already stepped over. The admin wait uses this rather than
 * ih_eq_next(), which would hand back the events it is holding and leave it
 * stepping over the same ones forever.
 */
static bool eq_next_ring(struct ih *h, uint32_t *cq_id)
{
    uint8_t b[4];

    /* struct ionic_v1_eqe: be32 of colour, type, code and qid */
    if (!fake_vfu_read(h->vfu, ring_slot(&h->eq, h->eq.cons), b, 4))
        return false;
    uint32_t evt = get_be32(b);
    if (((evt & 1u) != 0) != ring_color(&h->eq))
        return false;
    h->eq.cons++;
    *cq_id = evt >> 8;
    return true;
}

bool ih_eq_next(struct ih *h, uint32_t *cq_id)
{
    /* Anything an admin wait stepped over comes back first, in order. */
    if (h->eq_held_n) {
        *cq_id = h->eq_held[0];
        memmove(h->eq_held, h->eq_held + 1,
                (--h->eq_held_n) * sizeof(h->eq_held[0]));
        return true;
    }
    return eq_next_ring(h, cq_id);
}

int ih_admin_post(struct ih *h, uint8_t op, const void *body, uint16_t len)
{
    struct ih_ring *q = &h->aq;
    uint32_t strides = (ADMIN_HDR_LEN + len + ADMIN_STRIDE - 1) / ADMIN_STRIDE;
    uint8_t buf[4 * ADMIN_STRIDE];

    if (strides > 4)
        return fail_rc("admin body of %u bytes is too long", len);

    /* The device reads a multi-stride WQE linearly, so don't let one wrap:
     * pad to the end of the ring with NOOPs, as a driver would have to. */
    while (q->prod % ring_depth(q) + strides > ring_depth(q)) {
        if (ih_admin(h, IH_ADMIN_NOOP, NULL, 0, NULL) != 0)
            return -1;
    }

    uint32_t slot = q->prod % ring_depth(q);

    memset(buf, 0, sizeof(buf));
    buf[0] = op;
    put_le16(buf + 2, len);
    if (len)
        memcpy(buf + ADMIN_HDR_LEN, body, len);
    if (!fake_vfu_write(h->vfu, ring_slot(q, q->prod), buf,
                        strides * ADMIN_STRIDE))
        return fail_rc("admin WQE write failed");
    q->prod += strides;
    ih_doorbell(h, IH_QTYPE_AQ, 0, 0, (uint16_t)(q->prod % ring_depth(q)));
    return (int)slot;
}

int ih_admin(struct ih *h, uint8_t op, const void *body, uint16_t len,
             uint32_t *resp_len)
{
    int slot = ih_admin_post(h, op, body, len);

    if (slot < 0)
        return -1;
    ih_poll(h);

    int rc = ih_admin_complete(h, op, slot, resp_len);
    if (rc == IH_ADMIN_PENDING)
        return fail_rc("admin op %u drew no completion", op);
    return rc;
}

int ih_admin_complete(struct ih *h, uint8_t op, int slot, uint32_t *resp_len)
{
    struct ih_ring *cq = &h->acq;

    /* ionic_v1_cqe for an admin command: be16 cmd_idx, u8 cmd_op, ...,
     * be32 status_length, be32 qid_type_flags */
    uint8_t cqe[32];
    if (!fake_vfu_read(h->vfu, ring_slot(cq, cq->cons), cqe, sizeof(cqe)))
        return fail_rc("admin CQ read failed");
    uint32_t qtf = get_be32(cqe + 28);
    if (((qtf & 1u) != 0) != ring_color(cq))
        return IH_ADMIN_PENDING;
    cq->cons++;

    if (get_be16(cqe) != (uint32_t)slot || cqe[2] != op || (qtf >> 8) != 0 ||
        ((qtf >> 5) & 7u) != IH_CQE_TYPE_ADMIN) {
        fail("admin completion for op %u slot %u (qid %u type %u), expected "
             "op %u slot %u",
             cqe[2], get_be16(cqe), qtf >> 8, (qtf >> 5) & 7u, op,
             (unsigned)slot);
        return -1;
    }

    /*
     * The driver only looks at the admin CQ when the EQ says to. Other CQs'
     * events may be queued ahead of it -- an admin command can complete work
     * on a data CQ, which MODIFY_QP to Error does when it flushes -- so step
     * over them and hold them for ih_eq_next() rather than requiring the
     * admin event to be first.
     */
    uint32_t ev_cq;
    bool saw_admin = false;

    while (eq_next_ring(h, &ev_cq)) {
        if (ev_cq == IH_ACQ_ID) {
            saw_admin = true;
            break;
        }
        if (h->eq_held_n == IH_EQ_HELD_MAX) {
            fail("more than %u EQ events ahead of admin op %u's",
                 IH_EQ_HELD_MAX, op);
            return -1;
        }
        h->eq_held[h->eq_held_n++] = ev_cq;
    }
    if (!saw_admin) {
        fail("admin op %u completed with no event on the EQ", op);
        return -1;
    }

    uint32_t sl = get_be32(cqe + 24);
    if (qtf & 2u) {
        if (!(sl >> 24)) {
            fail("admin op %u has the error bit with status 0", op);
            return -1;
        }
        return (int)(sl >> 24);
    }
    if (resp_len)
        *resp_len = sl;
    return 0;
}

bool ih_bringup(struct ih *h)
{
    uint8_t cmd[64], comp[16];

    if (ih_bar0_read32(h, 0) != IONIC_DEV_SIG)
        return fail("no DEVI signature at BAR0 offset 0");

    /* ionic_identify(), ionic_reset(), ionic_port_identify/init(),
     * ionic_lif_identify(), ionic_lif_init(). */
    static const uint8_t simple[] = {IH_CMD_IDENTIFY, IH_CMD_RESET,
                                     IH_CMD_PORT_IDENTIFY, IH_CMD_LIF_IDENTIFY};
    for (size_t i = 0; i < sizeof(simple); i++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = simple[i];
        if (ih_devcmd(h, cmd, comp, NULL, 0) != 0)
            return fail("devcmd %u failed", simple[i]);
    }

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_PORT_INIT;
    put_le64(cmd + 8, h->port_info);
    if (ih_devcmd(h, cmd, comp, NULL, 0) != 0)
        return fail("PORT_INIT failed");

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_LIF_INIT;
    put_le64(cmd + 8, h->lif_info);
    if (ih_devcmd(h, cmd, comp, NULL, 0) != 0)
        return fail("LIF_INIT failed");

    /* The Ethernet admin queue: struct ionic_q_init_cmd */
    if (!ring_alloc(h, &h->eth_aq, ETH_AQ_DEPTH_LOG2, 6) ||
        !ring_alloc(h, &h->eth_acq, ETH_AQ_DEPTH_LOG2, 4))
        return false;
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_Q_INIT;
    cmd[4] = IH_QTYPE_ETH_ADMINQ;
    put_le32(cmd + 8, 0);
    put_le16(cmd + 14, IH_ETH_ADMINQ_INTR);
    cmd[19] = ETH_AQ_DEPTH_LOG2;
    put_le64(cmd + 20, h->eth_aq.gpa);
    put_le64(cmd + 28, h->eth_acq.gpa);
    if (ih_devcmd(h, cmd, comp, NULL, 0) != 0)
        return fail("Q_INIT of the adminq failed");

    ih_intr_mask(h, IH_ETH_ADMINQ_INTR, false);
    ih_intr_mask(h, IH_EQ_INTR, false);

    /* ionic_rdma_reset_devcmd() and the queue devcmds of
     * ionic_create_rdma_admin(), all over the Ethernet admin queue */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_RDMA_RESET;
    if (ih_eth_admin(h, cmd, comp) != 0)
        return fail("RDMA_RESET_LIF failed");

    struct {
        uint8_t op;
        struct ih_ring *ring;
        uint8_t depth_log2, stride_log2;
        uint32_t qid, cid;
    } queues[] = {
        {IH_CMD_RDMA_EQ, &h->eq, EQ_DEPTH_LOG2, 2, IH_EQ_ID, IH_EQ_INTR},
        {IH_CMD_RDMA_CQ, &h->acq, ACQ_DEPTH_LOG2, CQE_STRIDE_LOG2, IH_ACQ_ID,
         IH_EQ_ID},
        {IH_CMD_RDMA_ADMINQ, &h->aq, AQ_DEPTH_LOG2, 6, 0, IH_ACQ_ID},
    };
    for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); i++) {
        if (!ring_alloc(h, queues[i].ring, queues[i].depth_log2,
                        queues[i].stride_log2))
            return false;
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = queues[i].op;
        put_le32(cmd + 4, queues[i].qid | 1u << 24);
        put_le32(cmd + 8, queues[i].cid);
        cmd[14] = queues[i].depth_log2;
        cmd[15] = queues[i].stride_log2;
        put_le64(cmd + 16, queues[i].ring->gpa);
        if (ih_eth_admin(h, cmd, comp) != 0)
            return fail("RDMA queue devcmd %u failed", queues[i].op);
    }

    /* The server wires the admin queue in on its next loop. */
    ih_poll(h);
    return true;
}

/* ---- Resources --------------------------------------------------------- */

bool ih_cq_create(struct ih *h, struct ih_cq *cq, uint32_t id,
                  uint8_t depth_log2)
{
    uint8_t body[34] = {0};

    cq->id = id;
    if (!ring_alloc(h, &cq->ring, depth_log2, CQE_STRIDE_LOG2))
        return false;

    /* struct ionic_admin_create_cq */
    put_le32(body + 0, IH_EQ_ID);
    body[4] = depth_log2;
    body[5] = CQE_STRIDE_LOG2;
    body[7] = 12;
    put_le32(body + 12, id | 1u << 24);
    put_le32(body + 20, 1);
    put_le64(body + 24, cq->ring.gpa);
    int rc = ih_admin(h, IH_ADMIN_CREATE_CQ, body, sizeof(body), NULL);
    return rc == 0 || fail("CREATE_CQ %u: %d", id, rc);
}

int ih_cq_destroy(struct ih *h, struct ih_cq *cq)
{
    uint8_t body[4];

    put_le32(body, cq->id);
    return ih_admin(h, IH_ADMIN_DESTROY_CQ, body, sizeof(body), NULL);
}

bool ih_qp_create(struct ih *h, struct ih_qp *qp, uint32_t id, uint8_t type,
                  const struct ih_cq *scq, const struct ih_cq *rcq,
                  const struct ih_srq *srq)
{
    return ih_qp_create_stride(h, qp, id, type, scq, rcq, srq, WQE_STRIDE_LOG2);
}

bool ih_qp_create_stride(struct ih *h, struct ih_qp *qp, uint32_t id,
                         uint8_t type, const struct ih_cq *scq,
                         const struct ih_cq *rcq, const struct ih_srq *srq,
                         uint8_t sq_stride_log2)
{
    uint8_t body[64] = {0};

    if (sq_stride_log2 > IH_WQE_MAX_STRIDE_LOG2)
        return fail("SQ stride 2^%u is above 2^%u", sq_stride_log2,
                    IH_WQE_MAX_STRIDE_LOG2);
    memset(qp, 0, sizeof(*qp));
    qp->id = id;
    qp->type = type;
    qp->next_wqe_id = 1;
    if (!ring_alloc(h, &qp->sq, QP_DEPTH_LOG2, sq_stride_log2))
        return false;
    if (!srq && !ring_alloc(h, &qp->rq, QP_DEPTH_LOG2, WQE_STRIDE_LOG2))
        return false;

    /* struct ionic_admin_create_qp */
    put_le32(body + 8, scq->id);
    body[12] = QP_DEPTH_LOG2;
    body[13] = sq_stride_log2;
    body[15] = 12;
    put_le32(body + 20, 1);
    put_le64(body + 24, qp->sq.gpa);
    put_le32(body + 32, rcq->id);
    if (srq) {
        /* An SRQ-bound QP leaves its own receive ring empty. */
        put_le32(body + 40, srq->id);
    } else {
        body[36] = QP_DEPTH_LOG2;
        body[37] = WQE_STRIDE_LOG2;
        body[39] = 12;
        put_le32(body + 40, 0xffffffffu);
        put_le32(body + 44, 1);
        put_le64(body + 48, qp->rq.gpa);
    }
    put_le32(body + 56, id | 1u << 24);
    body[62] = type;
    int rc = ih_admin(h, IH_ADMIN_CREATE_QP, body, sizeof(body), NULL);
    return rc == 0 || fail("CREATE_QP %u: %d", id, rc);
}

int ih_qp_destroy(struct ih *h, struct ih_qp *qp)
{
    uint8_t body[4];

    put_le32(body, qp->id);
    return ih_admin(h, IH_ADMIN_DESTROY_QP, body, sizeof(body), NULL);
}

int ih_qp_modify(struct ih *h, uint32_t qp_id, uint32_t attr_mask,
                 uint8_t state, uint32_t qkey_dest_qpn, const void *hdr,
                 uint32_t hdr_len)
{
    uint8_t body[60] = {0};

    /* The template goes in the scratch page, which is the driver's
     * DMA-mapped header buffer here. */
    if (hdr) {
        if (hdr_len > 255 || !fake_vfu_write(h->vfu, h->scratch, hdr, hdr_len))
            return fail_rc("bad RoCE header template");
        put_le32(body + 32, hdr_len << 24);
        put_le64(body + 48, h->scratch);
    }

    /* struct ionic_admin_mod_qp */
    put_be32(body + 0, attr_mask);
    put_le32(body + 8, 0x100);  /* rq_psn */
    put_le32(body + 12, 0x200); /* sq_psn */
    put_le32(body + 16, qkey_dest_qpn);
    body[39] = state;
    put_le32(body + 56, qp_id | 1u << 24);
    return ih_admin(h, IH_ADMIN_MODIFY_QP, body, sizeof(body), NULL);
}

size_t ih_roce_hdr_v4(uint8_t *buf, size_t cap, uint32_t sip, uint32_t dip)
{
    static const uint8_t dmac[6] = {0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc};
    static const uint8_t smac[6] = {0x02, 0x00, 0x00, 0x11, 0x22, 0x33};
    const size_t len = 14 + 20 + 8;

    if (cap < len)
        return 0;
    memset(buf, 0, len);
    memcpy(buf, dmac, 6);
    memcpy(buf + 6, smac, 6);
    put_be16(buf + 12, 0x0800);

    uint8_t *ip = buf + 14;
    ip[0] = 0x45;
    put_be16(ip + 2, 20 + 8);
    ip[8] = 64;
    ip[9] = 17; /* UDP */
    put_be32(ip + 12, sip);
    put_be32(ip + 16, dip);

    put_be16(buf + 34 + 2, ROCE_UDP_PORT);
    put_be16(buf + 34 + 4, 8);
    return len;
}

bool ih_qp_connect(struct ih *h, const struct ih_qp *a, const struct ih_qp *b)
{
    const struct ih_qp *ends[2][2] = {{a, b}, {b, a}};
    uint8_t hdr[64];
    size_t hdr_len = ih_roce_hdr_v4(hdr, sizeof(hdr), 0xc0a80001u, 0xc0a80002u);

    for (size_t i = 0; i < 2; i++) {
        uint32_t self = ends[i][0]->id, peer = ends[i][1]->id;
        int rc;

        if ((rc = ih_qp_modify(h, self, IH_QP_STATE, IH_QPS_INIT, 0, NULL,
                               0)) != 0 ||
            (rc = ih_qp_modify(
                 h, self,
                 IH_QP_STATE | IH_QP_AV | IH_QP_DEST_QPN | IH_QP_RQ_PSN,
                 IH_QPS_RTR, peer, hdr, (uint32_t)hdr_len)) != 0 ||
            (rc = ih_qp_modify(h, self, IH_QP_STATE | IH_QP_SQ_PSN, IH_QPS_RTS,
                               0, NULL, 0)) != 0)
            return fail("connecting QP %u to %u: %d", self, peer, rc);
    }
    return true;
}

void ih_mr_free(struct ih_mr *mr)
{
    free(mr->pages);
    mr->pages = NULL;
}

bool ih_mr_alloc(struct ih *h, struct ih_mr *mr, uint32_t lkey, uint64_t va,
                 uint64_t len, bool reversed)
{
    uint64_t first_off = va % IH_PAGE_SIZE;

    memset(mr, 0, sizeof(*mr));
    mr->lkey = lkey;
    mr->va = va;
    mr->len = len;
    mr->npages =
        (uint32_t)((first_off + len + IH_PAGE_SIZE - 1) / IH_PAGE_SIZE);
    mr->pages = calloc(mr->npages, sizeof(*mr->pages));
    if (!mr->pages)
        return fail("out of memory");

    for (uint32_t i = 0; i < mr->npages; i++) {
        uint32_t idx = reversed ? mr->npages - 1 - i : i;
        mr->pages[idx] = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
        if (!mr->pages[idx]) {
            ih_mr_free(mr);
            return fail("out of guest memory for an MR");
        }
    }

    mr->pt_gpa = fake_vfu_alloc(h->vfu, mr->npages * sizeof(uint64_t), 8);
    if (!mr->pt_gpa) {
        ih_mr_free(mr);
        return fail("out of guest memory for a page table");
    }
    for (uint32_t i = 0; i < mr->npages; i++) {
        uint8_t le[8];

        put_le64(le, mr->pages[i]);
        fake_vfu_write(h->vfu, mr->pt_gpa + i * sizeof(uint64_t), le, 8);
    }
    return true;
}

/*
 * What the driver puts in a dma_addr field: the page table, or for a region
 * that fits one page, the page itself with the offset folded in (as
 * ionic_pgtbl_dma() does).
 */
static uint64_t mr_dma_addr(const struct ih_mr *mr)
{
    if (mr->npages == 1)
        return mr->pages[0] + mr->va % IH_PAGE_SIZE;
    return mr->pt_gpa;
}

bool ih_mr_create(struct ih *h, struct ih_mr *mr, uint32_t lkey, uint64_t va,
                  uint64_t len, uint32_t access, bool reversed)
{
    uint8_t body[45] = {0};

    if (!ih_mr_alloc(h, mr, lkey, va, len, reversed))
        return false;

    /* struct ionic_admin_create_mr */
    put_le64(body + 0, va);
    put_le64(body + 8, len);
    put_le32(body + 20, lkey);
    put_le32(body + 28, mr->npages);
    put_le64(body + 32, mr_dma_addr(mr));
    put_le16(body + 40, (uint16_t)access);
    body[44] = 12;
    int rc = ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL);
    if (rc != 0) {
        ih_mr_free(mr);
        return fail("CREATE_MR %#x: %d", lkey, rc);
    }
    return true;
}

int ih_mr_destroy(struct ih *h, struct ih_mr *mr)
{
    uint8_t body[4];

    put_le32(body, mr->lkey);
    ih_mr_free(mr);
    return ih_admin(h, IH_ADMIN_DESTROY_MR, body, sizeof(body), NULL);
}

static bool mr_copy(struct ih *h, const struct ih_mr *mr, uint64_t va,
                    void *buf, size_t len, bool to_guest)
{
    if (va < mr->va || len > mr->len || va - mr->va > mr->len - len)
        return fail("[%#lx, +%zu) is outside MR %#x", (unsigned long)va, len,
                    mr->lkey);

    uint64_t off = va - mr->va + mr->va % IH_PAGE_SIZE;
    size_t done = 0;

    while (done < len) {
        uint64_t gpa = mr->pages[off / IH_PAGE_SIZE] + off % IH_PAGE_SIZE;
        size_t n = IH_PAGE_SIZE - (size_t)(off % IH_PAGE_SIZE);

        if (n > len - done)
            n = len - done;
        bool ok = to_guest
                      ? fake_vfu_write(h->vfu, gpa, (uint8_t *)buf + done, n)
                      : fake_vfu_read(h->vfu, gpa, (uint8_t *)buf + done, n);
        if (!ok)
            return fail("MR page at %#lx is not guest memory",
                        (unsigned long)gpa);
        done += n;
        off += n;
    }
    return true;
}

bool ih_mr_write(struct ih *h, const struct ih_mr *mr, uint64_t va,
                 const void *buf, size_t len)
{
    /* mr_copy() only reads from buf when copying to the guest. */
    return mr_copy(h, mr, va, (void *)(uintptr_t)buf, len, true);
}

bool ih_mr_read(struct ih *h, const struct ih_mr *mr, uint64_t va, void *buf,
                size_t len)
{
    return mr_copy(h, mr, va, buf, len, false);
}

int ih_srq_create(struct ih *h, struct ih_srq *srq, uint8_t depth_log2)
{
    uint8_t body[42] = {0};
    const uint64_t resp = h->scratch + 512;
    const uint8_t poison[5] = {0xff, 0xff, 0xff, 0xff, 0xff};

    memset(srq, 0, sizeof(*srq));
    srq->next_wqe_id = 1;
    if (!ring_alloc(h, &srq->ring, depth_log2, WQE_STRIDE_LOG2))
        return -1;
    fake_vfu_write(h->vfu, resp, poison, sizeof(poison));

    /* struct ionic_admin_create_srq */
    put_le64(body + 0, srq->ring.gpa);
    put_le32(body + 8, 1);
    body[26] = depth_log2;
    body[27] = WQE_STRIDE_LOG2;
    body[28] = 12;
    put_le32(body + 30, 5);
    put_le64(body + 34, resp);

    uint32_t resp_len = 0;
    int rc = ih_admin(h, IH_ADMIN_CREATE_SRQ, body, sizeof(body), &resp_len);
    if (rc != 0)
        return rc;

    /* struct ionic_admin_create_srq_resp: le32 id, u8 udma_idx */
    uint8_t r[5];
    fake_vfu_read(h->vfu, resp, r, sizeof(r));
    srq->id = get_le32(r);
    if (resp_len != 5) {
        fail("CREATE_SRQ reported a %u-byte response", resp_len);
        return -1;
    }
    return 0;
}

int ih_srq_destroy(struct ih *h, struct ih_srq *srq)
{
    uint8_t body[4];

    put_le32(body, srq->id);
    return ih_admin(h, IH_ADMIN_DESTROY_SRQ, body, sizeof(body), NULL);
}

int ih_ah_create(struct ih *h, uint32_t ah_id, uint32_t sip, uint32_t dip)
{
    uint8_t hdr[64];
    uint8_t body[24] = {0};
    size_t hdr_len = ih_roce_hdr_v4(hdr, sizeof(hdr), sip, dip);

    if (!fake_vfu_write(h->vfu, h->scratch, hdr, hdr_len))
        return -1;

    /* struct ionic_admin_create_ah */
    put_le64(body + 0, h->scratch);
    put_le32(body + 8, (uint32_t)hdr_len);
    put_le32(body + 16, ah_id | 1u << 24);
    return ih_admin(h, IH_ADMIN_CREATE_AH, body, sizeof(body), NULL);
}

/* ---- Work requests and completions ------------------------------------- */

static void put_sges(uint8_t *wqe, const struct ih_sge *sge, uint8_t nsge)
{
    for (uint8_t i = 0; i < nsge; i++) {
        uint8_t *p = wqe + WQE_PLD_OFF + 16u * i;

        put_be64(p, sge[i].va);
        put_be32(p + 8, sge[i].len);
        put_be32(p + 12, sge[i].lkey);
    }
}

uint64_t ih_post_send(struct ih *h, struct ih_qp *qp, const struct ih_send *s)
{
    const uint32_t stride = 1u << qp->sq.stride_log2;
    uint8_t wqe[1u << IH_WQE_MAX_STRIDE_LOG2];
    uint64_t wqe_id = qp->next_wqe_id++;
    uint32_t total = 0;

    if (s->nsge > (stride - WQE_PLD_OFF) / 16 ||
        s->inline_len > stride - WQE_PLD_OFF) {
        fail("send WQE does not fit a %u-byte stride", stride);
        return 0;
    }
    for (uint8_t i = 0; i < s->nsge; i++)
        total += s->sge[i].len;

    /* struct ionic_v1_wqe */
    memset(wqe, 0, sizeof(wqe));
    memcpy(wqe, &wqe_id, 8);
    wqe[8] = s->op;
    wqe[9] = s->nsge;
    put_be16(wqe + 10, s->flags);
    if (s->op == IH_OP_SEND_INV || s->op == IH_OP_LOCAL_INV ||
        s->op == IH_OP_REG_MR)
        put_be32(wqe + 12, s->key);
    else
        memcpy(wqe + 12, &s->imm_be, 4);

    switch (s->op) {
    case IH_OP_RDMA_READ:
    case IH_OP_RDMA_WRITE:
    case IH_OP_RDMA_WRITE_IMM:
        put_be32_pair(wqe + 16, s->remote_va);
        put_be32(wqe + 24, s->rkey);
        put_be32(wqe + 28, s->length ? s->length : total);
        break;

    case IH_OP_ATOMIC_CS:
    case IH_OP_ATOMIC_FA:
        /* struct ionic_v1_atomic_bdy: one fixed 8-byte result SGE */
        put_be32_pair(wqe + 16, s->remote_va);
        put_be32(wqe + 24, s->rkey);
        put_be32_pair(wqe + 28, s->swap_add);
        put_be32_pair(wqe + 36, s->compare);
        if (s->nsge) {
            put_be64(wqe + 48, s->sge[0].va);
            put_be32(wqe + 56, 8);
            put_be32(wqe + 60, s->sge[0].lkey);
        }
        break;

    case IH_OP_REG_MR:
        /* struct ionic_v1_reg_mr_bdy */
        if (s->mr) {
            put_be64(wqe + 16, s->mr->va);
            put_be64(wqe + 24, s->mr->len);
            put_be64(wqe + 40, mr_dma_addr(s->mr));
            put_be32(wqe + 48, s->mr->npages);
            wqe[55] = 12;
        }
        break;

    case IH_OP_LOCAL_INV:
        break;

    default:
        /* The sends, and anything a test makes up */
        put_be32(wqe + 16, s->ah_id);
        put_be32(wqe + 20, s->dest_qpn);
        put_be32(wqe + 24, s->qkey);
        put_be32(wqe + 28, total);
        break;
    }

    if (s->op != IH_OP_ATOMIC_CS && s->op != IH_OP_ATOMIC_FA &&
        s->op != IH_OP_REG_MR) {
        if (s->flags & IH_WQE_INLINE) {
            memcpy(wqe + WQE_PLD_OFF, s->inline_data, s->inline_len);
            put_be32(wqe + 28, s->inline_len);
        } else {
            put_sges(wqe, s->sge, s->nsge);
        }
    }

    if (!fake_vfu_write(h->vfu, ring_slot(&qp->sq, qp->sq.prod), wqe, stride)) {
        fail("SQ write failed");
        return 0;
    }
    qp->sq.prod++;
    ih_doorbell(h, IH_QTYPE_SQ, qp->id, 0,
                (uint16_t)(qp->sq.prod % ring_depth(&qp->sq)));
    return wqe_id;
}

static uint64_t post_recv(struct ih *h, struct ih_ring *rq, uint32_t qid,
                          uint64_t *next_wqe_id, const struct ih_sge *sge,
                          uint8_t nsge)
{
    uint8_t wqe[1u << WQE_STRIDE_LOG2];
    uint64_t wqe_id = (*next_wqe_id)++;

    if (nsge > (sizeof(wqe) - WQE_PLD_OFF) / 16) {
        fail("receive WQE has too many SGEs");
        return 0;
    }
    memset(wqe, 0, sizeof(wqe));
    memcpy(wqe, &wqe_id, 8);
    wqe[9] = nsge;
    put_sges(wqe, sge, nsge);
    if (!fake_vfu_write(h->vfu, ring_slot(rq, rq->prod), wqe, sizeof(wqe))) {
        fail("RQ write failed");
        return 0;
    }
    rq->prod++;
    ih_doorbell(h, IH_QTYPE_RQ, qid, 0, (uint16_t)(rq->prod % ring_depth(rq)));
    return wqe_id;
}

uint64_t ih_post_recv(struct ih *h, struct ih_qp *qp, const struct ih_sge *sge,
                      uint8_t nsge)
{
    return post_recv(h, &qp->rq, qp->id, &qp->next_wqe_id, sge, nsge);
}

uint64_t ih_post_srq_recv(struct ih *h, struct ih_srq *srq,
                          const struct ih_sge *sge, uint8_t nsge)
{
    return post_recv(h, &srq->ring, srq->id, &srq->next_wqe_id, sge, nsge);
}

bool ih_cq_poll(struct ih *h, struct ih_cq *cq, struct ih_cqe *cqe)
{
    struct ih_ring *r = &cq->ring;
    uint8_t *b = cqe->raw;

    memset(cqe, 0, sizeof(*cqe));
    if (!fake_vfu_read(h->vfu, ring_slot(r, r->cons), b, sizeof(cqe->raw)))
        return false;

    /* struct ionic_v1_cqe */
    uint32_t qtf = get_be32(b + 28);
    if (((qtf & 1u) != 0) != ring_color(r))
        return false;
    r->cons++;
    ih_doorbell(h, IH_QTYPE_CQ, cq->id, 0, (uint16_t)(r->cons % ring_depth(r)));

    cqe->type = (uint8_t)((qtf >> 5) & 7u);
    cqe->error = (qtf & 2u) != 0;
    cqe->qid = qtf >> 8;
    cqe->status_len = get_be32(b + 24);

    switch (cqe->type) {
    case IH_CQE_TYPE_RECV: {
        uint32_t qpn_op = get_be32(b + 8);

        memcpy(&cqe->wqe_id, b, 8);
        /* 0x1f, as the driver masks it: bits 30 and 31 of src_qpn_op are
         * IS_VLAN and IS_IPV4, not part of the opcode. */
        cqe->recv_op = (uint8_t)((qpn_op >> 24) & 0x1fu);
        cqe->src_qpn = qpn_op & 0xffffffu;
        cqe->is_ipv4 = (qpn_op & 0x80000000u) != 0;
        memcpy(cqe->smac, b + 12, 6);
        /*
         * There is no SMAC-valid bit on the wire: the driver sets
         * IB_WC_WITH_SMAC for every UD/GSI completion and copies the field
         * unconditionally. Presence is therefore inferred from the field
         * itself, which the device only fills in for a datagram. This used
         * to read IS_IPV4 instead, which held only while the device set
         * that bit on every receive that carried a MAC.
         */
        static const uint8_t no_mac[6] = {0};
        cqe->has_smac = memcmp(cqe->smac, no_mac, sizeof(no_mac)) != 0;
        memcpy(&cqe->imm_be, b + 20, 4);
        break;
    }
    case IH_CQE_TYPE_SEND_MSN:
        cqe->msn = get_be32(b + 4);
        break;
    case IH_CQE_TYPE_SEND_NPG:
        memcpy(&cqe->wqe_id, b + 16, 8);
        break;
    default:
        break;
    }
    return true;
}

void ih_cq_arm(struct ih *h, struct ih_cq *cq)
{
    ih_doorbell(h, IH_QTYPE_CQ, cq->id, 1,
                (uint16_t)(cq->ring.cons % ring_depth(&cq->ring)));
}
