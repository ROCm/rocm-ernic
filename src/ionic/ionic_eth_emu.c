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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_dma.h"
#include "ernic_limits.h"
#include "ernic_stats.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_eth_emu_priv.h"
#include "ionic/ionic_emu_stats.h"
#include "ionic/ionic_eth_net.h"
#include "ionic/ionic_eth_steer.h"
#include "ionic/ionic_profile.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_status.h"
#include "ionic/ionic_fault.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"

/* The data path refuses a WQE stride above the max_stride advertised here,
 * and sizes its WQE buffers for it. */
_Static_assert(IONIC_MAX_STRIDE == IONIC_WQE_MAX_STRIDE_LOG2,
               "advertised max_stride must match the data path's");

struct eth_inbox_frame {
    struct eth_inbox_frame *next;
    size_t len;
    /* The eth_rx fault has seen it: a frame held back for want of a buffer
     * is one event, however many polls it waits. */
    bool fault_seen;
    bool port_counted; /* the MAC has seen it; it waits for a buffer */
    uint8_t data[];
};

/* The Ethernet queue pairs the LIF offers: queues.eth, which the profile
 * loader keeps inside eth_q[]. */
static uint32_t eth_qcount(const struct ionic_eth_emu *emu)
{
    uint32_t n = emu->profile.queues.eth;

    return n <= IONIC_EMU_ETH_QMAX ? n : IONIC_EMU_ETH_QMAX;
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------
 */
static void process_devcmd(struct ionic_eth_emu *emu);
static void handle_lif_setattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp);
static void handle_lif_getattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp);
static void handle_q_control(struct ionic_eth_emu *emu, const uint8_t *cmd,
                             uint8_t *comp);
static void handle_rdma_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                            uint8_t *comp);
static void handle_p4_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                          uint8_t *comp);
static void eth_adminq_service(struct ionic_eth_emu *emu, uint16_t p_index);
static void eth_txq_service(struct ionic_eth_emu *emu, uint32_t qid,
                            uint16_t p_index);

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

/* Fill dev_info_regs in the BAR0 shadow from the profile's identity.
 * ionic.ko reads signature at offset 0 to confirm the device is alive,
 * and fw_status bit 0 to confirm firmware is running. */
static void write_dev_info(struct ionic_eth_emu *emu)
{
    uint8_t *info = emu->bar0 + IONIC_BAR0_DEV_INFO_REGS_OFFSET;

    ernic_put_le32(info + DEVINFO_SIGNATURE_OFF, IONIC_DEV_INFO_SIGNATURE);
    info[DEVINFO_VERSION_OFF] = IONIC_DEV_INFO_VERSION;
    info[DEVINFO_ASIC_TYPE_OFF] = emu->profile.identity.asic_type;
    info[DEVINFO_ASIC_REV_OFF] = emu->profile.identity.asic_rev;
    info[DEVINFO_FW_STATUS_OFF] = IONIC_FW_STS_F_RUNNING;

    /* Leave fw_heartbeat at 0: ionic_heartbeat_check() special-cases a
     * zero heartbeat as "early FW with no heartbeat" and treats it as
     * healthy, so we do not need a ticking counter to keep the LIF up. */
    uint32_t hb = 0;
    memcpy(info + DEVINFO_FW_HEARTBEAT_OFF, &hb, 4);

    /* fw_version is at most 31 characters, so the zeroed tail always
     * leaves it NUL-terminated. */
    memset(info + DEVINFO_FW_VERSION_OFF, 0, DEVINFO_FWVERS_BUFLEN);
    memcpy(
        info + DEVINFO_FW_VERSION_OFF, emu->profile.identity.fw_version,
        strnlen(emu->profile.identity.fw_version, DEVINFO_FWVERS_BUFLEN - 1));
    strncpy((char *)(info + DEVINFO_SERIAL_NUM_OFF), "rocm-ernic-emulated",
            DEVINFO_SERIAL_BUFLEN);
}

struct ionic_eth_emu *ionic_eth_emu_create(vfu_ctx_t *vfu_ctx, size_t bar2_size)
{
    struct ionic_eth_emu *emu = calloc(1, sizeof(*emu));
    if (!emu)
        return NULL;

    emu->vfu_ctx = vfu_ctx;
    emu->bar2_size = bar2_size;
    memcpy(emu->mac, (const uint8_t[]){0x02, 0xa0, 0xd1, 0x00, 0x00, 0x01}, 6);
    emu->bar2 = calloc(1, bar2_size);
    emu->dma = ernic_dma_create();
    if (!emu->bar2 || !emu->dma) {
        ernic_dma_destroy(emu->dma);
        free(emu->bar2);
        free(emu);
        return NULL;
    }
    qemu_mutex_init(&emu->inbox_lock);

    ionic_profile_defaults(&emu->profile);
    write_dev_info(emu);

    /* Mask all interrupts initially. */
    for (int i = 0; i < IONIC_MSIX_MAX_VECTORS; i++)
        emu->intr_mask[i] = 1;

    return emu;
}

void ionic_eth_emu_set_profile(struct ionic_eth_emu *emu,
                               const struct ionic_profile *profile)
{
    if (!emu || !profile)
        return;
    emu->profile = *profile;
    write_dev_info(emu);
}

void ionic_eth_emu_set_faults(struct ionic_eth_emu *emu,
                              const struct ionic_fault_set *faults)
{
    if (!emu)
        return;
    emu->rx_fault = ionic_fault_set_get(faults, IONIC_FAULT_SITE_ETH_RX);
    emu->tx_fault = ionic_fault_set_get(faults, IONIC_FAULT_SITE_ETH_TX);
}

void ionic_eth_emu_destroy(struct ionic_eth_emu *emu)
{
    if (!emu)
        return;
    ionic_eth_net_close(emu->net);
    /* Like the frees below, this relies on nothing else reaching emu now */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    while (emu->inbox_head) {
        struct eth_inbox_frame *f = emu->inbox_head;
        emu->inbox_head = f->next;
        free(f);
    }
    emu->inbox_tail = NULL;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_mutex_destroy(&emu->inbox_lock);
    ernic_dma_destroy(emu->dma);
    free(emu->bar2);
    free(emu);
}

/* The largest frame the port takes in, FCS excluded: what LIF_IDENTIFY
 * advertises as max_frame_size.  The profile is fixed at create, so any
 * thread may read it. */
static size_t port_max_frame(const struct ionic_eth_emu *emu)
{
    return (size_t)emu->profile.link.mtu_max + IONIC_PROFILE_FRAME_OVERHEAD;
}

int ionic_eth_emu_queue_rx_frame(struct ionic_eth_emu *emu, const void *frame,
                                 size_t len)
{
    if (!emu || !frame || len == 0 || len > IONIC_ETH_NET_MTU_MAX)
        return -EINVAL;

    struct eth_inbox_frame *f = malloc(sizeof(*f) + len);
    if (!f)
        return -ENOMEM;
    f->next = NULL;
    f->len = len;
    f->fault_seen = false;
    f->port_counted = false;
    memcpy(f->data, frame, len);

    qemu_mutex_lock(&emu->inbox_lock);
    if (emu->inbox_count >= ETH_INBOX_MAX_FRAMES) {
        qemu_mutex_unlock(&emu->inbox_lock);
        free(f);
        ionic_emu_stats_rx_inbox_full(&emu->stats, frame, len,
                                      port_max_frame(emu));
        return -ENOSPC;
    }
    if (emu->inbox_tail)
        emu->inbox_tail->next = f;
    else
        emu->inbox_head = f;
    emu->inbox_tail = f;
    emu->inbox_count++;
    qemu_mutex_unlock(&emu->inbox_lock);

    return 0;
}

int ionic_eth_emu_send_to_net(const struct ionic_eth_emu *emu,
                              const void *frame, size_t len)
{
    if (!emu || !frame || len == 0 || len > IONIC_ETH_NET_MTU_MAX)
        return -EINVAL;
    if (!emu->net)
        return -ENODEV;

    /*
     * Deliberately uncounted. eth_txq_service() already counts a guest
     * frame as sent whether or not the Tx filter took it, so counting
     * again here doubled every byte an in-process endpoint put back on
     * the wire. And this cannot tell guest traffic from anything else:
     * a frame the pipeline reflects from the uplink back to the uplink
     * never came from the guest, and the counter is of guest bytes.
     * Only the caller that knows where the frame came from can count it.
     */
    return ionic_eth_net_send(emu->net, frame, len);
}

void ionic_eth_emu_register_rdma_handler(struct ionic_eth_emu *emu,
                                         ionic_rdma_devcmd_fn_t fn,
                                         void *opaque)
{
    emu->rdma_devcmd_fn = fn;
    emu->rdma_devcmd_opaque = opaque;
}

void ionic_eth_emu_register_p4_handler(struct ionic_eth_emu *emu,
                                       ionic_rdma_devcmd_fn_t fn, void *opaque)
{
    emu->p4_devcmd_fn = fn;
    emu->p4_devcmd_opaque = opaque;
}

void ionic_eth_emu_register_datapath(struct ionic_eth_emu *emu,
                                     struct ionic_datapath *dp)
{
    emu->dp = dp;
}

void ionic_eth_emu_register_tx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_tx_filter_fn fn, void *ctx)
{
    emu->tx_filter = fn;
    emu->tx_filter_ctx = ctx;
}

void ionic_eth_emu_get_tx_filter(const struct ionic_eth_emu *emu,
                                 ionic_eth_tx_filter_fn *fn, void **ctx)
{
    if (fn)
        *fn = emu ? emu->tx_filter : NULL;
    if (ctx)
        *ctx = emu ? emu->tx_filter_ctx : NULL;
}

