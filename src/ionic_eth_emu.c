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
#include <sys/mman.h>

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
#define IONIC_BAR0_DEV_INFO_REGS_OFFSET 0x0000u
#define IONIC_BAR0_DEV_CMD_REGS_OFFSET  0x0800u
#define IONIC_BAR0_INTR_STATUS_OFFSET   0x1000u
#define IONIC_BAR0_INTR_CTRL_OFFSET     0x2000u
#define IONIC_BAR0_SIZE                 0x8000u /* 32 KB */

/* dev_info_regs field offsets (union ionic_dev_info_regs in ionic_if.h).
 * fw_version is at +0x0c, NOT +0x08 -- +0x08 is fw_heartbeat, which the
 * driver's watchdog polls.  A constant non-zero heartbeat makes
 * ionic_heartbeat_check() report "FW heartbeat stalled" and return
 * -ENXIO, which tears the LIF down. */
#define DEVINFO_SIGNATURE_OFF    0x00u
#define DEVINFO_VERSION_OFF      0x04u
#define DEVINFO_ASIC_TYPE_OFF    0x05u
#define DEVINFO_ASIC_REV_OFF     0x06u
#define DEVINFO_FW_STATUS_OFF    0x07u
#define DEVINFO_FW_HEARTBEAT_OFF 0x08u
#define DEVINFO_FW_VERSION_OFF   0x0cu /* char[32] */
#define DEVINFO_SERIAL_NUM_OFF   0x2cu /* char[32] */
#define DEVINFO_FWVERS_BUFLEN    32u
#define DEVINFO_SERIAL_BUFLEN    32u

#define IONIC_DEV_CMD_DONE 0x00000001u

/* Device command opcodes (only those we actually handle are defined here) */
#define IONIC_CMD_NOP             0
#define IONIC_CMD_IDENTIFY        1
#define IONIC_CMD_INIT            2
#define IONIC_CMD_RESET           3
#define IONIC_CMD_GETATTR         4
#define IONIC_CMD_SETATTR         5
#define IONIC_CMD_PORT_IDENTIFY   10
#define IONIC_CMD_PORT_INIT       11
#define IONIC_CMD_PORT_RESET      12
#define IONIC_CMD_PORT_GETATTR    13
#define IONIC_CMD_PORT_SETATTR    14
#define IONIC_CMD_LIF_IDENTIFY    20
#define IONIC_CMD_LIF_INIT        21
#define IONIC_PORT_OPER_STATUS_UP 1
#define IONIC_LIF_INFO_STATUS_OFF 256u
#define IONIC_CMD_LIF_RESET       22
#define IONIC_CMD_LIF_GETATTR     23
#define IONIC_CMD_LIF_SETATTR     24
#define IONIC_CMD_Q_IDENTIFY      39
#define IONIC_CMD_Q_INIT          40
#define IONIC_CMD_Q_CONTROL       41

/* Completion status codes (enum ionic_status_code in ionic_if.h) */
#define IONIC_RC_SUCCESS 0
#define IONIC_RC_EOPCODE 2

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

/* Ethernet Tx/Rx queue pairs offered to the LIF. */
#define IONIC_EMU_ETH_QCOUNT 4

/* -------------------------------------------------------------------------
 * dev_cmd_regs layout, relative to IONIC_BAR0_DEV_CMD_REGS_OFFSET.
 *
 * These are the offsets of union ionic_dev_cmd_regs in ionic_if.h; the
 * driver reaches every field through that struct, so the struct is the
 * contract, not the IONIC_BAR0_DEV_CMD_* macros.
 *
 *   +0x00  doorbell   u32   (w1 triggers cmd processing)
 *   +0x04  done       u32   (bit 0 = 1 when complete)
 *   +0x08  cmd        union ionic_dev_cmd      (words[16], 64 B)
 *   +0x48  comp       union ionic_dev_cmd_comp (words[4],  16 B)
 *   +0x58  rsvd[48]
 *   +0x88  data[478]  u32   (1912 B, ends exactly at +0x800)
 *
 * Note IONIC_BAR0_DEV_CMD_DATA_REGS_OFFSET (0x0c00) is vestigial in
 * upstream -- no driver code references it.  The real data window is at
 * dev_cmd_regs+0x88, i.e. absolute BAR0 offset 0x0888.  Placing it at
 * 0x0c00 silently breaks every IDENTIFY-style command.
 * -------------------------------------------------------------------------
 */
