/*
 * ionic_eth_emu.h — ionic Ethernet admin protocol emulator interface
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ETH_EMU_H
#define IONIC_ETH_EMU_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#include <vfio-user/libvfio-user.h>

/* Opaque emulator state. */
struct ionic_eth_emu;

/*
 * Callback type for RDMA devcmds (opcodes 50-53) forwarded from the
 * Ethernet admin queue to the RDMA emulation layer.
 *
 * @opaque: caller-supplied pointer (set via
 * ionic_eth_emu_register_rdma_handler)
 * @cmd:    64-byte command buffer (opcode in cmd[0])
 * @comp:   16-byte completion buffer to fill on return
 */
typedef void (*ionic_rdma_devcmd_fn_t)(void *opaque, const uint8_t *cmd,
                                       uint8_t *comp);

/* Create / destroy */
struct ionic_eth_emu *ionic_eth_emu_create(vfu_ctx_t *vfu_ctx,
                                           size_t bar2_size);
void ionic_eth_emu_destroy(struct ionic_eth_emu *emu);

/* Register the RDMA devcmd handler (called before any client connects). */
void ionic_eth_emu_register_rdma_handler(struct ionic_eth_emu *emu,
                                         ionic_rdma_devcmd_fn_t fn,
                                         void *opaque);

/* BAR access callbacks — wire these up in rocm_ernic_server.c. */
ssize_t ionic_eth_emu_bar0_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write);

ssize_t ionic_eth_emu_bar2_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write);

/* Trigger an MSI-X vector (0 = success, -EINVAL = bad vec, 0 if masked). */
int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec);

/* Register the datapath handler for doorbell writes (BAR2). */
struct ionic_datapath;
void ionic_eth_emu_register_datapath(struct ionic_eth_emu *emu,
                                     struct ionic_datapath *dp);

/* Register the admin queue context for AQ doorbell producer-index updates. */
struct ionic_adminq_ctx;
void ionic_eth_emu_register_adminq(struct ionic_eth_emu *emu,
                                   struct ionic_adminq_ctx *adminq);

/*
 * Attach the emulated LIF to a host TAP interface, giving the guest a real
 * Ethernet segment.  Without this Tx is a sink and nothing is ever received.
 * @ifname may be NULL or empty to let the kernel pick a name; the name that
 * was actually assigned is copied into @out_ifname.
 *
 * Returns 0 on success, or a negative errno.
 */
int ionic_eth_emu_attach_tap(struct ionic_eth_emu *emu, const char *ifname,
                             char *out_ifname, size_t out_ifname_len);

/*
 * Move any frames waiting on the host backend into the guest's Rx ring.
 * Must be called from the thread that owns the vfio-user context; the
 * server's main loop does this on every iteration.
 */
void ionic_eth_emu_poll_rx(struct ionic_eth_emu *emu);

#endif /* IONIC_ETH_EMU_H */
