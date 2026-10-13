/*
 * Unit tests for the ionic emulator's queue and interrupt shape.
 *
 * The guest driver sizes itself from three identity numbers: the LIF's
 * Rx/Tx queue counts, its RDMA EQ count and the device's interrupt count.
 * ionic_lif_size() then wants 1 + min(queues, vCPUs) + min(EQs, vCPUs)
 * vectors and halves its EQs or queues until that fits.  These rows check
 * that each NIC profile's [queues] keys reach the identity, that the
 * vector budget fits where the profile says it should, that queue indices
 * past the profile's count are refused without touching memory, and that
 * every one of the 128 vectors has room in BAR0.
 *
 * The translation unit is #included to reach the devcmd handlers, and the
 * DMA entry points are backed by a plain buffer (ionic_eth_emu_fakes.h).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "ionic_eth_emu.c"

#include "ionic_eth_emu_fakes.h"

static int failures;

static void report(bool ok, const char *category, const char *description,
                   const char *expected)
{
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", category,
           description, expected);
    if (!ok)
        failures++;
}

/* ---- Helpers ------------------------------------------------------------ */

/*
 * A fresh device shaped by built-in profile @name (NULL: compat) and then
 * ini @layer (NULL: none).  Exits if either is refused: rows only use
 * valid profiles.
 */
static struct ionic_eth_emu *new_emu(const char *name, const char *layer)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;
    char err[256] = "";

    if (!emu) {
        fprintf(stderr, "ionic_eth_emu_create failed\n");
        exit(1);
    }
    ionic_profile_defaults(&p);
    if ((name && ionic_profile_apply(&p, name, err, sizeof(err)) != 0) ||
        (layer && ionic_profile_apply_text(&p, "row", layer, strlen(layer), err,
                                           sizeof(err)) != 0)) {
        fprintf(stderr, "profile refused: %s\n", err);
        exit(1);
    }
    ionic_eth_emu_set_profile(emu, &p);
    memset(g_mem, 0, sizeof(g_mem));
    g_nwrites = 0;
    return emu;
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return le32toh(v);
}

/* What the identity blocks tell the driver. */
struct shape {
    uint32_t rxq, txq;   /* LIF identity queue_count[RXQ], [TXQ] */
    uint32_t lif_eqs;    /* LIF identity rdma eq_qtype qid_count */
    uint32_t dev_eqs;    /* dev identity eq_count */
    uint32_t dev_nintrs; /* dev identity nintrs */
};

static void read_shape(struct ionic_eth_emu *emu, struct shape *s)
{
    static uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64] = {0}, comp[16] = {0};

    memset(data, 0, sizeof(data));
    handle_identify(emu, cmd, comp, data);
    s->dev_nintrs = get32(data + DEVID_NINTRS_OFF);
    s->dev_eqs = get32(data + DEVID_EQ_COUNT_OFF);

    memset(data, 0, sizeof(data));
    handle_lif_identify(emu, cmd, comp, data);
    s->rxq = get32(data + LIFID_CONFIG_QCOUNT_OFF + 4u * IONIC_QTYPE_RXQ);
    s->txq = get32(data + LIFID_CONFIG_QCOUNT_OFF + 4u * IONIC_QTYPE_TXQ);
    /* eq_qtype is the fifth ionic_lif_logical_qtype after rdma+32. */
    s->lif_eqs = get32(data + LIF_ID_RDMA_OFF + 32u + 4u * QTYPE_SZ + 4u);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/*
 * The vectors ionic_lif_size() (ionic_lif.c) asks for on its first try:
 * one for the adminq and notifyq, one per queue pair and one per EQ, each
 * capped at the online vCPUs.  If that exceeds nintrs it halves.
 */
static uint32_t driver_nintrs(const struct shape *s, uint32_t cpus)
{
    uint32_t nxqs = min_u32(min_u32(s->txq, s->rxq), cpus);

    return 1u + nxqs + min_u32(s->lif_eqs, cpus);
}

/* Q_INIT for @qtype/@index with a 4-entry ring at @ring. */
static void q_init(struct ionic_eth_emu *emu, uint8_t qtype, uint32_t index,
                   uint16_t intr, uint64_t ring, uint64_t cq)
{
    uint8_t cmd[64] = {0}, comp[16] = {0};
    uint32_t idx = htole32(index);
    uint16_t in = htole16(intr);
    uint64_t r = htole64(ring), c = htole64(cq);

    cmd[0] = IONIC_CMD_Q_INIT;
    cmd[4] = qtype;
    memcpy(cmd + 8, &idx, 4);
    memcpy(cmd + 14, &in, 2);
    cmd[19] = 2; /* log2 of the ring depth */
    memcpy(cmd + 20, &r, 8);
    memcpy(cmd + 28, &c, 8);
    handle_q_init(emu, cmd, comp);
}

