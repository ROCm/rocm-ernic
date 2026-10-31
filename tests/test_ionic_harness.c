/*
 * test_ionic_harness.c — the ionic device end to end, in-process
 *
 * Every other ionic test reaches into one translation unit and stubs the
 * rest. These link the whole device -- the Ethernet emulator, the RDMA
 * devcmds, the admin queue, the data path, the compat bridge, the resource
 * manager and the none, loopback and TCP backends -- and drive it the way
 * ionic.ko and ionic_rdma.ko do, through the model in ionic_harness.c. Only
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
#include <glib.h>
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
#include <syslog.h>
#include <unistd.h>

#include "byteorder.h"
#include "ernic_attributes.h"
#include "ernic_limits.h"
#include "ernic_mesh.h"
#include "ernic_rm.h"
#include "ernic_stats.h"
#include "fake_vfu.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_datapath_nvmeof.h"
#include "ionic/ionic_device.h"
#include "ionic/ionic_eth_emu.h"
/* For the emulator's admin queue pointer, which rdma-reset checks */
#include "ionic/ionic_eth_emu_priv.h"
#include "ionic/ionic_fault.h"
#include "ionic_harness.h"
#include "ionic/ionic_profile.h"
#include "ionic/ionic_rdma_devcmd.h"
#include "ionic/ionic_status.h"
#include "nvmeof/nvmeof.h"
#include "nvmeof/nvmeof_cm.h"
#include "nvmeof/nvmeof_target.h"
#ifdef ERNIC_HAVE_P4
#include "ernic_log.h"
#include "ionic/ionic_datapath_p4.h"
#include "ionic/ionic_datapath_priv.h"
#include "ionic/ionic_eth_p4.h"
#include "p4/p4_devcmd.h"
#include "p4/p4_engine.h"
#include "p4/p4_fp8.h"
#include "p4/p4_fmt.h"
#include "p4/p4_program.h"
#include "p4/p4_table.h"
#include "s3_alloc_fail.h"
#endif
#include "pvrdma/pvrdma.h"
#include "pvrdma/rdma_backend.h"
#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_rm.h"
#include "rdma/rdma_backend_ops.h"
#include "rocm_ernic_compat.h"
/*
 * struct rocm_ernic_dev.  IWYU credits uses of it to the repeated
 * rocm_ernic_dev_t typedef in rocm_ernic_compat.h, so it would drop this.
 */
#include "rocm_ernic_internal.h" // IWYU pragma: keep
#include "s3/s3_token.h"

static int failures;

static void check_failed(const char *test, const char *fmt, ...)
    ERNIC_PRINTF_FORMAT(2, 3);

/* Report and count a failed check. */
static void check_failed(const char *test, const char *fmt, ...)
{
    va_list ap;

    printf("FAIL %-12s: ", test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures++;
}

/*
 * Give whether @cond holds, reporting it if not. A macro rather than a
 * function: the static analyzer doesn't follow a variadic function, so it
 * couldn't see that this gives @cond and would report paths where a check
 * that failed went on as if it had passed.
 */
#define check(test, cond, ...) \
    ((cond) ? true : (check_failed((test), __VA_ARGS__), false))

/* ---- Helpers ----------------------------------------------------------- */

static bool all_zero(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i]) {
            return false;
        }
    }
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

    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ernic_get_be16(p + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/* A byte pattern that differs from row to row and from byte to byte. */
static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + (i * 7) + (i >> 8));
    }
}

/* Whether @cq has no completion waiting. */
static bool cq_empty(struct ih *h, struct ih_cq *cq)
{
    struct ih_cqe e;

    return !ih_cq_poll(h, cq, &e);
}

/* Poll @n completions from @cq, leaving the last in @e. */
static bool cq_poll_n(struct ih *h, struct ih_cq *cq, unsigned n,
                      struct ih_cqe *e)
{
    for (unsigned i = 0; i < n; i++) {
        if (!ih_cq_poll(h, cq, e)) {
            return false;
        }
    }
    return true;
}

/* Drain the EQ, counting the events for @cq_id. */
static unsigned eq_count(struct ih *h, uint32_t cq_id)
{
    unsigned n = 0;
    uint32_t id;

    while (ih_eq_next(h, &id)) {
        n += id == cq_id;
    }
    return n;
}

static bool setup(const char *test, struct ih *h, const char *backend)
{
    if (!check(test, ih_create(h, backend), "device for '%s' not created",
               backend)) {
        return false;
    }
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
#define DEVID_NLIFS          0x08U
#define DEVID_NINTRS         0x0cU
#define DEVID_COAL_DIV       0x18U
#define LIFID_MAC            0x0a0U
#define LIFID_RDMA           392U
#define LIFID_RDMA_ADMIN_OPS 2U

static void test_devcmds(void)
{
    const char *T = "devcmds";
    struct ih h;
    uint8_t cmd[64];
    uint8_t comp[16];
    uint8_t data[1912];

    if (!check(T, ih_create(&h, "loopback"), "device not created")) {
        return;
    }

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
    check(T, (data[0] & 3U) == 3U, "LIF capabilities lack ETH or RDMA");
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
    for (size_t i = 0; i < sizeof(qtypes) / sizeof(qtypes[0]); i++) {
        check(T, rdma[qtypes[i].off] == qtypes[i].qtype,
              "qtype at rdma+%u is %u, expected %u", qtypes[i].off,
              rdma[qtypes[i].off], qtypes[i].qtype);
    }
    check(T, ernic_get_le32(rdma + 100) == ERNIC_MAX_SRQ, "srq qid_count is %u",
          ernic_get_le32(rdma + 100));
    check(T, rdma[113] & 2U, "SRQ qid allocation not advertised");
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
    check(T, ionic_eth_emu_bar0_access(h.emu, b, 8, 0x8000 - 4, false) < 0,
          "BAR0 read past the register window succeeded");
    /* So do ones that only fit once the offset is converted to a size_t
     * and the count added to it, which wraps */
    check(T, ionic_eth_emu_bar0_access(h.emu, b, 8, -8, false) < 0,
          "BAR0 read at a negative offset succeeded");
    check(T, ionic_eth_emu_bar0_access(h.emu, b, SIZE_MAX - 7, 8, false) < 0,
          "BAR0 read of a count that wraps the window succeeded");
    check(T, ionic_eth_emu_bar2_access(h.emu, b, 8, -8, true) < 0,
          "BAR2 write at a negative offset succeeded");
    check(T,
          ionic_eth_emu_bar2_access(h.emu, b, 8, IONIC_BAR2_DB_SIZE, true) < 0,
          "BAR2 write past the doorbell window succeeded");
    check(T, ionic_eth_emu_bar2_access(h.emu, b, 8, 0, false) == 8,
          "BAR2 read failed");

    teardown(T, &h);
}

static void test_bringup(void)
{
    const char *T = "bringup";
    struct ih h;
    uint8_t buf[64];
    uint8_t cmd[64];
    uint8_t comp[16];

    if (!setup(T, &h, "loopback")) {
        return;
    }

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
    const uint32_t intr = 0x2000U + (IH_ETH_ADMINQ_INTR * 32U);
    ih_bar0_write32(&h, intr + 12, 1);
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq NOP failed");
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "adminq NOP failed");
    check(T, fake_vfu_irq_count(h.vfu, IH_ETH_ADMINQ_INTR) == irqs + 2,
          "mask-on-assert did not mask the vector");
    ih_bar0_write32(&h, intr + 8, 0x10000U);
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
    uint8_t cmd[64];
    uint8_t comp[16];

    if (!setup(T, &h, "loopback")) {
        return;
    }

    const struct ionic_eth_emu *emu = h.emu;
    struct ionic_rdma_devcmd_state *rdma = h.rdma;

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
                  "CREATE_CQ on the new admin queue failed")) {
            check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
        }
    }

    teardown(T, &h);
}

/* Run STATS_VALS into the scratch page; 0, or the completion's status */
static int stats_vals(struct ih *h)
{
    uint8_t body[32];

    /* le64 dma, le32 length, le32 id_ver */
    memset(body, 0, sizeof(body));
    ernic_put_le64(body, h->scratch);
    ernic_put_le32(body + 8, IH_PAGE_SIZE);
    return ih_admin(h, IH_ADMIN_STATS_VALS, body, sizeof(body), NULL);
}

/*
 * The admin queue context RDMA_RESET_LIF frees is made again by the next
 * CREATE_ADMINQ, and has to inject the faults the first one did, with
 * nothing in the main loop to hand them over again.
 */
static void test_rdma_reset_faults(void)
{
    const char *T = "rdma-reset-faults";
    struct ionic_fault_set *faults = ionic_fault_set_new();
    char err[128] = "";
    const struct ionic_fault *f;
    uint8_t cmd[64];
    uint8_t comp[16];
    struct ih h;
    int rc;

    if (!check(T,
               faults && ionic_fault_set_add(faults,
                                             "stats_vals: every 1 effect fail",
                                             err, sizeof(err)) == 0,
               "fault not made: %s", err) ||
        !check(T, ih_create_faults(&h, "loopback", faults),
               "device not created")) {
        ionic_fault_set_free(faults);
        return;
    }
    if (!check(T, ih_bringup(&h), "bring-up failed")) {
        goto out;
    }

    rc = stats_vals(&h);
    check(T, rc == IONIC_RC_EIO, "STATS_VALS returned %d, not the fault's %d",
          rc, IONIC_RC_EIO);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IH_CMD_RDMA_RESET;
    if (!check(T, ih_eth_admin(&h, cmd, comp) == 0, "RDMA_RESET_LIF failed") ||
        !check(T, ih_bringup(&h), "bring-up after RDMA_RESET_LIF failed")) {
        goto out;
    }

    rc = stats_vals(&h);
    check(T, rc == IONIC_RC_EIO,
          "STATS_VALS on the new admin queue returned %d, not the fault's %d",
          rc, IONIC_RC_EIO);

    f = ionic_fault_set_get(faults, IONIC_FAULT_SITE_STATS_VALS);
    check(T, f && f->fired == 2, "the fault fired %" PRIu64 " times, not 2",
          f ? f->fired : 0);

out:
    teardown(T, &h);
    ionic_fault_set_free(faults);
}

/* ---- RDMA admin commands that must be refused -------------------------- */

struct bad_admin {
    const char *what;
    uint8_t op;
    uint16_t len;
    void (*build)(uint8_t *body);
    int want; /* completion status */
};

/* Every row's build has this signature, though this one writes nothing */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
static void build_none(uint8_t *body)
{
    (void)body;
}

static void build_qp_unknown_cq(uint8_t *body)
{
    ernic_put_le32(body + 8, 4000);
    ernic_put_le32(body + 32, 4001);
    ernic_put_le32(body + 40, 0xffffffffU);
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
    ernic_put_le32(body + 0, 0x00777777U);
    ernic_put_le32(body + 28, 0x00777777U);
    ernic_put_le32(body + 56, 0x00777777U);
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

/* A QP's ring geometry, for build_qp() */
struct qp_rings {
    uint8_t sq_depth_log2, sq_stride_log2;
    uint8_t rq_depth_log2, rq_stride_log2;
};

/* Rings the device takes, for a build_*() to change one thing in */
static const struct qp_rings good_qp_rings = {
    .sq_depth_log2 = 4,
    .sq_stride_log2 = 7,
    .rq_depth_log2 = 4,
    .rq_stride_log2 = 7,
};

/* CREATE_QP for QP 3 on CQ 1, with an RQ of its own */
static void build_qp(uint8_t *body, const struct qp_rings *r)
{
    ernic_put_le32(body + 8, 1);
    body[12] = r->sq_depth_log2;
    body[13] = r->sq_stride_log2;
    ernic_put_le32(body + 32, 1);
    body[36] = r->rq_depth_log2;
    body[37] = r->rq_stride_log2;
    ernic_put_le32(body + 40, 0xffffffffU);
    ernic_put_le32(body + 56, 3);
}

static void build_qp_sq_depth_32(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.sq_depth_log2 = 32;
    build_qp(body, &r);
}

static void build_qp_sq_stride_32(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.sq_stride_log2 = 32;
    build_qp(body, &r);
}

static void build_qp_sq_stride_4(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.sq_stride_log2 = 4;
    build_qp(body, &r);
}

static void build_qp_sq_stride_10(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.sq_stride_log2 = 10;
    build_qp(body, &r);
}

static void build_qp_rq_stride_10(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.rq_stride_log2 = 10;
    build_qp(body, &r);
}

static void build_qp_rq_depth_32(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.rq_depth_log2 = 32;
    build_qp(body, &r);
}

static void build_qp_rq_stride_4(uint8_t *body)
{
    struct qp_rings r = good_qp_rings;

    r.rq_stride_log2 = 4;
    build_qp(body, &r);
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
    {"unknown opcode", 77, 0, build_none, IONIC_RC_EOPCODE},
    {"short CREATE_CQ", IH_ADMIN_CREATE_CQ, 10, build_none, IONIC_RC_EINVAL},
    {"short CREATE_QP", IH_ADMIN_CREATE_QP, 60, build_none, IONIC_RC_EINVAL},
    {"CREATE_QP on unknown CQs", IH_ADMIN_CREATE_QP, 64, build_qp_unknown_cq,
     IONIC_RC_ENOENT},
    {"CREATE_QP on an unknown SRQ", IH_ADMIN_CREATE_QP, 64,
     build_qp_unknown_srq, IONIC_RC_ENOENT},
    {"CREATE_CQ 2^32 deep", IH_ADMIN_CREATE_CQ, 34, build_cq_depth_32,
     IONIC_RC_ERANGE},
    {"CREATE_CQ deeper than 2^24", IH_ADMIN_CREATE_CQ, 34, build_cq_depth_25,
     IONIC_RC_ERANGE},
    {"CREATE_CQ with a stride below a CQE", IH_ADMIN_CREATE_CQ, 34,
     build_cq_stride_4, IONIC_RC_ERANGE},
    {"CREATE_CQ with a stride above 2^16", IH_ADMIN_CREATE_CQ, 34,
     build_cq_stride_17, IONIC_RC_ERANGE},
    {"CREATE_QP with an SQ 2^32 deep", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_depth_32, IONIC_RC_ERANGE},
    {"CREATE_QP with an SQ stride of 2^32", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_stride_32, IONIC_RC_ERANGE},
    {"CREATE_QP with an SQ stride below a WQE header", IH_ADMIN_CREATE_QP, 64,
     build_qp_sq_stride_4, IONIC_RC_ERANGE},
    {"CREATE_QP with an SQ stride above the advertised 2^9", IH_ADMIN_CREATE_QP,
     64, build_qp_sq_stride_10, IONIC_RC_ERANGE},
    {"CREATE_QP with an RQ stride above the advertised 2^9", IH_ADMIN_CREATE_QP,
     64, build_qp_rq_stride_10, IONIC_RC_ERANGE},
    {"CREATE_QP with an RQ 2^32 deep", IH_ADMIN_CREATE_QP, 64,
     build_qp_rq_depth_32, IONIC_RC_ERANGE},
    {"CREATE_QP with an RQ stride below a WQE header", IH_ADMIN_CREATE_QP, 64,
     build_qp_rq_stride_4, IONIC_RC_ERANGE},
    {"DESTROY_MR of an unknown MR", IH_ADMIN_DESTROY_MR, 4, build_unknown_id,
     IONIC_RC_ENOENT},
    {"MODIFY_QP of an unknown QP", IH_ADMIN_MODIFY_QP, 60, build_unknown_id,
     IONIC_RC_ENOENT},
    {"QUERY_QP of an unknown QP", IH_ADMIN_QUERY_QP, 34, build_unknown_id,
     IONIC_RC_ENOENT},
    {"short CREATE_AH", IH_ADMIN_CREATE_AH, 8, build_none, IONIC_RC_EINVAL},
    {"short DESTROY_AH", IH_ADMIN_DESTROY_AH, 2, build_none, IONIC_RC_EINVAL},
    {"short CREATE_SRQ", IH_ADMIN_CREATE_SRQ, 20, build_none, IONIC_RC_EINVAL},
    {"CREATE_SRQ deeper than advertised", IH_ADMIN_CREATE_SRQ, 42,
     build_srq_too_deep, IONIC_RC_ERANGE},
    {"CREATE_SRQ with no response buffer", IH_ADMIN_CREATE_SRQ, 42,
     build_srq_no_resp, IONIC_RC_EINVAL},
    {"short MODIFY_SRQ", IH_ADMIN_MODIFY_SRQ, 4, build_none, IONIC_RC_EINVAL},
    {"MODIFY_SRQ of an unknown SRQ", IH_ADMIN_MODIFY_SRQ, 6, build_unknown_id,
     IONIC_RC_ENOENT},
    {"short QUERY_SRQ", IH_ADMIN_QUERY_SRQ, 2, build_none, IONIC_RC_EINVAL},
    {"QUERY_SRQ of an unknown SRQ", IH_ADMIN_QUERY_SRQ, 4, build_unknown_id,
     IONIC_RC_ENOENT},
    {"short DESTROY_SRQ", IH_ADMIN_DESTROY_SRQ, 2, build_none, IONIC_RC_EINVAL},
    {"short STATS_HDRS", IH_ADMIN_STATS_HDRS, 0, build_none, IONIC_RC_EINVAL},
    /* One byte short of each body; these used to succeed doing nothing */
    {"short CREATE_MR", IH_ADMIN_CREATE_MR, 44, build_none, IONIC_RC_EINVAL},
    {"short DESTROY_MR", IH_ADMIN_DESTROY_MR, 3, build_none, IONIC_RC_EINVAL},
    {"short DESTROY_CQ", IH_ADMIN_DESTROY_CQ, 3, build_none, IONIC_RC_EINVAL},
    {"short DESTROY_QP", IH_ADMIN_DESTROY_QP, 3, build_none, IONIC_RC_EINVAL},
    {"short MODIFY_QP", IH_ADMIN_MODIFY_QP, 59, build_none, IONIC_RC_EINVAL},
    {"short QUERY_QP", IH_ADMIN_QUERY_QP, 33, build_none, IONIC_RC_EINVAL},
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

/* An RDMA queue devcmd, for rdma_queue_cmd() */
struct rdma_queue {
    uint8_t op;
    uint8_t depth_log2, stride_log2;
    /* CREATE_EQ's interrupt, or an admin CREATE_CQ's EQ: 0 unless given */
    uint32_t cid;
};

/* Queues the device takes */
static const struct rdma_queue good_eq = {
    .op = IH_CMD_RDMA_EQ, .depth_log2 = 4, .stride_log2 = 2};
static const struct rdma_queue good_admin_cq = {
    .op = IH_CMD_RDMA_CQ, .depth_log2 = 6, .stride_log2 = 5, .cid = IH_EQ_ID};
static const struct rdma_queue good_adminq = {
    .op = IH_CMD_RDMA_ADMINQ, .depth_log2 = 6, .stride_log2 = 6};

/* RDMA queue devcmds, sent over the Ethernet admin queue, whose geometry
 * must be refused */
struct bad_queue {
    const char *what;
    struct rdma_queue q;
};

static const struct bad_queue bad_queues[] = {
    {"CREATE_EQ 2^32 deep", {IH_CMD_RDMA_EQ, 32, 2, 0}},
    {"CREATE_EQ deeper than 2^24", {IH_CMD_RDMA_EQ, 25, 2, 0}},
    {"CREATE_EQ with a stride below an EQE", {IH_CMD_RDMA_EQ, 4, 1, 0}},
    {"CREATE_EQ with a stride above 2^16", {IH_CMD_RDMA_EQ, 4, 17, 0}},
    /* An interrupt past the MSI-X table, which past INT_MAX was also
     * converted to a negative int when the EQ was raised */
    {"CREATE_EQ on the interrupt after the last",
     {IH_CMD_RDMA_EQ, 4, 2, IONIC_MSIX_MAX_VECTORS}},
    {"CREATE_EQ on interrupt 2^31", {IH_CMD_RDMA_EQ, 4, 2, 0x80000000U}},
    {"admin CREATE_CQ 2^32 deep", {IH_CMD_RDMA_CQ, 32, 5, 0}},
    /* Sent after a good admin CREATE_CQ, so only its depth is wrong */
    {"CREATE_ADMINQ 2^32 deep", {IH_CMD_RDMA_ADMINQ, 32, 6, 0}},
    /* Deeper than its 16-bit producer index and cmd_idx can count */
    {"CREATE_ADMINQ 2^17 deep", {IH_CMD_RDMA_ADMINQ, 17, 6, 0}},
};

/* Queue id 5 is clear of the harness's own EQ and admin queues. */
static int rdma_queue_cmd(struct ih *h, const struct rdma_queue *q)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16];

    cmd[0] = q->op;
    ernic_put_le32(cmd + 4, 5U | 1U << 24);
    ernic_put_le32(cmd + 8, q->cid);
    cmd[14] = q->depth_log2;
    cmd[15] = q->stride_log2;
    return ih_eth_admin(h, cmd, comp);
}

static void test_queue_geometry_errors(const char *T, struct ih *h)
{
    for (size_t i = 0; i < sizeof(bad_queues) / sizeof(bad_queues[0]); i++) {
        const struct bad_queue *c = &bad_queues[i];

        if (c->q.op == IH_CMD_RDMA_ADMINQ) {
            check(T, rdma_queue_cmd(h, &good_admin_cq) == 0,
                  "admin CREATE_CQ failed");
        }
        int rc = rdma_queue_cmd(h, &c->q);
        check(T, rc == 1, "%s: status %d, expected 1", c->what, rc);
    }

    /* A refused CREATE_EQ takes none of the device's 32 EQ slots */
    for (int i = 0; i < 40; i++) {
        (void)rdma_queue_cmd(h, &(struct rdma_queue){.op = IH_CMD_RDMA_EQ,
                                                     .depth_log2 = 32,
                                                     .stride_log2 = 2});
    }
    check(T, rdma_queue_cmd(h, &good_eq) == 0,
          "CREATE_EQ failed after refused ones");
    /* The last interrupt in the table is one an EQ may raise */
    struct rdma_queue last = good_eq;

    last.cid = IONIC_MSIX_MAX_VECTORS - 1U;
    check(T, rdma_queue_cmd(h, &last) == 0,
          "CREATE_EQ on the last interrupt failed");
}

static void test_admin_errors(void)
{
    const char *T = "admin";
    struct ih h;
    struct ih_cq cq;

    if (!setup(T, &h, "loopback")) {
        return;
    }
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
    uint32_t aq_depth = 1U << h.aq.depth_log2;
    while (h.aq.prod % aq_depth != aq_depth - 1) {
        if (!check(T, ih_admin(&h, IH_ADMIN_NOOP, NULL, 0, NULL) == 0,
                   "NOOP failed")) {
            break;
        }
    }
    struct ih_qp qp;
    if (check(T, ih_qp_create(&h, &qp, 20, IH_QPT_UD, &cq, &cq, NULL),
              "CREATE_QP wrapping the admin queue failed")) {
        check(T, h.aq.prod % aq_depth == 1, "CREATE_QP took %u strides",
              (h.aq.prod % aq_depth) + 1);
        check(T, ih_qp_destroy(&h, &qp) == 0, "DESTROY_QP failed");
    }

    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Guest memory the device can't reach -------------------------------- */

/*
 * fake_vfu_fail_nth_map(n) refuses the nth mapping from then on, as
 * libvfio-user refuses an address the client has unmapped. Each DMA here
 * maps once, so the rows below name the DMA they refuse by where it falls in
 * the operation: for an admin command, the WQE is the first and its
 * completion the last.
 */
static void test_admin_dma_faults(void)
{
    const char *T = "admin-dma";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp qp;
    struct ih_srq srq;

    if (!setup(T, &h, "loopback")) {
        return;
    }
    if (!check(T,
               ih_cq_create(&h, &cq, 1, 4) &&
                   ih_qp_create(&h, &qp, 3, IH_QPT_UD, &cq, &cq, NULL),
               "CQ and QP setup failed")) {
        teardown(T, &h);
        return;
    }

    /* A completion the device can't write is lost, and the next command's
     * takes its place */
    int slot = ih_admin_post(&h, IH_ADMIN_NOOP, NULL, 0);
    fake_vfu_fail_nth_map(h.vfu, 2);
    ih_poll(&h);
    check(T,
          ih_admin_complete(&h, IH_ADMIN_NOOP, slot, NULL) == IH_ADMIN_PENDING,
          "a completion the device could not write arrived");
    check(T, ih_admin(&h, IH_ADMIN_NOOP, NULL, 0, NULL) == 0,
          "NOOP after a lost completion failed");

    /* The second stride of a WQE, like the first, is read again next poll */
    uint8_t body[100] = {0};
    slot = ih_admin_post(&h, IH_ADMIN_NOOP, body, sizeof(body));
    fake_vfu_fail_nth_map(h.vfu, 2);
    ih_poll(&h);
    check(T,
          ih_admin_complete(&h, IH_ADMIN_NOOP, slot, NULL) == IH_ADMIN_PENDING,
          "a WQE whose second stride could not be read completed");
    ih_poll(&h);
    check(T, ih_admin_complete(&h, IH_ADMIN_NOOP, slot, NULL) == 0,
          "a two-stride WQE was not retried");

    /* QUERY_QP refuses when either answer can't be written */
    for (unsigned fail = 2; fail <= 3; fail++) {
        memset(body, 0, sizeof(body));
        ernic_put_le64(body + 8, h.scratch + 1024);
        ernic_put_le64(body + 16, h.scratch + 1088);
        ernic_put_le32(body + 28, qp.id);
        fake_vfu_fail_nth_map(h.vfu, fail);
        check(T,
              ih_admin(&h, IH_ADMIN_QUERY_QP, body, 34, NULL) == IONIC_RC_EIO,
              "QUERY_QP with DMA %u refused succeeded", fail);
    }

    /* A RoCE header the device can't read leaves an AH with no address */
    fake_vfu_fail_nth_map(h.vfu, 2);
    check(T, ih_ah_create(&h, 5, 0xc0a80001U, 0xc0a80002U) == 0,
          "CREATE_AH with an unreadable header failed");

    /* CREATE_SRQ fails when its response can't be written... */
    fake_vfu_fail_nth_map(h.vfu, 2);
    check(T, ih_srq_create(&h, &srq, 4) == IONIC_RC_EIO,
          "CREATE_SRQ with an unwritable response succeeded");

    /* ...or when its page table can't be read. Two 4 KiB pages hold
     * 2^6 128-byte WQEs. */
    memset(body, 0, sizeof(body));
    ernic_put_le64(body + 0, h.scratch);
    ernic_put_le32(body + 8, 2);
    body[26] = 6;
    body[27] = 7;
    body[28] = 12;
    ernic_put_le32(body + 30, 5);
    ernic_put_le64(body + 34, h.scratch + 512);
    fake_vfu_fail_nth_map(h.vfu, 2);
    check(T, ih_admin(&h, IH_ADMIN_CREATE_SRQ, body, 42, NULL) == IONIC_RC_EIO,
          "CREATE_SRQ with an unreadable page table succeeded");
    if (check(T, ih_srq_create(&h, &srq, 4) == 0,
              "CREATE_SRQ after refused ones failed")) {
        check(T, ih_srq_destroy(&h, &srq) == 0, "DESTROY_SRQ failed");
    }

    /*
     * CREATE_CQ and CREATE_QP fail too when their page table can't be
     * read, since the queue would have no ring. Each leaves nothing behind:
     * the resource manager holds no more than before, and the id can be
     * created again. CREATE_QP's page table is its third DMA, after both of
     * the WQE's strides.
     */
    const PVRDMADev *pvrdma = h.dev->pvrdma_handle;
    const RdmaDeviceResources *res = &pvrdma->rdma_dev_res;
    const uint32_t cqs = res->cq_tbl.used;
    const uint32_t qps = res->qp_tbl.used;

    memset(body, 0, sizeof(body));
    build_cq(body, 8, 5);
    ernic_put_le32(body + 20, 2);
    ernic_put_le64(body + 24, h.scratch);
    fake_vfu_fail_nth_map(h.vfu, 2);
    check(T, ih_admin(&h, IH_ADMIN_CREATE_CQ, body, 34, NULL) == IONIC_RC_EIO,
          "CREATE_CQ with an unreadable page table succeeded");
    check(T, res->cq_tbl.used == cqs, "the refused CREATE_CQ kept its CQ");
    check(T, ih_admin(&h, IH_ADMIN_CREATE_CQ, body, 34, NULL) == 0,
          "CREATE_CQ of the refused CQ's id failed");
    ernic_put_le32(body, 2);
    check(T, ih_admin(&h, IH_ADMIN_DESTROY_CQ, body, 4, NULL) == 0,
          "DESTROY_CQ of that CQ failed");

    struct qp_rings rings = good_qp_rings;

    rings.sq_depth_log2 = 6;
    memset(body, 0, sizeof(body));
    build_qp(body, &rings);
    ernic_put_le32(body + 20, 2);
    ernic_put_le64(body + 24, h.scratch);
    ernic_put_le32(body + 56, 4);
    fake_vfu_fail_nth_map(h.vfu, 3);
    check(T, ih_admin(&h, IH_ADMIN_CREATE_QP, body, 64, NULL) == IONIC_RC_EIO,
          "CREATE_QP with an unreadable page table succeeded");
    check(T, res->qp_tbl.used == qps, "the refused CREATE_QP kept its QP");
    check(T, ih_admin(&h, IH_ADMIN_CREATE_QP, body, 64, NULL) == 0,
          "CREATE_QP of the refused QP's id failed");
    ernic_put_le32(body, 4);
    check(T, ih_admin(&h, IH_ADMIN_DESTROY_QP, body, 4, NULL) == 0,
          "DESTROY_QP of that QP failed");

    check(T, fake_vfu_mapped(h.vfu) == 0, "guest memory left mapped");
    check(T, ih_qp_destroy(&h, &qp) == 0, "DESTROY_QP failed");
    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
    teardown(T, &h);
}

/* ---- Connected QPs on the loopback backend ----------------------------- */

#define RC_CQ_A 1
#define RC_CQ_B 2
#define RC_QP_A 10
#define RC_QP_B 11
#define RC_QP_C 12
#define MR_A    IH_MRID(1, 0x01)
#define MR_B    IH_MRID(2, 0x02)
#define VA_A    UINT64_C(0x00007f0000000100)
#define VA_B    UINT64_C(0x00007f1000000000)
#define MR_LEN  (UINT64_C(3) * IH_PAGE_SIZE)

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
    if (!setup(test, &r->h, "loopback")) {
        return false;
    }

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
               what)) {
        return;
    }
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
#define SRC_OFF 0x0f0U
#define DST_OFF 0x123U

/* The receive completion's op for a row's send @op */
static uint8_t rc_recv_op(uint8_t op)
{
    switch (op) {
    case IH_OP_SEND_IMM:
        return IH_RECV_OP_SEND_IMM;
    case IH_OP_SEND_INV:
        return IH_RECV_OP_SEND_INV;
    case IH_OP_RDMA_WRITE_IMM:
        return IH_RECV_OP_RDMA_IMM;
    default:
        return IH_RECV_OP_SEND;
    }
}

