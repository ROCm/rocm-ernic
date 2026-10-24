/*
 * Unit tests for the ionic emulator's CQ arm semantics.
 *
 * Arming a CQ has to behave as a level and not an edge. The driver's
 * ib_req_notify_cq() reports no missed events, so it never re-polls on its
 * own; if a completion lands while the CQ is disarmed and the following arm
 * does not re-raise the event, that completion is stranded and whoever is
 * waiting on it waits forever.
 *
 * That is reachable whenever the guest posts more work from inside its own
 * completion handler. nvme-rdma does exactly that: it answers a response
 * capsule with a LOCAL_INV and does not retire the request until the LOCAL_INV
 * completes. The emulator ran that WQE synchronously, wrote its CQE with the
 * CQ already disarmed, and then never fired again -- so an NVMe-oF Connect
 * hung for the full 60s admin timeout even though the controller had answered
 * it correctly.
 *
 * The CQ state is private to ionic_datapath.c, so the test reaches it through
 * ionic_datapath_priv.h, and the event callback is counted in-process instead
 * of reaching a real vfio-user context.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_datapath.h"
#include "ionic_datapath_priv.h"
#include "ionic_eth_emu.h"
#include "nvmeof_cm.h"
#include "nvmeof_target.h"
#include "rocm_ernic_compat.h"
#include "s3_tcp.h"
#include "s3_target.h"

/* ---- Stubs for the TU's external symbols -------------------------------
 *
 * None of these are reachable from the CQ doorbell path; they exist only to
 * satisfy the linker. libvfio-user itself is linked for vfu_log/vfu_sgl_*.
 */

/*
 * The real vfu_log() asserts on a NULL context, and the doorbell path logs on
 * every call, so this one has to be overridden rather than linked.
 */
void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)level;
    (void)fmt;
}

int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec)
{
    (void)emu;
    (void)vec;
    return 0;
}

/* What ionic_dp_rdma_bytes() handed the eth emulator, for the LIF block. */
static uint64_t g_lif_rdma_tx, g_lif_rdma_rx;

void ionic_eth_emu_count_rdma(struct ionic_eth_emu *emu, uint64_t bytes,
                              bool is_tx)
{
    (void)emu;
    if (is_tx)
        g_lif_rdma_tx += bytes;
    else
        g_lif_rdma_rx += bytes;
}

uint32_t ionic_mesh_local_node(pvrdma_handle_t handle)
{
    (void)handle;
    return UINT32_MAX;
}
uint32_t ionic_mesh_node_from_gid(pvrdma_handle_t handle, const uint8_t *gid)
{
    (void)handle;
    (void)gid;
    return UINT32_MAX;
}
int ionic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     bool reply)
{
    (void)handle;
    (void)reply;
    (void)dst_node;
    (void)hdr;
    (void)hdr_len;
    (void)body;
    (void)body_len;
    return -1;
}
bool ionic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len)
{
    (void)handle;
    (void)dst_node;
    (void)len;
    return true;
}
void ionic_mesh_set_recv_cb(pvrdma_handle_t handle, ionic_mesh_recv_fn fn,
                            void *opaque)
{
    (void)handle;
    (void)fn;
    (void)opaque;
}

struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
void nvmeof_target_destroy(struct nvmeof_target *t)
{
    (void)t;
}
const struct nvmeof_target_cfg *nvmeof_target_config(
    const struct nvmeof_target *t)
{
    (void)t;
    return NULL;
}
struct nvmeof_queue *nvmeof_target_find_queue(struct nvmeof_target *t,
                                              uint32_t handle)
{
    (void)t;
    (void)handle;
    return NULL;
}
int nvmeof_queue_exec(struct nvmeof_queue *q, const void *capsule, size_t len,
                      const struct nvmeof_dma_ops *dma, void *dma_ctx,
                      void *rsp)
{
    (void)q;
    (void)capsule;
    (void)len;
    (void)dma;
    (void)dma_ctx;
    (void)rsp;
    return -1;
}
struct nvmeof_cm *nvmeof_cm_create(struct nvmeof_target *target,
                                   uint32_t traddr, uint16_t trsvcid)
{
    (void)target;
    (void)traddr;
    (void)trsvcid;
    return NULL;
}
void nvmeof_cm_destroy(struct nvmeof_cm *cm)
{
    (void)cm;
}
void nvmeof_cm_drop_qp(struct nvmeof_cm *cm, uint32_t guest_qpn)
{
    (void)cm;
    (void)guest_qpn;
}
bool nvmeof_cm_handle_mad(struct nvmeof_cm *cm, const void *mad, size_t len,
                          void *rsp, struct nvmeof_cm_result *res)
{
    (void)cm;
    (void)mad;
    (void)len;
    (void)rsp;
    (void)res;
    return false;
}

struct s3_target *s3_target_create(const struct s3_target_cfg *cfg, char *err,
                                   size_t errlen)
{
    (void)cfg;
    (void)err;
    (void)errlen;
    return NULL;
}
void s3_target_destroy(struct s3_target *t)
{
    (void)t;
}
void s3_tcp_cfg_from_target(struct s3_tcp_cfg *cfg,
                            const struct s3_target *target)
{
    (void)cfg;
    (void)target;
}
struct s3_tcp *s3_tcp_create(const struct s3_tcp_cfg *cfg,
                             struct s3_target *target,
                             const struct s3_dma_ops *dma, void *dma_ctx,
                             s3_tcp_tx_fn tx, void *tx_ctx, char *err,
                             size_t errlen)
{
    (void)cfg;
    (void)target;
    (void)dma;
    (void)dma_ctx;
    (void)tx;
    (void)tx_ctx;
    (void)err;
    (void)errlen;
    return NULL;
}
void s3_tcp_destroy(struct s3_tcp *s)
{
    (void)s;
}
bool s3_tcp_rx_frame(struct s3_tcp *s, const void *frame, size_t len,
                     uint64_t now_ms)
{
    (void)s;
    (void)frame;
    (void)len;
    (void)now_ms;
    return false;
}
bool s3_tcp_poll(struct s3_tcp *s, uint64_t now_ms)
{
    (void)s;
    (void)now_ms;
    return false;
}
bool s3_tcp_has_work(const struct s3_tcp *s)
{
    (void)s;
    return false;
}

void ionic_eth_emu_register_tx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_tx_filter_fn fn, void *ctx)
{
    (void)emu;
    (void)fn;
    (void)ctx;
}
int ionic_eth_emu_queue_rx_frame(struct ionic_eth_emu *emu, const void *frame,
                                 size_t len)
{
    (void)emu;
    (void)frame;
    (void)len;
    return -1;
}

void pvrdma_qp_cqe_count(pvrdma_handle_t handle, uint32_t qp_id)
{
    (void)handle;
    (void)qp_id;
}
void pvrdma_qp_doorbell_count(pvrdma_handle_t handle, uint32_t qp_id,
                              bool is_send)
{
    (void)handle;
    (void)qp_id;
    (void)is_send;
}
void pvrdma_qp_wqe_count(pvrdma_handle_t handle, uint32_t qp_id,
                         unsigned int pvrdma_opcode)
{
    (void)handle;
    (void)qp_id;
    (void)pvrdma_opcode;
}
static uint64_t g_file_rdma_bytes; /* what reached the stats file */

void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op stat_op)
{
    (void)handle;
    (void)qp_id;
    (void)stat_op;
    g_file_rdma_bytes += bytes;
}

/* ---- Harness ----------------------------------------------------------- */

static unsigned g_events; /* CQ events delivered to the "driver" */

static void count_event(void *opaque, uint32_t eq_id, uint32_t cq_id)
{
    (void)opaque;
    (void)eq_id;
    (void)cq_id;
    g_events++;
}

#define CQ_ID    5
#define CQ_DEPTH 64

