/*
 * ionic_eth_emu.c — emulate enough of the ionic Ethernet admin protocol
 * for ionic.ko to probe successfully and register an auxiliary bus device,
 * which ionic_rdma.ko then binds to.
 *
 * The real ionic hardware exposes three command mechanisms on BAR0:
 *   0x0000  dev_info_regs  (read-only: signature, fw_status, version string)
 *   0x0800  dev_cmd_regs   (read-write: doorbell/done/cmd/comp)
 *   0x0c00  dev_cmd_data   (r/w: side data for long commands)
 *   0x1000  intr_status    (r/o)
 *   0x2000  intr_ctrl[]    (r/w: per-vector coal/mask registers)
 *
 * BAR2 (doorbell BAR): per-LIF doorbell pages.
 *   The kernel driver stores lif->kern_dbpage = BAR2 base + kern_pid*PAGE_SIZE.
 *   We hand back kern_pid=0 in LIF_INIT so the kernel's doorbell page is at
 *   BAR2 offset 0.
 *
 * Command flow:
 *   1. ionic.ko reads dev_info_regs.signature; must be
 * IONIC_DEV_INFO_SIGNATURE.
 *   2. ionic.ko writes IDENTIFY cmd to dev_cmd_regs.cmd, rings doorbell.
 *   3. We process it (memcpy identify response into dev_cmd_data), set done=1.
 *   4. ionic.ko reads comp, reads data, proceeds to LIF_IDENTIFY, LIF_INIT,
 * etc.
 *   5. After adminq/notifyq init, ionic.ko calls ionic_auxbus_register() which
 *      creates the ionic.rdma auxiliary device -> ionic_rdma.ko probes it.
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include <endian.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_eth_emu.h"
#include "ionic_datapath.h"
#include "ionic_adminq.h"
#include "rocm_ernic_internal.h"

/* -------------------------------------------------------------------------
 * Inline helpers matching linux/byteorder conventions (host = little-endian
 * on x86; ionic wire format is little-endian for most fields).
 * -------------------------------------------------------------------------
 */
static inline uint16_t le16(uint16_t v)
{
    return htole16(v);
}
static inline uint32_t le32(uint32_t v)
{
    return htole32(v);
}
static inline uint64_t le64(uint64_t v)
{
    return htole64(v);
}

/* -------------------------------------------------------------------------
 * ionic_if.h constants we need without pulling in the full kernel header.
 * Keep in sync with the pinned kernel ref.
 * -------------------------------------------------------------------------
 */
#define IONIC_DEV_INFO_SIGNATURE 0x44455649u /* 'DEVI' */
#define IONIC_DEV_INFO_VERSION   1
#define IONIC_FW_STS_F_RUNNING   0x01u

/* BAR0 layout offsets */
#define IONIC_BAR0_DEV_INFO_REGS_OFFSET     0x0000u
#define IONIC_BAR0_DEV_CMD_REGS_OFFSET      0x0800u
#define IONIC_BAR0_DEV_CMD_DATA_REGS_OFFSET 0x0c00u
#define IONIC_BAR0_INTR_CTRL_OFFSET         0x2000u
#define IONIC_BAR0_SIZE                     0x8000u /* 32 KB */

#define IONIC_DEV_CMD_DONE 0x00000001u

/* Device command opcodes (only those we actually handle are defined here) */
#define IONIC_CMD_NOP           0
#define IONIC_CMD_IDENTIFY      1
#define IONIC_CMD_RESET         3
#define IONIC_CMD_PORT_IDENTIFY 10
#define IONIC_CMD_PORT_INIT     11
#define IONIC_CMD_PORT_RESET    12
#define IONIC_CMD_PORT_GETATTR  13
#define IONIC_CMD_PORT_SETATTR  14
#define IONIC_CMD_LIF_IDENTIFY  20
#define IONIC_CMD_LIF_INIT      21
#define IONIC_CMD_LIF_RESET     22
#define IONIC_CMD_LIF_GETATTR   23
#define IONIC_CMD_LIF_SETATTR   24
#define IONIC_CMD_Q_IDENTIFY    39
#define IONIC_CMD_Q_INIT        40
#define IONIC_CMD_Q_CONTROL     41

/* LIF capabilities */
#define IONIC_LIF_CAP_ETH  (1u << 0)
#define IONIC_LIF_CAP_RDMA (1u << 1)

