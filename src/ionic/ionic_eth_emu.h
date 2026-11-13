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

#include "net/net_headers.h"

/* Opaque emulator state. */
struct ionic_eth_emu;
struct ernic_stats;

/*
 * A devcmd's command and completion (union ionic_dev_cmd and union
 * ionic_dev_cmd_comp in ionic_if.h).  They are the same size whether the
 * command came through the BAR0 devcmd window or the Ethernet admin queue.
 */
#define IONIC_DEVCMD_CMD_LEN  64U
#define IONIC_DEVCMD_COMP_LEN 16U

/*
 * Callback type for a devcmd this layer does not implement itself and
 * forwards to the module that does: the RDMA opcodes
 * (IONIC_CMD_RDMA_*, ionic_rdma_devcmd.h) and the P4 opcodes
 * (IONIC_CMD_P4_*, p4/p4_devcmd.h).
 *
 * @opaque: caller-supplied pointer (set via the matching register call)
 * @cmd:    IONIC_DEVCMD_CMD_LEN-byte command buffer (opcode in cmd[0])
 * @comp:   IONIC_DEVCMD_COMP_LEN-byte completion buffer to fill on return
 */
typedef void (*ionic_rdma_devcmd_fn_t)(void *opaque, const uint8_t *cmd,
                                       uint8_t *comp);

/* Create / destroy */
struct ionic_eth_emu *ionic_eth_emu_create(vfu_ctx_t *vfu_ctx,
                                           size_t bar2_size);
void ionic_eth_emu_destroy(struct ionic_eth_emu *emu);

/*
 * Return the guest's view of the device to what it was before any client
 * attached, for a device reset: the BAR0 registers, BAR2 doorbells and
 * BAR4 CMB, every interrupt masked, no Ethernet queue, no LIF or port info
 * block, the station MAC, zeroed counters, and no frame queued for the
 * guest.
 * What the operator configured stays: the profile, faults, network backend
 * and filters, and the registered handlers.  Nothing is written to guest
 * memory.  @emu may be NULL.  Main loop thread only.
 */
void ionic_eth_emu_reset(struct ionic_eth_emu *emu);

/*
 * Register what IONIC_CMD_RESET runs for the rest of the device, before it
 * resets the emulator's own state: the data path and the RDMA queues, which
 * the emulator doesn't own.  NULL resets only the emulator.  Called before
 * any client connects.
 */
typedef void (*ionic_eth_emu_reset_fn_t)(void *opaque);
void ionic_eth_emu_register_reset_handler(struct ionic_eth_emu *emu,
                                          ionic_eth_emu_reset_fn_t fn,
                                          void *opaque);

/*
 * Do the work held while Bus Master Enable was clear, now that it is set:
 * the port info and LIF status that PORT_INIT and LIF_INIT publish, the
 * Ethernet admin and Tx queues up to their last doorbells, and the
 * interrupts that asserted on unmasked vectors.  Main loop thread only;
 * call it once BME is set again.
 */
void ionic_eth_emu_resume(struct ionic_eth_emu *emu);

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

/*
 * Register the P4 devcmd handler (opcodes 70-72), likewise before any
 * client connects.  A separate slot from the RDMA one rather than a
 * shared dispatcher: there is exactly one of each, and a single slot
 * registered twice would silently drop whichever registered first.
 *
 * Leaving it unset is the ordinary case -- a build without P4, or one
 * where the operator granted the guest nothing -- and the opcodes then
 * answer as any other unimplemented opcode does.
 */
void ionic_eth_emu_register_p4_handler(struct ionic_eth_emu *emu,
                                       ionic_rdma_devcmd_fn_t fn, void *opaque);

/* BAR access callbacks — wire these up in rocm_ernic_server.c. */
ssize_t ionic_eth_emu_bar0_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write);

ssize_t ionic_eth_emu_bar2_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write);

/*
 * An access to the CMB, BAR4, by a client that hasn't mapped it (see
 * ionic_eth_emu_cmb_fd()): a read or write of its memory, and nothing more.
 * A write posts no work, any more than a store through the mapping does:
 * the guest rings a doorbell for that.  The first access after the device
 * is created or the CMB renewed is logged, since a guest whose stores into
 * BAR4 exit to QEMU runs slowly, and a guest program storing there with an
 * instruction KVM can't emulate, such as MOVDIR64B, gets SIGILL.  Refuses
 * every access, with EINVAL, unless the device has a CMB.
 */
ssize_t ionic_eth_emu_bar4_access(struct ionic_eth_emu *emu, char *buf,
                                  size_t count, loff_t offset, bool is_write);

/*
 * Give the device a Controller Memory Buffer (CMB), BAR4, which it doesn't
 * have by default.  Without one, DISCOVER_CMB is refused, and BAR4 accesses
 * fail; the server doesn't set up BAR4 either, so the guest driver finds no
 * CMB and puts every ring in guest memory.  The CMB is shared memory (see
 * ernic_shm.h) that the client maps into the guest.  Call before the device
 * is used.  Returns 0, or -1 with errno set if the CMB can't be made.
 */