static struct ionic_cq_ring g_cq_storage[CQ_ID + 1];
static struct ionic_datapath g_dp;

/*
 * A datapath with nothing but one valid, armed CQ. The arm path never touches
 * guest memory, so none of the DMA machinery needs standing up.
 */
static struct ionic_datapath *dp_with_cq(uint32_t prod, uint32_t cons,
                                         bool armed)
{
    memset(&g_dp, 0, sizeof(g_dp));
    memset(g_cq_storage, 0, sizeof(g_cq_storage));

    g_dp.cq = g_cq_storage;
    g_dp.cq_count = CQ_ID + 1;
    g_dp.cq_event_fn = count_event;
    g_dp.cq_event_opaque = NULL;

    struct ionic_cq_ring *c = &g_dp.cq[CQ_ID];
    c->valid = true;
    c->depth = CQ_DEPTH;
    c->stride_log2 = 5;
    c->prod = prod;
    c->cons = cons;
    c->color = true;
    c->armed = armed;
    c->eq_id = 2;

    g_events = 0;
    return &g_dp;
}

static int g_failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        g_failures++;
}

/* Doorbell rings, as the driver uses them. */
#define RING_CONS          0
#define RING_ARM_ANY       1
#define RING_ARM_SOLICITED 2

/* The doorbell is one packed 64-bit write; see ionic_datapath_doorbell(). */
static uint64_t db(uint32_t qid, uint8_t ring, uint16_t p_index)
{
    return (uint64_t)p_index | ((uint64_t)ring << 16) |
           ((uint64_t)(qid & 0xffu) << 24) |
           ((uint64_t)((qid >> 8) & 0xffffu) << 32);
}

static void doorbell(struct ionic_datapath *dp, uint32_t qid, uint8_t ring,
                     uint16_t p_index)
{
    ionic_datapath_doorbell(dp, DP_QTYPE_CQ, db(qid, ring, p_index));
}

/* ---- Tests ------------------------------------------------------------- */

/*
 * The regression. Two CQEs were posted, the guest reaped only one of them
 * while disarmed, and now it arms. The ring is non-empty, so the event has to
 * fire immediately -- nothing else is ever going to write to this CQ.
 */
static void test_arm_fires_when_unreaped(void)
{
    struct ionic_datapath *dp = dp_with_cq(2, 0, false);

    doorbell(dp, CQ_ID, RING_CONS, 1);
    check(dp->cq[CQ_ID].cons == 1, "cons doorbell records the consumer index");

    doorbell(dp, CQ_ID, RING_ARM_ANY, 0);
    check(g_events == 1, "arm over a non-empty ring raises exactly one event");
    check(!dp->cq[CQ_ID].armed, "firing on arm consumes the arm");
}

/* A fully drained ring must stay quiet, or every arm becomes a spurious IRQ. */
static void test_arm_quiet_when_drained(void)
{
    struct ionic_datapath *dp = dp_with_cq(2, 0, false);

    doorbell(dp, CQ_ID, RING_CONS, 2);
    doorbell(dp, CQ_ID, RING_ARM_ANY, 0);

    check(g_events == 0, "arm over a drained ring raises no event");
    check(dp->cq[CQ_ID].armed, "the arm stays pending for the next CQE");
}

/* Arm-solicited takes the same path; it must not be the one that regresses. */
static void test_arm_solicited_fires(void)
{
    struct ionic_datapath *dp = dp_with_cq(1, 0, false);

    doorbell(dp, CQ_ID, RING_ARM_SOLICITED, 0);
    check(g_events == 1, "arm-solicited over a non-empty ring raises an event");
}

/*
 * The arm doorbell carries the driver's arm_any_prod, not a consumer index.
 * Letting it move cons would mark a non-empty ring drained and reintroduce the
 * lost wakeup by a different route.
 */
static void test_arm_does_not_move_cons(void)
{
    struct ionic_datapath *dp = dp_with_cq(4, 0, false);

    doorbell(dp, CQ_ID, RING_ARM_ANY, 4);
    check(dp->cq[CQ_ID].cons == 0, "an arm doorbell leaves cons alone");
    check(g_events == 1, "and still sees the ring as non-empty");
}

