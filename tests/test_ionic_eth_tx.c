/*
 * Unit tests for the emulated ionic NIC's Ethernet Tx offloads.
 *
 * Each row writes Tx descriptors and SG lists into fake guest memory the
 * way the Linux driver lays them out (ionic_tx_calc_csum(),
 * ionic_tx_calc_no_csum(), ionic_tx_tso()), rings the queue's doorbell, and
 * checks the frames that come out against literals: a frame scattered over
 * SG elements in both SG list layouts, the checksum a CSUM_PARTIAL
 * descriptor asks for, the segments of a TSO chain, and each way a
 * descriptor is refused.  The expected frames, in
 * golden/ionic_eth_tx_frames.h, were not made by the emulator.
 *
 * Then what the driver negotiates before it posts any of that: the
 * features LIF_SETATTR grants and LIF_IDENTIFY reports, and the Tx queue
 * versions Q_IDENTIFY offers and Q_INIT accepts.
 *
 * The test links the eth emulator and reaches its state through
 * ionic_eth_emu_priv.h; the DMA entry points are backed by a plain buffer
 * (ionic_eth_emu_fakes.h), and a Tx filter stands in for the wire.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "byteorder.h"
#include "golden/ionic_eth_tx_frames.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_emu_stats.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic/ionic_eth_emu_priv.h"
#include "ionic/ionic_eth_net.h"
#include "ionic/ionic_profile.h"
#include "ionic/ionic_status.h"

static int failures;

static void report(bool ok, const char *category, const char *description,
                   const char *expected)
{
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", category,
           description, expected);
    if (!ok) {
        failures++;
    }
}

/* ---- The guest's side ---------------------------------------------------- */

/*
 * Fake guest memory: an 8-descriptor Tx ring, its completion ring, an SG
 * ring with room for the larger (version 1) SG list of each descriptor,
 * and the rest for frame buffers.
 */
#define TXQ_DEPTH_LOG2 3U
#define TXQ_DEPTH      (1U << TXQ_DEPTH_LOG2)
#define TXQ_RING       0x0100U
#define TXQ_CQ         0x0200U
#define TXQ_SG         0x0400U
#define BUF_BASE       0x1000U
#define BUF_END        ((size_t)GUEST_MEM_SIZE)
#define BUF_GAP        3U    /* between two buffers */
#define BUF_FILL       0xeeU /* what is in the gaps */
#define TXQ_ID         1U    /* the queue the rows use */
#define TXQ_VECTOR     5U

_Static_assert(TXQ_CQ >= TXQ_RING + (TXQ_DEPTH * ETH_DESC_SIZE) &&
                   TXQ_SG >= TXQ_CQ + (TXQ_DEPTH * ETH_COMP_SIZE) &&
                   BUF_BASE >= TXQ_SG + (TXQ_DEPTH * TXQ_SG_V1_DESC_SIZE),
               "the rings do not overlap");

/* The values of union ionic_q_identity and the driver's own enums, written
 * here independently of the emulator's copies. */
#define OP_CSUM_PARTIAL 1U
#define OP_CSUM_HW      2U
#define OP_TSO          3U
#define FLAG_VLAN       0x1U
#define FLAG_ENCAP      0x2U
#define FLAG_SOT        0x4U
#define FLAG_EOT        0x8U
#define QINIT_F_IRQ     0x01U
#define QINIT_F_SG      0x04U

/* Frames the device sent, as the Tx filter saw them. */
#define SENT_MAX 12U
static struct {
    uint8_t frame[SENT_MAX][IONIC_ETH_NET_MTU_MAX];
    size_t len[SENT_MAX];
    unsigned int n; /* counts every frame, kept or not */
    /* Run once, when the next frame is sent: a guest writing to its ring
     * while the device is part of the way through it. */
    void (*on_next_frame)(void);
    struct ionic_eth_emu *emu; /* the device, for that */
} sent;

static bool capture_frame(void *ctx, const void *frame, size_t len)
{
    void (*hook)(void) = sent.on_next_frame;

    (void)ctx;
    sent.on_next_frame = NULL;
    if (hook) {
        hook();
    }
    if (sent.n < SENT_MAX && len <= sizeof(sent.frame[0])) {
        memcpy(sent.frame[sent.n], frame, len);
        sent.len[sent.n] = len;
    }
    sent.n++;
    return true;
}

static bool sent_is(unsigned int i, const uint8_t *want, size_t len)
{
    return i < sent.n && i < SENT_MAX && sent.len[i] == len &&
           memcmp(sent.frame[i], want, len) == 0;
}

/* Where the next frame buffer goes: they are handed out downwards, so the
 * buffers of one descriptor are neither adjacent nor in order. */
static size_t buf_top;

/*
 * A fresh device with Tx queue TXQ_ID initialized as version @ver with
 * Q_INIT @flags and its SG ring at @sg_ring, and enabled, empty guest
 * memory, and nothing sent.
 */
static struct ionic_eth_emu *tx_emu_sg(uint8_t ver, uint16_t flags,
                                       uint64_t sg_ring)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    if (!emu) {
        fprintf(stderr, "ionic_eth_emu_create failed\n");
        abort();
    }
    ionic_profile_defaults(&p);
    ionic_eth_emu_set_profile(emu, &p);
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_mem + BUF_BASE, BUF_FILL, BUF_END - BUF_BASE);
    memset(&sent, 0, sizeof(sent));
    buf_top = BUF_END;
    sent.emu = emu;
    ionic_eth_emu_register_tx_filter(emu, capture_frame, NULL);

    /* struct ionic_q_init_cmd, as ionic_lif_txq_init() fills it in. */
    cmd[0] = IONIC_CMD_Q_INIT;
    cmd[4] = IONIC_QTYPE_TXQ;
    cmd[5] = ver;
    ernic_put_le32(cmd + 8, TXQ_ID);
    ernic_put_le16(cmd + 14, TXQ_VECTOR);
    ernic_put_le16(cmd + 16, flags);
    cmd[19] = TXQ_DEPTH_LOG2;
    ernic_put_le64(cmd + 20, TXQ_RING);
    ernic_put_le64(cmd + 28, TXQ_CQ);
    ernic_put_le64(cmd + 36, sg_ring);
    ionic_eth_emu_handle_q_init(emu, cmd, comp);
    if (comp[0] != IONIC_RC_SUCCESS) {
        fprintf(stderr, "Q_INIT of the Tx queue failed: %u\n", comp[0]);
        abort();
    }

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = IONIC_CMD_Q_CONTROL;
    cmd[1] = IONIC_QTYPE_TXQ;
    ernic_put_le32(cmd + 4, TXQ_ID);
    cmd[8] = IONIC_Q_ENABLE;
    ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);
    return emu;
}

/* The same with the SG ring where post() writes SG lists. */
static struct ionic_eth_emu *tx_emu(uint8_t ver, uint16_t flags)
{
    return tx_emu_sg(ver, flags, TXQ_SG);
}

/* One Tx descriptor, and the buffers behind it. */
struct txd {
    uint8_t opcode;
    uint8_t flags;
    uint16_t hword1; /* csum_start or hdr_len */
    uint16_t hword2; /* csum_offset or mss */
    /* The bytes it brings, split into a first buffer of @head bytes and
     * @nsge SG elements of @sge[] bytes each. */
    const uint8_t *data;
    uint16_t head;
    uint8_t nsge;
    uint16_t sge[TXQ_SG_V1_MAX_ELEMS];
    /* Where the first buffer and each SG element are, for a row that says:
     * 0 gives each a buffer of its own holding its part of @data. */
    uint64_t head_addr;
    uint64_t sge_addr[TXQ_SG_V1_MAX_ELEMS];
};

/* Copy @len bytes into a frame buffer of their own and return where. */
static uint64_t buf_put(const uint8_t *data, size_t len)
{
    if (len + BUF_GAP > buf_top - BUF_BASE) {
        fprintf(stderr, "out of fake guest memory\n");
        abort();
    }
    buf_top -= len + BUF_GAP;
    if (len != 0) {
        memcpy(g_mem + buf_top, data, len);
    }
    return buf_top;
}

/*
 * Write @d at @index of the Tx ring as the driver does: the first buffer's
 * address and length in the descriptor, the others in slot @index of the
 * SG ring, whose slots are @sg_size bytes.
 */
