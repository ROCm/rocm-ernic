/*
 * The TCP mesh, for the ionic data path
 *
 * ionic resolves rkeys against its own guest-physical MR table rather than
 * through rdma_rm, so it cannot use the backend's RDMA messages.  It carries
 * its own protocol as an opaque mesh payload instead; these are thin
 * wrappers over the backend's mesh operations (RdmaMeshOps, in
 * rdma/rdma_backend_ops.h), through the opaque handle from
 * rocm_ernic_compat.h, so ionic_datapath.c does not have to pull in the
 * QEMU-derived headers.  rocm_ernic_compat.c implements them, and answers
 * for a backend with no mesh (loopback, none, nvmeof and s3) itself.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_MESH_H
#define ERNIC_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rocm_ernic_compat.h"

/*
 * An ernic_mesh_recv_fn that returns true has taken buf, which is from
 * malloc(), and must free() it on every path, rejections included.  One that
 * returns false has no room for the message yet, and leaves buf with the
 * mesh, which offers it again shortly and reads nothing more from that peer
 * meanwhile.  len is never 0.
 */
typedef bool (*ernic_mesh_recv_fn)(void *opaque, uint32_t src_node, void *buf,
                                   size_t len);

/*
 * Largest single message ernic_mesh_sendv() accepts, header included.  Mirrors
 * TCP_MAX_PAYLOAD_LEN in rdma_backend_tcp_priv.h, which static-asserts the
 * two agree.
 */
#define ERNIC_MESH_MAX_MSG (16U << 20)

/*
 * Most reply bytes, headers included, that a node may have asked one peer
 * for at once.  The peer queues its replies to the node, and drops one it has
 * no room to queue, so a node that asked for more could lose them whenever it
 * falls behind reading the peer.  This is the room the peer's queue is sure
 * to have for replies, beside the most it queues of its own requests:
 * TCP_SENDQ_REPLY_BYTES - TCP_SENDQ_REQUEST_BYTES.  rdma_backend_tcp_priv.h
 * static-asserts that it fits.
 */
#define ERNIC_MESH_REPLY_BUDGET (32U << 20)

/*
 * ernic_mesh_local_node() and ernic_mesh_node_from_gid() return UINT32_MAX
 * when the instance has no mesh backend (loopback or none), which the caller
 * reads as "every peer is local".
 */
uint32_t ernic_mesh_local_node(pvrdma_handle_t handle);
uint32_t ernic_mesh_node_from_gid(pvrdma_handle_t handle,
                                  const uint8_t *dest_gid_16bytes);
/*
 * Claim @gid_16bytes for this node, or withdraw it, and re-advertise to the
 * mesh.  Returns 0 on a non-mesh backend, which has no peers to tell.
 *
 * The mesh's only GID source in practice is still ERNIC_TCP_GUEST_GIDS, read
 * once at startup: nothing dispatches a guest GID-table change to here, so a
 * guest address that appears later resolves nowhere.  This is the entry
 * point that would fix that, and the gsi-mesh test is its only caller.
 */
int ernic_mesh_claim_gid(pvrdma_handle_t handle, const uint8_t *gid_16bytes,
                         bool add);
/*
 * Send a message to another node without waiting for it: 0 once queued, or a
 * negative errno.  -EAGAIN means a request (!reply) found the queue to
 * dst_node full, and is to be tried again later; a reply is queued past that
 * point.  tag is the caller's name for a request, for ernic_mesh_cancel(); a
 * reply's is ignored.  See tcp_mesh_sendv() in rdma_backend_tcp.c.
 */
int ernic_mesh_sendv(pvrdma_handle_t handle, uint32_t dst_node, const void *hdr,
                     size_t hdr_len, const void *body, size_t body_len,
                     uint64_t tag, bool reply);
/*
 * Whether a request of len bytes, header included, to dst_node would be
 * queued now rather than refused with -EAGAIN; true when there is no mesh.
 */
bool ernic_mesh_has_room(pvrdma_handle_t handle, uint32_t dst_node, size_t len);
/*
 * Take back the queued requests that haven't started to go out and whose tag
 * matches tag in the bits mask selects, so none of them reaches its peer.
 * Replies are left alone.  See tcp_mesh_cancel() in rdma_backend_tcp.c.
 */
void ernic_mesh_cancel(pvrdma_handle_t handle, uint64_t tag, uint64_t mask);
/*
 * Register fn to be called, on a receive thread, with each message for this
 * node, or none for NULL.  Returns once no receive thread is still calling
 * the callback it replaces, so that clearing it lets the caller free what it
 * used.  Never call it from the callback.
 */
void ernic_mesh_set_recv_cb(pvrdma_handle_t handle, ernic_mesh_recv_fn fn,
                            void *opaque);

#endif /* ERNIC_MESH_H */
