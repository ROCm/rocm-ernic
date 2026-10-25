/*
 * Compatibility Bridge Implementation
 *
 * This file implements the wrapper functions that isolate QEMU code from
 * our libvfio-user server. Only this file includes QEMU headers.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <infiniband/verbs.h>
#include "rocm_ernic_compat.h"
#include "rocm_ernic_internal.h"

/* QEMU-derived headers, from src/qemu/ */
#include "pvrdma.h"
#include "rdma_backend.h"
#include "rdma_backend_defs.h"
#include "rdma/rdma_backend_ops.h"
#include "rdma_rm.h"
#include "rdma_rm_defs.h"
#include "rdma/pvrdma_stats.h"
#include "rdma_utils.h"
#include "vmw_pvrdma-abi.h"

/*
 * Device Management
 */

pvrdma_handle_t pvrdma_device_create(rocm_ernic_dev_t *dev,
                                     const char *backend_type_str)
{
    PVRDMADev *pvrdma;
    RdmaBackendType backend_type;

    if (!dev) {
        rdma_error_report("pvrdma_device_create: dev is NULL");
        return NULL;
    }

    /* Parse backend type and extract config */
    backend_type = rdma_backend_get_type_from_string(backend_type_str);
    if (backend_type == RDMA_BACKEND_TYPE_MAX) {
        rdma_error_report("Unknown backend '%s'", backend_type_str);
        return NULL;
    }

    /* Allocate QEMU PVRDMA device structure */
    pvrdma = calloc(1, sizeof(PVRDMADev));
    if (!pvrdma) {
        rdma_error_report("Failed to allocate PVRDMADev");
        return NULL;
    }

    pvrdma->parent_obj.vfu_ctx = dev->vfu_ctx;

    rdma_info_report("Selected RDMA backend: %s",
                     rdma_backend_type_to_string(backend_type));

    /* Store backend type and config in device for later use */
    pvrdma->backend_dev.backend_type = backend_type;

    /* Extract backend config (part after ':') from backend string, which
     * may be NULL, meaning "none" */
    const char *colon = backend_type_str ? strchr(backend_type_str, ':') : NULL;
    const char *name = colon ? (colon + 1) : NULL;

    if (name) {
        rdma_info_report("Backend config string: '%s'", name);
        pvrdma->backend_device_name = strdup(name);
        if (!pvrdma->backend_device_name) {
            rdma_error_report("Failed to allocate the backend config");
            free(pvrdma);
            return NULL;
        }
    }

    /* Copy MAC address from rocm_ernic_dev */
    memcpy(pvrdma->mac_addr, dev->mac_addr, 6);

    /* Initialize device attributes with defaults */
    pvrdma->dev_attr.max_qp = MAX_QP;
    pvrdma->dev_attr.max_cq = MAX_CQ;
    pvrdma->dev_attr.max_mr = MAX_MR;
    pvrdma->dev_attr.max_pd = MAX_PD;
    pvrdma->dev_attr.max_qp_rd_atom = MAX_QP_RD_ATOM;
    pvrdma->dev_attr.max_qp_init_rd_atom = MAX_QP_INIT_RD_ATOM;
    pvrdma->dev_attr.max_ah = MAX_AH;
    pvrdma->dev_attr.max_srq = MAX_SRQ;
    pvrdma->dev_attr.max_mr_size = MAX_MR_SIZE;
    pvrdma->dev_attr.max_sge = MAX_SGE; /* Required for calculations below */

    /* Calculate dynamic device capabilities (from init_dev_caps in
     * upstream QEMU's hw/rdma/vmw/pvrdma_main.c) */
    {
        size_t pg_tbl_bytes =
            PVRDMA_PG_TBL_PAGES * PAGE_SIZE * (PAGE_SIZE / sizeof(uint64_t));
        size_t wr_sz = MAX(sizeof(struct pvrdma_sq_wqe_hdr),
                           sizeof(struct pvrdma_rq_wqe_hdr));

        rdma_info_report("Calculating device capabilities:");
        rdma_info_report("  PAGE_SIZE=%zu, pg_tbl_bytes=%zu", (size_t)PAGE_SIZE,
                         pg_tbl_bytes);

        /* Calculate max_qp_wr */
        pvrdma->dev_attr.max_qp_wr =
            (int)(pg_tbl_bytes /
                      (wr_sz + sizeof(struct pvrdma_sge) *
                                   (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        /* Calculate max_cqe */
        pvrdma->dev_attr.max_cqe =
            (int)(pg_tbl_bytes / sizeof(struct pvrdma_cqe) - PAGE_SIZE);

        /* Calculate max_srq_wr */
        pvrdma->dev_attr.max_srq_wr =
            (int)(pg_tbl_bytes / ((sizeof(struct pvrdma_rq_wqe_hdr) +
                                   sizeof(struct pvrdma_sge)) *
                                  (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        rdma_info_report("  max_qp_wr=%d", pvrdma->dev_attr.max_qp_wr);
        rdma_info_report("  max_cqe=%d", pvrdma->dev_attr.max_cqe);
        rdma_info_report("  max_sge=%d", pvrdma->dev_attr.max_sge);
        rdma_info_report("  max_srq_wr=%d", pvrdma->dev_attr.max_srq_wr);
    }

    /* Initialize stats */
    memset(&pvrdma->stats, 0, sizeof(pvrdma->stats));
    pvrdma->stats.qp_stats =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    pvrdma->stats.stats_file = NULL;
    pvrdma->stats.stats_write_count = 0;

    rdma_info_report("PVRDMA device created (handle=%p)", (void *)pvrdma);

    return pvrdma;
}

const char *pvrdma_backend_name(const char *backend_type_str)
{
    RdmaBackendType type = rdma_backend_get_type_from_string(backend_type_str);

    return type == RDMA_BACKEND_TYPE_MAX ? NULL
                                         : rdma_backend_type_to_string(type);
}

void pvrdma_device_destroy(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    /* Clean up resource manager */
    rdma_rm_fini(&pvrdma->rdma_dev_res);

    /* Clean up backend using the new abstraction */
    rdma_backend_fini_with_ops(&pvrdma->backend_dev);

    /* Free stats hash table */
    if (pvrdma->stats.qp_stats) {
        g_hash_table_destroy(pvrdma->stats.qp_stats);
        pvrdma->stats.qp_stats = NULL;
    }
    if (pvrdma->stats.stats_file) {
        free(pvrdma->stats.stats_file);
        pvrdma->stats.stats_file = NULL;
    }
    free(pvrdma->stats_socket_path);
    pvrdma->stats_socket_path = NULL;
    free(pvrdma->stats_backend_str);
    pvrdma->stats_backend_str = NULL;
    free(pvrdma->stats_connection_str);
    pvrdma->stats_connection_str = NULL;
    (void)pvrdma_stats_set_nic_profile(NULL);
    pvrdma_stats_set_section(NULL, NULL);

    /* Free device names */
    free(pvrdma->backend_device_name);

    free(pvrdma);

    rdma_info_report("PVRDMA device destroyed");
}

int pvrdma_device_realize(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;
    int rc;

    if (!pvrdma) {
        return -EINVAL;
    }

    /* Initialize RDMA backend with selected backend type */
    const char *backend_config =
        pvrdma
            ->backend_device_name; /* Config extracted from --backend string */

    rc = rdma_backend_init_with_ops(
        &pvrdma->backend_dev, pvrdma->backend_dev.backend_type, backend_config);

    if (rc < 0) {
        rdma_error_report("RDMA backend initialization failed (rc=%d)", rc);
        rdma_error_report(
            "Backend type: %s",
            rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));
        return -EIO;
    }

    /* Query device capabilities from backend to populate dev_attr */
    if (pvrdma->backend_dev.backend_ops &&
        pvrdma->backend_dev.backend_ops->query_device) {
        rc = pvrdma->backend_dev.backend_ops->query_device(&pvrdma->backend_dev,
                                                           &pvrdma->dev_attr);
        if (rc < 0) {
            rdma_error_report("Backend query_device failed (rc=%d)", rc);
            return -EIO;
        }
        rdma_info_report("Backend device attributes queried: max_qp=%d, "
                         "max_cq=%d, max_pd=%d",
                         pvrdma->dev_attr.max_qp, pvrdma->dev_attr.max_cq,
                         pvrdma->dev_attr.max_pd);
    } else {
        rdma_error_report("Backend does not support query_device");
        return -ENOTSUP;
    }

    rdma_info_report(
        "RDMA backend '%s' initialized successfully",
        rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));

    /* Initialize resource manager AFTER querying device capabilities */
    rdma_info_report("pvrdma_device_realize: About to call rdma_rm_init, "
                     "pvrdma=%p, &pvrdma->rdma_dev_res=%p",
                     (void *)pvrdma, (void *)&pvrdma->rdma_dev_res);
    if (rdma_rm_init(&pvrdma->rdma_dev_res, &pvrdma->dev_attr) < 0) {
        rdma_error_report("Failed to initialize resource manager");
        return -EIO;
    }
    rdma_info_report("Resource manager initialized with dev_attr tables");

    rdma_info_report("PVRDMA device realized successfully");

    return 0;
}

void pvrdma_bar0_mmio_count(pvrdma_handle_t handle, bool is_write)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }
    if (is_write) {
        pvrdma->stats.bar0_writes++;
    } else {
        pvrdma->stats.bar0_reads++;
    }
}

void pvrdma_uar_mmio_count(pvrdma_handle_t handle, bool is_write)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }
    if (is_write) {
        pvrdma->stats.uar_writes++;
    } else {
        pvrdma->stats.uar_reads++;
    }
}

