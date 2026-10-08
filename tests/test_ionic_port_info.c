/*
 * Unit tests for the ionic emulator's PORT_INIT port-info publication.
 *
 * ionic_get_link_ksettings() reads the speed and duplex out of
 * lif->info->status, which LIF_INIT has always filled in -- but only inside
 * `if (ks->base.port != PORT_NONE)`, and ks->base.port is derived from
 * port_info->status.xcvr.  While PORT_INIT was a stub that whole structure
 * stayed zero, so the transceiver read back as absent, the port resolved to
 * PORT_NONE, and the speed assignment never ran.  A guest then saw a NIC
 * logging "Link up - 100 Gbps" while ethtool reported "Speed: Unknown!" and
 * "Duplex: Half", and UCX -- which derives a TCP device's bandwidth from
 * that speed -- refused to build an endpoint address at all.
 *
 * These tests pin the published layout at the offsets the driver reads, so a
 * future change cannot silently go back to publishing zeros.  They run
 * entirely in-process: the TU is #included and the DMA entry points are
 * backed by a plain buffer, so no guest, no VM and no vfio-user socket is
 * involved.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "ionic_eth_emu.c"

/* ---- Fake guest memory ------------------------------------------------- */

/*
 * Large enough for a published port_info plus the LIF info block, with the
 * two placed far enough apart that a write to one cannot reach the other.
 */
#define GUEST_MEM_SIZE 8192
static uint8_t g_mem[GUEST_MEM_SIZE];

struct fake_sg {
    uint64_t addr;
    size_t len;
};

size_t dma_sg_size(void)
{
    return sizeof(struct fake_sg);
}

int vfu_addr_to_sgl(vfu_ctx_t *vfu_ctx, vfu_dma_addr_t dma_addr, size_t len,
                    dma_sg_t *sgl, size_t max_nr_sgs, int prot)
{
    struct fake_sg *sg = (struct fake_sg *)sgl;
    uint64_t addr = (uint64_t)(uintptr_t)dma_addr;

    (void)vfu_ctx;
    (void)prot;
    if (max_nr_sgs < 1 || addr + len > GUEST_MEM_SIZE)
        return -1;
    sg->addr = addr;
    sg->len = len;
    return 1;
}

int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    struct fake_sg *sg = (struct fake_sg *)sgl;

    (void)vfu_ctx;
    (void)flags;
    if (cnt < 1)
        return -1;
    iov->iov_base = g_mem + sg->addr;
    iov->iov_len = sg->len;
    return 0;
}

void vfu_sgl_put(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                 size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)iov;
    (void)cnt;
}

void vfu_sgl_mark_dirty(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)cnt;
}

void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)level;
    (void)fmt;
}

int vfu_irq_trigger(vfu_ctx_t *vfu_ctx, uint32_t subindex)
{
    (void)vfu_ctx;
    (void)subindex;
    return 0;
}

void pvrdma_irq_count(pvrdma_handle_t handle)
{
    (void)handle;
}

void pvrdma_eth_bytes_count(pvrdma_handle_t handle, uint64_t bytes, bool is_tx)
{
    (void)handle;
    (void)bytes;
    (void)is_tx;
}

void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val)
{
    (void)dp;
    (void)qtype;
    (void)doorbell_val;
}

void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx, int aq_idx,
                              uint16_t p_index)
{
    (void)ctx;
    (void)aq_idx;
    (void)p_index;
}

struct ionic_eth_net *ionic_eth_net_open_tap(const char *ifname,
                                             char *out_ifname,
                                             size_t out_ifname_len)
{
    (void)ifname;
    (void)out_ifname;
    (void)out_ifname_len;
    return NULL;
}

void ionic_eth_net_close(struct ionic_eth_net *net)
{
    (void)net;
}

int ionic_eth_net_send(struct ionic_eth_net *net, const void *frame, size_t len)
{
    (void)net;
    (void)frame;
    (void)len;
    return 0;
}

ssize_t ionic_eth_net_recv(struct ionic_eth_net *net, void *buf, size_t cap)
{
    (void)net;
    (void)buf;
    (void)cap;
    return 0;
}

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

