/*
 * Unit tests for the ionic emulator's MSI-X mask/pending logic.
 *
 * Every vector comes out of reset masked and the guest driver only unmasks
 * as it arms each handler during bring-up. An assertion raised in that
 * window used to be discarded with no pending state, so nothing replayed it
 * on unmask and the queue waited for an edge that never came again. These
 * tests pin the latch-and-replay behaviour on both unmask paths (a direct 0
 * to the mask register, and an INTR_CRED_UNMASK credit return) and check
 * that mask-on-assert still re-latches so a replayed interrupt behaves
 * exactly like a first-hand one.
 *
 * The per-vector state is private to the translation unit, so the TU is
 * #included and its external symbols are stubbed. vfu_irq_trigger() counts
 * deliveries per vector instead of touching a real vfio-user context, which
 * lets the whole interrupt path run in-process under AddressSanitizer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "ionic_eth_emu.c"

/* ---- Stubs for the TU's external symbols ------------------------------- */

/* Deliveries observed per vector, keyed by the vector number. */
static unsigned g_irqs[IONIC_MSIX_MAX_VECTORS];
/* Non-zero makes vfu_irq_trigger() fail, as a wedged socket would. */
static int g_irq_errno;

int vfu_irq_trigger(vfu_ctx_t *vfu_ctx, uint32_t subindex)
{
    (void)vfu_ctx;
    if (g_irq_errno)
        return g_irq_errno;
    if (subindex < IONIC_MSIX_MAX_VECTORS)
        g_irqs[subindex]++;
    return 0;
}

void vfu_log(vfu_ctx_t *vfu_ctx, int level, const char *fmt, ...)
{
    (void)vfu_ctx;
    (void)level;
    (void)fmt;
}

void pvrdma_irq_count(pvrdma_handle_t handle)
{
    (void)handle;
}
void pvrdma_eth_bytes_count(pvrdma_handle_t handle, uint64_t bytes, bool is_tx)
{
    (void)handle;
    (void)bytes;
    (void)is_tx;
}

/* The data path is never reached from the interrupt-control registers. */
void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val)
{
    (void)dp;
    (void)qtype;
    (void)doorbell_val;
}
void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx, int aq_idx,
                              uint16_t p_index)
{
    (void)ctx;
    (void)aq_idx;
    (void)p_index;
}

struct ionic_eth_net *ionic_eth_net_open_tap(const char *ifname,
                                             char *out_ifname,
                                             size_t out_ifname_len)
{
    (void)ifname;
    (void)out_ifname;
    (void)out_ifname_len;
    return NULL;
}
void ionic_eth_net_close(struct ionic_eth_net *net)
{
    (void)net;
}
int ionic_eth_net_send(struct ionic_eth_net *net, const void *frame, size_t len)
{
    (void)net;
    (void)frame;
    (void)len;
    return 0;
}
ssize_t ionic_eth_net_recv(struct ionic_eth_net *net, void *buf, size_t cap)
{
    (void)net;
    (void)buf;
    (void)cap;
    return 0;
}

size_t dma_sg_size(void)
{
    return 64;
}
int vfu_addr_to_sgl(vfu_ctx_t *vfu_ctx, vfu_dma_addr_t dma_addr, size_t len,
                    dma_sg_t *sgl, size_t max_nr_sgs, int prot)
{
    (void)vfu_ctx;
    (void)dma_addr;
    (void)len;
    (void)sgl;
    (void)max_nr_sgs;
    (void)prot;
    return -1;
}
int vfu_sgl_get(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                size_t cnt, int flags)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)iov;
    (void)cnt;
    (void)flags;
    return -1;
}
void vfu_sgl_put(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, struct iovec *iov,
                 size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)iov;
    (void)cnt;
}
void vfu_sgl_mark_dirty(vfu_ctx_t *vfu_ctx, dma_sg_t *sgl, size_t cnt)
{
    (void)vfu_ctx;
    (void)sgl;
    (void)cnt;
}

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL: %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            failures++;                                  \
        }                                                \
    } while (0)

/* Write one DWORD to a vector's interrupt-control register, the way the
 * guest driver's 32-bit MMIO store arrives at the BAR0 callback. */
static void intr_write(struct ionic_eth_emu *emu, int vec, unsigned reg_off,
                       uint32_t val)
{
    loff_t off = (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET +
                 (loff_t)vec * INTR_REG_STRIDE + (loff_t)reg_off;
    ssize_t ret = ionic_eth_emu_bar0_access(emu, (char *)&val, 4, off, true);

    CHECK(ret == 4, "bar0 write of vec %d reg %u returned %zd", vec, reg_off,
          ret);
}

static struct ionic_eth_emu *emu_new(void)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);

    memset(g_irqs, 0, sizeof(g_irqs));
    g_irq_errno = 0;
    return emu;
}

/* ---- Tests ------------------------------------------------------------- */

/* A vector is masked out of reset, so an assertion must latch rather than
 * vanish, and must be delivered exactly once when the driver unmasks. */
static void test_latch_replayed_on_mask_write(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 3;

    CHECK(emu->intr_mask[vec] == 1, "vector should start masked");

    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "masked trigger failed");
    CHECK(g_irqs[vec] == 0, "masked vector delivered %u irqs", g_irqs[vec]);
    CHECK(emu->intr_pending[vec] == 1, "assertion was not latched");

    intr_write(emu, vec, INTR_MASK_OFF, 0);
    CHECK(g_irqs[vec] == 1, "unmask delivered %u irqs, want 1", g_irqs[vec]);
    CHECK(emu->intr_pending[vec] == 0, "pending not cleared after replay");

    /* A second unmask with nothing latched must not invent an interrupt. */
    intr_write(emu, vec, INTR_MASK_OFF, 1);
    intr_write(emu, vec, INTR_MASK_OFF, 0);
    CHECK(g_irqs[vec] == 1, "spurious replay: %u irqs total", g_irqs[vec]);

    ionic_eth_emu_destroy(emu);
}

