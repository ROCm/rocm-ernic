/*
 * ionic_rdma_devcmd.h — ionic RDMA devcmd handler (opcodes 50-53)
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_RDMA_DEVCMD_H
#define IONIC_RDMA_DEVCMD_H

#include <stdint.h>
#include <vfio-user/libvfio-user.h>

struct ionic_rdma_devcmd_state;
struct ionic_eth_emu;

/* Callback type for triggering a numbered MSI-X interrupt. */
typedef int (*ionic_irq_trigger_fn_t)(void *opaque, int vec);

struct ionic_rdma_devcmd_state *ionic_rdma_devcmd_create(vfu_ctx_t *vfu_ctx);

void ionic_rdma_devcmd_destroy(struct ionic_rdma_devcmd_state *s);

/* Main dispatch — registered with ionic_eth_emu as the RDMA handler. */
void ionic_rdma_devcmd_dispatch(void *opaque, const uint8_t *cmd,
                                uint8_t *comp);

/* Supply the emulator used to raise EQ MSI-X vectors. */
void ionic_rdma_devcmd_set_eth_emu(struct ionic_rdma_devcmd_state *s,
                                   struct ionic_eth_emu *eth_emu);

/* Route data-path CQ notifications through the same EQ machinery. */
struct ionic_datapath;
void ionic_rdma_devcmd_set_datapath(struct ionic_rdma_devcmd_state *s,
                                    struct ionic_datapath *dp);

/*
 * Post an ionic_v1_eqe announcing that @cq_id has completions, then raise the
 * MSI-X vector behind @eq_id.  Without this the driver never runs
 * ionic_poll_eq() and every admin command it posts times out.
 */
int ionic_rdma_devcmd_post_cq_event(struct ionic_rdma_devcmd_state *s,
                                    uint32_t eq_id, uint32_t cq_id);

/* Return the admin queue context (may be NULL until CREATE_ADMINQ). */
struct ionic_adminq_ctx *ionic_rdma_devcmd_get_adminq_ctx(
    struct ionic_rdma_devcmd_state *s);

#endif /* IONIC_RDMA_DEVCMD_H */
