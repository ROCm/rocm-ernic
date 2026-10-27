/*
 * ionic_harness.h — a userspace model of the ionic driver, driving the
 * real device code in-process
 *
 * The harness builds the device with the calls the server makes -- the RDMA
 * core and backend, then ionic_device_create() -- over the fake libvfio-user
 * in fake_vfu.c, and then plays the guest driver against it: register reads
 * and writes on BAR0, doorbells on BAR2, and rings, work requests and
 * completions in guest memory. Each ih_poll() is the device's share of one
 * pass of the server's main loop, ionic_device_poll().
 *
 * Every layout below is written out from the driver's side (ionic_if.h and
 * ionic_fw.h) rather than shared with the device code, so a wrong offset
 * in one doesn't cancel a wrong offset in the other.
 *
 * Functions that can fail print why to stderr and return false (or a
 * negative value); the tests turn that into a failure.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_HARNESS_H
#define IONIC_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_internal.h"

/* ---- Wire constants, from the driver's headers -------------------------- */

/* BAR2 doorbell qtypes: Ethernet logical queues, then the RDMA hardware
 * qtypes the LIF identity advertises */
#define IH_QTYPE_ETH_ADMINQ 0
#define IH_QTYPE_ETH_RXQ    2
#define IH_QTYPE_ETH_TXQ    3
#define IH_QTYPE_AQ         5
#define IH_QTYPE_SQ         6
#define IH_QTYPE_RQ         7
#define IH_QTYPE_CQ         8

/* Device commands (enum ionic_cmd_opcode) */
#define IH_CMD_NOP           0
#define IH_CMD_IDENTIFY      1
#define IH_CMD_RESET         3
#define IH_CMD_PORT_IDENTIFY 10
#define IH_CMD_PORT_INIT     11
#define IH_CMD_LIF_IDENTIFY  20
#define IH_CMD_LIF_INIT      21
#define IH_CMD_LIF_RESET     22
#define IH_CMD_LIF_GETATTR   23
#define IH_CMD_LIF_SETATTR   24
#define IH_CMD_Q_IDENTIFY    39
#define IH_CMD_Q_INIT        40
#define IH_CMD_Q_CONTROL     41
#define IH_CMD_RDMA_RESET    50
#define IH_CMD_RDMA_EQ       51
#define IH_CMD_RDMA_CQ       52
#define IH_CMD_RDMA_ADMINQ   53

#define IH_LIF_ATTR_MAC 3

/* RDMA admin opcodes (enum ionic_v1_admin_op) */
#define IH_ADMIN_NOOP        0
#define IH_ADMIN_CREATE_CQ   1
#define IH_ADMIN_CREATE_QP   2
#define IH_ADMIN_CREATE_MR   3
#define IH_ADMIN_STATS_HDRS  4
#define IH_ADMIN_STATS_VALS  5
#define IH_ADMIN_DESTROY_MR  6
#define IH_ADMIN_DESTROY_CQ  8
#define IH_ADMIN_MODIFY_QP   9
#define IH_ADMIN_QUERY_QP    10
#define IH_ADMIN_DESTROY_QP  11
#define IH_ADMIN_CREATE_AH   13
#define IH_ADMIN_QUERY_AH    14
#define IH_ADMIN_DESTROY_AH  16
#define IH_ADMIN_CREATE_SRQ  26
#define IH_ADMIN_MODIFY_SRQ  27
#define IH_ADMIN_QUERY_SRQ   28
#define IH_ADMIN_DESTROY_SRQ 29

/* enum ionic_qp_type, as CREATE_QP carries it */
#define IH_QPT_RC 0
#define IH_QPT_UC 1
#define IH_QPT_UD 3

/* IB QP states and the MODIFY_QP attribute mask bits */
#define IH_QPS_RESET 0
#define IH_QPS_INIT  1
#define IH_QPS_RTR   2
#define IH_QPS_RTS   3
#define IH_QPS_SQD   4
#define IH_QPS_SQE   5
#define IH_QPS_ERR   6

