/*
 * ionic_datapath_priv.h — ionic data path internals, for its unit tests
 *
 * The ring, memory-region and address-handle state that ionic_datapath.c
 * works with, and the helpers its unit tests call directly. Only
 * ionic_datapath.c and its unit tests include this; the rest of the device
 * sees struct ionic_datapath only through the opaque declaration in
 * ionic_datapath.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_PRIV_H
#define IONIC_DATAPATH_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_datapath.h"
#include "qemu/thread.h"
#include "rocm_ernic_compat.h"

/* -------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------
 */

#define MAX_QP (1u << 15)
#define MAX_CQ (1u << 16)
/* The same count ionic_eth_emu.c puts in the LIF identity as the guest's
 * max_srq, so an srq_id the guest is entitled to use always indexes this
 * table. */
#define MAX_SRQ IONIC_EMU_SRQ_COUNT
/* Matches the nahs_per_lif the LIF identity advertises, so an ah_id the
 * guest is entitled to allocate always indexes this table. */
#define MAX_AH (1u << 15)
/* Sized with the rdma_rm table and the admin queue's map; see
 * IONIC_MAX_MR in ionic_datapath.h for why they have to agree. */
#define MAX_MR  IONIC_MAX_MR
#define MAX_SGE 32

/* In-flight work requests waiting for a peer instance to answer. */
#define MAX_PENDING 1024
#define PENDING_MS  30000

/*
 * How long an inbound SEND waits for the receiver to post a work request
 * before we give up on it.  A peer legitimately outruns its own RQ posting,
 * which real RoCE answers with an RNR NAK and a requester-side retry; here the
 * message simply stays queued and is retried on later polls.
 */
#define RNR_RETRY_MS 5000u

/* -------------------------------------------------------------------------
 * Cross-instance wire protocol
 *
 * Two guests on the bridge each have their own emulator, so a QP pointed at
 * the other one has no local ring to deliver into and no local MR to resolve
 * its rkey against.  The mesh backend carries these messages between the two
 * as opaque payloads (TCP_MSG_IONIC); everything below is private to the
 * ionic layer.
 *
 * rkeys stay per-instance and are resolved against the *receiver's* MR
 * table, which is correct: an rkey only ever means something to the side
 * that advertised it.
 * -------------------------------------------------------------------------
 */

#define IONIC_WIRE_MAGIC 0x434e4f49u /* "IONC" */

enum ionic_wire_op {
    IONIC_WIRE_SEND = 1,
    IONIC_WIRE_SEND_IMM,
    IONIC_WIRE_SEND_INV,
    IONIC_WIRE_WRITE,
    IONIC_WIRE_WRITE_IMM,
    IONIC_WIRE_READ_REQ,
    IONIC_WIRE_READ_RESP,
    IONIC_WIRE_ATOMIC_REQ,
    IONIC_WIRE_ATOMIC_RESP,
    IONIC_WIRE_ACK,
};

/* Little-endian on the wire, except @imm_be which is passed through in the
 * big-endian form the WQE and CQE both use. */
struct ionic_wire_hdr {
    uint32_t magic;
    uint8_t op;
    uint8_t atomic_cs; /* ATOMIC_REQ: 1 = compare-swap, 0 = fetch-add */
    uint16_t rsvd;
    uint32_t src_qp_id;
    uint32_t dst_qp_id;
    uint32_t req_id;
    uint32_t rkey;
    uint32_t length;
    uint32_t imm_be;
    uint32_t status;
    uint64_t remote_va;
    uint64_t swap_add;
    uint64_t compare;
} __attribute__((packed));
_Static_assert(sizeof(struct ionic_wire_hdr) == 60,
               "struct ionic_wire_hdr is on the wire and has changed size");

/* A guest buffer: either directly addressed or described by a page table. */
struct dp_buf {
    uint64_t base;   /* direct GPA when npages <= 1                    */
    uint64_t *pages; /* page-table contents when npages > 1            */
    uint32_t npages;
    uint8_t page_size_log2;
    uint32_t first_off; /* byte offset of the region within page 0      */
};

struct ionic_qp_ring {
    bool valid;
    uint8_t ib_qp_type; /* enum ibv_qp_type, or ERNIC_IB_QPT_GSI */

    struct dp_buf sq_buf;
    struct dp_buf rq_buf;
    uint32_t sq_depth;
    uint32_t rq_depth;
    uint8_t sq_stride_log2;
    uint8_t rq_stride_log2;
    uint32_t sq_cq_id;
    uint32_t rq_cq_id;

    uint32_t sq_cons;
    uint32_t sq_prod; /* the last SQ doorbell's index, wrapped */
    /* The send queue is stopped on a SEND the peer has posted no receive
     * for, since @sq_rnr_since_ms; counted in ionic_datapath.sq_stalled. */
    bool sq_rnr;
    uint64_t sq_rnr_since_ms;
    /* The send queue is stopped on a WQE for another node that the mesh has
     * no room to queue yet; also counted in ionic_datapath.sq_stalled. */
    bool sq_busy;
    uint32_t rq_prod;
    uint32_t rq_cons;
    uint32_t msn; /* running send sequence for SEND_MSN completions */