#define DEVCMD_DOORBELL_OFF 0x00u
#define DEVCMD_DONE_OFF     0x04u
#define DEVCMD_CMD_OFF      0x08u
#define DEVCMD_COMP_OFF     0x48u
#define DEVCMD_DATA_OFF     0x88u
#define DEVCMD_DATA_SIZE    1912u

/* -------------------------------------------------------------------------
 * Emulator state
 * -------------------------------------------------------------------------
 */

/* Size of the emulated BAR0 shadow buffer (32 KB).
 * This must equal the BAR0 size advertised to the guest in setup_bars(),
 * or the driver gets a window it cannot address. */
#define BAR0_BUF_SIZE IONIC_BAR0_SIZE

/* The dev_cmd data window ends exactly where intr_status begins; a
 * larger data area would silently corrupt the interrupt registers. */
_Static_assert(IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_DATA_OFF +
                       DEVCMD_DATA_SIZE ==
                   IONIC_BAR0_INTR_STATUS_OFFSET,
               "dev_cmd data window must abut intr_status at 0x1000");
_Static_assert(DEVCMD_DATA_OFF == DEVCMD_COMP_OFF + 16u + 48u,
               "dev_cmd data must follow comp[16] + rsvd[48]");
_Static_assert(DEVCMD_COMP_OFF == DEVCMD_CMD_OFF + 64u,
               "dev_cmd comp must follow cmd[64]");

/* Ethernet logical queue types (enum ionic_logical_qtype).  These index both
 * ionic_lif_config.queue_count[] and the LIF's doorbell page, and are
 * disjoint from the RDMA hardware qtypes 5-9 used by ionic_rdma.ko. */
#define IONIC_QTYPE_ADMINQ  0
#define IONIC_QTYPE_NOTIFYQ 1
#define IONIC_QTYPE_RXQ     2
#define IONIC_QTYPE_TXQ     3
#define IONIC_QTYPE_ETH_MAX 5

/* struct ionic_admin_cmd / ionic_admin_comp are fixed-size ring entries. */
#define ADMIN_CMD_SIZE        64u
#define ADMIN_COMP_SIZE       16u
#define ADMIN_COMP_COLOR_MASK 0x80u

/* State captured from Q_INIT for an Ethernet logical queue. */
struct eth_queue {
    bool valid;
    uint16_t intr_index;
    uint64_t ring_base;
    uint64_t cq_ring_base;
    uint16_t depth;
    uint16_t head;     /* next descriptor to consume */
    uint16_t cq_index; /* next completion slot to fill */
    uint8_t cq_color;  /* colour bit the driver is currently expecting */
};

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

    /* Ethernet logical queues, indexed by [IONIC_QTYPE_*][queue index]. */
    struct eth_queue eth_q[IONIC_QTYPE_ETH_MAX][IONIC_EMU_ETH_QCOUNT];
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
static void eth_adminq_service(struct ionic_eth_emu *emu, uint16_t p_index);
static void eth_txq_service(struct ionic_eth_emu *emu, uint32_t qid,
                            uint16_t p_index);