void pvrdma_irq_count(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }
    pvrdma->stats.interrupts++;
}

void pvrdma_eth_bytes_count(pvrdma_handle_t handle, uint64_t bytes, bool is_tx)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }
    if (is_tx) {
        pvrdma->stats.total_ip_bytes_tx += bytes;
    } else {
        pvrdma->stats.total_ip_bytes_rx += bytes;
    }
}

void pvrdma_adminq_count(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }
    pvrdma->stats.commands++;
}

void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op stat_op)
{
    PVRDMADev *pvrdma = handle;
    PVRDMAQPStats *qp;

    if (!pvrdma || !bytes) {
        return;
    }

    qp = qp_id == PVRDMA_STAT_NO_QP ? NULL : pvrdma_get_qp_stats(pvrdma, qp_id);

    switch (stat_op) {
    case PVRDMA_STAT_SEND:
        pvrdma->stats.total_bytes_sent += bytes;
        if (qp) {
            qp->bytes_sent += bytes;
        }
        break;
    case PVRDMA_STAT_RECV:
        pvrdma->stats.total_bytes_received += bytes;
        if (qp) {
            qp->bytes_received += bytes;
        }
        break;
    case PVRDMA_STAT_RDMA_READ:
        pvrdma->stats.total_bytes_rdma_read += bytes;
        if (qp) {
            qp->bytes_rdma_read += bytes;
        }
        break;
    case PVRDMA_STAT_RDMA_WRITE:
        pvrdma->stats.total_bytes_rdma_write += bytes;
        if (qp) {
            qp->bytes_rdma_write += bytes;
        }
        break;
    default:
        break;
    }
}

