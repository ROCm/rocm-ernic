/*
 * RDMA Backend: Loopback
 *
 * Internal RDMA loopback backend for testing without hardware.
 * Implements complete RDMA emulation with in-memory data transfer.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_backend.h"
#include "rdma_utils.h"
#include "standard-headers/rdma/vmw_pvrdma-abi.h"
#include "hw/rdma/rdma.h"    /* For rdma_pci_dma_map/unmap */
#include "hw/pci/pci.h"      /* For PCIDevice and pci_dma_sync */
#include "hw/pci/pci_regs.h" /* For PVRDMA_DEV */
#include "vmw/pvrdma.h"      /* For PVRDMADev and stats */
#include "vmw/pvrdma_comp_ctx.h"
#include "vmw/pvrdma_qp_ops.h"
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>

/*
 * Loopback Backend Data Structures
 */

typedef enum {
    LOOPBACK_DATA_PATTERN_ZEROS,        /* All 0x00 */
    LOOPBACK_DATA_PATTERN_ONES,         /* All 0xFF */
    LOOPBACK_DATA_PATTERN_INCREMENTING, /* 0x00, 0x01, 0x02, ... */
    LOOPBACK_DATA_PATTERN_DECREMENTING, /* 0xFF, 0xFE, 0xFD, ... */
    LOOPBACK_DATA_PATTERN_ALTERNATING,  /* 0xAA, 0x55, 0xAA, ... */
    LOOPBACK_DATA_PATTERN_RANDOM,       /* Random data */
    LOOPBACK_DATA_PATTERN_PRESERVE,     /* Use actual guest data (default) */
} LoopbackDataPattern;

typedef struct {
    uint32_t handle;
} LoopbackPD;

typedef struct {
    uint32_t handle;
    void *virt;
    size_t length;
    uint64_t guest_start;
    int access_flags;
    uint32_t lkey;
    uint32_t rkey;
    uint32_t pd_handle;
} LoopbackMR;

GHashTable *global_mrs_table = NULL;

typedef struct {
    enum ibv_wc_status status;
    uint64_t wr_id;
    uint32_t byte_len;
    uint32_t qp_num;
    enum ibv_wc_opcode opcode;
} LoopbackCompletion;

typedef struct {
    uint32_t handle;
    int cqe;
    GQueue *completions; /* Queue of LoopbackCompletion */
    QemuMutex lock;
} LoopbackCQ;

typedef struct {
    void *addr;
    uint32_t length;
    uint32_t lkey;
} LoopbackSGE;

typedef struct {
    uint64_t wr_id;
    uint32_t num_sge;
    LoopbackSGE sge[32]; /* Max SGEs */
} LoopbackWR;

typedef struct {
    uint32_t max_wr;
    uint32_t max_sge;
    uint32_t srq_limit;
    uint32_t pd_handle;
    GQueue *recv_queue;
    QemuMutex lock;
} LoopbackSRQ;

typedef struct {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;
    uint32_t qkey;
    uint32_t pd_handle;

    /* Connection info */
    uint32_t remote_qpn;
    union ibv_gid remote_gid;
    uint32_t rq_psn;
    uint32_t sq_psn;

    /* Ethernet key exchange info (for RoCE simulation) */
    uint64_t remote_addr; /* Remote virtual address */
    uint32_t remote_rkey; /* Remote rkey for RDMA operations */
    uint64_t local_addr;  /* Local virtual address (for remote to use) */
    uint32_t local_rkey;  /* Local rkey (for remote to use) */

    /* Associated CQs */
    LoopbackCQ *scq;
    LoopbackCQ *rcq;

    /* Backend device reference for auto-pairing */
    RdmaBackendDev *backend_dev;

    /* Work queues */
    GQueue *send_queue;
    GQueue *recv_queue;

    QemuMutex lock;
} LoopbackQP;

typedef struct {
    /* Resource tracking */
    GHashTable *pds; /* handle -> LoopbackPD */
    GHashTable *mrs; /* handle -> LoopbackMR */
    GHashTable *cqs; /* handle -> LoopbackCQ */
    GHashTable *qps; /* qpn -> LoopbackQP */

    /* Handle generators */
    uint32_t next_pd_handle;
    uint32_t next_mr_handle;
    uint32_t next_cq_handle;
    uint32_t next_qpn;

    /* For loopback connections */
    GHashTable *qp_pairs; /* local_qpn -> remote_qpn */

    /* Data pattern configuration */
    LoopbackDataPattern data_pattern;
    bool compute_md5; /* Whether to compute MD5 on data transfers */

    QemuMutex lock;
} LoopbackBackendPrivate;

/*
 * Helper Functions
 */

static LoopbackBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    if (!backend_dev) {
        return NULL;
    }
    return (LoopbackBackendPrivate *)backend_dev->backend_private;
}

/*
 * Helper: Update byte transfer statistics
 */
static void loopback_update_byte_stats(RdmaBackendDev *backend_dev,
                                       uint32_t qp_handle, uint32_t bytes,
                                       enum ibv_wc_opcode opcode)
{
    PCIDevice *pci_dev = backend_dev->dev;
    if (!pci_dev) {
        rdma_warn_report(">>> loopback_update_byte_stats: No pci_dev");
        return;
    }

    /* Get PVRDMADev from PCIDevice */
    PVRDMADev *dev = PVRDMA_DEV(pci_dev);
    if (!dev) {
        rdma_warn_report(">>> loopback_update_byte_stats: No PVRDMADev");
        return;
    }

    /* Get per-QP stats */
    PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
    if (!qp_stats) {
        rdma_warn_report(
            ">>> loopback_update_byte_stats: No stats for QP handle %u",
            qp_handle);
        return;
    }

    rdma_info_report(
        ">>> loopback_update_byte_stats: QP handle=%u, bytes=%u, opcode=%d",
        qp_handle, bytes, opcode);

    /* Update per-QP stats */
    switch (opcode) {
    case IBV_WC_SEND:
        qp_stats->bytes_sent += bytes;
        dev->stats.total_bytes_sent += bytes;
        break;
    case IBV_WC_RECV:
        qp_stats->bytes_received += bytes;
        dev->stats.total_bytes_received += bytes;
        break;
    case IBV_WC_RDMA_READ:
        qp_stats->bytes_rdma_read += bytes;
        dev->stats.total_bytes_rdma_read += bytes;
        break;
    case IBV_WC_RDMA_WRITE:
        qp_stats->bytes_rdma_write += bytes;
        dev->stats.total_bytes_rdma_write += bytes;
        break;
    default:
        break;
    }
}

static LoopbackDataPattern parse_data_pattern(const char *config)
{
    if (!config) {
        return LOOPBACK_DATA_PATTERN_PRESERVE; /* Default */
    }

    /* Parse mode=pattern format */
    const char *mode_str = strstr(config, "mode=");
    if (mode_str) {
        mode_str += 5; /* Skip "mode=" */
        /* Extract pattern value (up to comma or end of string) */
        char pattern[32] = {0};
        const char *comma = strchr(mode_str, ',');
        size_t len = comma ? (size_t)(comma - mode_str) : strlen(mode_str);
        if (len >= sizeof(pattern)) {
            len = sizeof(pattern) - 1;
        }
        strncpy(pattern, mode_str, len);
        pattern[len] = '\0';

        if (!strcmp(pattern, "preserve")) {
            return LOOPBACK_DATA_PATTERN_PRESERVE;
        }
        if (!strcmp(pattern, "zeros")) {
            return LOOPBACK_DATA_PATTERN_ZEROS;
        }
        if (!strcmp(pattern, "ones")) {
            return LOOPBACK_DATA_PATTERN_ONES;
        }
        if (!strcmp(pattern, "increment")) {
            return LOOPBACK_DATA_PATTERN_INCREMENTING;
        }
        if (!strcmp(pattern, "decrement")) {
            return LOOPBACK_DATA_PATTERN_DECREMENTING;
        }
        if (!strcmp(pattern, "alternate")) {
            return LOOPBACK_DATA_PATTERN_ALTERNATING;
        }
        if (!strcmp(pattern, "random")) {
            return LOOPBACK_DATA_PATTERN_RANDOM;
        }
    }

    /* Legacy format: check for patterns directly in config string */
    if (strstr(config, "preserve")) {
        return LOOPBACK_DATA_PATTERN_PRESERVE;
    }
    if (strstr(config, "zeros")) {
        return LOOPBACK_DATA_PATTERN_ZEROS;
    }
    if (strstr(config, "ones")) {
        return LOOPBACK_DATA_PATTERN_ONES;
    }
    if (strstr(config, "increment")) {
        return LOOPBACK_DATA_PATTERN_INCREMENTING;
    }
    if (strstr(config, "decrement")) {
        return LOOPBACK_DATA_PATTERN_DECREMENTING;
    }
    if (strstr(config, "alternate")) {
        return LOOPBACK_DATA_PATTERN_ALTERNATING;
    }
    if (strstr(config, "random")) {
        return LOOPBACK_DATA_PATTERN_RANDOM;
    }

    return LOOPBACK_DATA_PATTERN_PRESERVE; /* Default */
}

