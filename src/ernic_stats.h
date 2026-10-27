/*
 * Statistics: the device's counters and the stats file
 *
 * The server creates one struct ernic_stats for the instance and hands it
 * to the ionic device, which counts what it does here: MMIO accesses,
 * interrupts, Ethernet and RDMA bytes, admin commands, and per-QP
 * doorbells, WQEs and completions.  The admin queue reads the totals back
 * for the guest's STATS_VALS and QP_STATS_VALS commands, and the server
 * writes them to the stats file that ernic-exporter and ernicctl read,
 * with what it sets here about the instance and the section the ionic
 * device adds (its LIF and port counters).
 *
 * The guest's LIF statistics block (ionic/ionic_emu_stats.h) counts some of
 * the same traffic, but not the same way: by direction rather than by
 * operation and QP, and from zero again whenever the guest resets the LIF.
 * Neither can be derived from the other, so the data path counts RDMA bytes
 * into both (ionic_dp_rdma_bytes()).
 *
 * Main loop thread only.  Every function accepts a NULL @st and does
 * nothing, so code that counts can run in a unit test without one.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_STATS_H
#define ERNIC_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h> /* FILE, for ernic_stats_section_fn */

/* Opaque; from ernic_stats_create(). */
struct ernic_stats;

/**
 * ernic_stats_create - Create the counters, all zero, with no stats file
 *
 * Returns: the counters, or NULL when out of memory
 */
struct ernic_stats *ernic_stats_create(void);

/**
 * ernic_stats_destroy - Free the counters
 * @param st from ernic_stats_create(), or NULL
 */
void ernic_stats_destroy(struct ernic_stats *st);

/*
 * Device counters
 */

/**
 * ernic_stats_bar0_access - Count one BAR0 (registers and MSI-X) access
 * @param st Statistics
 * @param is_write true for a write, false for a read
 */
void ernic_stats_bar0_access(struct ernic_stats *st, bool is_write);

/**
 * ernic_stats_doorbell_access - Count one BAR2 (doorbell page) access
 * @param st Statistics
 * @param is_write true for a write, false for a read
 *
 * The stats file calls these uar_reads and uar_writes, after the PVRDMA UAR
 * they used to count.
 */
void ernic_stats_doorbell_access(struct ernic_stats *st, bool is_write);

/**
 * ernic_stats_irq - Count one delivered MSI-X interrupt
 * @param st Statistics
 */
void ernic_stats_irq(struct ernic_stats *st);

/**
 * ernic_stats_eth_bytes - Count guest Ethernet bytes moved
 * @param st Statistics
 * @param bytes Frame length
 * @param is_tx true for guest-to-host, false for host-to-guest
 */
void ernic_stats_eth_bytes(struct ernic_stats *st, uint64_t bytes, bool is_tx);

/**
 * ernic_stats_command - Count one executed admin queue command
 * @param st Statistics
 */
void ernic_stats_command(struct ernic_stats *st);

/**
 * ernic_stats_device_reset - Count one device reset or PCI FLR
 * @param st Statistics
 */
void ernic_stats_device_reset(struct ernic_stats *st);

/*
 * Per-QP counters
 *
 * A QP is the driver's qp_id, which is below ERNIC_MAX_QP (ernic_limits.h);
 * these ignore any other.  A QP's counters exist from the first thing
 * counted against it until ernic_stats_qp_forget().
 */

/* Operation classes for ernic_stats_rdma_bytes(). */
enum ernic_stat_op {
    ERNIC_STAT_SEND,
    ERNIC_STAT_RECV,
    ERNIC_STAT_RDMA_READ,
    ERNIC_STAT_RDMA_WRITE,
};

/* Pass as @qp_id to ernic_stats_rdma_bytes() to count only the totals. */
#define ERNIC_STAT_NO_QP UINT32_MAX

/**
 * ernic_stats_rdma_bytes - Count RDMA bytes against the device and a QP
 * @param st Statistics
 * @param qp_id QP to attribute the bytes to, or ERNIC_STAT_NO_QP for the totals
 *              only
 * @param bytes Number of bytes actually moved
 * @param stat_op Operation class
 */
void ernic_stats_rdma_bytes(struct ernic_stats *st, uint32_t qp_id,
                            uint64_t bytes, enum ernic_stat_op stat_op);

/**
 * ernic_stats_qp_doorbell - Count one doorbell ring against a QP
 * @param st Statistics
 * @param qp_id QP the doorbell targets
 * @param is_send true for the send queue, false for the receive queue
 *
 * One per ring, however many WQEs the ring makes ready.
 */
void ernic_stats_qp_doorbell(struct ernic_stats *st, uint32_t qp_id,
                             bool is_send);

/*
 * Send WQE opcodes for ernic_stats_qp_wqe(), each with its line in the
 * stats file's "WQEs by opcode".  The order and names are the PVRDMA ABI's
 * (PVRDMA_WR_*), which the file has always used.
 */