void pvrdma_qp_doorbell_count(pvrdma_handle_t handle, uint32_t qp_id,
                              bool is_send)
{
    PVRDMADev *pvrdma = handle;
    PVRDMAQPStats *qp;

    if (!pvrdma || qp_id == PVRDMA_STAT_NO_QP) {
        return;
    }

    qp = pvrdma_get_qp_stats(pvrdma, qp_id);
    if (!qp) {
        return;
    }

    if (is_send) {
        qp->doorbell_send++;
    } else {
        qp->doorbell_recv++;
    }
}

void pvrdma_qp_wqe_count(pvrdma_handle_t handle, uint32_t qp_id,
                         unsigned int pvrdma_opcode)
{
    PVRDMADev *pvrdma = handle;
    PVRDMAQPStats *qp;

    if (!pvrdma || qp_id == PVRDMA_STAT_NO_QP) {
        return;
    }

    qp = pvrdma_get_qp_stats(pvrdma, qp_id);
    if (!qp) {
        return;
    }

    qp->wqes_processed++;
    if (pvrdma_opcode < G_N_ELEMENTS(qp->wqes_by_opcode)) {
        qp->wqes_by_opcode[pvrdma_opcode]++;
    }
}

void pvrdma_qp_cqe_count(pvrdma_handle_t handle, uint32_t qp_id)
{
    PVRDMADev *pvrdma = handle;
    PVRDMAQPStats *qp;

    if (!pvrdma || qp_id == PVRDMA_STAT_NO_QP) {
        return;
    }

    qp = pvrdma_get_qp_stats(pvrdma, qp_id);
    if (qp) {
        qp->cqes_posted++;
    }
}

void pvrdma_qp_stats_forget(pvrdma_handle_t handle, uint32_t qp_id)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma || !pvrdma->stats.qp_stats || qp_id == PVRDMA_STAT_NO_QP) {
        return;
    }

    g_hash_table_remove(pvrdma->stats.qp_stats, GUINT_TO_POINTER(qp_id));
}