static const char *data_pattern_name(LoopbackDataPattern pattern)
{
    switch (pattern) {
    case LOOPBACK_DATA_PATTERN_ZEROS:
        return "zeros";
    case LOOPBACK_DATA_PATTERN_ONES:
        return "ones";
    case LOOPBACK_DATA_PATTERN_INCREMENTING:
        return "incrementing";
    case LOOPBACK_DATA_PATTERN_DECREMENTING:
        return "decrementing";
    case LOOPBACK_DATA_PATTERN_ALTERNATING:
        return "alternating";
    case LOOPBACK_DATA_PATTERN_RANDOM:
        return "random";
    case LOOPBACK_DATA_PATTERN_PRESERVE:
        return "preserve";
    default:
        return "unknown";
    }
}

static void generate_data_pattern(void *buffer, size_t length,
                                  LoopbackDataPattern pattern)
{
    uint8_t *buf = (uint8_t *)buffer;

    switch (pattern) {
    case LOOPBACK_DATA_PATTERN_ZEROS:
        memset(buf, 0x00, length);
        break;

    case LOOPBACK_DATA_PATTERN_ONES:
        memset(buf, 0xFF, length);
        break;

    case LOOPBACK_DATA_PATTERN_INCREMENTING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)(i & 0xFF);
        }
        break;

    case LOOPBACK_DATA_PATTERN_DECREMENTING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)((0xFF - i) & 0xFF);
        }
        break;

    case LOOPBACK_DATA_PATTERN_ALTERNATING:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (i % 2) ? 0x55 : 0xAA;
        }
        break;

    case LOOPBACK_DATA_PATTERN_RANDOM:
        for (size_t i = 0; i < length; i++) {
            buf[i] = (uint8_t)(g_random_int() & 0xFF);
        }
        break;

    case LOOPBACK_DATA_PATTERN_PRESERVE:
        /* Don't modify the buffer - use actual guest data */
        break;
    }
}

/*
 * Helper: Copy data from source SGEs to destination SGEs with pattern support
 * Returns number of bytes copied, or -1 on error
 */
static int loopback_copy_sge_data(PCIDevice *pci_dev, struct ibv_sge *src_sge,
                                  uint32_t num_src_sge, struct ibv_sge *dst_sge,
                                  uint32_t num_dst_sge,
                                  LoopbackDataPattern pattern)
{
    uint32_t src_idx = 0, dst_idx = 0;
    uint32_t src_offset = 0, dst_offset = 0;
    uint32_t total_copied = 0;
    void *src_host = NULL, *dst_host = NULL;
    uint64_t src_mapped_len = 0, dst_mapped_len = 0;
    int ret = 0;

    while (src_idx < num_src_sge && dst_idx < num_dst_sge) {
        /* Map source buffer if needed */
        if (!src_host || src_offset >= src_mapped_len) {
            if (src_host) {
                rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
                src_host = NULL;
            }
            src_mapped_len = src_sge[src_idx].length;
            src_host = rdma_pci_dma_map(pci_dev, src_sge[src_idx].addr,
                                        src_mapped_len);
            if (!src_host) {
                rdma_error_report(
                    "Loopback: Failed to map source SGE[%u] addr=%#lx len=%u",
                    src_idx, (unsigned long)src_sge[src_idx].addr,
                    src_sge[src_idx].length);
                ret = -1;
                goto out;
            }
            src_offset = 0;
        }

        /* Map destination buffer if needed */
        if (!dst_host || dst_offset >= dst_mapped_len) {
            if (dst_host) {
                /* Sync writes before unmapping */
                pci_dma_sync(pci_dev,
                             (dma_addr_t)(uintptr_t)dst_sge[dst_idx - 1].addr,
                             dst_mapped_len);
                rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
                dst_host = NULL;
            }
            dst_mapped_len = dst_sge[dst_idx].length;
            dst_host = rdma_pci_dma_map(pci_dev, dst_sge[dst_idx].addr,
                                        dst_mapped_len);
            if (!dst_host) {
                rdma_error_report(
                    "Loopback: Failed to map dest SGE[%u] addr=%#lx len=%u",
                    dst_idx, (unsigned long)dst_sge[dst_idx].addr,
                    dst_sge[dst_idx].length);
                ret = -1;
                goto out;
            }
            dst_offset = 0;
        }

        /* Calculate copy length */
        uint32_t src_remaining = src_mapped_len - src_offset;
        uint32_t dst_remaining = dst_mapped_len - dst_offset;
        uint32_t copy_len =
            (src_remaining < dst_remaining) ? src_remaining : dst_remaining;

        /* Copy data */
        if (pattern == LOOPBACK_DATA_PATTERN_PRESERVE) {
            /* Copy actual data from source */
            memcpy((uint8_t *)dst_host + dst_offset,
                   (uint8_t *)src_host + src_offset, copy_len);
        } else {
            /* Generate pattern data */
            generate_data_pattern((uint8_t *)dst_host + dst_offset, copy_len,
                                  pattern);
        }

        total_copied += copy_len;
        src_offset += copy_len;
        dst_offset += copy_len;

        /* Move to next SGE if current one exhausted */
        if (src_offset >= src_mapped_len) {
            rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
            src_host = NULL;
            src_idx++;
            src_offset = 0;
        }
        if (dst_offset >= dst_mapped_len) {
            /* Sync writes before unmapping */
            pci_dma_sync(pci_dev, (dma_addr_t)(uintptr_t)dst_sge[dst_idx].addr,
                         dst_mapped_len);
            rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
            dst_host = NULL;
            dst_idx++;
            dst_offset = 0;
        }
    }

    /* Sync and unmap remaining buffers */
    if (src_host) {
        rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    }
    if (dst_host) {
        pci_dma_sync(pci_dev, (dma_addr_t)(uintptr_t)dst_sge[dst_idx].addr,
                     dst_mapped_len);
        rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
    }

    return total_copied;

out:
    /* Cleanup on error */
    if (src_host) {
        rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    }
    if (dst_host) {
        rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
    }
    return ret;
}

static void *loopback_translate_addr(PCIDevice *pci_dev, uint64_t guest_addr,
                                     uint64_t len);

/*
 * Helper: Copy data from source SGEs to a single remote address (RDMA
 * Write/Read) Returns number of bytes copied, or -1 on error
 */
static int loopback_copy_to_remote_addr(
    PCIDevice *pci_dev, struct ibv_sge *src_sge, uint32_t num_src_sge,
    uint64_t remote_addr, uint32_t total_len, LoopbackDataPattern pattern)
{
    uint32_t src_idx = 0;
    uint32_t src_offset = 0;
    uint32_t remote_offset = 0;
    uint32_t total_copied = 0;
    void *src_host = NULL, *dst_host = NULL;
    uint64_t src_mapped_len = 0;
    uint64_t dst_mapped_len = 0;
    int ret = 0;
    bool dst_is_mr = false, src_is_mr = false;

    /* Map remote address via MR or DMA */
    dst_mapped_len = total_len;
    dst_host = loopback_translate_addr(pci_dev, remote_addr, dst_mapped_len);
    if (!dst_host) {
        rdma_error_report("Loopback: Failed to map remote "
                          "addr=%#lx len=%u",
                          (unsigned long)remote_addr, total_len);
        return -1;
    }

    while (src_idx < num_src_sge && remote_offset < total_len) {
        if (!src_host || src_offset >= src_mapped_len) {
            src_host = NULL;
            src_mapped_len = src_sge[src_idx].length;
            src_host = loopback_translate_addr(pci_dev, src_sge[src_idx].addr,
                                               src_mapped_len);
            if (!src_host) {
                rdma_error_report(
                    "Loopback: Failed to map source SGE[%u] addr=%#lx len=%u",
                    src_idx, (unsigned long)src_sge[src_idx].addr,
                    src_sge[src_idx].length);
                ret = -1;
                goto out;
            }
            src_offset = 0;
        }

        /* Calculate copy length */
        uint32_t src_remaining = src_mapped_len - src_offset;
        uint32_t dst_remaining = total_len - remote_offset;
        uint32_t copy_len =
            (src_remaining < dst_remaining) ? src_remaining : dst_remaining;

        /* Copy data */
        if (pattern == LOOPBACK_DATA_PATTERN_PRESERVE) {
            /* Copy actual data from source */
            memcpy((uint8_t *)dst_host + remote_offset,
                   (uint8_t *)src_host + src_offset, copy_len);
        } else {
            /* Generate pattern data */
            generate_data_pattern((uint8_t *)dst_host + remote_offset, copy_len,
                                  pattern);
        }

        total_copied += copy_len;
        src_offset += copy_len;
        remote_offset += copy_len;

        /* Move to next SGE if current one exhausted */
        if (src_offset >= src_mapped_len) {
            rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
            src_host = NULL;
            src_idx++;
            src_offset = 0;
        }
    }

    /* Sync writes to remote address */
    pci_dma_sync(pci_dev, remote_addr, dst_mapped_len);
    rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);

    if (src_host) {
        rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    }

    return total_copied;