static void run_rc_case(const char *test, struct rc *r, const struct rc_case *c,
                        uint8_t seed)
{
    struct ih *h = &r->h;
    uint8_t want[8192];
    uint8_t got[8192];
    struct ih_sge src[6];
    struct ih_sge dst[6];
    const uint32_t imm_be = htobe32(0xc0ffee00U + seed);
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
    if (takes_recv) {
        recv_id = ih_post_recv(h, &r->b, dst,
                               split_sges(dst, c->ndst, to_va, cap, MR_B));
    }

    struct ih_send s = {
        .op = c->op,
        .flags = IH_WQE_SIGNALLED,
        .imm_be = imm_be,
        .key = 0x0a0b0c0dU,
        .remote_va = is_read ? from_va : to_va,
        .rkey = c->bad_rkey ? 0x7e000099U : MR_B,
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
            uint8_t op = rc_recv_op(c->op);
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
            if (c->recv_status) {
                check(test, e.error && e.status_len == c->recv_status,
                      "%s: receive status %u (error %d), expected %u", c->what,
                      e.status_len, e.error, c->recv_status);
            } else {
                check(test, !e.error && e.status_len == c->len,
                      "%s: receive length %u (error %d), expected %u", c->what,
                      e.status_len, e.error, c->len);
            }
            if (op == IH_RECV_OP_SEND_IMM || op == IH_RECV_OP_RDMA_IMM) {
                check(test, e.imm_be == imm_be, "%s: immediate data lost",
                      c->what);
            }
        }
    }
    check(test, cq_empty(h, &r->cqb), "%s: an extra completion on B", c->what);
    check(test, fake_vfu_mapped(h->vfu) == 0,
          "%s: guest memory left mapped after the transfer", c->what);

    if (c->send_status || c->recv_status) {
        return;
    }
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
#define WIDE_NSGE (((1U << IH_WQE_MAX_STRIDE_LOG2) - 32U) / 16U)

static void widest_wqe_send(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    struct ih_cq cqc;
    struct ih_cq cqd;
    struct ih_qp c;
    struct ih_qp d;
    struct ih_cqe e;

    memset(&e, 0, sizeof(e));
    bool ok = ih_cq_create(h, &cqc, WIDE_CQ_C, 6) &&
              ih_cq_create(h, &cqd, WIDE_CQ_D, 6) &&
              ih_qp_create_stride(h, &c, WIDE_QP_C, IH_QPT_RC, &cqc, &cqc, NULL,
                                  IH_WQE_MAX_STRIDE_LOG2) &&
              ih_qp_create(h, &d, WIDE_QP_D, IH_QPT_RC, &cqd, &cqd, NULL) &&
              ih_qp_connect(h, &c, &d);
    if (!check(T, ok, "widest-WQE setup failed")) {
        return;
    }

    uint8_t want[WIDE_NSGE];
    uint8_t got[WIDE_NSGE];
    struct ih_sge sge[WIDE_NSGE];
    fill(want, sizeof(want), 0x77);
    memset(got, 0, sizeof(got));
    ih_mr_write(h, &r->mra, VA_A, want, sizeof(want));
    ih_mr_write(h, &r->mrb, VA_B, got, sizeof(got));
    for (uint32_t i = 0; i < WIDE_NSGE; i++) {
        sge[i] = (struct ih_sge){.va = VA_A + i, .len = 1, .lkey = MR_A};
    }

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

/* What QUERY_QP reports about a QP, from the side buffers the driver reads */
struct qp_answer {
    uint8_t state;
    uint32_t dest;
    uint32_t rq_psn;
    uint32_t sq_psn;
};

static bool query_qp(const char *test, struct ih *h, uint32_t qp_id,
                     struct qp_answer *a)
{
    uint8_t body[34] = {0};
    uint8_t sqb[20];
    uint8_t rqb[12];
    const uint64_t sq_buf = h->scratch + 1024;
    const uint64_t rq_buf = h->scratch + 1088;

    ernic_put_le64(body + 8, sq_buf);
    ernic_put_le64(body + 16, rq_buf);
    ernic_put_le32(body + 28, qp_id);
    if (!check(test,
               ih_admin(h, IH_ADMIN_QUERY_QP, body, sizeof(body), NULL) == 0,
               "QUERY_QP %u failed", qp_id)) {
        return false;
    }
    fake_vfu_read(h->vfu, sq_buf, sqb, sizeof(sqb));
    fake_vfu_read(h->vfu, rq_buf, rqb, sizeof(rqb));
    a->state = rqb[0] >> 4;
    a->dest = ernic_get_be32(sqb + 8);
    a->rq_psn = ernic_get_be32(sqb + 16);
    a->sq_psn = ernic_get_be32(rqb + 4);
    return true;
}

static void test_rc(void)
{
    const char *T = "rc";
    struct rc r;
    struct ih *h = &r.h;
    struct qp_answer qa;

    if (!rc_setup(T, &r)) {
        return;
    }

    /* QUERY_QP reports the state, and the peer by the driver's own qpid */
    if (query_qp(T, h, RC_QP_A, &qa)) {
        check(T, qa.state == IH_QPS_RTS, "QUERY_QP state %u, not RTS",
              qa.state);
        check(T, qa.dest == RC_QP_B, "QUERY_QP destination %u, not QP %u",
              qa.dest, (unsigned)RC_QP_B);
    }

    for (size_t i = 0; i < sizeof(rc_cases) / sizeof(rc_cases[0]); i++) {
        run_rc_case(T, &r, &rc_cases[i], (uint8_t)((i * 17) + 3));
    }

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
    uint8_t want[3000];
    uint8_t got[3000];
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
        uint8_t v = (uint8_t)i;
        uint8_t back = 0;
        ih_mr_write(h, &r.mra, VA_A + 8, &v, 1);
        struct ih_sge one_byte = {.va = VA_A + 8, .len = 1, .lkey = MR_A};
        struct ih_sge into = {.va = VA_B + 8, .len = 1, .lkey = MR_B};
        ih_post_recv(h, &r.b, &into, 1);
        s = (struct ih_send){.op = IH_OP_SEND, .sge = &one_byte, .nsge = 1};
        ih_post_send(h, &r.a, &s);
        expect_send(T, "SEND while wrapping", &r, 0);
        if (!check(T, ih_cq_poll(h, &r.cqb, &e) && !e.error,
                   "receive %u did not complete", i)) {
            break;
        }
        ih_mr_read(h, &r.mrb, VA_B + 8, &back, 1);
        if (!check(T, back == v, "receive %u carried %u", i, back)) {
            break;
        }
    }

    widest_wqe_send(T, &r);
    rc_teardown(T, &r);
}

/* ---- Speculative SGE lists --------------------------------------------- */

/*
 * The driver creates a queue that takes three to eight SGEs with speculative
 * SGEs, kernel and user alike, and lays out a work request with more than
 * two of them behind a table of their lengths in the first two SGE slots.
 * The device read the table as SGEs, so every such request named addresses
 * nobody had given it: nvmet-rdma's multi-page RDMA WRITEs could not gather.
 *
 * These are rc's rows with both QPs laying out their work requests that
 * way. Behind the table a 128-byte WQE has room for four SGEs. The last two
 * rows fail, but for the reason rc's do, not over the SGE list.
 */
static const struct rc_case spec_cases[] = {
    {"SEND, 3 SGEs into 4", IH_OP_SEND, 6000, 3, 4, 0, false, false, 0, 0},
    {"SEND, 4 SGEs into 3", IH_OP_SEND, 777, 4, 3, 0, false, false, 0, 0},
    {"SEND, 2 SGEs, so no table", IH_OP_SEND, 300, 2, 2, 0, false, false, 0, 0},
    {"SEND, 3 SGEs into 1", IH_OP_SEND, 450, 3, 1, 0, false, false, 0, 0},
    {"SEND_IMM, 3 SGEs", IH_OP_SEND_IMM, 900, 3, 3, 0, false, false, 0, 0},
    {"RDMA_WRITE, 4 SGEs", IH_OP_RDMA_WRITE, 8000, 4, 0, 0, false, false, 0, 0},
    {"RDMA_WRITE_IMM, 3 SGEs", IH_OP_RDMA_WRITE_IMM, 500, 3, 0, 0, false, false,
     0, 0},
    {"RDMA_READ, 3 SGEs", IH_OP_RDMA_READ, 4500, 3, 0, 0, false, false, 0, 0},
    {"RDMA_WRITE, 4 SGEs, bad rkey", IH_OP_RDMA_WRITE, 64, 4, 0, 0, false, true,
     IH_STS_REMOTE_ACC_ERR, 0},
    {"SEND too big for a 3-SGE receive", IH_OP_SEND, 400, 3, 3, 300, false,
     false, 0, IH_STS_LOCAL_LEN_ERR},
};

/*
 * Sixteen SGEs, the most a SPEC16 table describes, from an SQ with the
 * largest stride, received into four. Each SGE has a different length, so
 * an SGE list misread from the table can't come out right by chance.
 */
#define SPEC16_CQ_C 3
#define SPEC16_CQ_D 4
#define SPEC16_QP_C 12
#define SPEC16_QP_D 13

static void spec16_send(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    struct ih_cq cqc;
    struct ih_cq cqd;
    struct ih_qp c;
    struct ih_qp d;
    struct ih_cqe e;

    memset(&e, 0, sizeof(e));
    bool ok = ih_cq_create(h, &cqc, SPEC16_CQ_C, 6) &&
              ih_cq_create(h, &cqd, SPEC16_CQ_D, 6) &&
              ih_qp_create_stride(h, &c, SPEC16_QP_C, IH_QPT_RC, &cqc, &cqc,
                                  NULL, IH_WQE_MAX_STRIDE_LOG2) &&
              ih_qp_create(h, &d, SPEC16_QP_D, IH_QPT_RC, &cqd, &cqd, NULL) &&
              ih_qp_connect(h, &c, &d);
    if (!check(T, ok, "SPEC16 setup failed")) {
        return;
    }
    c.spec = true;
    d.spec = true;

    struct ih_sge sge[IH_SPEC16_MAX_SGE];
    struct ih_sge into[4];
    uint8_t want[512];
    uint8_t got[512];
    uint32_t total = 0;

    for (uint32_t i = 0; i < IH_SPEC16_MAX_SGE; i++) {
        sge[i] = (struct ih_sge){
            .va = VA_A + SRC_OFF + total, .len = (3 * i) + 1, .lkey = MR_A};
        total += sge[i].len;
    }
    fill(want, total, 0x5c);
    memset(got, 0, total);
    ih_mr_write(h, &r->mra, VA_A + SRC_OFF, want, total);
    ih_mr_write(h, &r->mrb, VA_B + DST_OFF, got, total);

    ih_post_recv(h, &d, into, split_sges(into, 4, VA_B + DST_OFF, total, MR_B));
    struct ih_send s = {
        .op = IH_OP_SEND, .sge = sge, .nsge = IH_SPEC16_MAX_SGE};
    ih_post_send(h, &c, &s);

    check(T, ih_cq_poll(h, &cqc, &e) && !e.error,
          "a SPEC16 SEND of %u SGEs did not complete", IH_SPEC16_MAX_SGE);
    check(T, ih_cq_poll(h, &cqd, &e) && !e.error && e.status_len == total,
          "a SPEC16 SEND of %u bytes was received as %u", total, e.status_len);
    ih_mr_read(h, &r->mrb, VA_B + DST_OFF, got, total);
    check(T, memcmp(got, want, total) == 0,
          "a SPEC16 SEND did not arrive whole");

    check(T, ih_qp_destroy(h, &c) == 0, "DESTROY_QP C failed");
    check(T, ih_qp_destroy(h, &d) == 0, "DESTROY_QP D failed");
    check(T, ih_cq_destroy(h, &cqc) == 0, "DESTROY_CQ C failed");
    check(T, ih_cq_destroy(h, &cqd) == 0, "DESTROY_CQ D failed");
}

/* A speculative receive out of an SRQ, which the device reads the same way */
#define SPEC_SRQ_CQ_E 5
#define SPEC_SRQ_CQ_F 6
#define SPEC_SRQ_QP_E 14
#define SPEC_SRQ_QP_F 15

static void spec_srq_recv(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    struct ih_srq srq;
    struct ih_cq cq_e;
    struct ih_cq cq_f;
    struct ih_qp qp_e;
    struct ih_qp qp_f;
    struct ih_cqe e;

    memset(&e, 0, sizeof(e));
    int rc = ih_srq_create(h, &srq, 4);
    if (!check(T, rc == 0, "CREATE_SRQ: %d", rc)) {
        return;
    }
    srq.spec = true;

    bool ok =
        ih_cq_create(h, &cq_e, SPEC_SRQ_CQ_E, 6) &&
        ih_cq_create(h, &cq_f, SPEC_SRQ_CQ_F, 6) &&
        ih_qp_create(h, &qp_e, SPEC_SRQ_QP_E, IH_QPT_RC, &cq_e, &cq_e, &srq) &&
        ih_qp_create(h, &qp_f, SPEC_SRQ_QP_F, IH_QPT_RC, &cq_f, &cq_f, NULL) &&
        ih_qp_connect(h, &qp_e, &qp_f);
    if (!check(T, ok, "speculative SRQ setup failed")) {
        ih_srq_destroy(h, &srq);
        return;
    }
    qp_f.spec = true;

    uint8_t want[1000];
    uint8_t got[1000];
    struct ih_sge src[3];
    struct ih_sge dst[4];
    fill(want, sizeof(want), 0x3a);
    memset(got, 0, sizeof(got));
    ih_mr_write(h, &r->mra, VA_A + SRC_OFF, want, sizeof(want));
    ih_mr_write(h, &r->mrb, VA_B + DST_OFF, got, sizeof(got));

    uint64_t id = ih_post_srq_recv(
        h, &srq, dst, split_sges(dst, 4, VA_B + DST_OFF, sizeof(got), MR_B));
    struct ih_send s = {.op = IH_OP_SEND, .sge = src};
    s.nsge = split_sges(src, 3, VA_A + SRC_OFF, sizeof(want), MR_A);
    ih_post_send(h, &qp_f, &s);

    check(T, ih_cq_poll(h, &cq_f, &e) && !e.error,
          "a SEND into a speculative SRQ receive did not complete");
    if (check(T, ih_cq_poll(h, &cq_e, &e),
              "no speculative SRQ receive completion")) {
        check(T, !e.error && e.wqe_id == id && e.status_len == sizeof(want),
              "the speculative SRQ receive completed WQE %" PRIu64
              " with %u bytes (error %d)",
              e.wqe_id, e.status_len, e.error);
    }
    ih_mr_read(h, &r->mrb, VA_B + DST_OFF, got, sizeof(got));
    check(T, memcmp(got, want, sizeof(got)) == 0,
          "a SEND into a speculative SRQ receive did not arrive whole");

    check(T, ih_qp_destroy(h, &qp_e) == 0 && ih_qp_destroy(h, &qp_f) == 0,
          "DESTROY_QP failed");
    check(T, ih_srq_destroy(h, &srq) == 0, "DESTROY_SRQ failed");
    check(T, ih_cq_destroy(h, &cq_e) == 0 && ih_cq_destroy(h, &cq_f) == 0,
          "DESTROY_CQ failed");
}

/*
 * Write @flags into the receive WQE last posted on @qp. The harness only
 * writes the formats the driver does; a guest can write anything.
 */
static bool last_recv_set_flags(struct ih *h, const struct ih_qp *qp,
                                uint16_t flags)
{
    const struct ih_ring *rq = &qp->rq;
    uint32_t slot = (rq->prod - 1) % (1U << rq->depth_log2);
    uint8_t b[2];

    ernic_put_be16(b, flags);
    return fake_vfu_write(h->vfu,
                          rq->gpa + ((uint64_t)slot << rq->stride_log2) + 10, b,
                          sizeof(b));
}

/*
 * Whether a warning the device logged after the first @since mentions
 * @what. Only the last FAKE_VFU_LOG_KEEP are kept to look at.
 */
static bool warned_since(struct ih *h, unsigned long since, const char *what)
{
    unsigned long n = fake_vfu_log_count(h->vfu, LOG_WARNING) - since;

    for (size_t back = 0; back < n && back < FAKE_VFU_LOG_KEEP; back++) {
        if (strstr(fake_vfu_log_text(h->vfu, LOG_WARNING, back), what)) {
            return true;
        }
    }
    return false;
}

/*
 * The four format bits have room for fifteen list formats besides the plain
 * one, and the driver defines two. The device can't tell where the SGEs of
 * any other are, so it
 * moves nothing rather than guess: a send completes with LOCAL_QP_OPER_ERR,
 * as an unknown opcode does, and consumes no receive; a receive takes no
 * bytes, so a SEND with a payload completes it with a length error and an
 * empty one completes it.
 */
static void spec_bad_formats(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    struct ih_cqe e;
    uint8_t want[64];
    uint8_t got[64];
    uint8_t guard[64];
    struct ih_sge from = {.va = VA_A + SRC_OFF, .len = 32, .lkey = MR_A};
    struct ih_sge into = {.va = VA_B + DST_OFF, .len = 64, .lkey = MR_B};

    r->a.spec = false;
    r->b.spec = false;
    fill(want, sizeof(want), 0x71);
    memset(guard, 0xee, sizeof(guard));
    ih_mr_write(h, &r->mra, VA_A + SRC_OFF, want, sizeof(want));
    ih_mr_write(h, &r->mrb, VA_B + DST_OFF, guard, sizeof(guard));

    /* Every undefined send format is refused, and takes no receive. */
    uint64_t recv_id = ih_post_recv(h, &r->b, &into, 1);
    for (uint16_t fmt = 3; fmt <= 15; fmt++) {
        char what[64];
        char fmt_s[32];
        snprintf(what, sizeof(what), "SEND in SGE list format %u", fmt);
        snprintf(fmt_s, sizeof(fmt_s), "format %#x", (unsigned)fmt << 12);
        struct ih_send s = {.op = IH_OP_SEND,
                            .flags = (uint16_t)(fmt << 12),
                            .sge = &from,
                            .nsge = 1};
        unsigned long since = fake_vfu_log_count(h->vfu, LOG_WARNING);
        ih_post_send(h, &r->a, &s);
        expect_send(T, what, r, IH_STS_LOCAL_QP_OPER);
        check(T, warned_since(h, since, fmt_s), "%s: no warning naming %s",
              what, fmt_s);
        check(T, cq_empty(h, &r->cqb), "%s: a receive completed", what);
    }

    /* The receive the refused sends left is still there for a good one. */
    struct ih_send s = {.op = IH_OP_SEND, .sge = &from, .nsge = 1};
    ih_post_send(h, &r->a, &s);
    expect_send(T, "SEND after the refused ones", r, 0);
    if (check(T, ih_cq_poll(h, &r->cqb, &e),
              "no receive for the SEND after the refused ones")) {
        check(T, !e.error && e.wqe_id == recv_id && e.status_len == 32,
              "the SEND after the refused ones completed WQE %" PRIu64
              " with %u bytes (error %d)",
              e.wqe_id, e.status_len, e.error);
    }
    ih_mr_read(h, &r->mrb, VA_B + DST_OFF, got, sizeof(got));
    check(T, memcmp(got, want, 32) == 0 && memcmp(got + 32, guard, 32) == 0,
          "the SEND after the refused ones did not arrive");

    /* A receive in an undefined format takes no bytes... */
    ih_mr_write(h, &r->mrb, VA_B + DST_OFF, guard, sizeof(guard));
    recv_id = ih_post_recv(h, &r->b, &into, 1);
    check(T, last_recv_set_flags(h, &r->b, 0x3000U),
          "could not rewrite the receive's flags");
    unsigned long since = fake_vfu_log_count(h->vfu, LOG_WARNING);
    ih_post_send(h, &r->a, &s);
    expect_send(T, "SEND into a receive in format 3", r, 0);
    check(T, warned_since(h, since, "format 0x3000"),
          "no warning naming the receive's format 0x3000");
    if (check(T, ih_cq_poll(h, &r->cqb, &e),
              "no completion for the receive in format 3")) {
        check(T,
              e.error && e.status_len == IH_STS_LOCAL_LEN_ERR &&
                  e.wqe_id == recv_id,
              "the receive in format 3 completed WQE %" PRIu64
              " with status %u (error %d), not a length error",
              e.wqe_id, e.status_len, e.error);
    }
    ih_mr_read(h, &r->mrb, VA_B + DST_OFF, got, sizeof(got));
    check(T, memcmp(got, guard, sizeof(guard)) == 0,
          "a receive in format 3 was written to");

    /* ...so an empty SEND is all it can take. */
    recv_id = ih_post_recv(h, &r->b, &into, 1);
    check(T, last_recv_set_flags(h, &r->b, 0xf000U),
          "could not rewrite the receive's flags");
    s.nsge = 0;
    ih_post_send(h, &r->a, &s);
    expect_send(T, "empty SEND into a receive in format 15", r, 0);
    if (check(T, ih_cq_poll(h, &r->cqb, &e),
              "no completion for the receive in format 15")) {
        check(T, !e.error && e.status_len == 0 && e.wqe_id == recv_id,
              "the receive in format 15 completed WQE %" PRIu64
              " with %u (error %d), not 0 bytes",
              e.wqe_id, e.status_len, e.error);
    }
    check(T, cq_empty(h, &r->cqb), "an extra completion on B");
}

static void test_spec_sge(void)
{
    const char *T = "spec-sge";
    struct rc r;

    if (!rc_setup(T, &r)) {
        return;
    }
    r.a.spec = true;
    r.b.spec = true;

    for (size_t i = 0; i < sizeof(spec_cases) / sizeof(spec_cases[0]); i++) {
        run_rc_case(T, &r, &spec_cases[i], (uint8_t)((i * 29) + 5));
    }

    spec16_send(T, &r);
    spec_srq_recv(T, &r);
    spec_bad_formats(T, &r);
    rc_teardown(T, &r);
}

/* ---- What RDMA_RESET_LIF frees ---------------------------------------- */

/*
 * A driver that reloads, or a guest that reboots, resets RDMA without
 * destroying what it made, and RDMA_RESET_LIF has to free all of it: the
 * resource manager's QPs, CQs, MRs, SRQs and PD, which every reset used to
 * leak, and the data path's registrations, which would otherwise still reach
 * memory the guest has taken back. Here that is an RDMA WRITE naming an MR
 * registered before the reset.
 */
static void test_rdma_reset_frees(void)
{
    const char *T = "rdma-reset-frees";
    struct rc r;
    struct ih *h = &r.h;
    struct ih_srq srq;
    struct ih_qp c;

    if (!rc_setup(T, &r)) {
        return;
    }

    PVRDMADev *pvrdma = h->dev->pvrdma_handle;
    RdmaDeviceResources *res = &pvrdma->rdma_dev_res;

    /* A third QP, connected to B, that the reloaded driver won't make
     * again */
    check(T,
          ih_qp_create(h, &c, RC_QP_C, IH_QPT_RC, &r.cqa, &r.cqa, NULL) &&
              ih_qp_connect(h, &c, &r.b),
          "making QP C failed");
    check(T, ih_srq_create(h, &srq, 4) == 0, "CREATE_SRQ failed");
    check(T, ih_ah_create(h, 5, 0xc0a80001U, 0xc0a80002U) == 0,
          "CREATE_AH failed");
    check(T,
          res->pd_tbl.used == 1 && res->cq_tbl.used == 2 &&
              res->qp_tbl.used == 3 && res->mr_tbl.used == 2 &&
              res->srq_tbl.used == 1,
          "%u PD(s), %u CQ(s), %u QP(s), %u MR(s) and %u SRQ(s) before the "
          "reset, expected 1, 2, 3, 2 and 1",
          res->pd_tbl.used, res->cq_tbl.used, res->qp_tbl.used,
          res->mr_tbl.used, res->srq_tbl.used);

    /* The driver reloads: RDMA_RESET_LIF, then its queues again */
    if (!check(T, ih_bringup(h), "bring-up after the reset failed")) {
        ih_mr_free(&r.mra);
        ih_mr_free(&r.mrb);
        teardown(T, h);
        return;
    }
    check(T,
          res->pd_tbl.used == 0 && res->cq_tbl.used == 0 &&
              res->qp_tbl.used == 0 && res->mr_tbl.used == 0 &&
              res->srq_tbl.used == 0 && g_hash_table_size(res->qp_hash) == 0,
          "%u PD(s), %u CQ(s), %u QP(s), %u QPN(s), %u MR(s) and %u SRQ(s) "
          "left after RDMA_RESET_LIF",
          res->pd_tbl.used, res->cq_tbl.used, res->qp_tbl.used,
          g_hash_table_size(res->qp_hash), res->mr_tbl.used, res->srq_tbl.used);

    /* It makes the same QPs again, but only A's MR, so B's key names
     * nothing */
    ih_mr_free(&r.mra);
    bool made =
        ih_cq_create(h, &r.cqa, RC_CQ_A, 6) &&
        ih_cq_create(h, &r.cqb, RC_CQ_B, 6) &&
        ih_qp_create(h, &r.a, RC_QP_A, IH_QPT_RC, &r.cqa, &r.cqa, NULL) &&
        ih_qp_create(h, &r.b, RC_QP_B, IH_QPT_RC, &r.cqb, &r.cqb, NULL) &&
        ih_qp_connect(h, &r.a, &r.b) &&
        ih_mr_create(h, &r.mra, MR_A, VA_A, MR_LEN, IH_ACCESS_ALL, true);
    if (check(T, made, "making the QPs again failed")) {
        uint8_t byte = 0x5a;
        uint8_t got = 0;
        struct ih_sge sge = {.va = VA_A, .len = 1, .lkey = MR_A};
        struct ih_send w = {.op = IH_OP_RDMA_WRITE,
                            .remote_va = VA_B,
                            .rkey = MR_B,
                            .sge = &sge,
                            .nsge = 1};

        ih_mr_write(h, &r.mra, VA_A, &byte, 1);
        r.msn_a = 1;
        ih_post_send(h, &r.a, &w);
        expect_send(T, "RDMA_WRITE to an MR from before the reset", &r,
                    IH_STS_REMOTE_ACC_ERR);
        ih_mr_read(h, &r.mrb, VA_B, &got, 1);
        check(T, got != byte,
              "an RDMA_WRITE reached an MR from before the reset");

        /*
         * A doorbell on C, whose rings are memory the guest has taken back,
         * runs nothing: here a WRITE into A's new MR, which B would accept
         */
        const uint64_t target = VA_A + 64;
        struct ih_send stale = w;
        stale.remote_va = target;
        stale.rkey = MR_A;
        ih_post_send(h, &c, &stale);
        ih_poll(h);
        got = 0;
        ih_mr_read(h, &r.mra, target, &got, 1);
        check(T, got != byte, "a QP from before the reset ran a WRITE");
        check(T, cq_empty(h, &r.cqa),
              "a QP from before the reset completed a WRITE");

        check(T, ih_qp_destroy(h, &r.a) == 0, "DESTROY_QP A failed");
        check(T, ih_qp_destroy(h, &r.b) == 0, "DESTROY_QP B failed");
        check(T, ih_mr_destroy(h, &r.mra) == 0, "DESTROY_MR A failed");
        check(T, ih_cq_destroy(h, &r.cqa) == 0, "DESTROY_CQ A failed");
        check(T, ih_cq_destroy(h, &r.cqb) == 0, "DESTROY_CQ B failed");
    }
    ih_mr_free(&r.mra);
    ih_mr_free(&r.mrb);
    teardown(T, h);
}

/* ---- Atomics and fast registration ------------------------------------- */

/* An atomic from A into B's MR, and what it should do */
struct atomic_case {
    uint8_t op; /* IH_OP_ATOMIC_CS or IH_OP_ATOMIC_FA */
    uint64_t swap_add;
    uint64_t compare;
    uint32_t rkey;
    uint64_t remote_va; /* 0 for the usual target, 0x800 into B */
    uint32_t status;    /* the completion's */
    uint64_t want_old;  /* what it fetches, if it succeeds */
    uint64_t want_new;  /* what it leaves at the target, if it succeeds */
};

static void atomic(const char *test, struct rc *r, const struct atomic_case *c)
{
    struct ih *h = &r->h;
    const uint64_t target = VA_B + 0x800;
    const uint64_t result = VA_A + 0x900;
    uint64_t v = 0;

    ih_mr_write(h, &r->mra, result, &v, 8);
    struct ih_sge sge = {.va = result, .len = 8, .lkey = MR_A};
    struct ih_send s = {.op = c->op,
                        .flags = IH_WQE_SIGNALLED,
                        .remote_va = c->remote_va ? c->remote_va : target,
                        .rkey = c->rkey,
                        .swap_add = c->swap_add,
                        .compare = c->compare,
                        .sge = &sge,
                        .nsge = 1};
    ih_post_send(h, &r->a, &s);
    expect_send(test, c->op == IH_OP_ATOMIC_CS ? "ATOMIC_CS" : "ATOMIC_FA", r,
                c->status);
    if (c->status) {
        return;
    }

    ih_mr_read(h, &r->mra, result, &v, 8);
    check(test, v == c->want_old,
          "atomic returned %" PRIu64 ", expected %" PRIu64, v, c->want_old);
    ih_mr_read(h, &r->mrb, target, &v, 8);
    check(test, v == c->want_new, "atomic left %" PRIu64 ", expected %" PRIu64,
          v, c->want_new);
}

