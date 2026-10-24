/*
 * test_ionic_harness.c — the ionic device end to end, in-process
 *
 * Every other ionic test reaches into one translation unit and stubs the
 * rest. These link the whole device -- the Ethernet emulator, the RDMA
 * devcmds, the admin queue, the data path, the compat bridge, the resource
 * manager and the loopback and TCP backends -- and drive it the way ionic.ko
 * and ionic_rdma.ko do, through the model in ionic_harness.c. Only
 * libvfio-user is replaced, by fake_vfu.c, so nothing needs a VM or RDMA
 * hardware, only the TCP backend opens a socket, on loopback, and the whole
 * stack runs under every sanitizer CI builds.
 *
 * With no argument every group runs; with one, only the group it names.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <endian.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "byteorder.h"
#include "fake_vfu.h"
#include "ionic_eth_emu.h"
/* For the emulator's admin queue pointer, which rdma-reset checks */
#include "ionic_eth_emu_priv.h"
#include "ionic_harness.h"
#include "ionic_rdma_devcmd.h"
#include "ionic_status.h"
#include "nvmeof.h"
#include "nvmeof_cm.h"
#ifdef ERNIC_HAVE_P4
#include "ionic_datapath.h"
#include "p4_devcmd.h"
#include "p4_engine.h"
#include "p4_fp8.h"
#include "p4_fmt.h"
#include "p4_port.h"
#include "p4_table.h"
#endif
#include "pvrdma.h"
#include "rdma_rm.h"
#include "rocm_ernic_compat.h"
#include "rocm_ernic_internal.h"

static int failures;

static bool check(const char *test, bool cond, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool check(const char *test, bool cond, const char *fmt, ...)
{
    va_list ap;

    if (cond)
        return true;
    printf("FAIL %-12s: ", test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures++;
    return false;
}

/* ---- Helpers ----------------------------------------------------------- */

static bool all_zero(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (p[i])
            return false;
    return true;
}

/*
 * The one's-complement sum of a header, which for a correct IPv4 checksum
 * comes to 0xffff -- summing the checksum field in with everything else
 * rather than recomputing it around a hole.
 */
static uint16_t ones_sum16(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;

    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ernic_get_be16(p + i);
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)sum;
}

/* A byte pattern that differs from row to row and from byte to byte. */
static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i * 7 + (i >> 8));
}

/* Whether @cq has no completion waiting. */
static bool cq_empty(struct ih *h, struct ih_cq *cq)
{
    struct ih_cqe e;

    return !ih_cq_poll(h, cq, &e);
}

/* Drain the EQ, counting the events for @cq_id. */
static unsigned eq_count(struct ih *h, uint32_t cq_id)
{
    unsigned n = 0;
    uint32_t id;

    while (ih_eq_next(h, &id))
        n += id == cq_id;
    return n;
}

static bool setup(const char *test, struct ih *h, const char *backend)
{
    if (!check(test, ih_create(h, backend), "device for '%s' not created",
               backend))
        return false;
    if (!check(test, ih_bringup(h), "bring-up failed")) {
        ih_destroy(h);
        return false;
    }
    return true;
}

static void teardown(const char *test, struct ih *h)
{
    check(test, ih_destroy(h), "teardown left guest memory mapped");
}

/* ---- Bring-up: devcmds, the Ethernet admin queue and interrupts --------- */

/* Byte offsets into the devcmd data window after the identify commands */
#define DEVID_NLIFS          0x08u
#define DEVID_NINTRS         0x0cu
#define DEVID_COAL_DIV       0x18u
#define LIFID_MAC            0x0a0u
#define LIFID_RDMA           392u
#define LIFID_RDMA_ADMIN_OPS 2u

static void test_devcmds(void)
{
    const char *T = "devcmds";
    struct ih h;
    uint8_t cmd[64], comp[16], data[1912];

    if (!check(T, ih_create(&h, "loopback"), "device not created"))
        return;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_IDENTIFY;
    check(T, ih_devcmd(&h, cmd, comp, data, 512) == 0, "IDENTIFY failed");
    check(T, ernic_get_le32(data + DEVID_NLIFS) == 1, "nlifs is %u",
          ernic_get_le32(data + DEVID_NLIFS));
    /* The default compat profile's intr_vectors; IONIC_MSIX_MAX_VECTORS is
     * only the most a profile may ask for. */
    check(T, ernic_get_le32(data + DEVID_NINTRS) == 32, "nintrs is %u",
          ernic_get_le32(data + DEVID_NINTRS));
    check(T, ernic_get_le32(data + DEVID_COAL_DIV) != 0,
          "intr_coal_div is 0, which ethtool divides by");

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_LIF_IDENTIFY;
    check(T, ih_devcmd(&h, cmd, comp, data, sizeof(data)) == 0,
          "LIF_IDENTIFY failed");
    check(T, (data[0] & 3u) == 3u, "LIF capabilities lack ETH or RDMA");
    static const uint8_t default_mac[6] = {0x02, 0xa0, 0xd1, 0, 0, 1};
    check(T, memcmp(data + LIFID_MAC, default_mac, 6) == 0,
          "LIF identity does not carry the default MAC");

    /* The RDMA qtypes, read where ionic_rdma.ko reads them */
    const uint8_t *rdma = data + LIFID_RDMA;
    static const struct {
        uint32_t off;
        uint8_t qtype;
    } qtypes[] = {
        {32, IH_QTYPE_AQ}, {44, IH_QTYPE_SQ}, {56, IH_QTYPE_RQ},
        {68, IH_QTYPE_CQ}, {80, 9},           {96, 10},
    };
    for (size_t i = 0; i < sizeof(qtypes) / sizeof(qtypes[0]); i++)
        check(T, rdma[qtypes[i].off] == qtypes[i].qtype,
              "qtype at rdma+%u is %u, expected %u", qtypes[i].off,
              rdma[qtypes[i].off], qtypes[i].qtype);
    check(T, ernic_get_le32(rdma + 100) == IONIC_EMU_SRQ_COUNT,
          "srq qid_count is %u", ernic_get_le32(rdma + 100));
    check(T, rdma[113] & 2u, "SRQ qid allocation not advertised");
    check(T, rdma[LIFID_RDMA_ADMIN_OPS] > IH_ADMIN_DESTROY_SRQ,
          "admin_opcodes %u refuses the SRQ commands",
          rdma[LIFID_RDMA_ADMIN_OPS]);

    /* Q_IDENTIFY for the Tx queue reports its scatter/gather layout */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_Q_IDENTIFY;
    cmd[3] = IH_QTYPE_ETH_TXQ;
    check(T, ih_devcmd(&h, cmd, comp, data, 64) == 0, "Q_IDENTIFY failed");
    check(T, data[16] == 16 && data[20] == 128,
          "Tx descriptor sizes are %u and %u", data[16], data[20]);

    /* Commands with nothing to configure all succeed */
    static const uint8_t trivial[] = {IH_CMD_NOP,
                                      2 /* INIT */,
                                      4 /* GETATTR */,
                                      5 /* SETATTR */,
                                      IH_CMD_LIF_RESET,
                                      IH_CMD_Q_CONTROL,
                                      12 /* PORT_RESET */};
    for (size_t i = 0; i < sizeof(trivial); i++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = trivial[i];
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 0, "devcmd %u failed",
              trivial[i]);
    }

    /* LIF_SETATTR and LIF_GETATTR of the station MAC, as devcmds */
    static const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_LIF_SETATTR;
    cmd[1] = IH_LIF_ATTR_MAC;
    memcpy(cmd + 4, mac, 6);
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 0, "LIF_SETATTR failed");
    cmd[0] = IH_CMD_LIF_GETATTR;
    memset(cmd + 4, 0, 6);
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 0, "LIF_GETATTR failed");
    check(T, memcmp(comp + 4, mac, 6) == 0,
          "LIF_GETATTR MAC is not the one set");

    /* An opcode the device doesn't know is refused, not ignored */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 99;
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 2,
          "unknown devcmd status is not EOPCODE");

    /* The RDMA devcmds also arrive through the BAR0 window, and
     * CREATE_ADMINQ with no CREATE_CQ ahead of it is refused */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_RDMA_RESET;
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 0, "RDMA_RESET_LIF failed");
    cmd[0] = IH_CMD_RDMA_ADMINQ;
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 1,
          "CREATE_ADMINQ with no admin CQ was accepted");

    /* Accesses past the register window fail rather than touch memory */
    char b[8];
    check(T,
          ionic_eth_emu_bar0_access(h.dev->ionic_emu, b, 8, 0x8000 - 4, false) <
              0,
          "BAR0 read past the register window succeeded");
    check(T,
          ionic_eth_emu_bar2_access(h.dev->ionic_emu, b, 8, IONIC_BAR2_DB_SIZE,
                                    true) < 0,
          "BAR2 write past the doorbell window succeeded");
    check(T, ionic_eth_emu_bar2_access(h.dev->ionic_emu, b, 8, 0, false) == 8,
          "BAR2 read failed");

    teardown(T, &h);
}

static void test_bringup(void)
{
    const char *T = "bringup";
    struct ih h;
    uint8_t buf[64], cmd[64], comp[16];

    if (!setup(T, &h, "loopback"))
        return;

    /* PORT_INIT published the port as an up 100G copper link */
    fake_vfu_read(h.vfu, h.port_info, buf, 64);
    check(T, ernic_get_le32(buf + 0) == 100000, "port config speed is %u",
          ernic_get_le32(buf + 0));
    fake_vfu_read(h.vfu, h.port_info + 256, buf, 64);
    check(T, ernic_get_le32(buf + 4) == 100000 && buf[8] == 1,
          "port status is speed %u state %u", ernic_get_le32(buf + 4), buf[8]);
    check(T, buf[60] == 1 && buf[61] == 1,
          "no copper transceiver in the port status");

    /* LIF_INIT published the link as up */
    fake_vfu_read(h.vfu, h.lif_info + 256, buf, 16);
    check(T, buf[10] == 1 && ernic_get_le32(buf + 12) == 100000,
          "LIF status is link %u speed %u", buf[10], ernic_get_le32(buf + 12));

    /* Every Ethernet admin command raised the adminq's vector */
    unsigned irqs = fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR);
    check(T, irqs == 4, "%u adminq interrupts for 4 commands", irqs);

    /* The station MAC round-trips through the Ethernet admin queue */
    static const uint8_t mac[6] = {0x02, 0x66, 0x77, 0x88, 0x99, 0xaa};
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_LIF_SETATTR;
    cmd[1] = IH_LIF_ATTR_MAC;
    memcpy(cmd + 4, mac, 6);
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq LIF_SETATTR failed");
    cmd[0] = IH_CMD_LIF_GETATTR;
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq LIF_GETATTR failed");
    check(T, memcmp(comp + 4, mac, 6) == 0,
          "adminq LIF_GETATTR MAC is not the one set");

    /* Commands with no emulator state, such as RX_MODE_SET, succeed */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 31;
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq opcode 31 failed");

    /* An assertion on a masked vector is latched and replayed on unmask */
    irqs = fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR);
    ih_intr_mask(&h, IH_ETH_ADMINQ_INTR, true);
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq NOP failed");
    check(T, fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR) == irqs,
          "a masked vector fired");
    ih_intr_mask(&h, IH_ETH_ADMINQ_INTR, false);
    check(T, fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR) == irqs + 1,
          "unmasking did not replay the latched interrupt");

    /* With mask-on-assert the vector masks itself as it fires, and NAPI's
     * credit return with the unmask bit re-arms it */
    const uint32_t intr = 0x2000u + IH_ETH_ADMINQ_INTR * 32u;
    ih_bar0_write32(&h, intr + 12, 1);
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq NOP failed");
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq NOP failed");
    check(T, fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR) == irqs + 2,
          "mask-on-assert did not mask the vector");
    ih_bar0_write32(&h, intr + 8, 0x10000u);
    check(T, fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR) == irqs + 3,
          "the credit unmask did not replay the latched interrupt");
    ih_bar0_write32(&h, intr + 12, 0);

    teardown(T, &h);
}

/* ---- RDMA_RESET_LIF ---------------------------------------------------- */

/*
 * RDMA_RESET_LIF frees the admin queue context, and the Ethernet emulator
 * keeps a pointer to it for the AQ doorbell. That pointer must go with the
 * context: a doorbell before the guest creates a new admin queue would
 * otherwise read and write freed memory. The doorbell here comes in that
 * window, where the sanitizer and valgrind runs see any such access.
 */
static void test_rdma_reset(void)
{
    const char *T = "rdma-reset";
    struct ih h;
    struct ih_cq cq;
    uint8_t cmd[64], comp[16];

    if (!setup(T, &h, "loopback"))
        return;

    const struct ionic_eth_emu *emu = h.dev->ionic_emu;
    struct ionic_rdma_devcmd_state *rdma = h.dev->ionic_rdma;

    check(T,
          emu->adminq != NULL &&
              emu->adminq == ionic_rdma_devcmd_get_adminq_ctx(rdma),
          "bring-up left the emulator without the admin queue");

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_RDMA_RESET;
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "RDMA_RESET_LIF failed");
    check(T, ionic_rdma_devcmd_get_adminq_ctx(rdma) == NULL,
          "RDMA_RESET_LIF kept the admin queue");
    check(T, emu->adminq == NULL,
          "the emulator still points at the freed admin queue");

    /* A doorbell for the admin queue that is gone, then a loop pass */
    ih_doorbell(&h, IH_QTYPE_AQ, 0, 0, 1);
    ih_poll(&h);

    /* The driver's next bring-up makes a new admin queue, which the
     * emulator must use without the server's loop re-registering it */
    if (check(T, ih_bringup(&h), "bring-up after RDMA_RESET_LIF failed")) {
        check(T,
              emu->adminq != NULL &&
                  emu->adminq == ionic_rdma_devcmd_get_adminq_ctx(rdma),
              "the emulator doesn't have the new admin queue");
        if (check(T, ih_cq_create(&h, &cq, 1, 4),
                  "CREATE_CQ on the new admin queue failed"))
            check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
    }

    teardown(T, &h);
}

/* ---- RDMA admin commands that must be refused -------------------------- */

struct bad_admin {
    const char *what;
    uint8_t op;
    uint16_t len;
    void (*build)(uint8_t *body);
    int want; /* completion status */
};

static void build_none(uint8_t *body)
{
    (void)body;
}

static void build_qp_unknown_cq(uint8_t *body)
{
    ernic_put_le32(body + 8, 4000);
    ernic_put_le32(body + 32, 4001);
    ernic_put_le32(body + 40, 0xffffffffu);
    ernic_put_le32(body + 56, 3);
}

static void build_qp_unknown_srq(uint8_t *body)
{
    /* CQ 1 exists in this group */
    ernic_put_le32(body + 8, 1);
    ernic_put_le32(body + 32, 1);
    ernic_put_le32(body + 40, IONIC_SRQ_QID_BASE + 7);
    ernic_put_le32(body + 56, 3);
}

static void build_unknown_id(uint8_t *body)
{
    /* MR, QP and SRQ ids, all at offset 0 or in id_ver */
    ernic_put_le32(body + 0, 0x00777777u);
    ernic_put_le32(body + 28, 0x00777777u);
    ernic_put_le32(body + 56, 0x00777777u);
}

/*
 * Ring geometry the device must refuse. The exponents are guest bytes the
 * device shifts by, so 32 is undefined behavior if it gets through (UBSan
 * reports it); the others are just outside the bounds, which a build
 * without sanitizers sees as the command succeeding.
 */

/* CREATE_CQ for CQ 2, on the group's EQ */
static void build_cq(uint8_t *body, uint8_t depth_log2, uint8_t stride_log2)
{
    body[4] = depth_log2;
    body[5] = stride_log2;
    body[7] = 12;
    ernic_put_le32(body + 12, 2);
    ernic_put_le32(body + 20, 1);
}

static void build_cq_depth_32(uint8_t *body)
{
    build_cq(body, 32, 5);
}

static void build_cq_depth_25(uint8_t *body)
{
    build_cq(body, 25, 5);
}

static void build_cq_stride_4(uint8_t *body)
{
    build_cq(body, 4, 4);
}

static void build_cq_stride_17(uint8_t *body)
{
    build_cq(body, 4, 17);
}

/* CREATE_QP for QP 3 on CQ 1, with an RQ of its own */
static void build_qp(uint8_t *body, uint8_t sq_depth_log2,
                     uint8_t sq_stride_log2, uint8_t rq_depth_log2,
                     uint8_t rq_stride_log2)
{
    ernic_put_le32(body + 8, 1);
    body[12] = sq_depth_log2;
    body[13] = sq_stride_log2;
    ernic_put_le32(body + 32, 1);
    body[36] = rq_depth_log2;
    body[37] = rq_stride_log2;
    ernic_put_le32(body + 40, 0xffffffffu);
    ernic_put_le32(body + 56, 3);
}

