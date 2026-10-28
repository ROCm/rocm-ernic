/*
 * ionic_datapath_priv.h — ionic data path internals
 *
 * The ring, memory-region and address-handle state that ionic_datapath.c
 * works with, the helpers it shares with the files that hook the in-process
 * targets and the P4 pipeline into it (ionic_datapath_nvmeof.c,
 * ionic_datapath_s3.c, ionic_datapath_p4.c), and the helpers its unit tests
 * call directly. Only those files and the data path's unit tests include
 * this; the rest of the device sees struct ionic_datapath only through the
 * opaque declaration in ionic_datapath.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_PRIV_H
#define IONIC_DATAPATH_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "ernic_limits.h"
#include "ernic_log.h"
#include "ernic_mesh.h"
#include "ernic_stats.h"
#include "ionic/ionic_datapath.h"
#include "net/net_headers.h"
#include "qemu/thread.h"
#include "rocm_ernic_compat.h"

/* -------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------
 */

/*
 * The QP, CQ, SRQ, AH and MR tables hold ernic_limits.h's counts, what the
 * LIF identity promises the guest, so every id it may use indexes them.
 */

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

/*
 * How much the queue of messages from other nodes holds, in bytes and in
 * messages.  When it is full, the mesh's receive thread waits for room before
 * reading on, so a peer that sends faster than the main thread applies is
 * slowed down by TCP, not buffered without limit.  An empty queue takes any
 * one message.  Four of the largest, so a few can come in while a pass
 * applies a few more.
 */
#define DP_RX_MAX_BYTES ((size_t)4 * ERNIC_MESH_MAX_MSG)
#define DP_RX_MAX_MSGS  4096u
_Static_assert(DP_RX_MAX_BYTES >= ERNIC_MESH_MAX_MSG,
               "the inbound queue must hold the largest message");

/*
 * A pass leaves SENDs waiting for a receive on the queue, and what is behind
 * them for the same QP.  Once they have left no room for a message a
 * receive thread is waiting to queue for this long, the next pass gives up
 * on them as if RNR_RETRY_MS had passed: waiting out RNR_RETRY_MS would
 * leave the peer's connection unread for as long.  Well under the 5 s a
 * manager's send waits for a peer that has stopped reading
 * (TCP_DEFAULT_SEND_TIMEOUT_MS, halved), past which the peer gives up on
 * this node's connection.
 */
#define DP_RX_CROWDED_MS 500u

/*
 * How long the main thread may go from the end of one pass without starting
 * another before a full queue stops holding up the receive threads and
 * drops what they bring instead.  The main thread polls only while a client
 * is attached, and a receive thread that waited for a pass that isn't coming
 * would leave its peer's connection unread until the peer gave up on it.
 * Under the 5 s that takes, as for DP_RX_CROWDED_MS.  A pass that runs
 * longer is still applying what is queued, and holds the receive threads up
 * however long it takes.
 */
#define DP_RX_STUCK_MS 2000u

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

/*
 * The header at the front of every message, IONIC_WIRE_HDR_LEN bytes:
 *
 *   [0:3]   le32 magic (IONIC_WIRE_MAGIC)
 *   [4]     op (enum ionic_wire_op)
 *   [5]     ATOMIC_REQ: 1 = compare-swap, 0 = fetch-add
 *   [6:7]   reserved, zero
 *   [8:11]  le32 src_qp_id
 *   [12:15] le32 dst_qp_id
 *   [16:19] le32 req_id
 *   [20:23] le32 rkey
 *   [24:27] le32 length
 *   [28:31] be32 immediate data
 *   [32:35] le32 status
 *   [36:43] le64 remote_va
 *   [44:51] le64 swap_add
 *   [52:59] le64 compare
 *
 * The payload follows it.  For a SEND, a WRITE and a READ_RESP it is the
 * guest's data, bytes with no byte order of their own; for an ATOMIC_RESP
 * it is the original value, le64.
 *
 * This header carries its fields little-endian, inside a mesh message whose
 * own header (TcpMsgHeader) is big-endian.  The two are separate layers, and
 * neither can change without breaking an unupgraded peer.
 */