out:
    /* Cleanup on error */
    if (src_host) {
        rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    }
    if (dst_host) {
        rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
    }
    return ret;
}

/*
 * Helper: Copy data from remote address to destination SGEs (RDMA Read)
 * Returns number of bytes copied, or -1 on error
 */
static int loopback_copy_from_remote_addr(
    PCIDevice *pci_dev, uint64_t remote_addr, uint32_t total_len,
    struct ibv_sge *dst_sge, uint32_t num_dst_sge, LoopbackDataPattern pattern)
{
    uint32_t dst_idx = 0;
    uint32_t dst_offset = 0;
    uint32_t remote_offset = 0;
    uint32_t total_copied = 0;
    void *src_host = NULL, *dst_host = NULL;
    uint64_t src_mapped_len = 0, dst_mapped_len = 0;
    int ret = 0;

    /* Map remote address via MR or DMA */
    src_mapped_len = total_len;
    src_host = loopback_translate_addr(pci_dev, remote_addr, src_mapped_len);
    if (!src_host) {
        rdma_error_report("Loopback: Failed to map remote "
                          "addr=%#lx len=%u",
                          (unsigned long)remote_addr, total_len);
        return -1;
    }

    while (dst_idx < num_dst_sge && remote_offset < total_len) {
        if (!dst_host || dst_offset >= dst_mapped_len) {
            dst_host = NULL;
            dst_mapped_len = dst_sge[dst_idx].length;
            dst_host = loopback_translate_addr(pci_dev, dst_sge[dst_idx].addr,
                                               dst_mapped_len);
            if (!dst_host) {
                rdma_error_report(
                    "Loopback: Failed to map dest SGE[%u] addr=%#lx len=%u",
                    dst_idx, (unsigned long)dst_sge[dst_idx].addr,
                    dst_sge[dst_idx].length);
                ret = -1;
                goto out;
            }
            dst_offset = 0;
        }

        /* Calculate copy length */
        uint32_t src_remaining = total_len - remote_offset;
        uint32_t dst_remaining = dst_mapped_len - dst_offset;
        uint32_t copy_len =
            (src_remaining < dst_remaining) ? src_remaining : dst_remaining;

        /* Copy data */
        if (pattern == LOOPBACK_DATA_PATTERN_PRESERVE) {
            /* Copy actual data from remote */
            memcpy((uint8_t *)dst_host + dst_offset,
                   (uint8_t *)src_host + remote_offset, copy_len);
        } else {
            /* Generate pattern data */
            generate_data_pattern((uint8_t *)dst_host + dst_offset, copy_len,
                                  pattern);
        }

        total_copied += copy_len;
        remote_offset += copy_len;
        dst_offset += copy_len;

        /* Move to next SGE if current one exhausted */
        if (dst_offset >= dst_mapped_len) {
            /* Sync writes before unmapping */
            pci_dma_sync(pci_dev, (dma_addr_t)(uintptr_t)dst_sge[dst_idx].addr,
                         dst_mapped_len);
            rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
            dst_host = NULL;
            dst_idx++;
            dst_offset = 0;
        }
    }

    /* Sync and unmap remaining buffers */
    rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    if (dst_host) {
        pci_dma_sync(pci_dev, (dma_addr_t)(uintptr_t)dst_sge[dst_idx].addr,
                     dst_mapped_len);
        rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
    }

    return total_copied;

out:
    /* Cleanup on error */
    if (src_host) {
        rdma_pci_dma_unmap(pci_dev, src_host, src_mapped_len);
    }
    if (dst_host) {
        rdma_pci_dma_unmap(pci_dev, dst_host, dst_mapped_len);
    }
    return ret;
}

static void compute_sge_md5(PCIDevice *pci_dev, struct ibv_sge *sge,
                            uint32_t num_sge, char *md5_str, size_t md5_str_len)
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
    uint32_t total_len = 0;
    void *host_addr = NULL;
    uint64_t mapped_len = 0;

    /* Compute MD5 over all SGE data by mapping each SGE */
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        if (sge[i].addr && sge[i].length > 0) {
            mapped_len = sge[i].length;
            host_addr = rdma_pci_dma_map(pci_dev, sge[i].addr, mapped_len);
            if (host_addr) {
                g_checksum_update(checksum, (const guchar *)host_addr,
                                  sge[i].length);
                total_len += sge[i].length;
                rdma_pci_dma_unmap(pci_dev, host_addr, mapped_len);
            } else {
                rdma_warn_report(
                    "Loopback: Failed to map SGE[%u] for MD5, addr=%#lx", i,
                    (unsigned long)sge[i].addr);
            }
        }
    }

    /* Get MD5 hex string */
    const gchar *md5_hex = g_checksum_get_string(checksum);
    snprintf(md5_str, md5_str_len, "%s", md5_hex);

    g_checksum_free(checksum);

    rdma_info_report("Loopback: Data MD5: %s (%u bytes)", md5_str, total_len);
}

__attribute__((unused)) static void loopback_post_completion(
    LoopbackCQ *cq, uint64_t wr_id, enum ibv_wc_status status,
    uint32_t byte_len, uint32_t qp_num, enum ibv_wc_opcode opcode)
{
    LoopbackCompletion *comp = g_new0(LoopbackCompletion, 1);

    comp->status = status;
    comp->wr_id = wr_id;
    comp->byte_len = byte_len;
    comp->qp_num = qp_num;
    comp->opcode = opcode;

    qemu_mutex_lock(&cq->lock);
    g_queue_push_tail(cq->completions, comp);
    qemu_mutex_unlock(&cq->lock);

    rdma_info_report("Loopback: Posted completion wr_id=%lu status=%d to CQ %u",
                     wr_id, status, cq->handle);
}

/*
 * Backend Lifecycle
 */

static int loopback_init(RdmaBackendDev *backend_dev, const char *config)
{
    LoopbackBackendPrivate *priv;

    rdma_info_report("Loopback backend: Initializing internal emulation");

    priv = g_new0(LoopbackBackendPrivate, 1);

    priv->pds =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->mrs =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->cqs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_free);
    priv->qps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_free);
    priv->qp_pairs = g_hash_table_new(g_direct_hash, g_direct_equal);

    priv->next_pd_handle = 1;
    priv->next_mr_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100; /* Start at 100 to avoid special QPs */

    /* Parse configuration for data pattern and MD5 */
    priv->data_pattern = parse_data_pattern(config);
    priv->compute_md5 = (config && strstr(config, "md5")) ? true : false;

    qemu_mutex_init(&priv->lock);

    backend_dev->backend_private = priv;

    rdma_info_report("Loopback backend: Data pattern='%s', MD5=%s",
                     data_pattern_name(priv->data_pattern),
                     priv->compute_md5 ? "enabled" : "disabled");
    rdma_info_report("Loopback backend: Initialized successfully");
    return 0;
}

static void loopback_fini(RdmaBackendDev *backend_dev)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);

    if (!priv) {
        return;
    }

    rdma_info_report("Loopback backend: Cleaning up");

    g_hash_table_destroy(priv->pds);
    g_hash_table_destroy(priv->mrs);
    g_hash_table_destroy(priv->cqs);
    g_hash_table_destroy(priv->qps);
    g_hash_table_destroy(priv->qp_pairs);

    qemu_mutex_destroy(&priv->lock);

    g_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int loopback_query_port(RdmaBackendDev *backend_dev,
                               struct ibv_port_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->state = IBV_PORT_ACTIVE;
    attr->max_mtu = IBV_MTU_4096;
    attr->active_mtu = IBV_MTU_1024;
    attr->gid_tbl_len = 1;
    attr->port_cap_flags = IBV_PORT_CM_SUP;
    attr->max_msg_sz = 0x80000000;
    attr->pkey_tbl_len = 1;
    attr->active_width = 4; /* 4X */
    attr->active_speed = 4; /* 10 Gbps */
    attr->phys_state = 5;   /* LinkUp */
    return 0;
}

static int loopback_query_device(RdmaBackendDev *backend_dev,
                                 struct ibv_device_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->max_qp = 1024;
    attr->max_qp_wr = 1024;
    attr->max_sge = 32;
    attr->max_cq = 1024;
    attr->max_cqe = 8192;
    attr->max_mr = 1024;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    attr->max_srq = MAX_SRQ;
    attr->max_srq_wr = 1024;
    attr->max_srq_sge = 32;
    return 0;
}

/*
 * Protection Domain Operations
 */

static int loopback_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackPD *lpd = g_new0(LoopbackPD, 1);

    qemu_mutex_lock(&priv->lock);
    lpd->handle = priv->next_pd_handle++;
    g_hash_table_insert(priv->pds, GUINT_TO_POINTER(lpd->handle), lpd);
    qemu_mutex_unlock(&priv->lock);

    pd->ibpd =
        (struct ibv_pd *)(uintptr_t)lpd->handle; /* Store handle as pointer */

    rdma_info_report("Loopback: Created PD handle %u", lpd->handle);
    return 0;
}