static void build_qp_sq_depth_32(uint8_t *body)
{
    build_qp(body, 32, 7, 4, 7);
}

static void build_qp_sq_stride_32(uint8_t *body)
{
    build_qp(body, 4, 32, 4, 7);
}

static void build_qp_sq_stride_4(uint8_t *body)
{
    build_qp(body, 4, 4, 4, 7);
}

static void build_qp_sq_stride_10(uint8_t *body)
{
    build_qp(body, 4, 10, 4, 7);
}

static void build_qp_rq_stride_10(uint8_t *body)
{
    build_qp(body, 4, 7, 4, 10);
}

static void build_qp_rq_depth_32(uint8_t *body)
{
    build_qp(body, 4, 7, 32, 7);
}

static void build_qp_rq_stride_4(uint8_t *body)
{
    build_qp(body, 4, 7, 4, 4);
}

static void build_srq_too_deep(uint8_t *body)
{
    body[26] = 20;
    body[27] = 7;
}

static void build_srq_no_resp(uint8_t *body)
{
    body[26] = 4;
    body[27] = 7;
}

static const struct bad_admin bad_admins[] = {
    {"unknown opcode", 77, 0, build_none, 1},
    {"short CREATE_CQ", IH_ADMIN_CREATE_CQ, 10, build_none, 1},
    {"short CREATE_QP", IH_ADMIN_CREATE_QP, 60, build_none, 1},
    {"CREATE_QP on unknown CQs", IH_ADMIN_CREATE_QP, 64, build_qp_unknown_cq,
     1},
    {"CREATE_QP on an unknown SRQ", IH_ADMIN_CREATE_QP, 64,
     build_qp_unknown_srq, 1},
    {"CREATE_CQ 2^32 deep", IH_ADMIN_CREATE_CQ, 34, build_cq_depth_32, 1},
    {"CREATE_CQ deeper than 2^24", IH_ADMIN_CREATE_CQ, 34, build_cq_depth_25,
     1},
    {"CREATE_CQ with a stride below a CQE", IH_ADMIN_CREATE_CQ, 34,
     build_cq_stride_4, 1},
    {"CREATE_CQ with a stride above 2^16", IH_ADMIN_CREATE_CQ, 34,
     build_cq_stride_17, 1},
    {"CREATE_QP with an SQ 2^32 deep", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_depth_32, 1},
    {"CREATE_QP with an SQ stride of 2^32", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_stride_32, 1},
    {"CREATE_QP with an SQ stride below a WQE header", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_stride_4, 1},
    {"CREATE_QP with an SQ stride above the advertised 2^9", IH_ADMIN_CREATE_QP,
     64, build_qp_sq_stride_10, 1},
    {"CREATE_QP with an RQ stride above the advertised 2^9", IH_ADMIN_CREATE_QP,
     64, build_qp_rq_stride_10, 1},
    {"CREATE_QP with an RQ 2^32 deep", IH_ADMIN_CREATE_QP, 64,
     build_qp_rq_depth_32, 1},
    {"CREATE_QP with an RQ stride below a WQE header", IH_ADMIN_CREATE_QP, 64,
     build_qp_rq_stride_4, 1},
    {"DESTROY_MR of an unknown MR", IH_ADMIN_DESTROY_MR, 4, build_unknown_id,
     1},
    {"MODIFY_QP of an unknown QP", IH_ADMIN_MODIFY_QP, 60, build_unknown_id, 1},
    {"QUERY_QP of an unknown QP", IH_ADMIN_QUERY_QP, 34, build_unknown_id, 1},
    {"short CREATE_AH", IH_ADMIN_CREATE_AH, 8, build_none, 1},
    {"short DESTROY_AH", IH_ADMIN_DESTROY_AH, 2, build_none, 1},
    {"short CREATE_SRQ", IH_ADMIN_CREATE_SRQ, 20, build_none, 1},
    {"CREATE_SRQ deeper than advertised", IH_ADMIN_CREATE_SRQ, 42,
     build_srq_too_deep, 1},
    {"CREATE_SRQ with no response buffer", IH_ADMIN_CREATE_SRQ, 42,
     build_srq_no_resp, 1},
    {"short MODIFY_SRQ", IH_ADMIN_MODIFY_SRQ, 4, build_none, 1},
    {"MODIFY_SRQ of an unknown SRQ", IH_ADMIN_MODIFY_SRQ, 6, build_unknown_id,
     1},
    {"short QUERY_SRQ", IH_ADMIN_QUERY_SRQ, 2, build_none, 1},
    {"QUERY_SRQ of an unknown SRQ", IH_ADMIN_QUERY_SRQ, 4, build_unknown_id, 1},
    {"short DESTROY_SRQ", IH_ADMIN_DESTROY_SRQ, 2, build_none, 1},
    {"short STATS_HDRS", IH_ADMIN_STATS_HDRS, 0, build_none, IONIC_RC_EINVAL},
    /* Harmless, so not refused */
    {"DESTROY_SRQ of an unknown SRQ", IH_ADMIN_DESTROY_SRQ, 4, build_unknown_id,
     0},
    {"DESTROY_CQ of an unknown CQ", IH_ADMIN_DESTROY_CQ, 4, build_unknown_id,
     0},
    {"DESTROY_QP of an unknown QP", IH_ADMIN_DESTROY_QP, 4, build_unknown_id,
     0},
    {"NOOP", IH_ADMIN_NOOP, 0, build_none, 0},
    {"QUERY_AH", IH_ADMIN_QUERY_AH, 0, build_none, 0},
};

/* RDMA queue devcmds, sent over the Ethernet admin queue, whose geometry
 * must be refused */
struct bad_queue {
    const char *what;
    uint8_t op;
    uint8_t depth_log2, stride_log2;
};

static const struct bad_queue bad_queues[] = {
    {"CREATE_EQ 2^32 deep", IH_CMD_RDMA_EQ, 32, 2},
    {"CREATE_EQ deeper than 2^24", IH_CMD_RDMA_EQ, 25, 2},
    {"CREATE_EQ with a stride below an EQE", IH_CMD_RDMA_EQ, 4, 1},
    {"CREATE_EQ with a stride above 2^16", IH_CMD_RDMA_EQ, 4, 17},
    {"admin CREATE_CQ 2^32 deep", IH_CMD_RDMA_CQ, 32, 5},
    /* Sent after a good admin CREATE_CQ, so only its depth is wrong */
    {"CREATE_ADMINQ 2^32 deep", IH_CMD_RDMA_ADMINQ, 32, 6},
};

/* Queue id 5 is clear of the harness's own EQ and admin queues. */
static int rdma_queue_cmd(struct ih *h, uint8_t op, uint8_t depth_log2,
                          uint8_t stride_log2)
{
    uint8_t cmd[64] = {0}, comp[16];

    cmd[0] = op;
    ernic_put_le32(cmd + 4, 5u | 1u << 24);
    ernic_put_le32(cmd + 8, IH_EQ_ID);
    cmd[14] = depth_log2;
    cmd[15] = stride_log2;
    return ih_eth_admin(h, cmd, comp);
}

static void test_queue_geometry_errors(const char *T, struct ih *h)
{
    for (size_t i = 0; i < sizeof(bad_queues) / sizeof(bad_queues[0]); i++) {
        const struct bad_queue *c = &bad_queues[i];

        if (c->op == IH_CMD_RDMA_ADMINQ)
            check(T, rdma_queue_cmd(h, IH_CMD_RDMA_CQ, 6, 5) == 0,
                  "admin CREATE_CQ failed");
        int rc = rdma_queue_cmd(h, c->op, c->depth_log2, c->stride_log2);
        check(T, rc == 1, "%s: status %d, expected 1", c->what, rc);
    }

    /* A refused CREATE_EQ takes none of the device's 32 EQ slots */
    for (int i = 0; i < 40; i++)
        (void)rdma_queue_cmd(h, IH_CMD_RDMA_EQ, 32, 2);
    check(T, rdma_queue_cmd(h, IH_CMD_RDMA_EQ, 4, 2) == 0,
          "CREATE_EQ failed after refused ones");
}