#define IONIC_WIRE_HDR_LEN       60
#define IONIC_WIRE_MAGIC_OFF     0
#define IONIC_WIRE_OP_OFF        4
#define IONIC_WIRE_ATOMIC_CS_OFF 5
#define IONIC_WIRE_SRC_QP_OFF    8
#define IONIC_WIRE_DST_QP_OFF    12
#define IONIC_WIRE_REQ_ID_OFF    16
#define IONIC_WIRE_RKEY_OFF      20
#define IONIC_WIRE_LENGTH_OFF    24
#define IONIC_WIRE_IMM_OFF       28
#define IONIC_WIRE_STATUS_OFF    32
#define IONIC_WIRE_REMOTE_VA_OFF 36
#define IONIC_WIRE_SWAP_ADD_OFF  44
#define IONIC_WIRE_COMPARE_OFF   52
_Static_assert(IONIC_WIRE_COMPARE_OFF + 8 == IONIC_WIRE_HDR_LEN,
               "the ionic wire header is on the wire and has changed size");

/*
 * The header as the code uses it, in host byte order but for @imm_be, which
 * holds the immediate data's four bytes as the WQE and CQE do: big-endian.
 * ionic_dp_wire_hdr_put() and ionic_dp_wire_hdr_get() convert it to and
 * from the wire.
 */
struct ionic_wire_hdr {
    uint32_t magic;
    uint8_t op;
    uint8_t atomic_cs;
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
};

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
    uint8_t dgid[ERNIC_GID_LEN];
    /* The guest's own GID, taken from the same RoCE header template the
     * destination came out of. Needed to put a truthful source in the GRH of
     * a datagram leaving for another instance; see dp_ud_grh(). */
    uint8_t sgid[ERNIC_GID_LEN];
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
    uint64_t va[ERNIC_MAX_SGE];
    uint32_t len[ERNIC_MAX_SGE];
    uint32_t lkey[ERNIC_MAX_SGE];
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
    uint64_t tag;         /* what the mesh calls the request (dp_mesh_tag()) */
    uint32_t dest_node;   /* the node the request went to */
    uint32_t reply_bytes; /* the most its reply can be, header included */
};

/*
 * A request whose reply is the largest a message can be still fits a budget
 * of its own, so the budget never holds up a send queue for good.
 */
_Static_assert(ERNIC_MESH_REPLY_BUDGET >= ERNIC_MESH_MAX_MSG,
               "ERNIC_MESH_REPLY_BUDGET must hold the largest reply");

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
    /* The server's counters, or NULL */
    struct ernic_stats *stats;

    ionic_dp_cq_event_fn_t cq_event_fn;
    void *cq_event_opaque;

    /* In-process NVMe-oF controller, NULL unless --backend nvmeof. */
    struct nvmeof_target *nvmeof;
    struct nvmeof_cm *nvmeof_cm;
    uint8_t nvmeof_sgid[ERNIC_GID_LEN]; /* controller GID, learned from the CM
                                           REQ */
    uint8_t nvmeof_dgid[ERNIC_GID_LEN]; /* the guest's own GID */

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
    /*
     * The limits on what ionic_dp_p4_policy() logs, which a guest can make
     * it log once per work request: a denial, a failed pipeline, and an
     * extern call without a drop.  Used only on the vfu thread, through
     * ionic_dp_log_ok().
     */
    ErnicLogLimit p4_log_denied;
    ErnicLogLimit p4_log_failed;
    ErnicLogLimit p4_log_no_drop;