static void test_atomic_and_reg_mr(void)
{
    const char *T = "atomic";
    struct rc r;
    struct ih *h = &r.h;

    if (!rc_setup(T, &r)) {
        return;
    }

    uint64_t v = 100;
    ih_mr_write(h, &r.mrb, VA_B + 0x800, &v, 8);
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_FA,
                                 .swap_add = 5,
                                 .rkey = MR_B,
                                 .want_old = 100,
                                 .want_new = 105});
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_CS,
                                 .swap_add = 7,
                                 .compare = 105,
                                 .rkey = MR_B,
                                 .want_old = 105,
                                 .want_new = 7});
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_CS,
                                 .swap_add = 9,
                                 .compare = 999,
                                 .rkey = MR_B,
                                 .want_old = 7,
                                 .want_new = 7});
    /* Operands of all ones, which the generic SGE parser once read as an
     * SGE running past 2^64, failing a valid compare-and-swap */
    v = UINT64_MAX;
    ih_mr_write(h, &r.mrb, VA_B + 0x800, &v, 8);
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_CS,
                                 .swap_add = UINT64_MAX,
                                 .compare = UINT64_MAX,
                                 .rkey = MR_B,
                                 .want_old = UINT64_MAX,
                                 .want_new = UINT64_MAX});
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_FA,
                                 .swap_add = 1,
                                 .rkey = 0x7e000099U,
                                 .status = IH_STS_REMOTE_ACC_ERR});
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_FA,
                                 .swap_add = 1,
                                 .rkey = MR_B,
                                 .remote_va = VA_B + MR_LEN - 4,
                                 .status = IH_STS_REMOTE_ACC_ERR});

    /* Operands split over two of A's pages, which aren't adjacent in guest
     * memory, can't be read or written as one u64 */
    const uint64_t split = VA_A + IH_PAGE_SIZE - VA_A % IH_PAGE_SIZE - 4;
    atomic(T, &r,
           &(struct atomic_case){.op = IH_OP_ATOMIC_FA,
                                 .swap_add = 1,
                                 .rkey = MR_A,
                                 .remote_va = split,
                                 .status = IH_STS_REMOTE_ACC_ERR});
    struct ih_sge split_sge = {.va = split, .len = 8, .lkey = MR_A};
    struct ih_send fa = {.op = IH_OP_ATOMIC_FA,
                         .remote_va = VA_B + 0x800,
                         .rkey = MR_B,
                         .swap_add = 1,
                         .sge = &split_sge,
                         .nsge = 1};
    ih_post_send(h, &r.a, &fa);
    expect_send(T, "ATOMIC_FA into a split result", &r, IH_STS_LOCAL_PROT_ERR);

    /*
     * Fast registration, as nvme-rdma does ahead of every command:
     * CREATE_MR leaves the MR unbound, a REG_MR work request binds it under
     * a rotated key, and LOCAL_INV unbinds it again.
     */
    struct ih_mr fr;
    const uint32_t fr_id = 4U;
    const uint32_t fr_key = IH_MRID(fr_id, 0x04);
    uint8_t body[45] = {0};
    ernic_put_le32(body + 20, IH_MRID(fr_id, 0x03));
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    check(T, ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL) == 0,
          "CREATE_MR of an unbound MR failed");
    if (!check(T,
               ih_mr_alloc(h, &fr, fr_key, UINT64_C(0x7f3000000200),
                           UINT64_C(2) * IH_PAGE_SIZE, true),
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

    uint8_t want[256];
    uint8_t got[256];
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
    ernic_put_le32(body, IH_MRID(fr_id, 0x03));
    check(T, ih_admin(h, IH_ADMIN_DESTROY_MR, body, 4, NULL) == 0,
          "DESTROY_MR of the fast-registered MR failed");

    rc_teardown(T, &r);
}

/* ---- Lengths and addresses that wrap ----------------------------------- */

/*
 * Every length and address here is the guest's, and each case is one whose
 * sum or offset carries past the top of its type. Wrapped, each used to name
 * something small and plausible instead: a short message, a region ending
 * near address 0, guest physical address 0 itself.
 */

#define WRAP_KEY_A IH_MRID(5, 0x05)
#define WRAP_KEY_B IH_MRID(6, 0x06)
#define WRAP_FR_ID 7U
#define WRAP_CQ    7
#define WRAP_QP    14
#define WRAP_VA    UINT64_C(0x00007f4000000000)

/* CREATE_MR of @len bytes at @va, through @map_count page table entries
 * at @dma_addr, whether or not they describe it. */
static int create_mr_as(struct ih *h, uint32_t lkey, uint64_t va, uint64_t len,
                        uint32_t map_count, uint64_t dma_addr)
{
    uint8_t body[45] = {0};

    ernic_put_le64(body + 0, va);
    ernic_put_le64(body + 8, len);
    ernic_put_le32(body + 20, lkey);
    ernic_put_le32(body + 28, map_count);
    ernic_put_le64(body + 32, dma_addr);
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    body[44] = 12;
    return ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL);
}

static int destroy_mr_id(struct ih *h, uint32_t lkey)
{
    uint8_t body[4];

    ernic_put_le32(body, lkey);
    return ih_admin(h, IH_ADMIN_DESTROY_MR, body, sizeof(body), NULL);
}

/* An RDMA_WRITE of the first @len bytes of A's MR to @va under @rkey. */
static void write_to(const char *T, struct rc *r, const char *what,
                     uint32_t rkey, uint64_t va, uint32_t len, uint32_t status)
{
    struct ih_sge sge = {.va = VA_A, .len = len, .lkey = MR_A};
    struct ih_send w = {.op = IH_OP_RDMA_WRITE,
                        .flags = IH_WQE_SIGNALLED,
                        .remote_va = va,
                        .rkey = rkey,
                        .sge = &sge,
                        .nsge = 1};

    ih_post_send(&r->h, &r->a, &w);
    expect_send(T, what, r, status);
}

/* A REG_MR or LOCAL_INV on @qp, which completes on @cq by its SQ index,
 * with @status. */
static void post_local_op(const char *T, struct ih *h, struct ih_qp *qp,
                          struct ih_cq *cq, const struct ih_send *s,
                          const char *what, uint32_t status)
{
    struct ih_cqe e;
    uint64_t id = ih_post_send(h, qp, s);

    if (!check(T,
               ih_cq_poll(h, cq, &e) && e.type == IH_CQE_TYPE_SEND_NPG &&
                   e.wqe_id == id,
               "%s did not complete by its SQ index", what)) {
        return;
    }
    check(T, e.error == (status != 0) && (!e.error || e.status_len == status),
          "%s: status %u (error %d), expected %u", what, e.status_len, e.error,
          status);
}

/* Sends whose length is more than they hold, or than a u32 holds. */
static void wrap_sends(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    const uint32_t holds = (1U << r->a.sq.stride_log2) - 32U;
    uint8_t want[1U << IH_WQE_MAX_STRIDE_LOG2];
    uint8_t got[sizeof(want)];
    struct ih_cqe e;

    /* B's one receive, which none of the refused SENDs may take */
    uint64_t rbuf = fake_vfu_alloc(h->vfu, sizeof(got), 8);
    uint64_t sbuf = fake_vfu_alloc(h->vfu, sizeof(want), 8);
    if (!check(T, rbuf && sbuf, "out of guest memory")) {
        return;
    }
    struct ih_sge into = {.va = rbuf, .len = sizeof(got), .lkey = 0};
    ih_post_recv(h, &r->b, &into, 1);
    fill(want, sizeof(want), 0x31);
    fake_vfu_write(h->vfu, sbuf, want, sizeof(want));

    /* Inline data longer than the WQE's payload area, which would be read
     * on from whatever follows the WQE */
    struct ih_send s = {.op = IH_OP_SEND,
                        .flags = IH_WQE_SIGNALLED | IH_WQE_INLINE,
                        .inline_data = want,
                        .inline_len = 16,
                        .length = holds + 1};
    ih_post_send(h, &r->a, &s);
    expect_send(T, "inline SEND longer than its WQE", r, IH_STS_LOCAL_LEN_ERR);

    /* SGE lengths that add up past 2^32: summed in 32 bits, a 16-byte
     * message */
    struct ih_sge big[2] = {{.va = sbuf, .len = 0xfffffff0U, .lkey = 0},
                            {.va = sbuf, .len = 0x20, .lkey = 0}};
    s = (struct ih_send){
        .op = IH_OP_SEND, .flags = IH_WQE_SIGNALLED, .sge = big, .nsge = 2};
    ih_post_send(h, &r->a, &s);
    expect_send(T, "SEND of more than 2^32 bytes", r, IH_STS_LOCAL_LEN_ERR);

    /* A bus address has no MR to bound it, but it still can't run off the
     * top of the address space and round to its bottom */
    struct ih_sge top = {.va = UINT64_MAX - 15, .len = 32, .lkey = 0};
    s = (struct ih_send){
        .op = IH_OP_SEND, .flags = IH_WQE_SIGNALLED, .sge = &top, .nsge = 1};
    ih_post_send(h, &r->a, &s);
    expect_send(T, "SEND past the top of the address space", r,
                IH_STS_LOCAL_PROT_ERR);

    check(T, cq_empty(h, &r->cqb), "a refused SEND took B's receive");

    /* Inline data that fills the payload area is still sent, into it */
    s = (struct ih_send){.op = IH_OP_SEND,
                         .flags = IH_WQE_SIGNALLED | IH_WQE_INLINE,
                         .inline_data = want,
                         .inline_len = holds};
    ih_post_send(h, &r->a, &s);
    expect_send(T, "inline SEND filling its WQE", r, 0);
    if (check(T, ih_cq_poll(h, &r->cqb, &e),
              "no receive for the inline SEND")) {
        check(T, !e.error && e.status_len == holds,
              "inline SEND received status %u (error %d), expected %u bytes",
              e.status_len, e.error, holds);
    }
    memset(got, 0, sizeof(got));
    fake_vfu_read(h->vfu, rbuf, got, holds);
    check(T, memcmp(got, want, holds) == 0, "inline SEND did not arrive");
}

/* Memory regions whose ends, and offsets into them, are at the top of the
 * address space. */
static void wrap_mrs(const char *T, struct rc *r)
{
    struct ih *h = &r->h;
    uint8_t want[64];
    uint8_t got[64];
    uint8_t zero[64] = {0};

    fill(want, sizeof(want), 0x52);
    ih_mr_write(h, &r->mra, VA_A, want, sizeof(want));

    /* A region past 2^64 is refused before anything is allocated for it, so
     * its id is still free */
    check(T,
          create_mr_as(h, WRAP_KEY_A, UINT64_MAX - 0xfff, 0x2000, 1,
                       h->scratch) == IONIC_RC_ERANGE,
          "CREATE_MR of a region past 2^64 succeeded");

    /*
     * One that ends at the top of the address space is a region, and two
     * pages are fewer than it could have, which is allowed. Counting its
     * pages rounded its length up past 2^64, and found it needed one, so
     * it was refused.
     */
    struct ih_mr pages;
    if (!check(T,
               ih_mr_alloc(h, &pages, WRAP_KEY_A, 0x100,
                           (2 * IH_PAGE_SIZE) - 0x100, false),
               "MR pages not allocated")) {
        return;
    }
    check(T,
          create_mr_as(h, WRAP_KEY_A, 0x100, UINT64_MAX - 0x100, 2,
                       pages.pt_gpa) == 0,
          "CREATE_MR of a region ending at 2^64 failed");
    write_to(T, r, "RDMA_WRITE into a region ending at 2^64", WRAP_KEY_A, 0x108,
             sizeof(want), 0);
    memset(got, 0, sizeof(got));
    fake_vfu_read(h->vfu, pages.pages[0] + 0x108, got, sizeof(got));
    check(T, memcmp(got, want, sizeof(got)) == 0,
          "RDMA_WRITE into a region ending at 2^64 did not land");
    ih_mr_free(&pages);
    check(T, destroy_mr_id(h, WRAP_KEY_A) == 0, "DESTROY_MR failed");

    /*
     * A one-page region based on the top page: an offset past that page
     * wrapped back to the bottom of guest memory, here to @victim.
     */
    uint64_t victim = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    if (!check(T, victim, "out of guest memory")) {
        return;
    }
    const uint64_t off = IH_PAGE_SIZE + victim;
    fake_vfu_write(h->vfu, victim, zero, sizeof(zero));
    check(T,
          create_mr_as(h, WRAP_KEY_B, WRAP_VA, off + IH_PAGE_SIZE, 1,
                       UINT64_MAX - (IH_PAGE_SIZE - 1)) == 0,
          "CREATE_MR of a region based on the top page failed");
    write_to(T, r, "RDMA_WRITE wrapping its region's base", WRAP_KEY_B,
             WRAP_VA + off, sizeof(want), IH_STS_LOCAL_PROT_ERR);
    fake_vfu_read(h->vfu, victim, got, sizeof(got));
    check(T, memcmp(got, zero, sizeof(got)) == 0,
          "a write wrapping its region's base landed at the bottom of guest "
          "memory");
    check(T, destroy_mr_id(h, WRAP_KEY_B) == 0, "DESTROY_MR failed");

    /*
     * Fast registration of a region past 2^64 binds nothing. It used to be
     * bound, and resolved nowhere: its end wrapped to below its start.
     */
    const uint32_t fr_key = IH_MRID(WRAP_FR_ID, 0x08);
    uint8_t body[45] = {0};
    struct ih_mr fr;
    ernic_put_le32(body + 20, IH_MRID(WRAP_FR_ID, 0x07));
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    if (!check(T,
               ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL) == 0,
               "CREATE_MR of an unbound MR failed") ||
        !check(T,
               ih_mr_alloc(h, &fr, fr_key, WRAP_VA, UINT64_C(2) * IH_PAGE_SIZE,
                           false),
               "MR pages not allocated")) {
        return;
    }

    struct ih_mr past = fr;
    past.va = UINT64_MAX - (IH_PAGE_SIZE - 1);
    struct ih_send reg = {.op = IH_OP_REG_MR, .key = fr_key, .mr = &past};
    post_local_op(T, h, &r->a, &r->cqa, &reg, "REG_MR of a region past 2^64",
                  IH_STS_MEM_MGMT_ERR);
    write_to(T, r, "RDMA_WRITE through a REG_MR of a region past 2^64", fr_key,
             past.va, sizeof(want), IH_STS_REMOTE_ACC_ERR);

    /*
     * A REG_MR that fails takes away the binding its key had. It used to
     * leave it with its page table freed and a base of 0, so a write
     * through the key went to guest physical address 0 onwards.
     */
    reg.mr = &fr;
    post_local_op(T, h, &r->a, &r->cqa, &reg, "REG_MR", 0);
    write_to(T, r, "RDMA_WRITE through a fast-registered MR", fr_key, fr.va + 8,
             sizeof(want), 0);
    struct ih_mr too_many = fr;
    too_many.npages = 1000; /* for a region of two pages */
    reg.mr = &too_many;
    post_local_op(T, h, &r->a, &r->cqa, &reg, "REG_MR with too many pages",
                  IH_STS_MEM_MGMT_ERR);
    write_to(T, r, "RDMA_WRITE through a key whose REG_MR failed", fr_key,
             fr.va + 8, sizeof(want), IH_STS_REMOTE_ACC_ERR);

    /*
     * WQEs of 32 bytes, too short for the fields a REG_MR or an atomic
     * carries. Neither may run on the bytes past the end of one.
     */
    struct ih_cq cqs;
    struct ih_qp small;
    if (check(T,
              ih_cq_create(h, &cqs, WRAP_CQ, 6) &&
                  ih_qp_create_stride(h, &small, WRAP_QP, IH_QPT_RC, &cqs, &cqs,
                                      NULL, 5),
              "short-WQE QP setup failed")) {
        /* A short REG_MR fails like any other, and takes away the binding
         * the MR had, under an earlier rotation of the key too */
        reg.mr = &fr;
        post_local_op(T, h, &r->a, &r->cqa, &reg, "REG_MR", 0);
        write_to(T, r, "RDMA_WRITE through a fast-registered MR", fr_key,
                 fr.va + 8, sizeof(want), 0);
        struct ih_send rotated = reg;
        rotated.key = IH_MRID(WRAP_FR_ID, 0x09);
        post_local_op(T, h, &small, &cqs, &rotated, "REG_MR in a 32-byte WQE",
                      IH_STS_MEM_MGMT_ERR);
        write_to(T, r, "RDMA_WRITE through a key a short REG_MR replaced",
                 fr_key, fr.va + 8, sizeof(want), IH_STS_REMOTE_ACC_ERR);

        /* A short atomic fails, and leaves its target alone. The result
         * SGE is past the end of the WQE, so none is written. */
        uint64_t v = 41;
        uint64_t after = 0;
        struct ih_send fa = {.op = IH_OP_ATOMIC_FA,
                             .flags = IH_WQE_SIGNALLED,
                             .remote_va = VA_B + 0x800,
                             .rkey = MR_B,
                             .swap_add = 1};
        struct ih_cqe e;
        ih_mr_write(h, &r->mrb, VA_B + 0x800, &v, 8);
        ih_post_send(h, &small, &fa);
        if (check(T, ih_cq_poll(h, &cqs, &e),
                  "a short atomic did not complete")) {
            check(T,
                  e.type == IH_CQE_TYPE_SEND_MSN && e.error &&
                      e.status_len == IH_STS_LOCAL_QP_OPER,
                  "a short atomic: type %u status %u (error %d), expected %d",
                  e.type, e.status_len, e.error, IH_STS_LOCAL_QP_OPER);
        }
        ih_mr_read(h, &r->mrb, VA_B + 0x800, &after, 8);
        check(T, after == v, "a short atomic changed its target to %" PRIu64,
              after);

        check(T, ih_qp_destroy(h, &small) == 0 && ih_cq_destroy(h, &cqs) == 0,
              "short-WQE QP teardown failed");
    }

    ih_mr_free(&fr);
    check(T, destroy_mr_id(h, IH_MRID(WRAP_FR_ID, 0x07)) == 0,
          "DESTROY_MR of the fast-registered MR failed");
}

static void test_wrap(void)
{
    const char *T = "wrap";
    struct rc r;

    if (!rc_setup(T, &r)) {
        return;
    }
    wrap_sends(T, &r);
    wrap_mrs(T, &r);
    check(T, fake_vfu_mapped(r.h.vfu) == 0, "guest memory left mapped");
    rc_teardown(T, &r);
}

/* ---- MR keys ------------------------------------------------------------ */

/*
 * An MR's key is its index above a key byte that fast registration rotates,
 * and only the key the MR is bound under reaches it. The byte was once read
 * as part of the index, so a rotation added a binding instead of replacing
 * one: every earlier key still reached the MR's pages, and still did once
 * DESTROY_MR had destroyed the MR.
 */

#define KEYS_ID 9U
#define KEYS_VA UINT64_C(0x00007f6000000000)

static void test_mr_keys(void)
{
    const char *T = "mr-keys";
    const uint32_t created = IH_MRID(KEYS_ID, 0x10);
    struct rc r;
    struct ih *h = &r.h;
    struct ih_mr fr;
    uint8_t want[64];
    uint8_t other[64];
    uint8_t got[64];

    if (!rc_setup(T, &r)) {
        return;
    }
    fill(want, sizeof(want), 0x4b);
    fill(other, sizeof(other), 0x6c);
    ih_mr_write(h, &r.mra, VA_A, want, sizeof(want));

    uint8_t body[45] = {0};
    ernic_put_le32(body + 20, created);
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    if (!check(T,
               ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL) == 0,
               "CREATE_MR of an unbound MR failed") ||
        !check(T,
               ih_mr_alloc(h, &fr, IH_MRID(KEYS_ID, 0x11), KEYS_VA,
                           IH_PAGE_SIZE, false),
               "MR pages not allocated")) {
        rc_teardown(T, &r);
        return;
    }

    /* REG_MR binds the MR under a rotated key, and only that key */
    struct ih_send reg = {
        .op = IH_OP_REG_MR, .key = IH_MRID(KEYS_ID, 0x11), .mr = &fr};
    post_local_op(T, h, &r.a, &r.cqa, &reg, "REG_MR", 0);
    write_to(T, &r, "RDMA_WRITE through the key REG_MR bound", reg.key, KEYS_VA,
             sizeof(want), 0);
    write_to(T, &r, "RDMA_WRITE through the key the MR was created with",
             created, KEYS_VA, sizeof(want), IH_STS_REMOTE_ACC_ERR);

    /* Rotated again, the key before reaches nothing */
    const uint32_t old_key = reg.key;
    reg.key = IH_MRID(KEYS_ID, 0x12);
    post_local_op(T, h, &r.a, &r.cqa, &reg, "REG_MR under a rotated key", 0);
    write_to(T, &r, "RDMA_WRITE through a key the MR rotated away from",
             old_key, KEYS_VA, sizeof(want), IH_STS_REMOTE_ACC_ERR);
    write_to(T, &r, "RDMA_WRITE through the rotated key", reg.key, KEYS_VA,
             sizeof(want), 0);

    /* LOCAL_INV of the earlier key leaves the binding that replaced it */
    struct ih_send inv = {.op = IH_OP_LOCAL_INV, .key = old_key};
    post_local_op(T, h, &r.a, &r.cqa, &inv, "LOCAL_INV of an earlier key", 0);
    write_to(T, &r, "RDMA_WRITE after a LOCAL_INV of an earlier key", reg.key,
             KEYS_VA, sizeof(want), 0);

    /* Index 0 is the reserved lkey's, and one past ERNIC_MAX_MR the device
     * has no slot for: neither names an MR to bind */
    const uint32_t no_mr[] = {IH_MRID(0, 0x05), IH_MRID(ERNIC_MAX_MR + 1, 0)};
    for (size_t i = 0; i < sizeof(no_mr) / sizeof(no_mr[0]); i++) {
        struct ih_send bad = {.op = IH_OP_REG_MR, .key = no_mr[i], .mr = &fr};

        post_local_op(T, h, &r.a, &r.cqa, &bad,
                      "REG_MR of an index with no slot", IH_STS_MEM_MGMT_ERR);
        write_to(T, &r, "RDMA_WRITE through an index with no slot", no_mr[i],
                 KEYS_VA, sizeof(want), IH_STS_REMOTE_ACC_ERR);
    }
    check(T,
          create_mr_as(h, IH_MRID(ERNIC_MAX_MR + 1, 0x01), KEYS_VA,
                       IH_PAGE_SIZE, 1, fr.pages[0]) == IONIC_RC_ERANGE,
          "CREATE_MR of an index past the table was not refused");

    /* An index in use is refused, whatever its key byte */
    check(T,
          create_mr_as(h, IH_MRID(KEYS_ID, 0x77), KEYS_VA, IH_PAGE_SIZE, 1,
                       fr.pages[0]) == IONIC_RC_EEXIST,
          "CREATE_MR of an index in use under another key was not refused");

    /*
     * DESTROY_MR names the MR by the key it was created with, and takes
     * away its binding under the rotated one: a write through that key
     * must not reach the pages of a destroyed MR.
     */
    check(T, destroy_mr_id(h, created) == 0, "DESTROY_MR failed");
    ih_mr_write(h, &r.mra, VA_A, other, sizeof(other));
    write_to(T, &r, "RDMA_WRITE through a destroyed MR's rotated key", reg.key,
             KEYS_VA, sizeof(other), IH_STS_REMOTE_ACC_ERR);
    ih_mr_read(h, &fr, KEYS_VA, got, sizeof(got));
    check(T, memcmp(got, want, sizeof(got)) == 0,
          "a write through a destroyed MR's rotated key landed");

    /* The index is free again */
    check(T,
          create_mr_as(h, IH_MRID(KEYS_ID, 0x13), KEYS_VA, IH_PAGE_SIZE, 1,
                       fr.pages[0]) == 0,
          "CREATE_MR of a destroyed MR's index failed");
    write_to(T, &r, "RDMA_WRITE through an index created again",
             IH_MRID(KEYS_ID, 0x13), KEYS_VA, sizeof(other), 0);
    ih_mr_read(h, &fr, KEYS_VA, got, sizeof(got));
    check(T, memcmp(got, other, sizeof(got)) == 0,
          "RDMA_WRITE through an index created again did not land");
    check(T, destroy_mr_id(h, IH_MRID(KEYS_ID, 0x13)) == 0,
          "DESTROY_MR failed");

    ih_mr_free(&fr);
    rc_teardown(T, &r);
}

/* ---- The data path when guest memory can't be reached ------------------ */

#define NO_CQE UINT32_MAX
#define MR_C   IH_MRID(3, 0x03)
#define VA_C   UINT64_C(0x00007f2000000000)

/* An 8-byte RDMA_WRITE or ATOMIC_FA from A to B, with the DMA @fail refused,
 * counted from the doorbell: the WQE, the transfer, then the completion. */
struct dp_dma_fault {
    const char *what;
    uint8_t op;
    unsigned fail;
    uint32_t status; /* of the send completion, or NO_CQE */
};

static const struct dp_dma_fault dp_dma_faults[] = {
    {"RDMA_WRITE's source", IH_OP_RDMA_WRITE, 2, IH_STS_LOCAL_ACC_ERR},
    {"RDMA_WRITE's destination", IH_OP_RDMA_WRITE, 3, IH_STS_LOCAL_ACC_ERR},
    {"ATOMIC_FA's read", IH_OP_ATOMIC_FA, 2, IH_STS_REMOTE_ACC_ERR},
    {"ATOMIC_FA's write", IH_OP_ATOMIC_FA, 3, IH_STS_REMOTE_ACC_ERR},
    {"ATOMIC_FA's result", IH_OP_ATOMIC_FA, 4, IH_STS_LOCAL_ACC_ERR},
    {"RDMA_WRITE's completion", IH_OP_RDMA_WRITE, 4, NO_CQE},
};

static void test_dp_dma_faults(void)
{
    const char *T = "dp-dma";
    struct rc r;
    struct ih *h = &r.h;
    struct ih_cqe e;
    struct ih_sge sge = {.va = VA_A + 0x900, .len = 8, .lkey = MR_A};
    struct ih_send s = {.flags = IH_WQE_SIGNALLED,
                        .remote_va = VA_B + 0x800,
                        .rkey = MR_B,
                        .swap_add = 1,
                        .sge = &sge,
                        .nsge = 1};

    if (!rc_setup(T, &r)) {
        return;
    }

    for (size_t i = 0; i < sizeof(dp_dma_faults) / sizeof(dp_dma_faults[0]);
         i++) {
        const struct dp_dma_fault *c = &dp_dma_faults[i];

        s.op = c->op;
        fake_vfu_fail_nth_map(h->vfu, c->fail);
        ih_post_send(h, &r.a, &s);
        fake_vfu_fail_nth_map(h->vfu, 0);
        if (c->status != NO_CQE) {
            expect_send(T, c->what, &r, c->status);
        } else {
            check(T, cq_empty(h, &r.cqa), "%s refused: a completion arrived",
                  c->what);
            /* A WQE that ran took an MSN, though its completion was lost */
            r.msn_a++;
        }
        check(T, fake_vfu_mapped(h->vfu) == 0,
              "%s refused: guest memory left mapped", c->what);
    }

    /*
     * A WQE the device can't read completes in error, named by its slot,
     * which is what the drivers put in its wqe_id, and flushes the QP. It
     * takes no MSN, which the send after it checks.
     */
    uint32_t slot = r.a.sq.prod % (1U << r.a.sq.depth_log2);
    s.op = IH_OP_RDMA_WRITE;
    fake_vfu_fail_nth_map(h->vfu, 1);
    ih_post_send(h, &r.a, &s);
    fake_vfu_fail_nth_map(h->vfu, 0);
    if (check(T, ih_cq_poll(h, &r.cqa, &e),
              "an unreadable WQE did not complete")) {
        check(T,
              e.type == IH_CQE_TYPE_SEND_NPG && e.qid == RC_QP_A && e.error &&
                  e.status_len == IH_STS_LOCAL_QP_OPER && e.wqe_id == slot,
              "an unreadable WQE: type %u qid %u status %u (error %d) "
              "wqe_id %" PRIu64 ", expected type %d qid %d status %d "
              "wqe_id %" PRIu32,
              e.type, e.qid, e.status_len, e.error, e.wqe_id,
              IH_CQE_TYPE_SEND_NPG, RC_QP_A, IH_STS_LOCAL_QP_OPER, slot);
    }
    bool flushed_sq = false, flushed_rq = false;
    while (ih_cq_poll(h, &r.cqa, &e)) {
        check(T, e.error && e.status_len == IH_STS_WQE_FLUSHED_ERR,
              "an unreadable WQE: completion status %u (error %d) after it, "
              "expected only flushes",
              e.status_len, e.error);
        flushed_sq |= e.type == IH_CQE_TYPE_SEND_NPG;
        flushed_rq |= e.type == IH_CQE_TYPE_RECV;
    }
    check(T, flushed_sq && flushed_rq,
          "an unreadable WQE: QP flushed send queue %d, receive queue %d",
          flushed_sq, flushed_rq);
    check(T, fake_vfu_mapped(h->vfu) == 0,
          "an unreadable WQE: guest memory left mapped");

    /* An event the device can't write is lost, but its completion isn't */
    ih_cq_arm(h, &r.cqa);
    s.op = IH_OP_RDMA_WRITE;
    fake_vfu_fail_nth_map(h->vfu, 6);
    ih_post_send(h, &r.a, &s);
    check(T, eq_count(h, RC_CQ_A) == 0, "an unwritable event arrived");
    expect_send(T, "RDMA_WRITE with its event refused", &r, 0);

    /*
     * An MR whose page table can't be read, or has more entries than its
     * region needs, is refused, and leaves nothing behind: the resource
     * manager holds no more MRs than before, no key reaches the region, and
     * the id can be created again.
     */
    const PVRDMADev *pvrdma = h->dev->pvrdma_handle;
    const uint32_t mrs = pvrdma->rdma_dev_res.mr_tbl.used;
    struct ih_mr mrc;
    if (check(
            T,
            ih_mr_alloc(h, &mrc, MR_C, VA_C, UINT64_C(2) * IH_PAGE_SIZE, false),
            "MR pages not allocated")) {
        fake_vfu_fail_nth_map(h->vfu, 2);
        check(T,
              create_mr_as(h, MR_C, VA_C, UINT64_C(2) * IH_PAGE_SIZE,
                           mrc.npages, mrc.pt_gpa) == IONIC_RC_EIO,
              "CREATE_MR with an unreadable page table succeeded");
        check(T,
              create_mr_as(h, MR_C, VA_C, IH_PAGE_SIZE, mrc.npages + 1,
                           mrc.pt_gpa) == IONIC_RC_EINVAL,
              "CREATE_MR with too long a page table succeeded");
        check(T, pvrdma->rdma_dev_res.mr_tbl.used == mrs,
              "the refused CREATE_MRs kept their MRs");
        s.remote_va = VA_C;
        s.rkey = MR_C;
        ih_post_send(h, &r.a, &s);
        expect_send(T, "RDMA_WRITE to a refused MR", &r, IH_STS_REMOTE_ACC_ERR);
        check(T,
              create_mr_as(h, MR_C, VA_C, UINT64_C(2) * IH_PAGE_SIZE,
                           mrc.npages, mrc.pt_gpa) == 0,
              "CREATE_MR of the refused MR's id failed");
        ih_post_send(h, &r.a, &s);
        expect_send(T, "RDMA_WRITE to the MR created again", &r, 0);
        check(T, ih_mr_destroy(h, &mrc) == 0, "DESTROY_MR C failed");
    }

    /*
     * Ring geometry the admin queue refuses before the data path sees it,
     * passed to the data path directly: an SRQ is refused, a QP keeps the
     * rings it had, and a CQ is turned off rather than written past.
     */
    struct ionic_datapath *dp = h->dp;
    const struct ionic_dp_ring_desc good = {
        .buf = {.dma_addr = h->scratch, .map_count = 1},
        .depth_log2 = 4,
        .stride_log2 = 7};
    static const uint8_t bad_geometry[][2] = {
        {IONIC_RING_MAX_DEPTH_LOG2 + 1, 7}, {4, 4}, {4, 10}};
    for (size_t i = 0; i < sizeof(bad_geometry) / sizeof(bad_geometry[0]);
         i++) {
        struct ionic_dp_ring_desc bad = good;
        bad.depth_log2 = bad_geometry[i][0];
        bad.stride_log2 = bad_geometry[i][1];
        check(T,
              ionic_datapath_register_srq(dp, IONIC_SRQ_QID_BASE, &bad) ==
                  -EINVAL,
              "the data path took an SRQ of 2^%u by 2^%u", bad.depth_log2,
              bad.stride_log2);
        check(T,
              ionic_datapath_register_qp(dp, RC_QP_A, IBV_QPT_RC, RC_CQ_A, &bad,
                                         RC_CQ_A, &good) == -EINVAL,
              "the data path took an SQ of 2^%u by 2^%u", bad.depth_log2,
              bad.stride_log2);
        /* An RQ has no stride floor here: an SRQ-bound QP's is zero */
        if (bad.stride_log2 >= IONIC_WQE_MIN_STRIDE_LOG2) {
            check(T,
                  ionic_datapath_register_qp(dp, RC_QP_A, IBV_QPT_RC, RC_CQ_A,
                                             &good, RC_CQ_A, &bad) == -EINVAL,
                  "the data path took an RQ of 2^%u by 2^%u", bad.depth_log2,
                  bad.stride_log2);
        }
    }
    s.remote_va = VA_B + 0x800;
    s.rkey = MR_B;
    ih_post_send(h, &r.a, &s);
    expect_send(T, "RDMA_WRITE after refused QP rings", &r, 0);

    /* The refused CQ's ring is a page of its own, since a CQ that took it
     * would complete there and not in the ring cq_empty() polls */
    struct ionic_dp_ring_desc bad_cq = good;
    uint8_t cq_was[IH_PAGE_SIZE];
    uint8_t cq_now[IH_PAGE_SIZE];
    bad_cq.buf.dma_addr = fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    fill(cq_was, sizeof(cq_was), 0x5a);
    fake_vfu_write(h->vfu, bad_cq.buf.dma_addr, cq_was, sizeof(cq_was));
    bad_cq.stride_log2 = 4;
    check(T,
          ionic_datapath_register_cq(dp, RC_CQ_A, IH_EQ_ID, &bad_cq) == -EINVAL,
          "the data path took a CQ of stride 2^%u", bad_cq.stride_log2);
    ih_post_send(h, &r.a, &s);
    check(T, cq_empty(h, &r.cqa), "a CQ with a refused stride took a CQE");
    fake_vfu_read(h->vfu, bad_cq.buf.dma_addr, cq_now, sizeof(cq_now));
    check(T, memcmp(cq_now, cq_was, sizeof(cq_now)) == 0,
          "a CQ with a refused stride wrote to its ring");

    rc_teardown(T, &r);
}

