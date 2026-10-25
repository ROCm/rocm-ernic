/*
 * ionic_eth_emu_priv.h — ionic Ethernet emulator internals, for its unit tests
 *
 * The BAR0 register and dev_cmd layout, the device command and identity
 * constants, and the emulator state that ionic_eth_emu.c works with. Only
 * ionic_eth_emu.c and its unit tests include this; the rest of the device
 * sees struct ionic_eth_emu only through the opaque declaration in
 * ionic_eth_emu.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ETH_EMU_PRIV_H
#define IONIC_ETH_EMU_PRIV_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_emu_stats.h"
#include "ionic_eth_emu.h"
#include "ionic_eth_net.h"
#include "ionic_profile.h"
#include "rocm_ernic_compat.h"
#include "rocm_ernic_internal.h"

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

/* struct ionic_port_info, as the driver lays it out in the DMA area whose
 * address arrives in PORT_INIT.  Offsets rather than a struct because only
 * these fields are published; the sprom and statistics blocks that follow
 * are left as the driver allocated them.
 *
 *   union ionic_port_config   config;   0   (__le32 words[64])
 *   struct ionic_port_status  status;   256 (__packed)
 *       __le32 id;                      256
 *       __le32 speed;                   260
 *       u8     status;                  264
 *       __le16 link_down_count;         265
 *       u8     fec_type;                267
 *       u8     rsvd[48];                268
 *       struct ionic_xcvr_status xcvr;  316
 *           u8     state;               316
 *           u8     phy;                 317
 *           __le16 pid;                 318
 */
#define IONIC_PORT_INFO_CONFIG_SPEED_OFF  0u
#define IONIC_PORT_INFO_CONFIG_MTU_OFF    4u
#define IONIC_PORT_INFO_CONFIG_STATE_OFF  8u
#define IONIC_PORT_INFO_STATUS_SPEED_OFF  260u
#define IONIC_PORT_INFO_STATUS_STATUS_OFF 264u
#define IONIC_PORT_INFO_XCVR_STATE_OFF    316u
#define IONIC_PORT_INFO_XCVR_PHY_OFF      317u
#define IONIC_PORT_INFO_XCVR_PID_OFF      318u
/* Everything above the highest field written, so one DMA write covers it. */
#define IONIC_PORT_INFO_PUBLISHED_LEN 320u

#define IONIC_PORT_ADMIN_STATE_UP 2
#define IONIC_XCVR_STATE_REMOVED  0
#define IONIC_XCVR_STATE_INSERTED 1
#define IONIC_PHY_TYPE_NONE       0
#define IONIC_PHY_TYPE_COPPER     1
#define IONIC_PHY_TYPE_FIBER      2
/* enum ionic_xcvr_pid: the part ids xcvr_pid() picks from. */
#define IONIC_XCVR_PID_UNKNOWN          0
#define IONIC_XCVR_PID_QSFP_100G_CR4    1
#define IONIC_XCVR_PID_QSFP_40GBASE_CR4 2
#define IONIC_XCVR_PID_SFP_25GBASE_CR_S 3
#define IONIC_XCVR_PID_QSFP_50G_CR2     7
#define IONIC_XCVR_PID_QSFP_200G_CR4    8
#define IONIC_XCVR_PID_QSFP_400G_CR4    9
#define IONIC_XCVR_PID_QSFP_100G_SR4    52
#define IONIC_XCVR_PID_QSFP_40GBASE_SR4 56
#define IONIC_XCVR_PID_SFP_25GBASE_SR   59
#define IONIC_XCVR_PID_SFP_10GBASE_SR   63
#define IONIC_XCVR_PID_SFP_10GBASE_CU   68
#define IONIC_XCVR_PID_SFP_1000BASE_T   73
#define IONIC_XCVR_PID_QSFP_200G_SR4    77
#define IONIC_XCVR_PID_QSFP_400G_SR4    81
/* port_info config.mtu; the driver only logs it.  The MTU the guest can set
 * comes from the LIF identity max_frame_size (link.mtu_max). */
#define IONIC_ETH_MTU         9000
#define IONIC_CMD_LIF_RESET   22
#define IONIC_CMD_LIF_GETATTR 23
#define IONIC_CMD_LIF_SETATTR 24
#define IONIC_CMD_Q_IDENTIFY  39
#define IONIC_CMD_Q_INIT      40
#define IONIC_CMD_Q_CONTROL   41

