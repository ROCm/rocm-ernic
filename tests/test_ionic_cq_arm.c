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
 * The CQ state is private to the translation unit, so the TU is #included and
 * the event callback is counted in-process instead of reaching a real
 * vfio-user context.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "ionic_datapath.c"

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
                     size_t hdr_len, const void *body, size_t body_len)
{
    (void)handle;
    (void)dst_node;
    (void)hdr;
    (void)hdr_len;
    (void)body;
    (void)body_len;
    return -1;
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
void pvrdma_rdma_bytes_count(pvrdma_handle_t handle, uint32_t qp_id,
                             uint64_t bytes, enum pvrdma_stat_op op)
{
    (void)handle;
    (void)qp_id;
    (void)bytes;
    (void)op;
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

int main(void)
{
    test_arm_fires_when_unreaped();
    test_arm_quiet_when_drained();
    test_arm_solicited_fires();
    test_arm_does_not_move_cons();
    test_cons_wraps();
    test_no_event_without_arm();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