static void loopback_destroy_pd(RdmaBackendPD *pd)
{
    /* Handle stored in ibpd - nothing to free here */
    rdma_info_report("Loopback: Destroyed PD");
}

/*
 * Memory Region Operations
 */

static int loopback_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                              size_t length, uint64_t guest_start, int access)
{
    LoopbackMR *lmr = g_new0(LoopbackMR, 1);
    uint32_t pd_handle = (uint32_t)(uintptr_t)pd->ibpd;

    static uint32_t mr_counter = 1;
    if (!global_mrs_table) {
        global_mrs_table =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }

    lmr->handle = mr_counter++;
    lmr->virt = addr;
    lmr->length = length;
    lmr->guest_start = guest_start;
    lmr->access_flags = access;
    lmr->lkey = lmr->handle;
    lmr->rkey = lmr->handle + 0x10000;
    lmr->pd_handle = pd_handle;

    g_hash_table_insert(global_mrs_table, GUINT_TO_POINTER(lmr->handle), lmr);

    /* Store handle in mr structure */
    mr->ibpd = pd->ibpd;
    mr->ibmr = (struct ibv_mr *)(uintptr_t)lmr->handle;

    rdma_info_report("Loopback: Created MR handle %u, lkey=0x%x, rkey=0x%x, "
                     "len=%zu, virt=%p, guest_start=0x%lx",
                     lmr->handle, lmr->lkey, lmr->rkey, length, addr,
                     (unsigned long)guest_start);
    return 0;
}

/*
 * Translate a guest virtual address to a host
 * virtual address using the MR mapping. Look up
 * the MR by lkey, compute the offset from
 * guest_start, and return virt + offset.
 * Falls back to rdma_pci_dma_map if no MR found.
 */
/*
 * Iterate all MRs to find one containing the
 * guest virtual address. Returns host_virt + offset.
 */
static void *loopback_translate_addr(PCIDevice *pci_dev, uint64_t guest_addr,
                                     uint64_t len)
{
    if (!global_mrs_table)
        goto fallback;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, global_mrs_table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LoopbackMR *lmr = (LoopbackMR *)value;
        if (!lmr->virt)
            continue;
        uint64_t start = lmr->guest_start;
        uint64_t end = start + lmr->length;
        if (guest_addr >= start && guest_addr + len <= end) {
            uint64_t off = guest_addr - start;
            rdma_info_report("Loopback: translate 0x%lx -> "
                             "MR %u virt+0x%lx",
                             (unsigned long)guest_addr, lmr->handle,
                             (unsigned long)off);
            return (uint8_t *)lmr->virt + off;
        }
    }

fallback:
    return rdma_pci_dma_map(pci_dev, guest_addr, len);
}

static void loopback_destroy_mr(RdmaBackendMR *mr)
{
    rdma_info_report("Loopback: Destroyed MR");
}

static uint32_t loopback_mr_lkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    rdma_info_report(">>> loopback_mr_lkey: mr=%p, ibmr=%p, handle=%u", mr,
                     mr->ibmr, handle);
    return handle; /* lkey = handle */
}

static uint32_t loopback_mr_rkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    return handle + 0x10000; /* rkey = handle + offset */
}

/*
 * Completion Queue Operations
 */

static int loopback_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                              int cqe)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackCQ *lcq = g_new0(LoopbackCQ, 1);

    qemu_mutex_lock(&priv->lock);
    lcq->handle = priv->next_cq_handle++;
    qemu_mutex_unlock(&priv->lock);

    lcq->cqe = cqe;
    lcq->completions = g_queue_new();
    qemu_mutex_init(&lcq->lock);

    g_hash_table_insert(priv->cqs, GUINT_TO_POINTER(lcq->handle), lcq);

    cq->backend_dev = backend_dev;
    cq->ibcq = (struct ibv_cq *)(uintptr_t)lcq->handle;

    rdma_info_report("Loopback: Created CQ handle %u with %d entries",
                     lcq->handle, cqe);
    return 0;
}

static void loopback_destroy_cq(RdmaBackendCQ *cq)
{
    uint32_t handle = (uint32_t)(uintptr_t)cq->ibcq;
    rdma_info_report("Loopback: Destroyed CQ handle %u", handle);
    /* Actual cleanup happens in fini */
}

static void loopback_poll_cq(RdmaDeviceResources *rdma_dev_res,
                             RdmaBackendCQ *cq)
{
    /* No-op for now - completions would be polled by driver */
}

/*
 * Queue Pair Operations
 */

static int loopback_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                              RdmaBackendPD *pd, RdmaBackendCQ *scq,
                              RdmaBackendCQ *rcq, RdmaBackendSRQ *srq,
                              uint32_t max_send_wr, uint32_t max_recv_wr,
                              uint32_t max_send_sge, uint32_t max_recv_sge)
{
    LoopbackBackendPrivate *priv = get_private(scq->backend_dev);
    LoopbackQP *lqp = g_new0(LoopbackQP, 1);
    LoopbackCQ *lscq, *lrcq;

    qemu_mutex_lock(&priv->lock);
    lqp->qpn = priv->next_qpn++;
    qemu_mutex_unlock(&priv->lock);

    lqp->qp_type = qp_type;
    lqp->state = IBV_QPS_RESET;
    lqp->pd_handle = (uint32_t)(uintptr_t)pd->ibpd;

    /* Get CQ handles */
    lscq = g_hash_table_lookup(
        priv->cqs, GUINT_TO_POINTER((uint32_t)(uintptr_t)scq->ibcq));
    lrcq = g_hash_table_lookup(
        priv->cqs, GUINT_TO_POINTER((uint32_t)(uintptr_t)rcq->ibcq));

    lqp->scq = lscq;
    lqp->rcq = lrcq;
    lqp->backend_dev = scq->backend_dev; /* Store for auto-pairing */

    lqp->send_queue = g_queue_new();
    lqp->recv_queue = g_queue_new();
    qemu_mutex_init(&lqp->lock);

    g_hash_table_insert(priv->qps, GUINT_TO_POINTER(lqp->qpn), lqp);

    qp->ibpd = pd->ibpd;
    /* Store the actual LoopbackQP pointer, not the QPN! */
    qp->ibqp = (struct ibv_qp *)lqp;
    qp->sgid_idx = 0;

    rdma_info_report("Loopback: Created QP %u type=%d (stored lqp=%p as ibqp)",
                     lqp->qpn, qp_type, lqp);
    return 0;
}

static void loopback_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    uint32_t qpn = lqp ? lqp->qpn : 0;
    rdma_info_report("Loopback: Destroyed QP %u", qpn);
}

static uint32_t loopback_qpn(const RdmaBackendQP *qp)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    return lqp ? lqp->qpn : 0;
}

/*
 * QP State Transitions
 */

static int loopback_qp_state_init(RdmaBackendDev *backend_dev,
                                  RdmaBackendQP *qp, uint8_t qp_type,
                                  uint32_t qkey)
{
    (void)backend_dev;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    if (lqp) {
        lqp->state = IBV_QPS_INIT;
        lqp->qkey = qkey;
        rdma_info_report("Loopback: QP %u -> INIT", lqp->qpn);
    }
    return 0;
}

static int loopback_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                                 uint8_t qp_type, uint8_t sgid_idx,
                                 union ibv_gid *dgid, uint32_t dqpn,
                                 uint32_t rq_psn, uint32_t qkey, bool qkey_set)
{
    (void)backend_dev;
    (void)sgid_idx;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    /* DCT reaches RTR as its penultimate arming step; no remote QPN needed. */
    if (qp_type == IBV_QPT_DRIVER) {
        if (lqp) {
            lqp->state = IBV_QPS_RTR;
            rdma_info_report("Loopback: DCT QP %u -> RTR (armed)", lqp->qpn);
        }
        return 0;
    }

    if (lqp) {
        lqp->state = IBV_QPS_RTR;
        lqp->remote_qpn = dqpn;
        if (dgid) {
            memcpy(&lqp->remote_gid, dgid, sizeof(union ibv_gid));
        }
        lqp->rq_psn = rq_psn;
        if (qkey_set) {
            lqp->qkey = qkey;
        }

        /* Initialize local connection info if not set */
        /* These will be exchanged during auto-pairing */
        /* Use QPN-based addressing for unique addresses per QP */
        if (lqp->local_addr == 0) {
            lqp->local_addr = 0x1000000 + (lqp->qpn * 0x1000);
        }
        if (lqp->local_rkey == 0) {
            lqp->local_rkey = 0xFFFFFFFF; /* Default rkey */
        }

        rdma_info_report("Loopback: QP %u -> RTR (remote_qpn=%u, rq_psn=%u, "
                         "local_addr=0x%lx, local_rkey=0x%x)",
                         lqp->qpn, dqpn, rq_psn, (unsigned long)lqp->local_addr,
                         lqp->local_rkey);
    }
    return 0;
}