static void test_admin_errors(void)
{
    const char *T = "admin";
    struct ih h;
    struct ih_cq cq;

    if (!setup(T, &h, "loopback"))
        return;
    if (!check(T, ih_cq_create(&h, &cq, 1, 4), "CREATE_CQ failed")) {
        teardown(T, &h);
        return;
    }

    for (size_t i = 0; i < sizeof(bad_admins) / sizeof(bad_admins[0]); i++) {
        const struct bad_admin *c = &bad_admins[i];
        uint8_t body[128] = {0};

        c->build(body);
        int rc = ih_admin(&h, c->op, body, c->len, NULL);
        check(T, rc == c->want, "%s: status %d, expected %d", c->what, rc,
              c->want);
    }

    test_queue_geometry_errors(T, &h);

    /* A WQE the device can't read stays queued and runs on the next poll,
     * rather than being skipped */
    uint8_t body[4] = {0};
    ernic_put_le32(body, 9999);
    int slot = ih_admin_post(&h, IH_ADMIN_DESTROY_CQ, body, 4);
    fake_vfu_fail_nth_map(h.vfu, 1);
    ih_poll(&h);
    check(T,
          ih_admin_complete(&h, IH_ADMIN_DESTROY_CQ, slot, NULL) ==
              IH_ADMIN_PENDING,
          "an admin WQE the device could not read completed");
    ih_poll(&h);
    check(T, ih_admin_complete(&h, IH_ADMIN_DESTROY_CQ, slot, NULL) == 0,
          "an admin WQE that hit a failed DMA was not retried");

    /* A CREATE_QP posted at the last slot, so its second stride wraps to
     * slot 0, runs, and the command after it completes from slot 1 */
    uint32_t aq_depth = 1u << h.aq.depth_log2;
    while (h.aq.prod % aq_depth != aq_depth - 1) {
        if (!check(T, ih_admin(&h, IH_ADMIN_NOOP, NULL, 0, NULL) == 0,
                   "NOOP failed"))
            break;
    }
    struct ih_qp qp;
    if (check(T, ih_qp_create(&h, &qp, 20, IH_QPT_UD, &cq, &cq, NULL),
              "CREATE_QP wrapping the admin queue failed")) {
        check(T, h.aq.prod % aq_depth == 1, "CREATE_QP took %u strides",
              h.aq.prod % aq_depth + 1);
        check(T, ih_qp_destroy(&h, &qp) == 0, "DESTROY_QP failed");
    }

    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Connected QPs on the loopback backend ----------------------------- */

#define RC_CQ_A 1
#define RC_CQ_B 2
#define RC_QP_A 10
#define RC_QP_B 11
#define MR_A    0x01000001u
#define MR_B    0x02000002u
#define VA_A    UINT64_C(0x00007f0000000100)
#define VA_B    UINT64_C(0x00007f1000000000)
#define MR_LEN  (3 * IH_PAGE_SIZE)

struct rc {
    struct ih h;
    struct ih_cq cqa, cqb;
    struct ih_qp a, b;
    struct ih_mr mra, mrb;
    uint32_t msn_a; /* the MSN A's next send completion carries */
};

static bool rc_setup(const char *test, struct rc *r)
{
    memset(r, 0, sizeof(*r));
    if (!setup(test, &r->h, "loopback"))
        return false;

    bool ok =
        ih_cq_create(&r->h, &r->cqa, RC_CQ_A, 6) &&
        ih_cq_create(&r->h, &r->cqb, RC_CQ_B, 6) &&
        ih_qp_create(&r->h, &r->a, RC_QP_A, IH_QPT_RC, &r->cqa, &r->cqa,
                     NULL) &&
        ih_qp_create(&r->h, &r->b, RC_QP_B, IH_QPT_RC, &r->cqb, &r->cqb,
                     NULL) &&
        ih_qp_connect(&r->h, &r->a, &r->b) &&
        ih_mr_create(&r->h, &r->mra, MR_A, VA_A, MR_LEN, IH_ACCESS_ALL, true) &&
        ih_mr_create(&r->h, &r->mrb, MR_B, VA_B, MR_LEN, IH_ACCESS_ALL, false);
    if (!check(test, ok, "RC setup failed")) {
        ih_mr_free(&r->mra);
        ih_mr_free(&r->mrb);
        teardown(test, &r->h);
        return false;
    }
    r->msn_a = 1;
    return true;
}

static void rc_teardown(const char *test, struct rc *r)
{
    check(test, ih_qp_destroy(&r->h, &r->a) == 0, "DESTROY_QP A failed");
    check(test, ih_qp_destroy(&r->h, &r->b) == 0, "DESTROY_QP B failed");
    check(test, ih_mr_destroy(&r->h, &r->mra) == 0, "DESTROY_MR A failed");
    check(test, ih_mr_destroy(&r->h, &r->mrb) == 0, "DESTROY_MR B failed");
    check(test, ih_cq_destroy(&r->h, &r->cqa) == 0, "DESTROY_CQ A failed");
    check(test, ih_cq_destroy(&r->h, &r->cqb) == 0, "DESTROY_CQ B failed");
    teardown(test, &r->h);
}

/* Split [va, va + len) into @n SGEs of near-equal size. */
static uint8_t split_sges(struct ih_sge *sge, uint8_t n, uint64_t va,
                          uint32_t len, uint32_t lkey)
{
    uint32_t done = 0;

    for (uint8_t i = 0; i < n; i++) {
        uint32_t part = i + 1 == n ? len - done : len / n;

        sge[i] = (struct ih_sge){.va = va + done, .len = part, .lkey = lkey};
        done += part;
    }
    return n;
}

/* A's send completion, with the status it should carry */
static void expect_send(const char *test, const char *what, struct rc *r,
                        uint32_t status)
{
    struct ih_cqe e;

    if (!check(test, ih_cq_poll(&r->h, &r->cqa, &e), "%s: no send completion",
               what))
        return;
    check(test, e.type == IH_CQE_TYPE_SEND_MSN && e.qid == RC_QP_A,
          "%s: send completion type %u qid %u", what, e.type, e.qid);
    check(test, e.msn == r->msn_a, "%s: MSN %u, expected %u", what, e.msn,
          r->msn_a);
    check(test,
          e.error == (status != 0) && (!e.error || e.status_len == status),
          "%s: send status %u (error %d), expected %u", what, e.status_len,
          e.error, status);
    r->msn_a++;
}

struct rc_case {
    const char *what;
    uint8_t op;
    uint32_t len;
    uint8_t nsrc;      /* SGEs on the sending side */
    uint8_t ndst;      /* SGEs in the receive */
    uint32_t recv_cap; /* bytes the receive can take; 0 means @len */
    bool inline_data;
    bool bad_rkey;
    uint32_t send_status;
    uint32_t recv_status; /* 0, or the error the receive completes with */
};

static const struct rc_case rc_cases[] = {
    {"SEND", IH_OP_SEND, 100, 1, 1, 0, false, false, 0, 0},
    {"SEND_IMM", IH_OP_SEND_IMM, 64, 1, 1, 0, false, false, 0, 0},
    {"SEND_INV", IH_OP_SEND_INV, 32, 1, 1, 0, false, false, 0, 0},
    {"SEND across pages", IH_OP_SEND, 6000, 3, 2, 0, false, false, 0, 0},
    {"SEND of 0 bytes", IH_OP_SEND, 0, 0, 1, 16, false, false, 0, 0},
    {"inline SEND", IH_OP_SEND, 90, 0, 1, 0, true, false, 0, 0},
    {"SEND too big for the receive", IH_OP_SEND, 200, 1, 1, 100, false, false,
     0, IH_STS_LOCAL_LEN_ERR},
    {"RDMA_WRITE", IH_OP_RDMA_WRITE, 5000, 2, 0, 0, false, false, 0, 0},
    {"RDMA_WRITE_IMM", IH_OP_RDMA_WRITE_IMM, 300, 1, 0, 0, false, false, 0, 0},
    {"RDMA_READ", IH_OP_RDMA_READ, 4500, 3, 0, 0, false, false, 0, 0},
    {"RDMA_WRITE with a bad rkey", IH_OP_RDMA_WRITE, 64, 1, 0, 0, false, true,
     IH_STS_REMOTE_ACC_ERR, 0},
    {"RDMA_READ with a bad rkey", IH_OP_RDMA_READ, 64, 1, 0, 0, false, true,
     IH_STS_REMOTE_ACC_ERR, 0},
    {"unsupported opcode", 12, 16, 1, 0, 0, false, false, IH_STS_LOCAL_QP_OPER,
     0},
};

/* Where in each MR a row's data goes: off page boundaries on purpose */
#define SRC_OFF 0x0f0u
#define DST_OFF 0x123u

static void run_rc_case(const char *test, struct rc *r, const struct rc_case *c,
                        uint8_t seed)
{
    struct ih *h = &r->h;
    uint8_t want[8192], got[8192];
    struct ih_sge src[6], dst[6];
    const uint32_t imm_be = htobe32(0xc0ffee00u + seed);
    bool is_send = c->op == IH_OP_SEND || c->op == IH_OP_SEND_IMM ||
                   c->op == IH_OP_SEND_INV;
    bool is_read = c->op == IH_OP_RDMA_READ;
    bool takes_recv = is_send || c->op == IH_OP_RDMA_WRITE_IMM;
    uint32_t cap = c->recv_cap ? c->recv_cap : c->len;

    /* The data moves from @from to @to; a READ moves it from B to A. */
    const struct ih_mr *from = is_read ? &r->mrb : &r->mra;
    const struct ih_mr *to = is_read ? &r->mra : &r->mrb;
    uint64_t from_va = from->va + (is_read ? DST_OFF : SRC_OFF);
    uint64_t to_va = to->va + (is_read ? SRC_OFF : DST_OFF);

    fill(want, c->len, seed);
    memset(got, 0, sizeof(got));
    ih_mr_write(h, from, from_va, want, c->len);
    ih_mr_write(h, to, to_va, got, cap > c->len ? cap : c->len);

    uint64_t recv_id = 0;
    if (takes_recv)
        recv_id = ih_post_recv(h, &r->b, dst,
                               split_sges(dst, c->ndst, to_va, cap, MR_B));

    struct ih_send s = {
        .op = c->op,
        .flags = IH_WQE_SIGNALLED,
        .imm_be = imm_be,
        .key = 0x0a0b0c0du,
        .remote_va = is_read ? from_va : to_va,
        .rkey = c->bad_rkey ? 0x7e000099u : MR_B,
        .length = c->len,
        .sge = src,
    };
    s.nsge = split_sges(src, c->nsrc, is_read ? to_va : from_va, c->len, MR_A);
    if (c->inline_data) {
        s.flags |= IH_WQE_INLINE;
        s.inline_data = want;
        s.inline_len = c->len;
    }
    ih_post_send(h, &r->a, &s);

    expect_send(test, c->what, r, c->send_status);

    struct ih_cqe e;
    if (takes_recv) {
        if (check(test, ih_cq_poll(h, &r->cqb, &e), "%s: no receive completion",
                  c->what)) {
            uint8_t op = c->op == IH_OP_SEND_IMM         ? IH_RECV_OP_SEND_IMM
                         : c->op == IH_OP_SEND_INV       ? IH_RECV_OP_SEND_INV
                         : c->op == IH_OP_RDMA_WRITE_IMM ? IH_RECV_OP_RDMA_IMM
                                                         : IH_RECV_OP_SEND;
            check(test, e.type == IH_CQE_TYPE_RECV && e.qid == RC_QP_B,
                  "%s: receive completion type %u qid %u", c->what, e.type,
                  e.qid);
            check(test, e.wqe_id == recv_id,
                  "%s: receive wqe_id %" PRIu64 ", not %" PRIu64, c->what,
                  e.wqe_id, recv_id);
            check(test, e.recv_op == op && e.src_qpn == RC_QP_A,
                  "%s: receive op %u from QP %u", c->what, e.recv_op,
                  e.src_qpn);
            check(test, !e.has_smac, "%s: RC receive carries a source MAC",
                  c->what);
            if (c->recv_status)
                check(test, e.error && e.status_len == c->recv_status,
                      "%s: receive status %u (error %d), expected %u", c->what,
                      e.status_len, e.error, c->recv_status);
            else
                check(test, !e.error && e.status_len == c->len,
                      "%s: receive length %u (error %d), expected %u", c->what,
                      e.status_len, e.error, c->len);
            if (op == IH_RECV_OP_SEND_IMM || op == IH_RECV_OP_RDMA_IMM)
                check(test, e.imm_be == imm_be, "%s: immediate data lost",
                      c->what);
        }
    }
    check(test, cq_empty(h, &r->cqb), "%s: an extra completion on B", c->what);
    check(test, fake_vfu_mapped(h->vfu) == 0,
          "%s: guest memory left mapped after the transfer", c->what);

    if (c->send_status || c->recv_status)
        return;
    ih_mr_read(h, to, to_va, got, c->len);
    check(test, memcmp(got, want, c->len) == 0, "%s: data did not arrive",
          c->what);
}

/*
 * A SEND from an SQ with the largest stride the device advertises, 2^9,
 * gathering from as many SGEs as that holds, 30, one byte from each. It
 * arrives whole only if the device reads the whole WQE. It used to read 256
 * bytes of it and then walk SGEs as far as the stride went, past the end of
 * the buffer it had read into.
 */
#define WIDE_CQ_C 3
#define WIDE_CQ_D 4
#define WIDE_QP_C 12
#define WIDE_QP_D 13
#define WIDE_NSGE (((1u << IH_WQE_MAX_STRIDE_LOG2) - 32u) / 16u)

static void widest_wqe_send(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    struct ih_cq cqc, cqd;
    struct ih_qp c, d;
    struct ih_cqe e;

    memset(&e, 0, sizeof(e));
    bool ok = ih_cq_create(h, &cqc, WIDE_CQ_C, 6) &&
              ih_cq_create(h, &cqd, WIDE_CQ_D, 6) &&
              ih_qp_create_stride(h, &c, WIDE_QP_C, IH_QPT_RC, &cqc, &cqc, NULL,
                                  IH_WQE_MAX_STRIDE_LOG2) &&
              ih_qp_create(h, &d, WIDE_QP_D, IH_QPT_RC, &cqd, &cqd, NULL) &&
              ih_qp_connect(h, &c, &d);
    if (!check(T, ok, "widest-WQE setup failed"))
        return;

    uint8_t want[WIDE_NSGE], got[WIDE_NSGE];
    struct ih_sge sge[WIDE_NSGE];
    fill(want, sizeof(want), 0x77);
    memset(got, 0, sizeof(got));
    ih_mr_write(h, &r->mra, VA_A, want, sizeof(want));
    ih_mr_write(h, &r->mrb, VA_B, got, sizeof(got));
    for (uint32_t i = 0; i < WIDE_NSGE; i++)
        sge[i] = (struct ih_sge){.va = VA_A + i, .len = 1, .lkey = MR_A};

    struct ih_sge into = {.va = VA_B, .len = sizeof(got), .lkey = MR_B};
    ih_post_recv(h, &d, &into, 1);
    struct ih_send s = {.op = IH_OP_SEND, .sge = sge, .nsge = WIDE_NSGE};
    ih_post_send(h, &c, &s);

    check(T, ih_cq_poll(h, &cqc, &e) && !e.error,
          "a SEND of %u SGEs did not complete", WIDE_NSGE);
    check(T, ih_cq_poll(h, &cqd, &e) && !e.error && e.status_len == WIDE_NSGE,
          "a SEND of %u SGEs was received as %u bytes", WIDE_NSGE,
          e.status_len);
    ih_mr_read(h, &r->mrb, VA_B, got, sizeof(got));
    check(T, memcmp(got, want, sizeof(got)) == 0,
          "a SEND of %u SGEs did not arrive whole", WIDE_NSGE);

    check(T, ih_qp_destroy(h, &c) == 0, "DESTROY_QP C failed");
    check(T, ih_qp_destroy(h, &d) == 0, "DESTROY_QP D failed");
    check(T, ih_cq_destroy(h, &cqc) == 0, "DESTROY_CQ C failed");
    check(T, ih_cq_destroy(h, &cqd) == 0, "DESTROY_CQ D failed");
}

static void test_rc(void)
{
    const char *T = "rc";
    struct rc r;
    struct ih *h = &r.h;

    if (!rc_setup(T, &r))
        return;

    /* QUERY_QP writes the state into the side buffers the driver reads */
    uint8_t body[34] = {0};
    const uint64_t sq_buf = h->scratch + 1024, rq_buf = h->scratch + 1088;
    ernic_put_le64(body + 8, sq_buf);
    ernic_put_le64(body + 16, rq_buf);
    ernic_put_le32(body + 28, RC_QP_A);
    check(T, ih_admin(h, IH_ADMIN_QUERY_QP, body, sizeof(body), NULL) == 0,
          "QUERY_QP failed");
    uint8_t rqb[12];
    fake_vfu_read(h->vfu, rq_buf, rqb, sizeof(rqb));
    check(T, rqb[0] >> 4 == IH_QPS_RTS, "QUERY_QP state %u, not RTS",
          (unsigned)(rqb[0] >> 4));

    for (size_t i = 0; i < sizeof(rc_cases) / sizeof(rc_cases[0]); i++)
        run_rc_case(T, &r, &rc_cases[i], (uint8_t)(i * 17 + 3));

    /* Unsignalled work still completes: the driver retires its SQ by it */
    uint8_t one = 0x5a;
    struct ih_sge s1 = {.va = VA_A, .len = 1, .lkey = MR_A};
    ih_mr_write(h, &r.mra, VA_A, &one, 1);
    struct ih_send w = {.op = IH_OP_RDMA_WRITE,
                        .remote_va = VA_B,
                        .rkey = MR_B,
                        .sge = &s1,
                        .nsge = 1};
    ih_post_send(h, &r.a, &w);
    expect_send(T, "unsignalled RDMA_WRITE", &r, 0);

    /*
     * A connected SEND with nothing posted waits for a receive, as a
     * requester behind an RNR NAK retries, rather than being dropped and
     * completed.  Neither side completes while it waits, and the send queue
     * stays on it: posting the receive releases it on the next poll.  The
     * drop only happens once RNR_RETRY_MS has passed, which ernic-rdma-errors
     * and test_ionic_dp_errors cover; this checks the wait itself.
     */
    struct ih_send s = {.op = IH_OP_SEND, .sge = &s1, .nsge = 1};
    ih_post_send(h, &r.a, &s);
    check(T, cq_empty(h, &r.cqa),
          "a SEND with no receive completed the sender");
    check(T, cq_empty(h, &r.cqb), "a SEND with no receive completed one");

    struct ih_sge held = {.va = VA_B, .len = 1, .lkey = MR_B};
    ih_post_recv(h, &r.b, &held, 1);
    ih_poll(h);
    expect_send(T, "SEND released by a late receive", &r, 0);
    struct ih_cqe held_e;
    check(T, ih_cq_poll(h, &r.cqb, &held_e) && !held_e.error,
          "a SEND released by a late receive did not deliver");

    /* Bus addresses (lkey 0) across a guest DMA region boundary take more
     * than one scatter/gather entry on both sides */
    uint8_t want[3000], got[3000];
    uint64_t sbus = fake_vfu_alloc_straddling(h->vfu, sizeof(want), 1000);
    uint64_t dbus = fake_vfu_alloc_straddling(h->vfu, sizeof(got), 2000);
    fill(want, sizeof(want), 0x42);
    fake_vfu_write(h->vfu, sbus, want, sizeof(want));
    struct ih_sge ds = {.va = dbus, .len = sizeof(got), .lkey = 0};
    struct ih_sge ss = {.va = sbus, .len = sizeof(want), .lkey = 0};
    ih_post_recv(h, &r.b, &ds, 1);
    s = (struct ih_send){.op = IH_OP_SEND, .sge = &ss, .nsge = 1};
    ih_post_send(h, &r.a, &s);
    expect_send(T, "SEND across a DMA region", &r, 0);
    struct ih_cqe e;
    check(T, ih_cq_poll(h, &r.cqb, &e) && !e.error, "no receive completion");
    fake_vfu_read(h->vfu, dbus, got, sizeof(got));
    check(T, memcmp(got, want, sizeof(got)) == 0,
          "data across a DMA region did not arrive");

    /* Wrap the send queue and both CQs a few times over */
    for (unsigned i = 0; i < 70; i++) {
        uint8_t v = (uint8_t)i, back = 0;
        ih_mr_write(h, &r.mra, VA_A + 8, &v, 1);
        struct ih_sge one_byte = {.va = VA_A + 8, .len = 1, .lkey = MR_A};
        struct ih_sge into = {.va = VA_B + 8, .len = 1, .lkey = MR_B};
        ih_post_recv(h, &r.b, &into, 1);
        s = (struct ih_send){.op = IH_OP_SEND, .sge = &one_byte, .nsge = 1};
        ih_post_send(h, &r.a, &s);
        expect_send(T, "SEND while wrapping", &r, 0);
        if (!check(T, ih_cq_poll(h, &r.cqb, &e) && !e.error,
                   "receive %u did not complete", i))
            break;
        ih_mr_read(h, &r.mrb, VA_B + 8, &back, 1);
        if (!check(T, back == v, "receive %u carried %u", i, back))
            break;
    }

    widest_wqe_send(T, &r);
    rc_teardown(T, &r);
}

/* ---- Atomics and fast registration ------------------------------------- */

static void atomic(const char *test, struct rc *r, uint8_t op, uint64_t add,
                   uint64_t compare, uint32_t rkey, uint64_t remote_va,
                   uint32_t status, uint64_t want_old, uint64_t want_new)
{
    struct ih *h = &r->h;
    const uint64_t target = VA_B + 0x800, result = VA_A + 0x900;
    uint64_t v = 0;

    ih_mr_write(h, &r->mra, result, &v, 8);
    struct ih_sge sge = {.va = result, .len = 8, .lkey = MR_A};
    struct ih_send s = {.op = op,
                        .flags = IH_WQE_SIGNALLED,
                        .remote_va = remote_va ? remote_va : target,
                        .rkey = rkey,
                        .swap_add = add,
                        .compare = compare,
                        .sge = &sge,
                        .nsge = 1};
    ih_post_send(h, &r->a, &s);
    expect_send(test, op == IH_OP_ATOMIC_CS ? "ATOMIC_CS" : "ATOMIC_FA", r,
                status);
    if (status)
        return;

    ih_mr_read(h, &r->mra, result, &v, 8);
    check(test, v == want_old, "atomic returned %" PRIu64 ", expected %" PRIu64,
          v, want_old);
    ih_mr_read(h, &r->mrb, target, &v, 8);
    check(test, v == want_new, "atomic left %" PRIu64 ", expected %" PRIu64, v,
          want_new);
}

static void test_atomic_and_reg_mr(void)
{
    const char *T = "atomic";
    struct rc r;
    struct ih *h = &r.h;

    if (!rc_setup(T, &r))
        return;

    uint64_t v = 100;
    ih_mr_write(h, &r.mrb, VA_B + 0x800, &v, 8);
    atomic(T, &r, IH_OP_ATOMIC_FA, 5, 0, MR_B, 0, 0, 100, 105);
    atomic(T, &r, IH_OP_ATOMIC_CS, 7, 105, MR_B, 0, 0, 105, 7);
    atomic(T, &r, IH_OP_ATOMIC_CS, 9, 999, MR_B, 0, 0, 7, 7);
    atomic(T, &r, IH_OP_ATOMIC_FA, 1, 0, 0x7e000099u, 0, IH_STS_REMOTE_ACC_ERR,
           0, 0);
    atomic(T, &r, IH_OP_ATOMIC_FA, 1, 0, MR_B, VA_B + MR_LEN - 4,
           IH_STS_REMOTE_ACC_ERR, 0, 0);

    /*
     * Fast registration, as nvme-rdma does ahead of every command:
     * CREATE_MR leaves the MR unbound, a REG_MR work request binds it under
     * a rotated key, and LOCAL_INV unbinds it again.
     */
    struct ih_mr fr;
    const uint32_t fr_id = 0x00000003u, fr_key = 0x04000000u | fr_id;
    uint8_t body[45] = {0};
    ernic_put_le32(body + 20, 0x03000000u | fr_id);
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    check(T, ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL) == 0,
          "CREATE_MR of an unbound MR failed");
    if (!check(T,
               ih_mr_alloc(h, &fr, fr_key, UINT64_C(0x7f3000000200),
                           2 * IH_PAGE_SIZE, true),
               "MR pages not allocated")) {
        rc_teardown(T, &r);
        return;
    }

    struct ih_send reg = {.op = IH_OP_REG_MR, .key = fr_key, .mr = &fr};
    uint64_t id = ih_post_send(h, &r.a, &reg);
    struct ih_cqe e;
    check(T,
          ih_cq_poll(h, &r.cqa, &e) && e.type == IH_CQE_TYPE_SEND_NPG &&
              e.wqe_id == id,
          "REG_MR did not complete by its SQ index");

    uint8_t want[256], got[256];
    fill(want, sizeof(want), 9);
    ih_mr_write(h, &r.mra, VA_A, want, sizeof(want));
    struct ih_sge sge = {.va = VA_A, .len = sizeof(want), .lkey = MR_A};
    struct ih_send w = {.op = IH_OP_RDMA_WRITE,
                        .flags = IH_WQE_SIGNALLED,
                        .remote_va = fr.va + 4000,
                        .rkey = fr_key,
                        .sge = &sge,
                        .nsge = 1};
    ih_post_send(h, &r.a, &w);
    expect_send(T, "RDMA_WRITE into a fast-registered MR", &r, 0);
    ih_mr_read(h, &fr, fr.va + 4000, got, sizeof(got));
    check(T, memcmp(want, got, sizeof(got)) == 0,
          "RDMA_WRITE through the new key did not land");

    struct ih_send inv = {.op = IH_OP_LOCAL_INV, .key = fr_key};
    id = ih_post_send(h, &r.a, &inv);
    check(T,
          ih_cq_poll(h, &r.cqa, &e) && e.type == IH_CQE_TYPE_SEND_NPG &&
              e.wqe_id == id,
          "LOCAL_INV did not complete by its SQ index");
    ih_post_send(h, &r.a, &w);
    expect_send(T, "RDMA_WRITE through an invalidated key", &r,
                IH_STS_REMOTE_ACC_ERR);

    ih_mr_free(&fr);
    ernic_put_le32(body, 0x03000000u | fr_id);
    check(T, ih_admin(h, IH_ADMIN_DESTROY_MR, body, 4, NULL) == 0,
          "DESTROY_MR of the fast-registered MR failed");

    rc_teardown(T, &r);
}