#endif

    struct ionic_qp_ring *qp;   /* indexed by driver qp_id */
    struct ionic_cq_ring *cq;   /* indexed by driver cq_id */
    struct ionic_srq_ring *srq; /* indexed by driver srq_id */
    struct ionic_ah *ah;        /* indexed by driver ah_id */
    struct dp_mr mr[ERNIC_MAX_MR];

    uint32_t qp_count;
    uint32_t cq_count;
    uint32_t srq_count;
    uint32_t ah_count;

    /* Mesh state.  local_node is UINT32_MAX until a mesh backend is
     * attached, which keeps every peer local for loopback runs. */
    uint32_t local_node;
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
     * What the queue holds, against DP_RX_MAX_BYTES and DP_RX_MAX_MSGS:
     * every message on @rx_head, and those a pass has taken off it and not
     * yet freed, which are still in memory.
     */
    size_t rx_bytes TSA_GUARDED_BY(rx_lock);
    uint32_t rx_msgs TSA_GUARDED_BY(rx_lock);
    /*
     * The largest message the queue has refused for want of room, until a
     * pass leaves room for it, or 0.  Kept from pass to pass, since a
     * receive thread tries again less often than the main thread passes.
     * It can outlast the thread that was refused, which, if the SENDs left
     * waiting have no room for it, only has their wait end early.
     */
    size_t rx_waiting TSA_GUARDED_BY(rx_lock);
    /*
     * For a receive thread to tell a main thread that has stopped polling
     * (DP_RX_STUCK_MS) from one in a long pass: whether a pass is under
     * way, and when the last one ended, on @now_ms's clock.
     */
    bool rx_in_pass TSA_GUARDED_BY(rx_lock);
    uint64_t rx_polled_ms TSA_GUARDED_BY(rx_lock);
    /* Messages dropped because the queue was full and nothing polled it */
    uint64_t rx_dropped TSA_GUARDED_BY(rx_lock);

    /*
     * The rest is the main thread's alone.  @rx_crowded is set while the
     * messages each pass leaves waiting for a receive leave no room for
     * @rx_waiting, since @rx_crowded_ms on @now_ms's clock.
     * @rx_dropped_logged is how much of @rx_dropped has been logged.
     * @log_cq_error limits cq_post()'s line for each error completion,
     * which a guest can have it log once per work request.
     */
    bool rx_crowded;
    uint64_t rx_crowded_ms;
    uint64_t rx_dropped_logged;
    ErnicLogLimit log_cq_error;

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
     * The clock, in milliseconds, that the RNR wait, the deadlines on
     * @pending and the inbound queue's limits run on.  NULL, as
     * ionic_datapath_create() leaves it, is CLOCK_MONOTONIC; a unit test sets
     * its own, before anything uses @dp, to land exactly on those deadlines.
     * Read on the vfu thread, and by the mesh's receive threads in
     * dp_mesh_recv(), which can do so without a lock because it never
     * changes once they run.
     */
    uint64_t (*now_ms)(void);
};

/* Hardware qtypes we advertise in Q_IDENTIFY (see ionic_eth_emu.c). */
#define DP_QTYPE_SQ 6
#define DP_QTYPE_RQ 7
#define DP_QTYPE_CQ 8

/* Write @h to the IONIC_WIRE_HDR_LEN bytes at @wire. */
void ionic_dp_wire_hdr_put(uint8_t *wire, const struct ionic_wire_hdr *h);

/* Read the IONIC_WIRE_HDR_LEN bytes at @wire into @h. */
void ionic_dp_wire_hdr_get(struct ionic_wire_hdr *h, const uint8_t *wire);

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
 * The high-order bit of a Q_Key, which makes it a controlled key. A send
 * whose work request carries one uses the sending QP's own Q_Key instead.
 */
#define DP_QKEY_CONTROLLED UINT32_C(0x80000000)

/*
 * Why a send from @sq may not deliver into @dq with Q_Key @sent_qkey, the
 * one its work request carries, or NULL when it may.
 */
const char *ionic_dp_ud_delivery_refused(const struct ionic_qp_ring *sq,
                                         const struct ionic_qp_ring *dq,
                                         uint32_t sent_qkey);

/* Directions for ionic_dp_rdma_bytes(); a local copy both sends and
 * receives. */
#define DP_RDMA_TX 1u
#define DP_RDMA_RX 2u

/*
 * Count @bytes of RDMA payload once for the server's statistics (by @op,
 * against @qp_id) and once per direction in @dirs for the guest's struct
 * ionic_lif_stats (tx/rx_rdma_ucast_*).  The two count differently, so
 * neither is derived from the other; see ernic_stats.h.
 */
void ionic_dp_rdma_bytes(const struct ionic_datapath *dp, uint32_t qp_id,
                         uint64_t bytes, enum ernic_stat_op op,
                         unsigned int dirs);

/* -------------------------------------------------------------------------
 * Shared with the targets' and the P4 pipeline's files
 *
 * Defined in ionic_datapath.c.
 * -------------------------------------------------------------------------
 */

/* Now, in milliseconds, on the data path's clock (@now_ms above). */
uint64_t ionic_dp_now_ms(const struct ionic_datapath *dp);

/*
 * Whether to log one more of the lines @lim, one of @dp's, limits
 * (ernic_log.h), on the data path's clock.  If any were held back, it logs
 * first how many @what there were.  On the vfu thread only.
 */
bool ionic_dp_log_ok(struct ionic_datapath *dp, ErnicLogLimit *lim,
                     const char *what);

/*
 * Does the MR named by @key cover [@va, @va + @len)?  Key 0 names a bus
 * address, so the range is only checked not to wrap.
 */
bool ionic_dp_mr_covers(struct ionic_datapath *dp, uint32_t key, uint64_t va,
                        uint64_t len);