static void post(uint16_t index, const struct txd *d, size_t sg_size)
{
    uint8_t *desc = g_mem + TXQ_RING + ((size_t)index * ETH_DESC_SIZE);
    uint8_t *sg = g_mem + TXQ_SG + ((size_t)index * sg_size);
    const uint8_t *data = d->data;
    uint64_t addr = d->head_addr ? d->head_addr : buf_put(data, d->head);

    /* encode_txq_desc_cmd() */
    ernic_put_le64(desc, (uint64_t)d->flags | ((uint64_t)d->opcode << 4) |
                             ((uint64_t)d->nsge << 8) | (addr << 12));
    ernic_put_le16(desc + 8, d->head);
    ernic_put_le16(desc + 10, 0);
    ernic_put_le16(desc + 12, d->hword1);
    ernic_put_le16(desc + 14, d->hword2);
    data += d->head;

    for (uint8_t i = 0; i < d->nsge; i++) {
        addr = d->sge_addr[i] ? d->sge_addr[i] : buf_put(data, d->sge[i]);
        /* struct ionic_txq_sg_elem */
        memset(sg + ((size_t)i * 16), 0, 16);
        ernic_put_le64(sg + ((size_t)i * 16), addr);
        ernic_put_le16(sg + ((size_t)i * 16) + 8, d->sge[i]);
        data += d->sge[i];
    }
}

/* Ring the Tx queue's doorbell with producer index @prod. */
static void kick(struct ionic_eth_emu *emu, uint16_t prod)
{
    /* p_index in bits 15:0, the queue in bits 31:24. */
    uint64_t db = (uint64_t)prod | ((uint64_t)TXQ_ID << 24);

    ionic_eth_emu_bar2_access(emu, (char *)&db, sizeof(db),
                              (loff_t)IONIC_QTYPE_TXQ * 8, true);
}

/* Whether completion @slot is written, and names descriptor @index. */
static bool completed(unsigned int slot, uint16_t index)
{
    const uint8_t *comp = g_mem + TXQ_CQ + ((size_t)slot * ETH_COMP_SIZE);

    return (comp[ETH_COMP_SIZE - 1] & ADMIN_COMP_COLOR_MASK) != 0 &&
           comp[0] == 0 && ernic_get_le16(comp + 2) == index;
}

static bool not_completed(unsigned int slot)
{
    static const uint8_t zero[ETH_COMP_SIZE];

    return memcmp(g_mem + TXQ_CQ + ((size_t)slot * ETH_COMP_SIZE), zero,
                  sizeof(zero)) == 0;
}

static uint64_t lif_stat(const struct ionic_eth_emu *emu, enum ionic_lif_stat s)
{
    return emu->stats.lif[s];
}

/* How many frames the LIF counts as sent, whatever their destination. */
static uint64_t tx_packets(const struct ionic_eth_emu *emu)
{
    return lif_stat(emu, IONIC_LIF_TX_UCAST_PACKETS) +
           lif_stat(emu, IONIC_LIF_TX_MCAST_PACKETS) +
           lif_stat(emu, IONIC_LIF_TX_BCAST_PACKETS);
}

static uint16_t tx_head(const struct ionic_eth_emu *emu)
{
    return emu->eth_q[IONIC_QTYPE_TXQ][TXQ_ID].head;
}

/* ---- Scatter-gather ------------------------------------------------------ */

/* A frame in one buffer, which is every frame the driver sent before it
 * was granted scatter-gather. */
static void check_plain(void)
{
    struct ionic_eth_emu *emu = tx_emu(0, QINIT_F_IRQ);
    const struct txd d = {.data = etx_plain60, .head = sizeof(etx_plain60)};

    post(0, &d, TXQ_SG_DESC_SIZE);
    kick(emu, 1);
    report(sent.n == 1 && sent_is(0, etx_plain60, sizeof(etx_plain60)) &&
               completed(0, 0) && tx_head(emu) == 1 && tx_packets(emu) == 1,
           "positive", "a 60-byte frame in one buffer, CSUM_NONE",
           "sent unchanged, completed, counted");
    ionic_eth_emu_destroy(emu);
}

/*
 * The driver's len is the first buffer's alone.  Here the SG elements hold
 * four times what it does, which the device once took for a descriptor
 * whose SG list was longer than its frame, and dropped.  The descriptor is
 * the third on its ring, so its SG list is found only by a device that
 * knows how big the two before it are.
 */
static void check_sg_v0(void)
{
    struct ionic_eth_emu *emu = tx_emu(0, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    const struct txd d = {.data = etx_sg_v0,
                          .head = 20,
                          .nsge = 8,
                          .sge = {10, 10, 10, 10, 10, 10, 10, 10}};

    post(0, &plain, TXQ_SG_DESC_SIZE);
    post(1, &plain, TXQ_SG_DESC_SIZE);
    post(2, &d, TXQ_SG_DESC_SIZE);
    kick(emu, 3);
    report(sent.n == 3 && sent_is(2, etx_sg_v0, sizeof(etx_sg_v0)) &&
               completed(2, 2) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "positive",
           "version 0 queue: a 20-byte first buffer and eight 10-byte SG "
           "elements, the most a version 0 list holds",
           "one 100-byte frame, the buffers in order");
    ionic_eth_emu_destroy(emu);
}

/* The same on a version 1 queue, whose SG lists are 256 bytes apart and
 * hold 15 elements. */
static void check_sg_v1(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    const struct txd d = {
        .data = etx_sg_v1,
        .head = 14,
        .nsge = 15,
        .sge = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};

    post(0, &plain, TXQ_SG_V1_DESC_SIZE);
    post(1, &plain, TXQ_SG_V1_DESC_SIZE);
    post(2, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 3);
    report(sent.n == 3 && sent_is(2, etx_sg_v1, sizeof(etx_sg_v1)) &&
               completed(2, 2) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "positive",
           "version 1 queue: a 14-byte first buffer and 15 SG elements of "
           "1 to 15 bytes",
           "one 134-byte frame, the buffers in order");
    ionic_eth_emu_destroy(emu);
}

/* The largest frame there is, and one byte more. */
static void check_frame_bound(void)
{
    /* One byte more than a frame, for the second row */
    static uint8_t big[IONIC_ETH_NET_MTU_MAX + 1];
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    struct txd d = {.data = big, .head = 4000, .nsge = 2, .sge = {4000, 1600}};

    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 13U);
    }
    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 1);
    report(sent.n == 1 && sent_is(0, big, IONIC_ETH_NET_MTU_MAX) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "corner", "a frame of 9600 bytes over three buffers", "sent whole");

    /* The same and one byte more: nothing is written past the device's
     * frame buffer, and nothing shorter is sent instead. */
    buf_top = BUF_END;
    d.sge[1] = 1601;
    post(1, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 2);
    report(sent.n == 1 && completed(1, 1) && tx_head(emu) == 2 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1,
           "negative", "a frame of 9601 bytes over three buffers",
           "refused whole as a descriptor data error, and completed");
    ionic_eth_emu_destroy(emu);
}

/* ---- Descriptors that are refused ---------------------------------------- */

struct refuse_case {
    const char *description;
    uint8_t ver;
    uint16_t qflags;
    struct txd d;
    enum ionic_lif_stat counter;
};

/* A buffer address no guest memory is behind. */
#define NOWHERE UINT64_C(0x00000ffffffff000)