/* ---- Flush when a QP enters the Error state ----------------------------- */

/*
 * A work request outstanding when its QP goes to Error has to be completed,
 * or whoever posted it waits for it forever. rping is the case that found
 * this: it keeps a receive posted for the life of the connection, so after
 * rdma_disconnect its completion thread sat in ibv_get_cq_event() and the
 * process never exited.
 *
 * The device signals this with one completion per queue rather than one per
 * work request -- it carries no wqe_id, and the driver matches it to what it
 * has posted -- so what is checked here is that both queues raise one, that
 * it is marked an error with the flushed status, and that it raises the CQ
 * event. The event is the half that unwedges a blocked consumer.
 */
static void test_qp_flush(void)
{
    const char *T = "qp-flush";
    struct rc r;
    struct ih *h = &r.h;
    struct ih_cqe e;

    if (!rc_setup(T, &r))
        return;

    /* The receive a consumer is still waiting on when the peer goes away. */
    struct ih_sge dst = {.va = VA_B, .len = 64, .lkey = MR_B};
    ih_post_recv(h, &r.b, &dst, 1);
    check(T, cq_empty(h, &r.cqb), "a posted receive completed on its own");

    /* Armed, so the event the flush raises is visible. */
    ih_cq_arm(h, &r.cqb);
    check(T, eq_count(h, RC_CQ_B) == 0, "arming an empty CQ fired");

    /* Only Error flushes. A move to RTS must leave the receive posted. */
    check(T, ih_qp_modify(h, RC_QP_B, IH_QP_STATE, IH_QPS_RTS, 0, NULL, 0) == 0,
          "MODIFY_QP to RTS failed");
    check(T, cq_empty(h, &r.cqb),
          "a state change other than Error raised a completion");

    /*
     * The state byte only means anything under IB_QP_STATE. A command that
     * changes something else leaves whatever the driver last put there, so a
     * flush keyed on the byte alone would tear down a live QP.
     */
    check(T,
          ih_qp_modify(h, RC_QP_B, IH_QP_QKEY, IH_QPS_ERR, 0x1234, NULL, 0) ==
              0,
          "MODIFY_QP of the qkey failed");
    check(T, cq_empty(h, &r.cqb),
          "a MODIFY_QP without IB_QP_STATE flushed the queue pair");

    check(T, ih_qp_modify(h, RC_QP_B, IH_QP_STATE, IH_QPS_ERR, 0, NULL, 0) == 0,
          "MODIFY_QP to Error failed");

    unsigned n = 0;
    bool saw_recv = false, saw_send = false;

    while (ih_cq_poll(h, &r.cqb, &e)) {
        n++;
        check(T, e.error, "a flush completion is not marked an error");
        check(T, e.status_len == IH_STS_WQE_FLUSHED_ERR,
              "flush status %u, want %u", e.status_len, IH_STS_WQE_FLUSHED_ERR);
        saw_recv |= e.type == IH_CQE_TYPE_RECV;
        saw_send |= e.type == IH_CQE_TYPE_SEND_NPG;
    }
    check(T, n == 2, "Error raised %u completions, want one per queue", n);
    check(T, saw_recv, "the receive queue was not flushed");
    check(T, saw_send, "the send queue was not flushed");
    check(T, eq_count(h, RC_CQ_B) >= 1,
          "the flush raised no event: a consumer blocked in "
          "ibv_get_cq_event() would never wake");

    rc_teardown(T, &r);
}

/*
 * The receives of a QP bound to a shared receive queue are not its own, so
 * Error must not flush them: the SRQ is still live and other QPs are still
 * drawing from it. Only the send queue flushes.
 */