/* ASIC type for emulated device */
#define IONIC_ASIC_TYPE_NONE 0

/* (Ethernet logical queue type IDs — kept only for documentation purposes) */

/* RDMA queue type hardware IDs (what the RDMA driver sees) */
#define IONIC_RDMA_QTYPE_AQ 5
#define IONIC_RDMA_QTYPE_SQ 6
#define IONIC_RDMA_QTYPE_RQ 7
#define IONIC_RDMA_QTYPE_CQ 8
#define IONIC_RDMA_QTYPE_EQ 9

/* Emulated RDMA capability version (must match ionic_fw.h expectations) */
#define IONIC_RDMA_VERSION       1
#define IONIC_RDMA_QP_OPCODES    16
#define IONIC_RDMA_ADMIN_OPCODES 19

/* Page table and MR counts for emulated device */
#define IONIC_NPTS_PER_LIF  (1u << 20) /* 1M page table entries */
#define IONIC_NMRS_PER_LIF  (1u << 17) /* 128K MRs              */
#define IONIC_NAHS_PER_LIF  (1u << 15) /* 32K AHs               */
#define IONIC_MAX_STRIDE    9          /* log2(512) bytes/WQE   */
#define IONIC_PAGE_SIZE_CAP (1u << 12) /* 4K pages supported     */

/* Number of emulated EQs / AQs we report in LIF identity */
#define IONIC_EMU_EQ_COUNT   32
#define IONIC_EMU_AQ_COUNT   4
#define IONIC_EMU_QP_COUNT   (1u << 15)
#define IONIC_EMU_CQ_COUNT   (1u << 16)
#define IONIC_EMU_UDMA_SHIFT 3 /* 8 queues per group */

/* -------------------------------------------------------------------------
 * dev_cmd_regs layout (64-bit register block starting at offset 0x0800)
 *
 *   +0x00  doorbell  (w1 triggers cmd processing)
 *   +0x04  done      (bit 0 = 1 when complete)
 *   +0x08  cmd[60]   (command bytes)
 *   +0x44  comp[16]  (completion bytes)
 *   +0x54  rsvd[48]
 * data area at 0x0c00: 478 * 4 = 1912 bytes
 * -------------------------------------------------------------------------
 */
#define DEVCMD_DOORBELL_OFF 0x00u
#define DEVCMD_DONE_OFF     0x04u
#define DEVCMD_CMD_OFF      0x08u
#define DEVCMD_COMP_OFF     0x44u

/* -------------------------------------------------------------------------
 * Emulator state
 * -------------------------------------------------------------------------
 */

/* Size of the emulated BAR0 shadow buffer (32 KB). */
#define BAR0_BUF_SIZE IONIC_BAR0_SIZE

struct ionic_eth_emu {
    vfu_ctx_t *vfu_ctx;

    /* Shadow copy of BAR0 contents.  Reads are served from here;
     * writes update it and, when the doorbell byte is written, trigger
     * command processing. */
    uint8_t bar0[BAR0_BUF_SIZE];

    /* Shadow copy of BAR2 (doorbell pages). */
    uint8_t *bar2;
    size_t bar2_size;

    /* True once LIF_INIT has completed. */
    bool lif_initialized;
    uint16_t lif_hw_index;

    /* RDMA devcmd handler (registered after construction). */
    ionic_rdma_devcmd_fn_t rdma_devcmd_fn;
    void *rdma_devcmd_opaque;

    /* Data-path handler for BAR2 doorbell writes. */
    struct ionic_datapath *dp;
    /* Admin queue context for AQ doorbell producer-index updates. */
    struct ionic_adminq_ctx *adminq;

    /* Interrupt controller shadow (per-vector: mask register). */
    uint32_t intr_mask[IONIC_MSIX_MAX_VECTORS];
};

/* -------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------
 */
static void process_devcmd(struct ionic_eth_emu *emu);
static void handle_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp, uint8_t *data);
static void handle_lif_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                                uint8_t *comp, uint8_t *data);
static void handle_lif_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp);
static void handle_lif_setattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp);
static void handle_lif_getattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp);
static void handle_q_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                              uint8_t *comp, uint8_t *data);
static void handle_q_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
                          uint8_t *comp);
static void handle_rdma_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp);

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