int pvrdma_get_dev_counters(pvrdma_handle_t handle,
                            struct pvrdma_dev_counters *out)
{
    PVRDMADev *pvrdma = handle;

    if (!out) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (!pvrdma) {
        return -EINVAL;
    }

    out->commands = pvrdma->stats.commands;
    out->total_bytes_sent = pvrdma->stats.total_bytes_sent;
    out->total_bytes_received = pvrdma->stats.total_bytes_received;
    out->total_bytes_rdma_read = pvrdma->stats.total_bytes_rdma_read;
    out->total_bytes_rdma_write = pvrdma->stats.total_bytes_rdma_write;
    return 0;
}

int pvrdma_get_qp_counters(pvrdma_handle_t handle, uint32_t qp_id,
                           struct pvrdma_qp_counters *out)
{
    PVRDMADev *pvrdma = handle;
    const PVRDMAQPStats *qp = NULL;

    if (!out) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (!pvrdma) {
        return -EINVAL;
    }

    /* A plain lookup: pvrdma_get_qp_stats() would insert an entry. */
    if (pvrdma->stats.qp_stats && qp_id != PVRDMA_STAT_NO_QP) {
        qp = g_hash_table_lookup(pvrdma->stats.qp_stats,
                                 GUINT_TO_POINTER(qp_id));
    }
    if (!qp) {
        return 0;
    }

    out->doorbell_send = qp->doorbell_send;
    out->doorbell_recv = qp->doorbell_recv;
    out->wqes_processed = qp->wqes_processed;
    out->cqes_posted = qp->cqes_posted;
    out->bytes_sent = qp->bytes_sent;
    out->bytes_received = qp->bytes_received;
    out->bytes_rdma_read = qp->bytes_rdma_read;
    out->bytes_rdma_write = qp->bytes_rdma_write;
    return 0;
}

/*
 * Command Execution - pvrdma_exec_cmd is implemented in pvrdma_cmd.c
 */

/*
 * Statistics
 */

void pvrdma_set_stats_file(pvrdma_handle_t handle, const char *stats_file)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats.stats_file);
    if (stats_file) {
        pvrdma->stats.stats_file = strdup(stats_file);
        if (!pvrdma->stats.stats_file) {
            rdma_error_report("stats file: out of memory for its path; "
                              "statistics won't be written");
        }
    } else {
        pvrdma->stats.stats_file = NULL;
    }
}

void pvrdma_set_stats_instance_info(pvrdma_handle_t handle,
                                    const char *socket_path,
                                    const char *backend_type_str)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats_socket_path);
    pvrdma->stats_socket_path = socket_path ? strdup(socket_path) : NULL;
    if (socket_path && !pvrdma->stats_socket_path) {
        rdma_error_report("stats file: out of memory for the socket path");
    }

    free(pvrdma->stats_backend_str);
    pvrdma->stats_backend_str =
        backend_type_str ? strdup(backend_type_str) : NULL;
    if (backend_type_str && !pvrdma->stats_backend_str) {
        rdma_error_report("stats file: out of memory for the backend");
    }
}

void pvrdma_set_stats_pci_ids(pvrdma_handle_t handle, uint16_t vid,
                              uint16_t did)
{
    PVRDMADev *pvrdma = handle;

    if (pvrdma) {
        pvrdma->stats_pci_vid = vid;
        pvrdma->stats_pci_did = did;
    }
}

void pvrdma_set_stats_connection_state(pvrdma_handle_t handle,
                                       const char *connection_str)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats_connection_str);
    pvrdma->stats_connection_str =
        connection_str ? strdup(connection_str) : NULL;
    if (connection_str && !pvrdma->stats_connection_str) {
        rdma_error_report("stats file: out of memory for the connection "
                          "state");
    }
}

void pvrdma_set_stats_nic_profile(pvrdma_handle_t handle, const char *profile)
{
    if (handle && pvrdma_stats_set_nic_profile(profile) != 0)
        rdma_error_report("stats file: out of memory for the NIC profile");
}

void pvrdma_set_stats_section(pvrdma_handle_t handle,
                              void (*fn)(FILE *fp, void *opaque), void *opaque)
{
    if (handle)
        pvrdma_stats_set_section(fn, opaque);
}

void pvrdma_inc_stats_reset_count(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (pvrdma) {
        pvrdma->stats.reset_count++;
    }
}

void pvrdma_write_stats(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma) {
        return;
    }

    pvrdma_write_stats_impl(pvrdma);
}