/*
 * Auto-pair QPs for rdma_cm simulation
 * When a QP reaches RTS without a remote_qpn, try to pair it with another
 * unpaired QP in RTS state. This simulates rdma_cm connection establishment.
 */
static void loopback_auto_pair_qp(LoopbackBackendPrivate *priv, LoopbackQP *lqp)
{
    GHashTableIter iter;
    gpointer key, value;
    LoopbackQP *other_qp;
    uint32_t other_qpn;

    if (!priv || !lqp) {
        return;
    }

    /* Only auto-pair RC/UC QPs */
    if (lqp->qp_type != IBV_QPT_RC && lqp->qp_type != IBV_QPT_UC) {
        return;
    }

    /* If remote_qpn is set to self (self-loopback), clear it for auto-pairing
     */
    if (lqp->remote_qpn == lqp->qpn) {
        lqp->remote_qpn = 0;
    }

    /* Skip if already paired with another QP */
    if (lqp->remote_qpn != 0) {
        return;
    }

    /* Find another QP in RTS state without a remote_qpn */
    /* Prefer pairing with QPs that have similar QPNs (likely created together)
     */
    qemu_mutex_lock(&priv->lock);

    /* First pass: look for QPs with similar QPNs (within 10) */
    LoopbackQP *best_match = NULL;
    uint32_t best_qpn = 0;
    int32_t best_distance = INT32_MAX;

    g_hash_table_iter_init(&iter, priv->qps);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        other_qpn = GPOINTER_TO_UINT(key);
        other_qp = (LoopbackQP *)value;

        /* Skip self */
        if (other_qp == lqp || other_qpn == lqp->qpn) {
            continue;
        }

        /* Match: same type, in RTS, no remote_qpn set */
        if (other_qp->qp_type == lqp->qp_type &&
            other_qp->state == IBV_QPS_RTS && other_qp->remote_qpn == 0) {
            /* Prefer QPs with similar QPNs (created close together) */
            int32_t distance = abs((int32_t)other_qpn - (int32_t)lqp->qpn);
            if (distance < best_distance) {
                best_match = other_qp;
                best_qpn = other_qpn;
                best_distance = distance;
            }
        }
    }

    /* If we found a match, use it */
    if (best_match) {
        other_qp = best_match;
        other_qpn = best_qpn;

        /* Pair them bidirectionally */
        lqp->remote_qpn = other_qpn;
        other_qp->remote_qpn = lqp->qpn;

        /* Exchange PSNs for proper sequencing */
        if (lqp->sq_psn == 0) {
            lqp->sq_psn = other_qp->rq_psn;
        }
        if (other_qp->sq_psn == 0) {
            other_qp->sq_psn = lqp->rq_psn;
        }

        /* Exchange connection info for Ethernet key exchange simulation */
        /* Ensure both QPs have local info set first */
        /* Use QPN-based addressing for unique addresses */
        if (lqp->local_addr == 0) {
            lqp->local_addr = 0x1000000 + (lqp->qpn * 0x1000);
        }
        if (lqp->local_rkey == 0) {
            lqp->local_rkey = 0xFFFFFFFF;
        }
        if (other_qp->local_addr == 0) {
            other_qp->local_addr = 0x1000000 + (other_qp->qpn * 0x1000);
        }
        if (other_qp->local_rkey == 0) {
            other_qp->local_rkey = 0xFFFFFFFF;
        }

        /* Exchange: each QP gets the other's local info as remote */
        lqp->remote_addr = other_qp->local_addr;
        lqp->remote_rkey = other_qp->local_rkey;
        other_qp->remote_addr = lqp->local_addr;
        other_qp->remote_rkey = lqp->local_rkey;

        rdma_info_report(
            "Loopback: Auto-paired QP %u <-> QP %u (simulating rdma_cm, "
            "distance=%d) "
            "[QP%u: remote_addr=0x%lx, remote_rkey=0x%x] "
            "[QP%u: remote_addr=0x%lx, remote_rkey=0x%x]",
            lqp->qpn, other_qpn, best_distance, lqp->qpn,
            (unsigned long)lqp->remote_addr, lqp->remote_rkey, other_qpn,
            (unsigned long)other_qp->remote_addr, other_qp->remote_rkey);
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    /* No match found - QP will wait for a partner */
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("Loopback: QP %u in RTS, waiting for pairing partner",
                     lqp->qpn);
}

static int loopback_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                                 uint32_t sq_psn, uint32_t qkey, bool qkey_set)
{
    (void)qkey;
    (void)qkey_set;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    LoopbackBackendPrivate *priv = NULL;

    if (!lqp) {
        return 0;
    }

    lqp->state = IBV_QPS_RTS;
    lqp->sq_psn = sq_psn;

    /* DC QPs use per-send destination addressing; no connection pairing. */
    if (qp_type == IBV_QPT_DRIVER) {
        rdma_info_report("Loopback: DC QP %u -> RTS (sq_psn=%u, no pairing)",
                         lqp->qpn, sq_psn);
        return 0;
    }

    rdma_info_report("Loopback: QP %u -> RTS (sq_psn=%u)", lqp->qpn, sq_psn);

    /* Try to auto-pair this QP with another unpaired QP */
    if (lqp->backend_dev) {
        priv = get_private(lqp->backend_dev);
        if (priv) {
            loopback_auto_pair_qp(priv, lqp);
        }
    }

    return 0;
}

/*
 * Query remote connection info for Ethernet key exchange simulation
 * Returns remote_addr and rkey if QP is paired
 */
static void loopback_query_remote_conn_info(RdmaBackendQP *qp,
                                            uint64_t *remote_addr,
                                            uint32_t *rkey)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    if (lqp && lqp->remote_qpn != 0) {
        /* QP is paired - return connection info */
        if (remote_addr) {
            *remote_addr = lqp->remote_addr;
        }
        if (rkey) {
            *rkey = lqp->remote_rkey;
        }
        rdma_info_report("Loopback: Query remote conn info QP %u -> "
                         "remote_qpn=%u, remote_addr=0x%lx, remote_rkey=0x%x",
                         lqp->qpn, lqp->remote_qpn,
                         remote_addr ? (unsigned long)*remote_addr : 0,
                         rkey ? *rkey : 0);
    } else {
        /* QP not paired yet - return zeros */
        if (remote_addr) {
            *remote_addr = 0;
        }
        if (rkey) {
            *rkey = 0;
        }
        if (lqp) {
            rdma_info_report("Loopback: Query remote conn info QP %u -> "
                             "not paired (remote_qpn=0)",
                             lqp->qpn);
        }
    }
}

static int loopback_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                             int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    memset(attr, 0, sizeof(*attr));
    if (lqp) {
        attr->qp_state = lqp->state;
        attr->cur_qp_state = lqp->state;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_REMOTE_READ;
        if (lqp->remote_qpn != 0) {
            attr->dest_qp_num = lqp->remote_qpn;
        }
        /* Connection info is exposed via query_remote_conn_info */
        /* This is called separately by the PVRDMA layer */
    } else {
        attr->qp_state = IBV_QPS_RTS;
        attr->cur_qp_state = IBV_QPS_RTS;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags =
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    }

    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
    }
    return 0;
}

/*
 * Shared Receive Queue (software path for DC targets and SRQ verbs).
 */
static int loopback_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                               uint32_t max_wr, uint32_t max_sge,
                               uint32_t srq_limit)
{
    LoopbackSRQ *lsrq = g_new0(LoopbackSRQ, 1);
    uint32_t pd_handle = (uint32_t)(uintptr_t)pd->ibpd;

    lsrq->max_wr = max_wr;
    lsrq->max_sge = max_sge;
    lsrq->srq_limit = srq_limit;
    lsrq->pd_handle = pd_handle;
    lsrq->recv_queue = g_queue_new();
    qemu_mutex_init(&lsrq->lock);
    srq->ibsrq = (struct ibv_srq *)(uintptr_t)lsrq;
    rdma_info_report("Loopback: Created SRQ (max_wr=%u max_sge=%u)", max_wr,
                     max_sge);
    return 0;
}

static void loopback_destroy_srq(RdmaBackendSRQ *srq)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq) {
        return;
    }
    qemu_mutex_lock(&lsrq->lock);
    while (!g_queue_is_empty(lsrq->recv_queue)) {
        LoopbackWR *wr = g_queue_pop_head(lsrq->recv_queue);

        if (wr->wr_id) {
            g_free((PvrdmaCompHandlerCtx *)(uintptr_t)wr->wr_id);
        }
        g_free(wr);
    }
    g_queue_free(lsrq->recv_queue);
    qemu_mutex_unlock(&lsrq->lock);
    qemu_mutex_destroy(&lsrq->lock);
    g_free(lsrq);
    srq->ibsrq = NULL;
    rdma_info_report("Loopback: Destroyed SRQ");
}

static int loopback_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq || !attr) {
        return -EINVAL;
    }
    attr->max_wr = lsrq->max_wr;
    attr->max_sge = lsrq->max_sge;
    attr->srq_limit = lsrq->srq_limit;
    return 0;
}