/* ---- Full tables -------------------------------------------------------- */

static void test_full_tables(void)
{
    const char *T = "tables";
    struct rc r;
    struct ih *h = &r.h;
    int rc = 0;

    if (!rc_setup(T, &r)) {
        return;
    }

    /* The device has room for 32 EQs and 4 admin queues, one of each the
     * harness's */
    unsigned n = 1;
    while (n < 64 && (rc = rdma_queue_cmd(h, &good_eq)) == 0) {
        n++;
    }
    check(T, n == 32 && rc == 1, "%u EQs, then status %d", n, rc);
    n = 1;
    while (n < 8 && rdma_queue_cmd(h, &good_admin_cq) == 0 &&
           (rc = rdma_queue_cmd(h, &good_adminq)) == 0) {
        n++;
    }
    check(T, n == 4 && rc == 1, "%u admin queues, then status %d", n, rc);

    /* Only RDMA devcmds reach the dispatcher, but it refuses others */
    uint8_t cmd[64] = {IH_CMD_RDMA_ADMINQ + 1};
    uint8_t comp[16];
    ionic_rdma_devcmd_dispatch(h->rdma, cmd, comp);
    check(T, comp[0] == 1, "an unknown RDMA devcmd returned %u", comp[0]);
    check(T, ionic_rdma_devcmd_post_cq_event(h->rdma, 9, RC_CQ_A) == -ENOENT,
          "an event was posted to an EQ that doesn't exist");

    /* CREATE_MR fills the MR table, every one of them on the same page:
     * indices 1 and 2 are A's and B's, and one past ERNIC_MAX_MR has no
     * slot */
    uint8_t body[45] = {0};
    ernic_put_le64(body + 0, VA_C);
    ernic_put_le64(body + 8, IH_PAGE_SIZE);
    ernic_put_le32(body + 28, 1);
    ernic_put_le64(body + 32,
                   fake_vfu_alloc(h->vfu, IH_PAGE_SIZE, IH_PAGE_SIZE));
    ernic_put_le16(body + 40, IH_ACCESS_ALL);
    body[44] = 12;
    for (n = 3; n <= ERNIC_MAX_MR + 1; n++) {
        ernic_put_le32(body + 20, IH_MRID(n, 0x5a));
        rc = ih_admin(h, IH_ADMIN_CREATE_MR, body, sizeof(body), NULL);
        if (rc != 0) {
            break;
        }
    }
    check(T, n == ERNIC_MAX_MR + 1 && rc == IONIC_RC_ERANGE,
          "MRs up to index %u, then status %d", n, rc);

    /* So REG_MR of an index past the table binds nothing, and its key names
     * nothing */
    const uint32_t key = IH_MRID(0xfff0U, 0x00);
    struct ih_send reg = {.op = IH_OP_REG_MR, .key = key, .mr = &r.mrb};
    struct ih_cqe e;
    ih_post_send(h, &r.a, &reg);
    check(T, ih_cq_poll(h, &r.cqa, &e) && e.type == IH_CQE_TYPE_SEND_NPG,
          "REG_MR past a full table did not complete");
    struct ih_sge sge = {.va = VA_A, .len = 8, .lkey = MR_A};
    struct ih_send w = {.op = IH_OP_RDMA_WRITE,
                        .remote_va = r.mrb.va,
                        .rkey = key,
                        .sge = &sge,
                        .nsge = 1};
    ih_post_send(h, &r.a, &w);
    expect_send(T, "RDMA_WRITE through a key with no slot", &r,
                IH_STS_REMOTE_ACC_ERR);

    rc_teardown(T, &r);
}

/* ---- Device limits ------------------------------------------------------ */

/*
 * What the identity promises the guest, every table has room for. The
 * guest was once told 32768 QPs and 65536 CQs by a device whose admin queue
 * could map 256 of each, so its 257th CREATE failed; and a backend sizes the
 * resource manager's tables, which the identity knows nothing about. So
 * this checks the counts the driver reads, the tables every backend sizes,
 * and that the guest can create a CQ and a QP under every id it may name --
 * and none past them, which the data path's tables have no room for.
 */

/* CREATE_CQ of @id, with its ring at @ring; returns as ih_admin() does. */
static int limits_create_cq(struct ih *h, uint32_t id, uint64_t ring)
{
    uint8_t body[34] = {0};

    ernic_put_le32(body + 0, IH_EQ_ID);
    body[4] = 6;  /* depth_log2: 64 CQEs, */
    body[5] = 5;  /* of 32 bytes, fill the page */
    body[7] = 12; /* page_size_log2 */
    ernic_put_le32(body + 12, id | 1U << 24);
    ernic_put_le32(body + 20, 1);
    ernic_put_le64(body + 24, ring);
    return ih_admin(h, IH_ADMIN_CREATE_CQ, body, sizeof(body), NULL);
}

/* CREATE_QP of an RC QP @id on CQ @cq, with its rings at @sq and @rq. */
static int limits_create_qp(struct ih *h, uint32_t id, uint32_t cq, uint64_t sq,
                            uint64_t rq)
{
    uint8_t body[64] = {0};

    ernic_put_le32(body + 8, cq);
    body[12] = 4; /* depth_log2: 16 WQEs, */
    body[13] = 7; /* of 128 bytes, in half the page */
    body[15] = 12;
    ernic_put_le32(body + 20, 1);
    ernic_put_le64(body + 24, sq);
    ernic_put_le32(body + 32, cq);
    body[36] = 4;
    body[37] = 7;
    body[39] = 12;
    ernic_put_le32(body + 40, 0xffffffffU); /* no SRQ */
    ernic_put_le32(body + 44, 1);
    ernic_put_le64(body + 48, rq);
    ernic_put_le32(body + 56, id | 1U << 24);
    body[62] = IH_QPT_RC;
    return ih_admin(h, IH_ADMIN_CREATE_QP, body, sizeof(body), NULL);
}

static void test_limits(void)
{
    static const char T[] = "limits";
    struct ih h;

    /*
     * Every backend's query_device(), which sizes the resource manager's
     * tables, reports the limits. Called directly, since the TCP backend
     * can't be set up without a mesh; none of them looks at the device.
     */
    for (unsigned t = 0; t < RDMA_BACKEND_TYPE_MAX; t++) {
        const RdmaBackendOps *ops = rdma_backend_get_ops((RdmaBackendType)t);
        RdmaBackendDev dev;
        struct ibv_device_attr a;

        if (!check(T, ops && ops->query_device, "backend %u has no query", t)) {
            continue;
        }
        memset(&dev, 0, sizeof(dev));
        if (!check(T, ops->query_device(&dev, &a) == 0, "%s: query failed",
                   ops->name)) {
            continue;
        }
        /* A backend that can't make an SRQ has no table for one */
        int srqs = ops->create_srq ? (int)ERNIC_MAX_SRQ : 0;
        check(T,
              a.max_qp == (int)ERNIC_MAX_QP && a.max_cq == (int)ERNIC_MAX_CQ &&
                  a.max_mr == (int)ERNIC_MAX_MR && a.max_srq == srqs,
              "%s: %d QPs, %d CQs, %d MRs and %d SRQs, expected %u, %u, %u "
              "and %d",
              ops->name, a.max_qp, a.max_cq, a.max_mr, a.max_srq, ERNIC_MAX_QP,
              ERNIC_MAX_CQ, ERNIC_MAX_MR, srqs);
    }

    if (!check(T, ih_create(&h, "loopback"), "device not created")) {
        return;
    }

    /* The counts, read where the driver reads them */
    const uint32_t db_pages = IONIC_BAR2_DB_SIZE / IONIC_DB_PAGE_SIZE;
    uint8_t cmd[64] = {IH_CMD_IDENTIFY};
    uint8_t comp[16];
    uint8_t data[1912];
    if (check(T, ih_devcmd(&h, cmd, comp, data, 512) == 0, "IDENTIFY failed")) {
        check(T, ernic_get_le32(data + 0x10) == db_pages,
              "ndbpgs_per_lif is %u, not the %u pages of BAR2",
              ernic_get_le32(data + 0x10), db_pages);
    }
    cmd[0] = IH_CMD_LIF_IDENTIFY;
    if (check(T, ih_devcmd(&h, cmd, comp, data, sizeof(data)) == 0,
              "LIF_IDENTIFY failed")) {
        const uint8_t *rdma = data + LIFID_RDMA;
        static const struct {
            uint32_t off;
            const char *what;
            uint32_t want;
        } counts[] = {
            /* The driver never hands out MR index 0 */
            {8, "nmrs_per_lif", ERNIC_MAX_MR + 1U},
            {12, "nahs_per_lif", ERNIC_MAX_AH},
            {36, "aq_qtype.qid_count", ERNIC_MAX_AQ},
            {48, "sq_qtype.qid_count", ERNIC_MAX_QP},
            {60, "rq_qtype.qid_count", ERNIC_MAX_QP},
            {72, "cq_qtype.qid_count", ERNIC_MAX_CQ},
            {100, "srq_qtype.qid_count", ERNIC_MAX_SRQ},
        };
        for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            check(T, ernic_get_le32(rdma + counts[i].off) == counts[i].want,
                  "%s is %u, expected %u", counts[i].what,
                  ernic_get_le32(rdma + counts[i].off), counts[i].want);
        }
    }

    if (!check(T, ih_bringup(&h), "bring-up failed")) {
        teardown(T, &h);
        return;
    }

    /*
     * A CQ under every id the driver can give one but the admin CQ's, all
     * on one page, since none of them completes anything; then one past the
     * last, which nothing can hold.
     */
    uint64_t page = fake_vfu_alloc(h.vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    uint32_t id;
    int rc = 0;
    for (id = 0; id < ERNIC_MAX_CQ; id++) {
        if (id == IH_ACQ_ID) {
            continue;
        }
        rc = limits_create_cq(&h, id, page);
        if (rc != 0) {
            break;
        }
    }
    check(T, id == ERNIC_MAX_CQ, "CREATE_CQ %u of %u failed with status %d", id,
          ERNIC_MAX_CQ, rc);
    rc = limits_create_cq(&h, ERNIC_MAX_CQ, page);
    check(T, rc == IONIC_RC_EQID, "CREATE_CQ %u: status %d, expected %d",
          ERNIC_MAX_CQ, rc, IONIC_RC_EQID);
    rc = limits_create_cq(&h, 0x00ffffffU, page);
    check(T, rc == IONIC_RC_EQID, "CREATE_CQ 0xffffff: status %d, expected %d",
          rc, IONIC_RC_EQID);

    /* And a QP under every id, with the rings in two halves of a page */
    for (id = 0; id < ERNIC_MAX_QP; id++) {
        rc = limits_create_qp(&h, id, 1, page, page + (IH_PAGE_SIZE / 2));
        if (rc != 0) {
            break;
        }
    }
    check(T, id == ERNIC_MAX_QP, "CREATE_QP %u of %u failed with status %d", id,
          ERNIC_MAX_QP, rc);
    rc = limits_create_qp(&h, ERNIC_MAX_QP, 1, page, page + (IH_PAGE_SIZE / 2));
    check(T, rc == IONIC_RC_EQID, "CREATE_QP %u: status %d, expected %d",
          ERNIC_MAX_QP, rc, IONIC_RC_EQID);

    /* The refused ones took nothing */
    const RdmaDeviceResources *res =
        &((PVRDMADev *)h.dev->pvrdma_handle)->rdma_dev_res;
    check(T,
          res->cq_tbl.used == ERNIC_MAX_CQ - 1U &&
              res->qp_tbl.used == ERNIC_MAX_QP,
          "%u CQs and %u QPs in the resource manager, expected %u and %u",
          res->cq_tbl.used, res->qp_tbl.used, ERNIC_MAX_CQ - 1U, ERNIC_MAX_QP);

    teardown(T, &h);
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

    if (!rc_setup(T, &r)) {
        return;
    }

    /* The receive a consumer is still waiting on when the peer goes away. */
    struct ih_sge dst = {.va = VA_B, .len = 64, .lkey = MR_B};
    ih_post_recv(h, &r.b, &dst, 1);
    check(T, cq_empty(h, &r.cqb), "a posted receive completed on its own");

    /* Armed, so the event the flush raises is visible. */
    ih_cq_arm(h, &r.cqb);
    check(T, eq_count(h, RC_CQ_B) == 0, "arming an empty CQ fired");

    /* Only Error flushes. A move to RTS must leave the receive posted. */
    check(T,
          ih_qp_modify(h, RC_QP_B,
                       &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                           .state = IH_QPS_RTS}) == 0,
          "MODIFY_QP to RTS failed");
    check(T, cq_empty(h, &r.cqb),
          "a state change other than Error raised a completion");

    /*
     * The state byte only means anything under IB_QP_STATE. A command that
     * changes something else leaves whatever the driver last put there, so a
     * flush keyed on the byte alone would tear down a live QP.
     */
    check(T,
          ih_qp_modify(h, RC_QP_B,
                       &(struct ih_qp_mod){.attr_mask = IH_QP_QKEY,
                                           .state = IH_QPS_ERR,
                                           .qkey_dest_qpn = 0x1234}) == 0,
          "MODIFY_QP of the qkey failed");
    check(T, cq_empty(h, &r.cqb),
          "a MODIFY_QP without IB_QP_STATE flushed the queue pair");

    check(T,
          ih_qp_modify(h, RC_QP_B,
                       &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                           .state = IH_QPS_ERR}) == 0,
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

    if (!setup(T, &h, "loopback")) {
        return;
    }

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

    check(T,
          ih_qp_modify(&h, 30,
                       &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                           .state = IH_QPS_ERR}) == 0,
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

    if (!rc_setup(T, &r)) {
        return;
    }

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

        struct ernic_stats *st = h->dev->stats;
        struct ernic_stats_instance inst = {
            .socket_path = "/tmp/harness.sock",
            .backend = "loopback",
            .pci_vid = 0x1dd8,
            .pci_did = 0x1002,
        };
        check(T,
              ernic_stats_set_file(st, path) == 0 &&
                  ernic_stats_set_instance(st, &inst) == 0,
              "can't point the statistics at a file");
        ernic_stats_set_connection(st, "connected");
        ernic_stats_device_reset(st);
        check(T, ernic_stats_write(st), "the stats file wasn't written");

        FILE *fp = fopen(path, "re");
        char text[16384] = "";
        if (check(T, fp != NULL, "no stats file")) {
            size_t n = fread(text, 1, sizeof(text) - 1, fp);
            text[n] = '\0';
            fclose(fp);
        }
        check(T, strstr(text, "SEND") != NULL && strstr(text, "connected"),
              "the stats file is missing the traffic");
        check(T, strstr(text, "\nLIF Statistics:\n") != NULL,
              "the device didn't add its section to the stats file");
        unlink(path);
        rmdir(dir);
    }

    rc_teardown(T, &r);
}

/* ---- Unreliable datagrams ---------------------------------------------- */

#define UD_QKEY 0x11111111U
#define UD_AH   5U
#define UD_SIP  0x0a000001U
#define UD_DIP  0x0a000002U

static void test_ud(void)
{
    const char *T = "ud";
    struct ih h;
    struct ih_cq cqc;
    struct ih_cq cqd;
    struct ih_qp c;
    struct ih_qp d;
    struct ih_qp rcq;
    struct ih_mr mr = {0};
    struct ih_cqe e;

    if (!setup(T, &h, "loopback")) {
        return;
    }

    bool ok = ih_cq_create(&h, &cqc, 3, 5) && ih_cq_create(&h, &cqd, 4, 5) &&
              ih_qp_create(&h, &c, 20, IH_QPT_UD, &cqc, &cqc, NULL) &&
              ih_qp_create(&h, &d, 21, IH_QPT_UD, &cqd, &cqd, NULL) &&
              ih_qp_create(&h, &rcq, 22, IH_QPT_RC, &cqd, &cqd, NULL) &&
              ih_mr_create(&h, &mr, IH_MRID(6, 0x06), UINT64_C(0x7f4000000000),
                           UINT64_C(2) * IH_PAGE_SIZE, IH_ACCESS_ALL, false);
    for (uint32_t qp = 20; ok && qp <= 21; qp++) {
        ok = ih_qp_modify(
                 &h, qp,
                 &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_QKEY,
                                     .state = IH_QPS_INIT,
                                     .qkey_dest_qpn = UD_QKEY}) == 0 &&
             ih_qp_modify(&h, qp,
                          &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                              .state = IH_QPS_RTR}) == 0 &&
             ih_qp_modify(
                 &h, qp,
                 &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_SQ_PSN,
                                     .state = IH_QPS_RTS}) == 0;
    }
    ok = ok && ih_ah_create(&h, UD_AH, UD_SIP, UD_DIP) == 0;
    if (!check(T, ok, "UD setup failed")) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    const uint64_t src_va = mr.va;
    const uint64_t dst_va = mr.va + 1024;
    uint8_t want[64];
    uint8_t got[IH_GRH_SIZE + 64];
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
    s.qkey = 0x22222222U;
    ih_post_send(&h, &c, &s);
    s.qkey = UD_QKEY;
    s.dest_qpn = 22;
    ih_post_send(&h, &c, &s);
    check(T, cq_poll_n(&h, &cqc, 2, &e), "refused UD sends did not complete");
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

    /*
     * A datagram the device refuses completes by its SQ index in error,
     * carrying the status, as an RC send's does by MSN. The NPG completion
     * used to be posted as success whatever the status, so a UD consumer
     * was told a refused send had gone.
     */
    struct ih_sge big[2] = {{.va = src_va, .len = 0xfffffff0U, .lkey = 0},
                            {.va = src_va, .len = 0x20, .lkey = 0}};
    struct ih_sge unreg = {.va = src_va, .len = 8, .lkey = 0x7e000099U};
    static const struct {
        const char *what;
        uint32_t status;
    } refused[] = {
        {"UD inline SEND longer than its WQE", IH_STS_LOCAL_LEN_ERR},
        {"UD SEND of more than 2^32 bytes", IH_STS_LOCAL_LEN_ERR},
        {"UD SEND naming an unregistered key", IH_STS_LOCAL_PROT_ERR},
    };
    s = (struct ih_send){.op = IH_OP_SEND,
                         .flags = IH_WQE_SIGNALLED,
                         .ah_id = UD_AH,
                         .dest_qpn = 21,
                         .qkey = UD_QKEY};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        struct ih_send r = s;

        if (i == 0) {
            r.flags |= IH_WQE_INLINE;
            r.inline_data = want;
            r.inline_len = 16;
            r.length = (1U << c.sq.stride_log2) - 32U + 1U;
        } else if (i == 1) {
            r.sge = big;
            r.nsge = 2;
        } else {
            r.sge = &unreg;
            r.nsge = 1;
        }
        send_id = ih_post_send(&h, &c, &r);
        if (check(T, ih_cq_poll(&h, &cqc, &e), "%s did not complete",
                  refused[i].what)) {
            check(T,
                  e.type == IH_CQE_TYPE_SEND_NPG && e.wqe_id == send_id &&
                      e.error && e.status_len == refused[i].status,
                  "%s: type %u wqe_id %" PRIu64 " status %u (error %d), "
                  "expected %u by SQ index",
                  refused[i].what, e.type, e.wqe_id, e.status_len, e.error,
                  refused[i].status);
        }
    }
    check(T, cq_empty(&h, &cqd), "a refused datagram was delivered");

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

/*
 * The SRQ's receives announced with the doorbell @db_qtype and @db_ring.
 * Each driver that posts to an SRQ has its own form, and a form the device
 * misses is a hang: the SRQ never learns of a receive, so a SEND to a QP on
 * it completes and its receive never does.
 */
static void test_srq_rung(const char *T, int db_qtype, uint8_t db_ring)
{
    struct ih h;
    struct ih_cq cqf;
    struct ih_cq cqg;
    struct ih_qp f;
    struct ih_qp g;
    struct ih_srq srq;
    struct ih_mr mr = {0};
    struct ih_cqe e;

    if (!setup(T, &h, "loopback")) {
        return;
    }

    int rc = ih_srq_create(&h, &srq, 4);
    if (!check(T, rc == 0, "CREATE_SRQ: %d", rc)) {
        teardown(T, &h);
        return;
    }
    check(T,
          srq.id >= IONIC_SRQ_QID_BASE &&
              srq.id < IONIC_SRQ_QID_BASE + ERNIC_MAX_SRQ,
          "SRQ id %#x is outside the SRQ range", srq.id);
    srq.db_qtype = db_qtype;
    srq.db_ring = db_ring;

    /*
     * On the CQ qtype, F's CQ takes the SRQ's id, which CQ ids may share:
     * only the ring says the doorbell is the SRQ's.
     */
    uint32_t cqf_id = db_qtype == IH_QTYPE_CQ ? srq.id : 5;
    bool ok = ih_cq_create(&h, &cqf, cqf_id, 5) &&
              ih_cq_create(&h, &cqg, 6, 5) &&
              ih_qp_create(&h, &f, 30, IH_QPT_RC, &cqf, &cqf, &srq) &&
              ih_qp_create(&h, &g, 31, IH_QPT_RC, &cqg, &cqg, NULL) &&
              ih_qp_connect(&h, &f, &g) &&
              ih_mr_create(&h, &mr, IH_MRID(7, 0x07), UINT64_C(0x7f5000000000),
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
            .va = mr.va + 512 + UINT64_C(64) * i, .len = 64, .lkey = mr.lkey};
        ids[i] = ih_post_srq_recv(&h, &srq, &dst, 1);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint8_t v[16];
        uint8_t got[16];
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
            ih_mr_read(&h, &mr, mr.va + 512 + UINT64_C(64) * i, got,
                       sizeof(got));
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
    ih_doorbell(&h, db_qtype, srq.id, db_ring, 1);

    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &cqf) == 0 && ih_cq_destroy(&h, &cqg) == 0,
          "DESTROY_CQ failed");
    teardown(T, &h);
}

/* The guest kernel's form: the RQ qtype, as for a QP's own RQ */
static void test_srq(void)
{
    test_srq_rung("srq", IH_QTYPE_RQ, 0);
}

/* The provider with ansible/files/patches/rdma-core/0005: the RQ qtype, but
 * with the SRQ ring it sets in every SRQ doorbell */
static void test_srq_rq_ring(void)
{
    test_srq_rung("srq-rq-ring", IH_QTYPE_RQ, IH_DB_RING_SRQ);
}

/* The provider as proposed upstream (rdma-core #1799): the CQ qtype, with
 * the SRQ ring */
static void test_srq_cq_ring(void)
{
    test_srq_rung("srq-cq-ring", IH_QTYPE_CQ, IH_DB_RING_SRQ);
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

/* An Ethernet queue's Q_INIT */
struct q_init_cmd {
    uint8_t qtype;
    uint32_t index;
    uint16_t intr;
    uint8_t depth_log2;
    uint64_t ring, cq, sg; /* guest addresses; sg 0 for none */
};

/* Q_INIT @q; returns the completion's status, or -1 if the devcmd never
 * completed. */
static int q_init_status(struct ih *h, const struct q_init_cmd *q)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16];

    cmd[0] = IH_CMD_Q_INIT;
    cmd[4] = q->qtype;
    ernic_put_le32(cmd + 8, q->index);
    ernic_put_le16(cmd + 14, q->intr);
    cmd[19] = q->depth_log2;
    ernic_put_le64(cmd + 20, q->ring);
    ernic_put_le64(cmd + 28, q->cq);
    ernic_put_le64(cmd + 36, q->sg);
    return ih_devcmd(h, cmd, comp, NULL, 0);
}

static bool q_init(struct ih *h, const struct q_init_cmd *q)
{
    return q_init_status(h, q) == IONIC_RC_SUCCESS;
}

/* Q_CONTROL ENABLE, which the driver sends once a queue is initialized. */
static bool q_enable(struct ih *h, uint8_t qtype, uint32_t index)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16];

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
    /* Allocated, not on the stack: the device keeps a pointer to it */
    struct tx_seen *seen = calloc(1, sizeof(*seen));

    if (seen == NULL) {
        check_failed(T, "out of memory");
        return;
    }
    if (!setup(T, &h, "loopback")) {
        free(seen);
        return;
    }

    const uint64_t txr = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t txc = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t txsg = fake_vfu_alloc(h.vfu, (size_t)8 * 8 * 16, 64);
    const uint64_t rxr = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t rxc = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t bufs = fake_vfu_alloc(h.vfu, (size_t)8 * 2048, 4096);
    /* Queues the device can't set up are refused, so the driver never rings
     * them: a ring size it can't shift by (UBSan reports a Q_INIT that
     * does), an unknown qtype, and an index past the LIF's queue count. */
    const struct q_init_cmd txq = {.qtype = IH_QTYPE_ETH_TXQ,
                                   .intr = TX_INTR,
                                   .depth_log2 = 3,
                                   .ring = txr,
                                   .cq = txc,
                                   .sg = txsg};
    const struct q_init_cmd rxq = {.qtype = IH_QTYPE_ETH_RXQ,
                                   .intr = RX_INTR,
                                   .depth_log2 = 3,
                                   .ring = rxr,
                                   .cq = rxc};
    struct q_init_cmd bad = txq;

    bad.depth_log2 = 32;
    int rc = q_init_status(&h, &bad);
    check(T, rc == IONIC_RC_ERANGE, "a 2^32-entry Tx ring got status %d", rc);
    bad = txq;
    bad.qtype = IONIC_QTYPE_ETH_MAX;
    rc = q_init_status(&h, &bad);
    check(T, rc == IONIC_RC_EQTYPE, "an unknown qtype got status %d", rc);
    bad = txq;
    bad.index = h.emu->profile.queues.eth;
    rc = q_init_status(&h, &bad);
    check(T, rc == IONIC_RC_EQID, "a Tx queue past the count got status %d",
          rc);
    check(T, q_init(&h, &txq), "Q_INIT of the Tx queue failed");
    check(T, q_init(&h, &rxq), "Q_INIT of the Rx queue failed");
    check(T, q_enable(&h, IH_QTYPE_ETH_TXQ, 0), "enabling the Tx queue failed");
    check(T, q_enable(&h, IH_QTYPE_ETH_RXQ, 0), "enabling the Rx queue failed");
    ih_intr_mask(&h, TX_INTR, false);
    ih_intr_mask(&h, RX_INTR, false);

    ionic_eth_emu_register_tx_filter(h.emu, tx_filter, seen);

    /* One frame in the descriptor, one split over two SG elements */
    uint8_t frame[200];
    fill(frame, sizeof(frame), 0x31);
    fake_vfu_write(h.vfu, bufs, frame, sizeof(frame));

    uint8_t desc[16] = {0};
    ernic_put_le64(desc, bufs << 12);
    ernic_put_le16(desc + 8, 120);
    fake_vfu_write(h.vfu, txr, desc, sizeof(desc));

    ernic_put_le64(desc, (bufs << 12) | 2U << 8);
    ernic_put_le16(desc + 8, sizeof(frame));
    fake_vfu_write(h.vfu, txr + 16, desc, sizeof(desc));
    uint8_t sg[2 * 16] = {0};
    ernic_put_le64(sg, bufs + 50);
    ernic_put_le16(sg + 8, 100);
    ernic_put_le64(sg + 16, bufs + 150);
    ernic_put_le16(sg + 24, 50);
    fake_vfu_write(h.vfu, txsg + UINT64_C(1) * 8 * 16, sg, sizeof(sg));

    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 0, 0, 2);
    check(T, seen->frames == 2, "the Tx filter saw %u frames", seen->frames);
    check(T,
          seen->len == sizeof(frame) &&
              memcmp(seen->frame, frame, sizeof(frame)) == 0,
          "the scatter/gather frame was not reassembled");
    uint8_t comp[16];
    fake_vfu_read(h.vfu, txc + 16, comp, sizeof(comp));
    check(T, (comp[15] & 0x80U) && comp[2] == 1,
          "no completion for the second Tx descriptor");
    check(T, fake_vfu_irq_count(h.vfu, TX_INTR) == 1,
          "Tx completions raised %u interrupts",
          fake_vfu_irq_count(h.vfu, TX_INTR));

    /* Frames handed in off-thread land in the guest's Rx buffers */
    for (unsigned i = 0; i < 2; i++) {
        memset(desc, 0, sizeof(desc));
        ernic_put_le16(desc + 6, i == 0 ? 2048 : 64);
        ernic_put_le64(desc + 8, bufs + UINT64_C(2048) * (i + 1));
        fake_vfu_write(h.vfu, rxr + UINT64_C(16) * i, desc, sizeof(desc));
    }
    ih_doorbell(&h, IH_QTYPE_ETH_RXQ, 0, 0, 2);

    check(T, ionic_eth_emu_queue_rx_frame(h.emu, frame, sizeof(frame)) == 0,
          "queueing an Rx frame failed");
    check(T, ionic_eth_emu_queue_rx_frame(h.emu, NULL, 10) == -EINVAL,
          "a NULL Rx frame was accepted");
    check(T, ionic_eth_emu_queue_rx_frame(h.emu, frame, 0) == -EINVAL,
          "an empty Rx frame was accepted");
    ih_poll(&h);

    uint8_t got[200];
    fake_vfu_read(h.vfu, bufs + 2048, got, sizeof(got));
    check(T, memcmp(got, frame, sizeof(frame)) == 0, "the Rx frame is wrong");
    fake_vfu_read(h.vfu, rxc, comp, sizeof(comp));
    check(T, (comp[15] & 0x80U) && ernic_get_le16(comp + 12) == sizeof(frame),
          "the Rx completion does not carry the frame length");
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 1, "Rx raised %u interrupts",
          fake_vfu_irq_count(h.vfu, RX_INTR));

    /*
     * A frame too big for the next buffer is dropped, not cut and not held.
     * Holding it would wedge the queue: head does not advance, so every
     * later pass reads the same descriptor and fails the same way, and no
     * frame behind it is ever delivered.
     */
    check(T, ionic_eth_emu_queue_rx_frame(h.emu, frame, sizeof(frame)) == 0,
          "queueing an Rx frame failed");
    ih_poll(&h);
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 1,
          "a frame that does not fit was delivered");

    /* ...and the next frame that does fit still gets through. */
    uint8_t small[64];
    fill(small, sizeof(small), 0x77);
    check(T, ionic_eth_emu_queue_rx_frame(h.emu, small, sizeof(small)) == 0,
          "queueing a second Rx frame failed");
    ih_poll(&h);
    check(T, fake_vfu_irq_count(h.vfu, RX_INTR) == 2,
          "an oversized frame wedged the Rx queue behind it");
    uint8_t got_small[64];
    fake_vfu_read(h.vfu, bufs + UINT64_C(2048) * 2, got_small,
                  sizeof(got_small));
    check(T, memcmp(got_small, small, sizeof(small)) == 0,
          "the frame behind the oversized one is wrong");

    /* Tx without a filter is a sink, and a doorbell for a queue that was
     * never initialized is ignored */
    ionic_eth_emu_register_tx_filter(h.emu, NULL, NULL);
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 0, 0, 3);
    check(T, seen->frames == 2, "Tx with no filter reached the filter");
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 3, 0, 1);
    ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 99, 0, 1);

    teardown(T, &h);
    free(seen);
}