static void test_qp_flush_srq(void)
{
    const char *T = "qp-flush-srq";
    struct ih h;
    struct ih_cq cqf;
    struct ih_qp f;
    struct ih_srq srq;
    struct ih_cqe e;

    if (!setup(T, &h, "loopback"))
        return;

    if (!check(T, ih_srq_create(&h, &srq, 4) == 0, "CREATE_SRQ failed")) {
        teardown(T, &h);
        return;
    }
    if (!check(T,
               ih_cq_create(&h, &cqf, 5, 5) &&
                   ih_qp_create(&h, &f, 30, IH_QPT_RC, &cqf, &cqf, &srq),
               "SRQ-bound QP setup failed")) {
        teardown(T, &h);
        return;
    }

    check(T, ih_qp_modify(&h, 30, IH_QP_STATE, IH_QPS_ERR, 0, NULL, 0) == 0,
          "MODIFY_QP to Error failed");

    unsigned n = 0;
    bool saw_recv = false;

    while (ih_cq_poll(&h, &cqf, &e)) {
        n++;
        saw_recv |= e.type == IH_CQE_TYPE_RECV;
    }
    check(T, !saw_recv,
          "an SRQ-bound QP flushed receives that belong to its SRQ");
    check(T, n == 1, "Error raised %u completions, want only the send one", n);

    check(T, ih_qp_destroy(&h, &f) == 0, "DESTROY_QP failed");
    check(T, ih_srq_destroy(&h, &srq) == 0, "DESTROY_SRQ failed");
    check(T, ih_cq_destroy(&h, &cqf) == 0, "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- CQ arming and events ---------------------------------------------- */

static void test_cq_events(void)
{
    const char *T = "cq-events";
    struct rc r;
    struct ih *h = &r.h;
    struct ih_cqe e;

    if (!rc_setup(T, &r))
        return;

    uint8_t byte = 1;
    ih_mr_write(h, &r.mra, VA_A, &byte, 1);
    struct ih_sge src = {.va = VA_A, .len = 1, .lkey = MR_A};
    struct ih_sge dst = {.va = VA_B, .len = 1, .lkey = MR_B};
    struct ih_send s = {.op = IH_OP_SEND, .sge = &src, .nsge = 1};
    unsigned irqs = fake_vfu_irq_count(h->vfu, IH_EQ_INTR);

    /* Unarmed: a completion raises no event */
    ih_post_recv(h, &r.b, &dst, 1);
    ih_post_send(h, &r.a, &s);
    check(T, eq_count(h, RC_CQ_B) == 0, "an unarmed CQ raised an event");

    /* Arming a CQ that still holds a completion fires at once */
    ih_cq_arm(h, &r.cqb);
    check(T, eq_count(h, RC_CQ_B) == 1, "arming over a waiting CQE was silent");
    check(T, fake_vfu_irq_count(h->vfu, IH_EQ_INTR) > irqs,
          "the event raised no interrupt");
    ih_cq_poll(h, &r.cqb, &e);
    ih_cq_poll(h, &r.cqa, &e);

    /* Armed and empty: the next completion fires once, then disarms */
    ih_cq_arm(h, &r.cqb);
    check(T, eq_count(h, RC_CQ_B) == 0, "arming an empty CQ fired");
    ih_post_recv(h, &r.b, &dst, 1);
    ih_post_send(h, &r.a, &s);
    check(T, eq_count(h, RC_CQ_B) == 1, "an armed CQ raised no event");
    ih_cq_poll(h, &r.cqb, &e);
    ih_cq_poll(h, &r.cqa, &e);
    ih_post_recv(h, &r.b, &dst, 1);
    ih_post_send(h, &r.a, &s);
    check(T, eq_count(h, RC_CQ_B) == 0, "a fired CQ stayed armed");
    ih_cq_poll(h, &r.cqb, &e);
    ih_cq_poll(h, &r.cqa, &e);

    /* Doorbells for queues that don't exist are ignored */
    ih_doorbell(h, IH_QTYPE_CQ, 4000, 1, 0);
    ih_doorbell(h, IH_QTYPE_SQ, 4000, 0, 1);
    ih_doorbell(h, IH_QTYPE_RQ, 4000, 0, 1);
    ih_doorbell(h, IH_QTYPE_RQ, IONIC_SRQ_QID_BASE + 3, 0, 1);
    ih_doorbell(h, 4, 0, 0, 1);
    ih_doorbell(h, 12, 0, 0, 1);
    check(T, cq_empty(h, &r.cqa) && cq_empty(h, &r.cqb),
          "a stray doorbell produced a completion");

    /* The statistics file reflects the traffic */
    char dir[] = "/tmp/ernic-harness-XXXXXX";
    if (check(T, mkdtemp(dir) != NULL, "mkdtemp: %s", strerror(errno))) {
        char path[sizeof(dir) + 16];
        snprintf(path, sizeof(path), "%s/stats", dir);

        pvrdma_handle_t ph = h->dev->pvrdma_handle;
        pvrdma_set_stats_file(ph, path);
        pvrdma_set_stats_instance_info(ph, "/tmp/harness.sock", "loopback");
        pvrdma_set_stats_pci_ids(ph, 0x1dd8, 0x1002);
        pvrdma_set_stats_connection_state(ph, "connected");
        pvrdma_inc_stats_reset_count(ph);
        pvrdma_write_stats(ph);

        FILE *fp = fopen(path, "re");
        char text[16384] = "";
        if (check(T, fp != NULL, "no stats file")) {
            size_t n = fread(text, 1, sizeof(text) - 1, fp);
            text[n] = '\0';
            fclose(fp);
        }
        check(T, strstr(text, "SEND") != NULL && strstr(text, "connected"),
              "the stats file is missing the traffic");
        unlink(path);
        rmdir(dir);
    }

    rc_teardown(T, &r);
}

/* ---- Unreliable datagrams ---------------------------------------------- */

#define UD_QKEY 0x11111111u
#define UD_AH   5u
#define UD_SIP  0x0a000001u
#define UD_DIP  0x0a000002u

static void test_ud(void)
{
    const char *T = "ud";
    struct ih h;
    struct ih_cq cqc, cqd;
    struct ih_qp c, d, rcq;
    struct ih_mr mr = {0};
    struct ih_cqe e;

    if (!setup(T, &h, "loopback"))
        return;

    bool ok = ih_cq_create(&h, &cqc, 3, 5) && ih_cq_create(&h, &cqd, 4, 5) &&
              ih_qp_create(&h, &c, 20, IH_QPT_UD, &cqc, &cqc, NULL) &&
              ih_qp_create(&h, &d, 21, IH_QPT_UD, &cqd, &cqd, NULL) &&
              ih_qp_create(&h, &rcq, 22, IH_QPT_RC, &cqd, &cqd, NULL) &&
              ih_mr_create(&h, &mr, 0x06000006u, UINT64_C(0x7f4000000000),
                           2 * IH_PAGE_SIZE, IH_ACCESS_ALL, false);
    for (uint32_t qp = 20; ok && qp <= 21; qp++)
        ok = ih_qp_modify(&h, qp, IH_QP_STATE | IH_QP_QKEY, IH_QPS_INIT,
                          UD_QKEY, NULL, 0) == 0 &&
             ih_qp_modify(&h, qp, IH_QP_STATE, IH_QPS_RTR, 0, NULL, 0) == 0 &&
             ih_qp_modify(&h, qp, IH_QP_STATE | IH_QP_SQ_PSN, IH_QPS_RTS, 0,
                          NULL, 0) == 0;
    ok = ok && ih_ah_create(&h, UD_AH, UD_SIP, UD_DIP) == 0;
    if (!check(T, ok, "UD setup failed")) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    const uint64_t src_va = mr.va, dst_va = mr.va + 1024;
    uint8_t want[64], got[IH_GRH_SIZE + 64];
    fill(want, sizeof(want), 0x77);
    ih_mr_write(&h, &mr, src_va, want, sizeof(want));
    struct ih_sge src = {.va = src_va, .len = sizeof(want), .lkey = mr.lkey};
    struct ih_sge dst = {.va = dst_va, .len = sizeof(got), .lkey = mr.lkey};
    struct ih_send s = {.op = IH_OP_SEND,
                        .flags = IH_WQE_SIGNALLED,
                        .ah_id = UD_AH,
                        .dest_qpn = 21,
                        .qkey = UD_QKEY,
                        .sge = &src,
                        .nsge = 1};

    /* A datagram arrives behind a GRH built from the address handle */
    uint64_t recv_id = ih_post_recv(&h, &d, &dst, 1);
    uint64_t send_id = ih_post_send(&h, &c, &s);
    check(T,
          ih_cq_poll(&h, &cqc, &e) && e.type == IH_CQE_TYPE_SEND_NPG &&
              e.wqe_id == send_id,
          "UD send did not complete by its SQ index");
    if (check(T, ih_cq_poll(&h, &cqd, &e), "no UD receive completion")) {
        check(T, !e.error && e.status_len == sizeof(got),
              "UD receive length %u, expected %zu", e.status_len, sizeof(got));
        check(T, e.wqe_id == recv_id && e.src_qpn == 20,
              "UD receive is for the wrong WQE or sender");
        check(T, e.has_smac, "UD receive carries no source MAC");
        ih_mr_read(&h, &mr, dst_va, got, sizeof(got));
        /*
         * A v4-mapped address handle means RoCEv2 over IPv4, and that is
         * reported as the packet's IPv4 header in the LAST 20 bytes of the
         * 40-byte field -- union rdma_network_hdr is reserved[20] then
         * struct iphdr -- not as an IPv6 GRH with GIDs at 8 and 24.
         *
         * The flag is checked with the bytes because nothing else ties them
         * together: it becomes wc->network_hdr_type, and a receiver reads
         * the addresses at 32 and 36 on the strength of that alone. Claiming
         * IPv4 over an IPv6 GRH is what made ib_cm resolve the middle of the
         * destination GID as the sender's address and drop every CM REQ.
         */
        const uint8_t *ip = got + IH_GRH_SIZE - 20;
        check(T, e.is_ipv4, "an IPv4 UD receive is not flagged as one");
        check(T, all_zero(got, 20), "the first 20 bytes are not reserved");
        check(T, ip[0] == 0x45 && ernic_get_be16(ip + 2) == 20 + sizeof(want),
              "the IPv4 header's version or total length is wrong");
        check(T, ip[9] == 17, "the IPv4 header does not name UDP");
        check(T, ones_sum16(ip, 20) == 0xffff,
              "the IPv4 header's checksum does not verify");
        check(T,
              ernic_get_be32(ip + 12) == UD_SIP &&
                  ernic_get_be32(ip + 16) == UD_DIP,
              "the IPv4 addresses do not come from the address handle");
        check(T, memcmp(got + IH_GRH_SIZE, want, sizeof(want)) == 0,
              "UD payload is not behind the 40-byte header");
    }

    /* The wrong Q_Key, and a connected destination, are refused even with a
     * receive posted at each; the send still completes. D and the RC QP
     * share a CQ, so either delivery would show up on it. */
    struct ih_sge rc_dst = {
        .va = dst_va + 512, .len = sizeof(got), .lkey = mr.lkey};
    ih_post_recv(&h, &d, &dst, 1);
    ih_post_recv(&h, &rcq, &rc_dst, 1);
    s.qkey = 0x22222222u;
    ih_post_send(&h, &c, &s);
    s.qkey = UD_QKEY;
    s.dest_qpn = 22;
    ih_post_send(&h, &c, &s);
    check(T, ih_cq_poll(&h, &cqc, &e) && ih_cq_poll(&h, &cqc, &e),
          "refused UD sends did not complete");
    check(T, cq_empty(&h, &cqd), "a refused datagram was delivered");

    /* Unsignalled, and naming an AH that was never created: delivered all
     * the same, since the peer is local */
    s.dest_qpn = 21;
    s.flags = 0;
    s.ah_id = 9;
    ih_post_send(&h, &c, &s);
    check(T, ih_cq_poll(&h, &cqc, &e), "unsignalled UD send did not complete");
    check(T, ih_cq_poll(&h, &cqd, &e) && !e.error,
          "a datagram with an unknown AH was not delivered");

    /* A RoCE header carrying IPv6, behind a VLAN tag */
    uint8_t hdr[80] = {0};
    hdr[12] = 0x81;
    hdr[16] = 0x86;
    hdr[17] = 0xdd;
    memset(hdr + 18 + 8, 0x20, 16);
    memset(hdr + 18 + 24, 0x30, 16);
    fake_vfu_write(h.vfu, h.scratch, hdr, sizeof(hdr));
    uint8_t body[24] = {0};
    ernic_put_le64(body, h.scratch);
    ernic_put_le32(body + 8, 18 + 40);
    ernic_put_le32(body + 16, 6);
    check(T, ih_admin(&h, IH_ADMIN_CREATE_AH, body, sizeof(body), NULL) == 0,
          "CREATE_AH from an IPv6 header failed");
    /* And one the device can't parse, which is still not fatal */
    hdr[16] = 0x12;
    fake_vfu_write(h.vfu, h.scratch, hdr, sizeof(hdr));
    ernic_put_le32(body + 16, 7);
    check(T, ih_admin(&h, IH_ADMIN_CREATE_AH, body, sizeof(body), NULL) == 0,
          "CREATE_AH from an unparsable header failed");

    for (uint32_t ah = 5; ah <= 7; ah++) {
        ernic_put_le32(body, ah);
        check(T, ih_admin(&h, IH_ADMIN_DESTROY_AH, body, 4, NULL) == 0,
              "DESTROY_AH %u failed", ah);
    }

    check(T,
          ih_qp_destroy(&h, &c) == 0 && ih_qp_destroy(&h, &d) == 0 &&
              ih_qp_destroy(&h, &rcq) == 0,
          "DESTROY_QP failed");
    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &cqc) == 0 && ih_cq_destroy(&h, &cqd) == 0,
          "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Shared receive queues --------------------------------------------- */

static void test_srq(void)
{
    const char *T = "srq";
    struct ih h;
    struct ih_cq cqf, cqg;
    struct ih_qp f, g;
    struct ih_srq srq;
    struct ih_mr mr = {0};
    struct ih_cqe e;

    if (!setup(T, &h, "loopback"))
        return;

    int rc = ih_srq_create(&h, &srq, 4);
    if (!check(T, rc == 0, "CREATE_SRQ: %d", rc)) {
        teardown(T, &h);
        return;
    }
    check(T,
          srq.id >= IONIC_SRQ_QID_BASE &&
              srq.id < IONIC_SRQ_QID_BASE + IONIC_EMU_SRQ_COUNT,
          "SRQ id %#x is outside the SRQ range", srq.id);

    bool ok = ih_cq_create(&h, &cqf, 5, 5) && ih_cq_create(&h, &cqg, 6, 5) &&
              ih_qp_create(&h, &f, 30, IH_QPT_RC, &cqf, &cqf, &srq) &&
              ih_qp_create(&h, &g, 31, IH_QPT_RC, &cqg, &cqg, NULL) &&
              ih_qp_connect(&h, &f, &g) &&
              ih_mr_create(&h, &mr, 0x07000007u, UINT64_C(0x7f5000000000),
                           IH_PAGE_SIZE, IH_ACCESS_ALL, false);
    if (!check(T, ok, "SRQ setup failed")) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    /* G's sends land in F's receives, which come out of the SRQ */
    uint64_t ids[2];
    for (unsigned i = 0; i < 2; i++) {
        struct ih_sge dst = {
            .va = mr.va + 512 + 64 * i, .len = 64, .lkey = mr.lkey};
        ids[i] = ih_post_srq_recv(&h, &srq, &dst, 1);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint8_t v[16], got[16];
        fill(v, sizeof(v), (uint8_t)(i + 1));
        ih_mr_write(&h, &mr, mr.va, v, sizeof(v));
        struct ih_sge src = {.va = mr.va, .len = sizeof(v), .lkey = mr.lkey};
        struct ih_send s = {.op = IH_OP_SEND, .sge = &src, .nsge = 1};
        ih_post_send(&h, &g, &s);
        check(T, ih_cq_poll(&h, &cqg, &e) && !e.error,
              "send %u did not complete", i);
        if (check(T, ih_cq_poll(&h, &cqf, &e), "SRQ receive %u missing", i)) {
            check(T, !e.error && e.qid == 30 && e.wqe_id == ids[i],
                  "SRQ receive %u completed for QP %u WQE %" PRIu64, i, e.qid,
                  e.wqe_id);
            ih_mr_read(&h, &mr, mr.va + 512 + 64 * i, got, sizeof(got));
            check(T, memcmp(got, v, sizeof(v)) == 0,
                  "SRQ receive %u carried the wrong data", i);
        }
    }

    /* MODIFY_SRQ with a limit and with none, then QUERY_SRQ */
    uint8_t body[8] = {0};
    ernic_put_le32(body, srq.id);
    ernic_put_le16(body + 4, 4);
    check(T, ih_admin(&h, IH_ADMIN_MODIFY_SRQ, body, 6, NULL) == 0,
          "MODIFY_SRQ to 4 failed");
    ernic_put_le16(body + 4, 0);
    check(T, ih_admin(&h, IH_ADMIN_MODIFY_SRQ, body, 6, NULL) == 0,
          "MODIFY_SRQ to 0 failed");
    check(T, ih_admin(&h, IH_ADMIN_QUERY_SRQ, body, 4, NULL) == 0,
          "QUERY_SRQ failed");

    check(T, ih_qp_destroy(&h, &f) == 0 && ih_qp_destroy(&h, &g) == 0,
          "DESTROY_QP failed");
    check(T, ih_srq_destroy(&h, &srq) == 0, "DESTROY_SRQ failed");

    /* A doorbell for the destroyed SRQ is ignored */
    ih_doorbell(&h, IH_QTYPE_RQ, srq.id, 0, 1);

    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &cqf) == 0 && ih_cq_destroy(&h, &cqg) == 0,
          "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Ethernet Tx and Rx ------------------------------------------------- */

#define TX_INTR 2
#define RX_INTR 3

struct tx_seen {
    unsigned frames;
    size_t len;
    uint8_t frame[256];
};

static bool tx_filter(void *ctx, const void *frame, size_t len)
{
    struct tx_seen *t = ctx;

    t->frames++;
    t->len = len;
    memcpy(t->frame, frame, len < sizeof(t->frame) ? len : sizeof(t->frame));
    return true;
}

static bool q_init(struct ih *h, uint8_t qtype, uint16_t intr,
                   uint8_t depth_log2, uint64_t ring, uint64_t cq, uint64_t sg)
{
    uint8_t cmd[64] = {0}, comp[16];

    cmd[0] = IH_CMD_Q_INIT;
    cmd[4] = qtype;
    ernic_put_le16(cmd + 14, intr);
    cmd[19] = depth_log2;
    ernic_put_le64(cmd + 20, ring);
    ernic_put_le64(cmd + 28, cq);
    ernic_put_le64(cmd + 36, sg);
    return ih_devcmd(h, cmd, comp, NULL, 0) == 0;
}

/* Q_CONTROL ENABLE, which the driver sends once a queue is initialized. */
static bool q_enable(struct ih *h, uint8_t qtype, uint32_t index)
{
    uint8_t cmd[64] = {0}, comp[16];

    cmd[0] = IH_CMD_Q_CONTROL;
    cmd[1] = qtype;
    ernic_put_le32(cmd + 4, index);
    cmd[8] = 1; /* IONIC_Q_ENABLE */
    return ih_devcmd(h, cmd, comp, NULL, 0) == 0;
}

static void test_eth(void)
{
    const char *T = "eth";
    struct ih h;
    struct tx_seen seen;

    if (!setup(T, &h, "loopback"))
        return;

    const uint64_t txr = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t txc = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t txsg = fake_vfu_alloc(h.vfu, 8 * 8 * 16, 64);
    const uint64_t rxr = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t rxc = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t bufs = fake_vfu_alloc(h.vfu, 8 * 2048, 4096);
    /* A ring size the device can't shift by: UBSan reports a Q_INIT that
     * does. The status isn't checked, since the device doesn't yet refuse
     * a Q_INIT it can't set up. */
    (void)q_init(&h, IH_QTYPE_ETH_TXQ, TX_INTR, 32, txr, txc, txsg);
    check(T, q_init(&h, IH_QTYPE_ETH_TXQ, TX_INTR, 3, txr, txc, txsg),
          "Q_INIT of the Tx queue failed");
    check(T, q_init(&h, IH_QTYPE_ETH_RXQ, RX_INTR, 3, rxr, rxc, 0),
          "Q_INIT of the Rx queue failed");
    check(T, q_enable(&h, IH_QTYPE_ETH_TXQ, 0), "enabling the Tx queue failed");
    check(T, q_enable(&h, IH_QTYPE_ETH_RXQ, 0), "enabling the Rx queue failed");
    ih_intr_mask(&h, TX_INTR, false);
    ih_intr_mask(&h, RX_INTR, false);

    memset(&seen, 0, sizeof(seen));
    ionic_eth_emu_register_tx_filter(h.dev->ionic_emu, tx_filter, &seen);

    /* One frame in the descriptor, one split over two SG elements */
    uint8_t frame[200];
    fill(frame, sizeof(frame), 0x31);
    fake_vfu_write(h.vfu, bufs, frame, sizeof(frame));

    uint8_t desc[16] = {0};
    ernic_put_le64(desc, bufs << 12);
    ernic_put_le16(desc + 8, 120);
    fake_vfu_write(h.vfu, txr, desc, sizeof(desc));

    ernic_put_le64(desc, (bufs << 12) | 2u << 8);
    ernic_put_le16(desc + 8, sizeof(frame));
    fake_vfu_write(h.vfu, txr + 16, desc, sizeof(desc));
    uint8_t sg[2 * 16] = {0};
    ernic_put_le64(sg, bufs + 50);
    ernic_put_le16(sg + 8, 100);
    ernic_put_le64(sg + 16, bufs + 150);
    ernic_put_le16(sg + 24, 50);
    fake_vfu_write(h.vfu, txsg + 1 * 8 * 16, sg, sizeof(sg));

    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 0, 0, 2);
    check(T, seen.frames == 2, "the Tx filter saw %u frames", seen.frames);
    check(T,
          seen.len == sizeof(frame) &&
              memcmp(seen.frame, frame, sizeof(frame)) == 0,
          "the scatter/gather frame was not reassembled");
    uint8_t comp[16];
    fake_vfu_read(h.vfu, txc + 16, comp, sizeof(comp));
    check(T, (comp[15] & 0x80u) && comp[2] == 1,
          "no completion for the second Tx descriptor");
    check(T, fake_vfu_irq_count(h.vfu, TX_INTR) == 1,
          "Tx completions raised %u interrupts",
          fake_vfu_irq_count(h.vfu, TX_INTR));

    /* Frames handed in off-thread land in the guest's Rx buffers */
    for (unsigned i = 0; i < 2; i++) {
        memset(desc, 0, sizeof(desc));
        ernic_put_le16(desc + 6, i == 0 ? 2048 : 64);
        ernic_put_le64(desc + 8, bufs + 2048 * (i + 1));
        fake_vfu_write(h.vfu, rxr + 16 * i, desc, sizeof(desc));
    }
    ih_doorbell(&h, IH_QTYPE_ETH_RXQ, 0, 0, 2);

    check(T,
          ionic_eth_emu_queue_rx_frame(h.dev->ionic_emu, frame,
                                       sizeof(frame)) == 0,
          "queueing an Rx frame failed");
    check(T,
          ionic_eth_emu_queue_rx_frame(h.dev->ionic_emu, NULL, 10) == -EINVAL,
          "a NULL Rx frame was accepted");
    check(T,
          ionic_eth_emu_queue_rx_frame(h.dev->ionic_emu, frame, 0) == -EINVAL,
          "an empty Rx frame was accepted");
    ih_poll(&h);

    uint8_t got[200];
    fake_vfu_read(h.vfu, bufs + 2048, got, sizeof(got));
    check(T, memcmp(got, frame, sizeof(frame)) == 0, "the Rx frame is wrong");
    fake_vfu_read(h.vfu, rxc, comp, sizeof(comp));
    check(T, (comp[15] & 0x80u) && ernic_get_le16(comp + 12) == sizeof(frame),
          "the Rx completion does not carry the frame length");
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 1, "Rx raised %u interrupts",
          fake_vfu_irq_count(h.vfu, RX_INTR));

    /*
     * A frame too big for the next buffer is dropped, not cut and not held.
     * Holding it would wedge the queue: head does not advance, so every
     * later pass reads the same descriptor and fails the same way, and no
     * frame behind it is ever delivered.
     */
    check(T,
          ionic_eth_emu_queue_rx_frame(h.dev->ionic_emu, frame,
                                       sizeof(frame)) == 0,
          "queueing an Rx frame failed");
    ih_poll(&h);
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 1,
          "a frame that does not fit was delivered");

    /* ...and the next frame that does fit still gets through. */
    uint8_t small[64];
    fill(small, sizeof(small), 0x77);
    check(T,
          ionic_eth_emu_queue_rx_frame(h.dev->ionic_emu, small,
                                       sizeof(small)) == 0,
          "queueing a second Rx frame failed");
    ih_poll(&h);
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 2,
          "an oversized frame wedged the Rx queue behind it");
    uint8_t got_small[64];
    fake_vfu_read(h.vfu, bufs + 2048 * 2, got_small, sizeof(got_small));
    check(T, memcmp(got_small, small, sizeof(small)) == 0,
          "the frame behind the oversized one is wrong");

    /* Tx without a filter is a sink, and a doorbell for a queue that was
     * never initialized is ignored */
    ionic_eth_emu_register_tx_filter(h.dev->ionic_emu, NULL, NULL);
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 0, 0, 3);
    check(T, seen.frames == 2, "Tx with no filter reached the filter");
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 3, 0, 1);
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 99, 0, 1);

    teardown(T, &h);
}

/* ---- NVMe-oF over the GSI QP ------------------------------------------- */

#define NV_TRADDR  0xc0a8c801u /* 192.168.200.1 */
#define NV_PORT    4420u
#define NV_NQN     "nvmet-harness"
#define NV_BACKEND "nvmeof:size=1M,ip=192.168.200.1,port=4420,nqn=" NV_NQN

#define NV_GSI_CQ 1
#define NV_RC_CQ  2
#define NV_RC_QP  0x42u
#define NV_MR     0x08000008u
#define NV_VA     UINT64_C(0x7f6000000000)

/* Where things live in the MR */
#define NV_MAD_OFF     0x0000u
#define NV_GSI_RX_OFF  0x0200u
#define NV_CAP_OFF     0x0400u
#define NV_RC_RX_OFF   0x0480u
#define NV_CONNECT_OFF 0x0800u
#define NV_IDENT_OFF   0x1000u

/* A CM MAD as test_nvmeof_cm.c builds it */
static void mad_hdr(uint8_t *m, uint16_t attr, uint64_t tid)
{
    memset(m, 0, IB_MAD_SIZE);
    m[0] = 1;
    m[1] = 0x07; /* CM class */
    m[2] = 2;
    m[3] = 3; /* SEND */
    ernic_put_be64(m + 8, tid);
    ernic_put_be16(m + 16, attr);
}

static void build_req(uint8_t *m)
{
    mad_hdr(m, 0x0010, 0x1234567800000001ull);
    ernic_put_be32(m + 24, 0xabcd0001u);
    ernic_put_be64(m + 32, ((uint64_t)0x0106 << 16) | NV_PORT);
    ernic_put_be64(m + 40, 0x0002c9030012abcdull);
    ernic_put_be32(m + 56, (NV_RC_QP << 8) | 4);
    ernic_put_be32(m + 60, 4);
    ernic_put_be32(m + 64, 20u << 3);
    ernic_put_be32(m + 68, 0x123456u << 8);
    memset(m + 80, 0x11, 16); /* primary local GID: the guest */
    memset(m + 96, 0x22, 16); /* primary remote GID: the target */

    uint8_t *priv = m + 164;
    priv[1] = 4 << 4;
    ernic_put_be16(priv + 2, NV_PORT);
    ernic_put_be32(priv + 4 + 12, 0xc0a8c814u);
    ernic_put_be32(priv + 20 + 12, NV_TRADDR);
    ernic_put_le16(priv + 36 + 4, 32); /* hrqsize */
    ernic_put_le16(priv + 36 + 6, 31); /* hsqsize */
}