static int loopback_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr,
                               int mask)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq || !attr) {
        return -EINVAL;
    }
    if (mask & IBV_SRQ_LIMIT) {
        lsrq->srq_limit = attr->srq_limit;
    }
    return 0;
}

static void loopback_post_srq_recv(RdmaBackendSRQ *srq, struct ibv_sge *sge,
                                   uint32_t num_sge, void *ctx)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;
    LoopbackWR *recv_wr;

    if (!lsrq) {
        rdma_error_report("Loopback: post_srq_recv on unknown SRQ");
        return;
    }

    recv_wr = g_new0(LoopbackWR, 1);
    recv_wr->wr_id = (uint64_t)(uintptr_t)ctx;
    recv_wr->num_sge = (num_sge < 32) ? num_sge : 32;

    for (uint32_t i = 0; i < recv_wr->num_sge; i++) {
        recv_wr->sge[i].addr = (void *)sge[i].addr;
        recv_wr->sge[i].length = sge[i].length;
        recv_wr->sge[i].lkey = sge[i].lkey;
    }

    qemu_mutex_lock(&lsrq->lock);
    g_queue_push_tail(lsrq->recv_queue, recv_wr);
    qemu_mutex_unlock(&lsrq->lock);

    rdma_info_report("Loopback: Posted SRQ recv (%u SGEs)", recv_wr->num_sge);
}

/*
 * Data Path Operations
 */