static const struct refuse_case refuse_cases[] = {
    {"a first buffer of no bytes",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.data = etx_plain60, .head = 0, .nsge = 1, .sge = {60}},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"an SG element of no bytes between two that have some",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.data = etx_plain60, .head = 20, .nsge = 3, .sge = {20, 0, 20}},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"nine SG elements on a version 0 queue, whose lists hold eight",
     0,
     QINIT_F_IRQ | QINIT_F_SG,
     {.data = etx_plain60,
      .head = 6,
      .nsge = 9,
      .sge = {6, 6, 6, 6, 6, 6, 6, 6, 6}},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"an SG element on a queue initialized without an SG ring",
     1,
     QINIT_F_IRQ,
     {.data = etx_plain60, .head = 30, .nsge = 1, .sge = {30}},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"a VLAN tag to insert, an offload that was not granted",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.flags = FLAG_VLAN, .data = etx_plain60, .head = 60},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_NONE with the flag that starts a TSO chain",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.flags = FLAG_SOT, .data = etx_plain60, .head = 60},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_PARTIAL with the flag that ends a TSO chain",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = OP_CSUM_PARTIAL,
      .flags = FLAG_EOT,
      .hword1 = 34,
      .hword2 = 16,
      .data = etx_cp_tcp4_in,
      .head = sizeof(etx_cp_tcp4_in)},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_HW, which the Linux driver never posts",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = OP_CSUM_HW,
      .flags = 0xc, /* CSUM_L3 | CSUM_L4 */
      .data = etx_cp_tcp4_in,
      .head = sizeof(etx_cp_tcp4_in)},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"opcode 4, which there isn't",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = 4, .data = etx_plain60, .head = 60},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_PARTIAL whose checksum field's second byte is past the frame",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = OP_CSUM_PARTIAL,
      .hword1 = 34,
      .hword2 = sizeof(etx_cp_tcp4_in) - 34 - 1,
      .data = etx_cp_tcp4_in,
      .head = sizeof(etx_cp_tcp4_in)},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_PARTIAL with csum_start past the frame",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = OP_CSUM_PARTIAL,
      .hword1 = sizeof(etx_cp_tcp4_in) + 1,
      .hword2 = 0,
      .data = etx_cp_tcp4_in,
      .head = sizeof(etx_cp_tcp4_in)},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"CSUM_PARTIAL with csum_start and csum_offset both 65535",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.opcode = OP_CSUM_PARTIAL,
      .hword1 = UINT16_MAX,
      .hword2 = UINT16_MAX,
      .data = etx_cp_tcp4_in,
      .head = sizeof(etx_cp_tcp4_in)},
     IONIC_LIF_TX_DESC_DATA_ERROR},
    {"a first buffer at an address with no memory behind it",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.data = etx_plain60,
      .head = 30,
      .nsge = 1,
      .sge = {30},
      .head_addr = NOWHERE},
     IONIC_LIF_TX_DMA_ERROR},
    {"a second SG element at an address with no memory behind it",
     1,
     QINIT_F_IRQ | QINIT_F_SG,
     {.data = etx_plain60,
      .head = 20,
      .nsge = 2,
      .sge = {20, 20},
      .sge_addr = {0, NOWHERE}},
     IONIC_LIF_TX_DMA_ERROR},
};

/* Each is posted between two good frames, which are sent either side. */
static void run_refuse_case(const struct refuse_case *c)
{
    struct ionic_eth_emu *emu = tx_emu(c->ver, c->qflags);
    size_t sg_size = c->ver ? TXQ_SG_V1_DESC_SIZE : TXQ_SG_DESC_SIZE;
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};

    post(0, &plain, sg_size);
    post(1, &c->d, sg_size);
    post(2, &plain, sg_size);
    kick(emu, 3);
    report(sent.n == 2 && sent_is(0, etx_plain60, sizeof(etx_plain60)) &&
               sent_is(1, etx_plain60, sizeof(etx_plain60)) &&
               completed(0, 0) && completed(1, 1) && completed(2, 2) &&
               tx_head(emu) == 3 && lif_stat(emu, c->counter) == 1 &&
               tx_packets(emu) == 2,
           "negative", c->description,
           "nothing sent for it; counted once; completed, and the frames "
           "either side sent");
    ionic_eth_emu_destroy(emu);
}

/* An SG ring that isn't in guest memory: the list can't be fetched. */
static void check_sg_ring_unmapped(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd d = {
        .data = etx_plain60, .head = 30, .nsge = 1, .sge = {30}};

    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    emu->eth_q[IONIC_QTYPE_TXQ][TXQ_ID].sg_ring_base = NOWHERE;
    kick(emu, 1);
    report(sent.n == 0 && completed(0, 0) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_FETCH_ERROR) == 1,
           "negative", "an SG list in an SG ring with no memory behind it",
           "nothing sent; a descriptor fetch error; completed");
    ionic_eth_emu_destroy(emu);
}

/*
 * The guest chooses where its SG ring is.  Here it is 448 bytes below the
 * top of the address space, so the third descriptor's list, 512 bytes
 * further on, would be at 0x40 if the sum were left to wrap; and at 0x40
 * there is a list that would do.
 */
static void check_sg_ring_wraps(void)
{
    const uint64_t wrapped = 0x40;
    struct ionic_eth_emu *emu =
        tx_emu_sg(1, QINIT_F_IRQ | QINIT_F_SG,
                  wrapped - ((uint64_t)2 * TXQ_SG_V1_DESC_SIZE));
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    const struct txd d = {
        .data = etx_plain60, .head = 30, .nsge = 1, .sge = {30}};

    post(0, &plain, TXQ_SG_V1_DESC_SIZE);
    post(1, &plain, TXQ_SG_V1_DESC_SIZE);
    post(2, &d, TXQ_SG_V1_DESC_SIZE);
    memcpy(g_mem + wrapped, g_mem + TXQ_SG + ((size_t)2 * TXQ_SG_V1_DESC_SIZE),
           TXQ_SG_ELEM_SIZE);
    kick(emu, 3);
    report(sent.n == 2 && completed(2, 2) && tx_head(emu) == 3 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_FETCH_ERROR) == 1 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "negative",
           "an SG list whose address, sg_ring_base plus its offset, is past "
           "the end of the address space",
           "not read from where the sum wraps to: a descriptor fetch error; "
           "completed");
    ionic_eth_emu_destroy(emu);
}

/* A queue that sets the SG flag and names no SG ring has none: the list is
 * not read from address 0, where there is one that would do. */
static void check_sg_ring_absent(void)
{
    struct ionic_eth_emu *emu = tx_emu_sg(1, QINIT_F_IRQ | QINIT_F_SG, 0);
    const struct txd d = {
        .data = etx_plain60, .head = 30, .nsge = 1, .sge = {30}};

    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    memcpy(g_mem, g_mem + TXQ_SG, TXQ_SG_ELEM_SIZE);
    kick(emu, 1);
    report(sent.n == 0 && completed(0, 0) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_FETCH_ERROR) == 0,
           "negative",
           "an SG element on a queue initialized with the SG flag and an "
           "sg_ring_base of 0",
           "nothing sent; a descriptor data error; completed");
    ionic_eth_emu_destroy(emu);
}

/* ---- CSUM_PARTIAL -------------------------------------------------------- */

struct csum_case {
    const char *description;
    const uint8_t *in;
    const uint8_t *out;
    size_t len;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t head; /* the rest is in two SG elements */
    uint8_t flags;
};

#define CSUM_FRAMES(name) \
    etx_cp_##name##_in, etx_cp_##name##_out, sizeof(etx_cp_##name##_in)

static const struct csum_case csum_cases[] = {
    {"TCP over IPv4", CSUM_FRAMES(tcp4), 34, 16, 54, 0},
    {"UDP over IPv4", CSUM_FRAMES(udp4), 34, 6, 42, 0},
    {"UDP over IPv4 whose checksum computes to 0, which UDP sends as ffff",
     CSUM_FRAMES(udp4_zero), 34, 6, 42, 0},
    {"TCP over IPv6", CSUM_FRAMES(tcp6), 54, 16, 74, 0},
    {"UDP over IPv6", CSUM_FRAMES(udp6), 54, 6, 62, 0},
    {"TCP over IPv4 with the ENCAP flag, and the headers split over buffers",
     CSUM_FRAMES(tcp4), 34, 16, 41, FLAG_ENCAP},
};

static void run_csum_case(const struct csum_case *c)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    size_t rest = c->len - c->head;
    const struct txd d = {
        .opcode = OP_CSUM_PARTIAL,
        .flags = c->flags,
        .hword1 = c->csum_start,
        .hword2 = c->csum_offset,
        .data = c->in,
        .head = c->head,
        .nsge = 2,
        .sge = {(uint16_t)(rest / 2), (uint16_t)(rest - (rest / 2))}};

    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 1);
    report(sent.n == 1 && sent_is(0, c->out, c->len) && completed(0, 0) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "positive", c->description,
           "the frame with its checksum filled in, nothing else changed");
    ionic_eth_emu_destroy(emu);
}

/* The checksum field may be the frame's last two bytes: a sum over those
 * two bytes alone, which hold 0x1234, is its complement. */
static void check_csum_at_end(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    uint8_t in[sizeof(etx_plain60)];
    uint8_t out[sizeof(etx_plain60)];
    const struct txd d = {.opcode = OP_CSUM_PARTIAL,
                          .hword1 = sizeof(in) - 2,
                          .hword2 = 0,
                          .data = in,
                          .head = sizeof(in)};

    memcpy(in, etx_plain60, sizeof(in));
    in[sizeof(in) - 2] = 0x12;
    in[sizeof(in) - 1] = 0x34;
    memcpy(out, in, sizeof(out));
    out[sizeof(out) - 2] = 0xed;
    out[sizeof(out) - 1] = 0xcb;
    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 1);
    report(sent.n == 1 && sent_is(0, out, sizeof(out)), "corner",
           "CSUM_PARTIAL whose checksum field is the frame's last two bytes",
           "filled in");
    ionic_eth_emu_destroy(emu);
}