/*
 * One Tx descriptor: a frame of @len bytes at the start of the buffers, its
 * last bytes in @nsge SG elements, the first two @sg_len bytes each. @fail is
 * the DMA refused, counted from the doorbell (the descriptor, the SG list,
 * the head, the elements, then the completion), or 0 for none.
 */
struct eth_tx_case {
    const char *what;
    uint16_t len;
    uint8_t nsge;
    uint16_t sg_len[2];
    unsigned fail;
    bool sent;
    bool completed;
};

static const struct eth_tx_case eth_tx_cases[] = {
    {"an unreadable frame", 120, 0, {0, 0}, 2, false, true},
    {"an unreadable SG list", 200, 2, {100, 50}, 2, false, true},
    {"an unreadable head", 200, 2, {100, 50}, 3, false, true},
    {"an unreadable SG element", 200, 2, {100, 50}, 4, false, true},
    {"an unwritable completion", 120, 0, {0, 0}, 3, true, false},
    {"SG elements as long as the frame", 150, 2, {100, 50}, 0, false, true},
    {"an empty SG element", 150, 2, {100, 0}, 0, true, true},
    {"too long an SG list", 200, TXQ_SG_MAX_ELEMS + 1, {0, 0}, 0, false, true},
};

static void test_eth_dma_faults(void)
{
    const char *T = "eth-dma";
    struct ih h;
    /* Allocated, not on the stack: the device keeps a pointer to it */
    struct tx_seen *seen = calloc(1, sizeof(*seen));
    uint8_t cmd[64] = {0};
    uint8_t comp[16];
    uint8_t desc[16];
    uint8_t buf[64];

    if (seen == NULL) {
        check_failed(T, "out of memory");
        return;
    }
    if (!setup(T, &h, "loopback")) {
        free(seen);
        return;
    }

    /* PORT_INIT succeeds even when the port information can't be written */
    const uint64_t info = fake_vfu_alloc(h.vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    cmd[0] = IH_CMD_PORT_INIT;
    ernic_put_le64(cmd + 8, info);
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T, ih_devcmd(&h, cmd, comp, NULL, 0) == 0,
          "PORT_INIT with unwritable port information failed");
    fake_vfu_read(h.vfu, info, buf, sizeof(buf));
    check(T, all_zero(buf, sizeof(buf)), "unwritable port information changed");

    /* An admin command the device can't read is read on the next doorbell.
     * ih_eth_admin() reports the first try's missing completion. */
    memset(cmd, 0, sizeof(cmd));
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T, ih_eth_admin(&h, cmd, comp) < 0,
          "an unreadable admin command completed");
    const uint32_t depth = 1U << h.eth_aq.depth_log2;
    ih_doorbell(&h, IH_QTYPE_ETH_ADMINQ, 0, 0,
                (uint16_t)(h.eth_aq.prod % depth));
    fake_vfu_read(h.vfu,
                  h.eth_acq.gpa + ((h.eth_acq.cons % depth) * UINT64_C(16)),
                  comp, sizeof(comp));
    check(T, (comp[15] & 0x80U) && comp[0] == 0,
          "the admin command did not complete on the next doorbell");
    h.eth_acq.cons++;
    check(T, ih_eth_admin(&h, cmd, comp) == 0, "the next admin command failed");

    const uint64_t txr = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t txc = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t txsg = fake_vfu_alloc(h.vfu, (size_t)8 * 8 * 16, 64);
    const uint64_t rxr = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t rxc = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t bufs = fake_vfu_alloc(h.vfu, 2048, 4096);
    const struct q_init_cmd txq = {.qtype = IH_QTYPE_ETH_TXQ,
                                   .intr = TX_INTR,
                                   .depth_log2 = 3,
                                   .ring = txr,
                                   .cq = txc,
                                   .sg = txsg};
    const struct q_init_cmd rxq = {.qtype = IH_QTYPE_ETH_RXQ,
                                   .intr = RX_INTR,
                                   .depth_log2 = 3,
                                   .ring = rxr,
                                   .cq = rxc};
    bool ok = q_init(&h, &txq) && q_init(&h, &rxq) &&
              q_enable(&h, IH_QTYPE_ETH_TXQ, 0) &&
              q_enable(&h, IH_QTYPE_ETH_RXQ, 0);
    if (!check(T, ok, "Tx and Rx queue setup failed")) {
        teardown(T, &h);
        free(seen);
        return;
    }
    ih_intr_mask(&h, RX_INTR, false);
    ionic_eth_emu_register_tx_filter(h.emu, tx_filter, seen);

    for (size_t i = 0; i < sizeof(eth_tx_cases) / sizeof(eth_tx_cases[0]);
         i++) {
        const struct eth_tx_case *c = &eth_tx_cases[i];
        uint8_t sg[2 * 16] = {0};
        unsigned frames = seen->frames;

        memset(desc, 0, sizeof(desc));
        ernic_put_le64(desc, bufs << 12 | (uint64_t)c->nsge << 8);
        ernic_put_le16(desc + 8, c->len);
        fake_vfu_write(h.vfu, txr + (16U * i), desc, sizeof(desc));
        for (size_t j = 0; j < 2; j++) {
            ernic_put_le64(sg + (16 * j), bufs + 50 + (100 * j));
            ernic_put_le16(sg + (16 * j) + 8, c->sg_len[j]);
        }
        fake_vfu_write(h.vfu, txsg + UINT64_C(8) * 16 * i, sg, sizeof(sg));

        fake_vfu_fail_nth_map(h.vfu, c->fail);
        ih_doorbell(&h, IH_QTYPE_ETH_TXQ, 0, 0, (uint16_t)(i + 1));
        fake_vfu_fail_nth_map(h.vfu, 0);
        check(T, (seen->frames != frames) == c->sent, "%s: the frame was %s",
              c->what, c->sent ? "dropped" : "sent");
        fake_vfu_read(h.vfu, txc + (16U * i), comp, sizeof(comp));
        check(T, ((comp[15] & 0x80U) && comp[2] == i) == c->completed,
              "%s: the descriptor %s", c->what,
              c->completed ? "did not complete" : "completed");
    }

    /* A received frame is dropped, and its buffer kept for the next, when
     * the descriptor can't be read, the frame or its completion written */
    static const uint8_t frame[64] = {0x5a};
    memset(desc, 0, sizeof(desc));
    ernic_put_le16(desc + 6, 2048);
    ernic_put_le64(desc + 8, bufs);
    fake_vfu_write(h.vfu, rxr, desc, sizeof(desc));
    ih_doorbell(&h, IH_QTYPE_ETH_RXQ, 0, 0, 1);
    for (unsigned fail = 1; fail <= 4; fail++) {
        unsigned irqs = fake_vfu_irq_count(h.vfu, RX_INTR);

        ionic_eth_emu_queue_rx_frame(h.emu, frame, sizeof(frame));
        fake_vfu_fail_nth_map(h.vfu, fail < 4 ? fail : 0);
        ih_poll(&h);
        fake_vfu_fail_nth_map(h.vfu, 0);
        check(T, (fake_vfu_irq_count(h.vfu, RX_INTR) != irqs) == (fail == 4),
              "Rx with DMA %u refused: %s", fail,
              fail < 4 ? "delivered" : "not delivered after refused ones");
    }

    check(T, fake_vfu_mapped(h.vfu) == 0, "guest memory left mapped");
    teardown(T, &h);
    free(seen);
}

/* ---- NVMe-oF over the GSI QP ------------------------------------------- */

#define NV_TRADDR  0xc0a8c801U /* 192.168.200.1 */
#define NV_PORT    4420U
#define NV_NQN     "nvmet-harness"
#define NV_BACKEND "nvmeof:size=1M,ip=192.168.200.1,port=4420,nqn=" NV_NQN

#define NV_GSI_CQ 1
#define NV_RC_CQ  2
#define NV_RC_QP  0x42U
#define NV_MR     IH_MRID(8, 0x08)
#define NV_VA     UINT64_C(0x7f6000000000)

/* Where things live in the MR */
#define NV_MAD_OFF     0x0000U
#define NV_GSI_RX_OFF  0x0200U
#define NV_CAP_OFF     0x0400U
#define NV_RC_RX_OFF   0x0480U
#define NV_CONNECT_OFF 0x0800U
#define NV_IDENT_OFF   0x1000U

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
    mad_hdr(m, 0x0010, UINT64_C(0x1234567800000001));
    ernic_put_be32(m + 24, 0xabcd0001U);
    ernic_put_be64(m + 32, ((uint64_t)0x0106 << 16) | NV_PORT);
    ernic_put_be64(m + 40, UINT64_C(0x0002c9030012abcd));
    ernic_put_be32(m + 56, (NV_RC_QP << 8) | 4);
    ernic_put_be32(m + 60, 4);
    ernic_put_be32(m + 64, 20U << 3);
    ernic_put_be32(m + 68, 0x123456U << 8);
    memset(m + 80, 0x11, 16); /* primary local GID: the guest */
    memset(m + 96, 0x22, 16); /* primary remote GID: the target */

    uint8_t *priv = m + 164;
    priv[1] = 4 << 4;
    ernic_put_be16(priv + 2, NV_PORT);
    ernic_put_be32(priv + 4 + 12, 0xc0a8c814U);
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
                        .qkey = 0x80010000U,
                        .sge = &tx,
                        .nsge = 1};

    ih_mr_write(h, mr, tx.va, mad, IB_MAD_SIZE);
    if (want_reply) {
        ih_post_recv(h, gsi, &rx, 1);
    }
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
    if (!want_reply) {
        return check(test, !got, "a MAD that needs no reply got one");
    }
    if (!check(test, got, "no GSI reply")) {
        return false;
    }
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
    uint8_t controller[16];
    uint8_t guest[16];
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

/* A keyed SGL data block: @len bytes at @addr in the MR @key names */
struct keyed_sgl {
    uint64_t addr;
    uint32_t len;
    uint32_t key;
};

static void capsule_init(uint8_t *c, uint8_t opcode, uint16_t cid,
                         struct keyed_sgl sgl)
{
    memset(c, 0, NVME_SQE_SIZE);
    c[NVME_SQE_OPCODE_OFF] = opcode;
    ernic_put_le16(c + NVME_SQE_CID_OFF, cid);

    /* A keyed SGL data block descriptor */
    uint8_t *d = c + NVME_SQE_DPTR_OFF;
    ernic_put_le64(d, sgl.addr);
    d[8] = (uint8_t)sgl.len;
    d[9] = (uint8_t)(sgl.len >> 8);
    d[10] = (uint8_t)(sgl.len >> 16);
    ernic_put_le32(d + 11, sgl.key);
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
#define P4_UPSTREAM_MAGIC 0xAAU

static bool p4_upstream_filter(void *ctx, const void *frame, size_t len)
{
    struct p4_upstream *u = ctx;
    const uint8_t *bytes = frame;

    u->offered++;
    if (len == 0 || bytes[0] != P4_UPSTREAM_MAGIC) {
        return false;
    }
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
    fake_vfu_write(h->vfu, txr + ((uint64_t)index * 16U), desc, sizeof(desc));
    ih_doorbell(h, IH_QTYPE_ETH_TXQ, 0, 0, (uint16_t)(index + 1));
}

static void test_p4_port(void)
{
    const char *T = "p4-port";
    struct ih h;
    /* Allocated, not on the stack: the device keeps a pointer to it */
    struct p4_upstream *up = calloc(1, sizeof(*up));
    struct ionic_eth_p4_upstream chain;
    struct p4_stats estats;
    struct ionic_eth_p4_stats pstats;

    if (up == NULL) {
        check_failed(T, "out of memory");
        return;
    }
    if (!setup(T, &h, "loopback")) {
        free(up);
        return;
    }

    const uint64_t txr = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t txc = fake_vfu_alloc(h.vfu, (size_t)8 * 16, 64);
    const uint64_t buf = fake_vfu_alloc(h.vfu, 4096, 4096);
    const struct q_init_cmd txq = {.qtype = IH_QTYPE_ETH_TXQ,
                                   .intr = TX_INTR,
                                   .depth_log2 = 3,
                                   .ring = txr,
                                   .cq = txc};
    check(T, q_init(&h, &txq), "Q_INIT of the Tx queue failed");
    ih_intr_mask(&h, TX_INTR, false);

    /* Stand in for the S3 control plane, which really does hold this slot. */
    ionic_eth_emu_register_tx_filter(h.emu, p4_upstream_filter, up);

    memset(&chain, 0, sizeof(chain));
    ionic_eth_emu_get_tx_filter(h.emu, &chain.fn, &chain.ctx);
    check(T, chain.fn == p4_upstream_filter && chain.ctx == up,
          "the registered Tx filter did not read back");

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        teardown(T, &h);
        free(up);
        return;
    }

    /* A port onto nothing is refused, and nothing is safe to let go of
     * or read. */
    check(T,
          ionic_eth_p4_attach(NULL, eng, &chain) == NULL &&
              ionic_eth_p4_attach(h.emu, NULL, &chain) == NULL,
          "a port was attached to nothing");
    ionic_eth_p4_detach(NULL);
    memset(&pstats, 0xAA, sizeof(pstats));
    ionic_eth_p4_get_stats(NULL, &pstats);
    check(T, pstats.to_guest == 0 && pstats.egress_errors == 0,
          "a port that is not there reported counters");
    ionic_eth_p4_get_stats(NULL, NULL);

    struct ionic_eth_p4 *port = ionic_eth_p4_attach(h.emu, eng, &chain);
    if (!check(T, port != NULL, "ionic_eth_p4_attach failed")) {
        p4_engine_destroy(eng);
        teardown(T, &h);
        free(up);
        return;
    }

    /* A frame the previous holder claims must not reach the pipeline. */
    p4_tx_one(&h, txr, buf, P4_UPSTREAM_MAGIC, 0);
    p4_engine_get_stats(eng, &estats);
    ionic_eth_p4_get_stats(port, &pstats);
    check(T, up->claimed == 1, "the chained filter claimed %u frames",
          up->claimed);
    check(T, estats.frames_in[P4_PORT_HOST] == 0,
          "a claimed frame reached the pipeline");
    check(T, pstats.to_upstream == 1, "the claimed frame was not counted");

    /* One it declines must, and with no backend attached its forward to the
     * uplink fails, which is the only observable the engine has today. */
    p4_tx_one(&h, txr, buf, 0xBBU, 1);
    p4_engine_get_stats(eng, &estats);
    ionic_eth_p4_get_stats(port, &pstats);
    check(T, up->offered == 2, "the chained filter saw %u frames", up->offered);
    check(T, up->claimed == 1,
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
    ionic_eth_p4_detach(port);
    ionic_eth_emu_get_tx_filter(h.emu, &chain.fn, &chain.ctx);
    check(T, chain.fn == p4_upstream_filter && chain.ctx == up,
          "detach did not restore the previous Tx filter");

    p4_tx_one(&h, txr, buf, P4_UPSTREAM_MAGIC, 2);
    check(T, up->claimed == 2,
          "the restored filter did not claim after detach");
    p4_engine_get_stats(eng, &estats);
    check(T, estats.frames_in[P4_PORT_HOST] == 1,
          "the pipeline ran after it was detached");

    p4_engine_destroy(eng);
    ionic_eth_emu_register_tx_filter(h.emu, NULL, NULL);
    teardown(T, &h);
    free(up);
}

/*
 * ionic_device_attach_p4(), as the server calls it for --p4, with the guest
 * given @scope: the port takes the Tx filter slot, chained to what held it,
 * and the guest gets the command surface for any scope but none.  A second
 * attach, or one with no engine, is refused.  The device's teardown takes
 * it all off again before the engine, which is the caller's, goes.
 */
static void p4_attach_one(const char *T, enum p4_scope scope)
{
    struct p4_engine *eng = p4_engine_create();
    /* Allocated, not on the stack: the device keeps a pointer to it */
    struct p4_upstream *up = calloc(1, sizeof(*up));
    char err[128] = "";
    struct ih h;

    if (!check(T, eng != NULL, "p4_engine_create failed") ||
        !check(T, up != NULL, "out of memory")) {
        p4_engine_destroy(eng);
        free(up);
        return;
    }
    if (!setup(T, &h, "loopback")) {
        p4_engine_destroy(eng);
        free(up);
        return;
    }

    /* Stand in for the S3 control plane, as test_p4_port() does. */
    ionic_eth_emu_register_tx_filter(h.emu, p4_upstream_filter, up);

    check(T,
          !ionic_device_attach_p4(h.dev->ionic, NULL, scope, err, sizeof(err)),
          "the device attached no engine");
    if (check(
            T,
            ionic_device_attach_p4(h.dev->ionic, eng, scope, err, sizeof(err)),
            "attach with scope %s failed: %s", p4_scope_name(scope), err)) {
        check(T, h.emu->tx_filter != p4_upstream_filter,
              "the port didn't take the Tx filter slot");
        if (scope == P4_SCOPE_NONE) {
            check(T, h.emu->p4_devcmd_fn == NULL,
                  "scope none gave the guest the command surface");
        } else {
            check(T, h.emu->p4_devcmd_fn == p4_devcmd_dispatch,
                  "scope %s didn't give the guest the command surface",
                  p4_scope_name(scope));
        }
        check(
            T,
            !ionic_device_attach_p4(h.dev->ionic, eng, scope, err, sizeof(err)),
            "the engine was attached twice");
    }

    teardown(T, &h);
    p4_engine_destroy(eng);
    free(up);
}

static void test_p4_attach(void)
{
    p4_attach_one("p4-attach", P4_SCOPE_NONE);
    p4_attach_one("p4-attach", P4_SCOPE_READ);
}

/* ---- In-network elementwise compute across three memory regions ------- */

#define FP8R_MR_A   IH_MRID(1, 0x03)
#define FP8R_MR_B   IH_MRID(2, 0x03)
#define FP8R_MR_C   IH_MRID(3, 0x03)
#define FP8R_VA_A   UINT64_C(0x7F2000000000)
#define FP8R_VA_B   UINT64_C(0x7F3000000000)
#define FP8R_VA_C   UINT64_C(0x7F4000000000)
#define FP8R_MR_LEN (UINT64_C(4) * IH_PAGE_SIZE)

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
    struct ih_mr mra;
    struct ih_mr mrb;
    struct ih_mr mrc;
    /* Longer than one streaming block, so the chunking is exercised and a
     * loop that dropped the offset would show up as a wrong tail. */
    enum { N = 5000 };
    static uint8_t a[N];
    static uint8_t b[N];
    static uint8_t got[N];

    if (!setup(T, &h, "loopback")) {
        return;
    }

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
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, N, mrc.lkey,
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
            uint8_t want = (i == 1234) ? 0x4CU : 0x44U;

            if (got[i] != want) {
                if (!wrong) {
                    first = i;
                }
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
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_MUL, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 3001, mrc.lkey,
                                 FP8R_VA_C) == 0,
          "the elementwise multiply failed");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, 3001), "read back from C");
    {
        unsigned wrong = 0;

        for (unsigned i = 0; i < 3001; i++) {
            if (got[i] != 0x4CU) { /* 2*3 is 6 */
                wrong++;
            }
        }
        check(T, wrong == 0, "%u of 3001 products are wrong", wrong);
    }

    /*
     * Key 0 is IONIC_DMA_LKEY, which bypasses the region table and treats
     * the address as a bus address. A guest-supplied request must not be
     * able to name it.
     */
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, 0, FP8R_VA_A, mrb.lkey,
                                 FP8R_VA_B, 64, mrc.lkey, FP8R_VA_C) == -EACCES,
          "key 0 was accepted as an operand");
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 64, 0,
                                 FP8R_VA_C) == -EACCES,
          "key 0 was accepted as a destination");

    /* A key that was never registered must fault, not read host memory. */
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, 0xDEADBEEFU, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 64, mrc.lkey,
                                 FP8R_VA_C) == -EACCES,
          "an unregistered operand key was accepted");
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 64, 0xDEADBEEFU,
                                 FP8R_VA_C) == -EACCES,
          "an unregistered destination key was accepted");

    /*
     * A length past the end of a region is refused before anything moves,
     * rather than part way through with a half-written destination.
     */
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey,
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
        (void)ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                     mrb.lkey, FP8R_VA_B + FP8R_MR_LEN - 8, 64,
                                     mrc.lkey, FP8R_VA_C);
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, probe, sizeof(probe)),
              "read C back");
        check(T, memcmp(probe, before, sizeof(probe)) == 0,
              "a refused request wrote to the destination anyway");
    }

    /* Bounds on the request itself. */
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, 0, mrc.lkey,
                                 FP8R_VA_C) == -EINVAL,
          "a zero length was accepted");
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, IONIC_FP8_MAP_MAX + 1,
                                 mrc.lkey, FP8R_VA_C) == -EINVAL,
          "a length over the cap was accepted");
    check(T,
          ionic_datapath_fp8_map(h.dp, 99, mra.lkey, FP8R_VA_A, mrb.lkey,
                                 FP8R_VA_B, 64, mrc.lkey, FP8R_VA_C) == -EINVAL,
          "an unknown operation was accepted");

    /* Registered memory the device then cannot reach: each operand's read
     * is one mapping, and the result's write is the third. */
    for (unsigned long fail = 1; fail <= 3; fail++) {
        fake_vfu_fail_nth_map(h.vfu, fail);
        check(T,
              ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                     mrb.lkey, FP8R_VA_B, 64, mrc.lkey,
                                     FP8R_VA_C) == -EFAULT,
              "a map whose DMA %lu failed did not fault", fail);
    }

    /*
     * The conversion's own checks, called as the extern calls it: fp16 in
     * A, fp8 out to C.  Each format is checked, and each length is checked
     * against its own region and its own element size.
     */
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP8_E4M3,
                                     mra.lkey, FP8R_VA_A, 0, mrc.lkey,
                                     FP8R_VA_C) == -EINVAL,
          "a conversion of nothing was accepted");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_COUNT, P4_FMT_FP8_E4M3,
                                     mra.lkey, FP8R_VA_A, 64, mrc.lkey,
                                     FP8R_VA_C) == -EINVAL &&
              ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, 0xFF, mra.lkey,
                                         FP8R_VA_A, 64, mrc.lkey,
                                         FP8R_VA_C) == -EINVAL,
          "a format there is none of was accepted");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP8_E4M3,
                                     mra.lkey, FP8R_VA_A, 63, mrc.lkey,
                                     FP8R_VA_C) == -EINVAL,
          "63 bytes of fp16 were accepted");
    check(T,
          ionic_datapath_fmt_convert(
              h.dp, P4_FMT_FP4_E2M1, P4_FMT_FP8_E4M3, mra.lkey, FP8R_VA_A,
              (IONIC_FP8_MAP_MAX / 2U) + 1U, mrc.lkey, FP8R_VA_C) == -EINVAL,
          "more elements than the cap were accepted");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP8_E4M3, P4_FMT_FP4_E2M1,
                                     mra.lkey, FP8R_VA_A, 63, mrc.lkey,
                                     FP8R_VA_C) == -EINVAL,
          "63 fp4 results, half a byte at the end, were accepted");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP8_E4M3, 0,
                                     FP8R_VA_A, 64, mrc.lkey,
                                     FP8R_VA_C) == -EACCES,
          "key 0 was accepted as a conversion's source");
    for (unsigned long fail = 1; fail <= 2; fail++) {
        fake_vfu_fail_nth_map(h.vfu, fail);
        check(T,
              ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP8_E4M3,
                                         mra.lkey, FP8R_VA_A, 64, mrc.lkey,
                                         FP8R_VA_C) == -EFAULT,
              "a conversion whose DMA %lu failed did not fault", fail);
    }
    check(T, fake_vfu_mapped(h.vfu) == 0, "guest memory left mapped");

    check(T, ih_mr_destroy(&h, &mra) == 0, "DESTROY_MR A failed");
    check(T, ih_mr_destroy(&h, &mrb) == 0, "DESTROY_MR B failed");
    check(T, ih_mr_destroy(&h, &mrc) == 0, "DESTROY_MR C failed");
    ih_mr_free(&mra);
    ih_mr_free(&mrb);
    ih_mr_free(&mrc);

    teardown(T, &h);
}

/* A second key over A's pages, so the same memory has two names. */
#define FP8R_MR_D IH_MRID(4, 0x03)
#define FP8R_VA_D UINT64_C(0x7F5000000000)

/* fp16 elements widened in place: more than one of the conversion's
 * chunks, so a device that wrote a chunk before reading the next would
 * have destroyed the next one's input. */
#define FP8O_N_ELEMS 1400U

/* How many of @n bytes at @va in @mr differ from @want. */
static unsigned fp8o_wrong(struct ih *h, const struct ih_mr *mr, uint64_t va,
                           const uint8_t *want, size_t n)
{
    static uint8_t got[FP8O_N_ELEMS * 4U];
    unsigned wrong = 0;

    if (n > sizeof(got) || !ih_mr_read(h, mr, va, got, n)) {
        return (unsigned)n;
    }
    for (size_t i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            wrong++;
        }
    }
    return wrong;
}

/*
 * A destination that overlaps its input. The device can't tell whether two
 * regions overlap, since two keys can name the same pages, so it reads all
 * the input before writing any of the result, and the answer is the one a
 * separate destination would get. Each case below once completed
 * successfully with the input overwritten part way through.
 */
static void test_fp8_overlap(void)
{
    const char *T = "fp8-overlap";
    struct ih h;
    struct ih_mr mra = {0};
    struct ih_mr mrb = {0};
    struct ih_mr mrd = {0};
    enum { N = 5000 };
    static uint8_t src[FP8O_N_ELEMS * 2U];
    static uint8_t want[FP8O_N_ELEMS * 4U];
    static uint8_t a[N];
    static uint8_t b[N];
    static uint8_t sum[N];

    if (!setup(T, &h, "loopback")) {
        return;
    }

    if (!check(T,
               ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, FP8R_MR_LEN,
                            IH_ACCESS_ALL, true) &&
                   ih_mr_create(&h, &mrb, FP8R_MR_B, FP8R_VA_B, FP8R_MR_LEN,
                                IH_ACCESS_ALL, false) &&
                   ih_mr_alias(&h, &mrd, &mra, FP8R_MR_D, FP8R_VA_D,
                               IH_ACCESS_ALL),
               "the memory regions were not registered")) {
        ih_mr_free(&mra);
        ih_mr_free(&mrb);
        ih_mr_free(&mrd);
        teardown(T, &h);
        return;
    }

    /* fp16 from 0.5 up to just under 1.0 in its smallest steps, so every
     * element differs from its neighbours and a misplaced one shows up. */
    for (size_t i = 0; i < FP8O_N_ELEMS; i++) {
        uint16_t v = (uint16_t)(0x3800U + (i % 1024U));

        src[2U * i] = (uint8_t)(v & 0xFFU);
        src[(2U * i) + 1U] = (uint8_t)(v >> 8);
    }
    check(T, p4_fmt_convert(P4_FMT_FP16, P4_FMT_FP32, src, FP8O_N_ELEMS, want),
          "the oracle refused the source buffer");

    /* fp16 to fp32 at the same address, through the same key: every byte
     * read is written twice over. */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the fp16 source");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP32, mra.lkey,
                                     FP8R_VA_A, sizeof(src), mra.lkey,
                                     FP8R_VA_A) == 0,
          "widening in place failed");
    {
        unsigned wrong = fp8o_wrong(&h, &mra, FP8R_VA_A, want, sizeof(want));

        check(T, wrong == 0, "widening in place got %u of %zu bytes wrong",
              wrong, sizeof(want));
    }

    /* The same, written through the second key: no comparison of keys and
     * addresses would call these two regions the same memory. */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the fp16 source again");
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP32, mra.lkey,
                                     FP8R_VA_A, sizeof(src), mrd.lkey,
                                     FP8R_VA_D) == 0,
          "widening through a second key failed");
    {
        unsigned wrong = fp8o_wrong(&h, &mra, FP8R_VA_A, want, sizeof(want));

        check(T, wrong == 0,
              "widening through a second key got %u of %zu bytes wrong", wrong,
              sizeof(want));
    }

    /*
     * map() with its destination one byte into the first operand: each
     * result lands on the operand byte after its own, which a device
     * streaming the operands would read as the next block's first input.
     */
    for (unsigned i = 0; i < N; i++) {
        a[i] = (uint8_t)(0x30U + (i % 16U)); /* E4M3 0.5 to 0.9375 */
    }
    memset(b, 0x38, sizeof(b)); /* 1.0 */
    check(T, p4_fp8_map(P4_FP8_OP_ADD, a, b, N, sum),
          "the oracle refused the operands");
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, a, sizeof(a)), "write A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, b, sizeof(b)), "write B");
    check(T,
          ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                 mrb.lkey, FP8R_VA_B, N, mra.lkey,
                                 FP8R_VA_A + 1) == 0,
          "a map into its own operand failed");
    {
        unsigned wrong = fp8o_wrong(&h, &mra, FP8R_VA_A + 1, sum, N);

        check(T, wrong == 0, "a map into its own operand got %u of %u wrong",
              wrong, (unsigned)N);
    }

    /* A source too long to stage in any format is refused before anything
     * is read. */
    check(T,
          ionic_datapath_fmt_convert(h.dp, P4_FMT_FP32, P4_FMT_FP16, mra.lkey,
                                     FP8R_VA_A, (IONIC_FP8_MAP_MAX * 4U) + 4U,
                                     mrd.lkey, FP8R_VA_D) == -EINVAL,
          "a source longer than the cap in any format was accepted");

    /*
     * No memory to stage the input in: the operation is refused, and
     * since nothing is written before the input is all read, the
     * destination is as it was. B is the destination here, primed with a
     * pattern neither operation would write.
     */
    {
        static uint8_t primed[FP8O_N_ELEMS * 4U];
        int rc;
        bool fired;

        memset(primed, 0x5A, sizeof(primed));
        check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, primed, sizeof(primed)),
              "prime B");

        s3_alloc_fail_arm(0);
        rc = ionic_datapath_fmt_convert(h.dp, P4_FMT_FP16, P4_FMT_FP32,
                                        mra.lkey, FP8R_VA_A, sizeof(src),
                                        mrb.lkey, FP8R_VA_B);
        fired = s3_alloc_fail_disarm();
        check(T, fired && rc == -ENOMEM,
              "a conversion that couldn't stage its source returned %d "
              "(allocation %s)",
              rc, fired ? "failed" : "not reached");

        s3_alloc_fail_arm(0);
        rc =
            ionic_datapath_fp8_map(h.dp, P4_FP8_OP_ADD, mra.lkey, FP8R_VA_A,
                                   mra.lkey, FP8R_VA_A, N, mrb.lkey, FP8R_VA_B);
        fired = s3_alloc_fail_disarm();
        check(T, fired && rc == -ENOMEM,
              "a map that couldn't stage its operands returned %d "
              "(allocation %s)",
              rc, fired ? "failed" : "not reached");

        check(T, fp8o_wrong(&h, &mrb, FP8R_VA_B, primed, sizeof(primed)) == 0,
              "an operation that couldn't stage its input wrote anyway");
    }

    check(T, fake_vfu_mapped(h.vfu) == 0, "guest memory left mapped");
    check(T, ih_mr_destroy(&h, &mrd) == 0, "DESTROY_MR D failed");
    check(T, ih_mr_destroy(&h, &mra) == 0, "DESTROY_MR A failed");
    check(T, ih_mr_destroy(&h, &mrb) == 0, "DESTROY_MR B failed");

    teardown(T, &h);
}

