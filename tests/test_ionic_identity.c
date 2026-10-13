/*
 * Unit tests for the NIC identity and link the ionic emulator presents.
 *
 * The guest driver learns what device it has from a handful of blocks:
 * dev_info_regs in BAR0, the IDENTIFY, LIF_IDENTIFY and Q_IDENTIFY data,
 * and what PORT_INIT and LIF_INIT write into guest memory.  NIC profiles
 * (ionic_profile.h) now drive the values in them that used to be
 * constants, so these tests check two things:
 *
 *   - the compat profile still produces every byte of those blocks as
 *     captured from develop before profiles existed (golden rows);
 *   - each identity and link key lands at the offset the driver reads,
 *     with the value it should have, and nowhere else changes.
 *
 * The test links the emulator, calls its devcmd handlers through
 * ionic_eth_emu_priv.h, and backs the DMA entry points with a plain buffer
 * (ionic_eth_emu_fakes.h).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "golden/ionic_compat_identity.h"
#include "ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic_eth_emu_priv.h"
#include "ionic_profile.h"

static int failures;

/* Where the fake driver puts its DMA areas. */
#define PORT_INFO_PA 1024u
#define LIF_INFO_PA  4096u

/* ---- Capturing the blocks ---------------------------------------------- */

enum block {
    BLK_DEV_INFO,
    BLK_DEV_IDENTITY,
    BLK_DEV_IDENTITY_COMP,
    BLK_LIF_IDENTITY,
    BLK_LIF_IDENTITY_COMP,
    BLK_Q_IDENTITY0,
    BLK_Q_IDENTITY1,
    BLK_Q_IDENTITY2,
    BLK_Q_IDENTITY3,
    BLK_PORT_INFO,
    BLK_LIF_STATUS,
    BLK_COUNT,
};

static const char *const block_names[BLK_COUNT] = {
    "dev_info_regs",
    "dev_identity",
    "dev_identity completion",
    "lif_identity",
    "lif_identity completion",
    "q_identity qtype 0",
    "q_identity qtype 1",
    "q_identity qtype 2",
    "q_identity qtype 3",
    "published port_info",
    "lif_status",
};

static const struct {
    const uint8_t *bytes;
    size_t len;
} golden[BLK_COUNT] = {
    {golden_dev_info_regs, sizeof(golden_dev_info_regs)},
    {golden_dev_identity, sizeof(golden_dev_identity)},
    {golden_dev_identity_comp, sizeof(golden_dev_identity_comp)},
    {golden_lif_identity, sizeof(golden_lif_identity)},
    {golden_lif_identity_comp, sizeof(golden_lif_identity_comp)},
    {golden_q_identity_qtype0, sizeof(golden_q_identity_qtype0)},
    {golden_q_identity_qtype1, sizeof(golden_q_identity_qtype1)},
    {golden_q_identity_qtype2, sizeof(golden_q_identity_qtype2)},
    {golden_q_identity_qtype3, sizeof(golden_q_identity_qtype3)},
    {golden_port_info_published, sizeof(golden_port_info_published)},
    {golden_lif_status, sizeof(golden_lif_status)},
};

/* Every block, as the guest would read it from @emu. */
struct blocks {
    uint8_t b[BLK_COUNT][DEVCMD_DATA_SIZE];
};

