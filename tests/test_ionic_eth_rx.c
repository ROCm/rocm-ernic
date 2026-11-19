/*
 * Unit tests for the emulated ionic NIC's Rx header split and Rx checksum
 * reporting, which the [rx] header_split profile key turns on.
 *
 * Each row posts one Rx descriptor and its SG list in fake guest memory the
 * way a driver lays them out, hands the device a frame as the host network
 * would, and compares all of guest memory with what it must then hold:
 * the frame's bytes in the buffers they belong in, one completion, and
 * nothing else touched.  The frames and their sums, in
 * golden/ionic_eth_rx_frames.h, were not made by the emulator.
 *
 * Then what a driver negotiates before any of that: the features
 * LIF_SETATTR grants and LIF_IDENTIFY reports, the Rx queue features
 * Q_IDENTIFY offers and the ones Q_INIT accepts; and that with the key off,
 * as it is by default, the device does none of it.
 *
 * The interface under test is a prototype, written down in
 * docs/ionic.rst: no ionic firmware has it.
 *
 * The test links the eth emulator and reaches its state through
 * ionic_eth_emu_priv.h; the DMA entry points are backed by a plain buffer
 * (ionic_eth_emu_fakes.h), and the fake network backend hands over one
 * frame.
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

#include "byteorder.h"
#include "golden/ionic_eth_rx_frames.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_emu_stats.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic/ionic_eth_emu_priv.h"
#include "ionic/ionic_eth_rx.h"
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
 * Fake guest memory: an 8-descriptor Rx ring, a completion ring with room
 * for the 32-byte entries of a header-split queue, an SG ring, the buffer
 * of the descriptor under test, and the rest for the buffers of its SG
 * elements.
 */
#define RXQ_DEPTH_LOG2 3U
#define RXQ_DEPTH      (1U << RXQ_DEPTH_LOG2)
#define RXQ_RING       0x0100U
#define RXQ_CQ         0x0200U
#define RXQ_SGL        0x0400U
#define BUF0           0x0900U
#define BUF0_CAP       0x0200U
#define ELEM_BASE      0x0b00U
/* The most of an element's length that is backed by fake memory: a row
 * may post a longer one, as a driver posts whole pages, but none fills
 * more than this. */
#define ELEM_CAP 1536U
#define ELEM_GAP 8U    /* between two element buffers */
#define BUF_FILL 0xeeU /* what the buffers hold before a frame arrives */
#define RXQ_ID   1U    /* the queue the rows use, and the only one enabled */

/* A completion ring entry of either size */
#define COMP_SIZE     16U
#define HDS_COMP_SIZE 32U

_Static_assert(RXQ_CQ >= RXQ_RING + (RXQ_DEPTH * 16U) &&
                   RXQ_SGL >= RXQ_CQ + (RXQ_DEPTH * HDS_COMP_SIZE) &&
                   BUF0 >= RXQ_SGL + (RXQ_DEPTH * 128U) &&
                   ELEM_BASE >= BUF0 + BUF0_CAP,
               "the rings and the first buffer do not overlap");

/* The values of ionic_if.h and of the prototype interface, written here
 * independently of the emulator's copies. */
#define OP_SIMPLE     0U
#define OP_SG         1U
#define QINIT_F_IRQ   0x01U
#define QINIT_F_SG    0x04U
#define Q_F_2X_CQ     UINT64_C(0x20)
#define RXQ_F_HWSTAMP UINT64_C(0x10000)
#define RXQ_F_HDS     UINT64_C(0x20000)
#define FEATURES_HDS  UINT64_C(0x20020)
#define HDS_F_SPLIT   0x01U
#define HDS_F_HBO     0x02U
#define CSUM_TCP_OK   0x01U
#define CSUM_TCP_BAD  0x02U
#define CSUM_UDP_OK   0x04U
#define CSUM_UDP_BAD  0x08U
#define CSUM_IP_OK    0x10U
#define CSUM_IP_BAD   0x20U
#define CSUM_CALC     0x80U
#define HW_RX_CSUM    UINT64_C(0x10)
/* TX_SG, TX_CSUM, TSO, TSO_IPV6 and TSO_ECN: what the device grants with
 * header split off */
#define HW_GRANTED_OFF UINT64_C(0x7a0)
#define HW_GRANTED_ON  UINT64_C(0x7b0)

/* How the device and the queue under test are set up. */
enum rx_mode {
    MODE_OFF, /* [rx] header_split = off, the default */
    MODE_ON,  /* on, and a queue that did not ask for header split */
    MODE_HDS, /* on, and a queue that did */
};

struct rx_setup {
    enum rx_mode mode;
    /* 0 for each gives the ring where the rows' SG lists and completions
     * are, RXQ_SGL and RXQ_CQ. */
    uint64_t sg_ring;
    uint64_t cq_ring;
};

/* What a Q_INIT says of a queue, beyond its type and index. */
struct q_init_args {
    uint16_t flags;
    uint64_t sg_ring;
    uint64_t cq_ring;
    uint64_t features;
};

/* Whether @a and @b are both true or both false. */
static bool same(bool a, bool b)
{
    return (a && b) || (!a && !b);
}

/* Q_INIT of queue @index of @qtype; returns the completion's status. */
static uint8_t q_init(struct ionic_eth_emu *emu, uint8_t qtype, uint32_t index,
                      const struct q_init_args *a)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    /* struct ionic_q_init_cmd, as ionic_lif_rxq_init() fills it in, with
     * the features a header-split driver adds. */
    cmd[0] = IONIC_CMD_Q_INIT;
    cmd[4] = qtype;
    ernic_put_le32(cmd + 8, index);
    ernic_put_le16(cmd + 14, 3);
    ernic_put_le16(cmd + 16, a->flags);
    cmd[19] = RXQ_DEPTH_LOG2;
    ernic_put_le64(cmd + 20, RXQ_RING);
    ernic_put_le64(cmd + 28, a->cq_ring);
    ernic_put_le64(cmd + 36, a->sg_ring);
    ernic_put_le64(cmd + 56, a->features);
    ionic_eth_emu_handle_q_init(emu, cmd, comp);
    return comp[0];
}

static void q_enable(struct ionic_eth_emu *emu, uint8_t qtype, uint32_t index)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    cmd[0] = IONIC_CMD_Q_CONTROL;
    cmd[1] = qtype;
    ernic_put_le32(cmd + 4, index);
    cmd[8] = IONIC_Q_ENABLE;
    ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);
}

/* A fresh device with header split on or off, and empty guest memory. */
static struct ionic_eth_emu *new_emu(bool header_split)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;
    char err[256];

    if (!emu) {
        fprintf(stderr, "ionic_eth_emu_create failed\n");
        abort();
    }
    ionic_profile_defaults(&p);
    /* The way a server is given the mode: --nic-profile header-split. */
    if (header_split &&
        ionic_profile_apply(&p, "header-split", err, sizeof(err)) != 0) {
        fprintf(stderr, "the header-split profile: %s\n", err);
        abort();
    }
    ionic_eth_emu_set_profile(emu, &p);
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_mem + BUF0, BUF_FILL, sizeof(g_mem) - BUF0);
    return emu;
}

/* The same with Rx queue RXQ_ID initialized as @s says, and enabled. */
static struct ionic_eth_emu *rx_emu(const struct rx_setup *s)
{
    struct ionic_eth_emu *emu = new_emu(s->mode != MODE_OFF);
    const struct q_init_args args = {
        .flags = QINIT_F_IRQ | QINIT_F_SG,
        .sg_ring = s->sg_ring ? s->sg_ring : RXQ_SGL,
        .cq_ring = s->cq_ring ? s->cq_ring : RXQ_CQ,
        .features = s->mode == MODE_HDS ? FEATURES_HDS : 0,
    };
    uint8_t status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID, &args);

    if (status != IONIC_RC_SUCCESS) {
        fprintf(stderr, "Q_INIT of the Rx queue failed: %u\n", status);
        abort();
    }
    q_enable(emu, IONIC_QTYPE_RXQ, RXQ_ID);
    return emu;
}

/* The buffers one descriptor posts. */
struct rx_post {
    uint8_t opcode;
    uint16_t len;                  /* of the descriptor's own buffer, at BUF0 */
    uint16_t sg[RXQ_SG_MAX_ELEMS]; /* each element's; 0 ends the list */
};

/* Where post() put each SG element's buffer */
static size_t elem_addr[RXQ_SG_MAX_ELEMS];

