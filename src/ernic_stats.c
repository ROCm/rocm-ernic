/*
 * Statistics: the device's counters and the stats file
 *
 * See ernic_stats.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ernic_limits.h"
#include "ernic_log.h"
#include "ernic_stats.h"

struct qp_stats {
    bool live; /* counted against since it was last forgotten */
    uint64_t doorbell_send;
    uint64_t doorbell_recv;
    uint64_t wqes_processed;
    uint64_t wqes_by_opcode[ERNIC_STAT_WR_COUNT];
    uint64_t cqes_posted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_rdma_read;
    uint64_t bytes_rdma_write;
};

struct ernic_stats {
    /* Device counters */
    uint64_t commands;
    uint64_t uar_reads;
    uint64_t uar_writes;
    uint64_t bar0_reads;
    uint64_t bar0_writes;
    uint64_t interrupts;
    uint64_t reset_count;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t total_bytes_rdma_read;
    uint64_t total_bytes_rdma_write;
    uint64_t total_ip_bytes_tx;
    uint64_t total_ip_bytes_rx;

    /* Indexed by qp_id */
    struct qp_stats qp[ERNIC_MAX_QP];

    /* The stats file; strings from malloc() */
    char *file;
    uint64_t write_count; /* files published */
    char *socket_path;
    char *backend;
    char *nic_profile;
    uint16_t pci_vid;
    uint16_t pci_did;
    uint8_t mac[6];
    const char *connection; /* the caller's literal */
    ernic_stats_section_fn section_fn;
    void *section_opaque;
};

struct ernic_stats *ernic_stats_create(void)
{
    return calloc(1, sizeof(struct ernic_stats));
}

void ernic_stats_destroy(struct ernic_stats *st)
{
    if (!st)
        return;
    free(st->file);
    free(st->socket_path);
    free(st->backend);
    free(st->nic_profile);
    free(st);
}

/*
 * Device counters
 */

void ernic_stats_bar0_access(struct ernic_stats *st, bool is_write)
{
    if (!st)
        return;
    if (is_write)
        st->bar0_writes++;
    else
        st->bar0_reads++;
}

void ernic_stats_doorbell_access(struct ernic_stats *st, bool is_write)
{
    if (!st)
        return;
    if (is_write)
        st->uar_writes++;
    else
        st->uar_reads++;
}

void ernic_stats_irq(struct ernic_stats *st)
{
    if (st)
        st->interrupts++;
}

void ernic_stats_eth_bytes(struct ernic_stats *st, uint64_t bytes, bool is_tx)
{
    if (!st)
        return;
    if (is_tx)
        st->total_ip_bytes_tx += bytes;
    else
        st->total_ip_bytes_rx += bytes;
}

void ernic_stats_command(struct ernic_stats *st)
{
    if (st)
        st->commands++;
}

void ernic_stats_device_reset(struct ernic_stats *st)
{
    if (st)
        st->reset_count++;
}

/*
 * Per-QP counters
 */

/* @qp_id's counters, marked live, or NULL for an id no QP can have. */
static struct qp_stats *qp_get(struct ernic_stats *st, uint32_t qp_id)
{
    if (qp_id >= ERNIC_MAX_QP)
        return NULL;
    st->qp[qp_id].live = true;
    return &st->qp[qp_id];
}

void ernic_stats_rdma_bytes(struct ernic_stats *st, uint32_t qp_id,
                            uint64_t bytes, enum ernic_stat_op stat_op)
{
    struct qp_stats *qp;

    if (!st || !bytes)
        return;

    qp = qp_get(st, qp_id);

    switch (stat_op) {
    case ERNIC_STAT_SEND:
        st->total_bytes_sent += bytes;
        if (qp)
            qp->bytes_sent += bytes;
        break;
    case ERNIC_STAT_RECV:
        st->total_bytes_received += bytes;
        if (qp)
            qp->bytes_received += bytes;
        break;
    case ERNIC_STAT_RDMA_READ:
        st->total_bytes_rdma_read += bytes;
        if (qp)
            qp->bytes_rdma_read += bytes;
        break;
    case ERNIC_STAT_RDMA_WRITE:
        st->total_bytes_rdma_write += bytes;
        if (qp)
            qp->bytes_rdma_write += bytes;
        break;
    default:
        break;
    }
}

void ernic_stats_qp_doorbell(struct ernic_stats *st, uint32_t qp_id,
                             bool is_send)
{
    struct qp_stats *qp = st ? qp_get(st, qp_id) : NULL;

    if (!qp)
        return;
    if (is_send)
        qp->doorbell_send++;
    else
        qp->doorbell_recv++;
}