/* -------------------------------------------------------------------------
 * Thin wrappers for ionic_adminq.c (cannot include rdma_rm.h directly).
 * -------------------------------------------------------------------------
 */

int ionic_rm_alloc_cq(pvrdma_handle_t handle, uint32_t cqe, uint32_t *cq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_cq(&pvrdma->rdma_dev_res, &pvrdma->backend_dev, cqe,
                            cq_handle);
}

void ionic_rm_dealloc_cq(pvrdma_handle_t handle, uint32_t cq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_cq(&pvrdma->rdma_dev_res, cq_handle);
}

int ionic_rm_alloc_srq(pvrdma_handle_t handle, uint32_t pd_handle,
                       uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                       uint32_t *srq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_srq(&pvrdma->rdma_dev_res, pd_handle, max_wr, max_sge,
                             srq_limit, srq_handle);
}

void ionic_rm_dealloc_srq(pvrdma_handle_t handle, uint32_t srq_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_srq(&pvrdma->rdma_dev_res, srq_handle);
}

int ionic_rm_modify_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                        uint32_t srq_limit)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_srq_attr attr = {.srq_limit = srq_limit};

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    /*
     * A limit of 0 disables the low-watermark and is a legal value that the
     * guest driver accepts, but rdma_rm_modify_srq() rejects it outright
     * (rdma_rm.c: IBV_SRQ_LIMIT with srq_limit == 0 is -EINVAL). There is
     * nothing for the backend to hold for "no limit", and the authoritative
     * copy lives in the adminq's SRQ map, so succeed here rather than
     * turning a valid ibv_modify_srq() into an error.
     */
    if (srq_limit == 0)
        return 0;
    /* IBV_SRQ_LIMIT only: the depth is fixed at create time, and the guest
     * driver's modify path carries nothing else (ionic_admin_modify_srq is
     * qid + low_wqes_limit). */
    return rdma_rm_modify_srq(&pvrdma->rdma_dev_res, srq_handle, &attr,
                              IBV_SRQ_LIMIT);
}

int ionic_rm_query_srq(pvrdma_handle_t handle, uint32_t srq_handle,
                       uint32_t *max_wr, uint32_t *max_sge, uint32_t *srq_limit)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_srq_attr attr = {0};
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    ret = rdma_rm_query_srq(&pvrdma->rdma_dev_res, srq_handle, &attr);
    if (ret)
        return ret;

    if (max_wr)
        *max_wr = attr.max_wr;
    if (max_sge)
        *max_sge = attr.max_sge;
    if (srq_limit)
        *srq_limit = attr.srq_limit;
    return 0;
}

int ionic_rm_alloc_pd(pvrdma_handle_t handle, uint32_t *pd_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    return rdma_rm_alloc_pd(&pvrdma->rdma_dev_res, &pvrdma->backend_dev,
                            pd_handle);
}

int ionic_rm_alloc_qp(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint8_t qp_type, uint32_t max_send_wr,
                      uint32_t max_recv_wr, uint32_t send_cq_handle,
                      uint32_t recv_cq_handle, uint32_t *qpn)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    RdmaRmQP *out_qp = NULL;
    return rdma_rm_alloc_qp(&pvrdma->rdma_dev_res, pd_handle, qp_type,
                            max_send_wr, 16, send_cq_handle, max_recv_wr, 16,
                            recv_cq_handle, qpn, 0, 0, &out_qp);
}

void ionic_rm_dealloc_qp(pvrdma_handle_t handle, uint32_t qpn)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_qp(&pvrdma->rdma_dev_res, qpn);
}

int ionic_rm_alloc_mr(pvrdma_handle_t handle, uint32_t pd_handle,
                      uint32_t access_flags, uint32_t *mr_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;
    uint32_t lkey = 0, rkey = 0;
    return rdma_rm_alloc_mr(&pvrdma->rdma_dev_res, pd_handle, (int)access_flags,
                            mr_handle, &lkey, &rkey);
}

void ionic_rm_dealloc_mr(pvrdma_handle_t handle, uint32_t mr_handle)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma)
        return;
    rdma_rm_dealloc_mr(&pvrdma->rdma_dev_res, mr_handle);
}