/*
 * Write @p at @index of the Rx ring as ionic_rx_fill() does: the first
 * buffer in the descriptor, the others in slot @index of the SG ring, and
 * tell the device of it.
 */
static void post(struct ionic_eth_emu *emu, uint16_t index,
                 const struct rx_post *p)
{
    uint8_t *desc = g_mem + RXQ_RING + ((size_t)index * 16);
    uint8_t *sg = g_mem + RXQ_SGL + ((size_t)index * 128);
    size_t next = ELEM_BASE;

    /* struct ionic_rxq_desc */
    memset(desc, 0, 16);
    desc[0] = p->opcode;
    ernic_put_le16(desc + 6, p->len);
    ernic_put_le64(desc + 8, BUF0);

    /* struct ionic_rxq_sg_desc: the element after the last is zero. */
    memset(sg, 0, 128);
    memset(elem_addr, 0, sizeof(elem_addr));
    for (size_t i = 0; i < RXQ_SG_MAX_ELEMS; i++) {
        size_t backed = p->sg[i] < ELEM_CAP ? p->sg[i] : ELEM_CAP;

        if (backed + ELEM_GAP > sizeof(g_mem) - next) {
            fprintf(stderr, "out of fake guest memory\n");
            abort();
        }
        elem_addr[i] = next;
        if (p->sg[i] != 0) {
            ernic_put_le64(sg + (i * 16), next);
            ernic_put_le16(sg + (i * 16) + 8, p->sg[i]);
        }
        next += backed + ELEM_GAP;
    }
    emu->eth_q[IONIC_QTYPE_RXQ][RXQ_ID].prod =
        (uint16_t)((index + 1U) % RXQ_DEPTH);
}

/* The frame the fake network backend offers next.  Static, so the pointer
 * left in g_net_rx_frame never refers to a caller's stack. */
static uint8_t tap_buf[1600];

/* Hand @frame to the device as a frame from the host network. */
static void tap_frame(struct ionic_eth_emu *emu, const uint8_t *frame,
                      size_t len)
{
    static int fake_net;

    if (len > sizeof(tap_buf)) {
        fprintf(stderr, "a %zu-byte frame is too long for the test\n", len);
        abort();
    }
    memcpy(tap_buf, frame, len);
    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_rx_frame = tap_buf;
    g_net_rx_len = len;
    ionic_eth_emu_poll_rx(emu);
    emu->net = NULL;
}

static uint64_t lif_stat(const struct ionic_eth_emu *emu, enum ionic_lif_stat s)
{
    return emu->stats.lif[s];
}

/* How many frames the LIF counts as received. */
static uint64_t rx_packets(const struct ionic_eth_emu *emu)
{
    return lif_stat(emu, IONIC_LIF_RX_UCAST_PACKETS) +
           lif_stat(emu, IONIC_LIF_RX_MCAST_PACKETS) +
           lif_stat(emu, IONIC_LIF_RX_BCAST_PACKETS);
}

static uint16_t rx_head(const struct ionic_eth_emu *emu)
{
    return emu->eth_q[IONIC_QTYPE_RXQ][RXQ_ID].head;
}

/* What a row expects of the device. */
struct rx_want {
    /* Dropped: nothing written, and this LIF counter moved.  0 for a frame
     * that is delivered. */
    enum ionic_lif_stat drop;
    uint16_t head;      /* frame bytes in the descriptor's own buffer */
    uint8_t num_sg;     /* SG elements the rest fills, each to its length */
    uint8_t hds_flags;  /* on a header-split queue */
    uint16_t csum;      /* with header split on */
    uint8_t csum_flags; /* the same */
};