struct ionic_eth_emu *ionic_eth_emu_create(vfu_ctx_t *vfu_ctx, size_t bar2_size)
{
    struct ionic_eth_emu *emu = calloc(1, sizeof(*emu));
    if (!emu)
        return NULL;

    emu->vfu_ctx = vfu_ctx;
    emu->bar2_size = bar2_size;
    emu->bar2 = calloc(1, bar2_size);
    if (!emu->bar2) {
        free(emu);
        return NULL;
    }

    /* Initialise dev_info_regs in BAR0 shadow.
     * ionic.ko reads signature at offset 0 to confirm the device is alive,
     * and fw_status bit 0 to confirm firmware is running. */
    uint8_t *info = emu->bar0 + IONIC_BAR0_DEV_INFO_REGS_OFFSET;

    uint32_t sig = le32(IONIC_DEV_INFO_SIGNATURE);
    memcpy(info + 0, &sig, 4);        /* signature  */
    info[4] = IONIC_DEV_INFO_VERSION; /* version    */
    info[5] = IONIC_ASIC_TYPE_NONE;   /* asic_type  */
    info[6] = 0;                      /* asic_rev   */
    info[7] = IONIC_FW_STS_F_RUNNING; /* fw_status  */

    /* fw_version string at offset 8 */
    strncpy((char *)(info + 8), "rocm-ernic-1.0", 32);

    /* Mask all interrupts initially. */
    for (int i = 0; i < IONIC_MSIX_MAX_VECTORS; i++)
        emu->intr_mask[i] = 1;

    return emu;
}

void ionic_eth_emu_destroy(struct ionic_eth_emu *emu)
{
    if (!emu)
        return;
    free(emu->bar2);
    free(emu);
}

void ionic_eth_emu_register_rdma_handler(struct ionic_eth_emu *emu,
                                         ionic_rdma_devcmd_fn_t fn,
                                         void *opaque)
{
    emu->rdma_devcmd_fn = fn;
    emu->rdma_devcmd_opaque = opaque;
}

void ionic_eth_emu_register_datapath(struct ionic_eth_emu *emu,
                                     struct ionic_datapath *dp)
{
    emu->dp = dp;
}

void ionic_eth_emu_register_adminq(struct ionic_eth_emu *emu,
                                   struct ionic_adminq_ctx *adminq)
{
    emu->adminq = adminq;
}

/* -------------------------------------------------------------------------
 * BAR0 access callback
 *
 * The ionic driver accesses BAR0 as 32-bit MMIO registers.  We maintain a
 * shadow buffer and process commands when the doorbell DWORD is written.
 * -------------------------------------------------------------------------
 */