/* ionic_q_control_cmd.oper; IONIC_Q_HANG_RESET (2) is acknowledged as is */
#define IONIC_Q_DISABLE 0
#define IONIC_Q_ENABLE  1

/* LIF capabilities */
#define IONIC_LIF_CAP_ETH  (1u << 0)
#define IONIC_LIF_CAP_RDMA (1u << 1)
/*
 * Not an upstream bit.  ionic_if.h defines only ETH and RDMA
 * (enum ionic_lif_capability), so bit 2 is free, and a patched guest
 * driver uses it to decide whether to create the P4 auxiliary device.
 * Advertised only when the operator enabled the engine AND granted the
 * guest a scope, so a guest sees the control surface exactly when there
 * is one to use, and real hardware -- which never sets it -- is
 * unaffected by a driver carrying the patch.
 */
#define IONIC_LIF_CAP_P4 (1u << 2)

/* (Ethernet logical queue type IDs — kept only for documentation purposes) */

/* RDMA queue type hardware IDs (what the RDMA driver sees) */
#define IONIC_RDMA_QTYPE_AQ 5
#define IONIC_RDMA_QTYPE_SQ 6
#define IONIC_RDMA_QTYPE_RQ 7
#define IONIC_RDMA_QTYPE_CQ 8
#define IONIC_RDMA_QTYPE_EQ 9
/* Next free logical qtype. The driver takes this value from the identity
 * rather than assuming one, so the number is ours to choose; it only has to
 * not collide with the five above. */
#define IONIC_RDMA_QTYPE_SRQ 10

/* Offset of the rdma section in union ionic_lif_identity (all packed):
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

/* Byte offsets within the rdma section of union ionic_lif_identity. Literal
 * because srq_qtype does not follow the other qtypes contiguously -- see the
 * comment at the write site. */
#define LIF_ID_RDMA_STATS_TYPE_OFF 92
#define LIF_ID_SRQ_QTYPE_OFF       96
#define LIF_ID_ALLOC_QID_CAP_OFF   113

/* enum ionic_lif_rdma_alloc_qid: which queue types firmware allocates qids
 * for. CQ is BIT(0) and is not claimed here -- CREATE_CQ still takes the
 * driver's cqid. */
#define IONIC_LIF_RDMA_ALLOC_QID_SRQ (1u << 1)

/* rdma.stats_type: enum ionic_lif_rdma_stat_type in ionic_if.h.  Without a
 * bit set the driver never sends STATS_HDRS / QP_STATS_HDRS, so the guest
 * gets no hw_counters directory and no rdma statistic qp counters. */
#define IONIC_LIF_RDMA_STAT_GLOBAL 0x1u
#define IONIC_LIF_RDMA_STAT_QP     0x2u

/* Emulated RDMA capability version (must match ionic_fw.h expectations) */
#define IONIC_RDMA_VERSION    1
#define IONIC_RDMA_QP_OPCODES 16
/*
 * Highest admin opcode supported, plus one. The driver gates each command on
 * this watermark rather than on a per-opcode bitmap:
 *
 *   if (dev->lif_cfg.admin_opcodes <= IONIC_V1_ADMIN_CREATE_SRQ)
 *           return -EOPNOTSUPP;
 *
 * so 19 (the old IONIC_V1_ADMIN_OPCODES_MAX) refuses CREATE_SRQ=26 ..
 * DESTROY_SRQ=29 and the guest reports no SRQ no matter what the identity
 * advertises. 30 is the new OPCODES_MAX.
 *
 * This also nominally claims 19-25, which upstream has left unassigned for
 * series still in flight. That is safe only because unassigned means the
 * driver never emits them; revisit when those land, since claiming an opcode
 * we do not handle is how a guest command hangs rather than fails.
 */
#define IONIC_RDMA_ADMIN_OPCODES 30