static void dump(const char *label, const uint8_t *p, size_t n)
{
    printf("     %s:", label);
    for (size_t i = 0; i < n; i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

/*
 * The completion ring entry for a frame of @len bytes that used descriptor
 * @index, written out from the row: struct ionic_rxq_comp, behind the
 * 16-byte extension on a header-split queue.  Returns its size.
 */
static size_t want_entry(uint8_t entry[HDS_COMP_SIZE], enum rx_mode mode,
                         size_t len, uint16_t index, bool color,
                         const struct rx_want *w)
{
    uint8_t *comp = entry;

    memset(entry, 0, HDS_COMP_SIZE);
    if (mode == MODE_HDS) {
        ernic_put_le16(entry, w->head);
        entry[2] = w->hds_flags;
        comp += 16;
    }
    comp[1] = w->num_sg;
    ernic_put_le16(comp + 2, index);
    if (mode != MODE_OFF) {
        comp[8] = (uint8_t)(w->csum >> 8);
        comp[9] = (uint8_t)(w->csum & 0xffU);
        comp[14] = w->csum_flags;
    }
    ernic_put_le16(comp + 12, (uint16_t)len);
    comp[15] = color ? 0x80 : 0;
    return mode == MODE_HDS ? HDS_COMP_SIZE : COMP_SIZE;
}

/*
 * Deliver @frame to a queue set up as @setup with @p posted at descriptor
 * 0, and check all of guest memory against @w: the frame's first @w->head
 * bytes in the descriptor's buffer, the rest in the first @w->num_sg SG
 * elements, one completion, and every other byte as it was.
 */
static bool rx_frame_is(const struct rx_setup *setup, const struct rx_post *p,
                        const uint8_t *frame, size_t len,
                        const struct rx_want *w)
{
    static uint8_t want_mem[GUEST_MEM_SIZE];
    struct ionic_eth_emu *emu = rx_emu(setup);
    bool ok;

    post(emu, 0, p);
    memcpy(want_mem, g_mem, sizeof(want_mem));
    if (!w->drop) {
        size_t off = w->head;
        uint8_t entry[HDS_COMP_SIZE];
        size_t entry_size = want_entry(entry, setup->mode, len, 0, true, w);

        memcpy(want_mem + BUF0, frame, w->head);
        for (uint8_t i = 0; i < w->num_sg; i++) {
            size_t n = len - off < p->sg[i] ? len - off : p->sg[i];

            memcpy(want_mem + elem_addr[i], frame + off, n);
            off += n;
        }
        if (off != len || w->head > p->len) {
            fprintf(stderr, "a row whose buffers do not add up to its frame\n");
            abort();
        }
        memcpy(want_mem + RXQ_CQ, entry, entry_size);
    }

    tap_frame(emu, frame, len);
    ok = memcmp(g_mem, want_mem, sizeof(want_mem)) == 0;
    if (!ok) {
        dump("completion", g_mem + RXQ_CQ, HDS_COMP_SIZE);
        dump("expected  ", want_mem + RXQ_CQ, HDS_COMP_SIZE);
        for (size_t i = 0; i < sizeof(want_mem); i++) {
            if (g_mem[i] != want_mem[i]) {
                printf("     guest memory first differs at %#zx: %02x, "
                       "expected %02x\n",
                       i, g_mem[i], want_mem[i]);
                break;
            }
        }
    }
    if (w->drop) {
        ok = ok && rx_head(emu) == 0 && rx_packets(emu) == 0 &&
             lif_stat(emu, w->drop) == 1;
    } else {
        ok = ok && rx_head(emu) == 1 && rx_packets(emu) == 1;
    }
    ionic_eth_emu_destroy(emu);
    return ok;
}

/* ---- What each kind of frame reports ------------------------------------- */

/* The buffers a header-split driver posts: 256 bytes for the headers and a
 * page for the payload. */
static const struct rx_post std_post = {
    .opcode = OP_SG, .len = 256, .sg = {4096}};

/*
 * One frame on a header-split queue with std_post: where the device splits
 * it, if it does, and the checksum status it reports.
 */
struct kind_case {
    const char *category;
    const char *description;
    const char *expected;
    const uint8_t *frame;
    size_t len;
    struct rx_want want;
};

#define FRAME(name) erx_##name, sizeof(erx_##name)
#define TCP4_OK     (CSUM_CALC | CSUM_IP_OK | CSUM_TCP_OK)
#define TCP6_OK     (CSUM_CALC | CSUM_TCP_OK)

static const struct kind_case kind_cases[] = {
    /* P1 and P2: frames that are split */
    {"positive",
     "TCP/IPv4, no options, 1448 payload bytes (case 1)",
     "split at 54; CALC, IP_OK, TCP_OK",
     FRAME(tcp4_1448),
     {0, 54, 1, HDS_F_SPLIT, ERX_TCP4_1448_CSUM, TCP4_OK}},
    {"positive",
     "TCP/IPv4, 12 bytes of TCP options (case 2, example A)",
     "split at 66",
     FRAME(a),
     {0, 66, 1, HDS_F_SPLIT, ERX_A_CSUM, TCP4_OK}},
    {"positive",
     "the same behind one 802.1Q tag (case 3)",
     "split at 70",
     FRAME(tcp4_vlan),
     {0, 70, 1, HDS_F_SPLIT, ERX_TCP4_VLAN_CSUM, TCP4_OK}},
    {"boundary",
     "the same behind two tags, the most looked past (case 3)",
     "split at 74",
     FRAME(tcp4_qinq),
     {0, 74, 1, HDS_F_SPLIT, ERX_TCP4_QINQ_CSUM, TCP4_OK}},
    {"positive",
     "TCP/IPv4 with a 4-byte IP option (case 4)",
     "split at 58",
     FRAME(tcp4_ihl6),
     {0, 58, 1, HDS_F_SPLIT, ERX_TCP4_IHL6_CSUM, TCP4_OK}},
    {"boundary",
     "TCP/IPv4 with the longest IP header, 60 bytes (case 4)",
     "split at 106",
     FRAME(tcp4_ihl15),
     {0, 106, 1, HDS_F_SPLIT, ERX_TCP4_IHL15_CSUM, TCP4_OK}},
    {"positive",
     "TCP/IPv6 with no extension header (case 5)",
     "split at 74; CALC and TCP_OK, no IP flag",
     FRAME(tcp6),
     {0, 74, 1, HDS_F_SPLIT, ERX_TCP6_CSUM, TCP6_OK}},
    {"positive",
     "TCP/IPv6 with 12 bytes of TCP options (case 5)",
     "split at 86",
     FRAME(tcp6_opts),
     {0, 86, 1, HDS_F_SPLIT, ERX_TCP6_OPTS_CSUM, TCP6_OK}},
    {"positive",
     "TCP/IPv6 behind one 802.1Q tag (case 5)",
     "split at 78",
     FRAME(tcp6_vlan),
     {0, 78, 1, HDS_F_SPLIT, ERX_TCP6_VLAN_CSUM, TCP6_OK}},
    {"boundary",
     "a 74-byte SYN: headers and no payload (case 6)",
     "SPLIT with all 74 bytes as headers and no SG element",
     FRAME(syn4),
     {0, 74, 0, HDS_F_SPLIT, ERX_SYN4_CSUM, TCP4_OK}},
    {"boundary",
     "a 54-byte ACK that was not padded (case 6)",
     "SPLIT with all 54 bytes as headers and no SG element",
     FRAME(ack4),
     {0, 54, 0, HDS_F_SPLIT, ERX_ACK4_CSUM, TCP4_OK}},

    /* P1.3: padding */
    {"negative",
     "the same ACK padded to 60 bytes (case 7)",
     "not split: 60 bytes in the first buffer; TCP_OK all the same",
     FRAME(ack4_pad),
     {0, 60, 0, 0, ERX_ACK4_PAD_CSUM, TCP4_OK}},
    {"negative",
     "one payload byte padded to 60 (case 7, example B)",
     "not split; TCP_OK, checked over the 21 bytes the IP header names",
     FRAME(b),
     {0, 60, 0, 0, ERX_B_CSUM, TCP4_OK}},

    /* P1.2: what is not looked into */
    {"negative",
     "TCP/IPv6 behind a hop-by-hop header (case 8)",
     "not split; CALC alone",
     FRAME(tcp6_hbh),
     {0, 98, 0, 0, ERX_TCP6_HBH_CSUM, CSUM_CALC}},
    {"negative",
     "TCP/IPv6 behind a fragment header (case 8)",
     "not split; CALC alone",
     FRAME(tcp6_frag),
     {0, 98, 0, 0, ERX_TCP6_FRAG_CSUM, CSUM_CALC}},
    {"negative",
     "the first fragment of a TCP/IPv4 datagram (case 9)",
     "not split; CALC and IP_OK, no TCP flag",
     FRAME(frag4_first),
     {0, 70, 0, 0, ERX_FRAG4_FIRST_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "a later fragment of one (case 9)",
     "not split; CALC and IP_OK",
     FRAME(frag4_later),
     {0, 66, 0, 0, ERX_FRAG4_LATER_CSUM, CSUM_CALC | CSUM_IP_OK}},

    /* Case 10: not TCP */
    {"negative",
     "UDP/IPv4 (case 10)",
     "not split; CALC, IP_OK and UDP_OK",
     FRAME(udp4),
     {0, 60, 0, 0, ERX_UDP4_CSUM, CSUM_CALC | CSUM_IP_OK | CSUM_UDP_OK}},
    {"negative",
     "UDP/IPv4, 1200 payload bytes (case 10)",
     "not split: 256 bytes in the first buffer and the rest in an element",
     FRAME(udp4_1200),
     {0, 256, 1, 0, ERX_UDP4_1200_CSUM, CSUM_CALC | CSUM_IP_OK | CSUM_UDP_OK}},
    {"boundary",
     "UDP/IPv4 with no checksum",
     "neither UDP flag",
     FRAME(udp4_nocsum),
     {0, 60, 0, 0, ERX_UDP4_NOCSUM_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "UDP/IPv4 with a wrong checksum",
     "UDP_BAD",
     FRAME(udp4_bad),
     {0, 60, 0, 0, ERX_UDP4_BAD_CSUM, CSUM_CALC | CSUM_IP_OK | CSUM_UDP_BAD}},
    {"boundary",
     "UDP/IPv4 padded to 60 bytes",
     "UDP_OK, checked over the 12 bytes the IP header names",
     FRAME(udp4_pad),
     {0, 60, 0, 0, ERX_UDP4_PAD_CSUM, CSUM_CALC | CSUM_IP_OK | CSUM_UDP_OK}},
    {"negative",
     "UDP whose own length is not the IP header's",
     "neither UDP flag",
     FRAME(udp4_lenmis),
     {0, 60, 0, 0, ERX_UDP4_LENMIS_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "IPv4 naming UDP with 6 bytes behind it, then padding",
     "neither UDP flag: that is no UDP header, whatever follows it",
     FRAME(udp4_short),
     {0, 60, 0, 0, ERX_UDP4_SHORT_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"positive",
     "UDP/IPv6",
     "CALC and UDP_OK",
     FRAME(udp6),
     {0, 80, 0, 0, ERX_UDP6_CSUM, CSUM_CALC | CSUM_UDP_OK}},
    {"negative",
     "UDP/IPv6 with a wrong checksum",
     "CALC and UDP_BAD",
     FRAME(udp6_bad),
     {0, 80, 0, 0, ERX_UDP6_BAD_CSUM, CSUM_CALC | CSUM_UDP_BAD}},
    {"boundary",
     "UDP/IPv6 with a zero checksum",
     "neither UDP flag",
     FRAME(udp6_nocsum),
     {0, 80, 0, 0, ERX_UDP6_NOCSUM_CSUM, CSUM_CALC}},
    {"negative",
     "an ICMP echo request (case 10)",
     "CALC and IP_OK",
     FRAME(icmp4),
     {0, 58, 0, 0, ERX_ICMP4_CSUM, CSUM_CALC | CSUM_IP_OK}},

    /* Case 11: not IP */
    {"negative",
     "an ARP request (case 11)",
     "CALC alone",
     FRAME(arp),
     {0, 60, 0, 0, ERX_ARP_CSUM, CSUM_CALC}},
    {"boundary",
     "another ethertype, an odd number of bytes (case 11)",
     "CALC, the last byte summed as the high half of a word",
     FRAME(other61),
     {0, 61, 0, 0, ERX_OTHER61_CSUM, CSUM_CALC}},
    {"negative",
     "TCP/IPv4 behind three VLAN tags (case 11)",
     "not split; CALC alone",
     FRAME(tcp4_vlan3),
     {0, 94, 0, 0, ERX_TCP4_VLAN3_CSUM, CSUM_CALC}},

    /* Case 12: runts and truncated frames */
    {"boundary",
     "a 13-byte frame, less than an Ethernet header (case 12)",
     "delivered; no CALC and a zero sum",
     FRAME(runt13),
     {0, 13, 0, 0, 0, 0}},
    {"boundary",
     "a 14-byte frame (case 12)",
     "CALC and a zero sum",
     FRAME(eth14),
     {0, 14, 0, 0, ERX_ETH14_CSUM, CSUM_CALC}},
    {"negative",
     "half a VLAN tag (case 12)",
     "CALC alone",
     FRAME(vlan_trunc),
     {0, 16, 0, 0, ERX_VLAN_TRUNC_CSUM, CSUM_CALC}},
    {"negative",
     "19 bytes of an IPv4 header (case 12)",
     "CALC alone",
     FRAME(ip4_short),
     {0, 33, 0, 0, ERX_IP4_SHORT_CSUM, CSUM_CALC}},
    {"negative",
     "an IPv4 header length of 16",
     "CALC alone",
     FRAME(ip4_ihl4),
     {0, 70, 0, 0, ERX_IP4_IHL4_CSUM, CSUM_CALC}},
    {"negative",
     "an IPv4 header length past the frame's end",
     "CALC alone",
     FRAME(ip4_ihl_over),
     {0, 70, 0, 0, ERX_IP4_IHL_OVER_CSUM, CSUM_CALC}},
    {"negative",
     "the IPv4 ethertype over version 6",
     "CALC alone",
     FRAME(ip4_ver6),
     {0, 70, 0, 0, ERX_IP4_VER6_CSUM, CSUM_CALC}},
    {"negative",
     "the IPv6 ethertype over version 4",
     "CALC alone",
     FRAME(ip6_ver4),
     {0, 90, 0, 0, ERX_IP6_VER4_CSUM, CSUM_CALC}},
    {"negative",
     "39 bytes of an IPv6 header (case 12)",
     "CALC alone",
     FRAME(ip6_short),
     {0, 53, 0, 0, ERX_IP6_SHORT_CSUM, CSUM_CALC}},
    {"negative",
     "an IPv6 payload length one past the frame's end (case 12)",
     "CALC alone",
     FRAME(ip6_plen_over),
     {0, 90, 0, 0, ERX_IP6_PLEN_OVER_CSUM, CSUM_CALC}},
    {"negative",
     "TCP/IPv4 cut 12 bytes short of its total length (case 12)",
     "not split; CALC and IP_OK, the TCP checksum not judged",
     FRAME(ip4_trunc),
     {0, 70, 0, 0, ERX_IP4_TRUNC_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "an IPv4 total length less than its header",
     "CALC and IP_OK",
     FRAME(ip4_total_under),
     {0, 70, 0, 0, ERX_IP4_TOTAL_UNDER_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "IPv4 naming TCP with 12 bytes behind it (case 12)",
     "CALC and IP_OK",
     FRAME(tcp4_short),
     {0, 46, 0, 0, ERX_TCP4_SHORT_CSUM, CSUM_CALC | CSUM_IP_OK}},
    {"negative",
     "a TCP header length of 16 under a good checksum (case 12)",
     "TCP_OK, but not split",
     FRAME(tcp4_doff4),
     {0, 70, 0, 0, ERX_TCP4_DOFF4_CSUM, TCP4_OK}},
    {"negative",
     "a TCP header length past the segment's end (case 12)",
     "TCP_OK, but not split",
     FRAME(tcp4_doff_over),
     {0, 70, 0, 0, ERX_TCP4_DOFF_OVER_CSUM, TCP4_OK}},

    /* Case 13: wrong checksums */
    {"negative",
     "a wrong IPv4 header checksum (case 13)",
     "not split; CALC and IP_BAD, the TCP checksum not judged",
     FRAME(ip4_badcsum),
     {0, 82, 0, 0, ERX_IP4_BADCSUM_CSUM, CSUM_CALC | CSUM_IP_BAD}},
    {"negative",
     "a wrong TCP checksum over IPv4 (case 13)",
     "not split; CALC, IP_OK and TCP_BAD",
     FRAME(tcp4_badcsum),
     {0, 82, 0, 0, ERX_TCP4_BADCSUM_CSUM,
      CSUM_CALC | CSUM_IP_OK | CSUM_TCP_BAD}},
    {"negative",
     "a wrong TCP checksum under 1448 payload bytes (case 13)",
     "not split: 256 bytes in the first buffer and the rest in an element",
     FRAME(tcp4_1448_bad),
     {0, 256, 1, 0, ERX_TCP4_1448_BAD_CSUM,
      CSUM_CALC | CSUM_IP_OK | CSUM_TCP_BAD}},
    {"negative",
     "a wrong TCP checksum over IPv6 (case 13)",
     "not split; CALC and TCP_BAD",
     FRAME(tcp6_badcsum),
     {0, 102, 0, 0, ERX_TCP6_BADCSUM_CSUM, CSUM_CALC | CSUM_TCP_BAD}},
};

/*
 * Each kind of frame three times: on a header-split queue, where the row
 * says what happens; on a queue of the same device that did not ask for
 * header split, which gets the same checksum status and the frame laid
 * out as ever (M4); and with header split off, where the completion says
 * nothing about checksums (M2, K1).
 */
static void run_kind_case(const struct kind_case *c)
{
    const struct rx_setup hds = {.mode = MODE_HDS};
    const struct rx_setup on = {.mode = MODE_ON};
    const struct rx_setup off = {.mode = MODE_OFF};
    /* Without header split a frame fills the first buffer, then the
     * element: std_post's 256 bytes and one page. */
    struct rx_want plain = c->want;
    char what[200];

    plain.head = (uint16_t)(c->len < std_post.len ? c->len : std_post.len);
    plain.num_sg = c->len > std_post.len ? 1 : 0;
    plain.hds_flags = 0;

    report(rx_frame_is(&hds, &std_post, c->frame, c->len, &c->want),
           c->category, c->description, c->expected);

    (void)snprintf(what, sizeof(what), "%s, on a queue without header split",
                   c->description);
    report(rx_frame_is(&on, &std_post, c->frame, c->len, &plain), "regression",
           what,
           "the first buffer filled first, a 16-byte completion, the same "
           "checksum status");

    (void)snprintf(what, sizeof(what), "%s, with header split off",
                   c->description);
    report(rx_frame_is(&off, &std_post, c->frame, c->len, &plain), "regression",
           what,
           "the first buffer filled first, a 16-byte completion, csum and "
           "csum_flags zero");
}

/* ---- Where a frame goes, by the buffers posted ----------------------------
 */

struct place_case {
    const char *category;
    const char *description;
    const char *expected;
    const uint8_t *frame;
    size_t len;
    enum rx_mode mode;
    struct rx_post post;
    struct rx_want want;
};

#define A_CSUM   ERX_A_CSUM, TCP4_OK
#define BIG_CSUM ERX_TCP4_1448_CSUM, TCP4_OK

static const struct place_case place_cases[] = {
    /* P2 */
    {"boundary",
     "66 bytes of headers and a first buffer of 66",
     "split: the headers fit exactly",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 66, {4096}},
     {0, 66, 1, HDS_F_SPLIT, A_CSUM}},
    {"boundary",
     "16 payload bytes and one element of 16",
     "split: the payload fits exactly",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 256, {16}},
     {0, 66, 1, HDS_F_SPLIT, A_CSUM}},
    {"positive",
     "16 payload bytes and two elements of 8",
     "split: 8 bytes in each element",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 70, {8, 8}},
     {0, 66, 2, HDS_F_SPLIT, A_CSUM}},
    {"positive",
     "1448 payload bytes and two elements of 1000",
     "split: 54 bytes, then 1000, then 448",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 256, {1000, 1000}},
     {0, 54, 2, HDS_F_SPLIT, BIG_CSUM}},
    {"boundary",
     "1448 payload bytes and eight elements of 181",
     "split: every element full",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 256, {181, 181, 181, 181, 181, 181, 181, 181}},
     {0, 54, 8, HDS_F_SPLIT, BIG_CSUM}},

    /* P3, case 14: the headers do not fit the first buffer */
    {"negative",
     "66 bytes of headers and a first buffer of 64 (case 14)",
     "HBO: 64 bytes in the first buffer, 18 in an element",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 64, {4096}},
     {0, 64, 1, HDS_F_HBO, A_CSUM}},
    {"boundary",
     "66 bytes of headers and a first buffer of 65",
     "HBO: 65 bytes in the first buffer, 17 in an element",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 65, {4096}},
     {0, 65, 1, HDS_F_HBO, A_CSUM}},
    {"negative",
     "the same with a 10-byte element, 7 short of the frame",
     "dropped: nothing written",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 65, {10}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},

    /* P3, case 15: the payload does not fit the elements */
    {"boundary",
     "16 payload bytes and one element of 15 (case 15)",
     "not split, no flag: the whole frame in the first buffer",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 256, {15}},
     {0, 82, 0, 0, A_CSUM}},
    {"negative",
     "the same with no element at all",
     "not split, no flag: the whole frame in the first buffer",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 256, {0}},
     {0, 82, 0, 0, A_CSUM}},
    {"negative",
     "an element of 8, an empty one, and a page (case 15)",
     "not split: the empty element ends the list",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 256, {8, 0, 4096}},
     {0, 82, 0, 0, A_CSUM}},
    {"negative",
     "a first buffer of 70 and an element of 14 (case 15)",
     "not split, no flag: 70 bytes, then 12",
     FRAME(a),
     MODE_HDS,
     {OP_SG, 70, {14}},
     {0, 70, 1, 0, A_CSUM}},
    {"negative",
     "1448 payload bytes and eight elements of 180 (case 15)",
     "not split, no flag: 256 bytes, six full elements, then 166",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 256, {180, 180, 180, 180, 180, 180, 180, 180}},
     {0, 256, 7, 0, BIG_CSUM}},

    {"negative",
     "a TCP header length past the segment's end, 64 and a page",
     "no HBO: it is not a frame that would be split",
     FRAME(tcp4_doff_over),
     MODE_HDS,
     {OP_SG, 64, {4096}},
     {0, 64, 1, 0, ERX_TCP4_DOFF_OVER_CSUM, TCP4_OK}},

    /* Case 16: a descriptor with no SG list */
    {"negative",
     "a SIMPLE descriptor on a header-split queue (case 16)",
     "not split, no flag: the whole frame in its buffer",
     FRAME(a),
     MODE_HDS,
     {OP_SIMPLE, 256, {4096}},
     {0, 82, 0, 0, A_CSUM}},
    {"negative",
     "a SIMPLE descriptor of 64 bytes, less than the frame (P5)",
     "dropped: nothing written, though its SG list has room",
     FRAME(a),
     MODE_HDS,
     {OP_SIMPLE, 64, {4096}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},
    {"negative",
     "a descriptor with opcode 2",
     "dropped: nothing written",
     FRAME(a),
     MODE_HDS,
     {2, 256, {4096}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},

    /* Case 17: no room */
    {"negative",
     "1502 bytes for 256, 1000, an empty element and a page",
     "dropped: nothing written (case 17)",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 256, {1000, 0, 4096}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},
    {"negative",
     "1502 bytes for 53 and 1448: the headers one byte short",
     "dropped, not split with a byte left over",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 53, {1448}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},
    {"boundary",
     "1502 bytes for 53 and 1449",
     "HBO: 53 bytes in the first buffer and 1449 in the element",
     FRAME(tcp4_1448),
     MODE_HDS,
     {OP_SG, 53, {1449}},
     {0, 53, 1, HDS_F_HBO, BIG_CSUM}},

    /* The same buffers on queues without header split: M2 and M4 */
    {"regression",
     "frame A for 64 and a page, header split not asked for",
     "64 bytes, then 18, as ever; the checksum status reported",
     FRAME(a),
     MODE_ON,
     {OP_SG, 64, {4096}},
     {0, 64, 1, 0, A_CSUM}},
    {"regression",
     "the same with header split off",
     "64 bytes, then 18; csum and csum_flags zero",
     FRAME(a),
     MODE_OFF,
     {OP_SG, 64, {4096}},
     {0, 64, 1, 0, 0, 0}},
    {"regression",
     "1502 bytes for 256 and two of 1000, not asked for",
     "256, 1000, then 246",
     FRAME(tcp4_1448),
     MODE_ON,
     {OP_SG, 256, {1000, 1000}},
     {0, 256, 2, 0, BIG_CSUM}},
    {"regression",
     "the same with header split off",
     "256, 1000, then 246",
     FRAME(tcp4_1448),
     MODE_OFF,
     {OP_SG, 256, {1000, 1000}},
     {0, 256, 2, 0, 0, 0}},
    {"regression",
     "1502 bytes for 256 and 1000, header split off",
     "dropped: nothing written",
     FRAME(tcp4_1448),
     MODE_OFF,
     {OP_SG, 256, {1000}},
     {.drop = IONIC_LIF_RX_DESC_DATA_ERROR}},
};

static void run_place_case(const struct place_case *c)
{
    const struct rx_setup setup = {.mode = c->mode};

    report(rx_frame_is(&setup, &c->post, c->frame, c->len, &c->want),
           c->category, c->description, c->expected);
}

/* ---- The worked examples of the interface ------------------------------- */

/*
 * rx-interface.md's examples, byte for byte: descriptors 5 and 6 of a
 * header-split queue take frame A, which is split, and frame B, which is
 * padded and is not; then frame A on a queue without header split, with
 * the mode on and off.
 */
static void check_examples(void)
{
    static const uint8_t a_hds[32] = {
        0x42, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xeb, 0xc6, 0x00, 0x00, 0x52, 0x00, 0x91, 0x80,
    };
    static const uint8_t b_hds[32] = {
        0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xeb, 0xe1, 0x00, 0x00, 0x3c, 0x00, 0x91, 0x80,
    };
    static const uint8_t a_on[16] = {
        0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xeb, 0xc6, 0x00, 0x00, 0x52, 0x00, 0x91, 0x80,
    };
    static const uint8_t a_off[16] = {
        0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x80,
    };
    static const uint8_t zero[32];
    static const struct {
        enum rx_mode mode;
        const uint8_t *a_entry;
        size_t entry_size;
        size_t a_head; /* of frame A's 82 bytes, those in the first buffer */
        const char *description;
    } rows[] = {
        {MODE_HDS, a_hds, 32, 66, "example A on a header-split queue"},
        {MODE_ON, a_on, 16, 82, "example A on a queue without header split"},
        {MODE_OFF, a_off, 16, 82, "example A with header split off"},
    };
    const struct rx_post simple = {.opcode = OP_SIMPLE, .len = 256};

    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        const struct rx_setup setup = {.mode = rows[r].mode};
        struct ionic_eth_emu *emu = rx_emu(&setup);
        const uint8_t *cq = g_mem + RXQ_CQ;
        size_t size = rows[r].entry_size;
        bool ok;

        /* Five frames first, so that the next descriptor is index 5. */
        for (uint16_t i = 0; i < 5; i++) {
            post(emu, i, &simple);
            tap_frame(emu, erx_arp, sizeof(erx_arp));
        }
        memset(g_mem + BUF0, BUF_FILL, sizeof(g_mem) - BUF0);

        post(emu, 5, &std_post);
        tap_frame(emu, erx_a, sizeof(erx_a));
        ok = rx_head(emu) == 6 &&
             memcmp(cq + (5 * size), rows[r].a_entry, size) == 0 &&
             memcmp(cq + (6 * size), zero, size) == 0 &&
             memcmp(g_mem + BUF0, erx_a, rows[r].a_head) == 0 &&
             g_mem[BUF0 + rows[r].a_head] == BUF_FILL &&
             (rows[r].a_head == sizeof(erx_a)
                  ? g_mem[elem_addr[0]] == BUF_FILL
                  : memcmp(g_mem + elem_addr[0], erx_a + 66, 16) == 0 &&
                        g_mem[elem_addr[0] + 16] == BUF_FILL);
        if (!ok) {
            dump("completion 5", cq + (5 * size), size);
        }
        report(ok, "positive", rows[r].description,
               "the bytes rx-interface.md gives, at entry 5 of the ring");

        if (rows[r].mode != MODE_HDS) {
            ionic_eth_emu_destroy(emu);
            continue;
        }
        memset(g_mem + BUF0, BUF_FILL, sizeof(g_mem) - BUF0);
        post(emu, 6, &std_post);
        tap_frame(emu, erx_b, sizeof(erx_b));
        ok = rx_head(emu) == 7 && memcmp(cq + (6 * size), b_hds, size) == 0 &&
             memcmp(g_mem + BUF0, erx_b, sizeof(erx_b)) == 0 &&
             g_mem[BUF0 + sizeof(erx_b)] == BUF_FILL &&
             g_mem[elem_addr[0]] == BUF_FILL;
        if (!ok) {
            dump("completion 6", cq + (6 * size), size);
        }
        report(ok, "positive", "example B on a header-split queue",
               "the bytes rx-interface.md gives, at entry 6 of the ring");
        ionic_eth_emu_destroy(emu);
    }
}