/* Send a MAD on the GSI QP and return the reply delivered back to it. */
static bool gsi_exchange(const char *test, struct ih *h, struct ih_qp *gsi,
                         struct ih_cq *cq, const struct ih_mr *mr,
                         const uint8_t *mad, bool want_reply, uint8_t *reply)
{
    struct ih_cqe e;
    struct ih_sge rx = {.va = mr->va + NV_GSI_RX_OFF,
                        .len = IH_GRH_SIZE + IB_MAD_SIZE,
                        .lkey = mr->lkey};
    struct ih_sge tx = {
        .va = mr->va + NV_MAD_OFF, .len = IB_MAD_SIZE, .lkey = mr->lkey};
    struct ih_send s = {.op = IH_OP_SEND,
                        .flags = IH_WQE_SIGNALLED,
                        .dest_qpn = 1,
                        .qkey = 0x80010000u,
                        .sge = &tx,
                        .nsge = 1};

    ih_mr_write(h, mr, tx.va, mad, IB_MAD_SIZE);
    if (want_reply)
        ih_post_recv(h, gsi, &rx, 1);
    ih_post_send(h, gsi, &s);

    bool sent = false, got = false, recv_ipv4 = false;
    while (ih_cq_poll(h, cq, &e)) {
        if (e.type == IH_CQE_TYPE_SEND_NPG) {
            sent = true;
        } else if (e.type == IH_CQE_TYPE_RECV) {
            got = check(test, !e.error && e.status_len == rx.len,
                        "GSI reply length %u", e.status_len);
            recv_ipv4 = e.is_ipv4;
        }
    }
    check(test, sent, "the GSI send did not complete");
    if (!want_reply)
        return check(test, !got, "a MAD that needs no reply got one");
    if (!check(test, got, "no GSI reply"))
        return false;
    uint8_t buf[IH_GRH_SIZE + IB_MAD_SIZE];
    ih_mr_read(h, mr, rx.va, buf, sizeof(buf));
    /*
     * build_req() fills the CM REQ's GIDs with 0x11 and 0x22, which are not
     * v4-mapped, so this is the other branch: a real IPv6 GRH, with the
     * GIDs where struct ib_grh keeps them and the completion's IPv4 flag
     * clear. The ud test covers the RoCEv2-over-IPv4 form. Between them the
     * dispatch is pinned both ways, which matters because the two forms put
     * the addresses 24 bytes apart and the flag is all a receiver has to
     * tell them apart.
     */
    uint8_t controller[16], guest[16];
    memset(controller, 0x22, sizeof(controller));
    memset(guest, 0x11, sizeof(guest));
    check(test, !recv_ipv4, "a GSI reply carrying a GRH is flagged IPv4");
    check(test, buf[0] == 0x60 && ernic_get_be16(buf + 4) == IB_MAD_SIZE,
          "the GSI reply has no GRH, or the wrong payload length");
    check(test,
          memcmp(buf + 8, controller, 16) == 0 &&
              memcmp(buf + 24, guest, 16) == 0,
          "the GRH's GIDs are not where a GRH keeps them");
    memcpy(reply, buf + IH_GRH_SIZE, IB_MAD_SIZE);
    return true;
}

/* Send a command capsule on the RC QP; returns the NVMe status word. */
static int nvme_exchange(const char *test, struct ih *h, struct ih_qp *qp,
                         struct ih_cq *cq, const struct ih_mr *mr,
                         const uint8_t *capsule)
{
    struct ih_cqe e;
    struct ih_sge rx = {
        .va = mr->va + NV_RC_RX_OFF, .len = 64, .lkey = mr->lkey};
    struct ih_sge tx = {
        .va = mr->va + NV_CAP_OFF, .len = NVME_SQE_SIZE, .lkey = mr->lkey};
    struct ih_send s = {
        .op = IH_OP_SEND, .flags = IH_WQE_SIGNALLED, .sge = &tx, .nsge = 1};

    ih_mr_write(h, mr, tx.va, capsule, NVME_SQE_SIZE);
    ih_post_recv(h, qp, &rx, 1);
    ih_post_send(h, qp, &s);

    int status = -1;
    while (ih_cq_poll(h, cq, &e)) {
        if (e.type == IH_CQE_TYPE_RECV &&
            check(test, !e.error && e.status_len == NVME_CQE_SIZE,
                  "response capsule length %u", e.status_len)) {
            uint8_t rsp[NVME_CQE_SIZE];
            ih_mr_read(h, mr, rx.va, rsp, sizeof(rsp));
            status = ernic_get_le16(rsp + NVME_CQE_STATUS_OFF) >> 1;
        }
    }
    return status;
}

static void capsule_init(uint8_t *c, uint8_t opcode, uint16_t cid,
                         uint64_t addr, uint32_t len, uint32_t key)
{
    memset(c, 0, NVME_SQE_SIZE);
    c[NVME_SQE_OPCODE_OFF] = opcode;
    ernic_put_le16(c + NVME_SQE_CID_OFF, cid);

    /* A keyed SGL data block descriptor */
    uint8_t *d = c + NVME_SQE_DPTR_OFF;
    ernic_put_le64(d, addr);
    d[8] = (uint8_t)len;
    d[9] = (uint8_t)(len >> 8);
    d[10] = (uint8_t)(len >> 16);
    ernic_put_le32(d + 11, key);
    d[15] = NVME_SGL_TYPE_KEYED_DATA << 4;
}

#ifdef ERNIC_HAVE_P4

/* ---- The P4 port's place in the Tx filter chain ------------------------- */

struct p4_upstream {
    unsigned offered;
    unsigned claimed;
};

/*
 * Claims only frames carrying the magic first byte, so one test exercises
 * both branches of the chain: a frame the previous filter holder consumes,
 * which must never reach the pipeline, and one it declines, which must.
 */
#define P4_UPSTREAM_MAGIC 0xAAu

static bool p4_upstream_filter(void *ctx, const void *frame, size_t len)
{
    struct p4_upstream *u = ctx;
    const uint8_t *bytes = frame;

    u->offered++;
    if (len == 0 || bytes[0] != P4_UPSTREAM_MAGIC)
        return false;
    u->claimed++;
    return true;
}

/* Put one frame on the Tx ring and ring the doorbell for it. */
static void p4_tx_one(struct ih *h, uint64_t txr, uint64_t buf, uint8_t first,
                      uint16_t index)
{
    uint8_t frame[64];
    uint8_t desc[16] = {0};

    fill(frame, sizeof(frame), 0x55);
    frame[0] = first;
    fake_vfu_write(h->vfu, buf, frame, sizeof(frame));

    ernic_put_le64(desc, buf << 12);
    ernic_put_le16(desc + 8, (uint16_t)sizeof(frame));
    fake_vfu_write(h->vfu, txr + (uint64_t)index * 16u, desc, sizeof(desc));
    ih_doorbell(h, IH_QTYPE_ETH_TXQ, 0, 0, (uint16_t)(index + 1));
}

static void test_p4_port(void)
{
    const char *T = "p4-port";
    struct ih h;
    struct p4_upstream up;
    struct p4_port_upstream chain;
    struct p4_stats estats;
    struct p4_port_stats pstats;

    if (!setup(T, &h, "loopback"))
        return;

    const uint64_t txr = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t txc = fake_vfu_alloc(h.vfu, 8 * 16, 64);
    const uint64_t buf = fake_vfu_alloc(h.vfu, 4096, 4096);
    check(T, q_init(&h, IH_QTYPE_ETH_TXQ, TX_INTR, 3, txr, txc, 0),
          "Q_INIT of the Tx queue failed");
    ih_intr_mask(&h, TX_INTR, false);

    /* Stand in for the S3 control plane, which really does hold this slot. */
    memset(&up, 0, sizeof(up));
    ionic_eth_emu_register_tx_filter(h.dev->ionic_emu, p4_upstream_filter, &up);

    memset(&chain, 0, sizeof(chain));
    ionic_eth_emu_get_tx_filter(h.dev->ionic_emu, &chain.fn, &chain.ctx);
    check(T, chain.fn == p4_upstream_filter && chain.ctx == &up,
          "the registered Tx filter did not read back");

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        teardown(T, &h);
        return;
    }
    struct p4_port *port = p4_port_attach(h.dev->ionic_emu, eng, &chain);
    if (!check(T, port != NULL, "p4_port_attach failed")) {
        p4_engine_destroy(eng);
        teardown(T, &h);
        return;
    }

    /* A frame the previous holder claims must not reach the pipeline. */
    p4_tx_one(&h, txr, buf, P4_UPSTREAM_MAGIC, 0);
    p4_engine_get_stats(eng, &estats);
    p4_port_get_stats(port, &pstats);
    check(T, up.claimed == 1, "the chained filter claimed %u frames",
          up.claimed);
    check(T, estats.frames_in[P4_PORT_HOST] == 0,
          "a claimed frame reached the pipeline");
    check(T, pstats.to_upstream == 1, "the claimed frame was not counted");

    /* One it declines must, and with no backend attached its forward to the
     * uplink fails, which is the only observable the engine has today. */
    p4_tx_one(&h, txr, buf, 0xBBu, 1);
    p4_engine_get_stats(eng, &estats);
    p4_port_get_stats(port, &pstats);
    check(T, up.offered == 2, "the chained filter saw %u frames", up.offered);
    check(T, up.claimed == 1,
          "the chained filter claimed a frame it should "
          "have declined");
    check(T, estats.frames_in[P4_PORT_HOST] == 1,
          "the declined frame did not reach the pipeline");
    check(T, estats.frames_out[P4_PORT_UPLINK] == 1,
          "the pipeline did not forward to the uplink");
    check(T, pstats.egress_errors == 1,
          "a forward with no host backend should fail, egress_errors=%" PRIu64,
          pstats.egress_errors);
    check(T, pstats.to_net == 0,
          "a frame left through a backend that is not "
          "attached");

    /* Detaching must hand the slot back, not leave it empty. */
    p4_port_detach(port);
    ionic_eth_emu_get_tx_filter(h.dev->ionic_emu, &chain.fn, &chain.ctx);
    check(T, chain.fn == p4_upstream_filter && chain.ctx == &up,
          "detach did not restore the previous Tx filter");

    p4_tx_one(&h, txr, buf, P4_UPSTREAM_MAGIC, 2);
    check(T, up.claimed == 2, "the restored filter did not claim after detach");
    p4_engine_get_stats(eng, &estats);
    check(T, estats.frames_in[P4_PORT_HOST] == 1,
          "the pipeline ran after it was detached");

    p4_engine_destroy(eng);
    ionic_eth_emu_register_tx_filter(h.dev->ionic_emu, NULL, NULL);
    teardown(T, &h);
}
/* ---- In-network elementwise compute across three memory regions ------- */

#define FP8R_MR_A   0x03000001u
#define FP8R_MR_B   0x03000002u
#define FP8R_MR_C   0x03000003u
#define FP8R_VA_A   UINT64_C(0x7F2000000000)
#define FP8R_VA_B   UINT64_C(0x7F3000000000)
#define FP8R_VA_C   UINT64_C(0x7F4000000000)
#define FP8R_MR_LEN (4u * IH_PAGE_SIZE)

/*
 * The worked example: the NIC reads two operand regions, combines them
 * pointwise, and writes a result region of the same length. Combining two
 * partial buffers is what a collective does, so this is the primitive an
 * allreduce is built from.
 *
 * Nothing here is on the Ethernet wire -- this is the RDMA plane, where
 * the loopback backend resolves a key to a local region.
 */
static void test_fp8_map(void)
{
    const char *T = "fp8-map";
    struct ih h;
    struct ih_mr mra, mrb, mrc;
    /* Longer than one streaming block, so the chunking is exercised and a
     * loop that dropped the offset would show up as a wrong tail. */
    enum { N = 5000 };
    static uint8_t a[N], b[N], got[N];

    if (!setup(T, &h, "loopback"))
        return;

    if (!check(T,
               ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, FP8R_MR_LEN,
                            IH_ACCESS_ALL, false) &&
                   ih_mr_create(&h, &mrb, FP8R_MR_B, FP8R_VA_B, FP8R_MR_LEN,
                                IH_ACCESS_ALL, false) &&
                   /* C's pages are reversed, so a write that ignores the
                    * page table lands in the wrong place and shows up. */
                   ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, FP8R_MR_LEN,
                                IH_ACCESS_ALL, true),
               "the three memory regions were not registered")) {
        teardown(T, &h);
        return;
    }

    /* 1.0 everywhere in A, 2.0 everywhere in B. */
    memset(a, 0x38, sizeof(a));
    memset(b, 0x40, sizeof(b));
    /* ...except one element, so a result that ignored position would be
     * indistinguishable from one that did not. */
    a[1234] = 0x48; /* 4.0 */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, a, sizeof(a)), "write to A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, b, sizeof(b)), "write to B");

    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, N, mrc.lkey,
                                 FP8R_VA_C) == 0,
          "the elementwise add failed");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back from C");

    {
        unsigned wrong = 0;
        unsigned first = 0;

        for (unsigned i = 0; i < N; i++) {
            /* 1+2 is 3 (0x44) everywhere but the one element, where
             * 4+2 is 6 (0x4C). */
            uint8_t want = (i == 1234) ? 0x4Cu : 0x44u;

            if (got[i] != want) {
                if (!wrong)
                    first = i;
                wrong++;
            }
        }
        check(T, wrong == 0, "%u of %u elements are wrong, the first at %u",
              wrong, (unsigned)N, first);
    }

    /* Multiply, over a length that is not a multiple of the block. */
    memset(a, 0x40, sizeof(a)); /* 2.0 */
    memset(b, 0x44, sizeof(b)); /* 3.0 */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, a, sizeof(a)), "write to A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, b, sizeof(b)), "write to B");
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_MUL, mra.lkey,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, 3001, mrc.lkey,
                                 FP8R_VA_C) == 0,
          "the elementwise multiply failed");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, 3001), "read back from C");
    {
        unsigned wrong = 0;

        for (unsigned i = 0; i < 3001; i++) {
            if (got[i] != 0x4Cu) /* 2*3 is 6 */
                wrong++;
        }
        check(T, wrong == 0, "%u of 3001 products are wrong", wrong);
    }

    /*
     * Key 0 is IONIC_DMA_LKEY, which bypasses the region table and treats
     * the address as a bus address. A guest-supplied request must not be
     * able to name it.
     */
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, 0, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 64, mrc.lkey,
                                 FP8R_VA_C) == -EACCES,
          "key 0 was accepted as an operand");
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, 64, 0,
                                 FP8R_VA_C) == -EACCES,
          "key 0 was accepted as a destination");

    /* A key that was never registered must fault, not read host memory. */
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, 0xDEADBEEFu,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, 64, mrc.lkey,
                                 FP8R_VA_C) == -EACCES,
          "an unregistered operand key was accepted");
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, 64,
                                 0xDEADBEEFu, FP8R_VA_C) == -EACCES,
          "an unregistered destination key was accepted");

    /*
     * A length past the end of a region is refused before anything moves,
     * rather than part way through with a half-written destination.
     */
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey,
                                 FP8R_VA_A + FP8R_MR_LEN - 8, mrb.lkey,
                                 FP8R_VA_B, 64, mrc.lkey, FP8R_VA_C) == -EACCES,
          "a read past the end of a region was accepted");
    {
        /* ...and the destination really was left alone. */
        uint8_t probe[8];
        uint8_t before[8];

        memset(before, 0x5A, sizeof(before));
        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, before, sizeof(before)),
              "prime C");
        (void)ionic_datapath_fp8_map(
            h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A, mrb.lkey,
            FP8R_VA_B + FP8R_MR_LEN - 8, 64, mrc.lkey, FP8R_VA_C);
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, probe, sizeof(probe)),
              "read C back");
        check(T, memcmp(probe, before, sizeof(probe)) == 0,
              "a refused request wrote to the destination anyway");
    }

    /* Bounds on the request itself. */
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey,
                                 FP8R_VA_A, mrb.lkey, FP8R_VA_B, 0, mrc.lkey,
                                 FP8R_VA_C) == -EINVAL,
          "a zero length was accepted");
    check(T,
          ionic_datapath_fp8_map(
              h.dev->ionic_dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A, mrb.lkey,
              FP8R_VA_B, IONIC_FP8_MAP_MAX + 1, mrc.lkey, FP8R_VA_C) == -EINVAL,
          "a length over the cap was accepted");
    check(T,
          ionic_datapath_fp8_map(h.dev->ionic_dp, 99, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 64, mrc.lkey,
                                 FP8R_VA_C) == -EINVAL,
          "an unknown operation was accepted");

    check(T, ih_mr_destroy(&h, &mra) == 0, "DESTROY_MR A failed");
    check(T, ih_mr_destroy(&h, &mrb) == 0, "DESTROY_MR B failed");
    check(T, ih_mr_destroy(&h, &mrc) == 0, "DESTROY_MR C failed");
    ih_mr_free(&mra);
    ih_mr_free(&mrb);
    ih_mr_free(&mrc);

    teardown(T, &h);
}

/* ---- A P4 program governing an RDMA operation -------------------------- */

#define P4R_CQ     7
#define P4R_QP_A   40
#define P4R_QP_B   41
#define P4R_N      3000u
#define P4R_MR_LEN (4u * IH_PAGE_SIZE)