/* ---- TSO ----------------------------------------------------------------- */

/* Descriptor @i of the Tx ring, counting on past its end. */
static uint16_t ring_index(unsigned int i)
{
    return (uint16_t)(i % TXQ_DEPTH);
}

#define SEGS_MAX 3U

struct tso_case {
    const char *description;
    const uint8_t *in; /* the headers, then all the payload */
    size_t in_len;
    uint16_t hdr_len;
    uint16_t mss;
    uint16_t head0; /* the first descriptor's first buffer */
    uint16_t start; /* where on the ring the chain starts */
    unsigned int nsegs;
    const uint8_t *seg[SEGS_MAX];
    size_t seg_len[SEGS_MAX];
};

#define SEG(name) name, sizeof(name)
#define SEGS1(n)         \
    {n##_seg0},          \
    {                    \
        sizeof(n##_seg0) \
    }
#define SEGS2(n)                           \
    {n##_seg0, n##_seg1},                  \
    {                                      \
        sizeof(n##_seg0), sizeof(n##_seg1) \
    }
#define SEGS3(n)                                             \
    {n##_seg0, n##_seg1, n##_seg2},                          \
    {                                                        \
        sizeof(n##_seg0), sizeof(n##_seg1), sizeof(n##_seg2) \
    }

static const struct tso_case tso_cases[] = {
    {"IPv4, mss 8, 20 bytes of payload, CWR, PSH and FIN set, sequence "
     "numbers and IP identification that wrap, on descriptors 6, 7 and 0",
     SEG(etx_tso4_in), 54, 8, 54, 6, 3, SEGS3(etx_tso4)},
    {"IPv6 behind an 802.1Q tag the stack put in the frame, mss 16, 33 "
     "bytes of payload, checksum fields not prepared, headers split over "
     "two buffers",
     SEG(etx_tso6_vlan_in), 78, 16, 40, 0, 3, SEGS3(etx_tso6_vlan)},
    {"IPv6 with a hop-by-hop options header, mss 6, 10 bytes of payload",
     SEG(etx_tso6_hbh_in), 82, 6, 82, 0, 2, SEGS2(etx_tso6_hbh)},
    {"IPv4 with IP and TCP options, mss 1000, 5 bytes of payload: one "
     "descriptor that both starts and ends its chain",
     SEG(etx_tso4_opts_in), 70, 1000, 70, 0, 1, SEGS1(etx_tso4_opts)},
    {"IPv6 behind an 802.1ad tag, mss 16, 20 bytes of payload",
     SEG(etx_tso6_qinq_in), 78, 16, 78, 0, 2, SEGS2(etx_tso6_qinq)},
    {"IPv6 with a destination-options header and a routing header with no "
     "segments left, mss 10, 15 bytes of payload, CWR, PSH and FIN set, a "
     "sequence number that wraps, headers split over two buffers",
     SEG(etx_tso6_dst_rt_in), 106, 10, 60, 0, 2, SEGS2(etx_tso6_dst_rt)},
    {"IPv6 with 254 bytes of headers, the most that fit the 256 the device "
     "takes: a 176-byte hop-by-hop header and four bytes of TCP options",
     SEG(etx_tso6_hbh254_in), 254, 6, 254, 0, 2, SEGS2(etx_tso6_hbh254)},
};

/*
 * Post the chain ionic_tx_tso() builds for the packet @in at @index
 * onwards: @hdr_len bytes of headers and the first @mss of payload in the
 * first descriptor, whose first buffer is @head0 bytes, and @mss more in
 * each one after, the last taking what is left.  Returns how many
 * descriptors that was.
 */
static uint16_t post_tso(uint16_t index, const uint8_t *in, size_t in_len,
                         uint16_t hdr_len, uint16_t mss, uint16_t head0)
{
    size_t off = 0;
    uint16_t n = 0;

    while (off < in_len) {
        size_t bytes = in_len - off;
        size_t most = off == 0 ? (size_t)hdr_len + mss : mss;
        struct txd d = {.opcode = OP_TSO,
                        .flags = off == 0 ? FLAG_SOT : 0,
                        .hword1 = hdr_len,
                        .hword2 = mss,
                        .data = in + off};

        if (bytes > most) {
            bytes = most;
        }
        if (off + bytes == in_len) {
            d.flags |= FLAG_EOT;
        }
        /* Each in two buffers if it has two bytes to put in them. */
        d.head = off == 0 ? head0 : (uint16_t)((bytes + 1) / 2);
        if (bytes > d.head) {
            d.nsge = 1;
            d.sge[0] = (uint16_t)(bytes - d.head);
        }
        post(ring_index((unsigned int)index + n), &d, TXQ_SG_V1_DESC_SIZE);
        off += bytes;
        n++;
    }
    return n;
}

static void run_tso_case(const struct tso_case *c)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    bool ok = true;
    uint16_t n;

    for (uint16_t i = 0; i < c->start; i++) {
        post(i, &plain, TXQ_SG_V1_DESC_SIZE);
    }
    kick(emu, c->start);
    n = post_tso(c->start, c->in, c->in_len, c->hdr_len, c->mss, c->head0);
    kick(emu, ring_index((unsigned int)c->start + n));

    for (unsigned int i = 0; i < c->nsegs; i++) {
        if (!sent_is(c->start + i, c->seg[i], c->seg_len[i])) {
            printf("     segment %u is wrong\n", i);
            ok = false;
        }
    }
    /* One completion for the chain, naming its last descriptor, in the
     * slot after the frames before it; none for the others. */
    report(
        ok && n == c->nsegs && sent.n == c->start + c->nsegs &&
            completed(c->start, ring_index((unsigned int)c->start + n - 1U)) &&
            not_completed(ring_index(c->start + 1U)) &&
            tx_head(emu) == ring_index((unsigned int)c->start + n) &&
            tx_packets(emu) == c->start + c->nsegs &&
            lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
        "positive", c->description,
        "each segment behind the headers, with its own lengths, "
        "identification, sequence number, flags and checksums; one "
        "completion; each segment counted");
    ionic_eth_emu_destroy(emu);
}

/*
 * A chain of two descriptors that is refused.  It is the packet @in,
 * @hdr_len bytes of headers, cut after the first @mss bytes of payload,
 * and then made wrong in the one way the row names.
 */
struct tso_bad {
    const char *description;
    const uint8_t *in;
    size_t in_len;
    uint16_t hdr_len;
    uint16_t mss;
    /* The first descriptor: its flags, a hdr_len to claim other than
     * @hdr_len, whether it brings the headers alone or one byte of payload
     * more than mss, and up to two bytes of the headers to change. */
    uint8_t flags0;
    uint16_t claim_hdr_len;
    bool no_payload0;
    bool big0;
    uint16_t poke_off;
    uint8_t poke_val;
    uint16_t poke2_off;
    uint8_t poke2_val;
    /* The second: flags besides TSO_EOT, an mss and hdr_len other than
     * the first's, and whether its first buffer is empty. */
    uint8_t flags1;
    uint16_t mss1;
    uint16_t hdr_len1;
    bool empty1;
    /* Or no second: the first has TSO_EOT too, and all the payload. */
    bool single;
};

static const struct tso_bad tso_bad_cases[] = {
    {.description = "a chain whose first descriptor lacks TSO_SOT",
     .flags0 = 0},
    {.description = "an mss of 0",
     .flags0 = FLAG_SOT,
     .mss = 0,
     .single = true},
    {.description = "a hdr_len of 300, more headers than the device takes",
     .flags0 = FLAG_SOT,
     .claim_hdr_len = 300},
    {.description = "an mss of 9547, which with 54 bytes of headers is one "
                    "byte more than a frame can be",
     .flags0 = FLAG_SOT,
     .mss = 9547,
     .single = true},
    {.description = "one descriptor that brings the headers and no payload",
     .flags0 = FLAG_SOT,
     .mss = 1000,
     .no_payload0 = true,
     .single = true},
    {.description = "a first descriptor that brings one byte of payload "
                    "more than mss",
     .flags0 = FLAG_SOT,
     .big0 = true},
    {.description = "a hdr_len of 10, less than an Ethernet header",
     .flags0 = FLAG_SOT,
     .mss = 1000,
     .claim_hdr_len = 10,
     .single = true},
    {.description = "a hdr_len of 30, which ends inside the IPv4 header",
     .flags0 = FLAG_SOT,
     .mss = 1000,
     .claim_hdr_len = 30,
     .single = true},
    {.description = "a hdr_len of 16, which ends inside the VLAN tag",
     .in = etx_tso6_vlan_in,
     .in_len = sizeof(etx_tso6_vlan_in),
     .hdr_len = 78,
     .mss = 1000,
     .flags0 = FLAG_SOT,
     .claim_hdr_len = 16,
     .single = true},
    {.description = "a hdr_len of 50, which ends inside the IPv6 header",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 1000,
     .flags0 = FLAG_SOT,
     .claim_hdr_len = 50,
     .single = true},
    {.description = "a hdr_len of 58, which ends inside the first 8 bytes "
                    "of an IPv6 extension header",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 1000,
     .flags0 = FLAG_SOT,
     .claim_hdr_len = 58,
     .single = true},
    {.description = "a hdr_len one byte short of the end of the TCP header",
     .flags0 = FLAG_SOT,
     .mss = 1000,
     .claim_hdr_len = 53,
     .single = true},
    {.description = "a hdr_len one byte past the end of the TCP header",
     .flags0 = FLAG_SOT,
     .claim_hdr_len = 55},
    {.description = "a VLAN tag to insert in each segment",
     .flags0 = FLAG_SOT | FLAG_VLAN},
    {.description = "tunnel headers to fix up in each segment (ENCAP)",
     .flags0 = FLAG_SOT | FLAG_ENCAP},
    {.description = "a second descriptor with a different mss",
     .flags0 = FLAG_SOT,
     .mss1 = 13},
    {.description = "a second descriptor with a different hdr_len",
     .flags0 = FLAG_SOT,
     .hdr_len1 = 50},
    {.description = "a second descriptor that brings more than mss bytes",
     .flags0 = FLAG_SOT,
     .mss = 8},
    {.description = "a second descriptor with the VLAN flag",
     .flags0 = FLAG_SOT,
     .flags1 = FLAG_VLAN},
    {.description = "a second descriptor with the ENCAP flag",
     .flags0 = FLAG_SOT,
     .flags1 = FLAG_ENCAP},
    {.description = "a second descriptor whose first buffer is empty",
     .flags0 = FLAG_SOT,
     .empty1 = true},
    {.description = "headers that are UDP, not TCP",
     .in = etx_cp_udp4_in,
     .in_len = sizeof(etx_cp_udp4_in),
     .hdr_len = 42,
     .mss = 5,
     .flags0 = FLAG_SOT},
    {.description = "a TCP header with SYN set",
     .flags0 = FLAG_SOT,
     .poke_off = 47,
     .poke_val = 0x12},
    {.description = "a TCP header with RST set",
     .flags0 = FLAG_SOT,
     .poke_off = 47,
     .poke_val = 0x14},
    {.description = "a TCP header whose data offset is 16 bytes",
     .flags0 = FLAG_SOT,
     .poke_off = 46,
     .poke_val = 0x40},
    {.description = "an IPv4 header that is a first fragment: more "
                    "fragments, offset 0",
     .flags0 = FLAG_SOT,
     .poke_off = 20,
     .poke_val = 0x20},
    {.description = "an IPv4 header that is a last fragment: no more "
                    "fragments, offset 8",
     .flags0 = FLAG_SOT,
     .poke_off = 21,
     .poke_val = 0x01},
    {.description = "an IPv4 header whose length is 16 bytes",
     .flags0 = FLAG_SOT,
     .poke_off = 14,
     .poke_val = 0x44},
    {.description = "an IPv4 ethertype in front of an IPv6 header",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 6,
     .flags0 = FLAG_SOT,
     .poke_off = 12,
     .poke_val = 0x08,
     .poke2_off = 13,
     .poke2_val = 0x00},
    {.description = "an IPv6 ethertype in front of an IPv4 header",
     .flags0 = FLAG_SOT,
     .poke_off = 12,
     .poke_val = 0x86,
     .poke2_off = 13,
     .poke2_val = 0xdd},
    {.description = "an IPv4 header in every way but its version, which "
                    "is 6",
     .flags0 = FLAG_SOT,
     .poke_off = 14,
     .poke_val = 0x65},
    {.description = "an IPv6 header in every way but its version, which "
                    "is 4",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 6,
     .flags0 = FLAG_SOT,
     .poke_off = 14,
     .poke_val = 0x40},
    {.description = "two VLAN tags, 802.1Q inside 802.1Q",
     .in = etx_tso6_vlan_in,
     .in_len = sizeof(etx_tso6_vlan_in),
     .hdr_len = 78,
     .mss = 20,
     .flags0 = FLAG_SOT,
     .poke_off = 16,
     .poke_val = 0x81,
     .poke2_off = 17,
     .poke2_val = 0x00},
    {.description = "two VLAN tags, 802.1ad inside 802.1Q",
     .in = etx_tso6_vlan_in,
     .in_len = sizeof(etx_tso6_vlan_in),
     .hdr_len = 78,
     .mss = 20,
     .flags0 = FLAG_SOT,
     .poke_off = 16,
     .poke_val = 0x88,
     .poke2_off = 17,
     .poke2_val = 0xa8},
    {.description = "an IPv6 fragment header after the hop-by-hop options",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 6,
     .flags0 = FLAG_SOT,
     .poke_off = 54,
     .poke_val = 44},
    {.description = "an IPv6 routing header with segments left, so the "
                    "IPv6 destination is not the one TCP summed",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 6,
     .flags0 = FLAG_SOT,
     .poke_off = 20,
     .poke_val = 43},
    {.description = "an IPv6 extension header that runs past hdr_len",
     .in = etx_tso6_hbh_in,
     .in_len = sizeof(etx_tso6_hbh_in),
     .hdr_len = 82,
     .mss = 6,
     .flags0 = FLAG_SOT,
     .poke_off = 55,
     .poke_val = 3},
};

/* Each is posted between two good frames, which are sent either side. */
static void run_tso_bad(const struct tso_bad *c)
{
    static uint8_t in[IONIC_ETH_NET_MTU_MAX];
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    /* Unless the row says otherwise: the IPv4 packet, cut 12 and 8. */
    size_t in_len = c->in ? c->in_len : sizeof(etx_tso4_in);
    uint16_t hdr_len = c->in ? c->hdr_len : 54;
    uint16_t mss = c->in || c->single || c->mss ? c->mss : 12;
    size_t payload0 = c->no_payload0 ? 0 : in_len - hdr_len;
    uint16_t last;

    memset(in, 0, sizeof(in));
    memcpy(in, c->in ? c->in : etx_tso4_in, in_len);
    if (c->poke_off) {
        in[c->poke_off] = c->poke_val;
    }
    if (c->poke2_off) {
        in[c->poke2_off] = c->poke2_val;
    }
    if (!c->single && !c->no_payload0 && payload0 > mss) {
        /* The second descriptor has the rest, which is at least a byte. */
        payload0 = c->big0 ? (size_t)mss + 1 : mss;
    }

    struct txd d0 = {.opcode = OP_TSO,
                     .flags = c->flags0,
                     .hword1 = c->claim_hdr_len ? c->claim_hdr_len : hdr_len,
                     .hword2 = mss,
                     .data = in,
                     .head = hdr_len};
    struct txd d1 = {.opcode = OP_TSO,
                     .flags = FLAG_EOT | c->flags1,
                     .hword1 = c->hdr_len1 ? c->hdr_len1 : d0.hword1,
                     .hword2 = c->mss1 ? c->mss1 : mss,
                     .data = in + hdr_len + payload0,
                     .head = (uint16_t)(in_len - hdr_len - payload0)};

    if (payload0 != 0) {
        d0.nsge = 1;
        d0.sge[0] = (uint16_t)payload0;
    }
    if (c->empty1) {
        d1.nsge = 1;
        d1.sge[0] = d1.head;
        d1.head = 0;
    }
    post(0, &plain, TXQ_SG_V1_DESC_SIZE);
    if (c->single) {
        d0.flags |= FLAG_EOT;
        post(1, &d0, TXQ_SG_V1_DESC_SIZE);
        last = 1;
    } else {
        post(1, &d0, TXQ_SG_V1_DESC_SIZE);
        post(2, &d1, TXQ_SG_V1_DESC_SIZE);
        last = 2;
    }
    post((uint16_t)(last + 1), &plain, TXQ_SG_V1_DESC_SIZE);
    kick(emu, (uint16_t)(last + 2));
    report(sent.n == 2 && sent_is(0, etx_plain60, sizeof(etx_plain60)) &&
               sent_is(1, etx_plain60, sizeof(etx_plain60)) &&
               completed(0, 0) && completed(1, last) &&
               completed(2, (uint16_t)(last + 1)) && not_completed(3) &&
               tx_head(emu) == last + 2 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1 &&
               tx_packets(emu) == 2,
           "negative", c->description,
           "no segment sent; the chain counted once as a descriptor data "
           "error and completed once; the frames either side sent");
    ionic_eth_emu_destroy(emu);
}

/*
 * A first descriptor that brings fewer bytes than hdr_len.  The device's
 * frame buffer still holds the chain before it, which had the same
 * headers, so the bytes the descriptor did not bring are there to be read
 * by a device that parses hdr_len bytes without asking how many it has.
 */
static void check_tso_short_first(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd d = {.opcode = OP_TSO,
                          .flags = FLAG_SOT | FLAG_EOT,
                          .hword1 = 70,
                          .hword2 = 1000,
                          .data = etx_tso4_opts_in,
                          .head = 69};

    post_tso(0, etx_tso4_opts_in, sizeof(etx_tso4_opts_in), 70, 1000, 70);
    post(1, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 2);
    report(sent.n == 1 && sent_is(0, SEG(etx_tso4_opts_seg0)) &&
               completed(0, 0) && completed(1, 1) && tx_head(emu) == 2 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1 &&
               tx_packets(emu) == 1,
           "negative",
           "a chain of one descriptor that brings 69 of its 70 bytes of "
           "headers, after a chain with the same headers",
           "refused as a descriptor data error; the chain before it sent");
    ionic_eth_emu_destroy(emu);
}

/*
 * Headers longer than the device takes, and valid in every other way: the
 * 254-byte headers with eight bytes more of hop-by-hop options, 262 in all.
 */
static void check_tso_hdr_bound(void)
{
    /* The hop-by-hop header is at 54: its length, in units of 8 bytes
     * beyond the first 8, at 55, and a PadN option's at 57. */
    static uint8_t in[sizeof(etx_tso6_hbh254_in) + 8];
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    struct txd d = {.opcode = OP_TSO,
                    .flags = FLAG_SOT | FLAG_EOT,
                    .hword1 = 262,
                    .hword2 = 100,
                    .data = in,
                    .head = sizeof(in)};

    memcpy(in, etx_tso6_hbh254_in, 62);
    memset(in + 62, 0, 8);
    memcpy(in + 70, etx_tso6_hbh254_in + 62, sizeof(etx_tso6_hbh254_in) - 62);
    in[55] = (uint8_t)(in[55] + 1);
    in[57] = (uint8_t)(in[57] + 8);
    post(0, &d, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 1);
    report(sent.n == 0 && completed(0, 0) && tx_head(emu) == 1 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1,
           "negative",
           "a hdr_len of 262, well-formed headers that are more than the "
           "256 bytes the device takes",
           "refused as a descriptor data error, and completed");
    ionic_eth_emu_destroy(emu);
}

/* The start of a chain, then something that is not the rest of it. */
static void check_tso_cut_short(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd plain = {.data = etx_plain60, .head = sizeof(etx_plain60)};
    const struct txd start = {.opcode = OP_TSO,
                              .flags = FLAG_SOT,
                              .hword1 = 54,
                              .hword2 = 8,
                              .data = etx_tso4_in,
                              .head = 62};

    post(0, &start, TXQ_SG_V1_DESC_SIZE);
    post(1, &plain, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 2);
    report(sent.n == 1 && sent_is(0, etx_plain60, sizeof(etx_plain60)) &&
               completed(0, 0) && completed(1, 1) && tx_head(emu) == 2 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1,
           "negative",
           "the first descriptor of a TSO chain, then a CSUM_NONE frame",
           "the chain ends before the frame and is refused; the frame is "
           "sent; each completed");

    /* ...and by the start of another chain, which is whole. */
    memset(&sent, 0, sizeof(sent));
    post(2, &start, TXQ_SG_V1_DESC_SIZE);
    post_tso(3, etx_tso4_in, sizeof(etx_tso4_in), 54, 8, 54);
    kick(emu, 6);
    report(sent.n == 3 && sent_is(0, SEG(etx_tso4_seg0)) &&
               sent_is(2, SEG(etx_tso4_seg2)) && completed(2, 2) &&
               completed(3, 5) && tx_head(emu) == 6 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 2,
           "negative",
           "the first descriptor of a TSO chain, then a whole chain",
           "the first refused and completed alone; the second sent");
    ionic_eth_emu_destroy(emu);
}

/* A doorbell that covers only part of a chain, which the Linux driver
 * never rings: nothing is sent or completed until the rest is posted. */
static void check_tso_waits(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    uint16_t n = post_tso(0, etx_tso4_in, sizeof(etx_tso4_in), 54, 8, 54);

    kick(emu, 2);
    report(n == 3 && sent.n == 0 && not_completed(0) && tx_head(emu) == 0 &&
               tx_packets(emu) == 0 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "corner",
           "a doorbell after two descriptors of a three-descriptor chain",
           "nothing sent, completed or counted");
    kick(emu, 3);
    report(sent.n == 3 && sent_is(0, SEG(etx_tso4_seg0)) &&
               sent_is(1, SEG(etx_tso4_seg1)) &&
               sent_is(2, SEG(etx_tso4_seg2)) && completed(0, 2) &&
               not_completed(1) && tx_head(emu) == 3,
           "corner", "then the doorbell after the third",
           "all three segments, and one completion");
    ionic_eth_emu_destroy(emu);
}

/* A payload buffer that can't be read, found only once the first segment
 * has gone: what was sent stays sent, and the chain is one drop. */
static void check_tso_dma_fault(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct txd d0 = {.opcode = OP_TSO,
                           .flags = FLAG_SOT,
                           .hword1 = 54,
                           .hword2 = 8,
                           .data = etx_tso4_in,
                           .head = 62};
    const struct txd d1 = {.opcode = OP_TSO,
                           .hword1 = 54,
                           .hword2 = 8,
                           .data = etx_tso4_in + 62,
                           .head = 8,
                           .head_addr = NOWHERE};
    const struct txd d2 = {.opcode = OP_TSO,
                           .flags = FLAG_EOT,
                           .hword1 = 54,
                           .hword2 = 8,
                           .data = etx_tso4_in + 70,
                           .head = 4};

    post(0, &d0, TXQ_SG_V1_DESC_SIZE);
    post(1, &d1, TXQ_SG_V1_DESC_SIZE);
    post(2, &d2, TXQ_SG_V1_DESC_SIZE);
    kick(emu, 3);
    report(sent.n == 1 && sent_is(0, SEG(etx_tso4_seg0)) && completed(0, 2) &&
               not_completed(1) && tx_head(emu) == 3 &&
               lif_stat(emu, IONIC_LIF_TX_DMA_ERROR) == 1 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 0,
           "negative",
           "a three-descriptor chain whose second descriptor's buffer has "
           "no memory behind it",
           "the first segment sent, the rest not; one DMA error; one "
           "completion for the chain");
    ionic_eth_emu_destroy(emu);
}

/*
 * The device checks a chain whole and then reads it again to send it, and
 * the guest can write to the ring in between.  Here it does, as the first
 * segment goes out: what the device then finds is not what it checked.
 */
static void rewrite_mss(void)
{
    ernic_put_le16(g_mem + TXQ_RING + ETH_DESC_SIZE + 14, 9000);
}

static void rewrite_len(void)
{
    ernic_put_le16(g_mem + TXQ_RING + ETH_DESC_SIZE + 8, 9000);
}

/* The second descriptor of three becomes the chain's last... */
static void rewrite_eot_set(void)
{
    g_mem[TXQ_RING + ETH_DESC_SIZE] |= FLAG_EOT;
}

/* ...or the third no longer is. */
static void rewrite_eot_clear(void)
{
    g_mem[TXQ_RING + (2 * ETH_DESC_SIZE)] &= (uint8_t)~FLAG_EOT;
}

/* Or the ring is no longer in guest memory at all. */
static void unmap_ring(void)
{
    sent.emu->eth_q[IONIC_QTYPE_TXQ][TXQ_ID].ring_base = NOWHERE;
}

static void check_tso_rewritten(void)
{
    static const struct {
        const char *description;
        void (*rewrite)(void);
        enum ionic_lif_stat counter;
        unsigned int nsent; /* segments that had gone by then */
    } rows[] = {
        {"a chain whose second descriptor's mss the guest rewrites",
         rewrite_mss, IONIC_LIF_TX_DESC_DATA_ERROR, 1},
        {"a chain whose second descriptor's first buffer the guest makes "
         "longer than mss",
         rewrite_len, IONIC_LIF_TX_DESC_DATA_ERROR, 1},
        {"a chain whose second descriptor the guest gives TSO_EOT",
         rewrite_eot_set, IONIC_LIF_TX_DESC_DATA_ERROR, 1},
        {"a chain whose third and last descriptor the guest takes TSO_EOT "
         "from",
         rewrite_eot_clear, IONIC_LIF_TX_DESC_DATA_ERROR, 2},
        {"a chain whose ring leaves guest memory", unmap_ring,
         IONIC_LIF_TX_DESC_FETCH_ERROR, 1},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
        char what[128];

        post_tso(0, etx_tso4_in, sizeof(etx_tso4_in), 54, 8, 54);
        sent.on_next_frame = rows[i].rewrite;
        kick(emu, 3);
        (void)snprintf(what, sizeof(what), "%s once the device has checked it",
                       rows[i].description);
        report(sent.n == rows[i].nsent && sent_is(0, SEG(etx_tso4_seg0)) &&
                   (rows[i].nsent < 2 || sent_is(1, SEG(etx_tso4_seg1))) &&
                   completed(0, 2) && not_completed(1) && tx_head(emu) == 3 &&
                   lif_stat(emu, rows[i].counter) == 1 &&
                   tx_packets(emu) == rows[i].nsent,
               "negative", what,
               "the segments before it sent, the rest not; one descriptor "
               "error; one completion for the chain");
        ionic_eth_emu_destroy(emu);
    }
}

/* A descriptor of the chain that can't be read ends it. */
static void check_tso_fetch_fault(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    /* A ring whose first two descriptors are the last 32 bytes of guest
     * memory, so its third has none behind it. */
    size_t ring = GUEST_MEM_SIZE - ((size_t)2 * ETH_DESC_SIZE);

    buf_top = ring; /* no frame buffer where the ring will be */
    post_tso(0, etx_tso4_in, sizeof(etx_tso4_in), 54, 8, 54);
    memcpy(g_mem + ring, g_mem + TXQ_RING, (size_t)2 * ETH_DESC_SIZE);
    emu->eth_q[IONIC_QTYPE_TXQ][TXQ_ID].ring_base = ring;
    kick(emu, 3);
    report(sent.n == 0 && completed(0, 2) && not_completed(1) &&
               tx_head(emu) == 3 &&
               lif_stat(emu, IONIC_LIF_TX_DESC_FETCH_ERROR) == 1,
           "negative",
           "a three-descriptor chain whose third descriptor can't be read",
           "nothing sent; one descriptor fetch error; one completion, for "
           "the unreadable descriptor");
    ionic_eth_emu_destroy(emu);
}

/*
 * A chain of seven descriptors with 54 bytes of headers: six segments as
 * large as a frame allows, 9546 bytes of payload each, and a seventh of
 * @last bytes.  Every one's payload is the same guest memory.
 */
#define BOUND_MSS   ((uint16_t)(IONIC_ETH_NET_MTU_MAX - 54))
#define BOUND_NDESC 7U

static void post_bound_chain(uint16_t last)
{
    for (uint16_t i = 0; i < BOUND_NDESC; i++) {
        struct txd d = {.opcode = OP_TSO,
                        .flags = i == BOUND_NDESC - 1 ? FLAG_EOT : 0,
                        .hword1 = 54,
                        .hword2 = BOUND_MSS,
                        .data = etx_tso4_in,
                        .head = i == BOUND_NDESC - 1 ? last : BOUND_MSS,
                        .head_addr = BUF_BASE};

        if (i == 0) {
            d.flags |= FLAG_SOT;
            d.head = 54;
            d.head_addr = 0;
            d.nsge = 1;
            d.sge[0] = BOUND_MSS;
            d.sge_addr[0] = BUF_BASE;
        }
        post(i, &d, TXQ_SG_V1_DESC_SIZE);
    }
}

/* The guest makes the seventh descriptor as long as the others. */
static void rewrite_bound_last(void)
{
    ernic_put_le16(g_mem + TXQ_RING +
                       ((size_t)(BOUND_NDESC - 1) * ETH_DESC_SIZE) + 8,
                   BOUND_MSS);
}

/* A chain may carry what one TCP send can, 65535 bytes of payload. */
static void check_tso_payload_bound(void)
{
    /* What the seventh segment may bring, and a byte more. */
    const uint16_t most = (uint16_t)(65535U - ((BOUND_NDESC - 1) * BOUND_MSS));

    for (uint16_t last = most; last <= most + 1; last++) {
        struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
        bool fits = last == most;
        char what[96];

        post_bound_chain(last);
        kick(emu, BOUND_NDESC);
        (void)snprintf(what, sizeof(what),
                       "a chain of %u segments with %zu bytes of payload",
                       BOUND_NDESC,
                       ((size_t)(BOUND_NDESC - 1) * BOUND_MSS) + last);
        report(sent.n == (fits ? BOUND_NDESC : 0U) &&
                   (!fits || (sent.len[5] == IONIC_ETH_NET_MTU_MAX &&
                              sent.len[6] == 54U + last)) &&
                   completed(0, BOUND_NDESC - 1) && not_completed(1) &&
                   lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) ==
                       (fits ? 0U : 1U),
               fits ? "corner" : "negative", what,
               fits ? "every segment sent"
                    : "refused whole as a descriptor data error, and "
                      "completed once");
        ionic_eth_emu_destroy(emu);
    }

    /* The bound holds for a chain the guest makes longer once the device
     * has checked it, each descriptor still no more than mss. */
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);

    post_bound_chain(100);
    sent.on_next_frame = rewrite_bound_last;
    kick(emu, BOUND_NDESC);
    report(sent.n == BOUND_NDESC - 1 && completed(0, BOUND_NDESC - 1) &&
               not_completed(1) &&
               lif_stat(emu, IONIC_LIF_TX_DESC_DATA_ERROR) == 1,
           "negative",
           "a chain of 57376 bytes of payload whose last descriptor the "
           "guest rewrites to make it 66822, once the device has checked it",
           "the six segments before it sent, and not the seventh; one "
           "descriptor data error; one completion");
    ionic_eth_emu_destroy(emu);
}

/* ---- Negotiation --------------------------------------------------------- */

/* enum ionic_eth_hw_features, the bits the rows name */
#define HW_TX_SG    (UINT64_C(1) << 5)
#define HW_RX_SG    (UINT64_C(1) << 6)
#define HW_TX_CSUM  (UINT64_C(1) << 7)
#define HW_TSO      (UINT64_C(1) << 8)
#define HW_TSO_IPV6 (UINT64_C(1) << 9)
#define HW_TSO_ECN  (UINT64_C(1) << 10)

/* What ionic_init_nic_features() asks for with more than one queue: bits 0
 * to 16 but RX_SG. */
#define HW_DRIVER_ASKS (UINT64_C(0x1ffff) & ~HW_RX_SG)

/* Every offload the device has. */
#define HW_GRANTED (HW_TX_SG | HW_TX_CSUM | HW_TSO | HW_TSO_IPV6 | HW_TSO_ECN)

/* LIF_SETATTR(FEATURES) of @features over the admin queue, or over the
 * devcmd registers; returns the features the completion grants. */
static uint64_t setattr_features(struct ionic_eth_emu *emu, uint64_t features,
                                 bool devcmd, uint8_t *status)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16];

    memset(comp, 0xa5, sizeof(comp));
    cmd[0] = IONIC_CMD_LIF_SETATTR;
    cmd[1] = 4; /* IONIC_LIF_ATTR_FEATURES */
    ernic_put_le64(cmd + 4, features);
    if (devcmd) {
        uint32_t doorbell = 1;

        ionic_eth_emu_bar0_access(
            emu, (char *)cmd, sizeof(cmd),
            IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_CMD_OFF, true);
        ionic_eth_emu_bar0_access(
            emu, (char *)&doorbell, sizeof(doorbell),
            IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_DOORBELL_OFF, true);
        memcpy(comp,
               emu->bar0 + IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_COMP_OFF,
               sizeof(comp));
    } else {
        memset(comp, 0, sizeof(comp));
        ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);
    }
    *status = comp[0];
    return ernic_get_le64(comp + 4);
}