/* ---- Identity and the vector budget ------------------------------------ */

struct shape_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *profile; /* built-in name, or NULL for compat */
    const char *layer;   /* ini text on top, or NULL */
    uint32_t cpus;       /* the guest's online vCPUs */
    struct shape want;
    bool want_fits; /* the driver's first try fits nintrs */
};

static const struct shape_case shape_cases[] = {
    {"positive",
     "generic-400g on a 64-vCPU guest",
     "64 queues, 32 EQs, 97 vectors; 1 + 64 + 32 = 97 fits, no halving",
     "generic-400g",
     NULL,
     64,
     {64, 64, 32, 32, 97},
     true},
    {"positive",
     "generic-100g on an 8-vCPU guest",
     "16 queues, 49 vectors; the driver wants 1 + 8 + 8 = 17, which fits",
     "generic-100g",
     NULL,
     8,
     {16, 16, 32, 32, 49},
     true},
    {"positive",
     "queues.eth = 16 with intr_vectors = auto on 16 vCPUs",
     "16 queues, 49 vectors; 1 + 16 + 16 = 33 fits",
     NULL,
     "[queues]\neth = 16\nintr_vectors = auto\n",
     16,
     {16, 16, 32, 32, 49},
     true},
    {"positive",
     "small-vm on a 2-vCPU guest",
     "2 queues, 4 EQs, 7 vectors; 1 + 2 + 2 = 5 fits",
     "small-vm",
     NULL,
     2,
     {2, 2, 4, 4, 7},
     true},
    {"boundary",
     "queues.eth = 1 on a 1-vCPU guest",
     "1 queue; auto gives 1 + 1 + 32 = 34 vectors and the driver wants 3",
     NULL,
     "[queues]\neth = 1\nintr_vectors = auto\n",
     1,
     {1, 1, 32, 32, 34},
     true},
    {"boundary",
     "queues.eth = 64 with intr_vectors = auto on 128 vCPUs",
     "64 queues; 1 + 64 + 32 = 97, the most a profile can need, fits",
     NULL,
     "[queues]\neth = 64\nintr_vectors = auto\n",
     128,
     {64, 64, 32, 32, 97},
     true},
    {"boundary",
     "queues.intr_vectors = 2 on a 4-vCPU guest",
     "nintrs 2; the driver wants 9, so it halves",
     NULL,
     "[queues]\nintr_vectors = 2\n",
     4,
     {4, 4, 32, 32, 2},
     false},
    {"boundary",
     "queues.rdma_eqs = 4",
     "4 EQs in both the dev and the LIF identity",
     NULL,
     "[queues]\nrdma_eqs = 4\n",
     4,
     {4, 4, 4, 4, 32},
     true},
    {"corner",
     "compat on a 64-vCPU guest",
     "4 queues, 32 EQs, 32 vectors as before; 1 + 4 + 32 = 37 > 32, so the "
     "driver halves its EQs, as it always has",
     NULL,
     NULL,
     64,
     {4, 4, 32, 32, 32},
     false},
    {"corner",
     "queues.eth = 64 with compat's 32 vectors on 64 vCPUs",
     "allowed: the driver halves to fit (the server warns)",
     NULL,
     "[queues]\neth = 64\n",
     64,
     {64, 64, 32, 32, 32},
     false},
};

static void run_shape_case(const struct shape_case *c)
{
    struct ionic_eth_emu *emu = new_emu(c->profile, c->layer);
    struct shape got;
    bool fits, ok;

    read_shape(emu, &got);
    fits = driver_nintrs(&got, c->cpus) <= got.dev_nintrs;
    ok = memcmp(&got, &c->want, sizeof(got)) == 0 && fits == c->want_fits &&
         got.dev_nintrs <= IONIC_MSIX_MAX_VECTORS;
    report(ok, c->category, c->description, c->expected);
    if (!ok)
        printf("     got rxq %" PRIu32 " txq %" PRIu32 " lif_eqs %" PRIu32
               " dev_eqs %" PRIu32 " nintrs %" PRIu32 "; driver wants %" PRIu32
               " on %" PRIu32 " vCPUs\n",
               got.rxq, got.txq, got.lif_eqs, got.dev_eqs, got.dev_nintrs,
               driver_nintrs(&got, c->cpus), c->cpus);
    ionic_eth_emu_destroy(emu);
}

/* ---- Queue indices past the profile's count ---------------------------- */

#define RING 0x0100u
#define CQ   0x0200u

struct qinit_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *layer;
    uint8_t qtype;
    uint32_t index;
    bool want_valid;
};