/* The bytes of a compiled fixture, or NULL. The caller frees them. */
static char *p4d_read_file(const char *name, size_t *out_len)
{
    char path[512];
    FILE *f;
    long n;
    char *buf;

    (void)snprintf(path, sizeof(path), "%s/%s", P4_FIXTURE_DIR, name);
    f = fopen(path, "rbe"); /* e: O_CLOEXEC */
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static int p4r_load(struct p4_engine *eng, const char *name)
{
    size_t n = 0;
    char *buf = p4d_read_file(name, &n);
    int rc;

    if (!buf)
        return -ENOENT;
    rc = p4_engine_load_begin(eng, n);
    if (rc == 0)
        rc = p4_engine_load_chunk(eng, 0, buf, n);
    if (rc == 0)
        rc = p4_engine_load_commit(eng);
    free(buf);
    return rc;
}

/*
 * The whole of what was asked for, in one path: a compiled P4 program
 * decides, from an RDMA operation's opcode, that an elementwise fp8 add
 * happens, and the result lands in a third region.
 *
 * What makes this P4 rather than a hard-coded offload is that the same
 * operation does an ordinary RDMA write when the table says nothing, and
 * the arithmetic changes when the table entry changes -- with no change
 * here, and with the program never naming a key or an address.
 */
static void test_p4_rdma(void)
{
    const char *T = "p4-rdma";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a, b;
    struct ih_mr mra, mrb, mrc;
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t opa[P4R_N], opb[P4R_N], got[P4R_N];

    if (!setup(T, &h, "loopback"))
        return;

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrb, FP8R_MR_B, FP8R_VA_B, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              /* Reversed pages again: a write that ignores the page table
               * lands somewhere else and the comparison says so. */
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "P4 RDMA setup failed")) {
        ih_mr_free(&mra);
        ih_mr_free(&mrb);
        ih_mr_free(&mrc);
        teardown(T, &h);
        return;
    }

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        ih_mr_free(&mra);
        ih_mr_free(&mrb);
        ih_mr_free(&mrc);
        teardown(T, &h);
        return;
    }
    check(T, p4r_load(eng, "fp8_policy.json") == 0,
          "the compiled policy program did not load");
    ionic_datapath_set_p4_engine(h.dev->ionic_dp, eng);

    /* 1.0 in A, 2.0 in B, with one element of A at 4.0 so a result that
     * ignored position could not pass. */
    memset(opa, 0x38, sizeof(opa));
    memset(opb, 0x40, sizeof(opb));
    opa[777] = 0x48;
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, opa, sizeof(opa)), "write to A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, opb, sizeof(opb)), "write to B");

    struct ih_sge sge[2] = {
        {.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey},
        {.va = FP8R_VA_B, .len = P4R_N, .lkey = mrb.lkey},
    };
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        /* Deliberately not set: the harness then sums the gather list,
         * which is what ionic_prep_common() puts in a real WQE. Setting
         * it by hand is a thing no guest driver can do, and doing so
         * here once hid a device-side check that no guest could pass. */
        .sge = sge,
        .nsge = 2,
    };

    /*
     * With the table empty this is just an RDMA write, so the two operand
     * regions arrive in C one after the other. That is the baseline the
     * entry has to change: without it, "the result is right" could be true
     * of a device that ignored the pipeline entirely.
     */
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the plain write did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T, memcmp(got, opa, sizeof(got)) == 0,
          "with no entry the operation was not an ordinary write");

    /* Now the entry: for an RDMA write, add the two operands elementwise. */
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.reduce_with");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    up.nparams = 1;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_ADD;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the entry was not installed: %s", err);

    memset(got, 0, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "clear C");

    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the governed operation did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back the result");
    {
        unsigned wrong = 0, first = 0;

        for (unsigned i = 0; i < P4R_N; i++) {
            uint8_t want = (i == 777) ? 0x4Cu : 0x44u; /* 4+2=6, else 1+2=3 */

            if (got[i] != want) {
                if (!wrong)
                    first = i;
                wrong++;
            }
        }
        check(T, wrong == 0,
              "%u of %u elements are wrong, the first at %u -- the program "
              "did not govern the operation",
              wrong, P4R_N, first);
    }

    /*
     * Change only the table entry and the arithmetic changes: 1*2 is 2
     * (0x40) and 4*2 is 8 (0x50). Nothing in the device was told.
     */
    up.type = P4_UPDATE_MODIFY;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_MUL;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the entry was not modified: %s", err);

    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the multiply did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back the product");
    check(T, got[0] == 0x40u && got[777] == 0x50u,
          "changing the entry did not change the arithmetic: got %#x, %#x",
          got[0], got[777]);

    /*
     * The operand convention is the architecture's, not the program's. A
     * gather list that is not two equal-length entries has not asked for
     * arithmetic at all, so the extern declines it and the operation
     * dispatches as the ordinary write it is.
     *
     * Failing it instead is what made a guest that installed a reduce
     * entry unable to do an ordinary RDMA WRITE on any QP of the device:
     * the table is keyed on the opcode, which cannot tell the two apart,
     * so the action fires for every write.
     */
    struct ih_send one = w;
    one.nsge = 1;
    memset(got, 0, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "clear C before the one-SGE write");
    ih_post_send(&h, &a, &one);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "a one-SGE write under the same entry was refused");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back the one-SGE write");
    check(T, memcmp(got, opa, sizeof(got)) == 0,
          "the one-SGE write did not land as an ordinary transfer");

    /*
     * A program whose action calls the extern but does not drop would have
     * the ordinary transfer land on top of the result. There is nothing
     * downstream to notice that, so the operation is refused here instead.
     */
    {
        struct p4_engine *nodrop = p4_engine_create();

        if (check(T, nodrop != NULL, "p4_engine_create failed")) {
            check(T, p4r_load(nodrop, "fp8_nodrop.json") == 0,
                  "the no-drop program did not load");
            up.type = P4_UPDATE_INSERT;
            check(T, p4_engine_update(nodrop, &up, err, sizeof(err)) == 0,
                  "the entry was not installed: %s", err);
            ionic_datapath_set_p4_engine(h.dev->ionic_dp, nodrop);
            ih_post_send(&h, &a, &w);
            check(T, ih_cq_poll(&h, &cq, &e) && e.error,
                  "an extern call without a drop was allowed to transfer "
                  "over its own result");
            ionic_datapath_set_p4_engine(h.dev->ionic_dp, eng);
            p4_engine_destroy(nodrop);
        }
    }

    /* Taking the engine away restores the device's own behaviour. */
    ionic_datapath_set_p4_engine(h.dev->ionic_dp, NULL);
    memset(got, 0, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "clear C");
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the write did not complete with the engine detached");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T, memcmp(got, opa, sizeof(got)) == 0,
          "detaching the engine did not restore the ordinary write");

    p4_engine_destroy(eng);

    check(T, ih_qp_destroy(&h, &a) == 0, "DESTROY_QP A failed");
    check(T, ih_qp_destroy(&h, &b) == 0, "DESTROY_QP B failed");
    check(T, ih_mr_destroy(&h, &mra) == 0, "DESTROY_MR A failed");
    check(T, ih_mr_destroy(&h, &mrb) == 0, "DESTROY_MR B failed");
    check(T, ih_mr_destroy(&h, &mrc) == 0, "DESTROY_MR C failed");
    ih_mr_free(&mra);
    ih_mr_free(&mrb);
    ih_mr_free(&mrc);
    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");

    teardown(T, &h);
}


/* ---- The guest's own way in to the engine ------------------------------ */

/*
 * One RPC, with no request or response buffer unless given, built in the
 * 64-byte buffer the BAR0 window takes. Everything the guest sends is
 * little-endian.
 */
static int p4d_rpc(struct ih *h, uint8_t scope, uint32_t ep, uint32_t op,
                   uint64_t req_pa, uint32_t req_len, uint64_t resp_pa,
                   uint32_t resp_len, uint8_t comp[16])
{
    uint8_t cmd[64] = {0};

    cmd[0] = IONIC_CMD_P4_RPC;
    cmd[1] = scope;
    ernic_put_le32(cmd + 4, ep);
    ernic_put_le32(cmd + 8, op);
    ernic_put_le32(cmd + 12, req_len);
    ernic_put_le64(cmd + 16, req_pa);
    ernic_put_le32(cmd + 24, resp_len);
    ernic_put_le64(cmd + 32, resp_pa);
    return ih_devcmd(h, cmd, comp, NULL, 0);
}

/*
 * The whole guest-facing story: a guest identifies the control surface,
 * enumerates what it may do, pushes a compiled P4 program down in chunks,
 * installs a table entry, and then sees an ordinary RDMA write become an
 * elementwise fp8 add.
 *
 * Every step goes through the BAR0 devcmd window, which is the same path
 * a driver in a VM uses -- nothing here calls the engine's C API, which
 * is exactly what made the existing coverage unable to speak for a guest.
 */
static void test_p4_devcmd(void)
{
    const char *T = "p4-devcmd";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a, b;
    struct ih_mr mra, mrb, mrc;
    struct ih_cqe e;
    uint8_t comp[16];
    static uint8_t opa[P4R_N], opb[P4R_N], got[P4R_N];
    size_t prog_len = 0;
    char *prog;

    if (!setup(T, &h, "loopback"))
        return;

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        teardown(T, &h);
        return;
    }
    struct p4_devcmd *pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_LOAD);
    if (!check(T, pd != NULL, "p4_devcmd_create failed")) {
        p4_engine_destroy(eng);
        teardown(T, &h);
        return;
    }
    ionic_eth_emu_register_p4_handler(h.dev->ionic_emu, p4_devcmd_dispatch, pd);
    ionic_datapath_set_p4_engine(h.dev->ionic_dp, eng);

    const uint64_t buf = fake_vfu_alloc(h.vfu, 4096, 4096);

    /* IDENT: what the device is, and what this guest may do with it. */
    {
        uint8_t cmd[64] = {0};
        struct p4_devcmd_ident id;

        cmd[0] = IONIC_CMD_P4_IDENT;
        ernic_put_le32(cmd + 4, (uint32_t)sizeof(id));
        ernic_put_le64(cmd + 8, buf);
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == P4_STS_OK,
              "IDENT failed");
        fake_vfu_read(h.vfu, buf, &id, sizeof(id));
        check(T, le32toh(id.abi_version) == 1, "IDENT reported ABI %u",
              le32toh(id.abi_version));
        check(T, le32toh(id.scope) == P4_SCOPE_LOAD,
              "IDENT reported scope %u, not the one granted",
              le32toh(id.scope));
        check(T, le32toh(id.max_program) == P4_PROGRAM_MAX,
              "IDENT reported a program limit of %u", le32toh(id.max_program));
        check(T, le32toh(id.endpoints) == 3, "IDENT reported %u endpoints",
              le32toh(id.endpoints));
    }

    /* QUERY(ROOT): the endpoints, and QUERY(ENDPOINT): their operations. */
    {
        uint8_t cmd[64] = {0};
        struct p4_devcmd_query_data qd;

        cmd[0] = IONIC_CMD_P4_QUERY;
        cmd[1] = P4_QUERY_ROOT;
        ernic_put_le32(cmd + 4, 4096);
        ernic_put_le64(cmd + 8, buf);
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == P4_STS_OK,
              "QUERY(ROOT) failed");
        fake_vfu_read(h.vfu, buf, &qd, sizeof(qd));
        check(T, le32toh(qd.num_entries) == 3 && le32toh(qd.total) == 3,
              "QUERY(ROOT) returned %u of %u", le32toh(qd.num_entries),
              le32toh(qd.total));

        /*
         * A buffer too small for everything must report what fits AND what
         * exists, so the guest can retry rather than walk off its own
         * buffer trusting a count the device chose.
         */
        ernic_put_le32(cmd + 4, (uint32_t)(sizeof(qd) + 4));
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == P4_STS_OK,
              "QUERY(ROOT) with a small buffer failed");
        fake_vfu_read(h.vfu, buf, &qd, sizeof(qd));
        check(T, le32toh(qd.num_entries) == 1 && le32toh(qd.total) == 3,
              "a truncated QUERY reported %u of %u, not 1 of 3",
              le32toh(qd.num_entries), le32toh(qd.total));

        cmd[1] = P4_QUERY_ENDPOINT;
        ernic_put_le32(cmd + 4, 4096);
        ernic_put_le32(cmd + 16, P4_EP_PROGRAM);
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == P4_STS_OK,
              "QUERY(PROGRAM) failed");
        fake_vfu_read(h.vfu, buf, &qd, sizeof(qd));
        check(T, le32toh(qd.num_entries) == 4,
              "the program endpoint offers %u operations, not 4",
              le32toh(qd.num_entries));

        /* An endpoint the device does not have is named as such. */
        ernic_put_le32(cmd + 16, 0x5eu);
        check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == P4_STS_EOPCODE,
              "a query for an absent endpoint was not refused");
    }

    /* Push the compiled program down, in chunks, as a driver would. */
    prog = p4d_read_file("fp8_policy.json", &prog_len);
    if (!check(T, prog != NULL, "could not read the program fixture")) {
        goto out;
    }
    {
        uint8_t body[8] = {0};
        const uint64_t req = fake_vfu_alloc(h.vfu, 65536, 4096);
        size_t off = 0;

        ernic_put_le32(body, (uint32_t)prog_len);
        fake_vfu_write(h.vfu, req, body, sizeof(body));
        check(T,
              p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, req,
                      (uint32_t)sizeof(struct p4_rpc_load_begin), 0, 0,
                      comp) == P4_STS_OK,
              "LOAD_BEGIN failed");

        while (off < prog_len) {
            uint8_t hdr[16] = {0};
            size_t n = prog_len - off;

            if (n > 4096)
                n = 4096;
            ernic_put_le64(hdr, off);
            ernic_put_le32(hdr + 8, (uint32_t)n);
            fake_vfu_write(h.vfu, req, hdr, sizeof(hdr));
            fake_vfu_write(h.vfu, req + sizeof(hdr), prog + off, n);
            if (!check(T,
                       p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM,
                               P4_OP_LOAD_CHUNK, req,
                               (uint32_t)(sizeof(hdr) + n), 0, 0,
                               comp) == P4_STS_OK,
                       "LOAD_CHUNK at %zu failed", off))
                break;
            off += n;
        }

        check(T,
              p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_COMMIT, 0, 0,
                      0, 0, comp) == P4_STS_OK,
              "LOAD_COMMIT failed");
        check(T, p4_engine_has_program(eng),
              "the engine has no program after a guest loaded one");
    }
    free(prog);

    /* Install the table entry that turns the write into a reduction. */
    {
        const uint64_t req = fake_vfu_alloc(h.vfu, 4096, 4096);
        uint8_t body[sizeof(struct p4_rpc_entry_update) +
                     sizeof(struct p4_rpc_entry_match) +
                     sizeof(struct p4_rpc_entry_param)] = {0};
        struct p4_rpc_entry_update u;
        struct p4_rpc_entry_match m;
        struct p4_rpc_entry_param pa;

        memset(&u, 0, sizeof(u));
        memset(&m, 0, sizeof(m));
        memset(&pa, 0, sizeof(pa));
        u.type = P4_UPDATE_INSERT;
        u.nmatch = htole16(1);
        u.nparams = htole16(1);
        (void)snprintf(u.table, sizeof(u.table), "Ing.policy");
        (void)snprintf(u.action, sizeof(u.action), "Ing.reduce_with");
        m.len = htole16(1);
        m.value[P4_DEVCMD_VALUE_MAX - 1] = IH_OP_RDMA_WRITE;
        pa.len = htole16(1);
        pa.value[P4_DEVCMD_VALUE_MAX - 1] = P4_FP8_OP_ADD;

        memcpy(body, &u, sizeof(u));
        memcpy(body + sizeof(u), &m, sizeof(m));
        memcpy(body + sizeof(u) + sizeof(m), &pa, sizeof(pa));
        fake_vfu_write(h.vfu, req, body, sizeof(body));

        check(T,
              p4d_rpc(&h, P4_SCOPE_WRITE, P4_EP_TABLE, P4_OP_ENTRY_UPDATE, req,
                      (uint32_t)sizeof(body), 0, 0, comp) == P4_STS_OK,
              "the guest could not install a table entry");
        /* Installing it twice is how the test knows the first one is
         * really there, rather than having been accepted and dropped. */
        check(T,
              p4d_rpc(&h, P4_SCOPE_WRITE, P4_EP_TABLE, P4_OP_ENTRY_UPDATE, req,
                      (uint32_t)sizeof(body), 0, 0, comp) == P4_STS_EEXIST,
              "a duplicate entry was not refused");
    }

    /* And now the point of all of it: the guest's own RDMA write becomes
     * an elementwise fp8 add, because of an entry the guest installed. */
    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrb, FP8R_MR_B, FP8R_VA_B, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "RDMA setup failed"))
        goto out_mr;

    memset(opa, 0x38, sizeof(opa)); /* 1.0 */
    memset(opb, 0x40, sizeof(opb)); /* 2.0 */
    opa[321] = 0x48;                /* 4.0, so position matters */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, opa, sizeof(opa)), "write A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, opb, sizeof(opb)), "write B");

    struct ih_sge sge[2] = {
        {.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey},
        {.va = FP8R_VA_B, .len = P4R_N, .lkey = mrb.lkey},
    };
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        /* Deliberately not set: the harness then sums the gather list,
         * which is what ionic_prep_common() puts in a real WQE. Setting
         * it by hand is a thing no guest driver can do, and doing so
         * here once hid a device-side check that no guest could pass. */
        .sge = sge,
        .nsge = 2,
    };
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the governed operation did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back the result");
    {
        unsigned wrong = 0, first = 0;

        for (unsigned i = 0; i < P4R_N; i++) {
            uint8_t want = (i == 321) ? 0x4Cu : 0x44u;

            if (got[i] != want) {
                if (!wrong)
                    first = i;
                wrong++;
            }
        }
        check(T, wrong == 0,
              "%u of %u elements wrong, first at %u -- a guest-installed "
              "entry did not govern the operation",
              wrong, P4R_N, first);
    }

    /* Counters come back the same way, which is the only runtime read of
     * them there has ever been. */
    {
        const uint64_t rb = fake_vfu_alloc(h.vfu, 4096, 4096);
        struct p4_rpc_stats st;

        check(T,
              p4d_rpc(&h, P4_SCOPE_READ, P4_EP_STATS, P4_OP_STATS_READ, 0, 0,
                      rb, (uint32_t)sizeof(st), comp) == P4_STS_OK,
              "STATS_READ failed");
        fake_vfu_read(h.vfu, rb, &st, sizeof(st));
        check(T, le64toh(st.frames_in[P4_PORT_HOST]) == 0,
              "the Ethernet plane saw %" PRIu64 " frames in an RDMA test",
              le64toh(st.frames_in[P4_PORT_HOST]));
    }