static void capture(struct ionic_eth_emu *emu, struct blocks *out)
{
    static uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64], comp[16];
    uint64_t pa;

    memset(out, 0, sizeof(*out));
    memcpy(out->b[BLK_DEV_INFO], emu->bar0 + IONIC_BAR0_DEV_INFO_REGS_OFFSET,
           golden[BLK_DEV_INFO].len);

    memset(cmd, 0, sizeof(cmd));
    memset(comp, 0, sizeof(comp));
    ionic_eth_emu_handle_identify(emu, cmd, comp, data);
    memcpy(out->b[BLK_DEV_IDENTITY], data, golden[BLK_DEV_IDENTITY].len);
    memcpy(out->b[BLK_DEV_IDENTITY_COMP], comp, sizeof(comp));

    memset(cmd, 0, sizeof(cmd));
    memset(comp, 0, sizeof(comp));
    ionic_eth_emu_handle_lif_identify(emu, cmd, comp, data);
    memcpy(out->b[BLK_LIF_IDENTITY], data, golden[BLK_LIF_IDENTITY].len);
    memcpy(out->b[BLK_LIF_IDENTITY_COMP], comp, sizeof(comp));

    for (uint8_t qt = 0; qt < 4; qt++) {
        memset(cmd, 0, sizeof(cmd));
        memset(comp, 0, sizeof(comp));
        cmd[3] = qt;
        ionic_eth_emu_handle_q_identify(emu, cmd, comp, data);
        memcpy(out->b[BLK_Q_IDENTITY0 + qt], data, 64);
    }

    memset(g_mem, 0, sizeof(g_mem));
    memset(cmd, 0, sizeof(cmd));
    pa = htole64(PORT_INFO_PA);
    cmd[0] = IONIC_CMD_PORT_INIT;
    memcpy(cmd + 8, &pa, 8);
    ionic_eth_emu_handle_port_init(emu, cmd, comp);
    memcpy(out->b[BLK_PORT_INFO], g_mem + PORT_INFO_PA,
           IONIC_PORT_INFO_PUBLISHED_LEN);

    memset(g_mem, 0, sizeof(g_mem));
    memset(cmd, 0, sizeof(cmd));
    pa = htole64(LIF_INFO_PA);
    cmd[0] = IONIC_CMD_LIF_INIT;
    memcpy(cmd + 8, &pa, 8);
    ionic_eth_emu_handle_lif_init(emu, cmd, comp);
    memcpy(out->b[BLK_LIF_STATUS],
           g_mem + LIF_INFO_PA + IONIC_LIF_INFO_STATUS_OFF,
           golden[BLK_LIF_STATUS].len);
}

/* The first offset at which block @blk of @got differs from golden, or -1. */
static long golden_diff(const struct blocks *got, enum block blk)
{
    for (size_t i = 0; i < golden[blk].len; i++)
        if (got->b[blk][i] != golden[blk].bytes[i])
            return (long)i;
    return -1;
}

/* An emulator with @ini layered on compat, or compat when @ini is NULL. */
static struct ionic_eth_emu *make_emu(const char *ini, bool set_profile)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;
    char err[256];

    if (!emu)
        return NULL;
    ionic_profile_defaults(&p);
    if (ini && ionic_profile_apply_text(&p, "row", ini, strlen(ini), err,
                                        sizeof(err)) != 0) {
        printf("     profile rejected: %s\n", err);
        ionic_eth_emu_destroy(emu);
        return NULL;
    }
    if (set_profile)
        ionic_eth_emu_set_profile(emu, &p);
    return emu;
}

/* ---- Golden rows ------------------------------------------------------- */

struct golden_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *ini;  /* layered on compat; NULL for none */
    bool set_profile; /* call ionic_eth_emu_set_profile() at all */
};

static const struct golden_case golden_cases[] = {
    {"corner", "a new emulator with no profile set",
     "every block byte-identical to develop dd8918e", NULL, false},
    {"corner", "the compat defaults passed to ionic_eth_emu_set_profile()",
     "every block byte-identical to develop dd8918e", NULL, true},
    {"corner", "the built-in compat text layered on compat",
     "every block byte-identical to develop dd8918e",
     "[identity]\nasic_type = none\nasic_rev = 0\nfw_version = "
     "rocm-ernic-1.0\n[link]\nspeed_mbps = 100000\nxcvr = copper\n"
     "mtu_max = 9198\n",
     true},
};