void ernic_stats_qp_wqe(struct ernic_stats *st, uint32_t qp_id, unsigned int wr)
{
    struct qp_stats *qp = st ? qp_get(st, qp_id) : NULL;

    if (!qp)
        return;
    qp->wqes_processed++;
    if (wr < ERNIC_STAT_WR_COUNT)
        qp->wqes_by_opcode[wr]++;
}

void ernic_stats_qp_cqe(struct ernic_stats *st, uint32_t qp_id)
{
    struct qp_stats *qp = st ? qp_get(st, qp_id) : NULL;

    if (qp)
        qp->cqes_posted++;
}

void ernic_stats_qp_forget(struct ernic_stats *st, uint32_t qp_id)
{
    if (st && qp_id < ERNIC_MAX_QP)
        memset(&st->qp[qp_id], 0, sizeof(st->qp[qp_id]));
}

/*
 * Reading the counters back
 */

int ernic_stats_dev_counters(const struct ernic_stats *st,
                             struct ernic_dev_counters *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!st)
        return -EINVAL;

    out->commands = st->commands;
    out->total_bytes_sent = st->total_bytes_sent;
    out->total_bytes_received = st->total_bytes_received;
    out->total_bytes_rdma_read = st->total_bytes_rdma_read;
    out->total_bytes_rdma_write = st->total_bytes_rdma_write;
    return 0;
}

int ernic_stats_qp_counters(const struct ernic_stats *st, uint32_t qp_id,
                            struct ernic_qp_counters *out)
{
    const struct qp_stats *qp;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!st)
        return -EINVAL;
    if (qp_id >= ERNIC_MAX_QP)
        return 0;

    /* A QP that isn't live is all zeros. */
    qp = &st->qp[qp_id];
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
 * The stats file
 */

/* Replace *@dst with a copy of @src (NULL for none).  False when out of
 * memory, leaving *@dst alone. */
static bool replace_string(char **dst, const char *src)
{
    char *copy = NULL;

    if (src) {
        copy = strdup(src);
        if (!copy)
            return false;
    }
    free(*dst);
    *dst = copy;
    return true;
}

int ernic_stats_set_file(struct ernic_stats *st, const char *path)
{
    if (!st)
        return 0;
    if (!replace_string(&st->file, path)) {
        free(st->file);
        st->file = NULL;
        return -ENOMEM;
    }
    return 0;
}

int ernic_stats_set_instance(struct ernic_stats *st,
                             const struct ernic_stats_instance *inst)
{
    char *socket_path = NULL;
    char *backend = NULL;
    char *nic_profile = NULL;

    if (!st || !inst)
        return 0;

    /* All or nothing, so a failure leaves the block as it was. */
    if (!replace_string(&socket_path, inst->socket_path) ||
        !replace_string(&backend, inst->backend) ||
        !replace_string(&nic_profile, inst->nic_profile)) {
        free(socket_path);
        free(backend);
        free(nic_profile);
        return -ENOMEM;
    }
    free(st->socket_path);
    free(st->backend);
    free(st->nic_profile);
    st->socket_path = socket_path;
    st->backend = backend;
    st->nic_profile = nic_profile;
    st->pci_vid = inst->pci_vid;
    st->pci_did = inst->pci_did;
    memcpy(st->mac, inst->mac, sizeof(st->mac));
    return 0;
}

void ernic_stats_set_connection(struct ernic_stats *st, const char *state)
{
    if (st)
        st->connection = state;
}

void ernic_stats_set_section(struct ernic_stats *st,
                             ernic_stats_section_fn write_fn, void *opaque)
{
    if (!st)
        return;
    st->section_fn = write_fn;
    st->section_opaque = write_fn ? opaque : NULL;
}

static const char *or_not_set(const char *s)
{
    return s ? s : "(not set)";
}