#define CHECK(cond, ...)         \
    do {                         \
        if (!(cond)) {           \
            failures++;          \
            printf("  FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
        }                        \
    } while (0)

/* Where the fake driver puts its port_info block. */
#define PORT_INFO_PA 1024u

static uint32_t rd32(uint32_t off)
{
    uint32_t v;
    memcpy(&v, g_mem + PORT_INFO_PA + off, 4);
    return le32toh(v);
}

static uint16_t rd16(uint32_t off)
{
    uint16_t v;
    memcpy(&v, g_mem + PORT_INFO_PA + off, 2);
    return le16toh(v);
}

static uint8_t rd8(uint32_t off)
{
    return g_mem[PORT_INFO_PA + off];
}

/* Build a PORT_INIT command carrying info_pa and run it. */
static void run_port_init(struct ionic_eth_emu *emu, uint64_t info_pa)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};
    uint64_t pa = htole64(info_pa);

    cmd[0] = IONIC_CMD_PORT_INIT;
    memcpy(cmd + 8, &pa, 8);
    handle_port_init(emu, cmd, comp);
    CHECK(comp[0] == 0, "PORT_INIT returned status %u, expected 0", comp[0]);
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * The regression itself: a transceiver that reads back as present and
 * copper. ionic_get_link_ksettings() resolves PORT_NONE without this, and
 * PORT_NONE is what suppresses the speed assignment entirely.
 */
static void test_transceiver_is_published(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);

    memset(g_mem, 0, sizeof(g_mem));
    run_port_init(emu, PORT_INFO_PA);

    CHECK(rd8(IONIC_PORT_INFO_XCVR_STATE_OFF) == IONIC_XCVR_STATE_INSERTED,
          "xcvr.state is %u, expected INSERTED(%u) -- a transceiver that "
          "reads as removed resolves the port to PORT_NONE",
          rd8(IONIC_PORT_INFO_XCVR_STATE_OFF),
          (unsigned)IONIC_XCVR_STATE_INSERTED);
    CHECK(rd8(IONIC_PORT_INFO_XCVR_PHY_OFF) == IONIC_PHY_TYPE_COPPER,
          "xcvr.phy is %u, expected COPPER(%u)",
          rd8(IONIC_PORT_INFO_XCVR_PHY_OFF), (unsigned)IONIC_PHY_TYPE_COPPER);
    CHECK(rd16(IONIC_PORT_INFO_XCVR_PID_OFF) == IONIC_XCVR_PID_QSFP_100G_CR4,
          "xcvr.pid is %u, expected QSFP_100G_CR4(%u) -- the pid is what "
          "adds the supported link modes ethtool prints",
          rd16(IONIC_PORT_INFO_XCVR_PID_OFF),
          (unsigned)IONIC_XCVR_PID_QSFP_100G_CR4);

    ionic_eth_emu_destroy(emu);
}

/*
 * The speed must be non-zero in both the config and the status block. UCX
 * rejects a device whose bandwidth computes to zero, which is the failure
 * this whole path exists to avoid.
 */
static void test_speed_is_non_zero(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);

    memset(g_mem, 0, sizeof(g_mem));
    run_port_init(emu, PORT_INFO_PA);

    CHECK(rd32(IONIC_PORT_INFO_STATUS_SPEED_OFF) == IONIC_ETH_LINK_SPEED_MBPS,
          "status.speed is %u Mbps, expected %u",
          rd32(IONIC_PORT_INFO_STATUS_SPEED_OFF),
          (unsigned)IONIC_ETH_LINK_SPEED_MBPS);
    CHECK(rd32(IONIC_PORT_INFO_CONFIG_SPEED_OFF) == IONIC_ETH_LINK_SPEED_MBPS,
          "config.speed is %u Mbps, expected %u",
          rd32(IONIC_PORT_INFO_CONFIG_SPEED_OFF),
          (unsigned)IONIC_ETH_LINK_SPEED_MBPS);
    CHECK(rd32(IONIC_PORT_INFO_STATUS_SPEED_OFF) != 0,
          "status.speed is zero -- UCX computes bandwidth 0.00 from this and "
          "refuses to create an endpoint");

    ionic_eth_emu_destroy(emu);
}