out_mr:
    ih_mr_destroy(&h, &mra);
    ih_mr_destroy(&h, &mrb);
    ih_mr_destroy(&h, &mrc);
    ih_mr_free(&mra);
    ih_mr_free(&mrb);
    ih_mr_free(&mrc);
out:
    /*
     * A narrower grant refuses the wide operations and keeps the narrow
     * ones, and the refusal does not depend on what the caller claims:
     * the operator's grant is checked before the caller's own scope.
     */
    ionic_datapath_set_p4_engine(h.dev->ionic_dp, NULL);
    p4_devcmd_destroy(pd);
    pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_READ);
    if (check(T, pd != NULL, "p4_devcmd_create failed")) {
        ionic_eth_emu_register_p4_handler(h.dev->ionic_emu, p4_devcmd_dispatch,
                                          pd);
        check(T,
              p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, 0, 8,
                      0, 0, comp) == P4_STS_EPERM,
              "a read-only guest was allowed to begin a program load");
        check(T,
              p4d_rpc(&h, P4_SCOPE_WRITE, P4_EP_TABLE, P4_OP_ENTRY_UPDATE, 0,
                      200, 0, 0, comp) == P4_STS_EPERM,
              "a read-only guest was allowed to change a table");
        check(T,
              p4d_rpc(&h, P4_SCOPE_READ, P4_EP_STATS, P4_OP_STATS_READ, 0, 0,
                      buf, (uint32_t)sizeof(struct p4_rpc_stats),
                      comp) == P4_STS_OK,
              "a read-only guest could not read the counters");
    }

    ionic_eth_emu_register_p4_handler(h.dev->ionic_emu, NULL, NULL);
    p4_devcmd_destroy(pd);
    p4_engine_destroy(eng);
    teardown(T, &h);
}

/* ---- Narrowing: fp16 in one region, fp8 in a shorter one ------------- */

/*
 * The conversion shape, driven through the whole device with the
 * compiled fp8_convert.json.
 *
 * Everything the reduction test establishes about the pipeline holds
 * here; what is new is that the destination is HALF the source, so a
 * device that assumed three regions of one length would write twice as
 * much as it should. The source is read as one gather entry, which is
 * also what tells a conversion from a reduction.
 */
#define P4C_N_ELEMS 1400u              /* fp8 results */
#define P4C_N_IN    (P4C_N_ELEMS * 2u) /* fp16 source bytes */

static void test_p4_cvt(void)
{
    const char *T = "p4-cvt";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a, b;
    struct ih_mr mra, mrc;
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t src[P4C_N_IN], got[P4C_N_IN], want[P4C_N_ELEMS];

    if (!setup(T, &h, "loopback"))
        return;

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              /* Reversed pages, so a write that ignores the page table
               * lands somewhere else and the comparison says so. */
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "P4 conversion setup failed")) {
        ih_mr_free(&mra);
        ih_mr_free(&mrc);
        teardown(T, &h);
        return;
    }

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        ih_mr_free(&mra);
        ih_mr_free(&mrc);
        teardown(T, &h);
        return;
    }
    check(T, p4r_load(eng, "fp8_convert.json") == 0,
          "the compiled conversion program did not load");
    ionic_datapath_set_p4_engine(h.dev->ionic_dp, eng);

    /*
     * A spread of fp16 encodings, little-endian as an array in guest
     * memory is written: 1.0, 2.0, 4.0, a NaN, an infinity that must
     * saturate rather than poison the buffer, and a zero.
     */
    {
        static const uint16_t pattern[6] = {0x3C00u, 0x4000u, 0x4400u,
                                            0x7E00u, 0x7C00u, 0x0000u};
        for (uint32_t i = 0; i < P4C_N_ELEMS; i++) {
            uint16_t v = pattern[i % 6u];

            src[2u * i] = (uint8_t)(v & 0xFFu);
            src[2u * i + 1u] = (uint8_t)(v >> 8);
        }
    }
    check(T,
          p4_fmt_convert(P4_FMT_FP16, P4_FMT_FP8_E4M3, src, P4C_N_ELEMS, want),
          "the oracle refused the source buffer");
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the fp16 source");

    /* One gather entry: that is what says "convert", not "reduce". */
    struct ih_sge sge = {.va = FP8R_VA_A, .len = P4C_N_IN, .lkey = mra.lkey};
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        .sge = &sge,
        .nsge = 1,
    };

    /*
     * With the table empty this is an ordinary one-buffer RDMA write, so
     * the raw fp16 bytes arrive in C unchanged. That is the baseline the
     * entry has to change.
     */
    memset(got, 0, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "clear C");
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the plain write did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T, memcmp(got, src, P4C_N_IN) == 0,
          "with no entry the operation was not an ordinary write");

    /* Now the entry: fp16 in, fp8 E4M3 out. */
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.convert");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    up.nparams = 2;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FMT_FP16;
    up.params[1].len = 1;
    up.params[1].value[P4_MATCH_BYTES - 1] = P4_FMT_FP8_E4M3;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the conversion entry was not installed: %s", err);

    memset(got, 0xA5, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "poison C");

    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the conversion did not complete");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
          "read back the converted result");
    check(T, memcmp(got, want, P4C_N_ELEMS) == 0,
          "the fp16 source was not converted elementwise to fp8");

    /*
     * The destination is half the source, so the bytes past the result
     * must be exactly as they were. A device that wrote the source length
     * would have overwritten them, and the comparison above would still
     * have passed.
     */
    {
        bool beyond_intact = true;

        for (uint32_t i = P4C_N_ELEMS; i < P4C_N_IN; i++)
            if (got[i] != 0xA5u) {
                beyond_intact = false;
                break;
            }
        check(T, beyond_intact,
              "the conversion wrote past the end of its result");
    }

    /*
     * A two-entry gather list is not the conversion shape. The extern
     * declines it, so it stays the ordinary gathered write it looks like
     * rather than failing the QP.
     */
    {
        struct ih_sge two[2] = {
            {.va = FP8R_VA_A, .len = 64, .lkey = mra.lkey},
            {.va = FP8R_VA_A + 64, .len = 64, .lkey = mra.lkey},
        };
        struct ih_send w2 = w;

        w2.sge = two;
        w2.nsge = 2;
        memset(got, 0, sizeof(got));
        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "clear C before the two-entry write");
        ih_post_send(&h, &a, &w2);
        check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
              "a two-entry write under the conversion entry was refused");
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "read back the two-entry write");
        check(T, memcmp(got, src, 128) == 0,
              "the two-entry write was not an ordinary gathered transfer");
    }

    ionic_datapath_set_p4_engine(h.dev->ionic_dp, NULL);
    p4_engine_destroy(eng);
    ih_mr_free(&mra);
    ih_mr_free(&mrc);
    teardown(T, &h);
}

#endif /* ERNIC_HAVE_P4 */

static void test_nvmeof(void)
{
    const char *T = "nvmeof";
    struct ih h;
    struct ih_cq gcq, rcq;
    struct ih_qp gsi, rc;
    struct ih_mr mr = {0};
    uint8_t mad[IB_MAD_SIZE], reply[IB_MAD_SIZE];

    if (!setup(T, &h, NV_BACKEND))
        return;

    /* The driver creates QP 1 as UD, and that id is what makes it GSI */
    bool ok = ih_cq_create(&h, &gcq, NV_GSI_CQ, 5) &&
              ih_cq_create(&h, &rcq, NV_RC_CQ, 5) &&
              ih_qp_create(&h, &gsi, 1, IH_QPT_UD, &gcq, &gcq, NULL) &&
              ih_qp_create(&h, &rc, NV_RC_QP, IH_QPT_RC, &rcq, &rcq, NULL) &&
              ih_mr_create(&h, &mr, NV_MR, NV_VA, 2 * IH_PAGE_SIZE,
                           IH_ACCESS_ALL, false);
    if (!check(T, ok, "NVMe-oF setup failed")) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    /* REQ draws a REP that names the controller's QP */
    build_req(mad);
    if (!gsi_exchange(T, &h, &gsi, &gcq, &mr, mad, true, reply)) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }
    check(T, ernic_get_be16(reply + 16) == 0x0013, "the REQ drew attribute %#x",
          ernic_get_be16(reply + 16));
    uint32_t local_comm = ernic_get_be32(reply + 24);
    uint32_t target_qpn = ernic_get_be32(reply + 36) >> 8;
    check(T, nvmeof_cm_is_target_qpn(target_qpn),
          "REP offers QPN %#x, outside the controller range", target_qpn);

    /* RTU completes the handshake and draws nothing */
    mad_hdr(mad, 0x0014, 0x1234567800000001ull);
    ernic_put_be32(mad + 24, 0xabcd0001u);
    ernic_put_be32(mad + 28, local_comm);
    gsi_exchange(T, &h, &gsi, &gcq, &mr, mad, false, reply);

    /* Point the RC QP at the controller and connect the admin queue */
    uint8_t hdr[64];
    size_t hdr_len = ih_roce_hdr_v4(hdr, sizeof(hdr), 0xc0a8c814u, NV_TRADDR);
    ok =
        ih_qp_modify(&h, NV_RC_QP, IH_QP_STATE, IH_QPS_INIT, 0, NULL, 0) == 0 &&
        ih_qp_modify(&h, NV_RC_QP, IH_QP_STATE | IH_QP_AV | IH_QP_DEST_QPN,
                     IH_QPS_RTR, target_qpn, hdr, (uint32_t)hdr_len) == 0 &&
        ih_qp_modify(&h, NV_RC_QP, IH_QP_STATE, IH_QPS_RTS, 0, NULL, 0) == 0;
    check(T, ok, "moving the RC QP to RTS failed");

    uint8_t data[NVMEOF_CONNECT_DATA_SIZE] = {0};
    memcpy(data + NVMEOF_CONNECT_HOSTID_OFF, "0123456789abcdef", 16);
    ernic_put_le16(data + NVMEOF_CONNECT_CNTLID_OFF, 0xffff);
    snprintf((char *)data + NVMEOF_CONNECT_SUBNQN_OFF, 256, "%s", NV_NQN);
    snprintf((char *)data + NVMEOF_CONNECT_HOSTNQN_OFF, 256, "%s",
             "nqn.2014-08.org.nvmexpress:uuid:harness");
    ih_mr_write(&h, &mr, mr.va + NV_CONNECT_OFF, data, sizeof(data));

    uint8_t cap[NVME_SQE_SIZE];
    capsule_init(cap, NVME_ADMIN_FABRICS, 1, mr.va + NV_CONNECT_OFF,
                 sizeof(data), NV_MR);
    cap[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_CONNECT;
    ernic_put_le16(cap + NVMEOF_CONNECT_QID_OFF, 0);
    ernic_put_le16(cap + NVMEOF_CONNECT_SQSIZE_OFF, 31);
    int sts = nvme_exchange(T, &h, &rc, &rcq, &mr, cap);
    check(T, sts == 0, "Connect status %#x", (unsigned)sts);

    /* Identify Controller writes through the keyed SGL into the MR */
    capsule_init(cap, NVME_ADMIN_IDENTIFY, 2, mr.va + NV_IDENT_OFF,
                 NVME_IDENTIFY_SIZE, NV_MR);
    ernic_put_le32(cap + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    sts = nvme_exchange(T, &h, &rc, &rcq, &mr, cap);
    check(T, sts == 0, "Identify status %#x", (unsigned)sts);
    char subnqn[32] = "";
    ih_mr_read(&h, &mr, mr.va + NV_IDENT_OFF + 768, subnqn, sizeof(subnqn) - 1);
    check(T, strcmp(subnqn, NV_NQN) == 0, "Identify subnqn is '%s'", subnqn);

    /* DREQ draws a DREP */
    mad_hdr(mad, 0x0015, 0x1234567800000002ull);
    ernic_put_be32(mad + 24, 0xabcd0001u);
    ernic_put_be32(mad + 28, local_comm);
    ernic_put_be32(mad + 32, target_qpn << 8);
    if (gsi_exchange(T, &h, &gsi, &gcq, &mr, mad, true, reply))
        check(T, ernic_get_be16(reply + 16) == 0x0016,
              "the DREQ drew attribute %#x", ernic_get_be16(reply + 16));

    check(T, ih_qp_destroy(&h, &rc) == 0 && ih_qp_destroy(&h, &gsi) == 0,
          "DESTROY_QP failed");
    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &gcq) == 0 && ih_cq_destroy(&h, &rcq) == 0,
          "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Teardown ---------------------------------------------------------- */

/*
 * Tear down a TCP-backed device with resources still allocated: a manager,
 * so no peer is needed.  Teardown must free what the TCP backend and the
 * resource manager hold for them, which LeakSanitizer checks, and leave no
 * guest memory mapped.
 */

/* A TCP port on loopback that nothing was listening on a moment ago. */
static uint16_t free_tcp_port(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    uint16_t port = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
        port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static void test_teardown(void)
{
    static const char T[] = "teardown";
    char backend[64];
    struct ih h;
    uint32_t pd = 0, cq = 0, qpn = 0, mr = 0;
    uint16_t port = free_tcp_port();

    if (!check(T, port != 0, "no free TCP port"))
        return;
    snprintf(backend, sizeof(backend), "tcp:manager:listen:%u", (unsigned)port);
    if (!check(T, ih_create(&h, backend), "device for '%s' not created",
               backend))
        return;

    PVRDMADev *pvrdma = h.dev->pvrdma_handle;
    bool made =
        check(T, ionic_rm_alloc_pd(pvrdma, &pd) == 0, "no PD") &&
        check(T, ionic_rm_alloc_cq(pvrdma, 16, &cq) == 0, "no CQ") &&
        check(T,
              ionic_rm_alloc_qp(pvrdma, pd, IBV_QPT_RC, 16, 16, cq, cq, &qpn) ==
                  0,
              "no QP") &&
        check(T,
              ionic_rm_alloc_mr(pvrdma, pd, IBV_ACCESS_LOCAL_WRITE, &mr) == 0,
              "no DMA memory region");

    if (made)
        check(T, rdma_rm_get_qp(&pvrdma->rdma_dev_res, qpn) != NULL,
              "QP %u isn't in the resource manager", qpn);

    teardown(T, &h);
}

/* ---- Driver ------------------------------------------------------------ */


static const struct {
    const char *name;
    void (*run)(void);
} groups[] = {
    {"devcmds", test_devcmds},
    {"bringup", test_bringup},
    {"rdma-reset", test_rdma_reset},
    {"admin", test_admin_errors},
    {"rc", test_rc},
    {"atomic", test_atomic_and_reg_mr},
    {"cq-events", test_cq_events},
    {"qp-flush", test_qp_flush},
    {"qp-flush-srq", test_qp_flush_srq},
    {"ud", test_ud},
    {"srq", test_srq},
    {"eth", test_eth},
#ifdef ERNIC_HAVE_P4
    {"p4-port", test_p4_port},
    {"fp8-map", test_fp8_map},
    {"p4-rdma", test_p4_rdma},
    {"p4-cvt", test_p4_cvt},
    {"p4-devcmd", test_p4_devcmd},
#endif
    {"nvmeof", test_nvmeof},
    {"teardown", test_teardown},
};

int main(int argc, char **argv)
{
    bool ran = false;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [group]\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        if (argc == 2 && strcmp(argv[1], groups[i].name) != 0)
            continue;
        groups[i].run();
        ran = true;
    }
    if (!ran) {
        fprintf(stderr, "%s: no test group '%s'\n", argv[0], argv[1]);
        return 2;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all ionic harness tests passed\n");
    return 0;
}