ssize_t ionic_eth_emu_bar0_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write)
{
    if ((size_t)offset + count > BAR0_BUF_SIZE) {
        errno = EINVAL;
        return -1;
    }

    if (!is_write) {
        memcpy(buf, emu->bar0 + offset, count);
        return (ssize_t)count;
    }

    /* Write path: update shadow, check for doorbell trigger. */
    memcpy(emu->bar0 + offset, buf, count);

    /* Doorbell is a DWORD write to dev_cmd_regs+0x00. Any non-zero value
     * written there means "process the command now". */
    loff_t cmd_base = IONIC_BAR0_DEV_CMD_REGS_OFFSET;
    loff_t doorbell_off = cmd_base + DEVCMD_DOORBELL_OFF;

    if (offset <= doorbell_off && offset + (loff_t)count > doorbell_off) {
        uint32_t db;
        memcpy(&db, emu->bar0 + doorbell_off, 4);
        if (db) {
            process_devcmd(emu);
            /* Clear doorbell and assert done. */
            uint32_t zero = 0;
            memcpy(emu->bar0 + doorbell_off, &zero, 4);
        }
    }

    /* Interrupt controller writes: just update mask shadow. */
    if (offset >= (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET &&
        (size_t)offset + count <= BAR0_BUF_SIZE) {
        loff_t rel = offset - (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET;
        int vec = (int)(rel / 32); /* each intr ctrl block is 32 bytes */
        if (vec >= 0 && vec < IONIC_MSIX_MAX_VECTORS) {
            loff_t in_blk = rel % 32;
            if (in_blk == 4 && count == 4)
                memcpy(&emu->intr_mask[vec], buf, 4);
        }
    }

    return (ssize_t)count;
}

/* -------------------------------------------------------------------------
 * BAR2 (doorbell) access callback
 * -------------------------------------------------------------------------
 */
ssize_t ionic_eth_emu_bar2_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write)
{
    if ((size_t)offset + count > emu->bar2_size) {
        errno = EINVAL;
        return -1;
    }

    if (!is_write) {
        memcpy(buf, emu->bar2 + offset, count);
        return (ssize_t)count;
    }

    /* Doorbell write: decode and forward to the appropriate queue.
     * ionic doorbell layout (8 bytes, little-endian):
     *   [15:0]  p_index  (producer index)
     *   [23:16] ring     (0=normal, 1=arm CQ/EQ)
     *   [31:24] qid_lo
     *   [47:32] qid_hi
     *   [63:48] reserved
     */
    memcpy(emu->bar2 + offset, buf, count);

    if (count == 8) {
        uint64_t db;
        memcpy(&db, buf, 8);
        uint16_t p_index = (uint16_t)(db & 0xffffu);
        uint8_t ring = (uint8_t)((db >> 16) & 0xffu);
        uint32_t qid =
            (uint32_t)(((db >> 24) & 0xffu) | (((db >> 32) & 0xffffu) << 8));

        /* Doorbell qtype is the slot index within the LIF's doorbell page.
         * ionic_dbell_ring(db_page, qtype, val) writes to &db_page[qtype],
         * i.e. byte offset (qtype * 8) within the page.  The page itself is
         * at BAR2 offset (kern_pid * PAGE_SIZE).  For the kernel LIF (pid=0):
         *   BAR2 offset = qtype * 8
         *   qtype = (offset % PAGE_SIZE) / sizeof(u64)
         * This correctly handles multiple LIFs (pid > 0) if ever supported.
         */
        int qtype = (int)((offset % IONIC_DB_PAGE_SIZE) / 8);

        vfu_log(emu->vfu_ctx, LOG_DEBUG,
                "ionic_eth_emu: doorbell BAR2 off=%#lx qtype=%d qid=%u "
                "ring=%u p_index=%u",
                (unsigned long)offset, qtype, qid, ring, p_index);

        /* AQ doorbell: update producer index so the poll loop knows WQEs are
         * ready. IONIC_RDMA_QTYPE_AQ = 5; each AQ is identified by qid (0-based
         * index). */
        if (qtype == IONIC_RDMA_QTYPE_AQ && emu->adminq)
            ionic_adminq_update_prod(emu->adminq, (int)qid, p_index);

        /* Forward all doorbells to the data-path handler for SQ/RQ/CQ/EQ. */
        if (emu->dp)
            ionic_datapath_doorbell(emu->dp, qtype, db);

        (void)ring;
    }

    return (ssize_t)count;
}

/* -------------------------------------------------------------------------
 * Command dispatch
 * -------------------------------------------------------------------------
 */

static void process_devcmd(struct ionic_eth_emu *emu)
{
    uint8_t *cmd_base = emu->bar0 + IONIC_BAR0_DEV_CMD_REGS_OFFSET;
    uint8_t *cmd = cmd_base + DEVCMD_CMD_OFF;
    uint8_t *comp = cmd_base + DEVCMD_COMP_OFF;
    uint8_t *data = emu->bar0 + IONIC_BAR0_DEV_CMD_DATA_REGS_OFFSET;

    uint8_t opcode = cmd[0];

    /* Clear completion and data before filling. */
    memset(comp, 0, 16);

    vfu_log(emu->vfu_ctx, LOG_INFO, "ionic_eth_emu: devcmd opcode=%u", opcode);

    switch (opcode) {
    case IONIC_CMD_NOP:
        comp[0] = 0; /* status OK */
        break;

    case IONIC_CMD_IDENTIFY:
        handle_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_RESET:
        comp[0] = 0;
        break;

    case IONIC_CMD_LIF_IDENTIFY:
        handle_lif_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_LIF_INIT:
        handle_lif_init(emu, cmd, comp);
        break;

    case IONIC_CMD_LIF_RESET:
        emu->lif_initialized = false;
        comp[0] = 0;
        break;

    case IONIC_CMD_LIF_SETATTR:
        handle_lif_setattr(emu, cmd, comp);
        break;

    case IONIC_CMD_LIF_GETATTR:
        handle_lif_getattr(emu, cmd, comp);
        break;

    case IONIC_CMD_Q_IDENTIFY:
        handle_q_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_Q_INIT:
        handle_q_init(emu, cmd, comp);
        break;

    case IONIC_CMD_Q_CONTROL:
        comp[0] = 0;
        break;

    case IONIC_CMD_PORT_IDENTIFY:
    case IONIC_CMD_PORT_INIT:
    case IONIC_CMD_PORT_RESET:
    case IONIC_CMD_PORT_GETATTR:
    case IONIC_CMD_PORT_SETATTR:
        /* Stub: return success, data zeroed = sane defaults. */
        comp[0] = 0;
        break;

    /* RDMA devcmds 50-53 forwarded to ionic_rdma_devcmd.c */
    case 50:
    case 51:
    case 52:
    case 53:
        handle_rdma_cmd(emu, cmd, comp);
        break;

    default:
        vfu_log(emu->vfu_ctx, LOG_WARNING, "ionic_eth_emu: unknown opcode=%u",
                opcode);
        comp[0] = 1; /* IONIC_RC_ENOSUPP */
        break;
    }

    /* Assert done bit. */
    uint32_t done = le32(IONIC_DEV_CMD_DONE);
    memcpy(cmd_base + DEVCMD_DONE_OFF, &done, 4);
}

/* -------------------------------------------------------------------------
 * IDENTIFY (opcode 1)
 *
 * The driver writes its own identity into data[], then reads the device
 * identity back from data[] after the command completes.  We ignore the
 * driver identity and fill in device identity in data[].
 *
 * ionic_dev_identity layout (union, fits in 512 bytes):
 *   u8  version
 *   u8  type
 *   u8  rsvd[2]
 *   le32 nlifs
 *   le32 nintrs
 *   le32 ndbpages_per_lif
 *   le32 nucasts_per_lif
 *   le32 nmcasts_per_lif
 *   ... (padded to 512 bytes)
 * -------------------------------------------------------------------------
 */
static void handle_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp, uint8_t *data)
{
    (void)cmd;

    /* Protocol: the driver writes drv_identity into data[] before ringing the
     * doorbell, then reads dev_identity from the same data[] region (offset 0)
     * after the command completes.  Both fit in the single data area.  We
     * overwrite data[0..] with the device identity in place.
     * See ionic_identify() in ionic_main.c:
     *   memcpy_fromio(&ident->dev, &idev->dev_cmd_regs->data, sz);  // offset 0
     */

    uint8_t *dev_id = data; /* union ionic_dev_identity at offset 0 */
    memset(dev_id, 0, 512);

    dev_id[0] = 1; /* version */
    dev_id[1] = 0; /* type: IONIC_DEV_TYPE_ENET */

    /* nlifs = 1 (single LIF for eth + RDMA) */
    uint32_t v = le32(1);
    memcpy(dev_id + 4, &v, 4); /* nlifs */

    /* nintrs: report IONIC_MSIX_MAX_VECTORS */
    v = le32(IONIC_MSIX_MAX_VECTORS);
    memcpy(dev_id + 8, &v, 4); /* nintrs */

    /* ndbpages_per_lif: number of doorbell pages we support */
    v = le32(IONIC_EMU_QP_COUNT + 4);
    memcpy(dev_id + 12, &v, 4);

    comp[0] = 0; /* status OK */
    comp[1] = 1; /* version */
}