/* The completion status a flushed work request carries, IONIC_STS_* in the
 * device and IB_WC_WR_FLUSH_ERR by the time a consumer sees it. */
#define IH_STS_WQE_FLUSHED_ERR 4u

#define IH_QP_STATE    (1u << 0)
#define IH_QP_QKEY     (1u << 6)
#define IH_QP_AV       (1u << 7)
#define IH_QP_RQ_PSN   (1u << 12)
#define IH_QP_SQ_PSN   (1u << 16)
#define IH_QP_DEST_QPN (1u << 20)

/* IB access flags, the low bits of CREATE_MR's flags */
#define IH_ACCESS_LOCAL_WRITE   (1u << 0)
#define IH_ACCESS_REMOTE_WRITE  (1u << 1)
#define IH_ACCESS_REMOTE_READ   (1u << 2)
#define IH_ACCESS_REMOTE_ATOMIC (1u << 3)
#define IH_ACCESS_ALL                                                         \
    (IH_ACCESS_LOCAL_WRITE | IH_ACCESS_REMOTE_WRITE | IH_ACCESS_REMOTE_READ | \
     IH_ACCESS_REMOTE_ATOMIC)

/* enum ionic_v1_op */
#define IH_OP_SEND           0
#define IH_OP_SEND_INV       1
#define IH_OP_SEND_IMM       2
#define IH_OP_RDMA_READ      3
#define IH_OP_RDMA_WRITE     4
#define IH_OP_RDMA_WRITE_IMM 5
#define IH_OP_ATOMIC_CS      6
#define IH_OP_ATOMIC_FA      7
#define IH_OP_REG_MR         8
#define IH_OP_LOCAL_INV      9

/* enum ionic_v1_flag */
#define IH_WQE_INLINE    0x0004u
#define IH_WQE_SIGNALLED 0x0008u

/* ionic_v1_cqe: type in bits 7:5 of qid_type_flags, receive opcode in the
 * top byte of src_qpn_op */
#define IH_CQE_TYPE_ADMIN    0
#define IH_CQE_TYPE_RECV     1
#define IH_CQE_TYPE_SEND_MSN 2
#define IH_CQE_TYPE_SEND_NPG 3

#define IH_RECV_OP_SEND     0
#define IH_RECV_OP_SEND_INV 1
#define IH_RECV_OP_SEND_IMM 2
#define IH_RECV_OP_RDMA_IMM 3

/* enum ionic_status */
#define IH_STS_OK             0
#define IH_STS_LOCAL_LEN_ERR  1
#define IH_STS_LOCAL_QP_OPER  2
#define IH_STS_LOCAL_PROT_ERR 3
#define IH_STS_MEM_MGMT_ERR   5
#define IH_STS_LOCAL_ACC_ERR  7
#define IH_STS_REMOTE_ACC_ERR 9

/* MSI-X vectors the harness gives its queues */
#define IH_ETH_ADMINQ_INTR 0
#define IH_EQ_INTR         1

/* The EQ every CQ reports on, and the admin CQ's id */
#define IH_EQ_ID  0
#define IH_ACQ_ID 0

/* Bytes of global route header ahead of every UD receive */
#define IH_GRH_SIZE 40

/* ---- State ------------------------------------------------------------- */

/* A ring in guest memory, and where the driver is in it. */
struct ih_ring {
    uint64_t gpa;
    uint8_t depth_log2;
    uint8_t stride_log2;
    uint32_t prod; /* free-running */
    uint32_t cons; /* free-running */
};

/* How many non-admin EQ events an admin wait will step over and hold. */
#define IH_EQ_HELD_MAX 16u

struct ih {
    vfu_ctx_t *vfu;
    rocm_ernic_dev_t *dev;

    /* The ionic device's parts, for tests that work on one directly */
    struct ionic_eth_emu *emu;
    struct ionic_rdma_devcmd_state *rdma;
    struct ionic_datapath *dp;

    /* Ethernet admin queue and its completion queue */
    struct ih_ring eth_aq;
    struct ih_ring eth_acq;