static void check_features(void)
{
    static const struct {
        const char *description;
        uint64_t ask;
        uint64_t want;
    } rows[] = {
        {"everything the Linux driver asks for", HW_DRIVER_ASKS, HW_GRANTED},
        {"every bit there is", UINT64_MAX, HW_GRANTED},
        {"scatter-gather alone", HW_TX_SG, HW_TX_SG},
        {"the Tx checksum alone", HW_TX_CSUM, HW_TX_CSUM},
        {"TSO over IPv4 alone", HW_TSO, HW_TSO},
        {"TSO over IPv6 and its ECN handling", HW_TSO_IPV6 | HW_TSO_ECN,
         HW_TSO_IPV6 | HW_TSO_ECN},
        {"only what the device has not: Rx checksum and hash, the VLAN "
         "offloads, TSO through tunnels",
         HW_DRIVER_ASKS &
             ~(HW_TX_SG | HW_TX_CSUM | HW_TSO | HW_TSO_IPV6 | HW_TSO_ECN),
         0},
        {"nothing", 0, 0},
    };

    for (int devcmd = 0; devcmd < 2; devcmd++) {
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
            uint8_t status = 0xff;
            uint64_t got =
                setattr_features(emu, rows[i].ask, devcmd != 0, &status);
            char what[160];

            (void)snprintf(what, sizeof(what),
                           "LIF_SETATTR(FEATURES) over the %s asking for %s",
                           devcmd ? "devcmd registers" : "admin queue",
                           rows[i].description);
            if (got != rows[i].want) {
                printf("     granted %#" PRIx64 ", expected %#" PRIx64 "\n",
                       got, rows[i].want);
            }
            report(status == IONIC_RC_SUCCESS && got == rows[i].want,
                   "positive", what,
                   "status 0; the offloads asked for that the device has, "
                   "and no other bit");
            ionic_eth_emu_destroy(emu);
        }
    }
}