/*
 * A header-split queue's completion ring has as many entries as its
 * descriptor ring, each 32 bytes: the ninth frame's completion is written
 * over the first, with the colour flipped.  The SG list read is the head
 * descriptor's own, not the first slot's.
 */
static void check_ring_wrap(void)
{
    const struct rx_setup setup = {.mode = MODE_HDS};
    struct ionic_eth_emu *emu = rx_emu(&setup);
    const struct rx_want split = {0, 66, 1, HDS_F_SPLIT, A_CSUM};
    const struct rx_want arp = {0, 60, 0, 0, ERX_ARP_CSUM, CSUM_CALC};
    const struct rx_post no_room = {.opcode = OP_SG, .len = 256, .sg = {8}};
    uint8_t want[HDS_COMP_SIZE];
    bool ok = true;

    for (unsigned int i = 0; i < RXQ_DEPTH + 2U; i++) {
        uint16_t index = (uint16_t)(i % RXQ_DEPTH);
        bool first_lap = i < RXQ_DEPTH;
        /* Frame A for every other descriptor but the first */
        bool a = index != 0 && i % 2 == 0;

        /* Slot 0's SG list has no room for the payload: a device that
         * read it for every descriptor would split none of these. */
        post(emu, 0, &no_room);
        if (index != 0) {
            post(emu, index, &std_post);
        }
        if (a) {
            (void)want_entry(want, MODE_HDS, sizeof(erx_a), index, first_lap,
                             &split);
            tap_frame(emu, erx_a, sizeof(erx_a));
        } else {
            (void)want_entry(want, MODE_HDS, sizeof(erx_arp), index, first_lap,
                             &arp);
            tap_frame(emu, erx_arp, sizeof(erx_arp));
        }
        if (memcmp(g_mem + RXQ_CQ + ((size_t)index * HDS_COMP_SIZE), want,
                   sizeof(want)) != 0) {
            printf("     frame %u, entry %u\n", i, index);
            dump("completion", g_mem + RXQ_CQ + ((size_t)index * HDS_COMP_SIZE),
                 HDS_COMP_SIZE);
            dump("expected  ", want, sizeof(want));
            ok = false;
        }
    }
    /* Nothing was written past the ring's eighth entry. */
    for (size_t i = RXQ_CQ + (RXQ_DEPTH * HDS_COMP_SIZE); i < RXQ_SGL; i++) {
        ok = ok && g_mem[i] == 0;
    }
    report(ok && rx_head(emu) == 2, "boundary",
           "ten frames through an 8-entry header-split queue",
           "32-byte entries 0 to 7, then 0 and 1 again with the colour "
           "clear; each descriptor's own SG list");
    ionic_eth_emu_destroy(emu);
}