    /* RDMA event queue, admin queue and admin completion queue */
    struct ih_ring eq;
    struct ih_ring aq;
    struct ih_ring acq;

    /* The DMA areas PORT_INIT and LIF_INIT publish into */
    uint64_t port_info;
    uint64_t lif_info;

    /* A page for RoCE header templates and side buffers */
    uint64_t scratch;

    /*
     * EQ events stepped over while looking for an admin completion's.
     * Waiting on the admin CQ has to tolerate other CQs' events arriving
     * first -- an admin command can complete work on a data CQ, as
     * MODIFY_QP to Error does when it flushes -- and the real driver copes
     * because it drains the EQ and dispatches on the qid rather than
     * assuming the order. Holding them here keeps them visible to
     * ih_eq_next() instead of dropping them on the floor.
     */
    uint32_t eq_held[IH_EQ_HELD_MAX];
    unsigned eq_held_n;
};

struct ih_cq {
    uint32_t id;
    struct ih_ring ring;
};

struct ih_srq {
    uint32_t id; /* as CREATE_SRQ returned it */
    struct ih_ring ring;
    uint64_t next_wqe_id;
};

struct ih_qp {
    uint32_t id;
    uint8_t type; /* IH_QPT_* */
    struct ih_ring sq;
    struct ih_ring rq;
    uint64_t next_wqe_id;
};

/*
 * A memory region as userspace sees it: a virtual range backed by guest
 * pages through a page table. The pages are 4 KiB and may be laid out in
 * any order in guest memory.
 */
#define IH_PAGE_SIZE 4096u

struct ih_mr {
    uint32_t lkey; /* the driver's full mrid: index | key << 24 */
    uint64_t va;
    uint64_t len;
    uint32_t npages;
    uint64_t *pages; /* GPA of each page */
    uint64_t pt_gpa; /* the page table, in guest memory */
};

struct ih_sge {
    uint64_t va;
    uint32_t len;
    uint32_t lkey;
};

/* One send-queue work request. Fields an opcode doesn't use are ignored. */
struct ih_send {
    uint8_t op;
    uint16_t flags;
    uint32_t imm_be; /* SEND_IMM / WRITE_IMM data, already big-endian */
    uint32_t key;    /* SEND_INV / LOCAL_INV / REG_MR key */

    /* UD */
    uint32_t ah_id;
    uint32_t dest_qpn;
    uint32_t qkey;

    /* RDMA and atomics */
    uint64_t remote_va;
    uint32_t rkey;
    /* RDMA length, filled in from the SGEs if 0; or, with IH_WQE_INLINE,
     * the length the WQE claims, @inline_len if 0 */
    uint32_t length;
    uint64_t swap_add;
    uint64_t compare;

    const struct ih_sge *sge;
    uint8_t nsge;

    /* With IH_WQE_INLINE: the payload carried in the WQE itself */
    const void *inline_data;
    uint32_t inline_len;

    /* REG_MR: the region to bind under @key */
    const struct ih_mr *mr;
};

/* A completion, decoded. */
struct ih_cqe {
    uint8_t raw[32];
    uint8_t type; /* IH_CQE_TYPE_* */
    bool error;
    uint32_t qid;
    uint32_t status_len; /* byte count, or status when @error */
    uint64_t wqe_id;     /* recv and NPG completions */
    uint32_t msn;        /* MSN completions */
    uint8_t recv_op;
    uint32_t src_qpn;
    uint32_t imm_be;
    bool has_smac;
    uint8_t smac[6];
    /*
     * IONIC_V1_CQE_RECV_IS_IPV4. The driver reports this as
     * wc->network_hdr_type, which is the only thing that tells a receiver
     * whether the 40-byte header in front of the payload is an IPv6 GRH or
     * a RoCEv2 IPv4 header -- so a test that checks the header's shape has
     * to check this with it.
     */
    bool is_ipv4;
};

/* ---- Device lifecycle -------------------------------------------------- */

/*
 * Build a device for @backend ("loopback", "nvmeof:...", and so on), as the
 * server's device_init() does. Nothing is brought up yet.
 */
