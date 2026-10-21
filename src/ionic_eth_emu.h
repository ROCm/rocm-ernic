/*
 * ionic_eth_emu.h — ionic Ethernet admin protocol emulator interface
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ETH_EMU_H
#define IONIC_ETH_EMU_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
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

/*
 * Give the device the identity and link of @profile (ionic_profile.h)
 * instead of compat.  Call before any client connects: the guest reads the
 * identity once, at probe.
 */
struct ionic_profile;
void ionic_eth_emu_set_profile(struct ionic_eth_emu *emu,
                               const struct ionic_profile *profile);

/*
 * Inject the eth_rx and eth_tx faults of @faults (ionic_fault.h), or none
 * for NULL.  The set is the caller's and must outlive the device.  Call
 * before any client connects.
 */
struct ionic_fault_set;
void ionic_eth_emu_set_faults(struct ionic_eth_emu *emu,
                              const struct ionic_fault_set *faults);

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

/*
 * Set the pvrdma handle so the emulator can record interrupts and Ethernet
 * bytes into the shared statistics block.
 * @handle: pvrdma_handle_t (void *) from pvrdma_device_create().
 */
void ionic_eth_emu_set_pvrdma(struct ionic_eth_emu *emu, void *handle);

/* Register the datapath handler for doorbell writes (BAR2). */
struct ionic_datapath;
void ionic_eth_emu_register_datapath(struct ionic_eth_emu *emu,
                                     struct ionic_datapath *dp);

/* Register the admin queue context for AQ doorbell producer-index updates. */
struct ionic_adminq_ctx;
void ionic_eth_emu_register_adminq(struct ionic_eth_emu *emu,
                                   struct ionic_adminq_ctx *adminq);

/*
 * Set the station MAC the LIF reports to the guest.  Two instances bridged
 * together must not share one, so the launcher derives it from the instance
 * id; without this every guest on the bridge answers to the same address.
 */
void ionic_eth_emu_set_mac(struct ionic_eth_emu *emu, const uint8_t mac[6]);

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
 * Give an in-process endpoint first refusal on every frame the guest
 * transmits.
 *
 * The filter returns true when the frame was addressed to it and has been
 * consumed, in which case the frame is not forwarded to the host backend.
 * This is what lets a service such as the S3 control plane sit on the
 * emulated wire at its own IP address without a TAP interface, and what
 * lets it keep working when there is one.
 */
typedef bool (*ionic_eth_tx_filter_fn)(void *ctx, const void *frame,
                                       size_t len);
void ionic_eth_emu_register_tx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_tx_filter_fn fn, void *ctx);

/*
 * Read back the registered Tx filter, so a second endpoint can take the slot
 * and chain to the first rather than displace it.  Either output may be
 * NULL; both are cleared when no filter is registered.
 */
void ionic_eth_emu_get_tx_filter(struct ionic_eth_emu *emu,
                                 ionic_eth_tx_filter_fn *fn, void **ctx);

/*
 * The mirror image: give an in-process endpoint first refusal on every frame
 * the host backend receives, before it reaches the guest's Rx ring.
 *
 * Same contract as the Tx filter -- true means the frame was consumed and
 * must not be delivered to the guest.  A filter registered on both directions
 * sees the whole emulated wire, which is what a programmable pipeline between
 * the guest and the host network needs.  Frames queued by
 * ionic_eth_emu_queue_rx_frame() do not pass through here: they come from
 * in-process producers that have already decided where the frame goes.
 */
typedef bool (*ionic_eth_rx_filter_fn)(void *ctx, const void *frame,
                                       size_t len);
void ionic_eth_emu_register_rx_filter(struct ionic_eth_emu *emu,
                                      ionic_eth_rx_filter_fn fn, void *ctx);

/*
 * Hand a received frame to the emulated LIF from a thread that may not DMA
 * (the TCP mesh receive thread).  The frame is copied onto an internal queue
 * and delivered to the guest by the next ionic_eth_emu_poll_rx().
 *
 * Returns 0 on success, -ENOSPC when the queue is full, or -EINVAL.
 */
int ionic_eth_emu_queue_rx_frame(struct ionic_eth_emu *emu, const void *frame,
                                 size_t len);

/*
 * Rewrite struct ionic_lif_stats in the guest's LIF info block if
 * stats.interval_ms has passed since the last write, by @now_ms (any
 * monotonic millisecond clock).  The first call after LIF_INIT writes at
 * once; nothing is written before LIF_INIT or after LIF_RESET or a device
 * reset.  Main loop thread only.
 */
void ionic_eth_emu_publish_stats(struct ionic_eth_emu *emu, uint64_t now_ms);

struct ionic_emu_stats;

/*
 * The counters behind the published block, with the off-thread ones folded
 * in, for the --stats-file writer.  Main loop thread only.
 */
const struct ionic_emu_stats *ionic_eth_emu_get_stats(
    struct ionic_eth_emu *emu);

/*
 * Write the "LIF Statistics" section of the --stats-file: every named
 * counter of struct ionic_lif_stats, by its ionic_if.h name, zero or not, so
 * the set of lines never changes.  @opaque is the struct ionic_eth_emu; the
 * signature matches pvrdma_set_stats_section().  Main loop thread only.
 */
void ionic_eth_emu_write_stats(FILE *fp, void *opaque);

/* Count one RDMA message of @bytes in the LIF's tx/rx_rdma_ucast_* stats. */
void ionic_eth_emu_count_rdma(struct ionic_eth_emu *emu, uint64_t bytes,
                              bool is_tx);
 * Send a frame out of the host network backend.
 *
 * The Tx path reaches the backend by itself; this is for an in-process
 * endpoint that claimed a frame through a filter and has to put it back on
 * the wire, possibly rewritten.  Returns 0, -ENODEV when no backend is
 * attached, or a negative errno from the backend.
 */
int ionic_eth_emu_send_to_net(struct ionic_eth_emu *emu, const void *frame,
                              size_t len);

/*
 * Move any frames waiting on the host backend or on the off-thread queue
 * into the guest's Rx ring.  Must be called from the thread that owns the
 * vfio-user context; the server's main loop does this on every iteration.
 */
void ionic_eth_emu_poll_rx(struct ionic_eth_emu *emu);

#endif /* IONIC_ETH_EMU_H */