/* ---- Frames cut short -----------------------------------------------------
 */

/* The ones-complement sum of @n bytes as big-endian words, the slow way. */
static uint16_t ref_sum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;

    for (size_t i = 0; i < n; i++) {
        sum += (i % 2 == 0) ? (uint32_t)p[i] << 8 : p[i];
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/*
 * The device's frame buffer still holds the last frame behind a shorter
 * one.  After a whole TCP/IPv4 frame behind a VLAN tag, each of its
 * prefixes must be judged by its own bytes: nothing of the IP header is
 * believed until all 20 bytes of it are in the frame, and then only that
 * its checksum is good, since its datagram is not all there.
 */
static void check_stale_bytes(void)
{
    const struct rx_setup setup = {.mode = MODE_HDS};
    struct ionic_eth_emu *emu = rx_emu(&setup);
    uint16_t index = 0;
    bool ok = true;

    post(emu, index, &std_post);
    tap_frame(emu, erx_tcp4_vlan, sizeof(erx_tcp4_vlan));
    ok = ok && g_mem[RXQ_CQ + 2] == HDS_F_SPLIT;

    for (size_t n = sizeof(erx_tcp4_vlan) - 1; n >= 13; n--) {
        struct rx_want w = {.head = (uint16_t)n};
        uint8_t want[HDS_COMP_SIZE];
        const uint8_t *got;

        /* 14 bytes of Ethernet header, a 4-byte tag, a 20-byte IP header */
        if (n >= 38) {
            w.csum_flags = CSUM_CALC | CSUM_IP_OK;
        } else if (n >= 14) {
            w.csum_flags = CSUM_CALC;
        }
        if (n >= 14) {
            w.csum = ref_sum(erx_tcp4_vlan + 14, n - 14);
        }
        index = (uint16_t)((index + 1U) % RXQ_DEPTH);
        post(emu, index, &std_post);
        /* The colour flips each time the ring wraps; it is not the point
         * here, so it is taken from the entry. */
        tap_frame(emu, erx_tcp4_vlan, n);
        got = g_mem + RXQ_CQ + ((size_t)index * HDS_COMP_SIZE);
        (void)want_entry(want, MODE_HDS, n, index, (got[31] & 0x80U) != 0, &w);
        if (memcmp(got, want, sizeof(want)) != 0) {
            printf("     the first %zu bytes\n", n);
            dump("completion", got, HDS_COMP_SIZE);
            dump("expected  ", want, sizeof(want));
            ok = false;
        }
    }
    report(ok, "negative",
           "each prefix of a TCP frame, 85 to 13 bytes, after the whole frame",
           "judged by its own bytes: none split, no IP_OK before the IP "
           "header is whole, never TCP_OK");
    ionic_eth_emu_destroy(emu);
}

/*
 * Every prefix of every frame, each in a buffer of exactly its size, so
 * that under ASan or valgrind a read past the frame is a failure.
 */
static void check_parse_bounds(void)
{
    bool ok = true;

    for (size_t i = 0; i < sizeof(kind_cases) / sizeof(kind_cases[0]); i++) {
        const struct kind_case *c = &kind_cases[i];

        for (size_t n = 0; n <= c->len; n++) {
            uint8_t *buf = malloc(n ? n : 1);
            struct ionic_eth_rx_info info;

            if (!buf) {
                fprintf(stderr, "out of memory\n");
                abort();
            }
            memcpy(buf, c->frame, n);
            memset(&info, 0xa5, sizeof(info));
            ionic_eth_rx_parse(buf, n, &info);
            ok = ok &&
                 (info.csum_flags & CSUM_CALC) == (n < 14 ? 0 : CSUM_CALC) &&
                 info.csum == (n < 14 ? 0 : ref_sum(buf + 14, n - 14)) &&
                 (info.splittable ? (info.csum_flags & CSUM_TCP_OK) != 0 &&
                                        info.hdr_len >= 54 && info.hdr_len <= n
                                  : info.hdr_len == 0);
            /* The whole frame is the row's own. */
            if (n == c->len) {
                ok = ok && info.csum == c->want.csum &&
                     info.csum_flags == c->want.csum_flags &&
                     same(info.splittable,
                          (c->want.hds_flags & HDS_F_SPLIT) != 0);
            }
            free(buf);
        }
    }
    report(ok, "boundary",
           "every prefix of every frame, in a buffer of exactly its size",
           "CALC and the sum from 14 bytes on; split only with TCP_OK and "
           "its headers inside the frame");
}

/* ---- Guest memory the device cannot reach ---------------------------------
 */

static void check_dma_faults(void)
{
    /* An SG ring outside guest memory, which Q_INIT does not look at. */
    const struct rx_setup no_sgl = {.mode = MODE_HDS,
                                    .sg_ring = UINT64_C(0x100000)};
    /* A completion ring with 16 bytes of guest memory behind it: room for
     * one entry of a queue without header split, and half of one with. */
    const struct rx_setup short_cq = {.mode = MODE_HDS,
                                      .cq_ring = GUEST_MEM_SIZE - 16U};
    const struct rx_setup short_cq_plain = {.mode = MODE_ON,
                                            .cq_ring = GUEST_MEM_SIZE - 16U};
    const struct rx_post simple = {.opcode = OP_SIMPLE, .len = 256};
    struct ionic_eth_emu *emu;
    bool ok;

    emu = rx_emu(&no_sgl);
    post(emu, 0, &std_post);
    tap_frame(emu, erx_a, sizeof(erx_a));
    report(rx_head(emu) == 0 && rx_packets(emu) == 0 &&
               lif_stat(emu, IONIC_LIF_RX_DESC_FETCH_ERROR) == 1 &&
               g_mem[BUF0] == BUF_FILL && g_mem[RXQ_CQ + 31] == 0,
           "negative", "a frame to split whose SG list is outside guest memory",
           "dropped as a descriptor fetch error; nothing written");
    ionic_eth_emu_destroy(emu);

    emu = rx_emu(&no_sgl);
    post(emu, 0, &std_post);
    tap_frame(emu, erx_syn4, sizeof(erx_syn4));
    report(rx_head(emu) == 1 && rx_packets(emu) == 1 &&
               ernic_get_le16(g_mem + RXQ_CQ) == sizeof(erx_syn4) &&
               g_mem[RXQ_CQ + 2] == HDS_F_SPLIT &&
               memcmp(g_mem + BUF0, erx_syn4, sizeof(erx_syn4)) == 0,
           "boundary", "a frame of headers alone on the same queue",
           "delivered with SPLIT: a list no payload goes to is not read");
    ionic_eth_emu_destroy(emu);

    emu = rx_emu(&short_cq);
    post(emu, 0, &simple);
    tap_frame(emu, erx_arp, sizeof(erx_arp));
    ok = rx_head(emu) == 0 && rx_packets(emu) == 0 &&
         lif_stat(emu, IONIC_LIF_RX_DMA_ERROR) == 1;
    for (size_t i = GUEST_MEM_SIZE - 16U; i < GUEST_MEM_SIZE; i++) {
        ok = ok && g_mem[i] == BUF_FILL;
    }
    report(ok, "negative",
           "a header-split queue whose 32-byte completion has 16 bytes of "
           "guest memory",
           "not completed, a DMA error: no half of the entry is written");
    ionic_eth_emu_destroy(emu);

    emu = rx_emu(&short_cq_plain);
    post(emu, 0, &simple);
    tap_frame(emu, erx_arp, sizeof(erx_arp));
    report(rx_head(emu) == 1 && rx_packets(emu) == 1 &&
               g_mem[GUEST_MEM_SIZE - 1] == 0x80 &&
               g_mem[GUEST_MEM_SIZE - 2] == CSUM_CALC,
           "regression", "the same ring on a queue without header split",
           "completed: its entry is 16 bytes");
    ionic_eth_emu_destroy(emu);
}

/* ---- Negotiation --------------------------------------------------------- */

/* LIF_SETATTR(FEATURES) of @features over the admin queue, or over the
 * devcmd registers; returns the features the completion grants. */
static uint64_t setattr_features(struct ionic_eth_emu *emu, uint64_t features,
                                 bool devcmd)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

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
        ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);
    }
    return comp[0] == IONIC_RC_SUCCESS ? ernic_get_le64(comp + 4) : UINT64_MAX;
}