/* NAPI re-arms a vector by returning credits with IONIC_INTR_CRED_UNMASK
 * rather than writing the mask register, so that path must replay too. */
static void test_latch_replayed_on_credit_unmask(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 0;

    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "masked trigger failed");
    CHECK(g_irqs[vec] == 0, "masked vector delivered %u irqs", g_irqs[vec]);

    intr_write(emu, vec, INTR_CREDITS_OFF, INTR_CRED_UNMASK | 8);
    CHECK(g_irqs[vec] == 1, "credit unmask delivered %u irqs, want 1",
          g_irqs[vec]);
    CHECK(emu->intr_mask[vec] == 0, "credit unmask left the vector masked");

    /* Credits without the unmask bit must not re-arm or replay. */
    intr_write(emu, vec, INTR_MASK_OFF, 1);
    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "masked trigger failed");
    intr_write(emu, vec, INTR_CREDITS_OFF, 8);
    CHECK(g_irqs[vec] == 1, "plain credit return delivered an irq");
    CHECK(emu->intr_pending[vec] == 1, "plain credit return ate the latch");

    ionic_eth_emu_destroy(emu);
}

/* Mask-on-assert re-latches the mask as the interrupt goes out. The next
 * assertion must then latch, and replay once on the following unmask. */
static void test_mask_on_assert_round_trip(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 7;

    intr_write(emu, vec, INTR_MASK_ASSERT_OFF, 1);
    intr_write(emu, vec, INTR_MASK_OFF, 0);
    CHECK(g_irqs[vec] == 0, "unmask with nothing latched delivered an irq");

    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "trigger failed");
    CHECK(g_irqs[vec] == 1, "delivered %u irqs, want 1", g_irqs[vec]);
    CHECK(emu->intr_mask[vec] == 1, "mask-on-assert did not re-latch");

    /* Second assertion arrives while the driver is still polling. */
    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "trigger failed");
    CHECK(g_irqs[vec] == 1, "interrupt raced the poll: %u irqs", g_irqs[vec]);
    CHECK(emu->intr_pending[vec] == 1, "second assertion was not latched");

    /* NAPI completes and re-arms: the replay lands, and re-latches again. */
    intr_write(emu, vec, INTR_CREDITS_OFF, INTR_CRED_UNMASK);
    CHECK(g_irqs[vec] == 2, "replay delivered %u irqs, want 2", g_irqs[vec]);
    CHECK(emu->intr_mask[vec] == 1, "replay did not re-latch the mask");
    CHECK(emu->intr_pending[vec] == 0, "pending not cleared after replay");

    ionic_eth_emu_destroy(emu);
}

/* Several assertions while masked collapse to one delivery: the pending
 * state is a level, not a counter, which is what real MSI-X does. */
static void test_repeated_assertions_collapse(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 5;

    for (int i = 0; i < 4; i++)
        CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "trigger %d failed", i);
    CHECK(emu->intr_pending[vec] == 1, "pending should be a level");

    intr_write(emu, vec, INTR_MASK_OFF, 0);
    CHECK(g_irqs[vec] == 1, "delivered %u irqs, want 1", g_irqs[vec]);

    ionic_eth_emu_destroy(emu);
}

/* An unmasked vector delivers straight through and latches nothing. */
static void test_unmasked_delivers_directly(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 2;

    intr_write(emu, vec, INTR_MASK_OFF, 0);
    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == 0, "trigger failed");
    CHECK(g_irqs[vec] == 1, "delivered %u irqs, want 1", g_irqs[vec]);
    CHECK(emu->intr_pending[vec] == 0, "unmasked delivery left a latch");
    CHECK(emu->intr_mask[vec] == 0, "delivery masked a vector without "
                                    "mask-on-assert");

    ionic_eth_emu_destroy(emu);
}

/* Out-of-range vectors are rejected and touch no per-vector state. */
static void test_vector_bounds(void)
{
    struct ionic_eth_emu *emu = emu_new();

    CHECK(ionic_eth_emu_trigger_irq(emu, -1) == -EINVAL, "vec -1 not rejected");
    CHECK(ionic_eth_emu_trigger_irq(emu, IONIC_MSIX_MAX_VECTORS) == -EINVAL,
          "vec %d not rejected", IONIC_MSIX_MAX_VECTORS);

    for (int i = 0; i < IONIC_MSIX_MAX_VECTORS; i++)
        CHECK(emu->intr_pending[i] == 0, "vector %d latched by a bad trigger",
              i);

    ionic_eth_emu_destroy(emu);
}

/* A failed delivery still consumed the latch, so the caller sees the error
 * rather than the interrupt silently going missing. */
static void test_failed_delivery_reports_error(void)
{
    struct ionic_eth_emu *emu = emu_new();
    const int vec = 1;

    intr_write(emu, vec, INTR_MASK_OFF, 0);
    g_irq_errno = -EPIPE;
    CHECK(ionic_eth_emu_trigger_irq(emu, vec) == -EPIPE,
          "trigger did not report the delivery failure");

    ionic_eth_emu_destroy(emu);
}

int main(void)
{
    test_latch_replayed_on_mask_write();
    test_latch_replayed_on_credit_unmask();
    test_mask_on_assert_round_trip();
    test_repeated_assertions_collapse();
    test_unmasked_delivers_directly();
    test_vector_bounds();
    test_failed_delivery_reports_error();

    if (failures) {
        printf("ionic_intr_pending: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("ionic_intr_pending: all checks passed\n");
    return 0;
}