bool ih_create(struct ih *h, const char *backend);

/*
 * ih_create(), injecting @faults (ionic_fault.h), as --inject does. The set
 * stays the caller's, and must outlive the device.
 */
struct ionic_fault_set;
bool ih_create_faults(struct ih *h, const char *backend,
                      struct ionic_fault_set *faults);

/* Tear it down as the server does at shutdown. False if the device code
 * left guest memory mapped. */
bool ih_destroy(struct ih *h);

/* One pass of the server's main loop. */
void ih_poll(struct ih *h);

/*
 * Everything the driver does before the first RDMA admin command: the
 * devcmds from IDENTIFY to LIF_INIT, the Ethernet admin queue, interrupt
 * unmasking, and the four RDMA devcmds that create the EQ, the admin CQ
 * and the admin queue.
 */
bool ih_bringup(struct ih *h);

/* ---- Registers and doorbells ------------------------------------------- */

void ih_bar0_read(struct ih *h, uint32_t off, void *buf, size_t len);
void ih_bar0_write(struct ih *h, uint32_t off, const void *buf, size_t len);
uint32_t ih_bar0_read32(struct ih *h, uint32_t off);
void ih_bar0_write32(struct ih *h, uint32_t off, uint32_t val);

/* Mask or unmask one MSI-X vector through its intr_ctrl registers. */
void ih_intr_mask(struct ih *h, uint32_t vec, bool masked);

void ih_doorbell(struct ih *h, int qtype, uint32_t qid, uint8_t ring,
                 uint16_t p_index);

/*
 * Run one devcmd through the BAR0 window: @cmd in, completion and status
 * out. @data, if not NULL, gets the first @data_len bytes of the data
 * window afterwards. Returns the completion status, or -1 if the device
 * didn't set done.
 */
int ih_devcmd(struct ih *h, const uint8_t cmd[64], uint8_t comp[16], void *data,
              size_t data_len);

/*
 * Run one command through the Ethernet admin queue. Returns the completion
 * status, or -1 if no completion arrived.
 */
int ih_eth_admin(struct ih *h, const uint8_t cmd[64], uint8_t comp[16]);

/*
 * Run one RDMA admin command. Returns 0 on success, the completion's status
 * when it has the error bit, or -1 if no completion arrived. @resp_len, if
 * not NULL, gets the completion's length field on success.
 */
int ih_admin(struct ih *h, uint8_t op, const void *body, uint16_t len,
             uint32_t *resp_len);

/*
 * The two halves of ih_admin(), for a test that has to do something between
 * them. ih_admin_post() writes the WQE and rings the doorbell, and returns
 * the ring slot the completion will name, or -1. ih_admin_complete() takes
 * that completion off the admin CQ and returns as ih_admin() does, or
 * IH_ADMIN_PENDING if it hasn't arrived.
 */
#define IH_ADMIN_PENDING (-2)

int ih_admin_post(struct ih *h, uint8_t op, const void *body, uint16_t len);
int ih_admin_complete(struct ih *h, uint8_t op, int slot, uint32_t *resp_len);

/* The next event on the EQ, or false if there is none. */
bool ih_eq_next(struct ih *h, uint32_t *cq_id);

/* ---- Resources --------------------------------------------------------- */

bool ih_cq_create(struct ih *h, struct ih_cq *cq, uint32_t id,
                  uint8_t depth_log2);
int ih_cq_destroy(struct ih *h, struct ih_cq *cq);

/*
 * Create a QP on @scq and @rcq. @srq binds its receives to a shared queue;
 * pass NULL for a QP with its own receive ring.
 */
bool ih_qp_create(struct ih *h, struct ih_qp *qp, uint32_t id, uint8_t type,
                  const struct ih_cq *scq, const struct ih_cq *rcq,
                  const struct ih_srq *srq);

/*
 * The largest send WQE stride the device advertises (max_stride in the LIF
 * identity), as log2 bytes.
 */
#define IH_WQE_MAX_STRIDE_LOG2 9u