static int eth_dma_rw(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len,
                      bool write);

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
    memcpy(info + DEVINFO_SIGNATURE_OFF, &sig, 4);
    info[DEVINFO_VERSION_OFF] = IONIC_DEV_INFO_VERSION;
    info[DEVINFO_ASIC_TYPE_OFF] = IONIC_ASIC_TYPE_NONE;
    info[DEVINFO_ASIC_REV_OFF] = 0;
    info[DEVINFO_FW_STATUS_OFF] = IONIC_FW_STS_F_RUNNING;

    /* Leave fw_heartbeat at 0: ionic_heartbeat_check() special-cases a
     * zero heartbeat as "early FW with no heartbeat" and treats it as
     * healthy, so we do not need a ticking counter to keep the LIF up. */
    uint32_t hb = 0;
    memcpy(info + DEVINFO_FW_HEARTBEAT_OFF, &hb, 4);

    strncpy((char *)(info + DEVINFO_FW_VERSION_OFF), "rocm-ernic-1.0",
            DEVINFO_FWVERS_BUFLEN);
    strncpy((char *)(info + DEVINFO_SERIAL_NUM_OFF), "rocm-ernic-emulated",
            DEVINFO_SERIAL_BUFLEN);

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

        /* Ethernet logical queues (adminq, notifyq, Rx, Tx) own doorbell
         * slots 0-4; the RDMA hardware qtypes start at 5, so there is no
         * overlap with the data path below.  Only the adminq carries
         * ionic_admin_cmd descriptors — Tx/Rx doorbells point at packet
         * descriptor rings and must not be fed to the command engine. */
        if (qtype >= 0 && qtype < IONIC_QTYPE_ETH_MAX) {
            if (qtype == IONIC_QTYPE_ADMINQ)
                eth_adminq_service(emu, p_index);
            else if (qtype == IONIC_QTYPE_TXQ)
                eth_txq_service(emu, qid, p_index);
            /* Rx and notify descriptors are buffers the driver hands us to
             * fill; there is nothing to complete until traffic arrives. */
            return (ssize_t)count;
        }

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
    uint8_t *data = cmd_base + DEVCMD_DATA_OFF;

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

    case IONIC_CMD_INIT:
    case IONIC_CMD_RESET:
    case IONIC_CMD_GETATTR:
    case IONIC_CMD_SETATTR:
        /* Device-level init/reset/attrs: nothing to configure in the
         * emulator, and a zeroed completion reads back as "no features". */
        comp[0] = IONIC_RC_SUCCESS;
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
        comp[0] = IONIC_RC_EOPCODE;
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
 * union ionic_dev_identity field offsets, from offsetof() against the real
 * ionic_if.h.  Note nlifs is at +0x08, not +0x04: version/type are followed
 * by rsvd[2], nports and rsvd2[3] before the first __le32.
 * -------------------------------------------------------------------------
 */
#define DEVID_VERSION_OFF        0x00u
#define DEVID_TYPE_OFF           0x01u
#define DEVID_NPORTS_OFF         0x04u
#define DEVID_NLIFS_OFF          0x08u
#define DEVID_NINTRS_OFF         0x0cu
#define DEVID_NDBPGS_OFF         0x10u
#define DEVID_INTR_COAL_MULT_OFF 0x14u
#define DEVID_INTR_COAL_DIV_OFF  0x18u
#define DEVID_EQ_COUNT_OFF       0x1cu
/* capabilities is at 0x30; left zero. */

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

    dev_id[DEVID_VERSION_OFF] = 1; /* version */
    dev_id[DEVID_TYPE_OFF] = 0;    /* type: IONIC_DEV_TYPE_ENET */
    dev_id[DEVID_NPORTS_OFF] = 1;

    uint32_t v;
#define PUT32(off, val)                \
    do {                               \
        v = le32(val);                 \
        memcpy(dev_id + (off), &v, 4); \
    } while (0)

    PUT32(DEVID_NLIFS_OFF, 1); /* single LIF for eth + RDMA */

    /* nintrs bounds ionic_lif_size(): it needs 1 (adminq) + nxqs + neqs. */
    PUT32(DEVID_NINTRS_OFF, IONIC_MSIX_MAX_VECTORS);
    PUT32(DEVID_NDBPGS_OFF, IONIC_EMU_QP_COUNT + 4);

    /* ethtool divides by intr_coal_div, so it must not be zero. */
    PUT32(DEVID_INTR_COAL_MULT_OFF, 1);
    PUT32(DEVID_INTR_COAL_DIV_OFF, 1);
    PUT32(DEVID_EQ_COUNT_OFF, IONIC_EMU_EQ_COUNT);