/* Page table and MR counts for emulated device */
/* 2M page table entries. This is the knob that sets the guest's maximum
 * memory-region size, and it is not obvious from here, so: the driver does
 *
 *   attr->max_mr_size = dev->lif_cfg.npts_per_lif * PAGE_SIZE / 2;
 *   (drivers/infiniband/hw/ionic/ionic_ibdev.c:37)
 *
 * -- an explicit halving, presumably for page-table headroom. At the previous
 * (1u << 20) that reported 1048576 * 4096 / 2 = 0x80000000, i.e. 2 GiB, which
 * reads like something truncating a 4 GiB constant and is not: it is this
 * formula working as intended. 1u << 21 reports 4 GiB.
 *
 * Nothing is allocated from this value on either side -- the emulator uses it
 * only in the LIF_IDENTIFY response below, and the driver uses it only for
 * max_mr_size and max_fast_reg_page_list_len (ionic_ibdev.c:37,74) -- so
 * raising it costs no memory.
 *
 * It matters because LMCache registers its whole L1 arena as ONE memory
 * region (nixl_impl.py:341), so max_mr_size is a hard ceiling on per-node L1.
 *
 * WHY SO LARGE. Real HCAs advertise max_mr_size as effectively unbounded
 * (ConnectX-class parts report UINT64_MAX) and let pinnable host memory be
 * the real limit. A small advertised cap is an emulation artifact that
 * surfaces as a mystifying ENOMEM from ibv_reg_mr long before the host is
 * anywhere near out of memory, so match the hardware convention and let
 * memory do the limiting.
 *
 * 1u << 31 reports 2^31 * 4096 / 2 = 2^42 = 4 TiB. The arithmetic is safe:
 * npts_per_lif is u32 and PAGE_SIZE is unsigned long, so the product is done
 * in 64 bits, and max_fast_reg_page_list_len (npts/2 = 2^30) still fits the
 * u32 it is assigned to. A literal 0xFFFFFFFF is avoided deliberately -- it
 * would be ~8 TiB but reads like a sentinel.
 *
 * MAX_MR_SIZE in src/qemu/rdma_rm_defs.h (1UL << 32) does NOT constrain
 * this: it is referenced only by rocm_ernic_compat.c's pvrdma dev_attr,
 * which the ionic guest driver never reads, and no registration path
 * validates against it.
 */
#define IONIC_NPTS_PER_LIF  (1u << 31) /* 2G PTEs -> 4 TiB max_mr_size */
#define IONIC_NMRS_PER_LIF  (1u << 17) /* 128K MRs              */
#define IONIC_NAHS_PER_LIF  (1u << 15) /* 32K AHs               */
#define IONIC_MAX_STRIDE    9          /* log2(512) bytes/WQE   */
#define IONIC_PAGE_SIZE_CAP (1u << 12) /* 4K pages supported     */

/* Number of emulated AQs we report in LIF identity; the EQ count is the
 * profile's queues.rdma_eqs. */
#define IONIC_EMU_AQ_COUNT   4
#define IONIC_EMU_QP_COUNT   (1u << 15)
#define IONIC_EMU_CQ_COUNT   (1u << 16)
#define IONIC_EMU_UDMA_SHIFT 3 /* 8 queues per group */

/* Room for the most Ethernet Tx/Rx queue pairs a profile can offer; the
 * LIF advertises the profile's queues.eth of them. */
#define IONIC_EMU_ETH_QMAX IONIC_PROFILE_ETH_QUEUES_MAX

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

/* struct ionic_txq_desc / ionic_rxq_desc / their completions are all 16 B. */
#define ETH_DESC_SIZE 16u
#define ETH_COMP_SIZE 16u

/* struct ionic_txq_sg_elem: le64 addr, le16 len, le16 rsvd[3]. */
#define TXQ_SG_ELEM_SIZE 16u
#define TXQ_SG_MAX_ELEMS 8u

/* encode_txq_desc_cmd(): flags[3:0] opcode[7:4] nsge[11:8] addr[63:12]. */
#define TXQ_DESC_NSGE_SHIFT  8
#define TXQ_DESC_ADDR_SHIFT  12
#define TXQ_DESC_NIBBLE_MASK 0xfu

/* struct ionic_intr is 32 bytes: coal_init, mask, credits, mask_assert, coal.
 * mask and mask_assert take IONIC_INTR_MASK_SET/CLEAR; a write to credits
 * with the UNMASK bit is how NAPI re-arms the vector. */