/* -------------------------------------------------------------------------
 * LIF_IDENTIFY (opcode 20)
 *
 * Returns ionic_lif_identity into data[].  The RDMA section (at byte offset
 * following the eth section) is what ionic_rdma.ko reads to get queue type
 * IDs, page table size, MR count, etc.
 * -------------------------------------------------------------------------
 */
static void handle_lif_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                                uint8_t *comp, uint8_t *data)
{
    (void)cmd;

    /* ionic_lif_identity: 478 * 4 = 1912 bytes, fits in data area. */
    memset(data, 0, 1912);

    /* capabilities: ETH | RDMA */
    uint64_t caps = le64((uint64_t)(IONIC_LIF_CAP_ETH | IONIC_LIF_CAP_RDMA));
    memcpy(data, &caps, 8);

    /* eth section starts at byte 8.
     * ionic_lif_identity.eth layout:
     *   u8  version
     *   u8  rsvd[3]
     *   le32 max_ucast_filters
     *   le32 max_mcast_filters
     *   le16 rss_ind_tbl_sz
     *   le32 min_frame_size
     *   le32 max_frame_size
     *   ...
     *   union ionic_lif_config config  (at offset 120 within eth)
     *
     * We set minimal values so the Ethernet side initialises without errors.
     */
    uint8_t *eth = data + 8;
    eth[0] = 1; /* version */

    uint32_t u;
    u = le32(4);
    memcpy(eth + 4, &u, 4); /* max_ucast_filters */
    u = le32(32);
    memcpy(eth + 8, &u, 4); /* max_mcast_filters */

    uint16_t rss = le16(128);
    memcpy(eth + 12, &rss, 2); /* rss_ind_tbl_sz */

    u = le32(64);
    memcpy(eth + 14, &u, 4); /* min_frame_size */
    u = le32(9216);
    memcpy(eth + 18, &u, 4); /* max_frame_size */

    /* ionic_lif_config embedded in eth at offset 120 within eth section:
     * features le64, queue_count[16] le32 each, name[16], mac[6], ...
     * Leave at zero (driver will set via LIF_SETATTR). */

    /* rdma section offset in union ionic_lif_identity (all packed):
     *   __le64 capabilities = 8
     *   eth (packed struct from ionic_if.h lines 563-576):
     *     u8 version(1) + u8 rsvd[3](3) + le32 max_ucast(4) +
     *     le32 max_mcast(4) + le16 rss_ind_tbl(2) + le32 min_frame(4) +
     *     le32 max_frame(4) + u8 rsvd2[2](2) + le64 hwstamp_tx(8) +
     *     le64 hwstamp_rx(8) + u8 rsvd3[88](88) +
     *     union ionic_lif_config config (words[64] = 256) = 384
     *   total offset = 8 + 384 = 392
     * Verified: rsvd3[88] present in kernel source (ionic_if.h grep confirms).
     * cross-check: words[478] * 4 = 1912 bytes total; 1912 - 392 = 1520 for
     * rdma+pad.
     */
#define LIF_ID_RDMA_OFF 392
    uint8_t *rdma = data + LIF_ID_RDMA_OFF;

    rdma[0] = IONIC_RDMA_VERSION;       /* version       */
    rdma[1] = IONIC_RDMA_QP_OPCODES;    /* qp_opcodes    */
    rdma[2] = IONIC_RDMA_ADMIN_OPCODES; /* admin_opcodes  */
    rdma[3] = 0;                        /* minor_version  */

    u = le32(IONIC_NPTS_PER_LIF);
    memcpy(rdma + 4, &u, 4); /* npts_per_lif */
    u = le32(IONIC_NMRS_PER_LIF);
    memcpy(rdma + 8, &u, 4); /* nmrs_per_lif */
    u = le32(IONIC_NAHS_PER_LIF);
    memcpy(rdma + 12, &u, 4); /* nahs_per_lif */

    rdma[16] = IONIC_MAX_STRIDE;     /* max_stride */
    rdma[17] = 6;                    /* cl_stride (log2 64B cache line) */
    rdma[18] = 3;                    /* pte_stride (log2 8B PTE)        */
    rdma[19] = 6;                    /* rrq_stride                       */
    rdma[20] = 6;                    /* rsq_stride                       */
    rdma[21] = 8;                    /* dcqcn_profiles                   */
    rdma[22] = IONIC_EMU_UDMA_SHIFT; /* udma_shift                  */
    rdma[23] = 2;                    /* rsvd_dimensions (udma_count=2)   */

    uint64_t page_size_cap = le64(IONIC_PAGE_SIZE_CAP);
    memcpy(rdma + 24, &page_size_cap, 8); /* page_size_cap */

    /* ionic_lif_logical_qtype layout (8 bytes each):
     *   u8  qtype    (hardware qtype number)
     *   u8  rsvd[3]
     *   le32 qid_count
     *   le32 qid_base
     *   -- wait, that is 9 bytes; kernel struct is:
     *     u8 qtype; u8 rsvd[3]; le32 qid_count; le32 qid_base; = 12 bytes.
     *
     * From ionic_if.h:
     *   struct ionic_lif_logical_qtype {
     *       u8  qtype;
     *       u8  rsvd[3];
     *       __le32 qid_count;
     *       __le32 qid_base;
     *   };  -- 12 bytes
     */
#define QTYPE_SZ 12
    /* aq_qtype at rdma+32 */
    uint8_t *aq = rdma + 32;
    aq[0] = IONIC_RDMA_QTYPE_AQ;
    u = le32(IONIC_EMU_AQ_COUNT);
    memcpy(aq + 4, &u, 4);
    u = le32(0);
    memcpy(aq + 8, &u, 4);

    /* sq_qtype at rdma+44 */
    uint8_t *sq = rdma + 32 + QTYPE_SZ;
    sq[0] = IONIC_RDMA_QTYPE_SQ;
    u = le32(IONIC_EMU_QP_COUNT);
    memcpy(sq + 4, &u, 4);
    u = le32(0);
    memcpy(sq + 8, &u, 4);

    /* rq_qtype at rdma+56 */
    uint8_t *rq = rdma + 32 + 2 * QTYPE_SZ;
    rq[0] = IONIC_RDMA_QTYPE_RQ;
    u = le32(IONIC_EMU_QP_COUNT);
    memcpy(rq + 4, &u, 4);
    u = le32(0);
    memcpy(rq + 8, &u, 4);

    /* cq_qtype at rdma+68 */
    uint8_t *cq = rdma + 32 + 3 * QTYPE_SZ;
    cq[0] = IONIC_RDMA_QTYPE_CQ;
    u = le32(IONIC_EMU_CQ_COUNT);
    memcpy(cq + 4, &u, 4);
    u = le32(0);
    memcpy(cq + 8, &u, 4);

    /* eq_qtype at rdma+80 */
    uint8_t *eq = rdma + 32 + 4 * QTYPE_SZ;
    eq[0] = IONIC_RDMA_QTYPE_EQ;
    u = le32(IONIC_EMU_EQ_COUNT);
    memcpy(eq + 4, &u, 4);
    u = le32(0);
    memcpy(eq + 8, &u, 4);

    comp[0] = 0; /* status OK */
    comp[1] = 1; /* version   */
}