/* The consumer index wraps at the ring depth; prod is free-running. */
static void test_cons_wraps(void)
{
    struct ionic_datapath *dp = dp_with_cq(CQ_DEPTH + 3, 0, false);

    doorbell(dp, CQ_ID, RING_CONS, CQ_DEPTH + 3);
    check(dp->cq[CQ_ID].cons == 3, "cons wraps at the ring depth");

    doorbell(dp, CQ_ID, RING_ARM_ANY, 0);
    check(g_events == 0, "a wrapped, drained ring is still drained");
}

/* An unarmed CQ must not deliver, and an invalid one must not be touched. */
static void test_no_event_without_arm(void)
{
    struct ionic_datapath *dp = dp_with_cq(2, 0, false);

    doorbell(dp, CQ_ID, RING_CONS, 0);
    check(g_events == 0, "a cons doorbell alone never raises an event");

    dp->cq[CQ_ID].valid = false;
    doorbell(dp, CQ_ID, RING_ARM_ANY, 0);
    check(g_events == 0, "an invalid CQ raises nothing");

    doorbell(dp, CQ_ID + 1, RING_ARM_ANY, 0);
    check(g_events == 0, "an out-of-range cq_id raises nothing");
}

/*
 * ionic_dp_rdma_bytes() feeds both counters: the stats file once per message,
 * the guest's LIF block once per direction.
 */
static void test_rdma_bytes_directions(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint64_t bytes;
        unsigned int dirs;
        uint64_t want_file, want_tx, want_rx;
    } rows[] = {
        {"positive", "a SEND or WRITE leaving the NIC",
         "file 4096, lif tx 4096, lif rx 0", 4096, DP_RDMA_TX, 4096, 4096, 0},
        {"positive", "a RECV or WRITE landing in guest memory",
         "file 4096, lif tx 0, lif rx 4096", 4096, DP_RDMA_RX, 4096, 0, 4096},
        {"corner", "a local copy between two QPs on this NIC",
         "file 4096 (once), lif tx 4096, lif rx 4096", 4096,
         DP_RDMA_TX | DP_RDMA_RX, 4096, 4096, 4096},
        {"boundary", "a zero-length message", "file 0, lif tx 0, lif rx 0", 0,
         DP_RDMA_TX | DP_RDMA_RX, 0, 0, 0},
        {"boundary", "the largest RDMA message (2^31 bytes)",
         "file 2147483648, lif tx 2147483648, lif rx 0", 1ull << 31, DP_RDMA_TX,
         1ull << 31, 1ull << 31, 0},
    };
    struct ionic_datapath dp;

    memset(&dp, 0, sizeof(dp));
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char what[256];
        bool ok;

        g_file_rdma_bytes = g_lif_rdma_tx = g_lif_rdma_rx = 0;
        ionic_dp_rdma_bytes(&dp, 1, rows[i].bytes, PVRDMA_STAT_SEND,
                            rows[i].dirs);
        ok = g_file_rdma_bytes == rows[i].want_file &&
             g_lif_rdma_tx == rows[i].want_tx &&
             g_lif_rdma_rx == rows[i].want_rx;
        snprintf(what, sizeof(what),
                 "%s: %s; expected: %s; got: file %" PRIu64 ", lif tx %" PRIu64
                 ", lif rx %" PRIu64,
                 rows[i].category, rows[i].description, rows[i].expected,
                 g_file_rdma_bytes, g_lif_rdma_tx, g_lif_rdma_rx);
        check(ok, what);
    }
}

int main(void)
{
    test_arm_fires_when_unreaped();
    test_arm_quiet_when_drained();
    test_arm_solicited_fires();
    test_arm_does_not_move_cons();
    test_cons_wraps();
    test_no_event_without_arm();
    test_rdma_bytes_directions();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