#define INTR_REG_STRIDE      32
#define INTR_MASK_OFF        4
#define INTR_CREDITS_OFF     8
#define INTR_MASK_ASSERT_OFF 12
#define INTR_CRED_UNMASK     0x10000u

/* Every vector's intr_ctrl block, and its intr_status bit, must sit inside
 * the register window the driver maps. */
_Static_assert(
    IONIC_BAR0_INTR_CTRL_OFFSET + IONIC_MSIX_MAX_VECTORS * INTR_REG_STRIDE <=
        IONIC_BAR0_SIZE,
    "intr_ctrl for every vector must fit in the BAR0 register window");
_Static_assert(IONIC_BAR0_INTR_STATUS_OFFSET + IONIC_MSIX_MAX_VECTORS / 8u <=
                   IONIC_BAR0_INTR_CTRL_OFFSET,
               "intr_status for every vector must end before intr_ctrl");
_Static_assert(IONIC_PROFILE_INTR_VECTORS_MAX <= IONIC_MSIX_MAX_VECTORS,
               "a profile may not ask for more vectors than the device has");

/* Bounds the inbox so a fast mesh peer cannot grow it without limit; the
 * blocking producer stalls above this, which back-pressures the mesh TCP
 * connection the same way the old PVRDMA Rx ring did. */
#define ETH_INBOX_MAX_FRAMES 256

/* Frames queued by ionic_eth_emu_queue_rx_frame(); private to the .c. */
struct eth_inbox_frame;

struct ernic_dma;

/* State captured from Q_INIT for an Ethernet logical queue. */
struct eth_queue {
    bool valid;
    bool enabled; /* Q_CONTROL ENABLE seen since Q_INIT; Rx serves only these */
    uint16_t intr_index;
    uint64_t ring_base;
    uint64_t cq_ring_base;
    uint64_t sg_ring_base;
    uint16_t depth;
    uint16_t head;     /* next descriptor to consume */
    uint16_t prod;     /* last producer index the driver rang for */
    uint16_t cq_index; /* next completion slot to fill */
    uint8_t cq_color;  /* colour bit the driver is currently expecting */
};

struct ionic_eth_emu {
    vfu_ctx_t *vfu_ctx;
    struct ernic_dma *dma; /* main loop thread only, like every DMA */

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

    /* The struct ionic_lif_info LIF_INIT pointed at, or 0.  Cleared by
     * LIF_RESET and device reset: after either the guest may free it, and
     * it must never be written again. */
    uint64_t lif_info_pa;

    /* What struct ionic_lif_stats reports (ionic_emu_stats.h). */
    struct ionic_emu_stats stats;
    /* Publish on the next tick rather than waiting out the interval: set
     * by LIF_INIT so a new info block is filled in straight away. */
    bool stats_publish_now;
    /* When the stats block was last written, by the caller's clock. */
    uint64_t stats_last_ms;
    /* The last write failed and was logged; log again only after one works. */
    bool stats_write_failed;

    /* The struct ionic_port_info PORT_INIT pointed at, or 0.  Cleared by
     * PORT_RESET and device reset, for the same reason as lif_info_pa. */
    uint64_t port_info_pa;
    /* As stats_write_failed, for the port block. */
    bool port_stats_write_failed;

    /* RDMA devcmd handler (registered after construction). */
    ionic_rdma_devcmd_fn_t rdma_devcmd_fn;
    void *rdma_devcmd_opaque;

    /* P4 devcmd handler (opcodes 70-72), NULL unless P4 is built and the
     * operator granted the guest a scope. */
    ionic_rdma_devcmd_fn_t p4_devcmd_fn;
    void *p4_devcmd_opaque;

    /* Statistics sink, shared with the pvrdma resource manager. */
    pvrdma_handle_t pvrdma_handle;

    /* Data-path handler for BAR2 doorbell writes. */
    struct ionic_datapath *dp;
    /* Admin queue context for AQ doorbell producer-index updates. */
    struct ionic_adminq_ctx *adminq;