#undef PUT32

    comp[0] = IONIC_RC_SUCCESS;
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

    /* ionic_lif_identity: 478 * 4 = 1912 bytes, fills the data area. */
    memset(data, 0, DEVCMD_DATA_SIZE);

    /* capabilities: ETH | RDMA */
    uint64_t caps = le64((uint64_t)(IONIC_LIF_CAP_ETH | IONIC_LIF_CAP_RDMA));
    memcpy(data, &caps, 8);

    /* Absolute offsets into union ionic_lif_identity, from offsetof() against
     * the real ionic_if.h.  The struct is packed, so nothing here is aligned
     * and every field has to be memcpy'd. */
#define LIFID_ETH_VERSION_OFF    0x008u
#define LIFID_MAX_UCAST_OFF      0x00cu
#define LIFID_MAX_MCAST_OFF      0x010u
#define LIFID_RSS_IND_TBL_SZ_OFF 0x014u
#define LIFID_MIN_FRAME_SIZE_OFF 0x016u
#define LIFID_MAX_FRAME_SIZE_OFF 0x01au
#define LIFID_CONFIG_NAME_OFF    0x08cu
#define LIFID_CONFIG_MTU_OFF     0x09cu
#define LIFID_CONFIG_MAC_OFF     0x0a0u
/* features is at 0x0a8; left zero, no offloads are emulated. */
#define LIFID_CONFIG_QCOUNT_OFF 0x0b0u /* le32 queue_count[16], by qtype */

    uint32_t u;
#define PUT32(off, val)              \
    do {                             \
        u = le32(val);               \
        memcpy(data + (off), &u, 4); \
    } while (0)
#define PUT_QCOUNT(qtype, val) \
    PUT32(LIFID_CONFIG_QCOUNT_OFF + 4u * (qtype), val)

    data[LIFID_ETH_VERSION_OFF] = 1;

    PUT32(LIFID_MAX_UCAST_OFF, 4);
    PUT32(LIFID_MAX_MCAST_OFF, 32);

    uint16_t rss = le16(128);
    memcpy(data + LIFID_RSS_IND_TBL_SZ_OFF, &rss, 2);

    PUT32(LIFID_MIN_FRAME_SIZE_OFF, 64);
    PUT32(LIFID_MAX_FRAME_SIZE_OFF, 9216);

    /* ionic_lif_config.  queue_count[] is not advisory: ionic_lif_size() reads
     * TXQ/RXQ straight out of it and hands the result to alloc_etherdev_mqs(),
     * which returns NULL (-ENOMEM) if it is zero. */
    PUT_QCOUNT(IONIC_QTYPE_ADMINQ, 1);
    PUT_QCOUNT(IONIC_QTYPE_NOTIFYQ, 1);
    PUT_QCOUNT(IONIC_QTYPE_RXQ, IONIC_EMU_ETH_QCOUNT);
    PUT_QCOUNT(IONIC_QTYPE_TXQ, IONIC_EMU_ETH_QCOUNT);

    memcpy(data + LIFID_CONFIG_NAME_OFF, "ernic0", 7);

    PUT32(LIFID_CONFIG_MTU_OFF, 1500);

    static const uint8_t mac[6] = {0x02, 0xa0, 0xd1, 0x00, 0x00, 0x01};
    memcpy(data + LIFID_CONFIG_MAC_OFF, mac, sizeof(mac));