/* ---- A P4 program governing an RDMA operation -------------------------- */

#define P4R_CQ     7
#define P4R_QP_A   40
#define P4R_QP_B   41
#define P4R_N      3000U
#define P4R_MR_LEN (UINT64_C(4) * IH_PAGE_SIZE)

/* The bytes of a compiled fixture, or NULL. The caller frees them. */
static char *p4d_read_file(const char *name, size_t *out_len)
{
    char path[512];
    FILE *f;
    long n;
    char *buf;

    (void)snprintf(path, sizeof(path), "%s/%s", P4_FIXTURE_DIR, name);
    f = fopen(path, "rbe"); /* e: O_CLOEXEC */
    if (!f) {
        return NULL;
    }
    /* No fixture is larger than a program the engine takes. */
    n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n < 0 || (uint64_t)n > P4_PROGRAM_MAX || fseek(f, 0, SEEK_SET) != 0) {
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

    if (!buf) {
        return -ENOENT;
    }
    rc = p4_engine_load_begin(eng, n);
    if (rc == 0) {
        rc = p4_engine_load_chunk(eng, 0, buf, n);
    }
    if (rc == 0) {
        rc = p4_engine_load_commit(eng);
    }
    free(buf);
    return rc;
}

/* p4r_load(), with the one occurrence of @from in the fixture made @to. */
static int p4r_load_patched(struct p4_engine *eng, const char *name,
                            const char *from, const char *to)
{
    size_t n = 0;
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    size_t at;
    size_t out_len;
    char *buf = p4d_read_file(name, &n);
    char *out;
    char *hit;
    int rc;

    if (!buf) {
        return -ENOENT;
    }
    /* The fixture is not NUL-terminated, so search a copy that is. */
    out = calloc(n + 1, 1);
    if (!out) {
        free(buf);
        return -ENOMEM;
    }
    memcpy(out, buf, n);
    hit = strstr(out, from);
    if (!hit || strstr(hit + 1, from)) {
        free(out);
        free(buf);
        return -EINVAL;
    }
    at = (size_t)(hit - out);
    free(out);
    /* @from was found, so n >= flen, and with tlen bounded too, out_len
     * can't wrap. Nothing larger would load anyway. */
    out_len = n - flen + tlen;
    if (tlen > P4_PROGRAM_MAX || out_len > P4_PROGRAM_MAX) {
        free(buf);
        return -EINVAL;
    }
    out = malloc(out_len);
    if (!out) {
        free(buf);
        return -ENOMEM;
    }
    memcpy(out, buf, at);
    /* The fixture is counted, not NUL-terminated, and so is the result.
     * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    memcpy(out + at, to, tlen);
    memcpy(out + at + tlen, buf + at + flen, n - at - flen);
    n = out_len;
    free(buf);

    rc = p4_engine_load_begin(eng, n);
    if (rc == 0) {
        rc = p4_engine_load_chunk(eng, 0, out, n);
    }
    if (rc == 0) {
        rc = p4_engine_load_commit(eng);
    }
    free(out);
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
    struct ih_qp a;
    struct ih_qp b;
    struct ih_mr mra = {0};
    struct ih_mr mrb = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t opa[P4R_N];
    static uint8_t opb[P4R_N];
    static uint8_t got[P4R_N];

    if (!setup(T, &h, "loopback")) {
        return;
    }

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
    ionic_datapath_set_p4_engine(h.dp, eng);

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
        unsigned wrong = 0;
        unsigned first = 0;

        for (unsigned i = 0; i < P4R_N; i++) {
            uint8_t want = (i == 777) ? 0x4CU : 0x44U; /* 4+2=6, else 1+2=3 */

            if (got[i] != want) {
                if (!wrong) {
                    first = i;
                }
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
    check(T, got[0] == 0x40U && got[777] == 0x50U,
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
     * Two equal operands, but a WQE length that is not their sum: no
     * driver writes that, and it is refused before anything is reduced.
     */
    {
        struct ih_send bad = w;

        bad.length = (2U * P4R_N) - 2U;
        memset(got, 0xA5, sizeof(got));
        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "poison C before the mislabelled write");
        ih_post_send(&h, &a, &bad);
        check(T, ih_cq_poll(&h, &cq, &e) && e.error,
              "a reduction whose length is not its operands' completed");
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, 16),
              "read back C after the mislabelled write");
        check(T, got[0] == 0xA5U && got[15] == 0xA5U,
              "a refused reduction wrote to the destination");
    }

    /*
     * Only a write can be a reduction.  An RDMA READ the table also sends
     * to reduce_with is declined by the extern and done as the read it
     * is: C's bytes arrive in A.
     */
    {
        struct p4_update rd = up;
        struct ih_send r = one;

        rd.type = P4_UPDATE_INSERT;
        rd.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_READ;
        check(T, p4_engine_update(eng, &rd, err, sizeof(err)) == 0,
              "the READ entry was not installed: %s", err);
        memset(got, 0x5A, sizeof(got));
        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "fill C before the read");
        r.op = IH_OP_RDMA_READ;
        ih_post_send(&h, &a, &r);
        check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
              "a READ under a reduce entry did not complete");
        check(T, ih_mr_read(&h, &mra, FP8R_VA_A, got, sizeof(got)),
              "read back A");
        check(T, got[0] == 0x5AU && got[P4R_N - 1] == 0x5AU,
              "the READ under a reduce entry was not an ordinary read");
        check(T, ih_mr_write(&h, &mra, FP8R_VA_A, opa, sizeof(opa)),
              "restore A");
    }

    /*
     * A selector wider than the byte an operation fits in: the program
     * declared reduce_with's argument 16 bits wide, and an entry put
     * something in the upper byte.  That names no operation there is, so
     * the extern refuses it rather than reading the low byte alone.
     */
    {
        struct p4_engine *wide = p4_engine_create();

        if (check(T, wide != NULL, "p4_engine_create failed")) {
            check(T,
                  p4r_load_patched(wide, "fp8_policy.json", "\"bitwidth\" : 8",
                                   "\"bitwidth\" : 16") == 0,
                  "fp8_policy.json with a 16-bit selector did not load");
            up.type = P4_UPDATE_INSERT;
            up.params[0].len = 2;
            up.params[0].value[P4_MATCH_BYTES - 2] = 1;
            check(T, p4_engine_update(wide, &up, err, sizeof(err)) == 0,
                  "the wide entry was not installed: %s", err);
            up.params[0].len = 1;
            up.params[0].value[P4_MATCH_BYTES - 2] = 0;
            ionic_datapath_set_p4_engine(h.dp, wide);
            memset(got, 0xA5, sizeof(got));
            check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
                  "poison C before the wide selector");
            ih_post_send(&h, &a, &w);
            check(T, ih_cq_poll(&h, &cq, &e) && e.error,
                  "a selector with its upper byte set was not refused");
            check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, 16),
                  "read back C after the wide selector");
            check(T, got[0] == 0xA5U && got[15] == 0xA5U,
                  "a refused selector wrote to the destination");
            ionic_datapath_set_p4_engine(h.dp, eng);
            p4_engine_destroy(wide);
        }
    }

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
            ionic_datapath_set_p4_engine(h.dp, nodrop);
            ih_post_send(&h, &a, &w);
            check(T, ih_cq_poll(&h, &cq, &e) && e.error,
                  "an extern call without a drop was allowed to transfer "
                  "over its own result");
            ionic_datapath_set_p4_engine(h.dp, eng);
            p4_engine_destroy(nodrop);
        }
    }

    /* Taking the engine away restores the device's own behaviour. */
    ionic_datapath_set_p4_engine(h.dp, NULL);
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
    struct ih_qp a;
    struct ih_qp b;
    struct ih_mr mra;
    struct ih_mr mrb;
    struct ih_mr mrc;
    struct ih_cqe e;
    uint8_t comp[16];
    static uint8_t opa[P4R_N];
    static uint8_t opb[P4R_N];
    static uint8_t got[P4R_N];
    size_t prog_len = 0;
    char *prog;
    bool ok;
    struct ih_sge sge[2];
    struct ih_send w;

    if (!setup(T, &h, "loopback")) {
        return;
    }

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
    ionic_eth_emu_register_p4_handler(h.emu, p4_devcmd_dispatch, pd);
    ionic_datapath_set_p4_engine(h.dp, eng);

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
        ernic_put_le32(cmd + 16, 0x5eU);
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

            if (n > 4096) {
                n = 4096;
            }
            ernic_put_le64(hdr, off);
            ernic_put_le32(hdr + 8, (uint32_t)n);
            fake_vfu_write(h.vfu, req, hdr, sizeof(hdr));
            fake_vfu_write(h.vfu, req + sizeof(hdr), prog + off, n);
            if (!check(T,
                       p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM,
                               P4_OP_LOAD_CHUNK, req,
                               (uint32_t)(sizeof(hdr) + n), 0, 0,
                               comp) == P4_STS_OK,
                       "LOAD_CHUNK at %zu failed", off)) {
                break;
            }
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
    ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
         ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
         ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
         ih_qp_connect(&h, &a, &b) &&
         ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN, IH_ACCESS_ALL,
                      false) &&
         ih_mr_create(&h, &mrb, FP8R_MR_B, FP8R_VA_B, P4R_MR_LEN, IH_ACCESS_ALL,
                      false) &&
         ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN, IH_ACCESS_ALL,
                      true);
    if (!check(T, ok, "RDMA setup failed")) {
        goto out_mr;
    }

    memset(opa, 0x38, sizeof(opa)); /* 1.0 */
    memset(opb, 0x40, sizeof(opb)); /* 2.0 */
    opa[321] = 0x48;                /* 4.0, so position matters */
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, opa, sizeof(opa)), "write A");
    check(T, ih_mr_write(&h, &mrb, FP8R_VA_B, opb, sizeof(opb)), "write B");

    sge[0] = (struct ih_sge){.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey};
    sge[1] = (struct ih_sge){.va = FP8R_VA_B, .len = P4R_N, .lkey = mrb.lkey};
    w = (struct ih_send){
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
        unsigned wrong = 0;
        unsigned first = 0;

        for (unsigned i = 0; i < P4R_N; i++) {
            uint8_t want = (i == 321) ? 0x4CU : 0x44U;

            if (got[i] != want) {
                if (!wrong) {
                    first = i;
                }
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
     * A narrower grant refuses the wide operations, whatever the caller
     * claims, and keeps the narrow ones, but only for a caller that
     * claims no more than it was granted.
     */
    ionic_datapath_set_p4_engine(h.dp, NULL);
    p4_devcmd_destroy(pd);
    pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_READ);
    if (check(T, pd != NULL, "p4_devcmd_create failed")) {
        ionic_eth_emu_register_p4_handler(h.emu, p4_devcmd_dispatch, pd);
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
        check(T,
              p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_STATS, P4_OP_STATS_READ, 0, 0,
                      buf, (uint32_t)sizeof(struct p4_rpc_stats),
                      comp) == P4_STS_EPERM,
              "a read-only guest claiming load was let read the counters");
    }

    ionic_eth_emu_register_p4_handler(h.emu, NULL, NULL);
    p4_devcmd_destroy(pd);
    p4_engine_destroy(eng);
    teardown(T, &h);
}

/* ---- What the guest's way in refuses ------------------------------------ */

static int p4d_ident(struct ih *h, uint32_t len, uint64_t pa, uint8_t comp[16])
{
    uint8_t cmd[64] = {0};

    cmd[0] = IONIC_CMD_P4_IDENT;
    ernic_put_le32(cmd + 4, len);
    ernic_put_le64(cmd + 8, pa);
    return ih_devcmd(h, cmd, comp, NULL, 0);
}

static int p4d_query(struct ih *h, uint8_t entity, uint32_t buf_len,
                     uint64_t pa, uint32_t ep, uint8_t comp[16])
{
    uint8_t cmd[64] = {0};

    cmd[0] = IONIC_CMD_P4_QUERY;
    cmd[1] = entity;
    ernic_put_le32(cmd + 4, buf_len);
    ernic_put_le64(cmd + 8, pa);
    ernic_put_le32(cmd + 16, ep);
    return ih_devcmd(h, cmd, comp, NULL, 0);
}

/* LOAD_BEGIN for @total bytes, with the request at @req. */
static int p4d_begin(struct ih *h, uint64_t req, uint32_t total,
                     uint8_t comp[16])
{
    uint8_t body[8] = {0};

    ernic_put_le32(body, total);
    fake_vfu_write(h->vfu, req, body, sizeof(body));
    return p4d_rpc(h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, req,
                   (uint32_t)sizeof(body), 0, 0, comp);
}

/* LOAD_CHUNK of @n bytes at @off, saying it carries @claim of them. */
static int p4d_chunk(struct ih *h, uint64_t req, uint64_t off, const void *data,
                     uint32_t n, uint32_t claim, uint8_t comp[16])
{
    uint8_t hdr[16] = {0};

    ernic_put_le64(hdr, off);
    ernic_put_le32(hdr + 8, claim);
    fake_vfu_write(h->vfu, req, hdr, sizeof(hdr));
    fake_vfu_write(h->vfu, req + sizeof(hdr), data, n);
    return p4d_rpc(h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, req,
                   (uint32_t)sizeof(hdr) + n, 0, 0, comp);
}

static int p4d_commit(struct ih *h, uint8_t comp[16])
{
    return p4d_rpc(h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_COMMIT, 0, 0, 0,
                   0, comp);
}

/* A fixture, pushed down in one chunk and committed: COMMIT's status. */
static int p4d_load(struct ih *h, uint64_t req, const char *name,
                    uint8_t comp[16])
{
    size_t n = 0;
    char *prog = p4d_read_file(name, &n);
    int rc;

    if (!prog || n > 65536 - 16) {
        free(prog);
        return -1;
    }
    rc = p4d_begin(h, req, (uint32_t)n, comp);
    if (rc == P4_STS_OK) {
        rc = p4d_chunk(h, req, 0, prog, (uint32_t)n, (uint32_t)n, comp);
    }
    if (rc == P4_STS_OK) {
        rc = p4d_commit(h, comp);
    }
    free(prog);
    return rc;
}

/*
 * One ENTRY_UPDATE of the record @u followed by @nm key fields and @np
 * arguments, sent as @req_len bytes.  0 sends exactly what was built.
 */
static int p4d_entry(struct ih *h, uint64_t req,
                     const struct p4_rpc_entry_update *u,
                     const struct p4_rpc_entry_match *m, size_t nm,
                     const struct p4_rpc_entry_param *p, size_t np,
                     uint32_t req_len, uint8_t comp[16])
{
    uint8_t body[sizeof(*u) + (2 * sizeof(*m)) + (2 * sizeof(*p))];
    size_t len = sizeof(*u);

    if (nm > 2 || np > 2) {
        return -1;
    }
    memset(body, 0, sizeof(body));
    memcpy(body, u, sizeof(*u));
    for (size_t i = 0; i < nm; i++, len += sizeof(*m)) {
        memcpy(body + len, &m[i], sizeof(*m));
    }
    for (size_t i = 0; i < np; i++, len += sizeof(*p)) {
        memcpy(body + len, &p[i], sizeof(*p));
    }
    fake_vfu_write(h->vfu, req, body, len);
    return p4d_rpc(h, P4_SCOPE_WRITE, P4_EP_TABLE, P4_OP_ENTRY_UPDATE, req,
                   req_len ? req_len : (uint32_t)len, 0, 0, comp);
}

/* An insert into features.json's ternary table "acl", setting class @cls
 * for the source address @src exactly. */
static void p4d_acl(struct p4_rpc_entry_update *u,
                    struct p4_rpc_entry_match m[2],
                    struct p4_rpc_entry_param *p, uint32_t src, uint8_t cls)
{
    memset(u, 0, sizeof(*u));
    memset(m, 0, 2 * sizeof(*m));
    memset(p, 0, sizeof(*p));
    u->type = P4_WIRE_UPDATE_INSERT;
    u->priority = htole32(1); /* a ternary key needs one */
    u->nmatch = htole16(2);
    u->nparams = htole16(1);
    (void)snprintf(u->table, sizeof(u->table), "acl");
    (void)snprintf(u->action, sizeof(u->action), "set_class");
    m[0].len = htole16(4);
    ernic_put_be32(m[0].value + P4_DEVCMD_VALUE_MAX - 4, src);
    memset(m[0].mask + P4_DEVCMD_VALUE_MAX - 4, 0xFF, 4);
    m[1].len = htole16(1);
    p->len = htole16(1);
    p->value[P4_DEVCMD_VALUE_MAX - 1] = cls;
}

/*
 * Every way a guest's command can be refused, and each refusal's status:
 * malformed commands and payloads, lengths past what the device allows,
 * an operation above the scope asked for or granted, guest memory the
 * device cannot reach, and the engine's own refusals mapped onto the
 * closed set of statuses a guest sees.
 */
static void test_p4_devcmd_errors(void)
{
    const char *T = "p4-devcmd-errors";
    struct ih h;
    uint8_t comp[16];
    struct p4_rpc_entry_update u;
    struct p4_rpc_entry_match m[2];
    struct p4_rpc_entry_param pa;
    enum p4_scope scope;

    /* The scope names an operator gives, and those a log prints. */
    check(T,
          p4_scope_parse("write", &scope) && scope == P4_SCOPE_WRITE &&
              p4_scope_parse("none", &scope) && scope == P4_SCOPE_NONE,
          "a scope name was not parsed");
    check(T,
          !p4_scope_parse("all", &scope) && !p4_scope_parse(NULL, &scope) &&
              !p4_scope_parse("read", NULL),
          "a scope that is not one was parsed");
    check(T,
          strcmp(p4_scope_name(P4_SCOPE_NONE), "none") == 0 &&
              strcmp(p4_scope_name(P4_SCOPE_READ), "read") == 0 &&
              strcmp(p4_scope_name(P4_SCOPE_WRITE), "write") == 0 &&
              strcmp(p4_scope_name(P4_SCOPE_LOAD), "load") == 0,
          "a scope was given the wrong name");
    /* Deliberately out of range: the default branch that names one has to
     * be covered.
     * NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    check(T, strcmp(p4_scope_name((enum p4_scope)9), "?") == 0,
          "a scope there is none of was not named '?'");

    if (!setup(T, &h, "loopback")) {
        return;
    }

    struct p4_engine *eng = p4_engine_create();
    if (!check(T, eng != NULL, "p4_engine_create failed")) {
        teardown(T, &h);
        return;
    }
    check(T,
          p4_devcmd_create(NULL, eng, P4_SCOPE_LOAD) == NULL &&
              p4_devcmd_create(h.vfu, NULL, P4_SCOPE_LOAD) == NULL,
          "a command surface was made onto nothing");

    /* Granted nothing, the surface answers nothing, IDENT included. */
    struct p4_devcmd *pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_NONE);
    if (!check(T, pd != NULL, "p4_devcmd_create failed")) {
        p4_engine_destroy(eng);
        teardown(T, &h);
        return;
    }
    ionic_eth_emu_register_p4_handler(h.emu, p4_devcmd_dispatch, pd);
    const uint64_t buf = fake_vfu_alloc(h.vfu, 4096, 4096);
    const uint64_t req = fake_vfu_alloc(h.vfu, 65536, 4096);
    check(T, p4d_ident(&h, 32, buf, comp) == P4_STS_EPERM,
          "a surface granted nothing answered IDENT");
    p4_devcmd_destroy(pd);

    pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_LOAD);
    if (!check(T, pd != NULL, "p4_devcmd_create failed")) {
        ionic_eth_emu_register_p4_handler(h.emu, NULL, NULL);
        p4_engine_destroy(eng);
        teardown(T, &h);
        return;
    }
    ionic_eth_emu_register_p4_handler(h.emu, p4_devcmd_dispatch, pd);

    /* The handler without its context, and an opcode it was never sent:
     * the device routes only 70-72 here, so both come in directly. */
    {
        uint8_t cmd[64] = {0};

        cmd[0] = IONIC_CMD_P4_IDENT;
        memset(comp, 0xEE, sizeof(comp));
        p4_devcmd_dispatch(NULL, cmd, comp);
        check(T, comp[0] == P4_STS_EDEVICE && comp[15] == 0,
              "a command with no surface behind it drew status %u", comp[0]);
        cmd[0] = 73;
        p4_devcmd_dispatch(pd, cmd, comp);
        check(T, comp[0] == P4_STS_EOPCODE,
              "an unknown P4 opcode drew status %u", comp[0]);
    }

    /* IDENT: a length of nothing or of more than there is, a shorter one
     * that gets what it asked for and no more, and memory that is gone. */
    check(T, p4d_ident(&h, 0, buf, comp) == P4_STS_EINVAL,
          "IDENT of no bytes was accepted");
    check(T,
          p4d_ident(&h, (uint32_t)sizeof(struct p4_devcmd_ident) + 1, buf,
                    comp) == P4_STS_EINVAL,
          "IDENT longer than the identity was accepted");
    {
        uint8_t got[32];

        memset(got, 0xEE, sizeof(got));
        fake_vfu_write(h.vfu, buf, got, sizeof(got));
        check(T, p4d_ident(&h, 16, buf, comp) == P4_STS_OK,
              "a 16-byte IDENT failed");
        fake_vfu_read(h.vfu, buf, got, sizeof(got));
        check(T, ernic_get_le32(got) == 1 && got[16] == 0xEE && got[31] == 0xEE,
              "a 16-byte IDENT did not write exactly 16 bytes");
    }
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T, p4d_ident(&h, 32, buf, comp) == P4_STS_EFAULT,
          "IDENT into memory the device cannot reach did not fault");

    /* QUERY: a buffer too small for its header or over the limit, an
     * entity there is none of, one endpoint's operations picked out from
     * among the others, and memory that is gone. */
    check(T, p4d_query(&h, P4_QUERY_ROOT, 15, buf, 0, comp) == P4_STS_EINVAL,
          "a QUERY buffer smaller than its header was accepted");
    check(T, p4d_query(&h, P4_QUERY_ROOT, 4097, buf, 0, comp) == P4_STS_EINVAL,
          "a QUERY buffer over the limit was accepted");
    check(T, p4d_query(&h, 2, 4096, buf, 0, comp) == P4_STS_EINVAL,
          "a QUERY of an unknown entity was accepted");
    {
        struct p4_devcmd_query_data qd;
        struct p4_query_operation op;

        check(T,
              p4d_query(&h, P4_QUERY_ENDPOINT, 4096, buf, P4_EP_TABLE, comp) ==
                  P4_STS_OK,
              "QUERY(TABLE) failed");
        fake_vfu_read(h.vfu, buf, &qd, sizeof(qd));
        fake_vfu_read(h.vfu, buf + sizeof(qd), &op, sizeof(op));
        check(T,
              le32toh(qd.num_entries) == 1 &&
                  le32toh(op.id) == P4_OP_ENTRY_UPDATE &&
                  op.scope == P4_SCOPE_WRITE,
              "QUERY(TABLE) did not list exactly ENTRY_UPDATE, at write");
    }
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T, p4d_query(&h, P4_QUERY_ROOT, 4096, buf, 0, comp) == P4_STS_EFAULT,
          "QUERY into memory the device cannot reach did not fault");

    /* RPC: operations there are none of, a caller asking for less than an
     * operation needs or for a scope there is none of, buffers over the
     * limits, and a request that cannot be read. */
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, 9, 0, 0, 0, 0, comp) ==
              P4_STS_EOPCODE,
          "an unknown program operation was accepted");
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, 9, 1, 0, 0, 0, 0, comp) == P4_STS_EOPCODE,
          "an unknown endpoint was accepted");
    check(T,
          p4d_rpc(&h, P4_SCOPE_READ, P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, req, 8, 0,
                  0, comp) == P4_STS_EPERM,
          "a caller asking for read was let begin a load");
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD + 1, P4_EP_STATS, P4_OP_STATS_READ, 0, 0,
                  buf, (uint32_t)sizeof(struct p4_rpc_stats),
                  comp) == P4_STS_EPERM,
          "a caller claiming a scope there is none of was accepted");
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, req,
                  65537, 0, 0, comp) == P4_STS_ERANGE,
          "a request over 64 KiB was accepted");
    check(T,
          p4d_rpc(&h, P4_SCOPE_READ, P4_EP_STATS, P4_OP_STATS_READ, 0, 0, buf,
                  4097, comp) == P4_STS_ERANGE,
          "a response over 4 KiB was accepted");
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T, p4d_begin(&h, req, 64, comp) == P4_STS_EFAULT,
          "a request the device cannot read did not fault");

    /* The program endpoint's payloads. */
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, req, 4, 0,
                  0, comp) == P4_STS_EINVAL,
          "a LOAD_BEGIN shorter than its payload was accepted");
    check(T, p4d_begin(&h, req, 0, comp) == P4_STS_EINVAL,
          "an empty program was begun");
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, req, 8, 0,
                  0, comp) == P4_STS_EINVAL,
          "a LOAD_CHUNK shorter than its header was accepted");
    check(T, p4d_begin(&h, req, 64, comp) == P4_STS_OK, "LOAD_BEGIN failed");
    check(T, p4d_chunk(&h, req, 0, "{}", 2, 0, comp) == P4_STS_EINVAL,
          "a LOAD_CHUNK of no bytes was accepted");
    check(T, p4d_begin(&h, req, 64, comp) == P4_STS_OK, "LOAD_BEGIN failed");
    check(T, p4d_chunk(&h, req, 0, "{}", 2, 3, comp) == P4_STS_EINVAL,
          "a LOAD_CHUNK claiming more bytes than it carried was accepted");

    /* ABORT drops a load part way, so there is nothing to commit. */
    check(T, p4d_begin(&h, req, 64, comp) == P4_STS_OK, "LOAD_BEGIN failed");
    check(T, p4d_chunk(&h, req, 0, "{}", 2, 2, comp) == P4_STS_OK,
          "LOAD_CHUNK failed");
    check(T,
          p4d_rpc(&h, P4_SCOPE_LOAD, P4_EP_PROGRAM, P4_OP_LOAD_ABORT, 0, 0, 0,
                  0, comp) == P4_STS_OK,
          "LOAD_ABORT failed");
    check(T, p4d_commit(&h, comp) == P4_STS_EINVAL,
          "a load that was aborted was committed");

    /* A program the engine refuses: one using checksums, which the
     * engine names as unsupported, which the guest sees as EDEVICE. */
    check(T, p4d_load(&h, req, "simple_router.json", comp) == P4_STS_EDEVICE,
          "a program using checksums was not refused as EDEVICE");
    check(T, !p4_engine_has_program(eng), "a refused program was installed");

    /* The table endpoint, with no program loaded: the engine has no
     * tables, which the guest sees as ENOENT. */
    p4d_acl(&u, m, &pa, 0xC0A80001U, 1);
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_ENOENT,
          "an entry with no program loaded was not ENOENT");

    check(T, p4d_load(&h, req, "features.json", comp) == P4_STS_OK,
          "the guest could not load features.json");

    check(T,
          p4d_entry(&h, req, &u, m, 2, &pa, 1, (uint32_t)sizeof(u) - 1, comp) ==
              P4_STS_EINVAL,
          "an ENTRY_UPDATE shorter than its record was accepted");
    check(T,
          p4d_entry(&h, req, &u, m, 2, &pa, 1, (uint32_t)sizeof(u), comp) ==
              P4_STS_EINVAL,
          "an ENTRY_UPDATE missing its key fields was accepted");
    u.nmatch = htole16(P4_MAX_TABLE_KEY + 1);
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_EINVAL,
          "an ENTRY_UPDATE with too many key fields was accepted");
    p4d_acl(&u, m, &pa, 0xC0A80001U, 1);
    m[1].len = htole16(P4_DEVCMD_VALUE_MAX + 1);
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_EINVAL,
          "a key field longer than a value was accepted");
    p4d_acl(&u, m, &pa, 0xC0A80001U, 1);
    pa.len = htole16(P4_DEVCMD_VALUE_MAX + 1);
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_EINVAL,
          "an argument longer than a value was accepted");
    p4d_acl(&u, m, &pa, 0xC0A80001U, 1);
    (void)snprintf(u.table, sizeof(u.table), "ghost");
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_EINVAL,
          "an entry for a table there is none of was not EINVAL");

    /* acl holds 4: the fifth is ENOSPC. */
    for (uint32_t i = 0; i < 4; i++) {
        p4d_acl(&u, m, &pa, 0xC0A80001U + i, 1);
        check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_OK,
              "ACL row %u was not installed", i);
    }
    p4d_acl(&u, m, &pa, 0xC0A80005U, 1);
    check(T, p4d_entry(&h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_ENOSPC,
          "a fifth row in a 4-row table was not ENOSPC");

    /* The stats endpoint: a response too small for the counters, and one
     * the device cannot write. */
    check(T,
          p4d_rpc(&h, P4_SCOPE_READ, P4_EP_STATS, P4_OP_STATS_READ, 0, 0, buf,
                  (uint32_t)sizeof(struct p4_rpc_stats) - 1,
                  comp) == P4_STS_ERANGE,
          "a stats response too small for the counters was accepted");
    fake_vfu_fail_nth_map(h.vfu, 1);
    check(T,
          p4d_rpc(&h, P4_SCOPE_READ, P4_EP_STATS, P4_OP_STATS_READ, 0, 0, buf,
                  (uint32_t)sizeof(struct p4_rpc_stats), comp) == P4_STS_EFAULT,
          "a stats response the device cannot write did not fault");
    check(T, ernic_get_le32(comp + 4) == 0,
          "a failed command reported %u response bytes",
          ernic_get_le32(comp + 4));

    check(T, fake_vfu_mapped(h.vfu) == 0, "guest memory left mapped");
    ionic_eth_emu_register_p4_handler(h.emu, NULL, NULL);
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
#define P4C_N_ELEMS 1400U              /* fp8 results */
#define P4C_N_IN    (P4C_N_ELEMS * 2U) /* fp16 source bytes */

static void test_p4_cvt(void)
{
    const char *T = "p4-cvt";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a;
    struct ih_qp b;
    /* Zeroed: the setup chain below short-circuits, so a failure before
     * ih_mr_create() leaves a descriptor untouched and the failure path
     * still frees both.  ih_mr_free() does an unguarded free(). */
    struct ih_mr mra = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t src[P4C_N_IN];
    static uint8_t got[P4C_N_IN];
    static uint8_t want[P4C_N_ELEMS];

    if (!setup(T, &h, "loopback")) {
        return;
    }

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
    ionic_datapath_set_p4_engine(h.dp, eng);

    /*
     * A spread of fp16 encodings, little-endian as an array in guest
     * memory is written: 1.0, 2.0, 4.0, a NaN, an infinity that must
     * saturate rather than poison the buffer, and a zero.
     */
    {
        static const uint16_t pattern[6] = {0x3C00U, 0x4000U, 0x4400U,
                                            0x7E00U, 0x7C00U, 0x0000U};
        for (size_t i = 0; i < P4C_N_ELEMS; i++) {
            uint16_t v = pattern[i % 6U];

            src[2U * i] = (uint8_t)(v & 0xFFU);
            src[(2U * i) + 1U] = (uint8_t)(v >> 8);
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
    check(T, memcmp(got, src, sizeof(src)) == 0,
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

        for (uint32_t i = P4C_N_ELEMS; i < P4C_N_IN; i++) {
            if (got[i] != 0xA5U) {
                beyond_intact = false;
                break;
            }
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

    /*
     * A WQE whose length is not its gather list's is malformed -- a driver
     * always sums the list into it -- and is refused before anything is
     * converted.
     */
    {
        struct ih_send bad = w;

        bad.length = P4C_N_IN - 2;
        memset(got, 0xA5, sizeof(got));
        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "poison C before the mislabelled write");
        ih_post_send(&h, &a, &bad);
        check(T, ih_cq_poll(&h, &cq, &e) && e.error,
              "a conversion whose length is not its source's completed");
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, 16),
              "read back C after the mislabelled write");
        check(T, got[0] == 0xA5U && got[15] == 0xA5U,
              "a refused conversion wrote to the destination");
    }

    ionic_datapath_set_p4_engine(h.dp, NULL);
    p4_engine_destroy(eng);
    ih_mr_free(&mra);
    ih_mr_free(&mrc);
    teardown(T, &h);
}

/*
 * An action that converts and then asks for an arithmetic the request
 * cannot be: the extern writes the destination, the second call
 * declines, and the operation must FAIL rather than dispatch the
 * ordinary transfer over the result it has already produced.
 *
 * Without this the ordinary write lands on top of the converted bytes
 * and the guest is told the operation succeeded, which is the one thing
 * the "called an extern but did not suppress" check exists to prevent.
 */
static void test_p4_cvt_then_map(void)
{
    const char *T = "p4-cvt-map";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a;
    struct ih_qp b;
    struct ih_mr mra = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t src[P4C_N_IN];
    static uint8_t got[P4C_N_IN];

    if (!setup(T, &h, "loopback")) {
        return;
    }

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "setup failed")) {
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
    check(T, p4r_load(eng, "fp8_cvt_then_map.json") == 0,
          "the two-extern program did not load");
    ionic_datapath_set_p4_engine(h.dp, eng);

    /* Every element fp16 1.0, 0x3C00, low byte first. */
    memset(src, 0, sizeof(src));
    for (uint32_t i = 1; i < P4C_N_IN; i += 2) {
        src[i] = 0x3CU;
    }
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the source");
    memset(got, 0xA5, sizeof(got));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "poison C");

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.convert_then_reduce");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    up.nparams = 3;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FMT_FP16;
    up.params[1].len = 1;
    up.params[1].value[P4_MATCH_BYTES - 1] = P4_FMT_FP8_E4M3;
    up.params[2].len = 1;
    up.params[2].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_ADD;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the entry was not installed: %s", err);

    /* One gather entry: a conversion's shape, and not a reduction's. */
    struct ih_sge sge = {.va = FP8R_VA_A, .len = P4C_N_IN, .lkey = mra.lkey};
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        .sge = &sge,
        .nsge = 1,
    };

    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && e.error,
          "an operation whose extern ran and then declined was not failed");

    /*
     * And nothing transferred: the raw fp16 source must not be sitting
     * in the destination. The converted prefix may be there -- the
     * extern wrote it before the second call declined -- but the bytes
     * past it are what an ordinary write would have reached.
     */
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T,
          memcmp(got + P4C_N_ELEMS, src + P4C_N_ELEMS,
                 P4C_N_IN - P4C_N_ELEMS) != 0,
          "the ordinary transfer landed on top of the converted result");

    ionic_datapath_set_p4_engine(h.dp, NULL);
    p4_engine_destroy(eng);
    ih_mr_free(&mra);
    ih_mr_free(&mrc);
    teardown(T, &h);
}

