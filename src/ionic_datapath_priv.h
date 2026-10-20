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

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_datapath.h"
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
    uint8_t ib_qp_type;

    struct dp_buf sq_buf;
    struct dp_buf rq_buf;
    uint32_t sq_depth;
    uint32_t rq_depth;
    uint8_t sq_stride_log2;
    uint8_t rq_stride_log2;
    uint32_t sq_cq_id;
    uint32_t rq_cq_id;

    uint32_t sq_cons;
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

    /* Bound to a shared receive queue, in which case rq_buf/rq_prod/rq_cons
     * above are unused and every receive is taken from dp->srq[srq_id]. */
    bool has_srq;
    uint32_t srq_id;
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
    bool valid;
    struct dp_buf buf;
    uint32_t depth;
    uint8_t stride_log2;
    uint32_t prod;
    uint32_t cons;
    pthread_mutex_t lock;
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
};

/* A mesh message parked by the receive thread; private to the .c. */
struct dp_inmsg;

struct ionic_eth_emu;
struct nvmeof_cm;
struct nvmeof_target;
struct s3_target;
struct s3_tcp;

struct ionic_datapath {
    vfu_ctx_t *vfu_ctx;
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
    struct dp_pending pending[MAX_PENDING];

    pthread_mutex_t rx_lock;
    struct dp_inmsg *rx_head;
    struct dp_inmsg *rx_tail;
    /*
     * Messages on @rx_head that have never been tried.  Only these justify
     * skipping the main loop's idle sleep: a message put back by the RNR
     * defer path will still be undeliverable on the next pass, so treating it
     * as work would spin the loop until the peer posts a receive.
     */
    uint32_t rx_fresh;

    /*
     * Why the last transfer stopped short.  A byte count on its own cannot
     * say: a short copy is a receive buffer that ran out, an SGE outside the
     * MR its key names, or a guest mapping that refused the DMA -- and those
     * are three different completion statuses.  Everything that touches this
     * runs on the vfu thread, so a plain field is enough.
     */
    uint32_t fault;
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
int ionic_dp_buf_init(struct ionic_datapath *dp, struct dp_buf *b,
                      const struct ionic_dp_buf_desc *desc, uint64_t va,
                      uint64_t span);

/*
 * Why a send from @sq may not deliver into @dq with Q_Key @sent_qkey, or
 * NULL when it may.
 */
const char *ionic_dp_ud_delivery_refused(const struct ionic_qp_ring *sq,
                                         const struct ionic_qp_ring *dq,
                                         uint32_t sent_qkey);

#endif /* IONIC_DATAPATH_PRIV_H */