/* I2: Rx checksum is granted with header split on, and only then. */
static void check_lif_features(void)
{
    static const struct {
        bool header_split;
        const char *description;
        uint64_t ask;
        uint64_t want;
    } rows[] = {
        {true, "header split on, asking for every bit", UINT64_MAX,
         HW_GRANTED_ON},
        {true, "header split on, asking for Rx checksum alone", HW_RX_CSUM,
         HW_RX_CSUM},
        {true, "header split on, asking for all but Rx checksum", ~HW_RX_CSUM,
         HW_GRANTED_OFF},
        {false, "header split off, asking for every bit", UINT64_MAX,
         HW_GRANTED_OFF},
        {false, "header split off, asking for Rx checksum alone", HW_RX_CSUM,
         0},
    };
    static uint8_t data[DEVCMD_DATA_SIZE];

    for (int devcmd = 0; devcmd < 2; devcmd++) {
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            struct ionic_eth_emu *emu = new_emu(rows[i].header_split);
            uint64_t got = setattr_features(emu, rows[i].ask, devcmd != 0);
            char what[160];

            (void)snprintf(what, sizeof(what),
                           "LIF_SETATTR(FEATURES) over the %s, %s",
                           devcmd ? "devcmd registers" : "admin queue",
                           rows[i].description);
            if (got != rows[i].want) {
                printf("     granted %#" PRIx64 ", expected %#" PRIx64 "\n",
                       got, rows[i].want);
            }
            report(got == rows[i].want,
                   rows[i].header_split ? "positive" : "regression", what,
                   "what was asked for of the mode's offloads");
            ionic_eth_emu_destroy(emu);
        }
    }

    for (int on = 0; on < 2; on++) {
        struct ionic_eth_emu *emu = new_emu(on != 0);
        uint8_t cmd[64] = {IONIC_CMD_LIF_IDENTIFY};
        uint8_t comp[16] = {0};
        uint64_t want = on ? HW_GRANTED_ON : HW_GRANTED_OFF;

        ionic_eth_emu_handle_lif_identify(emu, cmd, comp, data);
        /* union ionic_lif_identity: eth.config.features is the le64 at
         * 0x0a8. */
        report(comp[0] == IONIC_RC_SUCCESS &&
                   ernic_get_le64(data + 0x0a8) == want,
               on ? "positive" : "regression",
               on ? "LIF_IDENTIFY's eth.config.features, header split on"
                  : "LIF_IDENTIFY's eth.config.features, header split off",
               on ? "0x7b0: Rx checksum and the Tx offloads"
                  : "0x7a0: the Tx offloads alone");
        ionic_eth_emu_destroy(emu);
    }
}