/*
 * An action that drops and then asks for an arithmetic the request cannot
 * be: the extern declines, but the drop was made first and must stand.
 *
 * The decline is what lets an ordinary write through a table keyed on the
 * opcode, and it once let this one through too -- the pipeline stopped at
 * the call before the drop reached the datapath, so a write the program
 * had denied was dispatched and completed successfully.
 */
static void test_p4_drop_then_map(void)
{
    const char *T = "p4-drop-map";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a;
    struct ih_qp b;
    struct ih_mr mra = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t src[P4R_N];
    static uint8_t got[P4R_N];
    static uint8_t poison[P4R_N];

    if (!setup(T, &h, "loopback")) {
        return;
    }

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "setup failed")) {
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
    check(T, p4r_load(eng, "fp8_drop_then_map.json") == 0,
          "the drop-then-map program did not load");
    ionic_datapath_set_p4_engine(h.dp, eng);

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.drop_then_reduce");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    up.nparams = 1;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_ADD;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the entry was not installed: %s", err);

    memset(src, 0x38, sizeof(src));
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the source");
    memset(poison, 0xA5, sizeof(poison));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, poison, sizeof(poison)),
          "poison C");

    /* One gather entry: not a reduction's shape, so map() declines. */
    struct ih_sge sge = {.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey};
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        .sge = &sge,
        .nsge = 1,
    };

    /* A dropped operation fails without transferring, as it does when
     * nothing declines. */
    ih_post_send(&h, &a, &w);
    if (check(T, ih_cq_poll(&h, &cq, &e),
              "the dropped write did not complete")) {
        check(T, e.error && e.status_len == IH_STS_LOCAL_QP_OPER,
              "the dropped write: status %u (error %d), expected %d",
              e.status_len, e.error, IH_STS_LOCAL_QP_OPER);
    }
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T, memcmp(got, poison, sizeof(got)) == 0,
          "a write the program dropped before the extern declined was "
          "dispatched anyway");

    ionic_datapath_set_p4_engine(h.dp, NULL);
    p4_engine_destroy(eng);

    check(T, ih_qp_destroy(&h, &a) == 0, "DESTROY_QP A failed");
    check(T, ih_qp_destroy(&h, &b) == 0, "DESTROY_QP B failed");
    check(T, ih_mr_destroy(&h, &mra) == 0, "DESTROY_MR A failed");
    check(T, ih_mr_destroy(&h, &mrc) == 0, "DESTROY_MR C failed");
    ih_mr_free(&mra);
    ih_mr_free(&mrc);
    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");

    teardown(T, &h);
}

#define P4D_CQ_D 8
#define P4D_QP_C 42
#define P4D_QP_D 43

/*
 * A program that denies an operation, with no extern anywhere: nothing is
 * transferred, and the work request fails with a local QP operation error,
 * on RC and UD alike, signalled or not.
 *
 * A successful completion would tell an RC sender its write had landed,
 * and leave its peer waiting for data that never comes.
 */
static void test_p4_deny(void)
{
    const char *T = "p4-deny";
    struct ih h;
    struct ih_cq cq;
    struct ih_cq cqd;
    struct ih_qp a;
    struct ih_qp b;
    struct ih_qp c;
    struct ih_qp d;
    struct ih_mr mra = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    static uint8_t src[P4R_N];
    static uint8_t got[P4R_N];
    static uint8_t poison[P4R_N];

    if (!setup(T, &h, "loopback")) {
        return;
    }

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_cq_create(&h, &cqd, P4D_CQ_D, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_qp_create(&h, &c, P4D_QP_C, IH_QPT_UD, &cq, &cq, NULL) &&
              ih_qp_create(&h, &d, P4D_QP_D, IH_QPT_UD, &cqd, &cqd, NULL) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    for (uint32_t qp = P4D_QP_C; ok && qp <= P4D_QP_D; qp++) {
        ok = ih_qp_modify(
                 &h, qp,
                 &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_QKEY,
                                     .state = IH_QPS_INIT,
                                     .qkey_dest_qpn = UD_QKEY}) == 0 &&
             ih_qp_modify(&h, qp,
                          &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                              .state = IH_QPS_RTR}) == 0 &&
             ih_qp_modify(
                 &h, qp,
                 &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_SQ_PSN,
                                     .state = IH_QPS_RTS}) == 0;
    }
    ok = ok && ih_ah_create(&h, UD_AH, UD_SIP, UD_DIP) == 0;
    if (!check(T, ok, "setup failed")) {
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
    check(T, p4r_load(eng, "rdma_deny.json") == 0,
          "the deny program did not load");
    ionic_datapath_set_p4_engine(h.dp, eng);

    memset(src, 0x38, sizeof(src));
    check(T, ih_mr_write(&h, &mra, FP8R_VA_A, src, sizeof(src)),
          "write the source");
    memset(poison, 0xA5, sizeof(poison));
    check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, poison, sizeof(poison)),
          "poison C");

    struct ih_sge sge = {.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey};
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        .sge = &sge,
        .nsge = 1,
    };

    /* With the table empty, the default action lets the write through:
     * the program runs, and denies nothing it has not been told to. */
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && !e.error,
          "the write did not complete with no entry");
    check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)), "read back C");
    check(T, memcmp(got, src, sizeof(got)) == 0,
          "with no entry the write did not land");

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.acl");
    (void)snprintf(up.action, sizeof(up.action), "Ing.deny");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the WRITE entry was not installed: %s", err);
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_SEND;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the SEND entry was not installed: %s", err);

    /*
     * Denied, signalled and then not: the device posts a send completion
     * for every work request, and a failed one must reach the guest
     * whether or not it asked to be told of success.
     */
    for (int sig = 1; sig >= 0; sig--) {
        const char *what = sig ? "a signalled write" : "an unsignalled write";

        check(T, ih_mr_write(&h, &mrc, FP8R_VA_C, poison, sizeof(poison)),
              "poison C");
        w.flags = sig ? IH_WQE_SIGNALLED : 0;
        ih_post_send(&h, &a, &w);
        if (check(T, ih_cq_poll(&h, &cq, &e),
                  "%s the program denied did "
                  "not complete",
                  what)) {
            check(T,
                  e.type == IH_CQE_TYPE_SEND_MSN && e.error &&
                      e.status_len == IH_STS_LOCAL_QP_OPER,
                  "%s the program denied: type %u status %u (error %d), "
                  "expected %d",
                  what, e.type, e.status_len, e.error, IH_STS_LOCAL_QP_OPER);
        }
        check(T, ih_mr_read(&h, &mrc, FP8R_VA_C, got, sizeof(got)),
              "read back C");
        check(T, memcmp(got, poison, sizeof(got)) == 0,
              "%s the program denied was transferred", what);
    }

    /*
     * A denied datagram fails the same way, by its SQ index: it was never
     * sent, which is not the same as being lost on the way. A receive is
     * posted at D so that a datagram the device did deliver would show.
     */
    struct ih_sge dst = {
        .va = FP8R_VA_C, .len = IH_GRH_SIZE + 64, .lkey = mrc.lkey};
    struct ih_sge dgram = {.va = FP8R_VA_A, .len = 64, .lkey = mra.lkey};
    struct ih_send s = {.op = IH_OP_SEND,
                        .flags = IH_WQE_SIGNALLED,
                        .ah_id = UD_AH,
                        .dest_qpn = P4D_QP_D,
                        .qkey = UD_QKEY,
                        .sge = &dgram,
                        .nsge = 1};

    ih_post_recv(&h, &d, &dst, 1);
    uint64_t send_id = ih_post_send(&h, &c, &s);
    if (check(T, ih_cq_poll(&h, &cq, &e),
              "a datagram the program denied did not complete")) {
        check(T,
              e.type == IH_CQE_TYPE_SEND_NPG && e.wqe_id == send_id &&
                  e.error && e.status_len == IH_STS_LOCAL_QP_OPER,
              "a datagram the program denied: type %u wqe_id %" PRIu64
              " status %u (error %d), expected %d by SQ index",
              e.type, e.wqe_id, e.status_len, e.error, IH_STS_LOCAL_QP_OPER);
    }
    check(T, cq_empty(&h, &cqd), "a datagram the program denied was delivered");

    ionic_datapath_set_p4_engine(h.dp, NULL);
    p4_engine_destroy(eng);
    ih_mr_free(&mra);
    ih_mr_free(&mrc);
    teardown(T, &h);
}

/* The clock in p4-log-limit, which moves only when told to. */
static uint64_t p4l_now;

static uint64_t p4l_now_ms(void)
{
    return p4l_now;
}

/* Denied writes, and refused commands, sent in a flood */
#define P4L_FLOOD (3U * ERNIC_LOG_LIMIT_BURST)

/*
 * A flood of P4L_FLOOD commands through the command surface, each of which
 * @send sends and checks was refused: a burst of lines in one interval, and
 * in the next, the line @report counting the rest before the next one, which
 * starts with @line.
 */
static void p4l_flood(const char *T, struct ih *h, const char *what,
                      bool (*send)(struct ih *h, uint64_t req), uint64_t req,
                      const char *report, const char *line)
{
    unsigned long base = fake_vfu_log_count(h->vfu, LOG_WARNING);

    for (unsigned i = 0; i < P4L_FLOOD; i++) {
        if (!check(T, send(h, req), "%s %u was not refused", what, i)) {
            return;
        }
    }
    check(
        T,
        fake_vfu_log_count(h->vfu, LOG_WARNING) - base == ERNIC_LOG_LIMIT_BURST,
        "%u %s in one interval logged %lu lines, not %u", P4L_FLOOD, what,
        fake_vfu_log_count(h->vfu, LOG_WARNING) - base, ERNIC_LOG_LIMIT_BURST);

    p4l_now += ERNIC_LOG_LIMIT_MS;
    base = fake_vfu_log_count(h->vfu, LOG_WARNING);
    check(T, send(h, req), "the next interval's %s was not refused", what);
    check(T, fake_vfu_log_count(h->vfu, LOG_WARNING) - base == 2,
          "the next interval's %s logged %lu lines, not 2", what,
          fake_vfu_log_count(h->vfu, LOG_WARNING) - base);
    check(T, strcmp(fake_vfu_log_text(h->vfu, LOG_WARNING, 1), report) == 0,
          "the %s held back were reported as '%s', not '%s'", what,
          fake_vfu_log_text(h->vfu, LOG_WARNING, 1), report);
    check(T,
          strncmp(fake_vfu_log_text(h->vfu, LOG_WARNING, 0), line,
                  strlen(line)) == 0,
          "the next interval's %s was logged as '%s'", what,
          fake_vfu_log_text(h->vfu, LOG_WARNING, 0));
}

/* An update to a table features.json doesn't have. */
static bool p4l_send_update(struct ih *h, uint64_t req)
{
    struct p4_rpc_entry_update u;
    struct p4_rpc_entry_match m[2];
    struct p4_rpc_entry_param pa;
    uint8_t comp[16];

    p4d_acl(&u, m, &pa, 0xC0A80001U, 1);
    (void)snprintf(u.table, sizeof(u.table), "ghost");
    return p4d_entry(h, req, &u, m, 2, &pa, 1, 0, comp) == P4_STS_EINVAL;
}

/* A commit with no load begun. */
static bool p4l_send_commit(struct ih *h, uint64_t req)
{
    uint8_t comp[16];

    (void)req;
    return p4d_commit(h, comp) == P4_STS_EINVAL;
}

/*
 * p4-log-limit's floods through @pd, the command surface: refused table
 * updates, with a program loaded so that they get as far as the engine, and
 * refused commits; and then an unknown opcode, which has a limit of its own,
 * so the floods don't hold it back.
 */
static void p4l_devcmd(const char *T, struct ih *h, struct p4_devcmd *pd)
{
    const uint64_t req = fake_vfu_alloc(h->vfu, 65536, 4096);
    char report[FAKE_VFU_LOG_LEN];
    uint8_t comp[16];

    p4_devcmd_set_clock(pd, p4l_now_ms);
    if (!check(T, p4d_load(h, req, "features.json", comp) == P4_STS_OK,
               "the guest could not load features.json")) {
        return;
    }

    (void)snprintf(report, sizeof(report),
                   "p4_devcmd: %u more refused table update(s) not logged",
                   P4L_FLOOD - ERNIC_LOG_LIMIT_BURST);
    p4l_flood(T, h, "refused updates", p4l_send_update, req, report,
              "p4_devcmd: guest table update refused: ");
    (void)snprintf(report, sizeof(report),
                   "p4_devcmd: %u more refused program(s) not logged",
                   P4L_FLOOD - ERNIC_LOG_LIMIT_BURST);
    p4l_flood(T, h, "refused commits", p4l_send_commit, req, report,
              "p4_devcmd: guest program refused: ");

    /* The device routes only 70-72 here, so the unknown opcode comes in
     * directly. */
    uint8_t cmd[64] = {0};
    unsigned long base = fake_vfu_log_count(h->vfu, LOG_WARNING);

    cmd[0] = 73;
    p4_devcmd_dispatch(pd, cmd, comp);
    check(T,
          fake_vfu_log_count(h->vfu, LOG_WARNING) - base == 1 &&
              strcmp(fake_vfu_log_text(h->vfu, LOG_WARNING, 0),
                     "p4_devcmd: unknown opcode=73") == 0,
          "an unknown opcode after the floods logged %lu lines, the last '%s'",
          fake_vfu_log_count(h->vfu, LOG_WARNING) - base,
          fake_vfu_log_text(h->vfu, LOG_WARNING, 0));
}

/*
 * A guest that loops on something the device logs gets a burst of lines
 * through in each interval, and the next line says how many were held back:
 * through the data path, for work requests the program denies, and through
 * the command surface, for refused table updates and programs and unknown
 * opcodes.  Unlimited, each was a line per request, with nothing to stop a
 * guest from filling the host's disk.
 *
 * Both run on the test's clock, so a flood stays in one interval until the
 * test moves the clock on to the next.
 */
static void test_p4_log_limit(void)
{
    const char *T = "p4-log-limit";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a;
    struct ih_qp b;
    struct ih_mr mra = {0};
    struct ih_mr mrc = {0};
    struct ih_cqe e;
    struct p4_update up;
    char err[160];
    unsigned long base;

    if (!setup(T, &h, "loopback")) {
        return;
    }
    /* Set before any work reaches the data path; the loopback backend has
     * no receive threads to read it. */
    p4l_now = UINT64_C(1000000);
    h.dp->now_ms = p4l_now_ms;

    bool ok = ih_cq_create(&h, &cq, P4R_CQ, 6) &&
              ih_qp_create(&h, &a, P4R_QP_A, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_create(&h, &b, P4R_QP_B, IH_QPT_RC, &cq, &cq, NULL) &&
              ih_qp_connect(&h, &a, &b) &&
              ih_mr_create(&h, &mra, FP8R_MR_A, FP8R_VA_A, P4R_MR_LEN,
                           IH_ACCESS_ALL, false) &&
              ih_mr_create(&h, &mrc, FP8R_MR_C, FP8R_VA_C, P4R_MR_LEN,
                           IH_ACCESS_ALL, true);
    if (!check(T, ok, "setup failed")) {
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
    check(T, p4r_load(eng, "rdma_deny.json") == 0,
          "the deny program did not load");
    ionic_datapath_set_p4_engine(h.dp, eng);

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.acl");
    (void)snprintf(up.action, sizeof(up.action), "Ing.deny");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = IH_OP_RDMA_WRITE;
    check(T, p4_engine_update(eng, &up, err, sizeof(err)) == 0,
          "the WRITE entry was not installed: %s", err);

    struct ih_sge sge = {.va = FP8R_VA_A, .len = P4R_N, .lkey = mra.lkey};
    struct ih_send w = {
        .op = IH_OP_RDMA_WRITE,
        .flags = IH_WQE_SIGNALLED,
        .remote_va = FP8R_VA_C,
        .rkey = mrc.lkey,
        .sge = &sge,
        .nsge = 1,
    };

    /*
     * A flood of denials in one interval: a burst of lines saying so, and a
     * burst saying that work requests completed in error, no more.
     */
    base = fake_vfu_log_count(h.vfu, LOG_WARNING);
    for (unsigned i = 0; i < P4L_FLOOD; i++) {
        ih_post_send(&h, &a, &w);
        if (!check(T,
                   ih_cq_poll(&h, &cq, &e) && e.error &&
                       e.status_len == IH_STS_LOCAL_QP_OPER,
                   "denied write %u did not fail as a denial", i)) {
            break;
        }
    }
    check(T,
          fake_vfu_log_count(h.vfu, LOG_WARNING) - base ==
              (unsigned long)2 * ERNIC_LOG_LIMIT_BURST,
          "%u denials in one interval logged %lu lines, not %u", P4L_FLOOD,
          fake_vfu_log_count(h.vfu, LOG_WARNING) - base,
          2 * ERNIC_LOG_LIMIT_BURST);

    /*
     * The next interval's first denial is logged, and its error completion,
     * each after a line that counts the ones that weren't.
     */
    char denials[FAKE_VFU_LOG_LEN];
    char errors[FAKE_VFU_LOG_LEN];

    (void)snprintf(denials, sizeof(denials),
                   "ionic_datapath: %u more P4 denial(s) not logged",
                   P4L_FLOOD - ERNIC_LOG_LIMIT_BURST);
    (void)snprintf(errors, sizeof(errors),
                   "ionic_datapath: %u more error completion(s) not logged",
                   P4L_FLOOD - ERNIC_LOG_LIMIT_BURST);

    /* The last four, newest first: the counts are whole lines, and the
     * others are checked by how they start. */
    const struct {
        const char *text;
        bool whole;
    } want_last[] = {
        {"ionic_datapath: CQ ", false},
        {errors, true},
        {"ionic_datapath: QP ", false},
        {denials, true},
    };

    p4l_now += ERNIC_LOG_LIMIT_MS;
    base = fake_vfu_log_count(h.vfu, LOG_WARNING);
    ih_post_send(&h, &a, &w);
    check(T, ih_cq_poll(&h, &cq, &e) && e.error,
          "the next interval's denied write did not fail");
    check(T, fake_vfu_log_count(h.vfu, LOG_WARNING) - base == 4,
          "the next interval's denial logged %lu lines, not 4",
          fake_vfu_log_count(h.vfu, LOG_WARNING) - base);
    for (size_t i = 0; i < sizeof(want_last) / sizeof(want_last[0]); i++) {
        const char *got = fake_vfu_log_text(h.vfu, LOG_WARNING, i);
        const char *want = want_last[i].text;

        check(T,
              want_last[i].whole ? strcmp(got, want) == 0
                                 : strncmp(got, want, strlen(want)) == 0,
              "line %zu from the end was '%s', not '%s'", i, got, want);
    }

    ionic_datapath_set_p4_engine(h.dp, NULL);

    struct p4_devcmd *pd = p4_devcmd_create(h.vfu, eng, P4_SCOPE_LOAD);
    if (check(T, pd != NULL, "p4_devcmd_create failed")) {
        ionic_eth_emu_register_p4_handler(h.emu, p4_devcmd_dispatch, pd);
        p4l_devcmd(T, &h, pd);
        ionic_eth_emu_register_p4_handler(h.emu, NULL, NULL);
        p4_devcmd_destroy(pd);
    }
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
    struct ih_cq gcq;
    struct ih_cq rcq;
    struct ih_qp gsi;
    struct ih_qp rc;
    struct ih_mr mr = {0};
    uint8_t mad[IB_MAD_SIZE];
    uint8_t reply[IB_MAD_SIZE];

    if (!setup(T, &h, NV_BACKEND)) {
        return;
    }

    /* The driver creates QP 1 as UD, and that id is what makes it GSI */
    bool ok = ih_cq_create(&h, &gcq, NV_GSI_CQ, 5) &&
              ih_cq_create(&h, &rcq, NV_RC_CQ, 5) &&
              ih_qp_create(&h, &gsi, 1, IH_QPT_UD, &gcq, &gcq, NULL) &&
              ih_qp_create(&h, &rc, NV_RC_QP, IH_QPT_RC, &rcq, &rcq, NULL) &&
              ih_mr_create(&h, &mr, NV_MR, NV_VA, UINT64_C(2) * IH_PAGE_SIZE,
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
    mad_hdr(mad, 0x0014, UINT64_C(0x1234567800000001));
    ernic_put_be32(mad + 24, 0xabcd0001U);
    ernic_put_be32(mad + 28, local_comm);
    gsi_exchange(T, &h, &gsi, &gcq, &mr, mad, false, reply);

    /* Point the RC QP at the controller and connect the admin queue */
    uint8_t hdr[64];
    size_t hdr_len = ih_roce_hdr_v4(hdr, sizeof(hdr), 0xc0a8c814U, NV_TRADDR);
    ok = ih_qp_modify(&h, NV_RC_QP,
                      &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                          .state = IH_QPS_INIT}) == 0 &&
         ih_qp_modify(&h, NV_RC_QP,
                      &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_AV |
                                                       IH_QP_DEST_QPN,
                                          .state = IH_QPS_RTR,
                                          .qkey_dest_qpn = target_qpn,
                                          .hdr = hdr,
                                          .hdr_len = (uint32_t)hdr_len}) == 0 &&
         ih_qp_modify(&h, NV_RC_QP,
                      &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                          .state = IH_QPS_RTS}) == 0;
    check(T, ok, "moving the RC QP to RTS failed");

    uint8_t data[NVMEOF_CONNECT_DATA_SIZE] = {0};
    memcpy(data + NVMEOF_CONNECT_HOSTID_OFF, "0123456789abcdef", 16);
    ernic_put_le16(data + NVMEOF_CONNECT_CNTLID_OFF, 0xffff);
    snprintf((char *)data + NVMEOF_CONNECT_SUBNQN_OFF, 256, "%s", NV_NQN);
    snprintf((char *)data + NVMEOF_CONNECT_HOSTNQN_OFF, 256, "%s",
             "nqn.2014-08.org.nvmexpress:uuid:harness");
    ih_mr_write(&h, &mr, mr.va + NV_CONNECT_OFF, data, sizeof(data));

    uint8_t cap[NVME_SQE_SIZE];
    capsule_init(cap, NVME_ADMIN_FABRICS, 1,
                 (struct keyed_sgl){.addr = mr.va + NV_CONNECT_OFF,
                                    .len = sizeof(data),
                                    .key = NV_MR});
    cap[NVMEOF_SQE_FCTYPE_OFF] = NVMEOF_FCTYPE_CONNECT;
    ernic_put_le16(cap + NVMEOF_CONNECT_QID_OFF, 0);
    ernic_put_le16(cap + NVMEOF_CONNECT_SQSIZE_OFF, 31);
    int sts = nvme_exchange(T, &h, &rc, &rcq, &mr, cap);
    check(T, sts == 0, "Connect status %#x", (unsigned)sts);

    /* Identify Controller writes through the keyed SGL into the MR */
    capsule_init(cap, NVME_ADMIN_IDENTIFY, 2,
                 (struct keyed_sgl){.addr = mr.va + NV_IDENT_OFF,
                                    .len = NVME_IDENTIFY_SIZE,
                                    .key = NV_MR});
    ernic_put_le32(cap + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
    sts = nvme_exchange(T, &h, &rc, &rcq, &mr, cap);
    check(T, sts == 0, "Identify status %#x", (unsigned)sts);
    char subnqn[32] = "";
    ih_mr_read(&h, &mr, mr.va + NV_IDENT_OFF + 768, subnqn, sizeof(subnqn) - 1);
    check(T, strcmp(subnqn, NV_NQN) == 0, "Identify subnqn is '%s'", subnqn);

    /*
     * Key 0 is IONIC_DMA_LKEY, which the data path resolves as a raw guest
     * bus address without the MR table. Any guest process can put it in a
     * keyed SGL, so the controller must refuse it: aim an Identify at a page
     * no MR covers and check the page is untouched.
     */
    static uint8_t raw_page[IH_PAGE_SIZE];
    uint64_t raw = fake_vfu_alloc(h.vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    if (check(T, raw != 0, "out of guest memory for the unregistered page")) {
        memset(raw_page, 0xee, sizeof(raw_page));
        fake_vfu_write(h.vfu, raw, raw_page, sizeof(raw_page));
        capsule_init(cap, NVME_ADMIN_IDENTIFY, 3,
                     (struct keyed_sgl){
                         .addr = raw, .len = NVME_IDENTIFY_SIZE, .key = 0});
        ernic_put_le32(cap + NVME_SQE_CDW10_OFF, NVME_ID_CNS_CTRL);
        sts = nvme_exchange(T, &h, &rc, &rcq, &mr, cap);
        check(T, (sts & 0xff) == NVME_SC_DATA_XFER_ERROR,
              "Identify through key 0 status %#x", (unsigned)sts);
        fake_vfu_read(h.vfu, raw, raw_page, sizeof(raw_page));
        unsigned changed = 0;
        for (size_t i = 0; i < sizeof(raw_page); i++) {
            changed += raw_page[i] != 0xee;
        }
        check(T, changed == 0,
              "Identify through key 0 wrote %u bytes no MR covers", changed);
    }

    /* DREQ draws a DREP */
    mad_hdr(mad, 0x0015, UINT64_C(0x1234567800000002));
    ernic_put_be32(mad + 24, 0xabcd0001U);
    ernic_put_be32(mad + 28, local_comm);
    ernic_put_be32(mad + 32, target_qpn << 8);
    if (gsi_exchange(T, &h, &gsi, &gcq, &mr, mad, true, reply)) {
        check(T, ernic_get_be16(reply + 16) == 0x0016,
              "the DREQ drew attribute %#x", ernic_get_be16(reply + 16));
    }

    check(T, ih_qp_destroy(&h, &rc) == 0 && ih_qp_destroy(&h, &gsi) == 0,
          "DESTROY_QP failed");
    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &gcq) == 0 && ih_cq_destroy(&h, &rcq) == 0,
          "DESTROY_CQ failed");
    teardown(T, &h);
}