int ionic_eth_emu_enable_cmb(struct ionic_eth_emu *emu);

/*
 * The descriptor of the file behind the CMB, for libvfio-user to hand the
 * client so that it can map BAR4 into the guest, or -1 if the device has no
 * CMB.
 */
int ionic_eth_emu_cmb_fd(const struct ionic_eth_emu *emu);

/*
 * Give the CMB new, zeroed memory under the same descriptor, for the next
 * client: the one that went keeps its mapping of the old memory, which
 * nothing reaches any more.  A device without a CMB has nothing to renew.
 * Returns 0, or -1 with errno set, leaving the CMB as it was.  Main loop
 * thread only, with no client attached.
 */
int ionic_eth_emu_renew_cmb(struct ionic_eth_emu *emu);

/*
 * Zero the CMB, BAR4, so that nothing written there -- work requests and
 * their inline data -- survives a device reset or the client that wrote it.
 * It runs on every reset libvfio-user reports, which includes a client's
 * going, through ionic_eth_emu_reset(), and on the driver's
 * IONIC_CMD_RESET.  @emu may be NULL, and a device without a CMB has
 * nothing to clear.  Main loop thread only.
 */
void ionic_eth_emu_clear_cmb(struct ionic_eth_emu *emu);

/*
 * Trigger an MSI-X vector (0 = success, -EINVAL = bad vec).  One that is
 * masked, or raised while Bus Master Enable is clear, is held and returns 0:
 * it is delivered once the vector is unmasked, or by ionic_eth_emu_resume().
 */
int ionic_eth_emu_trigger_irq(struct ionic_eth_emu *emu, int vec);

/*
 * Set the statistics (ernic_stats.h) the emulator counts interrupts and
 * Ethernet bytes into, or NULL for none.  They must outlive @emu.
 */
void ionic_eth_emu_set_stats(struct ionic_eth_emu *emu,
                             struct ernic_stats *stats);

/* Register the datapath handler for doorbell writes (BAR2). */
struct ionic_datapath;
void ionic_eth_emu_register_datapath(struct ionic_eth_emu *emu,
                                     struct ionic_datapath *dp);

/*
 * Register the admin queue context for AQ doorbell producer-index updates,
 * or NULL when there is none.  ionic_rdma_devcmd.c owns the context and
 * calls this whenever it creates or frees one.
 */
struct ionic_adminq_ctx;
void ionic_eth_emu_register_adminq(struct ionic_eth_emu *emu,
                                   struct ionic_adminq_ctx *adminq);

/*
 * Set the station MAC the LIF reports to the guest.  Two instances bridged
 * together must not share one, so the launcher derives it from the instance
 * id; without this every guest on the bridge answers to the same address.
 */
void ionic_eth_emu_set_mac(struct ionic_eth_emu *emu,
                           const uint8_t mac[ETH_ADDR_LEN]);

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
void ionic_eth_emu_get_tx_filter(const struct ionic_eth_emu *emu,
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
 * Hand a frame for the guest to the emulated LIF from an in-process
 * endpoint: the S3 control plane's replies, and what the P4 pipeline sends
 * to the host port.  The frame is copied onto an internal queue and
 * delivered by the next ionic_eth_emu_poll_rx(), which holds it there until
 * the Rx queue it steers to has a buffer.  Main loop thread only: the queue
 * is not locked.
 *
 * Returns 0 on success, -ENOSPC when the queue is full, -ENOMEM, or
 * -EINVAL.
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

/*
 * The counters behind the published block, with the RDMA ones refreshed
 * from the data path, for the --stats-file writer.  Main loop thread only.
 */
const struct ionic_emu_stats *ionic_eth_emu_get_stats(
    struct ionic_eth_emu *emu);

/*
 * Write the "LIF Statistics" section of the --stats-file: every named
 * counter of struct ionic_lif_stats, by its ionic_if.h name, zero or not, so
 * the set of lines never changes.  @opaque is the struct ionic_eth_emu; it's
 * an ernic_stats_section_fn.  Main loop thread only.
 */
void ionic_eth_emu_write_stats(FILE *fp, void *opaque);

/* Count one RDMA message of @bytes in the LIF's tx/rx_rdma_ucast_* stats. */
void ionic_eth_emu_count_rdma(struct ionic_eth_emu *emu, uint64_t bytes,
                              bool is_tx);

/*
 * Send a frame out of the host network backend.
 *
 * The Tx path reaches the backend by itself; this is for an in-process
 * endpoint that claimed a frame through a filter and has to put it back on
 * the wire, possibly rewritten.  Returns 0, -ENODEV when no backend is
 * attached, or a negative errno from the backend.
 */
int ionic_eth_emu_send_to_net(const struct ionic_eth_emu *emu,
                              const void *frame, size_t len);

/*
 * Move any frames waiting on the host backend or on the off-thread queue
 * into the guest's Rx ring.  Must be called from the thread that owns the
 * vfio-user context; the server's main loop does this on every iteration.
 */
void ionic_eth_emu_poll_rx(struct ionic_eth_emu *emu);

#endif /* IONIC_ETH_EMU_H */