#undef PUT_QCOUNT
#undef PUT32

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

    uint64_t info_pa;
    memcpy(&info_pa, cmd + 8, 8);
    info_pa = le64toh(info_pa);

    /* ionic_link_status_check() reads lif->info->status out of this DMA area
     * and leaves the netdev carrier off unless link_status is
     * IONIC_PORT_OPER_STATUS_UP.  Without a carrier the RDMA port never leaves
     * PORT_DOWN, so publish a permanently-up link here.
     *   struct ionic_lif_info: config[0..255], status at 256
     *   struct ionic_lif_status: link_status at +10, link_speed at +12 */
    if (info_pa) {
        uint8_t status[64] = {0};
        uint16_t up = le16(IONIC_PORT_OPER_STATUS_UP);
        uint32_t speed = le32(100000); /* Mbps */
        memcpy(status + 10, &up, 2);
        memcpy(status + 12, &speed, 4);
        if (eth_dma_rw(emu->vfu_ctx, info_pa + IONIC_LIF_INFO_STATUS_OFF,
                       status, sizeof(status), true) < 0)
            vfu_log(emu->vfu_ctx, LOG_ERR,
                    "ionic_eth_emu: LIF_INIT: link status write to %#lx failed",
                    (unsigned long)(info_pa + IONIC_LIF_INFO_STATUS_OFF));
    }

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

    /* Remaining ionic_q_init_cmd fields (packed): intr_index le16 @14,
     * flags le16 @16, cos @18, ring_size @19 (log2 depth), ring_base le64
     * @20, cq_ring_base le64 @28. */
    uint16_t intr_index;
    memcpy(&intr_index, cmd + 14, 2);
    intr_index = le16toh(intr_index);
    uint8_t ring_size = cmd[19];
    uint64_t ring_base, cq_ring_base;
    memcpy(&ring_base, cmd + 20, 8);
    memcpy(&cq_ring_base, cmd + 28, 8);

    vfu_log(emu->vfu_ctx, LOG_INFO,
            "ionic_eth_emu: Q_INIT qtype=%u index=%u intr=%u depth=%u "
            "ring=%#lx cq=%#lx",
            qtype, index, intr_index, 1u << ring_size,
            (unsigned long)le64toh(ring_base),
            (unsigned long)le64toh(cq_ring_base));

    if (qtype < IONIC_QTYPE_ETH_MAX && index < IONIC_EMU_ETH_QCOUNT &&
        ring_size < 16) {
        struct eth_queue *q = &emu->eth_q[qtype][index];
        *q = (struct eth_queue){
            .valid = true,
            .intr_index = intr_index,
            .ring_base = le64toh(ring_base),
            .cq_ring_base = le64toh(cq_ring_base),
            .depth = (uint16_t)(1u << ring_size),
            /* ionic_cq_init() starts with done_color = 1. */
            .cq_color = 1,
        };
    }

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
 * Ethernet admin queue
 *
 * Unlike the devcmd path, adminq commands are DMA'd: the driver writes
 * 64-byte ionic_admin_cmd descriptors into a ring in guest memory, rings the
 * BAR2 doorbell, and waits on a completion that ionic_adminq_service() only
 * runs from NAPI.  So we must DMA a 16-byte ionic_admin_comp back with the
 * colour bit the driver expects and then raise the queue's MSI-X vector --
 * without the interrupt the driver blocks for DEVCMD_TIMEOUT and gives up.
 * -------------------------------------------------------------------------
 */

static int eth_dma_rw(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len,
                      bool is_write)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg, 1,
                          is_write ? PROT_WRITE : PROT_READ);
    if (ret < 0)
        goto out;

    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0)
        goto out;

    if (is_write) {
        memcpy(iov.iov_base, buf, len);
        vfu_sgl_mark_dirty(vfu_ctx, sg, 1);
    } else {
        memcpy(buf, iov.iov_base, len);
    }
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    ret = 0;
out:
    free(sg);
    return ret;
}