static void run_golden_case(const struct golden_case *c)
{
    static struct blocks got;
    struct ionic_eth_emu *emu = make_emu(c->ini, c->set_profile);
    bool ok = emu != NULL;

    if (emu) {
        capture(emu, &got);
        ionic_eth_emu_destroy(emu);
    }
    for (int b = 0; ok && b < BLK_COUNT; b++)
        if (golden_diff(&got, (enum block)b) >= 0)
            ok = false;
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        for (int b = 0; emu && b < BLK_COUNT; b++) {
            long at = golden_diff(&got, (enum block)b);

            if (at >= 0)
                printf("     %s differs at byte %ld: got 0x%02x, golden "
                       "0x%02x\n",
                       block_names[b], at, got.b[b][at], golden[b].bytes[at]);
        }
        failures++;
    }
}

/* ---- Field rows -------------------------------------------------------- */

#define LE16B(v) (uint8_t)((v) & 0xff), (uint8_t)(((v) >> 8) & 0xff)
#define LE32B(v)                                         \
    (uint8_t)((v) & 0xff), (uint8_t)(((v) >> 8) & 0xff), \
        (uint8_t)(((v) >> 16) & 0xff), (uint8_t)(((v) >> 24) & 0xff)

/* Offsets the driver reads (ionic_if.h), named for the rows below. */
#define DEVINFO_FW_VERSION DEVINFO_FW_VERSION_OFF
#define PORT_CONFIG_SPEED  IONIC_PORT_INFO_CONFIG_SPEED_OFF
#define PORT_CONFIG_MTU    IONIC_PORT_INFO_CONFIG_MTU_OFF
#define PORT_STATUS_SPEED  IONIC_PORT_INFO_STATUS_SPEED_OFF
#define PORT_XCVR_STATE    IONIC_PORT_INFO_XCVR_STATE_OFF
#define LIF_STATUS_SPEED   12u
/* lif_identity eth.max_frame_size (LIFID_MAX_FRAME_SIZE_OFF, which is local
 * to ionic_eth_emu_handle_lif_identify()). */
#define LIF_MAX_FRAME 0x01au

#define FIELD_MAX 32u

/* Blocks a row's key may also change outside its own field. */
#define ALLOW(blk)   (1u << (blk))
#define SPEED_BLOCKS (ALLOW(BLK_PORT_INFO) | ALLOW(BLK_LIF_STATUS))
#define MTU_BLOCKS   (ALLOW(BLK_PORT_INFO) | ALLOW(BLK_LIF_IDENTITY))

/*
 * One profile, and the bytes it must produce at one offset of one block.
 * Every other byte of every block must match compat, except in the blocks
 * in @allow: speed, for one, reaches port_info and lif_status both, and
 * each place has its own row.
 */
struct field_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *ini;
    enum block blk;
    uint32_t off;
    uint32_t len;
    uint8_t want[FIELD_MAX];
    uint32_t allow;
};