    /*
     * The Q_Key a datagram must present to be delivered into this QP, set
     * by MODIFY_QP with IB_QP_QKEY. Only meaningful for UD and GSI; a
     * connected QP never consults it, because it never accepts a datagram
     * at all.
     */
    uint32_t qkey;
    bool qkey_valid;

    bool dest_valid;
    uint32_t dest_qp_id;
    uint32_t dest_node_id; /* UINT32_MAX = local / no mesh */

    /*
     * This QP's name in the mesh's request tags, new at each registration
     * (ionic_datapath.next_mesh_id).  qp_ids are reused, and a flush takes
     * back only this QP's requests, not those a destroyed QP with the same
     * id left queued, which still go out.
     */
    uint32_t mesh_id;

    /* Bound to a shared receive queue, in which case rq_buf/rq_prod/rq_cons
     * above are unused and every receive is taken from dp->srq[srq_id]. */
    bool has_srq;
    uint32_t srq_id;

    /* This QP's share of ionic_datapath.rdma, zeroed at registration. */
    uint64_t rdma[IONIC_DP_RDMA_COUNT];
};

/*
 * A resolved address handle: where a UD send's ah_id points.
 */
struct ionic_ah {
    bool valid;
    uint8_t dgid[16];
    /* The guest's own GID, taken from the same RoCE header template the
     * destination came out of. Needed to put a truthful source in the GRH of
     * a datagram leaving for another instance; see dp_ud_grh(). */
    uint8_t sgid[16];
    bool sgid_valid;
    uint8_t dmac[6];
    uint32_t node_id; /* UINT32_MAX = local / no mesh */
};

/*
 * A shared receive queue's ring. Same shape as a QP's RQ, plus a lock,
 * because unlike an RQ this is drawn from by several QPs and the local
 * send path and the mesh receive path run on different threads.
 */
struct ionic_srq_ring {
    QemuMutex lock;
    bool valid TSA_GUARDED_BY(lock);
    struct dp_buf buf TSA_GUARDED_BY(lock);
    uint32_t depth TSA_GUARDED_BY(lock);
    uint8_t stride_log2 TSA_GUARDED_BY(lock);
    uint32_t prod TSA_GUARDED_BY(lock);
    uint32_t cons TSA_GUARDED_BY(lock);
};

struct ionic_cq_ring {
    bool valid;
    struct dp_buf buf;
    uint32_t depth;
    uint8_t stride_log2;
    uint32_t prod;
    uint32_t cons; /* guest's consumer index, wrapped, from its ring-0 db */
    bool color;
    bool armed;
    uint32_t eq_id;
};

struct dp_mr {
    bool valid;
    uint32_t lkey;
    uint64_t va;
    uint64_t length;
    struct dp_buf buf;
};

struct dp_sge_list {
    uint64_t va[MAX_SGE];
    uint32_t len[MAX_SGE];
    uint32_t lkey[MAX_SGE];
    uint32_t count;
    uint32_t total;
};

/*
 * A work request that has been transmitted but not yet answered.  Remote
 * completions have to wait for the peer: posting one at transmit time would
 * report a latency that excludes the network, and for READ and atomics the
 * result has not even arrived yet.
 */
struct dp_pending {
    bool valid;
    uint32_t req_id;
    uint32_t qp_id;
    uint64_t wqe_id;
    uint32_t msn;
    bool use_msn;    /* RC/UC retire by MSN, everything else by SQ index */
    bool signalled;  /* NPG completions are only posted when asked for */
    uint32_t length; /* expected READ/ATOMIC response size */
    struct dp_sge_list local; /* scatter target for the response */
    uint64_t deadline_ms;
    uint64_t tag; /* what the mesh calls the request (dp_mesh_tag()) */
};

/* A mesh message parked by the receive thread; private to the .c. */
struct dp_inmsg;

struct ernic_dma;
struct ionic_eth_emu;
struct nvmeof_cm;
struct nvmeof_target;
struct s3_target;
struct s3_tcp;
#ifdef ERNIC_HAVE_P4
struct p4_engine;
#endif

struct ionic_datapath {
    vfu_ctx_t *vfu_ctx;
    /* Scratch for every guest transfer, so used only on the vfu thread,
     * which does all DMA; no lock. A test that builds the context itself,
     * rather than with ionic_datapath_create(), sets this too if the paths
     * it runs touch guest memory. */
    struct ernic_dma *dma;
    struct ionic_eth_emu *eth_emu;
    pvrdma_handle_t pvrdma_handle;

    ionic_dp_cq_event_fn_t cq_event_fn;
    void *cq_event_opaque;

    /* In-process NVMe-oF controller, NULL unless --backend nvmeof. */
    struct nvmeof_target *nvmeof;
    struct nvmeof_cm *nvmeof_cm;
    uint8_t nvmeof_sgid[16]; /* controller GID, learned from the CM REQ */
    uint8_t nvmeof_dgid[16]; /* the guest's own GID                    */