static void process_adminq_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp)
{
    switch (cmd[0]) {
    case IONIC_CMD_Q_INIT:
        handle_q_init(emu, cmd, comp);
        break;

    /* ionic_rdma_devcmd() is a misnomer: RDMA opcodes 50-53 go out over the
     * Ethernet adminq, not the BAR0 devcmd window. */
    case 50:
    case 51:
    case 52:
    case 53:
        handle_rdma_cmd(emu, cmd, comp);
        break;

    default:
        /* Everything else the Ethernet driver posts during bring-up
         * (LIF_SETATTR, RX_MODE_SET, RX_FILTER_ADD, ...) has no state in the
         * emulator, and a zeroed completion reads back as success. */
        vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: adminq opcode=%u",
                cmd[0]);
        comp[0] = IONIC_RC_SUCCESS;
        break;
    }
}

static void eth_adminq_service(struct ionic_eth_emu *emu, uint16_t p_index)
{
    struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_ADMINQ][0];
    if (!q->valid)
        return;
    uint16_t prod = (uint16_t)(p_index % q->depth);

    /* Bounded so a bogus producer index can never spin the server. */
    for (unsigned n = 0; q->head != prod && n < q->depth; n++) {
        uint8_t cmd[ADMIN_CMD_SIZE] = {0};
        uint8_t comp[ADMIN_COMP_SIZE] = {0};

        if (eth_dma_rw(emu->vfu_ctx,
                       q->ring_base + (uint64_t)q->head * ADMIN_CMD_SIZE, cmd,
                       sizeof(cmd), false) < 0) {
            vfu_log(emu->vfu_ctx, LOG_ERR,
                    "ionic_eth_emu: adminq desc DMA read failed at %u",
                    q->head);
            return;
        }

        process_adminq_cmd(emu, cmd, comp);

        uint16_t comp_index = le16(q->head);
        memcpy(comp + 2, &comp_index, 2);
        comp[ADMIN_COMP_SIZE - 1] =
            q->cq_color ? (uint8_t)ADMIN_COMP_COLOR_MASK : 0;

        eth_dma_rw(emu->vfu_ctx,
                   q->cq_ring_base + (uint64_t)q->cq_index * ADMIN_COMP_SIZE,
                   comp, sizeof(comp), true);

        q->head = (uint16_t)((q->head + 1) % q->depth);
        if (++q->cq_index == q->depth) {
            q->cq_index = 0;
            q->cq_color ^= 1;
        }
    }

    ionic_eth_emu_trigger_irq(emu, q->intr_index);
}

/* The loopback backend has no wire to put frames on, so Tx is a sink: every
 * descriptor the driver posts is completed immediately.  Without this the
 * netdev watchdog fires every five seconds and resets the queues. */
static void eth_txq_service(struct ionic_eth_emu *emu, uint32_t qid,
                            uint16_t p_index)
{
    if (qid >= IONIC_EMU_ETH_QCOUNT)
        return;

    struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][qid];
    if (!q->valid)
        return;

    uint16_t prod = (uint16_t)(p_index % q->depth);

    for (unsigned n = 0; q->head != prod && n < q->depth; n++) {
        /* struct ionic_txq_comp: status @0, comp_index le16 @2, colour @15. */
        uint8_t comp[ADMIN_COMP_SIZE] = {0};
        uint16_t comp_index = le16(q->head);
        memcpy(comp + 2, &comp_index, 2);
        comp[ADMIN_COMP_SIZE - 1] =
            q->cq_color ? (uint8_t)ADMIN_COMP_COLOR_MASK : 0;

        eth_dma_rw(emu->vfu_ctx,
                   q->cq_ring_base + (uint64_t)q->cq_index * ADMIN_COMP_SIZE,
                   comp, sizeof(comp), true);

        q->head = (uint16_t)((q->head + 1) % q->depth);
        if (++q->cq_index == q->depth) {
            q->cq_index = 0;
            q->cq_color ^= 1;
        }
    }

    ionic_eth_emu_trigger_irq(emu, q->intr_index);
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