static const struct field_case field_cases[] = {
    {"positive",
     "link.speed_mbps = 400000, port_info status",
     "le32 400000 in port_info status.speed",
     "[link]\nspeed_mbps = 400000\n",
     BLK_PORT_INFO,
     PORT_STATUS_SPEED,
     4,
     {LE32B(400000)},
     SPEED_BLOCKS},
    {"positive",
     "link.speed_mbps = 400000, port_info config",
     "le32 400000 in port_info config.speed",
     "[link]\nspeed_mbps = 400000\n",
     BLK_PORT_INFO,
     PORT_CONFIG_SPEED,
     4,
     {LE32B(400000)},
     SPEED_BLOCKS},
    {"positive",
     "link.speed_mbps = 400000, lif_status",
     "le32 400000 in lif_status.link_speed, which ethtool and sysfs rate "
     "report",
     "[link]\nspeed_mbps = 400000\n",
     BLK_LIF_STATUS,
     LIF_STATUS_SPEED,
     4,
     {LE32B(400000)},
     SPEED_BLOCKS},
    {"boundary",
     "link.speed_mbps = 1000, the slowest speed",
     "le32 1000 in lif_status.link_speed",
     "[link]\nspeed_mbps = 1000\n",
     BLK_LIF_STATUS,
     LIF_STATUS_SPEED,
     4,
     {LE32B(1000)},
     SPEED_BLOCKS},
    {"positive",
     "identity.fw_version = 1.8.0-E-32",
     "the string NUL-padded to 32 bytes in dev_info fw_version",
     "[identity]\nfw_version = 1.8.0-E-32\n",
     BLK_DEV_INFO,
     DEVINFO_FW_VERSION,
     32,
     {'1', '.', '8', '.', '0', '-', 'E', '-', '3', '2'},
     0},
    {"boundary",
     "identity.fw_version of exactly 31 characters",
     "all 31 characters, then a NUL at byte 31",
     "[identity]\nfw_version = abcdefghijklmnopqrstuvwxyz01234\n",
     BLK_DEV_INFO,
     DEVINFO_FW_VERSION,
     32,
     {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k',
      'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
      'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', 0},
     0},
    {"positive",
     "identity.asic_type = elba, asic_rev = 3",
     "dev_info asic_type 2 (IONIC_ASIC_TYPE_ELBA) and asic_rev 3",
     "[identity]\nasic_type = elba\nasic_rev = 3\n",
     BLK_DEV_INFO,
     DEVINFO_ASIC_TYPE_OFF,
     2,
     {2, 3},
     0},
    {"boundary",
     "identity.asic_rev = 255",
     "dev_info asic_rev 255",
     "[identity]\nasic_rev = 255\n",
     BLK_DEV_INFO,
     DEVINFO_ASIC_REV_OFF,
     1,
     {255},
     0},
    {"positive",
     "link.xcvr = fiber at 400G",
     "inserted, phy FIBER (2), pid QSFP_400G_SR4 (81): ethtool port FIBRE",
     "[link]\nspeed_mbps = 400000\nxcvr = fiber\n",
     BLK_PORT_INFO,
     PORT_XCVR_STATE,
     4,
     {1, 2, LE16B(81)},
     SPEED_BLOCKS},
    {"positive",
     "link.xcvr = fiber at 100G",
     "inserted, phy FIBER, pid QSFP_100G_SR4 (52)",
     "[link]\nxcvr = fiber\n",
     BLK_PORT_INFO,
     PORT_XCVR_STATE,
     4,
     {1, 2, LE16B(52)},
     0},
    {"positive",
     "link.xcvr = copper at 25G",
     "inserted, phy COPPER (1), pid SFP_25GBASE_CR_S (3)",
     "[link]\nspeed_mbps = 25000\n",
     BLK_PORT_INFO,
     PORT_XCVR_STATE,
     4,
     {1, 1, LE16B(3)},
     SPEED_BLOCKS},
    {"boundary",
     "link.xcvr = fiber at 50G, which has no SR part id",
     "inserted, phy FIBER, pid UNKNOWN (0)",
     "[link]\nspeed_mbps = 50000\nxcvr = fiber\n",
     BLK_PORT_INFO,
     PORT_XCVR_STATE,
     4,
     {1, 2, LE16B(0)},
     SPEED_BLOCKS},
    {"positive",
     "link.xcvr = none",
     "state REMOVED (0), phy NONE (0), pid 0: ethtool port None",
     "[link]\nxcvr = none\n",
     BLK_PORT_INFO,
     PORT_XCVR_STATE,
     4,
     {0, 0, LE16B(0)},
     0},
    {"boundary",
     "link.mtu_max = 1500, the smallest",
     "LIF identity max_frame_size 1518, so the guest's max MTU is 1500",
     "[link]\nmtu_max = 1500\n",
     BLK_LIF_IDENTITY,
     LIF_MAX_FRAME,
     4,
     {LE32B(1518)},
     MTU_BLOCKS},
    {"boundary",
     "link.mtu_max = 1500, port_info config",
     "port_info config.mtu 1500, not above the max",
     "[link]\nmtu_max = 1500\n",
     BLK_PORT_INFO,
     PORT_CONFIG_MTU,
     4,
     {LE32B(1500)},
     MTU_BLOCKS},
    {"boundary",
     "link.mtu_max = 9582, the largest",
     "max_frame_size 9600, the emulator's frame buffer",
     "[link]\nmtu_max = 9582\n",
     BLK_LIF_IDENTITY,
     LIF_MAX_FRAME,
     4,
     {LE32B(9600)},
     MTU_BLOCKS},
    {"corner",
     "link.mtu_max = 9582, port_info config",
     "port_info config.mtu stays 9000, as compat",
     "[link]\nmtu_max = 9582\n",
     BLK_PORT_INFO,
     PORT_CONFIG_MTU,
     4,
     {LE32B(9000)},
     MTU_BLOCKS},
};