/* -------------------------------------------------------------------------
 * LIF_INIT (opcode 21)
 * -------------------------------------------------------------------------
 */
static void handle_lif_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp)
{
    /* cmd layout: opcode(1) type(1) index(2) rsvd(4) info_pa(8) rsvd2(48) */
    uint16_t lif_index;
    memcpy(&lif_index, cmd + 2, 2);
    lif_index = le16toh(lif_index);

    /* DMA the ionic_lif_info struct to info_pa if needed.
     * For now we don't map it — driver tolerates a zeroed page. */

    emu->lif_initialized = true;
    emu->lif_hw_index = lif_index;

    comp[0] = 0; /* status OK */
    comp[1] = 0; /* rsvd      */

    /* hw_index in comp[2:3] */
    uint16_t hw = le16(lif_index);
    memcpy(comp + 2, &hw, 2);

    vfu_log(emu->vfu_ctx, LOG_INFO, "ionic_eth_emu: LIF_INIT lif_index=%u",
            lif_index);
}

/* -------------------------------------------------------------------------
 * LIF_SETATTR / LIF_GETATTR (opcodes 24, 23)
 * -------------------------------------------------------------------------
 */
static void handle_lif_setattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp)
{
    (void)emu;
    uint8_t attr = cmd[2];
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: LIF_SETATTR attr=%u",
            attr);
    comp[0] = 0;
}