/* I1: the first 26 bytes of union ionic_q_identity for each queue type. */
static void check_q_identify(void)
{
    /* version 0, supported 0x01, features, desc_sz 16, comp_sz 16,
     * sg_desc_sz 128, max_sg_elems 8, sg_desc_stride 8 */
    static const uint8_t rxq_on[26] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x10, 0x00, 0x80, 0x00, 0x08, 0x00, 0x08, 0x00,
    };
    static const uint8_t rxq_off[26] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x10, 0x00, 0x80, 0x00, 0x08, 0x00, 0x08, 0x00,
    };
    static const uint8_t other_types[] = {IONIC_QTYPE_ADMINQ,
                                          IONIC_QTYPE_NOTIFYQ, IONIC_QTYPE_TXQ};
    static uint8_t data[DEVCMD_DATA_SIZE];

    for (int on = 0; on < 2; on++) {
        struct ionic_eth_emu *emu = new_emu(on != 0);
        uint8_t cmd[64] = {IONIC_CMD_Q_IDENTIFY};
        uint8_t comp[16] = {0};
        bool ok = true;

        cmd[4] = IONIC_QTYPE_RXQ;
        cmd[5] = 2; /* the highest version Linux knows for an Rx queue */
        memset(data, 0xa5, sizeof(data));
        ionic_eth_emu_handle_q_identify(emu, cmd, comp, data);
        if (memcmp(data, on ? rxq_on : rxq_off, sizeof(rxq_on)) != 0) {
            dump("identity", data, sizeof(rxq_on));
        }
        report(comp[0] == IONIC_RC_SUCCESS &&
                   memcmp(data, on ? rxq_on : rxq_off, sizeof(rxq_on)) == 0,
               on ? "positive" : "regression",
               on ? "Q_IDENTIFY of the Rx queue, header split on"
                  : "Q_IDENTIFY of the Rx queue, header split off",
               on ? "features 0x20020, and comp_sz still 16"
                  : "no feature, as before");

        for (size_t i = 0; i < sizeof(other_types); i++) {
            cmd[4] = other_types[i];
            cmd[5] = 0;
            memset(data, 0xa5, sizeof(data));
            ionic_eth_emu_handle_q_identify(emu, cmd, comp, data);
            ok = ok && comp[0] == IONIC_RC_SUCCESS &&
                 ernic_get_le64(data + 8) == 0;
        }
        report(ok, "regression",
               on ? "Q_IDENTIFY of the other queue types, header split on"
                  : "Q_IDENTIFY of the other queue types, header split off",
               "no feature");
        ionic_eth_emu_destroy(emu);
    }
}