void ionic_eth_emu_register_rx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_rx_filter_fn fn, void *ctx)
{
    emu->rx_filter = fn;
    emu->rx_filter_ctx = ctx;
}

void ionic_eth_emu_register_adminq(struct ionic_eth_emu *emu,
                                   struct ionic_adminq_ctx *adminq)
{
    emu->adminq = adminq;
}

void ionic_eth_emu_set_stats(struct ionic_eth_emu *emu,
                             struct ernic_stats *stats)
{
    emu->stats_sink = stats;
}

void ionic_eth_emu_set_mac(struct ionic_eth_emu *emu, const uint8_t mac[6])
{
    memcpy(emu->mac, mac, 6);
}

/* -------------------------------------------------------------------------
 * Deliver one MSI-X assertion on an unmasked vector.  Callers must have
 * established that the vector is in range and currently unmasked.
 * -------------------------------------------------------------------------
 */
static int deliver_irq(struct ionic_eth_emu *emu, int vec)
{
    emu->intr_pending[vec] = 0;

    /* Real hardware latches the mask as it asserts when mask_assert is set,
     * so the driver's NAPI poll runs without a second interrupt racing it. */
    if (emu->intr_mask_assert[vec])
        emu->intr_mask[vec] = 1;

    int ret = vfu_irq_trigger(emu->vfu_ctx, (uint32_t)vec);
    if (!ret)
        ernic_stats_irq(emu->stats_sink);

    return ret;
}

/* Replay an assertion that arrived while the vector was masked. */
static void unmask_irq(struct ionic_eth_emu *emu, int vec)
{
    emu->intr_mask[vec] = 0;
    if (emu->intr_pending[vec])
        deliver_irq(emu, vec);
}

/* -------------------------------------------------------------------------
 * BAR0 access callback
 *
 * The ionic driver accesses BAR0 as 32-bit MMIO registers.  We maintain a
 * shadow buffer and process commands when the doorbell DWORD is written.
 * -------------------------------------------------------------------------
 */
/*
 * Does [@offset, @offset + @count) lie inside a region of @size bytes?  The
 * client chooses both, and @offset is signed: converted and added, a
 * negative offset, or a count that carries the sum past SIZE_MAX, wraps to a
 * small total that passes.
 */
static bool region_access_ok(loff_t offset, size_t count, size_t size)
{
    return offset >= 0 && (uint64_t)offset <= size &&
           count <= size - (size_t)offset;
}