/*
 * The device takes no more NVMe-oF queues than its memory-region table can
 * register for, IONIC_MR_PER_QUEUE each. The server checks the options
 * with ionic_datapath_nvmeof_cfg_check() at startup, and attaching refuses
 * them too, so a caller that skipped the check gets no controller.
 */
static void test_nvmeof_queue_limit(void)
{
    static const char T[] = "nvmeof-queues";
    struct nvmeof_target_cfg cfg;
    char err[256];
    char want[256];
    struct ih h;

    nvmeof_target_cfg_defaults(&cfg);
    cfg.size = 1U << 20;

    cfg.max_queues = IONIC_MAX_MR_QUEUES;
    err[0] = '\0';
    check(T, ionic_datapath_nvmeof_cfg_check(&cfg, err, sizeof(err)),
          "%d queues refused: %s", IONIC_MAX_MR_QUEUES, err);

    cfg.max_queues = IONIC_MAX_MR_QUEUES + 1;
    err[0] = '\0';
    snprintf(want, sizeof(want),
             "queues must be 1..%d, one per %d memory regions in a table of "
             "%u (got '%d')",
             IONIC_MAX_MR_QUEUES, IONIC_MR_PER_QUEUE, ERNIC_MAX_MR,
             IONIC_MAX_MR_QUEUES + 1);
    check(T, !ionic_datapath_nvmeof_cfg_check(&cfg, err, sizeof(err)),
          "%d queues accepted", IONIC_MAX_MR_QUEUES + 1);
    check(T, strcmp(err, want) == 0, "refused with '%s', not '%s'", err, want);

    if (!check(T, ih_create(&h, "none"), "device not created")) {
        return;
    }
    err[0] = '\0';
    check(T, !ionic_datapath_attach_nvmeof(h.dp, &cfg, err, sizeof(err)),
          "a controller with %d queues attached", IONIC_MAX_MR_QUEUES + 1);
    check(T, strcmp(err, want) == 0, "attach refused with '%s', not '%s'", err,
          want);

    cfg.max_queues = IONIC_MAX_MR_QUEUES;
    check(T, ionic_datapath_attach_nvmeof(h.dp, &cfg, err, sizeof(err)),
          "a controller with %d queues did not attach: %s", IONIC_MAX_MR_QUEUES,
          err);
    teardown(T, &h);
}

/* ---- The S3 store's RDMA data plane ------------------------------------- */

#define S3H_BACKEND    "s3:size=1M"
#define S3H_TARGET_IP  0xc0a8c801U /* 192.168.200.1, the default */
#define S3H_GUEST_IP   0xc0a8c809U /* 192.168.200.9 */
#define S3H_PORT       9000U
#define S3H_GUEST_PORT 44001U
#define S3H_ISS        0x1000U /* the store's first initial sequence number */
#define S3H_TCP_SYN    0x02U
#define S3H_TCP_PSH    0x08U
#define S3H_TCP_ACK    0x10U
#define S3H_MR         IH_MRID(10, 0x0a)
#define S3H_VA         UINT64_C(0x7f7000000000)
#define S3H_LEN        64U

/*
 * One TCP connection from the guest to the store. Its segments go straight
 * to the Tx filter the store registers, which is where the Ethernet Tx path
 * would hand them; the store's answers are queued for the guest's Rx and
 * never read, since everything this test checks is in guest memory.
 */
struct s3h_conn {
    ionic_eth_tx_filter_fn fn;
    void *ctx;
    uint32_t seq;
};

/* Send one segment. The store doesn't verify checksums, so none are set. */
static bool s3h_segment(struct s3h_conn *c, uint8_t flags, const char *data)
{
    static const uint8_t guest_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t f[14 + 20 + 20 + 512];
    size_t len = data ? strlen(data) : 0;

    if (len > sizeof(f) - 54) {
        return false;
    }
    memset(f, 0, sizeof(f));
    memset(f, 0xff, 6); /* broadcast, which the store accepts */
    memcpy(f + 6, guest_mac, sizeof(guest_mac));
    ernic_put_be16(f + 12, 0x0800);

    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ernic_put_be16(ip + 2, (uint16_t)(40 + len));
    ip[8] = 64;
    ip[9] = 6; /* TCP */
    ernic_put_be32(ip + 12, S3H_GUEST_IP);
    ernic_put_be32(ip + 16, S3H_TARGET_IP);

    uint8_t *tcp = ip + 20;
    ernic_put_be16(tcp, S3H_GUEST_PORT);
    ernic_put_be16(tcp + 2, S3H_PORT);
    ernic_put_be32(tcp + 4, c->seq);
    /* Acknowledge the SYN-ACK and nothing after it: the answers are unread */
    ernic_put_be32(tcp + 8, (flags & S3H_TCP_ACK) ? S3H_ISS + 1 : 0);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    ernic_put_be16(tcp + 14, 0xffff);
    if (len) {
        /* A TCP payload, not a string
         * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
        memcpy(tcp + 20, data, len);
    }

    c->seq += (uint32_t)len + ((flags & S3H_TCP_SYN) ? 1U : 0U);
    return c->fn(c->ctx, f, 54 + len);
}

/* An RDMA GET or PUT of S3H_LEN bytes, naming (rkey, addr) in its token. */
static bool s3h_request(struct s3h_conn *c, const char *method, const char *key,
                        uint32_t rkey, uint64_t addr)
{
    struct s3_token tok;
    char hex[S3_TOKEN_HEX_LEN + 1];
    char req[384];

    memset(&tok, 0, sizeof(tok));
    tok.transport = S3_TRANSPORT_RC;
    tok.qp_num = 2;
    tok.gid[10] = 0xff;
    tok.gid[11] = 0xff;
    ernic_put_be32(tok.gid + 12, S3H_GUEST_IP);
    tok.rkey = rkey;
    tok.remote_addr = addr;
    tok.length = S3H_LEN;
    tok.port_num = 1;
    s3_token_encode(&tok, hex);

    /* What tests/s3_rdma_client.c sends: a Content-Length with no body */
    int n =
        snprintf(req, sizeof(req),
                 "%s /ernic/%s HTTP/1.1\r\n"
                 "Host: 192.168.200.1:9000\r\n"
                 "x-amz-rdma-token: %s\r\n"
                 "Content-Length: %u\r\n\r\n",
                 method, key, hex, strcmp(method, "PUT") == 0 ? S3H_LEN : 0U);
    return n > 0 && (size_t)n < sizeof(req) &&
           s3h_segment(c, S3H_TCP_PSH | S3H_TCP_ACK, req);
}

/*
 * The store moves object bytes by the rkey in the client's token. Key 0 is
 * IONIC_DMA_LKEY, which the data path resolves as a raw guest bus address
 * without the MR table, and any guest process can put it in a token, so the
 * store must refuse it in both directions. The registered-key requests
 * around the key-0 ones show the connection was serving throughout.
 */
static void test_s3(void)
{
    const char *T = "s3";
    struct ih h;
    struct ih_mr mr = {0};
    struct s3h_conn c = {.seq = 0x2000U};
    static uint8_t raw_page[IH_PAGE_SIZE];
    uint8_t pattern[S3H_LEN];
    uint8_t got[S3H_LEN];
    uint8_t zero[S3H_LEN] = {0};
    unsigned changed = 0;

    if (!setup(T, &h, S3H_BACKEND)) {
        return;
    }

    ionic_eth_emu_get_tx_filter(h.emu, &c.fn, &c.ctx);
    uint64_t raw = fake_vfu_alloc(h.vfu, IH_PAGE_SIZE, IH_PAGE_SIZE);
    if (!check(T,
               c.fn != NULL && raw != 0 &&
                   ih_mr_create(&h, &mr, S3H_MR, S3H_VA, IH_PAGE_SIZE,
                                IH_ACCESS_ALL, false),
               "S3 setup failed")) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    for (unsigned i = 0; i < S3H_LEN; i++) {
        pattern[i] = (uint8_t)((i * 7) + 1);
    }
    ih_mr_write(&h, &mr, S3H_VA, pattern, S3H_LEN);
    ih_mr_write(&h, &mr, S3H_VA + 0xc00, zero, S3H_LEN);
    memset(raw_page, 0xee, sizeof(raw_page));
    fake_vfu_write(h.vfu, raw, raw_page, sizeof(raw_page));

    check(T, s3h_segment(&c, S3H_TCP_SYN, NULL), "the store passed on a SYN");

    /* A PUT out of the region and a GET back into it, by its key */
    check(T,
          s3h_request(&c, "PUT", "obj", mr.lkey, S3H_VA) &&
              s3h_request(&c, "GET", "obj", mr.lkey, S3H_VA + 0x800),
          "the store passed on a request");
    ih_mr_read(&h, &mr, S3H_VA + 0x800, got, S3H_LEN);
    check(T, memcmp(got, pattern, S3H_LEN) == 0,
          "an RDMA PUT and GET by a registered key did not round-trip");

    /* A GET by key 0 would write the object into a page no MR covers */
    check(T, s3h_request(&c, "GET", "obj", 0, raw),
          "the store passed on a request");
    fake_vfu_read(h.vfu, raw, raw_page, sizeof(raw_page));
    for (size_t i = 0; i < sizeof(raw_page); i++) {
        changed += raw_page[i] != 0xee;
    }
    check(T, changed == 0, "a GET by key 0 wrote %u bytes no MR covers",
          changed);

    /* A PUT by key 0 would store that page, for a GET to read back */
    check(T,
          s3h_request(&c, "PUT", "leak", 0, raw) &&
              s3h_request(&c, "GET", "leak", mr.lkey, S3H_VA + 0xc00),
          "the store passed on a request");
    ih_mr_read(&h, &mr, S3H_VA + 0xc00, got, S3H_LEN);
    check(T, memcmp(got, zero, S3H_LEN) == 0,
          "a PUT by key 0 stored memory no MR covers");

    check(T, s3h_request(&c, "GET", "obj", mr.lkey, S3H_VA + 0x400),
          "the store passed on a request");
    ih_mr_read(&h, &mr, S3H_VA + 0x400, got, S3H_LEN);
    check(T, memcmp(got, pattern, S3H_LEN) == 0,
          "the store stopped serving after the key-0 requests");

    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    teardown(T, &h);
}

/* ---- Backends with no mesh --------------------------------------------- */

/* Never called: no backend here has a mesh to call it from */
static bool mesh_none_recv(void *opaque, uint32_t src_node, void *buf,
                           size_t len)
{
    (void)opaque;
    (void)src_node;
    (void)len;
    free(buf);
    return true;
}

/*
 * The data path calls the mesh wrappers whatever the backend, and only the
 * TCP backend has mesh operations. On every other backend the wrappers
 * answer themselves: no node, nothing to send on, room for anything, and no
 * peers to cancel for, call back from, or tell about a GID. The TCP
 * backend's operations once took every backend's private block for their
 * own, so shutting down a loopback server that a guest had used wrote the
 * receive callback past the end of the loopback state.
 */
static void test_mesh_none(void)
{
    static const char T[] = "mesh-none";
    static const char *const backends[] = {"none", "loopback", NV_BACKEND,
                                           S3H_BACKEND};
    static const uint8_t hdr[8] = {1};
    static int opaque;
    uint8_t gid[16] = {0xfe, 0x80};

    for (unsigned t = 0; t < RDMA_BACKEND_TYPE_MAX; t++) {
        const RdmaBackendOps *ops = rdma_backend_get_ops((RdmaBackendType)t);

        if (!check(T, ops != NULL, "backend %u isn't registered", t)) {
            continue;
        }
        if (t == RDMA_BACKEND_TYPE_TCP) {
            check(T, ops->mesh != NULL, "%s: no mesh", ops->name);
        } else {
            check(T, ops->mesh == NULL, "%s: a mesh", ops->name);
        }
    }

    /* No device at all, then each backend's, as the data path calls them */
    for (size_t i = 0; i <= sizeof(backends) / sizeof(backends[0]); i++) {
        const char *name = i == 0 ? "no device" : backends[i - 1];
        pvrdma_handle_t handle = NULL;
        struct ih h;
        uint32_t node;
        int rc;

        if (i > 0) {
            if (!setup(T, &h, name)) {
                continue;
            }
            handle = h.dev->pvrdma_handle;
        }

        node = ernic_mesh_local_node(handle);
        check(T, node == UINT32_MAX, "%s: local node %u", name, node);
        node = ernic_mesh_node_from_gid(handle, gid);
        check(T, node == UINT32_MAX, "%s: the GID is node %u's", name, node);
        rc = ernic_mesh_claim_gid(handle, gid, true);
        check(T, rc == (handle ? 0 : -EINVAL), "%s: claiming a GID gave %d",
              name, rc);
        rc = ernic_mesh_sendv(handle, 2, hdr, sizeof(hdr), NULL, 0, 0, false);
        check(T, rc == -EINVAL, "%s: a send gave %d, not -EINVAL", name, rc);
        check(T, ernic_mesh_has_room(handle, 2, sizeof(hdr)),
              "%s: no room to send", name);
        ernic_mesh_cancel(handle, 0, UINT64_MAX);
        ernic_mesh_set_recv_cb(handle, mesh_none_recv, &opaque);
        ernic_mesh_set_recv_cb(handle, NULL, NULL);

        if (i > 0) {
            teardown(T, &h);
        }
    }
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

    if (fd < 0) {
        return 0;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

static void test_teardown(void)
{
    static const char T[] = "teardown";
    char backend[64];
    struct ih h;
    uint32_t pd = 0;
    uint32_t cq = 0;
    uint32_t qpn = 0;
    uint32_t mr = 0;
    uint16_t port = free_tcp_port();

    if (!check(T, port != 0, "no free TCP port")) {
        return;
    }
    snprintf(backend, sizeof(backend), "tcp:manager:listen:%u", (unsigned)port);
    if (!check(T, ih_create(&h, backend), "device for '%s' not created",
               backend)) {
        return;
    }

    PVRDMADev *pvrdma = h.dev->pvrdma_handle;
    bool made =
        check(T, ernic_rm_alloc_pd(pvrdma, &pd) == 0, "no PD") &&
        check(T, ernic_rm_alloc_cq(pvrdma, 16, &cq) == 0, "no CQ") &&
        check(T,
              ernic_rm_alloc_qp(pvrdma,
                                &(struct ernic_rm_qp_init_attr){
                                    .pd_handle = pd,
                                    .qp_type = IBV_QPT_RC,
                                    .send_cq_handle = cq,
                                    .recv_cq_handle = cq,
                                },
                                &qpn) == 0,
              "no QP") &&
        check(T,
              ernic_rm_alloc_mr(pvrdma, pd, IBV_ACCESS_LOCAL_WRITE, &mr) == 0,
              "no DMA memory region");

    if (made) {
        check(T, rdma_rm_get_qp(&pvrdma->rdma_dev_res, qpn) != NULL,
              "QP %u isn't in the resource manager", qpn);
    }

    teardown(T, &h);
}

/*
 * The name is everything before the first ':', matched exactly, for every
 * backend: once the server took "s3junk" for s3 while the core took it for
 * an unknown name.
 */
static void test_backend_names(void)
{
    static const char T[] = "backend-names";
    static const struct {
        const char *str;
        const char *name; /* NULL: unknown */
    } cases[] = {
        {"none", "none"},
        {"none:", "none"},
        {"loopback", "loopback"},
        {"loopback:x", "loopback"},
        {"tcp", "tcp"},
        {"tcp:manager:listen:9000", "tcp"},
        {"nvmeof", "nvmeof"},
        {"nvmeof:size=1M", "nvmeof"},
        {"s3", "s3"},
        {"s3:bucket=b", "s3"},
        {"", NULL},
        {":", NULL},
        {":s3", NULL},
        {"nonex", NULL},
        {"loopbackjunk", NULL},
        {"loopbackjunk:x", NULL},
        /* The verbs backend was never implemented, and is gone */
        {"verbs", NULL},
        {"verbs:device=mlx5_0,port=1", NULL},
        {"tcpx:manager:listen:9000", NULL},
        {"nvmeofx", NULL},
        {"s3junk", NULL},
        {"s3junk:bucket=b", NULL},
        {"s", NULL},
        {"loop", NULL},
        {"S3", NULL},
        {" s3", NULL},
        {"s3 ", NULL},
        {"unknown", NULL},
    };

    const char *name = pvrdma_backend_name(NULL);
    check(T, name && !strcmp(name, "none"), "NULL is '%s', not 'none'",
          name ? name : "(unknown)");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        name = pvrdma_backend_name(cases[i].str);
        if (cases[i].name) {
            check(T, name && !strcmp(name, cases[i].name),
                  "'%s' is '%s', not '%s'", cases[i].str,
                  name ? name : "(unknown)", cases[i].name);
        } else {
            check(T, !name, "'%s' is '%s', not unknown", cases[i].str, name);
        }
    }

    /* The device refuses an unknown name, rather than choosing a backend */
    rocm_ernic_dev_t *dev = calloc(1, sizeof(*dev));

    if (!check(T, dev != NULL, "out of memory")) {
        return;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].name) {
            continue;
        }
        pvrdma_handle_t handle = pvrdma_device_create(dev, cases[i].str);
        if (!check(T, !handle, "a device was created for '%s'", cases[i].str)) {
            pvrdma_device_destroy(handle);
        }
    }
    free(dev);
}

/* ---- The none backend -------------------------------------------------- */

/*
 * The none backend keeps nothing and answers with stubs, but CQs, QPs and
 * an MR still live out their lives on it as the driver drives them. Two
 * QPs are told apart -- the backend once gave every QP the same number, so
 * the second replaced the first -- and QUERY_QP reports each one's own
 * state, PSNs and peer, from RESET to Error. It reports GSI's state too,
 * though the backend never sees GSI's transitions.
 */
static void test_none(void)
{
    static const char T[] = "none";
    struct ih h;
    struct ih_cq cq;
    struct ih_qp a;
    struct ih_qp b;
    struct ih_qp gsi;
    struct ih_mr mr;
    struct qp_answer qa;

    memset(&mr, 0, sizeof(mr));
    if (!setup(T, &h, "none")) {
        return;
    }

    PVRDMADev *pvrdma = h.dev->pvrdma_handle;
    RdmaDeviceResources *res = &pvrdma->rdma_dev_res;
    const uint32_t cqs = res->cq_tbl.used;
    const uint32_t qps = res->qp_tbl.used;
    const uint32_t mrs = res->mr_tbl.used;

    bool made =
        check(T, ih_cq_create(&h, &cq, 1, 6), "CREATE_CQ failed") &&
        check(T, ih_qp_create(&h, &a, RC_QP_A, IH_QPT_RC, &cq, &cq, NULL),
              "CREATE_QP A failed") &&
        check(T, ih_qp_create(&h, &b, RC_QP_B, IH_QPT_RC, &cq, &cq, NULL),
              "CREATE_QP B failed") &&
        /* The driver creates QP 1 as UD, and that id is what makes it GSI */
        check(T, ih_qp_create(&h, &gsi, 1, IH_QPT_UD, &cq, &cq, NULL),
              "CREATE_QP 1 failed") &&
        check(T,
              ih_mr_create(&h, &mr, MR_A, VA_A, MR_LEN, IH_ACCESS_ALL, false),
              "CREATE_MR failed");
    if (!made) {
        ih_mr_free(&mr);
        teardown(T, &h);
        return;
    }

    /* Until it is modified, a QP is in RESET */
    if (query_qp(T, &h, a.id, &qa)) {
        check(T, qa.state == IH_QPS_RESET, "new QP A in state %u, not RESET",
              qa.state);
    }

    check(T, ih_qp_connect(&h, &a, &b), "connecting A and B failed");

    /* ih_qp_modify() sets an RQ PSN of 0x100 and an SQ PSN of 0x200 */
    const struct ih_qp *qps_ab[2][2] = {{&a, &b}, {&b, &a}};
    for (size_t i = 0; i < 2; i++) {
        uint32_t self = qps_ab[i][0]->id;
        uint32_t peer = qps_ab[i][1]->id;

        if (!query_qp(T, &h, self, &qa)) {
            continue;
        }
        check(T,
              qa.state == IH_QPS_RTS && qa.dest == peer && qa.rq_psn == 0x100 &&
                  qa.sq_psn == 0x200,
              "QP %u: state %u, peer %u, PSNs %#x/%#x, expected RTS, %u, "
              "0x100/0x200",
              self, qa.state, qa.dest, qa.rq_psn, qa.sq_psn, peer);
    }

    /* Moving A to Error leaves B where it was */
    check(T,
          ih_qp_modify(&h, a.id,
                       &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                           .state = IH_QPS_ERR}) == 0,
          "A to Error failed");
    if (query_qp(T, &h, a.id, &qa)) {
        check(T, qa.state == IH_QPS_ERR, "QP A in state %u, not Error",
              qa.state);
    }
    if (query_qp(T, &h, b.id, &qa)) {
        check(T, qa.state == IH_QPS_RTS, "QP B in state %u, not RTS", qa.state);
    }

    /* GSI's transitions are recorded without the backend */
    bool gsi_rts =
        ih_qp_modify(&h, gsi.id,
                     &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                         .state = IH_QPS_INIT}) == 0 &&
        ih_qp_modify(&h, gsi.id,
                     &(struct ih_qp_mod){.attr_mask = IH_QP_STATE,
                                         .state = IH_QPS_RTR}) == 0 &&
        ih_qp_modify(
            &h, gsi.id,
            &(struct ih_qp_mod){.attr_mask = IH_QP_STATE | IH_QP_SQ_PSN,
                                .state = IH_QPS_RTS}) == 0;
    if (check(T, gsi_rts, "taking QP 1 to RTS failed") &&
        query_qp(T, &h, gsi.id, &qa)) {
        check(T, qa.state == IH_QPS_RTS && qa.sq_psn == 0x200,
              "QP 1: state %u, SQ PSN %#x, expected RTS, 0x200", qa.state,
              qa.sq_psn);
    }

    check(T, ih_qp_destroy(&h, &a) == 0, "DESTROY_QP A failed");
    check(T, ih_qp_destroy(&h, &b) == 0, "DESTROY_QP B failed");
    check(T, ih_qp_destroy(&h, &gsi) == 0, "DESTROY_QP 1 failed");
    check(T, ih_mr_destroy(&h, &mr) == 0, "DESTROY_MR failed");
    check(T, ih_cq_destroy(&h, &cq) == 0, "DESTROY_CQ failed");
    check(T,
          res->cq_tbl.used == cqs && res->qp_tbl.used == qps &&
              res->mr_tbl.used == mrs && g_hash_table_size(res->qp_hash) == qps,
          "%u CQ(s), %u QP(s), %u QPN(s) and %u MR(s) left in the resource "
          "manager, expected %u, %u, %u and %u",
          res->cq_tbl.used, res->qp_tbl.used, g_hash_table_size(res->qp_hash),
          res->mr_tbl.used, cqs, qps, qps, mrs);
    teardown(T, &h);
}

/* ---- Loopback pairing -------------------------------------------------- */

/*
 * The loopback backend stands in for rdma_cm: an RC or UC QP that reaches
 * RTS with no destination, or naming itself, is paired with one of its
 * type waiting the same way, and each then names the other. Only the
 * backend's answer to a query shows it -- the data path goes where
 * MODIFY_QP told it -- so this drives the resource manager through the
 * compat layer, where the backend's QPNs are visible.
 */

/* Create a QP of @type, returning its QPN, or 0. */
static uint32_t lp_create(const char *T, PVRDMADev *pvrdma, uint32_t pd,
                          uint32_t cq, uint8_t type)
{
    const struct ernic_rm_qp_init_attr attr = {
        .pd_handle = pd,
        .qp_type = type,
        .send_cq_handle = cq,
        .recv_cq_handle = cq,
    };
    uint32_t qpn = 0;

    if (!check(T, ernic_rm_alloc_qp(pvrdma, &attr, &qpn) == 0,
               "creating a QP of type %u failed", type)) {
        return 0;
    }
    return qpn;
}

/* Take QP @qpn through INIT and RTR, naming @dest, to RTS. */
static void lp_rts(const char *T, PVRDMADev *pvrdma, uint32_t qpn,
                   uint32_t dest)
{
    static const uint8_t states[] = {IH_QPS_INIT, IH_QPS_RTR, IH_QPS_RTS};

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        const struct ernic_rm_qp_attr attr = {
            .attr_mask = IBV_QP_STATE,
            .type_state = states[i],
            .qkey_dest_qpn = dest,
        };
        int rc = ernic_rm_modify_qp(pvrdma, qpn, &attr);
        if (!check(T, rc == 0, "QP %u to state %u: %d", qpn, states[i], rc)) {
            return;
        }
    }
}

/* Check that QP @qpn is in RTS and names @dest. */
static void lp_expect(const char *T, PVRDMADev *pvrdma, uint32_t qpn,
                      uint32_t dest)
{
    uint8_t state = 0;
    uint32_t got = UINT32_MAX;
    int rc =
        ernic_rm_query_qp(pvrdma, qpn, &state, NULL, &got, NULL, NULL, NULL);

    check(T, rc == 0 && state == IH_QPS_RTS && got == dest,
          "QP %u: query returned %d with state %u and destination %u, "
          "expected 0 with RTS and %u",
          qpn, rc, state, got, dest);
}

static void test_loopback_pair(void)
{
    static const char T[] = "loopback-pair";
    struct ih h;
    uint32_t pd = 0;
    uint32_t cq = 0;

    if (!check(T, ih_create(&h, "loopback"), "device not created")) {
        return;
    }

    PVRDMADev *pvrdma = h.dev->pvrdma_handle;
    if (!check(T, ernic_rm_alloc_pd(pvrdma, &pd) == 0, "no PD") ||
        !check(T, ernic_rm_alloc_cq(pvrdma, 16, &cq) == 0, "no CQ")) {
        teardown(T, &h);
        return;
    }

    /* An RC QP with no destination waits for a partner */
    uint32_t rc1 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc1, 0);
    lp_expect(T, pvrdma, rc1, 0);

    /* UD and DC QPs aren't connected, so two of each don't pair */
    static const uint8_t unconnected[] = {IBV_QPT_UD, IBV_QPT_UD,
                                          IBV_QPT_DRIVER, IBV_QPT_DRIVER};
    uint32_t qpns[sizeof(unconnected)];
    for (size_t i = 0; i < sizeof(unconnected); i++) {
        qpns[i] = lp_create(T, pvrdma, pd, cq, unconnected[i]);
        lp_rts(T, pvrdma, qpns[i], 0);
    }
    for (size_t i = 0; i < sizeof(unconnected); i++) {
        lp_expect(T, pvrdma, qpns[i], 0);
    }

    /* A UC QP doesn't pair with an RC one */
    uint32_t uc1 = lp_create(T, pvrdma, pd, cq, IBV_QPT_UC);
    lp_rts(T, pvrdma, uc1, 0);
    lp_expect(T, pvrdma, uc1, 0);
    lp_expect(T, pvrdma, rc1, 0);

    /* The next RC QP with no destination does */
    uint32_t rc2 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc2, 0);
    lp_expect(T, pvrdma, rc1, rc2);
    lp_expect(T, pvrdma, rc2, rc1);

    /* A QP that names another keeps it, and its partner isn't taken */
    uint32_t rc3 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc3, rc1);
    lp_expect(T, pvrdma, rc3, rc1);
    lp_expect(T, pvrdma, rc1, rc2);

    /* One that names itself names nothing, and waits for the next */
    uint32_t rc4 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc4, rc4);
    lp_expect(T, pvrdma, rc4, 0);
    uint32_t rc5 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc5, 0);
    lp_expect(T, pvrdma, rc4, rc5);
    lp_expect(T, pvrdma, rc5, rc4);

    /* And the UC QP gets the next UC one */
    uint32_t uc2 = lp_create(T, pvrdma, pd, cq, IBV_QPT_UC);
    lp_rts(T, pvrdma, uc2, 0);
    lp_expect(T, pvrdma, uc1, uc2);
    lp_expect(T, pvrdma, uc2, uc1);

    /* A QP destroyed while waiting is gone, and can't be paired */
    uint32_t gone = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, gone, 0);
    ernic_rm_dealloc_qp(pvrdma, gone);
    uint32_t rc6 = lp_create(T, pvrdma, pd, cq, IBV_QPT_RC);
    lp_rts(T, pvrdma, rc6, 0);
    lp_expect(T, pvrdma, rc6, 0);

    /* The backend only sees INIT, RTR and RTS, but a query reports Error */
    uint8_t state = 0;
    const struct ernic_rm_qp_attr to_err = {
        .attr_mask = IBV_QP_STATE,
        .type_state = IH_QPS_ERR,
    };
    int rc = ernic_rm_modify_qp(pvrdma, rc6, &to_err);
    check(T,
          rc == 0 &&
              ernic_rm_query_qp(pvrdma, rc6, &state, NULL, NULL, NULL, NULL,
                                NULL) == 0 &&
              state == IH_QPS_ERR,
          "QP %u to Error returned %d and reports state %u", rc6, rc, state);

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
    {"rdma-reset-faults", test_rdma_reset_faults},
    {"rdma-reset-frees", test_rdma_reset_frees},
    {"admin", test_admin_errors},
    {"admin-dma", test_admin_dma_faults},
    {"rc", test_rc},
    {"spec-sge", test_spec_sge},
    {"atomic", test_atomic_and_reg_mr},
    {"wrap", test_wrap},
    {"mr-keys", test_mr_keys},
    {"dp-dma", test_dp_dma_faults},
    {"tables", test_full_tables},
    {"limits", test_limits},
    {"cq-events", test_cq_events},
    {"qp-flush", test_qp_flush},
    {"qp-flush-srq", test_qp_flush_srq},
    {"ud", test_ud},
    {"srq", test_srq},
    {"srq-rq-ring", test_srq_rq_ring},
    {"srq-cq-ring", test_srq_cq_ring},
    {"eth", test_eth},
    {"eth-dma", test_eth_dma_faults},
#ifdef ERNIC_HAVE_P4
    {"p4-port", test_p4_port},
    {"p4-attach", test_p4_attach},
    {"fp8-map", test_fp8_map},
    {"fp8-overlap", test_fp8_overlap},
    {"p4-rdma", test_p4_rdma},
    {"p4-cvt", test_p4_cvt},
    {"p4-cvt-map", test_p4_cvt_then_map},
    {"p4-drop-map", test_p4_drop_then_map},
    {"p4-deny", test_p4_deny},
    {"p4-log-limit", test_p4_log_limit},
    {"p4-devcmd", test_p4_devcmd},
    {"p4-devcmd-errors", test_p4_devcmd_errors},
#endif
    {"nvmeof", test_nvmeof},
    {"nvmeof-queues", test_nvmeof_queue_limit},
    {"s3", test_s3},
    {"mesh-none", test_mesh_none},
    {"teardown", test_teardown},
    {"backend-names", test_backend_names},
    {"none", test_none},
    {"loopback-pair", test_loopback_pair},
};

int main(int argc, char **argv)
{
    bool ran = false;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [group]\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        if (argc == 2 && strcmp(argv[1], groups[i].name) != 0) {
            continue;
        }
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