int ionic_rm_modify_qp(pvrdma_handle_t handle, uint32_t qpn, uint32_t attr_mask,
                       uint8_t type_state, uint32_t sq_psn, uint32_t rq_psn,
                       uint32_t qkey_dest_qpn, const uint8_t *dest_gid_16bytes)
{
    PVRDMADev *pvrdma = handle;
    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    /* Decode ionic type_state: bits[3:0]=to_state, bits[7:4]=from_state.
     * Map ionic QP state values to ibv_qp_state.  ionic uses the same
     * numeric encoding as IB (RESET=0, INIT=1, RTR=2, RTS=3, SQD=4,
     * SQE=5, ERROR=6) so a direct cast is safe for the common path. */
    enum ibv_qp_state to_state = (enum ibv_qp_state)(type_state & 0x0fu);

    /* Build a minimal dgid from the raw bytes provided by the caller. */
    union ibv_gid dgid;
    if (dest_gid_16bytes)
        memcpy(&dgid, dest_gid_16bytes, sizeof(dgid));
    else
        memset(&dgid, 0, sizeof(dgid));

    /* dest QPN sits in the lower 24 bits of qkey_dest_qpn when sending UD. */
    uint32_t dqpn = qkey_dest_qpn & 0x00ffffffu;
    uint32_t qkey = qkey_dest_qpn >> 24;

    return rdma_rm_modify_qp(&pvrdma->rdma_dev_res, &pvrdma->backend_dev, qpn,
                             attr_mask, 0, /* sgid_idx: use default GID */
                             &dgid, dqpn, to_state, qkey, rq_psn, sq_psn);
}

int ionic_rm_query_qp(pvrdma_handle_t handle, uint32_t qpn, uint8_t *state,
                      uint8_t *path_mtu, uint32_t *dest_qpn,
                      uint32_t *access_flags, uint32_t *rq_psn,
                      uint32_t *sq_psn)
{
    PVRDMADev *pvrdma = handle;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_ctx)
        return -EINVAL;

    ret = rdma_rm_query_qp(&pvrdma->rdma_dev_res, qpn, &attr,
                           IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                               IBV_QP_ACCESS_FLAGS | IBV_QP_RQ_PSN |
                               IBV_QP_SQ_PSN,
                           &init_attr);
    if (ret)
        return ret;

    if (state)
        *state = (uint8_t)attr.qp_state;
    if (path_mtu)
        *path_mtu = (uint8_t)attr.path_mtu;
    if (dest_qpn)
        *dest_qpn = attr.dest_qp_num;
    if (access_flags)
        *access_flags = (uint32_t)attr.qp_access_flags;
    if (rq_psn)
        *rq_psn = attr.rq_psn;
    if (sq_psn)
        *sq_psn = attr.sq_psn;
    return 0;
}

/* pvrdma_get_dev_resources is retained in the header for potential future use
 * but is currently unused — the ionic path uses the ionic_rm_* wrappers above
 * instead. */

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return UINT32_MAX;
    return tcp_backend_local_node_id(&pvrdma->backend_dev);
}

uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle,
                                  const uint8_t *dest_gid_16bytes)
{
    PVRDMADev *pvrdma = handle;
    union ibv_gid dgid;

    if (!pvrdma)
        return UINT32_MAX;

    if (!dest_gid_16bytes)
        return tcp_backend_node_from_gid(&pvrdma->backend_dev, NULL);

    memcpy(&dgid, dest_gid_16bytes, sizeof(dgid));
    return tcp_backend_node_from_gid(&pvrdma->backend_dev, &dgid);
}

int ionic_mesh_claim_gid(pvrdma_handle_t handle, const uint8_t *gid_16bytes,
                         bool add)
{
    PVRDMADev *pvrdma = handle;
    union ibv_gid gid;

    if (!pvrdma || !gid_16bytes)
        return -EINVAL;

    memcpy(&gid, gid_16bytes, sizeof(gid));
    return tcp_backend_claim_gid(&pvrdma->backend_dev, &gid, add);
}

int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return -EINVAL;
    return tcp_backend_send_ionic_v(&pvrdma->backend_dev, dst_node, hdr,
                                    hdr_len, body, body_len, tag, reply);
}

bool ionic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return true;
    return tcp_backend_ionic_has_room(&pvrdma->backend_dev, dst_node, len);
}

void ionic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return;
    tcp_backend_ionic_cancel(&pvrdma->backend_dev, tag, mask);
}

void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque)
{
    PVRDMADev *pvrdma = handle;

    if (!pvrdma)
        return;
    tcp_backend_set_ionic_recv_cb(&pvrdma->backend_dev, (tcp_ionic_recv_fn)fn,
                                  opaque);
}