/*
 * Copy up to @len bytes of the guest memory @l describes into @dst.  Returns
 * how many were copied.
 */
uint32_t ionic_dp_gather(struct ionic_datapath *dp, const struct dp_sge_list *l,
                         uint8_t *dst, uint32_t len);

/*
 * Move @len bytes from the guest region (@lkey, @va) to @dst, or from @src
 * to it.  Returns how many moved; when that is short, @fault says why.
 */
uint32_t ionic_dp_sge_to_host(struct ionic_datapath *dp, uint32_t lkey,
                              uint64_t va, uint8_t *dst, uint32_t len);
uint32_t ionic_dp_host_to_sge(struct ionic_datapath *dp, uint32_t lkey,
                              uint64_t va, const uint8_t *src, uint32_t len);

/*
 * An in-process target's DMA callbacks, for @ctx the struct ionic_datapath:
 * ionic_dp_sge_to_host() and ionic_dp_host_to_sge(), but refusing key 0,
 * which a guest process must not be able to name.
 */
uint32_t ionic_dp_target_from_host(void *ctx, uint32_t key, uint64_t addr,
                                   void *dst, uint32_t len);
uint32_t ionic_dp_target_to_host(void *ctx, uint32_t key, uint64_t addr,
                                 const void *src, uint32_t len);

/*
 * Build the header in front of a UD receive of @paylen bytes, from @sgid to
 * @dgid (either may be NULL): an IPv4 header if both are v4-mapped, a GRH
 * otherwise.
 */
void ionic_dp_grh_build(uint8_t grh[IB_GRH_SIZE], uint32_t paylen,
                        const uint8_t *sgid, const uint8_t *dgid);

/*
 * Deliver a SEND from @src_qp_id into the next receive posted to @dq
 * (@dst_qp_id), completing it with @recv_op and @imm_be.  @src names the
 * payload in guest memory, or, if @src_host is set, only its length, the
 * bytes being at @src_host.  Returns the bytes delivered, or -1 if no
 * receive was posted.
 */
int64_t ionic_dp_deliver_recv(struct ionic_datapath *dp,
                              struct ionic_qp_ring *dq, uint32_t dst_qp_id,
                              uint32_t src_qp_id, const struct dp_sge_list *src,
                              const uint8_t *src_host, uint8_t recv_op,
                              uint32_t imm_be);

/* -------------------------------------------------------------------------
 * The targets' and the P4 pipeline's hooks, which ionic_datapath.c calls
 * -------------------------------------------------------------------------
 */

/* ionic_datapath_nvmeof.c: tear down the NVMe-oF controller, if any. */
void ionic_dp_nvmeof_detach(struct ionic_datapath *dp);

/* Forget the controller connection of @qp_id, which is being destroyed. */
void ionic_dp_nvmeof_drop_qp(struct ionic_datapath *dp, uint32_t qp_id);

/*
 * A SEND @src on the GSI QP @q (@qp_id): if it is a CM MAD for the
 * controller, answer it and return true, so the caller delivers it nowhere
 * else.
 */
bool ionic_dp_nvmeof_gsi(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                         uint32_t qp_id, const struct dp_sge_list *src);

/*
 * A SEND @src on the connected QP @q (@qp_id): if its peer is a controller
 * queue, execute the command capsule and return true.
 */
bool ionic_dp_nvmeof_rc_send(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                             uint32_t qp_id, const struct dp_sge_list *src);

/* ionic_datapath_s3.c: tear down the S3 store, if any. */
void ionic_dp_s3_detach(struct ionic_datapath *dp);

/* Whether the S3 store's TCP stack has something to send right now. */
bool ionic_dp_s3_has_work(struct ionic_datapath *dp);

/* Push the S3 store's pending output: retransmissions and queued replies. */
void ionic_dp_s3_poll(struct ionic_datapath *dp);

#ifdef ERNIC_HAVE_P4
/*
 * ionic_datapath_p4.c: run the pipeline over the send WQE @wqe (@read_sz
 * bytes of it) of opcode @op on @qp_id, which gathers @src.  Returns the
 * completion status, and sets *@done when the ordinary dispatch must not
 * run: either because the program suppressed the operation, or because the
 * status is an error.
 */
uint32_t ionic_dp_p4_policy(struct ionic_datapath *dp, uint32_t qp_id,
                            uint8_t op, const uint8_t *wqe, uint32_t read_sz,
                            const struct dp_sge_list *src, uint32_t imm_be,
                            bool *done);
#endif

#endif /* IONIC_DATAPATH_PRIV_H */