static void run_field_case(const struct field_case *c)
{
    static struct blocks got;
    struct ionic_eth_emu *emu = make_emu(c->ini, true);
    bool field_ok = false, rest_ok = true;
    long stray_at = -1;
    int stray_blk = -1;

    if (emu) {
        capture(emu, &got);
        ionic_eth_emu_destroy(emu);
        field_ok = memcmp(got.b[c->blk] + c->off, c->want, c->len) == 0;
        for (int b = 0; b < BLK_COUNT && rest_ok; b++) {
            if (c->allow & ALLOW(b))
                continue;
            for (size_t i = 0; i < golden[b].len; i++) {
                if (b == (int)c->blk && i >= c->off && i < c->off + c->len)
                    continue;
                if (got.b[b][i] != golden[b].bytes[i]) {
                    rest_ok = false;
                    stray_blk = b;
                    stray_at = (long)i;
                    break;
                }
            }
        }
    }
    bool ok = emu && field_ok && rest_ok;

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        if (emu && !field_ok) {
            printf("     %s +%u:", block_names[c->blk], c->off);
            for (uint32_t i = 0; i < c->len; i++)
                printf(" %02x", got.b[c->blk][c->off + i]);
            printf(" (want");
            for (uint32_t i = 0; i < c->len; i++)
                printf(" %02x", c->want[i]);
            printf(")\n");
        }
        if (emu && !rest_ok)
            printf("     %s changed at byte %ld\n", block_names[stray_blk],
                   stray_at);
        failures++;
    }
}

/*
 * Corner: a second ionic_eth_emu_set_profile() with a shorter fw_version
 * must not leave the first one's tail behind it.
 */
static void check_fw_version_shrinks(void)
{
    struct ionic_eth_emu *emu = make_emu(
        "[identity]\nfw_version = a-very-long-firmware-version\n", true);
    struct ionic_profile p;
    bool ok = emu != NULL;

    ionic_profile_defaults(&p);
    memcpy(p.identity.fw_version, "v2", 3);
    if (emu) {
        const uint8_t *fw;

        ionic_eth_emu_set_profile(emu, &p);
        fw = emu->bar0 + IONIC_BAR0_DEV_INFO_REGS_OFFSET + DEVINFO_FW_VERSION;
        ok = memcmp(fw, "v2", 3) == 0;
        for (size_t i = 3; ok && i < DEVINFO_FWVERS_BUFLEN; i++)
            if (fw[i] != 0)
                ok = false;
        ionic_eth_emu_destroy(emu);
    }
    printf("%s corner: a shorter fw_version set after a longer one; "
           "expected: 'v2' and 29 zero bytes, no stale tail\n",
           ok ? "ok  " : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    for (size_t i = 0; i < sizeof(golden_cases) / sizeof(golden_cases[0]); i++)
        run_golden_case(&golden_cases[i]);
    for (size_t i = 0; i < sizeof(field_cases) / sizeof(field_cases[0]); i++)
        run_field_case(&field_cases[i]);
    check_fw_version_shrinks();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