static void check_lif_identity_features(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    static uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64] = {IONIC_CMD_LIF_IDENTIFY};
    uint8_t comp[16] = {0};

    ionic_eth_emu_handle_lif_identify(emu, cmd, comp, data);
    /* union ionic_lif_identity: eth.config.features is the le64 at 0x0a8. */
    report(comp[0] == IONIC_RC_SUCCESS &&
               ernic_get_le64(data + 0x0a8) == HW_GRANTED,
           "positive", "LIF_IDENTIFY's eth.config.features",
           "the offloads LIF_SETATTR grants, and no timestamp bit");
    ionic_eth_emu_destroy(emu);
}

/* union ionic_q_identity, and struct ionic_q_identify_comp's ver. */
struct qident {
    uint8_t status;
    uint8_t comp_ver;
    uint8_t version;
    uint8_t supported;
    uint16_t sg_desc_sz;
    uint16_t max_sg_elems;
    uint16_t sg_desc_stride;
};

static struct qident q_identify(struct ionic_eth_emu *emu, uint8_t qtype,
                                uint8_t ver)
{
    static uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64] = {IONIC_CMD_Q_IDENTIFY};
    uint8_t comp[16] = {0};

    cmd[4] = qtype;
    cmd[5] = ver;
    ionic_eth_emu_handle_q_identify(emu, cmd, comp, data);
    return (struct qident){.status = comp[0],
                           .comp_ver = comp[4],
                           .version = data[0],
                           .supported = data[1],
                           .sg_desc_sz = ernic_get_le16(data + 20),
                           .max_sg_elems = ernic_get_le16(data + 22),
                           .sg_desc_stride = ernic_get_le16(data + 24)};
}