static void handle_lif_getattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp)
{
    (void)emu;
    uint8_t attr = cmd[2];
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: LIF_GETATTR attr=%u",
            attr);
    comp[0] = 0;
}

/* -------------------------------------------------------------------------
 * Q_IDENTIFY (opcode 39)
 *
 * Returns ionic_q_identity for the requested queue type into data[].
 * The driver uses this to learn WQE stride ranges and speculative SGE counts.
 * -------------------------------------------------------------------------
 */
static void handle_q_identify(struct ionic_eth_emu *emu, const uint8_t *cmd,
                              uint8_t *comp, uint8_t *data)
{
    uint8_t qtype = cmd[3]; /* ionic_logical_qtype */
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: Q_IDENTIFY qtype=%u",
            qtype);

    /* ionic_q_identity layout:
     *   u8  version
     *   u8  supported (max version)
     *   u8  rsvd[2]
     *   le16 max_sg_elems
     *   le16 sg_desc_stride
     *   le16 desc_stride (log2)
     *   ... (64 bytes total)
     *
     * We return minimal safe values. The RDMA driver uses Q_IDENTIFY for
     * EQ, AQ, SQ, RQ, CQ — fill with RDMA-appropriate strides.
     */
    memset(data, 0, 64);
    data[0] = 1; /* version */
    data[1] = 1; /* supported */

    if (qtype == IONIC_RDMA_QTYPE_SQ || qtype == IONIC_RDMA_QTYPE_RQ) {
        uint16_t v = le16(16); /* max_sg_elems */
        memcpy(data + 4, &v, 2);
        v = le16(64); /* sg_desc_stride */
        memcpy(data + 6, &v, 2);
        v = le16(6); /* desc_stride_log2 = 64 bytes */
        memcpy(data + 8, &v, 2);
    } else {
        uint16_t v = le16(64);
        memcpy(data + 6, &v, 2);
        v = le16(6);
        memcpy(data + 8, &v, 2);
    }

    comp[0] = 0;
    comp[1] = 1; /* version */
}