/*
 * ih_qp_create(), with a send queue stride of 2^@sq_stride_log2 bytes rather
 * than the 128 every other QP uses, up to IH_WQE_MAX_STRIDE_LOG2.
 */
bool ih_qp_create_stride(struct ih *h, struct ih_qp *qp, uint32_t id,
                         uint8_t type, const struct ih_cq *scq,
                         const struct ih_cq *rcq, const struct ih_srq *srq,
                         uint8_t sq_stride_log2);
int ih_qp_destroy(struct ih *h, struct ih_qp *qp);

/*
 * MODIFY_QP. @qkey_dest_qpn is the destination QPN with IH_QP_DEST_QPN or
 * the Q_Key with IH_QP_QKEY. @hdr, if not NULL, is a RoCE header template
 * the device reads the peer's address from.
 */
int ih_qp_modify(struct ih *h, uint32_t qp_id, uint32_t attr_mask,
                 uint8_t state, uint32_t qkey_dest_qpn, const void *hdr,
                 uint32_t hdr_len);

/* Take two RC QPs through INIT, RTR and RTS, pointed at each other. */
bool ih_qp_connect(struct ih *h, const struct ih_qp *a, const struct ih_qp *b);

/*
 * Register [@va, @va + @len) as @lkey, backed by fresh guest pages. With
 * @reversed the pages go into guest memory in reverse order, so no two
 * neighbours are physically adjacent.
 */
bool ih_mr_create(struct ih *h, struct ih_mr *mr, uint32_t lkey, uint64_t va,
                  uint64_t len, uint32_t access, bool reversed);
int ih_mr_destroy(struct ih *h, struct ih_mr *mr);

/* Allocate the backing pages and page table only, without CREATE_MR, for
 * an MR that a REG_MR work request will bind. */
bool ih_mr_alloc(struct ih *h, struct ih_mr *mr, uint32_t lkey, uint64_t va,
                 uint64_t len, bool reversed);
void ih_mr_free(struct ih_mr *mr);

/* Copy through an MR's page table, as the application sees its memory. */
bool ih_mr_write(struct ih *h, const struct ih_mr *mr, uint64_t va,
                 const void *buf, size_t len);
bool ih_mr_read(struct ih *h, const struct ih_mr *mr, uint64_t va, void *buf,
                size_t len);

/* CREATE_SRQ; the id comes back by DMA. Returns as ih_admin() does. */
int ih_srq_create(struct ih *h, struct ih_srq *srq, uint8_t depth_log2);
int ih_srq_destroy(struct ih *h, struct ih_srq *srq);

/* CREATE_AH from an IPv4 RoCE header template. */
int ih_ah_create(struct ih *h, uint32_t ah_id, uint32_t sip, uint32_t dip);

/*
 * Build the header template ib_ud_header_pack() leaves for an IPv4 RoCE
 * peer: Ethernet, IPv4, UDP. Returns its length.
 */
size_t ih_roce_hdr_v4(uint8_t *buf, size_t cap, uint32_t sip, uint32_t dip);

/* ---- Work requests and completions ------------------------------------- */

/* Post one send WQE and ring the SQ doorbell. Returns its wqe_id. */
uint64_t ih_post_send(struct ih *h, struct ih_qp *qp, const struct ih_send *s);

/* Post one receive and ring the RQ doorbell. Returns its wqe_id. */
uint64_t ih_post_recv(struct ih *h, struct ih_qp *qp, const struct ih_sge *sge,
                      uint8_t nsge);
uint64_t ih_post_srq_recv(struct ih *h, struct ih_srq *srq,
                          const struct ih_sge *sge, uint8_t nsge);

/* Take the next completion off @cq and return the consumer index to the
 * device. False if there is none. */
bool ih_cq_poll(struct ih *h, struct ih_cq *cq, struct ih_cqe *cqe);

/* Arm @cq for its next completion. */
void ih_cq_arm(struct ih *h, struct ih_cq *cq);

#endif /* IONIC_HARNESS_H */