    /* Interrupt controller shadow (per-vector: mask, mask-on-assert). */
    uint32_t intr_mask[IONIC_MSIX_MAX_VECTORS];
    uint32_t intr_mask_assert[IONIC_MSIX_MAX_VECTORS];
    /* Set when a vector asserted while masked; replayed on unmask. */
    uint32_t intr_pending[IONIC_MSIX_MAX_VECTORS];

    /* Ethernet logical queues, indexed by [IONIC_QTYPE_*][queue index]. */
    struct eth_queue eth_q[IONIC_QTYPE_ETH_MAX][IONIC_EMU_ETH_QMAX];

    /* Station MAC reported by LIF_IDENTIFY. */
    uint8_t mac[6];

    /* Identity and link shape (ionic_profile.h); compat unless set. */
    struct ionic_profile profile;

    /* Host network backend, or NULL when Tx is a sink. */
    struct ionic_eth_net *net;
    /* In-process endpoint offered every Tx frame before the backend. */
    ionic_eth_tx_filter_fn tx_filter;
    void *tx_filter_ctx;
    /* Injected faults (ionic_fault.h), or NULL; owned by the caller. */
    struct ionic_fault *rx_fault;
    struct ionic_fault *tx_fault;
    /* In-process endpoint offered every Rx frame before the guest. */
    ionic_eth_rx_filter_fn rx_filter;
    void *rx_filter_ctx;
    /* Staging buffer for one frame in either direction. */
    uint8_t frame[IONIC_ETH_NET_MTU_MAX];

    /*
     * Frames handed over by threads that may not DMA -- today only the TCP
     * mesh receive thread.  Producers append under @inbox_lock; the main
     * loop drains the queue from ionic_eth_emu_poll_rx().
     */
    struct eth_inbox_frame *inbox_head;
    struct eth_inbox_frame *inbox_tail;
    unsigned inbox_count;
    pthread_mutex_t inbox_lock;
};

/* The width of the names in the stats file's "LIF Statistics" section: the
 * longest ionic_if.h name, rdma_resp_rx_out_of_atomic_resource, is 35. */
#define LIF_STAT_NAME_WIDTH 35

/* The same, for the "Port Statistics" section: the longest port name,
 * frames_rx_less_than_64b, is 23. */
#define PORT_STAT_NAME_WIDTH 23

/*
 * Device command handlers, each given the 64-byte command, the 16-byte
 * completion to fill in, and, for the commands that return data, the
 * dev_cmd data window. process_devcmd() in ionic_eth_emu.c calls them when
 * the driver rings the dev_cmd doorbell.
 */
void ionic_eth_emu_handle_identify(const struct ionic_eth_emu *emu,
                                   const uint8_t *cmd, uint8_t *comp,
                                   uint8_t *data);
void ionic_eth_emu_handle_port_init(struct ionic_eth_emu *emu,
                                    const uint8_t *cmd, uint8_t *comp);
void ionic_eth_emu_handle_lif_identify(struct ionic_eth_emu *emu,
                                       const uint8_t *cmd, uint8_t *comp,
                                       uint8_t *data);
void ionic_eth_emu_handle_lif_init(struct ionic_eth_emu *emu,
                                   const uint8_t *cmd, uint8_t *comp);
void ionic_eth_emu_handle_q_identify(const struct ionic_eth_emu *emu,
                                     const uint8_t *cmd, uint8_t *comp,
                                     uint8_t *data);
void ionic_eth_emu_handle_q_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
                                 uint8_t *comp);

/*
 * Run one command from the Ethernet adminq ring: Q_INIT, Q_CONTROL, the RDMA
 * opcodes and the LIF attributes, filling in the 16-byte completion.
 */
void ionic_eth_emu_process_adminq_cmd(struct ionic_eth_emu *emu,
                                      const uint8_t *cmd, uint8_t *comp);

/*
 * The Rx queues frames may land on: initialized and enabled, in index order.
 * Returns how many were written to @out.
 */
uint32_t ionic_eth_emu_rx_active(const struct ionic_eth_emu *emu,
                                 uint8_t out[IONIC_EMU_ETH_QMAX]);

/* The Rx queue index @frame steers to, one of the @n entries of @active. */
uint8_t ionic_eth_emu_rx_steer(const uint8_t *active, uint32_t n,
                               const uint8_t *frame, size_t len);

#endif /* IONIC_ETH_EMU_PRIV_H */