enum ernic_stat_wr {
    ERNIC_STAT_WR_RDMA_WRITE,
    ERNIC_STAT_WR_RDMA_WRITE_WITH_IMM,
    ERNIC_STAT_WR_SEND,
    ERNIC_STAT_WR_SEND_WITH_IMM,
    ERNIC_STAT_WR_RDMA_READ,
    ERNIC_STAT_WR_ATOMIC_CMP_AND_SWP,
    ERNIC_STAT_WR_ATOMIC_FETCH_AND_ADD,
    ERNIC_STAT_WR_LSO,
    ERNIC_STAT_WR_SEND_WITH_INV,
    ERNIC_STAT_WR_RDMA_READ_WITH_INV,
    ERNIC_STAT_WR_LOCAL_INV,
    ERNIC_STAT_WR_FAST_REG_MR,
    ERNIC_STAT_WR_MASKED_ATOMIC_CMP_SWP,
    ERNIC_STAT_WR_MASKED_ATOMIC_FETCH,
    ERNIC_STAT_WR_BIND_MW,
    ERNIC_STAT_WR_REG_SIG_MR,
    ERNIC_STAT_WR_ERROR,
    ERNIC_STAT_WR_SEND_DC,
    ERNIC_STAT_WR_COUNT
};

/**
 * ernic_stats_qp_wqe - Count one processed send WQE against a QP
 * @param st Statistics
 * @param qp_id QP that owns the WQE
 * @param wr enum ernic_stat_wr, or ERNIC_STAT_WR_COUNT or more to count the WQE
 *           in the QP's total only
 */
void ernic_stats_qp_wqe(struct ernic_stats *st, uint32_t qp_id,
                        unsigned int wr);

/**
 * ernic_stats_qp_cqe - Count one completion posted for a QP
 * @param st Statistics
 * @param qp_id QP the completion belongs to
 */
void ernic_stats_qp_cqe(struct ernic_stats *st, uint32_t qp_id);

/**
 * ernic_stats_qp_forget - Drop a destroyed QP's counters
 * @param st Statistics
 * @param qp_id QP being destroyed
 *
 * The stats file then stops listing it, and a new QP with the same id
 * starts from zero.
 */
void ernic_stats_qp_forget(struct ernic_stats *st, uint32_t qp_id);

/*
 * Reading the counters back
 */

/* Device totals, as reported to the guest by the ionic STATS_VALS command. */
struct ernic_dev_counters {
    uint64_t commands;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t total_bytes_rdma_read;
    uint64_t total_bytes_rdma_write;
};

/* One QP's counters, as reported by the ionic QP_STATS_VALS command. */
struct ernic_qp_counters {
    uint64_t doorbell_send;
    uint64_t doorbell_recv;
    uint64_t wqes_processed;
    uint64_t cqes_posted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_rdma_read;
    uint64_t bytes_rdma_write;
};

/**
 * ernic_stats_dev_counters - Snapshot the device totals
 * @param st Statistics
 * @param out Filled on success, zeroed on failure
 *
 * Returns: 0, or -EINVAL for a NULL @p st or @p out
 */
int ernic_stats_dev_counters(const struct ernic_stats *st,
                             struct ernic_dev_counters *out);

/**
 * ernic_stats_qp_counters - Snapshot one QP's counters
 * @param st Statistics
 * @param qp_id QP to read
 * @param out Filled on success, zeroed on failure
 *
 * A QP with nothing counted yet reads as all zeros.
 *
 * Returns: 0, or -EINVAL for a NULL @p st or @p out
 */
int ernic_stats_qp_counters(const struct ernic_stats *st, uint32_t qp_id,
                            struct ernic_qp_counters *out);

/*
 * The stats file
 */

/**
 * ernic_stats_set_file - Set the path ernic_stats_write() writes to
 * @param st Statistics
 * @param path Path, copied, or NULL for no file
 *
 * Returns: 0, or -ENOMEM, leaving no file set
 */
int ernic_stats_set_file(struct ernic_stats *st, const char *path);

/* What the stats file's "Instance" block shows about the server. */
struct ernic_stats_instance {
    const char *socket_path; /* NULL shows "(not set)" */
    const char *backend;     /* the --backend string; NULL: "(not set)" */
    const char *nic_profile; /* chain and hash; NULL: "(not set)" */
    uint16_t pci_vid;        /* both 0 shows "(not set)" */
    uint16_t pci_did;
    uint8_t mac[6];
};

/**
 * ernic_stats_set_instance - Set the stats file's "Instance" block
 * @param st Statistics
 * @param inst What to show; its strings are copied
 *
 * Returns: 0, or -ENOMEM, leaving the block as it was
 */
int ernic_stats_set_instance(struct ernic_stats *st,
                             const struct ernic_stats_instance *inst);

/**
 * ernic_stats_set_connection - Set the connection state the file shows
 * @param st Statistics
 * @param state e.g. "connected" or "disconnected (lost connection)", or NULL to
 *              show "(not set)".  Not copied: pass a string literal.
 *
 * ernic-exporter reports any state starting "connected" as up.
 */
void ernic_stats_set_connection(struct ernic_stats *st, const char *state);

/* Writes one section of the stats file to @fp. */
typedef void (*ernic_stats_section_fn)(FILE *fp, void *opaque);

/**
 * ernic_stats_set_section - Add a section to the stats file
 * @param st Statistics
 * @param write_fn Writes the section, after "Device Statistics"; NULL
 *                 removes it
 * @param opaque Passed to @p write_fn
 *
 * There is one section, which the ionic device sets for its LIF and port
 * counters.
 */
void ernic_stats_set_section(struct ernic_stats *st,
                             ernic_stats_section_fn write_fn, void *opaque);

/**
 * ernic_stats_write - Replace the stats file with the counters as they are
 * @param st Statistics
 *
 * A reader sees the old file or the new one, whole, never part of one; a
 * write that fails leaves the old file, and logs why.
 *
 * Returns: true if a new file replaced the old one; false if it didn't, or
 * there is no file
 */
bool ernic_stats_write(struct ernic_stats *st);

#endif /* ERNIC_STATS_H */