/* -------------------------------------------------------------------------
 * Q_INIT (opcode 40)
 *
 * The driver calls this to initialise admin queue, notifyq, and (optionally)
 * rxq/txq.  We stub it: return success so the driver proceeds to call
 * ionic_auxbus_register().
 * -------------------------------------------------------------------------
 */
static void handle_q_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
                          uint8_t *comp)
{
    /* ionic_q_init_cmd layout (__packed):
     *   [0]   u8  opcode
     *   [1]   u8  rsvd
     *   [2:3] le16 lif_index
     *   [4]   u8  type   ← logical queue type
     *   [5]   u8  ver
     *   [6:7] u8  rsvd1[2]
     *   [8:11]le32 index  ← (lif, qtype) relative queue index
     *   [12:13]le16 pid   ← doorbell page id
     */
    uint8_t qtype = cmd[4]; /* type field */
    uint32_t index;
    memcpy(&index, cmd + 8, 4);
    index = le32toh(index);

    vfu_log(emu->vfu_ctx, LOG_INFO, "ionic_eth_emu: Q_INIT qtype=%u index=%u",
            qtype, index);

    /* ionic_q_init_comp layout:
     *   [0]   u8   status
     *   [1]   u8   rsvd
     *   [2:3] le16 comp_index   (descriptor ring index for this completion)
     *   [4:7] le32 hw_index     (hardware queue ID; driver stores as
     * q->hw_index and uses it for doorbell base address in BAR2) [8]   u8
     * hw_type      (hardware queue type for doorbell slot selection) [9:14]u8
     * rsvd2[6] [15]  u8   color
     *
     * hw_index: echo back the software queue index; the driver maps
     * doorbells as BAR2 + (kern_pid * PAGE_SIZE) + qtype * 8.  For the
     * Ethernet admin path, the exact hw_index is not critical since
     * ionic_rdma.ko handles the RDMA doorbell page independently.
     */
    comp[0] = 0; /* status OK */
    comp[1] = 0; /* rsvd      */
    uint16_t comp_index = 0;
    memcpy(comp + 2, &comp_index, 2);
    uint32_t hw_index = htole32(index);
    memcpy(comp + 4, &hw_index, 4); /* hw_index as le32 */
    comp[8] = qtype;                /* hw_type  */
}

/* -------------------------------------------------------------------------
 * RDMA devcmds (opcodes 50-53) — forward to ionic_rdma_devcmd.c
 * -------------------------------------------------------------------------
 */
static void handle_rdma_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp)
{
    if (!emu->rdma_devcmd_fn) {
        vfu_log(emu->vfu_ctx, LOG_WARNING,
                "ionic_eth_emu: RDMA devcmd opcode=%u but no handler", cmd[0]);
        comp[0] = 0; /* succeed silently so driver probes further */
        return;
    }
    emu->rdma_devcmd_fn(emu->rdma_devcmd_opaque, cmd, comp);
}

/* -------------------------------------------------------------------------
 * Trigger an MSI-X interrupt vector (called by RDMA layer to signal EQ).
 * -------------------------------------------------------------------------
 */
int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    if (vec < 0 || vec >= IONIC_MSIX_MAX_VECTORS)
        return -EINVAL;
    if (emu->intr_mask[vec])
        return 0; /* masked */
    return vfu_irq_trigger(emu->vfu_ctx, (uint32_t)vec);
}