static const struct qinit_case qinit_cases[] = {
    {"positive", "Rx queue 15 with queues.eth = 16", "initialized",
     "[queues]\neth = 16\n", IONIC_QTYPE_RXQ, 15, true},
    {"boundary", "Rx queue 0 with queues.eth = 1", "initialized",
     "[queues]\neth = 1\n", IONIC_QTYPE_RXQ, 0, true},
    {"boundary", "Rx queue 1 with queues.eth = 1", "refused: past the count",
     "[queues]\neth = 1\n", IONIC_QTYPE_RXQ, 1, false},
    {"boundary", "Tx queue 63 with queues.eth = 64", "initialized",
     "[queues]\neth = 64\n", IONIC_QTYPE_TXQ, 63, true},
    {"boundary", "Tx queue 64 with queues.eth = 64",
     "refused: past the array, with no write out of bounds",
     "[queues]\neth = 64\n", IONIC_QTYPE_TXQ, 64, false},
    {"negative", "Rx queue 4 with compat's 4 queues", "refused", NULL,
     IONIC_QTYPE_RXQ, 4, false},
    {"negative", "Rx queue 0xffffffff", "refused", "[queues]\neth = 64\n",
     IONIC_QTYPE_RXQ, 0xffffffffu, false},
};

static void run_qinit_case(const struct qinit_case *c)
{
    struct ionic_eth_emu *emu = new_emu(NULL, c->layer);
    bool valid = false;

    q_init(emu, c->qtype, c->index, 1, RING, CQ);
    for (uint32_t i = 0; i < IONIC_EMU_ETH_QMAX; i++)
        if (emu->eth_q[c->qtype][i].valid)
            valid = valid || i == c->index;
    report(valid == c->want_valid, c->category, c->description, c->expected);
    ionic_eth_emu_destroy(emu);
}

/* A Tx doorbell for a queue past the count moves nothing. */
static void check_txq_past_count(void)
{
    struct ionic_eth_emu *emu = new_emu(NULL, NULL);

    q_init(emu, IONIC_QTYPE_TXQ, 3, 1, RING, CQ);
    g_nwrites = 0;
    eth_txq_service(emu, 4, 1);
    report(g_nwrites == 0 && !emu->eth_q[IONIC_QTYPE_TXQ][4].valid, "negative",
           "a Tx doorbell for queue 4 of 4", "ignored: no DMA, no state");
    ionic_eth_emu_destroy(emu);
}

/* ---- Vectors ------------------------------------------------------------ */

static void check_vectors(void)
{
    struct ionic_eth_emu *emu = new_emu("generic-400g", NULL);
    uint32_t zero = 0;
    loff_t mask127 = (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET +
                     127 * INTR_REG_STRIDE + INTR_MASK_OFF;

    report(
        ionic_eth_emu_trigger_irq(emu, 127) == 0 && emu->intr_pending[127] == 1,
        "boundary", "vector 127 raised while masked", "accepted and latched");
    report(ionic_eth_emu_trigger_irq(emu, IONIC_MSIX_MAX_VECTORS) == -EINVAL,
           "boundary", "vector 128", "refused with -EINVAL");
    report(ionic_eth_emu_bar0_access(emu, (char *)&zero, 4, mask127, true) ==
                   4 &&
               emu->intr_mask[127] == 0 && emu->intr_pending[127] == 0,
           "corner", "unmask vector 127 through its intr_ctrl block",
           "unmasked, and the latched interrupt delivered");
    ionic_eth_emu_destroy(emu);
}

/* Where each per-vector structure ends with every vector in use. */
static void check_bar0_layout(void)
{
    uint32_t n = IONIC_MSIX_MAX_VECTORS;

    report(n >= 97, "positive", "IONIC_MSIX_MAX_VECTORS",
           "at least 1 + 64 + 32 = 97, the most a profile can ask for");
    report(IONIC_BAR0_INTR_CTRL_OFFSET + n * INTR_REG_STRIDE <=
               IONIC_BAR0_REGS_SIZE,
           "corner", "intr_ctrl for 128 vectors",
           "ends inside the 32 KB register window");
    report(IONIC_BAR0_MSIX_TABLE + n * 16u <= IONIC_BAR0_MSIX_PBA, "corner",
           "the MSI-X table for 128 vectors", "ends before the PBA");
    report(IONIC_BAR0_MSIX_PBA + (n + 63u) / 64u * 8u <= IONIC_BAR0_TOTAL_SIZE,
           "corner", "the MSI-X PBA for 128 vectors", "ends inside BAR0");
}

int main(void)
{
    for (size_t i = 0; i < sizeof(shape_cases) / sizeof(shape_cases[0]); i++)
        run_shape_case(&shape_cases[i]);
    for (size_t i = 0; i < sizeof(qinit_cases) / sizeof(qinit_cases[0]); i++)
        run_qinit_case(&qinit_cases[i]);
    check_txq_past_count();
    check_vectors();
    check_bar0_layout();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