/* Write the whole file's contents to @fp; the caller checks for errors. */
static void write_contents(const struct ernic_stats *st, FILE *fp)
{
    static const char *const opcode_names[] = {
        [ERNIC_STAT_WR_RDMA_WRITE] = "RDMA_WRITE",
        [ERNIC_STAT_WR_RDMA_WRITE_WITH_IMM] = "RDMA_WRITE_WITH_IMM",
        [ERNIC_STAT_WR_SEND] = "SEND",
        [ERNIC_STAT_WR_SEND_WITH_IMM] = "SEND_WITH_IMM",
        [ERNIC_STAT_WR_RDMA_READ] = "RDMA_READ",
        [ERNIC_STAT_WR_ATOMIC_CMP_AND_SWP] = "ATOMIC_CMP_AND_SWP",
        [ERNIC_STAT_WR_ATOMIC_FETCH_AND_ADD] = "ATOMIC_FETCH_AND_ADD",
        [ERNIC_STAT_WR_LSO] = "LSO",
        [ERNIC_STAT_WR_SEND_WITH_INV] = "SEND_WITH_INV",
        [ERNIC_STAT_WR_RDMA_READ_WITH_INV] = "RDMA_READ_WITH_INV",
        [ERNIC_STAT_WR_LOCAL_INV] = "LOCAL_INV",
        [ERNIC_STAT_WR_FAST_REG_MR] = "FAST_REG_MR",
        [ERNIC_STAT_WR_MASKED_ATOMIC_CMP_SWP] = "MASKED_ATOMIC_CMP_SWP",
        [ERNIC_STAT_WR_MASKED_ATOMIC_FETCH] = "MASKED_ATOMIC_FETCH",
        [ERNIC_STAT_WR_BIND_MW] = "BIND_MW",
        [ERNIC_STAT_WR_REG_SIG_MR] = "REG_SIG_MR",
        [ERNIC_STAT_WR_ERROR] = "ERROR",
        [ERNIC_STAT_WR_SEND_DC] = "SEND_DC",
    };
    bool any_qp = false;

    /* The per-QP loop below indexes both arrays with one bound. */
    _Static_assert(sizeof(opcode_names) / sizeof(opcode_names[0]) ==
                       ERNIC_STAT_WR_COUNT,
                   "opcode_names must name every wqes_by_opcode entry");

    fprintf(fp, "=== ROCm ERNIC Statistics ===\n");
    fprintf(fp, "Instance:\n");
    fprintf(fp, "  %-11s : %s\n", "socket", or_not_set(st->socket_path));
    fprintf(fp, "  %-11s : %s\n", "backend", or_not_set(st->backend));
    if (st->pci_vid || st->pci_did) {
        fprintf(fp, "  %-11s : 0x%04x:0x%04x\n", "pci", st->pci_vid,
                st->pci_did);
    } else {
        fprintf(fp, "  %-11s : (not set)\n", "pci");
    }
    fprintf(fp, "  %-11s : %02x:%02x:%02x:%02x:%02x:%02x\n", "mac", st->mac[0],
            st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5]);
    fprintf(fp, "  %-11s : %s\n", "connection", or_not_set(st->connection));
    fprintf(fp, "  %-11s : %s\n", "nic_profile", or_not_set(st->nic_profile));
    /* The " : " separator is what ernic-exporter splits every stat line on;
     * without it this counter is invisible to the exporter. */
    fprintf(fp, "\nWrite count : %" PRIu64 "\n\n", st->write_count);
    fprintf(fp, "Device Statistics:\n");
#define STATS_LABEL_WIDTH 23
#define STAT_LINE(name, value) \
    fprintf(fp, "  %-*s : %" PRIu64 "\n", STATS_LABEL_WIDTH, name, value)
    STAT_LINE("commands", st->commands);
    STAT_LINE("bar0_reads", st->bar0_reads);
    STAT_LINE("bar0_writes", st->bar0_writes);
    STAT_LINE("uar_reads", st->uar_reads);
    STAT_LINE("uar_writes", st->uar_writes);
    STAT_LINE("mmio_reads_total", st->bar0_reads + st->uar_reads);
    STAT_LINE("mmio_writes_total", st->bar0_writes + st->uar_writes);
    STAT_LINE("interrupts", st->interrupts);
    STAT_LINE("reset_count", st->reset_count);
    STAT_LINE("total_bytes_sent", st->total_bytes_sent);
    STAT_LINE("total_bytes_received", st->total_bytes_received);
    STAT_LINE("total_bytes_rdma_read", st->total_bytes_rdma_read);
    STAT_LINE("total_bytes_rdma_write", st->total_bytes_rdma_write);
    STAT_LINE("total_ip_bytes_tx", st->total_ip_bytes_tx);
    STAT_LINE("total_ip_bytes_rx", st->total_ip_bytes_rx);