/* The port must report itself operationally up and administratively up. */
static void test_port_is_up(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);

    memset(g_mem, 0, sizeof(g_mem));
    run_port_init(emu, PORT_INFO_PA);

    CHECK(rd8(IONIC_PORT_INFO_STATUS_STATUS_OFF) == IONIC_PORT_OPER_STATUS_UP,
          "status.status is %u, expected OPER_STATUS_UP(%u)",
          rd8(IONIC_PORT_INFO_STATUS_STATUS_OFF),
          (unsigned)IONIC_PORT_OPER_STATUS_UP);
    CHECK(rd8(IONIC_PORT_INFO_CONFIG_STATE_OFF) == IONIC_PORT_ADMIN_STATE_UP,
          "config.state is %u, expected ADMIN_STATE_UP(%u)",
          rd8(IONIC_PORT_INFO_CONFIG_STATE_OFF),
          (unsigned)IONIC_PORT_ADMIN_STATE_UP);

    ionic_eth_emu_destroy(emu);
}

/*
 * The offsets are the contract with the driver's struct layout, so pin them
 * rather than trusting that whoever edits the writer also reads ionic_if.h.
 * status sits at 256 because union ionic_port_config is __le32 words[64];
 * xcvr sits 60 bytes into the __packed status block.
 */
static void test_published_offsets_match_the_driver_layout(void)
{
    CHECK(IONIC_PORT_INFO_STATUS_SPEED_OFF == 256u + 4u,
          "status.speed offset drifted from the ionic_if.h layout");
    CHECK(IONIC_PORT_INFO_STATUS_STATUS_OFF == 256u + 8u,
          "status.status offset drifted from the ionic_if.h layout");
    CHECK(IONIC_PORT_INFO_XCVR_STATE_OFF == 256u + 60u,
          "status.xcvr offset drifted from the ionic_if.h layout");
    CHECK(IONIC_PORT_INFO_XCVR_PID_OFF == IONIC_PORT_INFO_XCVR_STATE_OFF + 2u,
          "xcvr.pid is not two bytes past xcvr.state");
    CHECK(IONIC_PORT_INFO_PUBLISHED_LEN >= IONIC_PORT_INFO_XCVR_PID_OFF + 2u,
          "the published write is too short to cover xcvr.pid");
}

/*
 * A null info_pa is what the driver sends when it is only probing, and it
 * must not be dereferenced or reported as an error.
 */
static void test_null_info_pa_is_harmless(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);

    memset(g_mem, 0xa5, sizeof(g_mem));
    run_port_init(emu, 0);

    CHECK(g_mem[0] == 0xa5, "a null info_pa wrote to guest memory anyway");

    ionic_eth_emu_destroy(emu);
}

/* Re-running PORT_INIT must be idempotent; the driver may init twice. */
static void test_port_init_is_idempotent(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    uint8_t first[IONIC_PORT_INFO_PUBLISHED_LEN];

    memset(g_mem, 0, sizeof(g_mem));
    run_port_init(emu, PORT_INFO_PA);
    memcpy(first, g_mem + PORT_INFO_PA, sizeof(first));

    run_port_init(emu, PORT_INFO_PA);
    CHECK(memcmp(first, g_mem + PORT_INFO_PA, sizeof(first)) == 0,
          "a second PORT_INIT published different bytes");

    ionic_eth_emu_destroy(emu);
}

/*
 * The write must stay inside the published window: the sprom and statistics
 * blocks that follow belong to the driver's allocation.
 */
static void test_write_stays_within_the_published_window(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    size_t i;

    memset(g_mem, 0, sizeof(g_mem));
    memset(g_mem + PORT_INFO_PA + IONIC_PORT_INFO_PUBLISHED_LEN, 0x5a, 256);
    run_port_init(emu, PORT_INFO_PA);

    for (i = 0; i < 256; i++) {
        uint8_t b = g_mem[PORT_INFO_PA + IONIC_PORT_INFO_PUBLISHED_LEN + i];
        if (b != 0x5a) {
            CHECK(false, "PORT_INIT wrote %u bytes past the published window",
                  (unsigned)(i + 1));
            break;
        }
    }

    ionic_eth_emu_destroy(emu);
}

/* ---- SRQ capability in LIF_IDENTIFY ------------------------------------ */

/*
 * These are offset tests, because an offset is the only thing here that can
 * be wrong silently. Get srq_qtype's position wrong and the guest still
 * probes, still attaches, and simply reports max_srq as whatever rcq_sign_bit
 * and stats_type happen to spell -- there is no error anywhere to notice.
 *
 * The layout is union ionic_lif_identity's rdma section (ionic_if.h, as
 * extended by the SRQ series in rdma/for-next).
 */