static void loopback_post_send(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                               uint8_t qp_type, struct ibv_sge *sge,
                               uint32_t num_sge, uint8_t sgid_idx,
                               union ibv_gid *sgid, union ibv_gid *dgid,
                               uint32_t dqpn, uint32_t dqkey, void *ctx)
{
    (void)qp_type;
    (void)sgid_idx;
    (void)sgid;
    (void)dgid;
    (void)dqkey;
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    rdma_info_report(
        ">>> Loopback: post_send ENTRY: qp=%p, lqp=%p, num_sge=%u, ctx=%p", qp,
        lqp, num_sge, ctx);
    LoopbackQP *remote_qp = NULL;
    LoopbackWR *recv_wr = NULL;
    uint32_t total_len = 0;
    uint32_t transferred = 0;
    enum ibv_wc_opcode wc_opcode = IBV_WC_SEND;
    uint32_t pvrdma_opcode = 0;
    uint64_t remote_addr = 0;
    uint32_t rkey = 0;
    bool write_failed = false;

    if (!lqp) {
        rdma_error_report("Loopback: post_send on unknown QP");
        return;
    }
    rdma_info_report(">>> Loopback: post_send: lqp->qpn=%u, lqp->state=%d",
                     lqp->qpn, lqp->state);

    /* Extract opcode and RDMA parameters from context if available */
    PvrdmaCompHandlerCtx *comp_ctx = (PvrdmaCompHandlerCtx *)ctx;
    uint32_t pvrdma_qp_handle = 0; /* PVRDMA QP handle for stats */
    if (comp_ctx) {
        pvrdma_opcode = comp_ctx->opcode;
        remote_addr = comp_ctx->remote_addr;
        rkey = comp_ctx->rkey;
        wc_opcode = comp_ctx->cqe.opcode;
        pvrdma_qp_handle =
            comp_ctx->cqe.qp; /* QP handle from completion context */
        rdma_info_report(">>> Loopback: post_send: Extracted "
                         "pvrdma_qp_handle=%u from comp_ctx",
                         pvrdma_qp_handle);
    } else {
        rdma_warn_report(
            ">>> Loopback: post_send: comp_ctx is NULL, cannot get QP handle");
    }

    /* Calculate total send length */
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        total_len += sge[i].length;
    }

    /* Get PCIDevice for DMA operations */
    PCIDevice *pci_dev = backend_dev->dev;
    if (!pci_dev) {
        rdma_error_report(
            "Loopback: No PCIDevice available for DMA operations");
        return;
    }
    rdma_info_report(
        ">>> Loopback: post_send: pci_dev=%p, total_len=%u, pvrdma_opcode=%u",
        pci_dev, total_len, pvrdma_opcode);

    if (comp_ctx && pvrdma_opcode == PVRDMA_WR_SEND_DC &&
        comp_ctx->dc_target_srq) {
        PVRDMADev *pdev = PVRDMA_DEV(pci_dev);
        LoopbackSRQ *lsrq =
            (LoopbackSRQ *)(uintptr_t)comp_ctx->dc_target_srq->ibsrq;
        uint32_t recv_byte_len;

        recv_wr = NULL;
        if (lsrq) {
            qemu_mutex_lock(&lsrq->lock);
            recv_wr = g_queue_pop_head(lsrq->recv_queue);
            qemu_mutex_unlock(&lsrq->lock);
        }
        if (!recv_wr) {
            recv_wr = g_new0(LoopbackWR, 1);
            recv_wr->wr_id = 0;
            recv_wr->num_sge = 1;
            recv_wr->sge[0].addr = (void *)0x1000000;
            recv_wr->sge[0].length = total_len;
            recv_wr->sge[0].lkey = 0xffffffff;
        }

        transferred = total_len;
        if (recv_wr->wr_id != 0) {
            PvrdmaCompHandlerCtx *rctx =
                (PvrdmaCompHandlerCtx *)(uintptr_t)recv_wr->wr_id;
            struct ibv_sge recv_sge[32];
            int copy_result;

            for (uint32_t i = 0; i < recv_wr->num_sge && i < 32; i++) {
                recv_sge[i].addr = (uint64_t)(uintptr_t)recv_wr->sge[i].addr;
                recv_sge[i].length = recv_wr->sge[i].length;
                recv_sge[i].lkey = recv_wr->sge[i].lkey;
            }
            copy_result =
                loopback_copy_sge_data(pci_dev, sge, num_sge, recv_sge,
                                       recv_wr->num_sge, priv->data_pattern);
            if (copy_result >= 0) {
                transferred = (uint32_t)copy_result;
            } else {
                transferred = 0;
            }
            recv_byte_len = (transferred > 0) ? transferred : total_len;
            pvrdma_queue_recv_work_completion(pdev, comp_ctx->dc_recv_cq_handle,
                                              rctx->cqe.wr_id, recv_byte_len,
                                              comp_ctx->dc_src_qp);
            if (recv_byte_len > 0 && pvrdma_qp_handle > 0) {
                loopback_update_byte_stats(backend_dev, pvrdma_qp_handle,
                                           recv_byte_len, IBV_WC_RECV);
            }
            g_free(rctx);
        }
        g_free(recv_wr);
        recv_wr = NULL;
        wc_opcode = IBV_WC_SEND;
        if (pvrdma_qp_handle > 0) {
            uint32_t stats_bytes = (transferred > 0) ? transferred : total_len;

            loopback_update_byte_stats(backend_dev, pvrdma_qp_handle,
                                       stats_bytes, IBV_WC_SEND);
        }
        goto post_send_finalize;
    }

    /* Handle RDMA Read operations */
    if (pvrdma_opcode == PVRDMA_WR_RDMA_READ ||
        pvrdma_opcode == PVRDMA_WR_RDMA_READ_WITH_INV) {
        /* RDMA Read: read from remote memory, write to local SGEs */
        /* If remote_addr/rkey not provided, use values from paired QP */
        if (remote_addr == 0 && lqp->remote_addr != 0) {
            remote_addr = lqp->remote_addr;
            rdma_info_report(
                "Loopback: RDMA READ using paired QP remote_addr=0x%lx",
                (unsigned long)remote_addr);
        }
        if (rkey == 0 && lqp->remote_rkey != 0) {
            rkey = lqp->remote_rkey;
            rdma_info_report("Loopback: RDMA READ using paired QP rkey=0x%x",
                             rkey);
        }

        rdma_info_report(
            "Loopback: RDMA READ QP %u: %u bytes, remote_addr=0x%lx, "
            "rkey=0x%x",
            lqp->qpn, total_len, (unsigned long)remote_addr, rkey);

        /* Copy data from remote address to destination SGEs */
        if (remote_addr != 0) {
            /* Convert destination SGEs from ibv_sge to struct ibv_sge */
            struct ibv_sge dst_sge[32];
            for (uint32_t i = 0; i < num_sge && i < 32; i++) {
                dst_sge[i].addr = sge[i].addr;
                dst_sge[i].length = sge[i].length;
                dst_sge[i].lkey = sge[i].lkey;
            }
            int copy_result = loopback_copy_from_remote_addr(
                pci_dev, remote_addr, total_len, dst_sge, num_sge,
                priv->data_pattern);
            if (copy_result < 0) {
                rdma_error_report("Loopback: RDMA READ copy failed, QP %u",
                                  lqp->qpn);
                transferred = 0;
            } else {
                transferred = (uint32_t)copy_result;
            }
        } else {
            rdma_warn_report("Loopback: RDMA READ with zero remote_addr, QP %u",
                             lqp->qpn);
            transferred = 0;
        }

        wc_opcode = IBV_WC_RDMA_READ;

        /* Update byte statistics */
        /* Use transferred if > 0, otherwise use total_len */
        if (pvrdma_qp_handle > 0) {
            uint32_t stats_bytes = (transferred > 0) ? transferred : total_len;
            rdma_info_report(">>> Loopback: Calling loopback_update_byte_stats "
                             "for RDMA_READ: "
                             "qp_handle=%u, stats_bytes=%u",
                             pvrdma_qp_handle, stats_bytes);
            loopback_update_byte_stats(backend_dev, pvrdma_qp_handle,
                                       stats_bytes, IBV_WC_RDMA_READ);
        } else {
            rdma_warn_report(">>> Loopback: Skipping stats update for "
                             "RDMA_READ: pvrdma_qp_handle=0");
        }
    } else if (pvrdma_opcode == PVRDMA_WR_RDMA_WRITE ||
               pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM) {
        /* RDMA Write: write from local SGEs to remote memory */
        /* If remote_addr/rkey not provided, use values from paired QP */
        if (remote_addr == 0 && lqp->remote_addr != 0) {
            remote_addr = lqp->remote_addr;
            rdma_info_report(
                "Loopback: RDMA WRITE using paired QP remote_addr=0x%lx",
                (unsigned long)remote_addr);
        }
        if (rkey == 0 && lqp->remote_rkey != 0) {
            rkey = lqp->remote_rkey;
            rdma_info_report("Loopback: RDMA WRITE using paired QP rkey=0x%x",
                             rkey);
        }

        rdma_info_report(
            "Loopback: RDMA WRITE QP %u: %u bytes, remote_addr=0x%lx, "
            "rkey=0x%x",
            lqp->qpn, total_len, (unsigned long)remote_addr, rkey);

        /* Copy data from source SGEs to remote address */
        if (remote_addr != 0) {
            int copy_result =
                loopback_copy_to_remote_addr(pci_dev, sge, num_sge, remote_addr,
                                             total_len, priv->data_pattern);
            if (copy_result < 0) {
                rdma_error_report("Loopback: RDMA WRITE copy failed, QP %u",
                                  lqp->qpn);
                transferred = 0;
                write_failed = true;
            } else {
                transferred = (uint32_t)copy_result;
            }
        } else {
            rdma_warn_report(
                "Loopback: RDMA WRITE with zero remote_addr, QP %u", lqp->qpn);
            transferred = 0;
            write_failed = true;
        }

        wc_opcode = IBV_WC_RDMA_WRITE;

        /* Update byte statistics */
        /* Use transferred if > 0, otherwise use total_len */
        if (pvrdma_qp_handle > 0 && !write_failed) {
            uint32_t stats_bytes = (transferred > 0) ? transferred : total_len;
            rdma_info_report(">>> Loopback: Calling loopback_update_byte_stats "
                             "for RDMA_WRITE: "
                             "qp_handle=%u, stats_bytes=%u",
                             pvrdma_qp_handle, stats_bytes);
            loopback_update_byte_stats(backend_dev, pvrdma_qp_handle,
                                       stats_bytes, IBV_WC_RDMA_WRITE);
        } else {
            rdma_warn_report(">>> Loopback: Skipping stats update for "
                             "RDMA_WRITE: handle=%u failed=%d",
                             pvrdma_qp_handle, write_failed);
        }

        /* WRITE_WITH_IMM consumes a receive on the responder, exactly
         * like SEND does on a real HCA: the immediate is delivered to
         * the responder's receive CQ with opcode RECV_RDMA_WITH_IMM
         * and the IBV_WC_WITH_IMM flag. Without this, the responder
         * side never observes the write. Only the QP's own receive
         * queue is consulted on this path; an SRQ-backed responder
         * is not handled for WRITE_WITH_IMM. */
        if (pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM && !write_failed) {
            LoopbackQP *imm_remote = NULL;

            if (lqp->remote_qpn) {
                imm_remote = g_hash_table_lookup(
                    priv->qps, GUINT_TO_POINTER(lqp->remote_qpn));
            } else {
                imm_remote = lqp; /* self-loopback */
            }

            if (imm_remote) {
                qemu_mutex_lock(&imm_remote->lock);
                LoopbackWR *imm_recv = g_queue_pop_head(imm_remote->recv_queue);
                qemu_mutex_unlock(&imm_remote->lock);

                if (imm_recv) {
                    PvrdmaCompHandlerCtx *imm_rctx =
                        (PvrdmaCompHandlerCtx *)(uintptr_t)imm_recv->wr_id;
                    PVRDMADev *pdev = PVRDMA_DEV(pci_dev);

                    if (imm_rctx) {
                        uint32_t imm_len =
                            (transferred > 0) ? transferred : total_len;
                        pvrdma_queue_recv_imm_work_completion(
                            pdev, imm_rctx->cq_handle, imm_rctx->cqe.qp,
                            imm_rctx->cqe.wr_id, imm_len, lqp->qpn,
                            comp_ctx ? comp_ctx->imm_data : 0);
                        rdma_info_report(
                            "Loopback: WRITE_WITH_IMM QP %u -> recv "
                            "QP %u (imm=0x%x, %u bytes)",
                            lqp->qpn, imm_remote->qpn,
                            comp_ctx ? comp_ctx->imm_data : 0, imm_len);
                        if (imm_rctx->cqe.qp > 0 && imm_len > 0) {
                            loopback_update_byte_stats(backend_dev,
                                                       imm_rctx->cqe.qp,
                                                       imm_len, IBV_WC_RECV);
                        }
                        g_free(imm_rctx);
                    }
                    g_free(imm_recv);
                } else {
                    rdma_warn_report(
                        "Loopback: WRITE_WITH_IMM QP %u: no posted "
                        "recv on QP %u, immediate dropped",
                        lqp->qpn, imm_remote->qpn);
                }
            } else {
                rdma_warn_report("Loopback: WRITE_WITH_IMM QP %u: remote QP %u "
                                 "not found, immediate dropped",
                                 lqp->qpn, lqp->remote_qpn);
            }
        }
    } else {
        /* SEND operations: need matching recv */
        /* Compute MD5 of send data if enabled */
        if (priv->compute_md5 && total_len > 0 && num_sge > 0) {
            char md5_str[33];
            compute_sge_md5(pci_dev, sge, num_sge, md5_str, sizeof(md5_str));
            rdma_info_report(
                "Loopback: SEND QP %u: %u bytes, pattern=%s, MD5=%s", lqp->qpn,
                total_len, data_pattern_name(priv->data_pattern), md5_str);
        } else if (total_len > 0) {
            rdma_info_report("Loopback: SEND QP %u: %u bytes, pattern=%s",
                             lqp->qpn, total_len,
                             data_pattern_name(priv->data_pattern));
        }

        /* For UD QP, use dqpn; for connected QPs, use paired remote_qpn */
        if (lqp->qp_type == IBV_QPT_UD) {
            remote_qp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(dqpn));
        } else {
            /* For RC/UC, use the remote_qpn stored in lqp from RTR */
            if (lqp->remote_qpn) {
                remote_qp = g_hash_table_lookup(
                    priv->qps, GUINT_TO_POINTER(lqp->remote_qpn));
            } else {
                /* Self-loopback: use same QP */
                remote_qp = lqp;
            }
        }

        /* Try to match with a recv on remote/local QP */
        if (remote_qp) {
            qemu_mutex_lock(&remote_qp->lock);
            recv_wr = g_queue_pop_head(remote_qp->recv_queue);
            qemu_mutex_unlock(&remote_qp->lock);
        }

        /* Auto-server mode: If no matching receive, automatically create one.
         * This allows single-ended testing (e.g., ib_read_lat) without
         * requiring a separate server process to post receives. */
        if (!recv_wr && remote_qp) {
            /* Create a fake receive work request with a single SGE large
             * enough to hold the send data. This allows single-ended testing
             * without requiring the application to post receives. */
            recv_wr = g_new0(LoopbackWR, 1);
            recv_wr->wr_id = 0; /* Use 0 as a marker for auto-generated recv */
            recv_wr->num_sge = 1;
            recv_wr->sge[0].addr = (void *)0x1000000; /* Dummy address */
            recv_wr->sge[0].length = total_len;       /* Match send size */
            recv_wr->sge[0].lkey = 0xFFFFFFFF;        /* Dummy lkey */

            rdma_info_report(
                "Loopback: Auto-server mode: Created fake recv for QP %u "
                "(%u bytes)",
                remote_qp->qpn, total_len);
        }

        if (recv_wr) {
            /* Copy data from send SGEs to receive SGEs */
            /* Convert receive SGEs from LoopbackSGE to struct ibv_sge */
            struct ibv_sge recv_sge[32];
            for (uint32_t i = 0; i < recv_wr->num_sge && i < 32; i++) {
                recv_sge[i].addr = (uint64_t)(uintptr_t)recv_wr->sge[i].addr;
                recv_sge[i].length = recv_wr->sge[i].length;
                recv_sge[i].lkey = recv_wr->sge[i].lkey;
            }

            /* Only copy if this is a real receive (not auto-generated) */
            if (recv_wr->wr_id != 0) {
                int copy_result = loopback_copy_sge_data(
                    pci_dev, sge, num_sge, recv_sge, recv_wr->num_sge,
                    priv->data_pattern);
                if (copy_result < 0) {
                    rdma_error_report(
                        "Loopback: SEND copy failed, QP %u -> QP %u", lqp->qpn,
                        remote_qp->qpn);
                    transferred = 0;
                } else {
                    transferred = (uint32_t)copy_result;
                }
            } else {
                /* Auto-generated recv: just calculate transfer size */
                uint32_t send_total = 0, recv_total = 0;
                for (uint32_t i = 0; i < num_sge && i < 32; i++) {
                    send_total += sge[i].length;
                }
                for (uint32_t i = 0; i < recv_wr->num_sge && i < 32; i++) {
                    recv_total += recv_sge[i].length;
                }
                transferred =
                    (send_total < recv_total) ? send_total : recv_total;
            }

            /* Post recv completion directly to PVRDMA layer */
            /* Only post recv completion if this wasn't an auto-generated recv
             * (wr_id == 0 means auto-generated, skip completion) */
            if (recv_wr->wr_id != 0) {
                /* For recv completion, byte_len should be the amount received,
                 * which is the send length (total_len), not necessarily the
                 * amount copied (transferred). Use transferred if > 0,
                 * otherwise use total_len. */
                uint32_t recv_byte_len =
                    (transferred > 0) ? transferred : total_len;
                PvrdmaCompHandlerCtx *rctx =
                    (PvrdmaCompHandlerCtx *)(uintptr_t)recv_wr->wr_id;

                rdma_info_report(">>> Loopback: Posting RECV completion: "
                                 "wr_id=%lu, transferred=%u, total_len=%u, "
                                 "recv_byte_len=%u",
                                 (unsigned long)rctx->cqe.wr_id, transferred,
                                 total_len, recv_byte_len);
                pvrdma_queue_recv_work_completion(
                    PVRDMA_DEV(pci_dev), rctx->cq_handle, rctx->cqe.wr_id,
                    recv_byte_len, lqp->qpn);
                g_free(rctx);

                /* Update byte statistics for receive */
                /* Use pvrdma_qp_handle from context, or fallback to
                 * remote_qp->qpn */
                if (recv_byte_len > 0) {
                    uint32_t recv_qp_handle = pvrdma_qp_handle > 0
                                                  ? pvrdma_qp_handle
                                                  : remote_qp->qpn;
                    loopback_update_byte_stats(backend_dev, recv_qp_handle,
                                               recv_byte_len, IBV_WC_RECV);
                }
            }

            g_free(recv_wr);
            rdma_info_report("Loopback: Send QP %u -> Recv QP %u (%u bytes "
                             "transferred)",
                             lqp->qpn, remote_qp->qpn, transferred);
        } else {
            rdma_info_report("Loopback: Send QP %u (no remote QP, %u bytes)",
                             lqp->qpn, total_len);
        }

        /* Update byte statistics for send */
        /* Use transferred if > 0, otherwise use total_len */
        if (pvrdma_qp_handle > 0) {
            uint32_t stats_bytes = (transferred > 0) ? transferred : total_len;
            rdma_info_report(
                ">>> Loopback: Calling loopback_update_byte_stats for SEND: "
                "qp_handle=%u, stats_bytes=%u, transferred=%u, total_len=%u",
                pvrdma_qp_handle, stats_bytes, transferred, total_len);
            loopback_update_byte_stats(backend_dev, pvrdma_qp_handle,
                                       stats_bytes, IBV_WC_SEND);
        } else {
            rdma_warn_report(">>> Loopback: Skipping stats update for SEND: "
                             "pvrdma_qp_handle=0");
        }
    }