#undef STAT_LINE
#undef STATS_LABEL_WIDTH
    fprintf(fp, "\n");

    if (st->section_fn)
        st->section_fn(fp, st->section_opaque);

    fprintf(fp, "Per-QP Statistics:\n");
    for (uint32_t id = 0; id < ERNIC_MAX_QP; id++) {
        const struct qp_stats *qp = &st->qp[id];
        uint64_t total_wqes = 0;

        if (!qp->live)
            continue;
        any_qp = true;

        fprintf(fp, "  QP %" PRIu32 ":\n", id);
        fprintf(fp, "    doorbell_send  : %" PRIu64 "\n", qp->doorbell_send);
        fprintf(fp, "    doorbell_recv  : %" PRIu64 "\n", qp->doorbell_recv);
        fprintf(fp, "    wqes_processed : %" PRIu64 "\n", qp->wqes_processed);
        fprintf(fp, "    cqes_posted    : %" PRIu64 "\n", qp->cqes_posted);
        fprintf(fp, "    bytes_sent     : %" PRIu64 "\n", qp->bytes_sent);
        fprintf(fp, "    bytes_received : %" PRIu64 "\n", qp->bytes_received);
        fprintf(fp, "    bytes_rdma_read : %" PRIu64 "\n", qp->bytes_rdma_read);
        fprintf(fp, "    bytes_rdma_write : %" PRIu64 "\n",
                qp->bytes_rdma_write);

        fprintf(fp, "    WQEs by opcode:\n");
        for (unsigned int i = 0; i < ERNIC_STAT_WR_COUNT; i++) {
            if (qp->wqes_by_opcode[i] > 0) {
                /* The width must exceed the longest opcode name, and the
                 * space before the colon must be explicit: ernic-exporter
                 * splits on " : ", so a name that fills the field would
                 * make the line unparsable. */
                fprintf(fp, "      %-21s : %" PRIu64 "\n", opcode_names[i],
                        qp->wqes_by_opcode[i]);
                total_wqes += qp->wqes_by_opcode[i];
            }
        }
        if (total_wqes != qp->wqes_processed) {
            fprintf(fp, "      %-21s : %" PRIu64 "\n", "(other opcodes)",
                    qp->wqes_processed - total_wqes);
        }
        fprintf(fp, "\n");
    }
    if (!any_qp)
        fprintf(fp, "  (no QPs created yet)\n\n");
}

bool ernic_stats_write(struct ernic_stats *st)
{
    static const char suffix[] = ".XXXXXX";
    FILE *fp;
    char *tmp_path;
    size_t len;
    int fd;
    int write_err;

    if (!st || !st->file)
        return false;

    /* ernic-exporter and ernicctl read the file whenever they like, so it's
     * never rewritten in place, where they could find it empty or half
     * written.  The new contents go to a temporary file in the same
     * directory, and rename() replaces the old file with it in one step: a
     * reader opens one whole file or the other.  mkstemp() creates the file
     * 0600, which would lock out an exporter running as another user. */
    len = strlen(st->file);
    tmp_path = malloc(len + sizeof(suffix));
    if (!tmp_path) {
        ernic_log(ERNIC_LOG_ERROR, "stats: out of memory writing stats file %s",
                  st->file);
        return false;
    }
    memcpy(tmp_path, st->file, len);
    memcpy(tmp_path + len, suffix, sizeof(suffix));
    fd = mkstemp(tmp_path);
    if (fd < 0) {
        ernic_log(ERNIC_LOG_ERROR,
                  "stats: failed to create a temporary file for stats file "
                  "%s: %s",
                  st->file, strerror(errno));
        free(tmp_path);
        return false;
    }
    fp = fchmod(fd, 0644) == 0 ? fdopen(fd, "w") : NULL;
    if (!fp) {
        ernic_log(ERNIC_LOG_ERROR,
                  "stats: failed to open %s for stats file %s: %s", tmp_path,
                  st->file, strerror(errno));
        close(fd);
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    /* Says why a write below failed, if nothing after it changes errno */
    errno = 0;
    write_contents(st, fp);

    /* A failed fprintf() leaves the error indicator set; fflush() reports a
     * failure writing what's still buffered, and fclose() can fail after
     * that.  A file missing any of it must not replace the old one. */
    write_err = 0;
    if (fflush(fp) != 0 || ferror(fp))
        write_err = errno ? errno : EIO;
    if (fclose(fp) != 0 && !write_err)
        write_err = errno;
    if (write_err) {
        ernic_log(ERNIC_LOG_ERROR, "stats: failed to write stats file %s: %s",
                  st->file, strerror(write_err));
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    if (rename(tmp_path, st->file) != 0) {
        ernic_log(ERNIC_LOG_ERROR, "stats: failed to replace stats file %s: %s",
                  st->file, strerror(errno));
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }
    free(tmp_path);

    /* Counts files published, so a reader that sees it go up knows it's
     * looking at a new one */
    st->write_count++;
    return true;
}