static void test_srq_qtype_is_not_the_sixth_qtype(void)
{
    /* The trap: five contiguous qtypes, then a four-byte gap of stats_type,
     * rsvd and rcq_sign_bit before srq_qtype. Striding past eq_qtype lands
     * on stats_type, 4 bytes short. */
    CHECK(LIF_ID_SRQ_QTYPE_OFF == 32u + 5u * 12u + 4u,
          "srq_qtype is not four bytes past the fifth qtype slot");
    CHECK(LIF_ID_SRQ_QTYPE_OFF == 96u, "srq_qtype offset drifted from 96");
    CHECK(LIF_ID_ALLOC_QID_CAP_OFF == LIF_ID_SRQ_QTYPE_OFF + 12u + 5u,
          "alloc_qid_cap is not rsvd2[5] past the end of srq_qtype");
}

static void test_srq_capability_is_published(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};
    uint32_t qid_count;

    memset(data, 0xaa, sizeof(data)); /* poison: prove we write every field */
    handle_lif_identify(emu, cmd, comp, data);

    const uint8_t *rdma = data + LIF_ID_RDMA_OFF;
    const uint8_t *srq = rdma + LIF_ID_SRQ_QTYPE_OFF;

    CHECK(srq[0] == IONIC_RDMA_QTYPE_SRQ, "srq_qtype.qtype is %u, expected %u",
          srq[0], (unsigned)IONIC_RDMA_QTYPE_SRQ);

    memcpy(&qid_count, srq + 4, 4);
    /* This is the value the driver copies into max_srq. Zero here is exactly
     * the state that makes UCX drop rc_verbs and leaves nixl with ud_verbs,
     * which cannot do one-sided RDMA at all. */
    CHECK(le32toh(qid_count) == IONIC_EMU_SRQ_COUNT,
          "srq_qtype.qid_count is %u, expected %u", le32toh(qid_count),
          (unsigned)IONIC_EMU_SRQ_COUNT);

    CHECK((rdma[LIF_ID_ALLOC_QID_CAP_OFF] & IONIC_LIF_RDMA_ALLOC_QID_SRQ) != 0,
          "alloc_qid_cap does not claim SRQ qid allocation");

    ionic_eth_emu_destroy(emu);
}

/*
 * Both halves of ionic_query_device()'s gate, together. Either one alone
 * leaves max_srq at zero, and the symptom is identical in both cases, so a
 * test that only checked the count would pass while the feature stayed dead.
 */
static void test_srq_gate_needs_count_and_alloc_bit(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    uint8_t data[DEVCMD_DATA_SIZE];
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};
    uint32_t qid_count;

    memset(data, 0, sizeof(data));
    handle_lif_identify(emu, cmd, comp, data);

    const uint8_t *rdma = data + LIF_ID_RDMA_OFF;
    memcpy(&qid_count, rdma + LIF_ID_SRQ_QTYPE_OFF + 4, 4);

    CHECK(le32toh(qid_count) != 0 &&
              (rdma[LIF_ID_ALLOC_QID_CAP_OFF] & IONIC_LIF_RDMA_ALLOC_QID_SRQ),
          "the guest's SRQ gate is not satisfied: count=%u cap=0x%02x",
          le32toh(qid_count), rdma[LIF_ID_ALLOC_QID_CAP_OFF]);

    ionic_eth_emu_destroy(emu);
}

/*
 * The capability is useless without the opcodes to act on it: the driver
 * gates each SRQ command on `admin_opcodes <= IONIC_V1_ADMIN_<op>`, so a
 * watermark of 19 refuses CREATE_SRQ (26) through DESTROY_SRQ (29) however
 * the identity is filled in.
 */
static void test_admin_opcode_watermark_covers_srq(void)
{
    CHECK(IONIC_RDMA_ADMIN_OPCODES > 29,
          "admin_opcodes watermark %u does not reach DESTROY_SRQ (29)",
          (unsigned)IONIC_RDMA_ADMIN_OPCODES);
}

int main(void)
{
    test_published_offsets_match_the_driver_layout();
    test_srq_qtype_is_not_the_sixth_qtype();
    test_srq_capability_is_published();
    test_srq_gate_needs_count_and_alloc_bit();
    test_admin_opcode_watermark_covers_srq();
    test_transceiver_is_published();
    test_speed_is_non_zero();
    test_port_is_up();
    test_null_info_pa_is_harmless();
    test_port_init_is_idempotent();
    test_write_stays_within_the_published_window();

    if (failures) {
        printf("ionic_port_info: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("ionic_port_info: all checks passed\n");
    return 0;
}