/* Q2: the Rx queue features Q_INIT takes, and how it refuses the rest. */
static void check_q_init_features(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        bool header_split;
        uint16_t flags;
        uint64_t sg_ring;
        uint64_t features;
        uint8_t want_status;
        bool want_hds;
    } rows[] = {
        {"positive", "no feature", "a queue without header split", true,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, 0, IONIC_RC_SUCCESS, false},
        {"positive", "0x20020 with an SG ring", "a header-split queue", true,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, FEATURES_HDS, IONIC_RC_SUCCESS,
         true},
        {"positive", "no feature and no SG ring",
         "a queue without header split", true, QINIT_F_IRQ, 0, 0,
         IONIC_RC_SUCCESS, false},
        {"negative", "2X_CQ_DESC without HDS", "EINVAL", true,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, Q_F_2X_CQ, IONIC_RC_EINVAL, false},
        {"negative", "HDS without 2X_CQ_DESC", "EINVAL", true,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, RXQ_F_HDS, IONIC_RC_EINVAL, false},
        {"negative", "0x20020 without IONIC_QINIT_F_SG", "EINVAL", true,
         QINIT_F_IRQ, RXQ_SGL, FEATURES_HDS, IONIC_RC_EINVAL, false},
        {"negative", "0x20020 with an sg_ring_base of 0", "EINVAL", true,
         QINIT_F_IRQ | QINIT_F_SG, 0, FEATURES_HDS, IONIC_RC_EINVAL, false},
        {"negative", "0x20020 and bit 0", "ENOSUPP", true,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, FEATURES_HDS | 1U, IONIC_RC_ENOSUPP,
         false},
        {"negative", "0x20020 and IONIC_RXQ_F_HWSTAMP, which is not offered",
         "ENOSUPP", true, QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL,
         FEATURES_HDS | RXQ_F_HWSTAMP, IONIC_RC_ENOSUPP, false},
        {"negative", "bit 63 alone", "ENOSUPP", true, QINIT_F_IRQ | QINIT_F_SG,
         RXQ_SGL, UINT64_C(1) << 63, IONIC_RC_ENOSUPP, false},
        {"negative", "a bit not offered, without an SG ring",
         "ENOSUPP, which comes before EINVAL", true, QINIT_F_IRQ, 0,
         FEATURES_HDS | RXQ_F_HWSTAMP, IONIC_RC_ENOSUPP, false},
        {"regression", "0x20020 with header split off",
         "accepted and ignored, as every feature was (M2)", false,
         QINIT_F_IRQ | QINIT_F_SG, RXQ_SGL, FEATURES_HDS, IONIC_RC_SUCCESS,
         false},
        {"regression", "every bit with header split off",
         "accepted and ignored (M2)", false, QINIT_F_IRQ, 0, UINT64_MAX,
         IONIC_RC_SUCCESS, false},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct ionic_eth_emu *emu = new_emu(rows[i].header_split);
        const struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][RXQ_ID];
        bool refused = rows[i].want_status != IONIC_RC_SUCCESS;
        uint8_t status;
        char what[160];

        (void)snprintf(what, sizeof(what), "Q_INIT of an Rx queue with %s",
                       rows[i].description);
        g_nwarnings = 0;
        status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID,
                        &(struct q_init_args){.flags = rows[i].flags,
                                              .sg_ring = rows[i].sg_ring,
                                              .cq_ring = RXQ_CQ,
                                              .features = rows[i].features});
        if (status != rows[i].want_status) {
            printf("     status %u, expected %u\n", status,
                   rows[i].want_status);
        }
        report(status == rows[i].want_status && same(q->valid, !refused) &&
                   same(q->hds, rows[i].want_hds) &&
                   g_nwarnings == (refused ? 1U : 0U),
               rows[i].category, what, rows[i].expected);
        ionic_eth_emu_destroy(emu);
    }
}

/* Q2, Q4 and Q5: what a refused or repeated Q_INIT leaves behind, and the
 * queue types whose features are not looked at. */
static void check_q_init_state(void)
{
    struct ionic_eth_emu *emu = new_emu(true);
    const struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][RXQ_ID];
    static const uint8_t ignoring[] = {IONIC_QTYPE_ADMINQ, IONIC_QTYPE_NOTIFYQ,
                                       IONIC_QTYPE_TXQ};
    const struct q_init_args hds = {.flags = QINIT_F_IRQ | QINIT_F_SG,
                                    .sg_ring = RXQ_SGL,
                                    .cq_ring = RXQ_CQ,
                                    .features = FEATURES_HDS};
    const struct q_init_args plain = {.flags = QINIT_F_IRQ | QINIT_F_SG,
                                      .sg_ring = RXQ_SGL,
                                      .cq_ring = RXQ_CQ};
    /* Half of the pair, and other rings than the queue has */
    const struct q_init_args half = {
        .flags = QINIT_F_IRQ, .cq_ring = 0x3000, .features = Q_F_2X_CQ};
    const struct q_init_args every_bit = {
        .flags = QINIT_F_IRQ, .cq_ring = RXQ_CQ, .features = UINT64_MAX};
    struct eth_queue before;
    uint8_t status;
    bool ok = true;

    status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID, &hds);
    q_enable(emu, IONIC_QTYPE_RXQ, RXQ_ID);
    before = *q;
    report(status == IONIC_RC_SUCCESS && q->hds && q->enabled, "positive",
           "Q_INIT and Q_CONTROL of a header-split queue", "set up");

    status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID, &half);
    report(status == IONIC_RC_EINVAL && q->valid && q->enabled && q->hds &&
               q->sg && q->cq_ring_base == before.cq_ring_base &&
               q->sg_ring_base == before.sg_ring_base,
           "negative", "a refused Q_INIT of a queue that is set up",
           "EINVAL; the queue as it was, still a header-split queue");

    status = q_init(emu, IONIC_QTYPE_RXQ, 64, &every_bit);
    report(status == IONIC_RC_EQID, "negative",
           "Q_INIT of queue 64 with every feature bit",
           "EQID: the index is checked before the features");

    status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID, &plain);
    report(status == IONIC_RC_SUCCESS && q->valid && !q->hds && !q->enabled,
           "positive", "Q_INIT of the same queue again with no feature",
           "a queue without header split: it lasts until the next Q_INIT");

    for (size_t i = 0; i < sizeof(ignoring); i++) {
        status = q_init(emu, ignoring[i], 0, &every_bit);
        ok = ok && status == IONIC_RC_SUCCESS &&
             emu->eth_q[ignoring[i]][0].valid &&
             !emu->eth_q[ignoring[i]][0].hds;
    }
    report(ok, "regression",
           "Q_INIT of an admin, notify and Tx queue with every feature bit",
           "accepted: only an Rx queue's features are looked at (Q5)");
    ionic_eth_emu_destroy(emu);
}

/* What a refused Q_INIT logs: the features an Rx queue may ask for are a
 * rule only where the device reads them, so only there are they named. */
static void check_q_init_refusal_log(void)
{
    const struct q_init_args every_bit = {.flags = QINIT_F_IRQ | QINIT_F_SG,
                                          .sg_ring = RXQ_SGL,
                                          .cq_ring = RXQ_CQ,
                                          .features = UINT64_MAX};
    const struct q_init_args extra_bit = {.flags = QINIT_F_IRQ | QINIT_F_SG,
                                          .sg_ring = RXQ_SGL,
                                          .cq_ring = RXQ_CQ,
                                          .features = FEATURES_HDS | 1U};
    struct ionic_eth_emu *emu = new_emu(false);
    uint8_t status;

    g_nwarnings = 0;
    g_last_warning[0] = '\0';
    status = q_init(emu, IONIC_QTYPE_RXQ, 64, &every_bit);
    report(status == IONIC_RC_EQID && g_nwarnings == 1 &&
               strstr(g_last_warning, "index below") != NULL &&
               strstr(g_last_warning, "features") == NULL,
           "regression",
           "the log line of a refused Rx Q_INIT with header split off",
           "no word of features, which the device does not read");
    ionic_eth_emu_destroy(emu);

    emu = new_emu(true);
    g_nwarnings = 0;
    g_last_warning[0] = '\0';
    status = q_init(emu, IONIC_QTYPE_TXQ, 64, &every_bit);
    report(status == IONIC_RC_EQID && g_nwarnings == 1 &&
               strstr(g_last_warning, "index below") != NULL &&
               strstr(g_last_warning, "features") == NULL,
           "regression",
           "the log line of a refused Tx Q_INIT with header split on",
           "no word of features: only an Rx queue's are read");

    g_nwarnings = 0;
    g_last_warning[0] = '\0';
    status = q_init(emu, IONIC_QTYPE_RXQ, RXQ_ID, &extra_bit);
    report(status == IONIC_RC_ENOSUPP && g_nwarnings == 1 &&
               strstr(g_last_warning, "features=0x20021") != NULL &&
               strstr(g_last_warning, "0x20020 with an SG ring") != NULL,
           "positive", "the log line of an Rx Q_INIT refused for its features",
           "the features asked for and the two the device takes");
    ionic_eth_emu_destroy(emu);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(kind_cases) / sizeof(kind_cases[0]); i++) {
        run_kind_case(&kind_cases[i]);
    }
    for (size_t i = 0; i < sizeof(place_cases) / sizeof(place_cases[0]); i++) {
        run_place_case(&place_cases[i]);
    }
    check_examples();
    check_ring_wrap();
    check_stale_bytes();
    check_parse_bounds();
    check_dma_faults();
    check_lif_features();
    check_q_identify();
    check_q_init_features();
    check_q_init_state();
    check_q_init_refusal_log();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