static bool qident_is(const struct qident *got, const struct qident *want)
{
    return got->status == want->status && got->comp_ver == want->comp_ver &&
           got->version == want->version && got->supported == want->supported &&
           got->sg_desc_sz == want->sg_desc_sz &&
           got->max_sg_elems == want->max_sg_elems &&
           got->sg_desc_stride == want->sg_desc_stride;
}

static void check_q_identify(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    /* A driver posts 15 SG elements however many the device says it takes,
     * and lays them out in 256 bytes only if the device says version 1 or
     * later and exactly that size. */
    const struct qident v1 = {.comp_ver = 1,
                              .version = 1,
                              .supported = 0x3,
                              .sg_desc_sz = 256,
                              .max_sg_elems = 15,
                              .sg_desc_stride = 16};
    const struct qident v0 = {.supported = 0x3,
                              .sg_desc_sz = 128,
                              .max_sg_elems = 8,
                              .sg_desc_stride = 8};
    struct qident got = q_identify(emu, IONIC_QTYPE_TXQ, 3);

    report(qident_is(&got, &v1), "positive",
           "Q_IDENTIFY of the Tx queue by a driver that knows versions up "
           "to 3, as Linux does",
           "version 1: 256-byte SG lists of 15 usable elements");
    got = q_identify(emu, IONIC_QTYPE_TXQ, 1);
    report(qident_is(&got, &v1), "positive",
           "Q_IDENTIFY of the Tx queue by a driver that knows version 1",
           "version 1");
    got = q_identify(emu, IONIC_QTYPE_TXQ, 0);
    report(qident_is(&got, &v0), "positive",
           "Q_IDENTIFY of the Tx queue by a driver that knows only "
           "version 0",
           "version 0: 128-byte SG lists of 8 elements");
    got = q_identify(emu, IONIC_QTYPE_RXQ, 2);
    report(got.status == IONIC_RC_SUCCESS && got.version == 0 &&
               got.comp_ver == 0 && got.supported == 0x1 &&
               got.sg_desc_sz == 128,
           "positive",
           "Q_IDENTIFY of the Rx queue by a driver that knows versions up "
           "to 2",
           "version 0, the only one, as before");
    ionic_eth_emu_destroy(emu);
}