ssize_t ionic_eth_emu_bar0_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write)
{
    if (!region_access_ok(offset, count, IONIC_BAR0_REGS_SIZE)) {
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
        uint32_t db = ernic_get_le32(emu->bar0 + doorbell_off);
        if (db) {
            process_devcmd(emu);
            /* Clear doorbell and assert done. */
            uint32_t zero = 0;
            memcpy(emu->bar0 + doorbell_off, &zero, 4);
        }
    }

    /* Interrupt controller writes. */
    if (offset >= (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET && count == 4 &&
        (size_t)offset + count <= IONIC_BAR0_REGS_SIZE) {
        loff_t rel = offset - (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET;
        int vec = (int)(rel / INTR_REG_STRIDE);
        if (vec >= 0 && vec < IONIC_MSIX_MAX_VECTORS) {
            uint32_t val = ernic_get_le32((const uint8_t *)buf);

            switch (rel % INTR_REG_STRIDE) {
            case INTR_MASK_OFF:
                if (val)
                    emu->intr_mask[vec] = val;
                else
                    unmask_irq(emu, vec);
                break;
            case INTR_MASK_ASSERT_OFF:
                emu->intr_mask_assert[vec] = val;
                break;
            case INTR_CREDITS_OFF:
                /* NAPI returns credits with IONIC_INTR_CRED_UNMASK to re-arm
                 * a vector that mask-on-assert disabled.  Ignoring this pins
                 * the mask after the first interrupt and the queue stalls. */
                if (val & INTR_CRED_UNMASK)
                    unmask_irq(emu, vec);
                break;
            default:
                break;
            }
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
    if (!region_access_ok(offset, count, emu->bar2_size)) {
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
     *   [23:16] ring     (0=normal, 1=arm CQ/EQ, 2=arm solicited, 3=SRQ)
     *   [31:24] qid_lo
     *   [47:32] qid_hi
     *   [63:48] reserved
     */
    memcpy(emu->bar2 + offset, buf, count);

    if (count == 8) {
        uint64_t db = ernic_get_le64((const uint8_t *)buf);
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
                "ionic_eth_emu: doorbell BAR2 off=%#" PRIx64 " qtype=%d qid=%u "
                "ring=%u p_index=%u",
                (uint64_t)offset, qtype, qid, ring, p_index);

        /* Ethernet logical queues (adminq, notifyq, Rx, Tx) own doorbell
         * slots 0-4; the RDMA hardware qtypes start at 5, so there is no
         * overlap with the data path below.  Only the adminq carries
         * ionic_admin_cmd descriptors — Tx/Rx doorbells point at packet
         * descriptor rings and must not be fed to the command engine. */
        if (qtype >= 0 && qtype < IONIC_QTYPE_ETH_MAX) {
            if (qtype == IONIC_QTYPE_ADMINQ) {
                eth_adminq_service(emu, p_index);
            } else if (qtype == IONIC_QTYPE_TXQ) {
                eth_txq_service(emu, qid, p_index);
            } else if (qtype == IONIC_QTYPE_RXQ && qid < eth_qcount(emu)) {
                /* Rx descriptors are buffers the driver hands us to fill;
                 * record how many are posted and wait for traffic. */
                struct eth_queue *rq = &emu->eth_q[IONIC_QTYPE_RXQ][qid];
                if (rq->valid)
                    rq->prod = (uint16_t)(p_index % rq->depth);
            }
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
 * Statistics
 *
 * The driver reads struct ionic_lif_stats out of the LIF info block and
 * never asks for it to be refreshed, so the device rewrites it on a timer.
 * -------------------------------------------------------------------------
 */

/*
 * Copy the datapath's RDMA error counters into the LIF statistics block.
 *
 * The datapath owns the events that move these -- it is where a completion
 * error is raised -- and serves them to the guest through the adminq STATS
 * tables, which is what fills `hw_counters`.  struct ionic_lif_stats has
 * slots for the same counters, because real hardware reports them there
 * too, so the LIF block has to be told about them before anything reads
 * it.
 *
 * Without this the `rdma_*` lines in --stats-file, and the same fields in
 * the block DMAed to the guest, read zero for ever while a guest reading
 * hw_counters sees the real values: a measurement that is always wrong,
 * which is worse than one that is absent.
 *
 * Most names match, so the mapping is spelled out rather than derived --
 * four do not, and a silent mismatch here would put a counter in the
 * wrong slot.  Main loop thread only, as ionic_datapath_rdma_counters()
 * requires.
 */
#define IONIC_LIF_RDMA_STAT_MAP(X)                                           \
    X(REQ_RX_PKT_SEQ_ERR, IONIC_LIF_RDMA_REQ_RX_PKT_SEQ_ERR)                 \
    X(REQ_RX_RNR_RETRY_ERR, IONIC_LIF_RDMA_REQ_RX_RNR_RETRY_ERR)             \
    X(REQ_RX_REMOTE_ACCESS_ERR, IONIC_LIF_RDMA_REQ_RX_REMOTE_ACCESS_ERR)     \
    X(REQ_RX_REMOTE_INV_REQ_ERR, IONIC_LIF_RDMA_REQ_RX_REMOTE_INV_REQ_ERR)   \
    X(REQ_RX_REMOTE_OPER_ERR, IONIC_LIF_RDMA_REQ_RX_REMOTE_OPER_ERR)         \
    X(REQ_RX_IMPLIED_NAK_SEQ_ERR, IONIC_LIF_RDMA_REQ_RX_IMPLIED_NAK_SEQ_ERR) \
    X(REQ_RX_CQE_ERR, IONIC_LIF_RDMA_REQ_RX_CQE_ERR)                         \
    X(REQ_RX_CQE_FLUSH_ERR, IONIC_LIF_RDMA_REQ_RX_CQE_FLUSH_ERR)             \
    X(REQ_RX_DUP_RESPONSES, IONIC_LIF_RDMA_REQ_RX_DUP_RESPONSES)             \
    X(REQ_RX_INVALID_PACKETS, IONIC_LIF_RDMA_REQ_RX_INVALID_PACKETS)         \
    X(REQ_TX_LOCAL_ACCESS_ERR, IONIC_LIF_RDMA_REQ_TX_LOCAL_ACCESS_ERR)       \
    X(REQ_TX_LOCAL_OPER_ERR, IONIC_LIF_RDMA_REQ_TX_LOCAL_OPER_ERR)           \
    X(REQ_TX_MEMORY_MGMT_ERR, IONIC_LIF_RDMA_REQ_TX_MEMORY_MGMT_ERR)         \
    X(RESP_RX_DUP_REQUESTS, IONIC_LIF_RDMA_RESP_RX_DUP_REQUESTS)             \
    X(RESP_RX_OUT_OF_BUFFER, IONIC_LIF_RDMA_RESP_RX_OUT_OF_BUFFER)           \
    X(RESP_RX_OUT_OF_SEQ_PKTS, IONIC_LIF_RDMA_RESP_RX_OUT_OF_SEQ_PKTS)       \
    X(RESP_RX_CQE_ERR, IONIC_LIF_RDMA_RESP_RX_CQE_ERR)                       \
    X(RESP_RX_CQE_FLUSH_ERR, IONIC_LIF_RDMA_RESP_RX_CQE_FLUSH_ERR)           \
    X(RESP_RX_LOCAL_LEN_ERR, IONIC_LIF_RDMA_RESP_RX_LOCAL_LEN_ERR)           \
    X(RESP_RX_INV_REQUEST_ERR, IONIC_LIF_RDMA_RESP_RX_INV_REQUEST_ERR)       \
    X(RESP_RX_LOCAL_QP_OPER_ERR, IONIC_LIF_RDMA_RESP_RX_LOCAL_QP_OPER_ERR)   \
    X(RESP_RX_OUT_OF_ATOMIC_RES,                                             \
      IONIC_LIF_RDMA_RESP_RX_OUT_OF_ATOMIC_RESOURCE)                         \
    X(RESP_TX_PKT_SEQ_ERR, IONIC_LIF_RDMA_RESP_TX_PKT_SEQ_ERR)               \
    X(RESP_TX_REMOTE_INV_REQ_ERR, IONIC_LIF_RDMA_RESP_TX_REMOTE_INV_REQ_ERR) \
    X(RESP_TX_REMOTE_ACCESS_ERR, IONIC_LIF_RDMA_RESP_TX_REMOTE_ACCESS_ERR)   \
    X(RESP_TX_REMOTE_OPER_ERR, IONIC_LIF_RDMA_RESP_TX_REMOTE_OPER_ERR)       \
    X(RESP_TX_RNR_RETRY_ERR, IONIC_LIF_RDMA_RESP_TX_RNR_RETRY_ERR)           \
    X(TX_CNP_PKTS, IONIC_LIF_TX_RDMA_CNP_PACKETS)                            \
    X(RX_CNP_PKTS, IONIC_LIF_RX_RDMA_CNP_PACKETS)                            \
    X(RX_ECN_PKTS, IONIC_LIF_RX_RDMA_ECN_PACKETS)

static void refresh_rdma_lif_stats(struct ionic_eth_emu *emu)
{
    uint64_t v[IONIC_DP_RDMA_COUNT];

    if (!emu || !emu->dp)
        return;

    ionic_datapath_rdma_counters(emu->dp, IONIC_DP_DEVICE, v);
#define IONIC_LIF_RDMA_STAT_COPY(dp_id, lif_id) \
    emu->stats.lif[lif_id] = v[IONIC_DP_##dp_id];
    IONIC_LIF_RDMA_STAT_MAP(IONIC_LIF_RDMA_STAT_COPY)
#undef IONIC_LIF_RDMA_STAT_COPY
}

/*
 * Write one statistics block at @base + @off, logging a failure once until a
 * write works.  The guest supplies @base, so a block whose last byte would
 * run past UINT64_MAX is refused rather than written to a wrapped address;
 * the log then names @base, which is the value the guest gave us.
 */
static void publish_block(const struct ionic_eth_emu *emu, const char *what,
                          uint64_t base, uint64_t off, const uint8_t *block,
                          size_t len, bool *failed)
{
    bool address_valid = base <= UINT64_MAX - off - (uint64_t)(len - 1);
    uint64_t pa = address_valid ? base + off : base;

    if (!address_valid ||
        ernic_dma_write(emu->dma, emu->vfu_ctx, pa, block, len) < 0) {
        if (!*failed)
            vfu_log(emu->vfu_ctx, LOG_WARNING,
                    "ionic_eth_emu: %s stats write to %#" PRIx64
                    " failed; retrying every %u ms",
                    what, pa, (unsigned)emu->profile.stats.interval_ms);
        *failed = true;
        return;
    }
    *failed = false;
}

void ionic_eth_emu_publish_stats(struct ionic_eth_emu *emu, uint64_t now_ms)
{
    if (!emu || (!emu->lif_info_pa && !emu->port_info_pa))
        return;
    if (!emu->stats_publish_now &&
        now_ms - emu->stats_last_ms < emu->profile.stats.interval_ms)
        return;
    emu->stats_publish_now = false;
    emu->stats_last_ms = now_ms;

    /* The RDMA error counters live in the datapath; copy them in before
     * the block is rendered, so the guest's copy agrees with hw_counters. */
    refresh_rdma_lif_stats(emu);

    /* fill() writes all of each block, so no stack bytes reach the guest. */
    if (emu->lif_info_pa) {
        uint8_t block[IONIC_LIF_STATS_BYTES];

        ionic_emu_stats_fill(&emu->stats, block);
        publish_block(emu, "LIF", emu->lif_info_pa, IONIC_LIF_INFO_STATS_OFF,
                      block, sizeof(block), &emu->stats_write_failed);
    }
    if (emu->port_info_pa) {
        uint8_t block[IONIC_PORT_STATS_BYTES];

        ionic_emu_port_fill(&emu->stats, block);
        publish_block(emu, "port", emu->port_info_pa, IONIC_PORT_INFO_STATS_OFF,
                      block, sizeof(block), &emu->port_stats_write_failed);
    }
}

const struct ionic_emu_stats *ionic_eth_emu_get_stats(struct ionic_eth_emu *emu)
{
    if (!emu)
        return NULL;
    refresh_rdma_lif_stats(emu);
    ionic_emu_stats_fold(&emu->stats);
    return &emu->stats;
}

void ionic_eth_emu_write_stats(FILE *fp, void *opaque)
{
    const struct ionic_emu_stats *st = ionic_eth_emu_get_stats(opaque);

    if (!fp || !st)
        return;
    fprintf(fp, "LIF Statistics:\n");
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
        const char *name = ionic_emu_lif_stat_name(q);

        /* " : " is what ernic-exporter splits on; keep the space. */
        if (name)
            fprintf(fp, "  %-*s : %" PRIu64 "\n", LIF_STAT_NAME_WIDTH, name,
                    st->lif[q]);
    }
    fprintf(fp, "\n");
    fprintf(fp, "Port Statistics:\n");
    for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
        fprintf(fp, "  %-*s : %" PRIu64 "\n", PORT_STAT_NAME_WIDTH,
                ionic_emu_port_stat_name(q), st->port[q]);
    fprintf(fp, "\n");
}

void ionic_eth_emu_count_rdma(struct ionic_eth_emu *emu, uint64_t bytes,
                              bool is_tx)
{
    if (emu)
        ionic_emu_stats_rdma(&emu->stats, bytes, is_tx);
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
        ionic_eth_emu_handle_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_RESET:
        /* Firmware zeroes its counters on reset, and the guest may free the
         * LIF info block once the device is reset.  The RDMA error
         * counters are the datapath's, and are copied into the LIF block
         * every time it is read, so they are zeroed where they live: the
         * LIF block alone would get its old values back on the next read,
         * and hw_counters would disagree with it. */
        emu->lif_info_pa = 0;
        emu->port_info_pa = 0;
        ionic_emu_stats_reset(&emu->stats);
        ionic_datapath_reset_rdma_counters(emu->dp);
        comp[0] = IONIC_RC_SUCCESS;
        break;

    case IONIC_CMD_INIT:
    case IONIC_CMD_GETATTR:
    case IONIC_CMD_SETATTR:
        /* Device-level init/reset/attrs: nothing to configure in the
         * emulator, and a zeroed completion reads back as "no features". */
        comp[0] = IONIC_RC_SUCCESS;
        break;

    case IONIC_CMD_LIF_IDENTIFY:
        ionic_eth_emu_handle_lif_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_LIF_INIT:
        ionic_eth_emu_handle_lif_init(emu, cmd, comp);
        break;

    case IONIC_CMD_LIF_RESET:
        emu->lif_initialized = false;
        emu->lif_info_pa = 0;
        comp[0] = 0;
        break;

    case IONIC_CMD_LIF_SETATTR:
        handle_lif_setattr(emu, cmd, comp);
        break;

    case IONIC_CMD_LIF_GETATTR:
        handle_lif_getattr(emu, cmd, comp);
        break;

    case IONIC_CMD_Q_IDENTIFY:
        ionic_eth_emu_handle_q_identify(emu, cmd, comp, data);
        break;

    case IONIC_CMD_Q_INIT:
        ionic_eth_emu_handle_q_init(emu, cmd, comp);
        break;

    case IONIC_CMD_Q_CONTROL:
        handle_q_control(emu, cmd, comp);
        break;

    case IONIC_CMD_PORT_INIT:
        ionic_eth_emu_handle_port_init(emu, cmd, comp);
        break;

    case IONIC_CMD_PORT_RESET:
        /* The guest may free the port info block after this, and firmware
         * zeroes the port counters, so a later PORT_INIT starts from 0. */
        emu->port_info_pa = 0;
        ionic_emu_port_clear(&emu->stats);
        comp[0] = 0;
        break;

    case IONIC_CMD_PORT_IDENTIFY:
    case IONIC_CMD_PORT_GETATTR:
    case IONIC_CMD_PORT_SETATTR:
        /* Stub: return success, data zeroed = sane defaults.  PORT_INIT is
         * handled above because the driver reads the area it points at. */
        comp[0] = 0;
        break;

    /* RDMA devcmds 50-53 forwarded to ionic_rdma_devcmd.c */
    case 50:
    case 51:
    case 52:
    case 53:
        handle_rdma_cmd(emu, cmd, comp);
        break;

    /* P4 devcmds 70-72 forwarded to p4_devcmd.c */
    case 70:
    case 71:
    case 72:
        handle_p4_cmd(emu, cmd, comp);
        break;

    default:
        vfu_log(emu->vfu_ctx, LOG_WARNING, "ionic_eth_emu: unknown opcode=%u",
                opcode);
        comp[0] = IONIC_RC_EOPCODE;
        break;
    }

    /* Assert done bit. */
    ernic_put_le32(cmd_base + DEVCMD_DONE_OFF, IONIC_DEV_CMD_DONE);
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

void ionic_eth_emu_handle_identify(const struct ionic_eth_emu *emu,
                                   const uint8_t *cmd, uint8_t *comp,
                                   uint8_t *data)
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

    ernic_put_le32(dev_id + DEVID_NLIFS_OFF, 1); /* single LIF for eth + RDMA */

    /* nintrs bounds ionic_lif_size(): it needs 1 (adminq) + nxqs + neqs.
     * It is also the MSI-X table size the server sets up. */
    ernic_put_le32(dev_id + DEVID_NINTRS_OFF,
                   ionic_profile_intr_vectors(&emu->profile));
    /* One doorbell page per process using RDMA, and one for the kernel:
     * every page BAR2 has. The driver hands each user context one and
     * maps it from BAR2, so a count past the end of BAR2 has it map pages
     * that aren't the device's. */
    size_t db_pages = emu->bar2_size / IONIC_DB_PAGE_SIZE;
    ernic_put_le32(dev_id + DEVID_NDBPGS_OFF,
                   db_pages > UINT32_MAX ? UINT32_MAX : (uint32_t)db_pages);

    /* ethtool divides by intr_coal_div, so it must not be zero. */
    ernic_put_le32(dev_id + DEVID_INTR_COAL_MULT_OFF, 1);
    ernic_put_le32(dev_id + DEVID_INTR_COAL_DIV_OFF, 1);
    ernic_put_le32(dev_id + DEVID_EQ_COUNT_OFF, emu->profile.queues.rdma_eqs);

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
void ionic_eth_emu_handle_lif_identify(struct ionic_eth_emu *emu,
                                       const uint8_t *cmd, uint8_t *comp,
                                       uint8_t *data)
{
    (void)cmd;

    /* ionic_lif_identity: 478 * 4 = 1912 bytes, fills the data area. */
    memset(data, 0, DEVCMD_DATA_SIZE);

    /*
     * capabilities: ETH | RDMA, plus P4 when there is a P4 control
     * surface to find.  Keyed on the handler rather than on a separate
     * flag so the bit and the surface cannot disagree: a guest told it
     * has the capability and then refused every command would have no
     * way to tell that from a broken device.
     */
    uint64_t cap_bits = IONIC_LIF_CAP_ETH | IONIC_LIF_CAP_RDMA;
    if (emu->p4_devcmd_fn)
        cap_bits |= IONIC_LIF_CAP_P4;
    ernic_put_le64(data, cap_bits);

    /* Absolute offsets into union ionic_lif_identity, from offsetof() against
     * the real ionic_if.h.  The struct is packed, so nothing here is aligned
     * and every field goes through the byteorder.h accessors. */
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

    data[LIFID_ETH_VERSION_OFF] = 1;

    ernic_put_le32(data + LIFID_MAX_UCAST_OFF, 4);
    ernic_put_le32(data + LIFID_MAX_MCAST_OFF, 32);

    ernic_put_le16(data + LIFID_RSS_IND_TBL_SZ_OFF, 128);

    ernic_put_le32(data + LIFID_MIN_FRAME_SIZE_OFF, 64);
    /* The driver lets the guest set an MTU up to max_frame_size -
     * VLAN_ETH_HLEN (ionic_lif.c), so this is link.mtu_max plus 18. */
    ernic_put_le32(data + LIFID_MAX_FRAME_SIZE_OFF,
                   emu->profile.link.mtu_max + IONIC_PROFILE_FRAME_OVERHEAD);

    /* ionic_lif_config.  queue_count[] is not advisory: ionic_lif_size() reads
     * TXQ/RXQ straight out of it and hands the result to alloc_etherdev_mqs(),
     * which returns NULL (-ENOMEM) if it is zero. */
    uint8_t *qcount = data + LIFID_CONFIG_QCOUNT_OFF;
    ernic_put_le32(qcount + 4u * IONIC_QTYPE_ADMINQ, 1);
    ernic_put_le32(qcount + 4u * IONIC_QTYPE_NOTIFYQ, 1);
    ernic_put_le32(qcount + 4u * IONIC_QTYPE_RXQ, eth_qcount(emu));
    ernic_put_le32(qcount + 4u * IONIC_QTYPE_TXQ, eth_qcount(emu));

    memcpy(data + LIFID_CONFIG_NAME_OFF, "ernic0", 7);

    ernic_put_le32(data + LIFID_CONFIG_MTU_OFF, 1500);

    memcpy(data + LIFID_CONFIG_MAC_OFF, emu->mac, sizeof(emu->mac));

    /* LIF_ID_RDMA_OFF is worked out in ionic_eth_emu_priv.h. */
    uint8_t *rdma = data + LIF_ID_RDMA_OFF;

    rdma[0] = IONIC_RDMA_VERSION;       /* version       */
    rdma[1] = IONIC_RDMA_QP_OPCODES;    /* qp_opcodes    */
    rdma[2] = IONIC_RDMA_ADMIN_OPCODES; /* admin_opcodes  */
    rdma[3] = 0;                        /* minor_version  */

    ernic_put_le32(rdma + 4, ERNIC_MAX_PAGE_TABLE_ENTRIES); /* npts_per_lif */
    ernic_put_le32(rdma + 8, IONIC_EMU_NMRS_PER_LIF);       /* nmrs_per_lif */
    ernic_put_le32(rdma + 12, ERNIC_MAX_AH);                /* nahs_per_lif */

    rdma[16] = IONIC_MAX_STRIDE;     /* max_stride */
    rdma[17] = 6;                    /* cl_stride (log2 64B cache line) */
    rdma[18] = 3;                    /* pte_stride (log2 8B PTE)        */
    rdma[19] = 6;                    /* rrq_stride                       */
    rdma[20] = 6;                    /* rsq_stride                       */
    rdma[21] = 8;                    /* dcqcn_profiles                   */
    rdma[22] = IONIC_EMU_UDMA_SHIFT; /* udma_shift                  */
    rdma[23] = IONIC_UDMA_COUNT;     /* rsvd_dimensions (udma_count)     */

    ernic_put_le64(rdma + 24, IONIC_PAGE_SIZE_CAP); /* page_size_cap */

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
    ernic_put_le32(aq + 4, ERNIC_MAX_AQ);
    ernic_put_le32(aq + 8, 0);

    /* sq_qtype at rdma+44 */
    uint8_t *sq = rdma + 32 + QTYPE_SZ;
    sq[0] = IONIC_RDMA_QTYPE_SQ;
    ernic_put_le32(sq + 4, ERNIC_MAX_QP);
    ernic_put_le32(sq + 8, 0);

    /* rq_qtype at rdma+56 */
    uint8_t *rq = rdma + 32 + 2 * QTYPE_SZ;
    rq[0] = IONIC_RDMA_QTYPE_RQ;
    ernic_put_le32(rq + 4, ERNIC_MAX_QP);
    ernic_put_le32(rq + 8, 0);

    /* cq_qtype at rdma+68 */
    uint8_t *cq = rdma + 32 + 3 * QTYPE_SZ;
    cq[0] = IONIC_RDMA_QTYPE_CQ;
    ernic_put_le32(cq + 4, ERNIC_MAX_CQ);
    ernic_put_le32(cq + 8, 0);

    /* eq_qtype at rdma+80 */
    uint8_t *eq = rdma + 32 + 4 * QTYPE_SZ;
    eq[0] = IONIC_RDMA_QTYPE_EQ;
    /* ionic_lif_size() takes min(this, online vCPUs) EQs. */
    ernic_put_le32(eq + 4, emu->profile.queues.rdma_eqs);
    ernic_put_le32(eq + 8, 0);

    /* stats_type (le16), straight after eq_qtype. */
    ernic_put_le16(
        rdma + LIF_ID_RDMA_STATS_TYPE_OFF,
        (uint16_t)(IONIC_LIF_RDMA_STAT_GLOBAL | IONIC_LIF_RDMA_STAT_QP));

    /*
     * srq_qtype, and NOT at rdma+32+5*QTYPE_SZ. The five qtypes above are
     * contiguous, but srq_qtype is not the sixth: union ionic_lif_identity
     * puts four bytes between the end of eq_qtype and the start of it --
     *
     *     struct ionic_lif_logical_qtype eq_qtype;   rdma+80 .. 91
     *     __le16 stats_type;                         rdma+92
     *     u8     rsvd;                               rdma+94
     *     u8     rcq_sign_bit;                       rdma+95
     *     struct ionic_lif_logical_qtype srq_qtype;  rdma+96 .. 107
     *     u8     rsvd2[5];                           rdma+108
     *     u8     alloc_qid_cap;                      rdma+113
     *
     * -- so the usual stride arithmetic lands on stats_type and the driver
     * reads a qid_count built out of rcq_sign_bit. Offsets are literal here
     * for that reason. (ionic_if.h, the SRQ series in rdma/for-next.)
     */
    uint8_t *srq = rdma + LIF_ID_SRQ_QTYPE_OFF;
    srq[0] = IONIC_RDMA_QTYPE_SRQ;
    /* qid_count -- becomes the guest's max_srq */
    ernic_put_le32(srq + 4, ERNIC_MAX_SRQ);
    /* qid_base -- the first id handle_create_srq_op() hands out. The driver
     * reads only the count, but the field should still be true. */
    ernic_put_le32(srq + 8, IONIC_SRQ_QID_BASE);

    /*
     * Both halves of the gate are required. ionic_query_device() reports an
     * SRQ capability only when the count is non-zero AND the matching
     * alloc-qid bit is set:
     *
     *   if (dev->lif_cfg.srq_count &&
     *       ionic_fw_has_qid_alloc(dev, IONIC_LIF_RDMA_ALLOC_QID_SRQ))
     *           attr->max_srq = dev->lif_cfg.srq_count;
     *
     * The bit is also a promise we can keep: it says firmware allocates the
     * SRQ qid, which is what handle_create_srq_op() does -- it assigns the id
     * and DMAs it back, rather than honouring the one the driver sent.
     */
    rdma[LIF_ID_ALLOC_QID_CAP_OFF] = IONIC_LIF_RDMA_ALLOC_QID_SRQ;

    comp[0] = 0; /* status OK */
    comp[1] = 1; /* version   */
}

/* -------------------------------------------------------------------------
 * PORT_INIT (opcode 11)
 *
 * Publishes the port identity into the DMA area the driver passes in.
 *
 * This is what makes `ethtool` report a link speed, and it is less direct
 * than it looks.  ionic_get_link_ksettings() takes the speed and duplex from
 * lif->info->status -- which LIF_INIT below already fills in with 100 Gbps
 * and a live carrier -- but only inside:
 *
 *     if (ks->base.port != PORT_NONE) {
 *             ks->base.speed = le32_to_cpu(lif->info->status.link_speed);
 *             ...
 *     }
 *
 * and ks->base.port comes from port_info->status.xcvr: PORT_DA for a copper
 * phy or a recognised copper transceiver, PORT_FIBRE for fiber, PORT_NONE
 * otherwise.  While this command was a stub the whole structure stayed zero,
 * so the transceiver read back as absent, the port resolved to PORT_NONE,
 * and the assignment above never ran.  The result was a NIC that logs
 * "Link up - 100 Gbps" while ethtool reports:
 *
 *     Supported link modes:   Not reported
 *     Speed: Unknown!
 *     Duplex: Half
 *
 * That is not cosmetic.  UCX derives a TCP device's bandwidth from the
 * ethtool speed, so a zero made it refuse to build an endpoint address
 * ("failed to unpack address, invalid bandwidth 0.00") and every NIXL agent
 * -- and therefore every LMCache MP server -- died at startup on a guest
 * that could see this NIC.
 *
 * Declaring a QSFP 100G CR4 transceiver is what resolves the port to PORT_DA.
 * It also populates the supported link modes, which otherwise read "Not
 * reported".
 * -------------------------------------------------------------------------
 */
/*
 * The transceiver part id for a link.xcvr and speed (enum ionic_xcvr_pid in
 * ionic_if.h).  ethtool lists the link modes a pid supports, so the pid
 * matches the speed: a CR4/CR copper cable, or an SR module for fiber.
 * Speeds with no such pid report IONIC_XCVR_PID_UNKNOWN (0).
 */
static uint16_t xcvr_pid(uint8_t xcvr, uint32_t speed_mbps)
{
    static const struct {
        uint32_t speed;
        uint16_t copper;
        uint16_t fiber;
    } pids[] = {
        {1000, IONIC_XCVR_PID_SFP_1000BASE_T, IONIC_XCVR_PID_UNKNOWN},
        {10000, IONIC_XCVR_PID_SFP_10GBASE_CU, IONIC_XCVR_PID_SFP_10GBASE_SR},
        {25000, IONIC_XCVR_PID_SFP_25GBASE_CR_S, IONIC_XCVR_PID_SFP_25GBASE_SR},
        {40000, IONIC_XCVR_PID_QSFP_40GBASE_CR4,
         IONIC_XCVR_PID_QSFP_40GBASE_SR4},
        {50000, IONIC_XCVR_PID_QSFP_50G_CR2, IONIC_XCVR_PID_UNKNOWN},
        {100000, IONIC_XCVR_PID_QSFP_100G_CR4, IONIC_XCVR_PID_QSFP_100G_SR4},
        {200000, IONIC_XCVR_PID_QSFP_200G_CR4, IONIC_XCVR_PID_QSFP_200G_SR4},
        {400000, IONIC_XCVR_PID_QSFP_400G_CR4, IONIC_XCVR_PID_QSFP_400G_SR4},
    };

    if (xcvr == IONIC_PROFILE_XCVR_NONE)
        return IONIC_XCVR_PID_UNKNOWN;
    for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); i++)
        if (pids[i].speed == speed_mbps)
            return xcvr == IONIC_PROFILE_XCVR_FIBER ? pids[i].fiber
                                                    : pids[i].copper;
    return IONIC_XCVR_PID_UNKNOWN;
}

void ionic_eth_emu_handle_port_init(struct ionic_eth_emu *emu,
                                    const uint8_t *cmd, uint8_t *comp)
{
    /* cmd layout: opcode(1) index(1) rsvd(6) info_pa(8) rsvd2(48) */
    uint64_t info_pa = ernic_get_le64(cmd + 8);

    if (info_pa) {
        uint8_t info[IONIC_PORT_INFO_PUBLISHED_LEN];
        const struct ionic_profile *prof = &emu->profile;
        uint32_t mtu = prof->link.mtu_max < IONIC_ETH_MTU ? prof->link.mtu_max
                                                          : IONIC_ETH_MTU;

        memset(info, 0, sizeof(info));
        ernic_put_le32(info + IONIC_PORT_INFO_CONFIG_SPEED_OFF,
                       prof->link.speed_mbps);
        ernic_put_le32(info + IONIC_PORT_INFO_CONFIG_MTU_OFF, mtu);
        info[IONIC_PORT_INFO_CONFIG_STATE_OFF] = IONIC_PORT_ADMIN_STATE_UP;
        ernic_put_le32(info + IONIC_PORT_INFO_STATUS_SPEED_OFF,
                       prof->link.speed_mbps);
        info[IONIC_PORT_INFO_STATUS_STATUS_OFF] = IONIC_PORT_OPER_STATUS_UP;
        if (prof->link.xcvr != IONIC_PROFILE_XCVR_NONE) {
            info[IONIC_PORT_INFO_XCVR_STATE_OFF] = IONIC_XCVR_STATE_INSERTED;
            info[IONIC_PORT_INFO_XCVR_PHY_OFF] =
                prof->link.xcvr == IONIC_PROFILE_XCVR_FIBER
                    ? IONIC_PHY_TYPE_FIBER
                    : IONIC_PHY_TYPE_COPPER;
        } else {
            info[IONIC_PORT_INFO_XCVR_STATE_OFF] = IONIC_XCVR_STATE_REMOVED;
            info[IONIC_PORT_INFO_XCVR_PHY_OFF] = IONIC_PHY_TYPE_NONE;
        }
        ernic_put_le16(info + IONIC_PORT_INFO_XCVR_PID_OFF,
                       xcvr_pid(prof->link.xcvr, prof->link.speed_mbps));

        if (ernic_dma_write(emu->dma, emu->vfu_ctx, info_pa, info,
                            sizeof(info)) < 0)
            vfu_log(emu->vfu_ctx, LOG_ERR,
                    "ionic_eth_emu: PORT_INIT: port info write to %#" PRIx64
                    " failed",
                    info_pa);
        /* The stats tick fills in the MAC counters from here on. */
        emu->port_info_pa = info_pa;
        emu->stats_publish_now = true;
    }

    comp[0] = 0; /* status OK */

    vfu_log(emu->vfu_ctx, LOG_INFO,
            "ionic_eth_emu: PORT_INIT speed=%uMbps info_pa=%#" PRIx64,
            (unsigned)emu->profile.link.speed_mbps, info_pa);
}

/* -------------------------------------------------------------------------
 * LIF_INIT (opcode 21)
 * -------------------------------------------------------------------------
 */
void ionic_eth_emu_handle_lif_init(struct ionic_eth_emu *emu,
                                   const uint8_t *cmd, uint8_t *comp)
{
    /* cmd layout: opcode(1) type(1) index(2) rsvd(4) info_pa(8) rsvd2(48) */
    uint16_t lif_index = ernic_get_le16(cmd + 2);

    uint64_t info_pa = ernic_get_le64(cmd + 8);

    /* ionic_link_status_check() reads lif->info->status out of this DMA area
     * and leaves the netdev carrier off unless link_status is
     * IONIC_PORT_OPER_STATUS_UP.  Without a carrier the RDMA port never leaves
     * PORT_DOWN, so publish a permanently-up link here.
     *   struct ionic_lif_info: config[0..255], status at 256
     *   struct ionic_lif_status: link_status at +10, link_speed at +12 */
    if (info_pa) {
        uint8_t status[64] = {0};
        ernic_put_le16(status + 10, IONIC_PORT_OPER_STATUS_UP);
        ernic_put_le32(status + 12, emu->profile.link.speed_mbps); /* Mbps */
        if (info_pa >
                UINT64_MAX - IONIC_LIF_INFO_STATUS_OFF - (sizeof(status) - 1) ||
            ernic_dma_write(emu->dma, emu->vfu_ctx,
                            info_pa + IONIC_LIF_INFO_STATUS_OFF, status,
                            sizeof(status)) < 0)
            vfu_log(emu->vfu_ctx, LOG_ERR,
                    "ionic_eth_emu: LIF_INIT: link status write for info base "
                    "%#" PRIx64 " failed",
                    info_pa);
    }

    emu->lif_initialized = true;
    emu->lif_hw_index = lif_index;
    emu->lif_info_pa = info_pa;
    emu->stats_publish_now = true;

    comp[0] = 0; /* status OK */
    comp[1] = 0; /* rsvd      */

    /* hw_index in comp[2:3] */
    ernic_put_le16(comp + 2, lif_index);

    vfu_log(emu->vfu_ctx, LOG_INFO, "ionic_eth_emu: LIF_INIT lif_index=%u",
            lif_index);
}

/* -------------------------------------------------------------------------
 * LIF_SETATTR / LIF_GETATTR (opcodes 24, 23)
 * -------------------------------------------------------------------------
 */
/* enum ionic_lif_attr */
#define IONIC_LIF_ATTR_MAC 3

/* struct ionic_lif_{get,set}attr_{cmd,comp}: opcode, attr, __le16 index,
 * then the attribute union at byte 4 in both directions. */
#define LIF_ATTR_CMD_OFF  4u
#define LIF_ATTR_COMP_OFF 4u

static void handle_lif_setattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp)
{
    uint8_t attr = cmd[1];
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: LIF_SETATTR attr=%u",
            attr);
    if (attr == IONIC_LIF_ATTR_MAC)
        memcpy(emu->mac, cmd + LIF_ATTR_CMD_OFF, sizeof(emu->mac));
    comp[0] = IONIC_RC_SUCCESS;
}

static void handle_lif_getattr(struct ionic_eth_emu *emu, const uint8_t *cmd,
                               uint8_t *comp)
{
    uint8_t attr = cmd[1];
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: LIF_GETATTR attr=%u",
            attr);
    if (attr == IONIC_LIF_ATTR_MAC)
        memcpy(comp + LIF_ATTR_COMP_OFF, emu->mac, sizeof(emu->mac));
    comp[0] = IONIC_RC_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Q_IDENTIFY (opcode 39)
 *
 * Returns ionic_q_identity for the requested queue type into data[].
 * The driver uses this to learn WQE stride ranges and speculative SGE counts.
 * -------------------------------------------------------------------------
 */
void ionic_eth_emu_handle_q_identify(const struct ionic_eth_emu *emu,
                                     const uint8_t *cmd, uint8_t *comp,
                                     uint8_t *data)
{
    uint8_t qtype = cmd[3]; /* ionic_logical_qtype */
    vfu_log(emu->vfu_ctx, LOG_DEBUG, "ionic_eth_emu: Q_IDENTIFY qtype=%u",
            qtype);

    /* union ionic_q_identity (packed):
     *   u8   version     @0
     *   u8   supported   @1   bitfield of versions, bit 0 = ver 0
     *   u8   rsvd[6]     @2
     *   le64 features    @8   enum ionic_q_feature
     *   le16 desc_sz     @16
     *   le16 comp_sz     @18
     *   le16 sg_desc_sz  @20
     *   le16 max_sg_elems   @22
     *   le16 sg_desc_stride @24
     *
     * ionic_lif_queue_identify() only asks about the four Ethernet logical
     * qtypes, and it overwrites max_sg_elems with its own per-qtype limit
     * afterwards, so the sizes are what actually matter here. */
#define QID_FEATURES_OFF       8u
#define QID_DESC_SZ_OFF        16u
#define QID_COMP_SZ_OFF        18u
#define QID_SG_DESC_SZ_OFF     20u
#define QID_MAX_SG_ELEMS_OFF   22u
#define QID_SG_DESC_STRIDE_OFF 24u

    memset(data, 0, 64);
    data[0] = 0; /* version 0: the base descriptor formats */
    data[1] = 1; /* supported: bit 0 = version 0 */

    ernic_put_le16(data + QID_DESC_SZ_OFF, ETH_DESC_SIZE);
    ernic_put_le16(data + QID_COMP_SZ_OFF, ETH_COMP_SIZE);

    if (qtype == IONIC_QTYPE_TXQ || qtype == IONIC_QTYPE_RXQ) {
        ernic_put_le16(data + QID_SG_DESC_SZ_OFF,
                       TXQ_SG_MAX_ELEMS * TXQ_SG_ELEM_SIZE);
        ernic_put_le16(data + QID_MAX_SG_ELEMS_OFF, TXQ_SG_MAX_ELEMS);
        ernic_put_le16(data + QID_SG_DESC_STRIDE_OFF, TXQ_SG_MAX_ELEMS);
    }

    /* features stays zero: no CMB, no 2x/4x descriptor rings, no expanded
     * doorbell.  Every one of those changes a ring layout we would then have
     * to decode differently. */
    ernic_put_le16(data + QID_FEATURES_OFF, 0);
#undef QID_SG_DESC_STRIDE_OFF
#undef QID_MAX_SG_ELEMS_OFF
#undef QID_SG_DESC_SZ_OFF
#undef QID_COMP_SZ_OFF
#undef QID_DESC_SZ_OFF
#undef QID_FEATURES_OFF

    /* struct ionic_q_identify_comp: status@0, rsvd@1, comp_index@2, ver@4. */
    comp[0] = 0;
    comp[4] = 0;
}

/* -------------------------------------------------------------------------
 * Q_INIT (opcode 40)
 *
 * The driver calls this to initialise admin queue, notifyq, and (optionally)
 * rxq/txq.  We stub it: return success so the driver proceeds to call
 * ionic_auxbus_register().
 * -------------------------------------------------------------------------
 */
void ionic_eth_emu_handle_q_init(struct ionic_eth_emu *emu, const uint8_t *cmd,
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
    uint32_t index = ernic_get_le32(cmd + 8);

    /* Remaining ionic_q_init_cmd fields (packed): intr_index le16 @14,
     * flags le16 @16, cos @18, ring_size @19 (log2 depth), ring_base le64
     * @20, cq_ring_base le64 @28. */
    uint16_t intr_index = ernic_get_le16(cmd + 14);
    uint8_t ring_size = cmd[19];
    uint64_t ring_base = ernic_get_le64(cmd + 20);
    uint64_t cq_ring_base = ernic_get_le64(cmd + 28);
    uint64_t sg_ring_base = ernic_get_le64(cmd + 36);

    /* ring_size is the guest's exponent, and isn't checked until further
     * down, so it is logged as one rather than shifted by. */
    vfu_log(emu->vfu_ctx, LOG_INFO,
            "ionic_eth_emu: Q_INIT qtype=%u index=%u intr=%u depth=2^%u "
            "ring=%#" PRIx64 " cq=%#" PRIx64,
            qtype, index, intr_index, ring_size, ring_base, cq_ring_base);

    if (qtype < IONIC_QTYPE_ETH_MAX && index < eth_qcount(emu) &&
        ring_size < 16) {
        struct eth_queue *q = &emu->eth_q[qtype][index];
        *q = (struct eth_queue){
            .valid = true,
            .intr_index = intr_index,
            .ring_base = ring_base,
            .cq_ring_base = cq_ring_base,
            .sg_ring_base = sg_ring_base,
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
    comp[0] = 0;                     /* status OK */
    comp[1] = 0;                     /* rsvd      */
    ernic_put_le16(comp + 2, 0);     /* comp_index */
    ernic_put_le32(comp + 4, index); /* hw_index as le32 */
    comp[8] = qtype;                 /* hw_type  */
}

/* -------------------------------------------------------------------------
 * Q_CONTROL (opcode 41)
 *
 * ionic_qcq_enable() sends ENABLE once a queue is initialized and
 * ionic_qcq_disable() sends DISABLE before tearing it down.  Only the Rx
 * side acts on it: frames are steered across the enabled Rx queues, so a
 * guest that runs fewer channels than the LIF offers never has a frame
 * land on a queue it is not servicing.  HANG_RESET and unknown queues
 * succeed with no effect, as every other stateless command does here.
 *
 * struct ionic_q_control_cmd: opcode@0, type@1, lif_index le16 @2,
 * index le32 @4, oper@8.
 * -------------------------------------------------------------------------
 */
static void handle_q_control(struct ionic_eth_emu *emu, const uint8_t *cmd,
                             uint8_t *comp)
{
    uint8_t qtype = cmd[1];
    uint32_t index;
    uint8_t oper = cmd[8];

    index = ernic_get_le32(cmd + 4);

    vfu_log(emu->vfu_ctx, LOG_DEBUG,
            "ionic_eth_emu: Q_CONTROL type=%u index=%u oper=%u", qtype, index,
            oper);

    if (qtype < IONIC_QTYPE_ETH_MAX && index < eth_qcount(emu)) {
        struct eth_queue *q = &emu->eth_q[qtype][index];

        if (oper == IONIC_Q_ENABLE)
            q->enabled = true;
        else if (oper == IONIC_Q_DISABLE)
            q->enabled = false;
    }
    comp[0] = IONIC_RC_SUCCESS;
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

void ionic_eth_emu_process_adminq_cmd(struct ionic_eth_emu *emu,
                                      const uint8_t *cmd, uint8_t *comp)
{
    switch (cmd[0]) {
    case IONIC_CMD_Q_INIT:
        ionic_eth_emu_handle_q_init(emu, cmd, comp);
        break;

    /* ionic_qcq_enable() and ionic_qcq_disable() post these here. */
    case IONIC_CMD_Q_CONTROL:
        handle_q_control(emu, cmd, comp);
        break;

    /* ionic_rdma_devcmd() is a misnomer: RDMA opcodes 50-53 go out over the
     * Ethernet adminq, not the BAR0 devcmd window. */
    case 50:
    case 51:
    case 52:
    case 53:
        handle_rdma_cmd(emu, cmd, comp);
        break;

    /* P4 devcmds, which reach the engine the same two ways. */
    case 70:
    case 71:
    case 72:
        handle_p4_cmd(emu, cmd, comp);
        break;

    /* The station MAC does not come from LIF_IDENTIFY: ionic_lif_alloc()
     * asks for it here, and reads an all-zero answer as "nothing
     * programmed", generating a random address instead.  Two bridged
     * instances then collide only by luck. */
    case IONIC_CMD_LIF_GETATTR:
        handle_lif_getattr(emu, cmd, comp);
        break;

    case IONIC_CMD_LIF_SETATTR:
        handle_lif_setattr(emu, cmd, comp);
        break;

    default:
        /* Everything else the Ethernet driver posts during bring-up
         * (RX_MODE_SET, RX_FILTER_ADD, ...) has no state in the
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

        if (ernic_dma_read(emu->dma, emu->vfu_ctx,
                           q->ring_base + (uint64_t)q->head * ADMIN_CMD_SIZE,
                           cmd, sizeof(cmd)) < 0) {
            vfu_log(emu->vfu_ctx, LOG_ERR,
                    "ionic_eth_emu: adminq desc DMA read failed at %u",
                    q->head);
            return;
        }

        ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);

        ernic_put_le16(comp + 2, q->head);
        comp[ADMIN_COMP_SIZE - 1] =
            q->cq_color ? (uint8_t)ADMIN_COMP_COLOR_MASK : 0;

        ernic_dma_write(emu->dma, emu->vfu_ctx,
                        q->cq_ring_base +
                            (uint64_t)q->cq_index * ADMIN_COMP_SIZE,
                        comp, sizeof(comp));

        q->head = (uint16_t)((q->head + 1) % q->depth);
        if (++q->cq_index == q->depth) {
            q->cq_index = 0;
            q->cq_color ^= 1;
        }
    }

    ionic_eth_emu_trigger_irq(emu, q->intr_index);
}

/* Advance a queue's completion cursor after writing one completion. */
static void eth_cq_advance(struct eth_queue *q)
{
    if (++q->cq_index == q->depth) {
        q->cq_index = 0;
        q->cq_color ^= 1;
    }
}

/* Pull one transmitted frame out of guest memory into emu->frame.  Returns
 * the assembled length, or 0 if the descriptor cannot be gathered. */
static size_t eth_tx_gather(struct ionic_eth_emu *emu, struct eth_queue *q,
                            uint16_t index, enum ionic_emu_drop *why)
{
    uint8_t desc[ETH_DESC_SIZE];

    *why = IONIC_EMU_DROP_DESC_FETCH;
    if (ernic_dma_read(emu->dma, emu->vfu_ctx,
                       q->ring_base + (uint64_t)index * ETH_DESC_SIZE, desc,
                       sizeof(desc)) < 0)
        return 0;

    uint64_t cmd = ernic_get_le64(desc);
    uint16_t len = ernic_get_le16(desc + 8);

    uint8_t nsge =
        (uint8_t)((cmd >> TXQ_DESC_NSGE_SHIFT) & TXQ_DESC_NIBBLE_MASK);
    uint64_t addr = cmd >> TXQ_DESC_ADDR_SHIFT;

    /* len is the whole frame; the head descriptor carries the first fragment
     * and the SG ring carries the rest.  We advertise no offloads, so the
     * driver never posts a TSO descriptor whose len exceeds one frame. */
    *why = IONIC_EMU_DROP_DESC_DATA;
    if (!len || len > sizeof(emu->frame))
        return 0;

    if (!nsge) {
        *why = IONIC_EMU_DROP_DMA;
        if (ernic_dma_read(emu->dma, emu->vfu_ctx, addr, emu->frame, len) < 0)
            return 0;
        return len;
    }

    if (nsge > TXQ_SG_MAX_ELEMS)
        return 0;

    *why = IONIC_EMU_DROP_DESC_FETCH;

    uint8_t sg[TXQ_SG_MAX_ELEMS * TXQ_SG_ELEM_SIZE];
    uint64_t sg_gpa =
        q->sg_ring_base + (uint64_t)index * TXQ_SG_MAX_ELEMS * TXQ_SG_ELEM_SIZE;
    if (ernic_dma_read(emu->dma, emu->vfu_ctx, sg_gpa, sg,
                       (size_t)nsge * TXQ_SG_ELEM_SIZE) < 0)
        return 0;

    /* The head descriptor's own fragment length is len minus everything the
     * SG elements contribute. */
    size_t sg_total = 0;
    for (uint8_t i = 0; i < nsge; i++)
        sg_total += ernic_get_le16(sg + (size_t)i * TXQ_SG_ELEM_SIZE + 8);
    *why = IONIC_EMU_DROP_DESC_DATA;
    if (sg_total >= len)
        return 0;

    *why = IONIC_EMU_DROP_DMA;
    size_t head_len = len - sg_total;
    if (ernic_dma_read(emu->dma, emu->vfu_ctx, addr, emu->frame, head_len) < 0)
        return 0;

    size_t off = head_len;
    for (uint8_t i = 0; i < nsge; i++) {
        const uint8_t *e = sg + (size_t)i * TXQ_SG_ELEM_SIZE;
        uint64_t eaddr = ernic_get_le64(e);
        uint16_t elen = ernic_get_le16(e + 8);
        if (!elen)
            continue;
        if (ernic_dma_read(emu->dma, emu->vfu_ctx, eaddr, emu->frame + off,
                           elen) < 0)
            return 0;
        off += elen;
    }

    return off;
}

/* Drain the Tx ring.  A registered filter sees each frame first and may
 * claim it; what it leaves goes to the host network backend when one is
 * attached.  With neither, Tx is a sink, which is still necessary -- without
 * a completion the netdev watchdog fires every five seconds and resets the
 * queues. */
static void eth_txq_service(struct ionic_eth_emu *emu, uint32_t qid,
                            uint16_t p_index)
{
    if (qid >= eth_qcount(emu))
        return;

    struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][qid];
    if (!q->valid) {
        ionic_emu_stats_drop(&emu->stats, NULL, 0, true,
                             IONIC_EMU_DROP_DISABLED);
        return;
    }

    uint16_t prod = (uint16_t)(p_index % q->depth);
    q->prod = prod;

    for (unsigned n = 0; q->head != prod && n < q->depth; n++) {
        /* Gathered even when Tx is a sink, so the frame is still counted as
         * sent: a NIC with nothing on the far end of its link counts too. */
        enum ionic_emu_drop why;
        size_t len = eth_tx_gather(emu, q, q->head, &why);
        if (!len) {
            ionic_emu_stats_drop(&emu->stats, NULL, 0, true, why);
        } else if (ionic_fault_event(emu->tx_fault)) {
            /* eth_tx drop: lost on the wire, so the guest still gets its
             * completion and only the drop counters see it. */
            ionic_emu_stats_drop(&emu->stats, emu->frame, len, true,
                                 IONIC_EMU_DROP_INJECTED);
        } else {
            bool taken = emu->tx_filter != NULL &&
                         emu->tx_filter(emu->tx_filter_ctx, emu->frame, len);
            if (!taken && emu->net &&
                ionic_eth_net_send(emu->net, emu->frame, len) < 0)
                ionic_emu_stats_drop(&emu->stats, emu->frame, len, true,
                                     IONIC_EMU_DROP_SEND);
            else {
                ionic_emu_stats_frame(&emu->stats, emu->frame, len, true);
                ionic_emu_port_frame(&emu->stats, emu->frame, len, true, 0);
            }
            if (emu->net || emu->tx_filter)
                ernic_stats_eth_bytes(emu->stats_sink, len, true);
        }

        /* struct ionic_txq_comp: status @0, comp_index le16 @2, colour @15. */
        uint8_t comp[ETH_COMP_SIZE] = {0};
        ernic_put_le16(comp + 2, q->head);
        comp[ETH_COMP_SIZE - 1] =
            q->cq_color ? (uint8_t)ADMIN_COMP_COLOR_MASK : 0;

        if (ernic_dma_write(emu->dma, emu->vfu_ctx,
                            q->cq_ring_base +
                                (uint64_t)q->cq_index * ETH_COMP_SIZE,
                            comp, sizeof(comp)) < 0)
            ionic_emu_stats_drop(&emu->stats, NULL, 0, true,
                                 IONIC_EMU_DROP_DMA);

        q->head = (uint16_t)((q->head + 1) % q->depth);
        eth_cq_advance(q);
    }

    ionic_eth_emu_trigger_irq(emu, q->intr_index);
}

/* -------------------------------------------------------------------------
 * Receive path
 *
 * ionic_rx_fill() posts one descriptor per buffer and rings the Rx doorbell
 * with its head index; we treat that as the producer index and consume
 * descriptors from our own tail as frames arrive.  With no offloads
 * advertised the driver never asks for more than IONIC_PAGE_SIZE in the first
 * fragment, so a 1500-byte MTU frame always fits in a single descriptor.
 * -------------------------------------------------------------------------
 */

static int eth_rx_deliver(const struct ionic_eth_emu *emu, struct eth_queue *q,
                          uint8_t *frame, size_t len)
{
    if (q->head == q->prod)
        return -ENOBUFS; /* guest has posted no buffers */

    uint8_t desc[ETH_DESC_SIZE];
    if (ernic_dma_read(emu->dma, emu->vfu_ctx,
                       q->ring_base + (uint64_t)q->head * ETH_DESC_SIZE, desc,
                       sizeof(desc)) < 0)
        return -EFAULT;

    /* struct ionic_rxq_desc: opcode@0, rsvd[5], len le16 @6, addr le64 @8. */
    uint16_t cap = ernic_get_le16(desc + 6);
    uint64_t addr = ernic_get_le64(desc + 8);

    if (len > cap)
        return -EMSGSIZE;

    if (ernic_dma_write(emu->dma, emu->vfu_ctx, addr, frame, len) < 0)
        return -EIO;

    /* struct ionic_rxq_comp: status@0, num_sg_elems@1, comp_index le16 @2,
     * rss_hash le32 @4, csum le16 @8, vlan_tci le16 @10, len le16 @12,
     * csum_flags@14, pkt_type_color@15.  csum_flags stays zero so the driver
     * marks the skb CHECKSUM_NONE and verifies in software. */
    uint8_t comp[ETH_COMP_SIZE] = {0};
    comp[1] = 0;
    ernic_put_le16(comp + 2, q->head);
    ernic_put_le16(comp + 12, (uint16_t)len);
    comp[ETH_COMP_SIZE - 1] = q->cq_color ? (uint8_t)ADMIN_COMP_COLOR_MASK : 0;

    if (ernic_dma_write(emu->dma, emu->vfu_ctx,
                        q->cq_ring_base + (uint64_t)q->cq_index * ETH_COMP_SIZE,
                        comp, sizeof(comp)) < 0)
        return -EIO;

    q->head = (uint16_t)((q->head + 1) % q->depth);
    eth_cq_advance(q);
    return 0;
}

/*
 * Apply the eth_rx fault to a frame about to be delivered.  Returns true
 * when the fault fired and the frame is gone.
 */
static bool eth_rx_fault(struct ionic_eth_emu *emu, const uint8_t *frame,
                         size_t len)
{
    if (!emu->rx_fault || !ionic_fault_event(emu->rx_fault))
        return false;
    /* drop: lost before the NIC saw it, so no counter moves.  nobuf: the
     * MAC took it in and the LIF had no buffer for it, so both count it,
     * unless the MAC refused it first for being too long. */
    if (emu->rx_fault->effect == IONIC_FAULT_EFFECT_NOBUF &&
        ionic_emu_port_frame(&emu->stats, frame, len, false,
                             port_max_frame(emu))) {
        ionic_emu_stats_drop(&emu->stats, frame, len, false,
                             IONIC_EMU_DROP_NO_BUFFER);
        ionic_emu_port_rx_dropped(&emu->stats);
    }
    return true;
}

/* The lif_stats reason for an eth_rx_deliver() error. */
static enum ionic_emu_drop rx_drop_reason(int rc)
{
    switch (rc) {
    case -ENOBUFS:
        return IONIC_EMU_DROP_NO_BUFFER;
    case -EMSGSIZE:
        return IONIC_EMU_DROP_TOO_BIG;
    case -EFAULT:
        return IONIC_EMU_DROP_DESC_FETCH;
    default:
        return IONIC_EMU_DROP_DMA;
    }
}

/*
 * The Rx queues frames may land on: initialized and enabled, in index order.
 * Returns how many were written to @out.
 */
uint32_t ionic_eth_emu_rx_active(const struct ionic_eth_emu *emu,
                                 uint8_t out[IONIC_EMU_ETH_QMAX])
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < eth_qcount(emu); i++) {
        const struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][i];

        if (q->valid && q->enabled)
            out[n++] = (uint8_t)i;
    }
    return n;
}

/* The Rx queue index @frame steers to, one of the @n entries of @active. */
uint8_t ionic_eth_emu_rx_steer(const uint8_t *active, uint32_t n,
                               const uint8_t *frame, size_t len)
{
    return active[ionic_eth_flow_hash(frame, len) % n];
}

_Static_assert(IONIC_EMU_ETH_QMAX <= UINT8_MAX + 1,
               "ionic_eth_emu_rx_active() stores queue indices in a uint8_t");

void ionic_eth_emu_poll_rx(struct ionic_eth_emu *emu)
{
    if (!emu)
        return;

    uint8_t active[IONIC_EMU_ETH_QMAX];
    uint32_t nactive = ionic_eth_emu_rx_active(emu, active);
    if (nactive == 0)
        return;

    /* The queues that took a frame this poll, each interrupted once. */
    bool hit[IONIC_EMU_ETH_QMAX] = {false};

    /* Bounded per poll so a busy tap cannot starve the RDMA admin queue. */
    for (unsigned n = 0; emu->net && n < 64; n++) {
        ssize_t len =
            ionic_eth_net_recv(emu->net, emu->frame, sizeof(emu->frame));
        if (len <= 0)
            break;
        if (eth_rx_fault(emu, emu->frame, (size_t)len))
            continue;

        /* A frame the pipeline consumed is neither delivered nor
         * dropped by the NIC, so it skips both branches below rather
         * than being counted as either.  Before the steering, because a
         * frame that never reaches a queue has no business picking one. */
        if (emu->rx_filter &&
            emu->rx_filter(emu->rx_filter_ctx, emu->frame, (size_t)len))
            continue;

        /* The MAC drops a frame over the port's maximum before the LIF
         * sees it, so it is counted at the port and never steered. */
        if (!ionic_emu_port_frame(&emu->stats, emu->frame, (size_t)len, false,
                                  port_max_frame(emu)))
            continue;

        uint8_t qid =
            ionic_eth_emu_rx_steer(active, nactive, emu->frame, (size_t)len);
        struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][qid];

        /* A TAP frame the guest cannot take is gone, as on a real NIC. */
        int rc = eth_rx_deliver(emu, q, emu->frame, (size_t)len);
        if (rc == 0) {
            hit[qid] = true;
            ionic_emu_stats_frame(&emu->stats, emu->frame, (size_t)len, false);
            ernic_stats_eth_bytes(emu->stats_sink, (uint64_t)len, false);
        } else {
            ionic_emu_stats_drop(&emu->stats, emu->frame, (size_t)len, false,
                                 rx_drop_reason(rc));
            ionic_emu_port_rx_dropped(&emu->stats);
        }
    }

    /* Frames queued by in-process endpoints (the S3 control plane, the P4
     * pipeline).  A frame that the guest has no buffer for is put back at
     * the head so ordering holds and the producer keeps seeing
     * back-pressure; that is not a drop.  A frame
     * that can never be delivered (too big, or a DMA failure) is dropped
     * and counted, so it cannot block the queue behind it. */
    for (unsigned n = 0; n < 64; n++) {
        qemu_mutex_lock(&emu->inbox_lock);
        struct eth_inbox_frame *f = emu->inbox_head;
        if (f) {
            emu->inbox_head = f->next;
            if (!emu->inbox_head)
                emu->inbox_tail = NULL;
            emu->inbox_count--;
        }
        qemu_mutex_unlock(&emu->inbox_lock);
        if (!f)
            break;
        if (!f->fault_seen && eth_rx_fault(emu, f->data, f->len)) {
            free(f);
            continue;
        }
        f->fault_seen = true;

        /* Counted at the MAC once, however often it waits for a buffer. */
        if (!f->port_counted) {
            f->port_counted = true;
            if (!ionic_emu_port_frame(&emu->stats, f->data, f->len, false,
                                      port_max_frame(emu))) {
                free(f);
                continue;
            }
        }

        uint8_t qid = ionic_eth_emu_rx_steer(active, nactive, f->data, f->len);
        struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][qid];

        int rc = eth_rx_deliver(emu, q, f->data, f->len);
        if (rc == 0) {
            hit[qid] = true;
            ionic_emu_stats_frame(&emu->stats, f->data, f->len, false);
            ernic_stats_eth_bytes(emu->stats_sink, (uint64_t)f->len, false);
            free(f);
        } else if (rc != -ENOBUFS) {
            ionic_emu_stats_drop(&emu->stats, f->data, f->len, false,
                                 rx_drop_reason(rc));
            ionic_emu_port_rx_dropped(&emu->stats);
            free(f);
        } else {
            qemu_mutex_lock(&emu->inbox_lock);
            f->next = emu->inbox_head;
            emu->inbox_head = f;
            if (!emu->inbox_tail)
                emu->inbox_tail = f;
            emu->inbox_count++;
            qemu_mutex_unlock(&emu->inbox_lock);
            break;
        }
    }

    for (uint32_t i = 0; i < nactive; i++) {
        if (hit[active[i]])
            ionic_eth_emu_trigger_irq(
                emu, emu->eth_q[IONIC_QTYPE_RXQ][active[i]].intr_index);
    }
}

int ionic_eth_emu_attach_tap(struct ionic_eth_emu *emu, const char *ifname,
                             char *out_ifname, size_t out_ifname_len)
{
    if (emu->net)
        return -EBUSY;

    emu->net = ionic_eth_net_open_tap(ifname, out_ifname, out_ifname_len);
    if (!emu->net)
        return -errno;

    return 0;
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

/*
 * P4 devcmds (opcodes 70-72) -> p4_devcmd.c.
 *
 * With no handler the opcode is unknown, which is the truth: the engine
 * is not built, or the operator granted the guest nothing. That is
 * deliberately NOT the silent success handle_rdma_cmd() returns -- a
 * driver probing for the control surface has to be able to tell that it
 * is absent, and a success with an unfilled completion would read as a
 * device that identified as having no capabilities.
 */
static void handle_p4_cmd(struct ionic_eth_emu *emu, const uint8_t *cmd,
                          uint8_t *comp)
{
    if (!emu->p4_devcmd_fn) {
        vfu_log(emu->vfu_ctx, LOG_DEBUG,
                "ionic_eth_emu: P4 devcmd opcode=%u but no handler", cmd[0]);
        comp[0] = IONIC_RC_EOPCODE;
        return;
    }
    emu->p4_devcmd_fn(emu->p4_devcmd_opaque, cmd, comp);
}

/* -------------------------------------------------------------------------
 * Trigger an MSI-X interrupt vector (called by RDMA layer to signal EQ).
 * -------------------------------------------------------------------------
 */
int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    if (vec < 0 || vec >= IONIC_MSIX_MAX_VECTORS)
        return -EINVAL;

    /* Latch rather than drop: every vector starts masked, so an assertion
     * raised before the driver arms its handler would otherwise be lost and
     * the queue would wait forever for an interrupt that never comes again. */
    if (emu->intr_mask[vec]) {
        emu->intr_pending[vec] = 1;
        return 0;
    }

    return deliver_irq(emu, vec);
}