post_send_finalize:
    /* Post send completion directly to PVRDMA layer */
    /* Use total_len if transferred is 0 (no data copied or error) */
    uint32_t byte_len = (transferred > 0) ? transferred : total_len;
    enum ibv_wc_status final_status = IBV_WC_SUCCESS;
    if (write_failed) {
        final_status = IBV_WC_LOC_PROT_ERR;
        byte_len = 0;
    }
    rdma_info_report(
        ">>> Loopback: post_send: About to post completion, "
        "transferred=%u, total_len=%u, byte_len=%u, wc_opcode=%d, qpn=%u, "
        "status=%d",
        transferred, total_len, byte_len, wc_opcode, lqp->qpn, final_status);
    rdma_backend_complete_work(final_status, 0, byte_len, lqp->qpn, wc_opcode,
                               ctx);
    rdma_info_report(">>> Loopback: post_send: Completion posted");
}

static void loopback_post_recv(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                               uint8_t qp_type, struct ibv_sge *sge,
                               uint32_t num_sge, void *ctx)
{
    (void)backend_dev;
    (void)qp_type;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    if (!lqp) {
        rdma_error_report("Loopback: post_recv on unknown QP");
        return;
    }

    /* Create and queue the receive work request */
    LoopbackWR *recv_wr = g_new0(LoopbackWR, 1);
    recv_wr->wr_id = (uint64_t)(uintptr_t)ctx;
    recv_wr->num_sge = (num_sge < 32) ? num_sge : 32;

    /* Copy SGE list */
    for (uint32_t i = 0; i < recv_wr->num_sge; i++) {
        recv_wr->sge[i].addr = (void *)sge[i].addr;
        recv_wr->sge[i].length = sge[i].length;
        recv_wr->sge[i].lkey = sge[i].lkey;
    }

    /* Queue the receive work request */
    qemu_mutex_lock(&lqp->lock);
    g_queue_push_tail(lqp->recv_queue, recv_wr);
    qemu_mutex_unlock(&lqp->lock);

    rdma_info_report("Loopback: Posted recv on QP %u (wr_id=0x%lx, %u SGEs)",
                     lqp->qpn, (unsigned long)recv_wr->wr_id, recv_wr->num_sge);
}

/*
 * GID Management
 */

static int loopback_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                            union ibv_gid *gid)
{
    /* Always use deterministic GID format for loopback backend.
     * This ensures consistent GIDs regardless of network interface state. */
    memset(gid, 0, sizeof(*gid));
    gid->raw[0] = 0xfe;
    gid->raw[1] = 0x80;
    gid->raw[8] = 0x02;
    gid->raw[11] = 0xff;
    gid->raw[12] = 0xfe;
    /* Use mesh node ID if mesh is enabled, otherwise 0 */
    gid->raw[15] = backend_dev->mesh_enabled ? backend_dev->mesh_node_id : 0;

    rdma_info_report(
        "Loopback: Set GID to %02x%02x::%02x%02x (mesh=%d node_id=%u)",
        gid->raw[0], gid->raw[1], gid->raw[14], gid->raw[15],
        backend_dev->mesh_enabled, backend_dev->mesh_node_id);
    return 0;
}

static int loopback_del_gid(RdmaBackendDev *backend_dev, const char *ifname,
                            int gid_idx)
{
    rdma_info_report("Loopback: Deleted GID index %d", gid_idx);
    return 0;
}

static int loopback_get_backend_gid_index(RdmaBackendDev *backend_dev,
                                          int sgid_idx)
{
    return sgid_idx; /* Identity mapping */
}

/*
 * Backend Operations Structure
 */
const RdmaBackendOps rdma_backend_ops_loopback = {
    .name = "loopback",
    .type = RDMA_BACKEND_TYPE_LOOPBACK,

    .init = loopback_init,
    .fini = loopback_fini,

    .query_port = loopback_query_port,
    .query_device = loopback_query_device,

    .create_pd = loopback_create_pd,
    .destroy_pd = loopback_destroy_pd,

    .create_mr = loopback_create_mr,
    .destroy_mr = loopback_destroy_mr,
    .mr_lkey = loopback_mr_lkey,
    .mr_rkey = loopback_mr_rkey,

    .create_cq = loopback_create_cq,
    .destroy_cq = loopback_destroy_cq,
    .poll_cq = loopback_poll_cq,

    .create_qp = loopback_create_qp,
    .destroy_qp = loopback_destroy_qp,
    .qpn = loopback_qpn,

    .qp_state_init = loopback_qp_state_init,
    .qp_state_rtr = loopback_qp_state_rtr,
    .qp_state_rts = loopback_qp_state_rts,
    .query_qp = loopback_query_qp,
    .query_remote_conn_info = loopback_query_remote_conn_info,

    .post_send = loopback_post_send,
    .post_recv = loopback_post_recv,

    .add_gid = loopback_add_gid,
    .del_gid = loopback_del_gid,
    .get_backend_gid_index = loopback_get_backend_gid_index,

    .create_srq = loopback_create_srq,
    .destroy_srq = loopback_destroy_srq,
    .query_srq = loopback_query_srq,
    .modify_srq = loopback_modify_srq,
    .post_srq_recv = loopback_post_srq_recv,
};