static void check_q_init_version(void)
{
    struct ionic_eth_emu *emu = tx_emu(1, QINIT_F_IRQ | QINIT_F_SG);
    const struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][2];
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    cmd[0] = IONIC_CMD_Q_INIT;
    cmd[4] = IONIC_QTYPE_TXQ;
    cmd[5] = 2;
    ernic_put_le32(cmd + 8, 2);
    cmd[19] = TXQ_DEPTH_LOG2;
    ernic_put_le64(cmd + 20, TXQ_RING);
    ernic_put_le64(cmd + 28, TXQ_CQ);
    g_nwarnings = 0;
    ionic_eth_emu_handle_q_init(emu, cmd, comp);
    report(comp[0] == IONIC_RC_EVERSION && !q->valid && g_nwarnings == 1,
           "negative",
           "Q_INIT of a Tx queue as version 2, which Q_IDENTIFY never offers",
           "refused with EVERSION and logged; the queue not created");

    /* An Rx queue's version selects nothing, and Linux sends what it was
     * told, 0; another value is not a reason to refuse the queue. */
    cmd[4] = IONIC_QTYPE_RXQ;
    memset(comp, 0, sizeof(comp));
    ionic_eth_emu_handle_q_init(emu, cmd, comp);
    report(comp[0] == IONIC_RC_SUCCESS, "corner",
           "Q_INIT of an Rx queue as version 2", "accepted, as before");
    ionic_eth_emu_destroy(emu);
}

int main(void)
{
    check_plain();
    check_sg_v0();
    check_sg_v1();
    check_frame_bound();
    for (size_t i = 0; i < sizeof(refuse_cases) / sizeof(refuse_cases[0]);
         i++) {
        run_refuse_case(&refuse_cases[i]);
    }
    check_sg_ring_unmapped();
    check_sg_ring_wraps();
    check_sg_ring_absent();
    for (size_t i = 0; i < sizeof(csum_cases) / sizeof(csum_cases[0]); i++) {
        run_csum_case(&csum_cases[i]);
    }
    check_csum_at_end();
    for (size_t i = 0; i < sizeof(tso_cases) / sizeof(tso_cases[0]); i++) {
        run_tso_case(&tso_cases[i]);
    }
    for (size_t i = 0; i < sizeof(tso_bad_cases) / sizeof(tso_bad_cases[0]);
         i++) {
        run_tso_bad(&tso_bad_cases[i]);
    }
    check_tso_short_first();
    check_tso_hdr_bound();
    check_tso_cut_short();
    check_tso_waits();
    check_tso_dma_fault();
    check_tso_rewritten();
    check_tso_fetch_fault();
    check_tso_payload_bound();
    check_features();
    check_lif_identity_features();
    check_q_identify();
    check_q_init_version();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