    /* In-process S3 object store, NULL unless --backend s3. */
    struct s3_target *s3;
    struct s3_tcp *s3_tcp;

#ifdef ERNIC_HAVE_P4
    /*
     * The P4 pipeline that governs RDMA operations, NULL unless a program
     * was loaded.  The data path does not own it: the server creates it,
     * the Ethernet port shares it, and this is a borrowed pointer that
     * must be cleared before the engine is destroyed.
     */
    struct p4_engine *p4_engine;
#endif

    struct ionic_qp_ring *qp;   /* indexed by driver qp_id */
    struct ionic_cq_ring *cq;   /* indexed by driver cq_id */
    struct ionic_srq_ring *srq; /* indexed by driver srq_id */
    struct ionic_ah *ah;        /* indexed by driver ah_id */
    struct dp_mr mr[MAX_MR];

    uint32_t qp_count;
    uint32_t cq_count;
    uint32_t srq_count;
    uint32_t ah_count;

    /* Mesh state.  local_node is UINT32_MAX until a mesh backend is
     * attached, which keeps every peer local for loopback runs. */
    uint32_t local_node;
    /* The poll loop re-attaches the handle every iteration, so the mesh
     * state is only worth logging when it actually changes. */
    bool mesh_reported;
    uint32_t next_req_id;
    uint32_t next_mesh_id; /* the last QP registered's mesh_id */
    struct dp_pending pending[MAX_PENDING];

    QemuMutex rx_lock;
    struct dp_inmsg *rx_head TSA_GUARDED_BY(rx_lock);
    struct dp_inmsg *rx_tail TSA_GUARDED_BY(rx_lock);
    /*
     * Messages on @rx_head that have never been tried.  Only these justify
     * skipping the main loop's idle sleep: a message put back by the RNR
     * defer path will still be undeliverable on the next pass, so treating it
     * as work would spin the loop until the peer posts a receive.
     */
    uint32_t rx_fresh TSA_GUARDED_BY(rx_lock);

    /*
     * Why the last transfer stopped short.  A byte count on its own cannot
     * say: a short copy is a receive buffer that ran out, an SGE outside the
     * MR its key names, or a guest mapping that refused the DMA -- and those
     * are three different completion statuses.  Everything that touches this
     * runs on the vfu thread, so a plain field is enough.
     */
    uint32_t fault;

    /* QPs whose send queue is stopped behind an RNR (sq_rnr), or on a full
     * mesh send queue (sq_busy). */
    uint32_t sq_stalled;

    /*
     * RDMA error counters for the whole device (IONIC_DP_RDMA_COUNTERS).
     * Like @fault, they only move on the vfu thread.
     */
    uint64_t rdma[IONIC_DP_RDMA_COUNT];

    /*
     * The clock, in milliseconds, that the RNR wait and the deadlines on
     * @pending run on.  NULL, as ionic_datapath_create() leaves it, is
     * CLOCK_MONOTONIC; a unit test sets its own, to land exactly on those
     * deadlines.  Read on the vfu thread only.
     */
    uint64_t (*now_ms)(void);
};

/* Hardware qtypes we advertise in Q_IDENTIFY (see ionic_eth_emu.c). */
#define DP_QTYPE_SQ 6
#define DP_QTYPE_RQ 7
#define DP_QTYPE_CQ 8

/* Free @b's page table and zero it, so it can be initialised again. */
void ionic_dp_buf_release(struct dp_buf *b);

/*
 * 2^(depth_log2 + stride_log2), or 0 when that is not a size any ring could
 * have.
 */
uint64_t ionic_dp_ring_span_bytes(uint8_t depth_log2, uint8_t stride_log2);

/*
 * Set @b up from the guest's description of a buffer covering @span bytes
 * from @va, reading its page table out of guest memory. Returns 0, or
 * -EINVAL for a geometry the device can't honour, -ENOMEM, or -EIO.
 */
int ionic_dp_buf_init(const struct ionic_datapath *dp, struct dp_buf *b,
                      const struct ionic_dp_buf_desc *desc, uint64_t va,
                      uint64_t span);

/*
 * Why a send from @sq may not deliver into @dq with Q_Key @sent_qkey, or
 * NULL when it may.
 */
const char *ionic_dp_ud_delivery_refused(const struct ionic_qp_ring *sq,
                                         const struct ionic_qp_ring *dq,
                                         uint32_t sent_qkey);

/* Directions for ionic_dp_rdma_bytes(); a local copy both sends and
 * receives. */
#define DP_RDMA_TX 1u
#define DP_RDMA_RX 2u

/*
 * Count @bytes of RDMA payload once for the stats file (by @op, against
 * @qp_id) and once per direction in @dirs for the guest's struct
 * ionic_lif_stats (tx/rx_rdma_ucast_*).
 */
void ionic_dp_rdma_bytes(const struct ionic_datapath *dp, uint32_t qp_id,
                         uint64_t bytes, enum pvrdma_stat_op op,
                         unsigned int dirs);

#endif /* IONIC_DATAPATH_PRIV_H */
