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

/* Callback type for triggering a numbered MSI-X interrupt. */
typedef int (*ionic_irq_trigger_fn_t)(void *opaque, int vec);

struct ionic_rdma_devcmd_state *ionic_rdma_devcmd_create(vfu_ctx_t *vfu_ctx);

void ionic_rdma_devcmd_destroy(struct ionic_rdma_devcmd_state *s);

/* Main dispatch — registered with ionic_eth_emu as the RDMA handler. */
void ionic_rdma_devcmd_dispatch(void *opaque, const uint8_t *cmd,
                                uint8_t *comp);

/* Trigger EQ interrupt (called by adminq layer on completion). */
int ionic_rdma_devcmd_trigger_eq(struct ionic_rdma_devcmd_state *s, int eq_idx,
                                 ionic_irq_trigger_fn_t trigger_fn,
                                 void *trigger_opaque);

/* Return the admin queue context (may be NULL until CREATE_ADMINQ). */
struct ionic_adminq_ctx *ionic_rdma_devcmd_get_adminq_ctx(
    struct ionic_rdma_devcmd_state *s);

#endif /* IONIC_RDMA_DEVCMD_H */
