/*
 * RDMA Backend: TCP mesh - GIDs
 *
 * Which node owns each guest RoCE GID: the claims each node advertises
 * (node_gids), the GID -> node index derived from them (gid_owner), and
 * resolution against it.  Also this node's own GIDs, and the
 * TCP_MSG_NODE_GIDS messages that advertise a node's set.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma_backend_tcp_priv.h"
#include "rdma_utils.h"
#include "qemu/thread.h"
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * Format a GID for a log line.  All 16 bytes: the destination GID is
 * frequently IPv6 link-local, and printing only the tail as a dotted quad
 * renders those as nonsense.  Returns @buf.
 *
 * Written out by hand rather than with snprintf("%02x"): under
 * -Wformat-truncation=2 gcc assumes the worst-case width of a %x conversion
 * rather than the 2 digits a uint8_t can actually produce, and rejects any
 * buffer sized for the real output.  Indexing a hex table sidesteps the
 * format analysis entirely and TCP_GID_STR_LEN is then exact.
 */
char *tcp_gid_str(const uint8_t gid[16], char buf[TCP_GID_STR_LEN])
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;

    for (size_t i = 0; i < 8; i++) {
        if (i > 0) {
            buf[o++] = ':';
        }
        buf[o++] = hex[gid[i * 2] >> 4];
        buf[o++] = hex[gid[i * 2] & 0x0f];
        buf[o++] = hex[gid[i * 2 + 1] >> 4];
        buf[o++] = hex[gid[i * 2 + 1] & 0x0f];
    }
    buf[o] = '\0';
    return buf;
}

/*
 * Parse the value of ERNIC_TCP_GUEST_GIDS, which may be NULL: a
 * comma-separated list of this instance's own guest RoCE addresses, each in
 * inet_pton() notation.  An IPv4 address is stored in the ::ffff:a.b.c.d
 * mapped form that adminq_parse_roce_hdr() builds for a RoCE-over-IPv4
 * header, so both branches compare equal to what arrives on the wire.
 *
 * A malformed entry is reported and skipped rather than fatal: losing one
 * address costs connectivity to one peer, which the unresolved-GID error
 * names, while refusing to start costs the whole node.
 *
 * The caller reads the environment, so the tests can pass a list without
 * calling setenv(), which isn't thread safe.
 */
uint32_t tcp_parse_guest_gids(const char *val,
                              uint8_t out[TCP_MAX_NODE_GIDS][16])
{
    uint32_t n = 0;

    if (!val || !*val)
        return 0;

    char *dup = g_strdup(val);
    char *save = NULL;

    for (char *tok = strtok_r(dup, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        /* Trim through a separate cursor rather than advancing tok: strtok_r
         * owns the loop variable, and walking it here reads as a second
         * iteration step. */
        char *val_s = tok;

        while (*val_s == ' ')
            val_s++;
        char *end = val_s + strlen(val_s);
        while (end > val_s && end[-1] == ' ')
            *--end = '\0';
        if (!*val_s)
            continue;

        if (n == TCP_MAX_NODE_GIDS) {
            rdma_warn_report("TCP: more than %d entries in "
                             "ERNIC_TCP_GUEST_GIDS; ignoring the rest",
                             TCP_MAX_NODE_GIDS);
            break;
        }

        struct in6_addr a6;
        struct in_addr a4;

        if (inet_pton(AF_INET6, val_s, &a6) == 1) {
            memcpy(out[n], &a6, 16);
        } else if (inet_pton(AF_INET, val_s, &a4) == 1) {
            memset(out[n], 0, 16);
            out[n][10] = 0xff;
            out[n][11] = 0xff;
            memcpy(out[n] + 12, &a4, 4);
        } else {
            rdma_error_report("TCP: ignoring unparsable GID '%s' in "
                              "ERNIC_TCP_GUEST_GIDS",
                              val_s);
            continue;
        }
        n++;
    }

    g_free(dup);
    return n;
}

/* g_bytes_unref() takes GBytes*, not gpointer; casting the pointer type
 * instead trips -Wcast-function-type-strict under clang. */
void tcp_gid_key_free(gpointer key)
{
    g_bytes_unref((GBytes *)key);
}

/*
 * Rebuild the GID -> owner index from the per-node claims.
 *
 * A GID claimed by exactly one node resolves to it.  A GID claimed by two
 * or more maps to UINT32_MAX, which resolution treats as unresolved, so a
 * duplicate address makes both claimants unreachable at it rather than
 * handing the traffic to whichever advertised last.  Keeping the claims
 * and deriving this means a contested GID stays contested: a later
 * advertisement from either claimant, or a third, re-derives the same
 * answer instead of finding an empty slot to take.
 *
 * Caller holds gid_table_lock.
 */
static void tcp_gid_index_rebuild(TcpBackendPrivate *priv)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_remove_all(priv->gid_owner);

    g_hash_table_iter_init(&iter, priv->node_gids);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        uint32_t node_id = GPOINTER_TO_UINT(key);
        const TcpNodeGids *claims = value;

        /*
         * A node the manager has declared dead keeps its claims but takes
         * no part in resolution, so its addresses are unreachable rather
         * than wrong. It also stops contesting them, which is what lets a
         * node that is restarted -- and so registers under a new id, since
         * ids are allocated monotonically, while the dead one stays until
         * a full mesh needs its slot -- own its guest's GID instead of
         * deadlocking against the id it used to have.
         */
        if (!claims->alive)
            continue;

        for (uint32_t i = 0; i < claims->num_gids; i++) {
            GBytes *gid = g_bytes_new(claims->gids[i], 16);
            gpointer held;

            if (g_hash_table_lookup_extended(priv->gid_owner, gid, NULL,
                                             &held) &&
                GPOINTER_TO_UINT(held) != node_id) {
                /* Contested: replace the value, keep the existing key. */
                g_hash_table_insert(priv->gid_owner, gid,
                                    GUINT_TO_POINTER(UINT32_MAX));
            } else {
                g_hash_table_insert(priv->gid_owner, gid,
                                    GUINT_TO_POINTER(node_id));
            }
        }
    }
}

/*
 * The table half of tcp_gid_claims_set(), which has checked its arguments.
 * @alive is whether the claims take part in resolution; see TcpNodeGids.
 * Returns whether that changed what @node_id claims, or whether its claims
 * are alive.  Only a change rebuilds the index, which takes time in the
 * number of claims, under the lock the data path resolves under; the manager
 * sends a node's set to every worker whenever it may have changed, and the
 * whole table to a worker that registers again, most of it unchanged.
 */
static bool tcp_gid_claims_record(TcpBackendPrivate *priv, uint32_t node_id,
                                  const uint8_t (*gids)[16], uint32_t num_gids,
                                  bool alive)
{
    bool changed;

    qemu_mutex_lock(&priv->gid_table_lock);

    const TcpNodeGids *old =
        g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(node_id));

    if (num_gids == 0) {
        changed =
            g_hash_table_remove(priv->node_gids, GUINT_TO_POINTER(node_id));
    } else if (old && old->alive == alive && old->num_gids == num_gids &&
               memcmp(old->gids, gids, (size_t)num_gids * 16) == 0) {
        changed = false;
    } else {
        TcpNodeGids *claims = g_new0(TcpNodeGids, 1);

        claims->num_gids = num_gids;
        claims->alive = alive;
        for (uint32_t i = 0; i < num_gids; i++)
            memcpy(claims->gids[i], gids[i], 16);
        g_hash_table_insert(priv->node_gids, GUINT_TO_POINTER(node_id), claims);
        changed = true;
    }

    if (changed)
        tcp_gid_index_rebuild(priv);

    qemu_mutex_unlock(&priv->gid_table_lock);
    return changed;
}

/*
 * The logging half of tcp_gid_claims_set(): what @node_id now owns, given
 * whether its claims were recorded @alive.  Called with no lock held, since
 * rdma_info_report() can block on stderr.
 */
static void tcp_gid_claims_report(TcpBackendPrivate *priv, uint32_t node_id,
                                  const uint8_t (*gids)[16], uint32_t num_gids,
                                  bool alive)
{
    char buf[TCP_GID_STR_LEN];

    if (num_gids == 0) {
        rdma_info_report("TCP: node %u withdrew its GIDs", node_id);
        return;
    }
    if (!alive) {
        rdma_info_report("TCP: node %u, which is dead, claimed %u GID(s); "
                         "they don't resolve to it until it answers",
                         node_id, num_gids);
        return;
    }

    for (uint32_t i = 0; i < num_gids; i++) {
        uint32_t owner;

        qemu_mutex_lock(&priv->gid_table_lock);
        GBytes *gid = g_bytes_new_static(gids[i], 16);
        gpointer held;
        owner = g_hash_table_lookup_extended(priv->gid_owner, gid, NULL, &held)
                    ? GPOINTER_TO_UINT(held)
                    : UINT32_MAX;
        g_bytes_unref(gid);
        qemu_mutex_unlock(&priv->gid_table_lock);

        if (owner == node_id) {
            rdma_info_report("TCP: node %u owns GID %s", node_id,
                             tcp_gid_str(gids[i], buf));
        } else {
            rdma_error_report("TCP: GID %s is claimed by more than one node "
                              "including node %u; it resolves to none of "
                              "them until the duplicate is removed",
                              tcp_gid_str(gids[i], buf), node_id);
        }
    }
}

/*
 * Record @node_id's advertised set, replacing whatever it claimed before.
 * An empty set withdraws its claims, which is how the manager revokes.
 * Returns whether that changed anything, and only a change is logged.
 *
 * The claims are recorded live.  A worker records what the manager relays,
 * and the manager advertises a dead node as owning nothing.  The manager
 * records only its own claims here, and a worker's through
 * tcp_gid_claims_set_if_member(), which takes the node's liveness.
 */
bool tcp_gid_claims_set(TcpBackendPrivate *priv, uint32_t node_id,
                        const uint8_t (*gids)[16], uint32_t num_gids)
{
    if (!priv || !priv->node_gids || num_gids > TCP_MAX_NODE_GIDS)
        return false;

    if (!tcp_gid_claims_record(priv, node_id, gids, num_gids, true))
        return false;
    tcp_gid_claims_report(priv, node_id, gids, num_gids, true);
    return true;
}

/*
 * tcp_gid_claims_set() for a worker's own advertisement on the manager, but
 * only if the worker's node is still in mesh_nodes; returns whether it was,
 * whether or not the advertisement changed anything.
 *
 * A registration can evict a node whose receive thread is still handling a
 * message: a connection leaves the table before its thread is joined
 * (tcp_health_check_unpublish()).  Recorded then, the claims of a node no
 * broadcast carries would stay, alive, until teardown, contesting those of
 * the same guest under its new id.  Checking and recording under
 * mesh_table_lock, which the eviction holds while it drops the node's
 * claims (tcp_gid_claims_forget()), puts each advertisement either before
 * the eviction, whose drop then covers it, or after, when it is refused.
 *
 * The claims are recorded live only if the node is, so that they take part
 * in resolution exactly when the topology says the node is alive (see
 * tcp_node_set_alive()).  A node the health check has declared dead can
 * still have a message handled, on a connection not yet retired, and it
 * owns what it advertised then only once it answers a heartbeat.
 *
 * Lock order: gid_table_lock under mesh_table_lock (see Locks in
 * rdma_backend_tcp_priv.h).
 */
static bool tcp_gid_claims_set_if_member(TcpBackendPrivate *priv,
                                         uint32_t node_id,
                                         const uint8_t (*gids)[16],
                                         uint32_t num_gids)
{
    const MeshNodeInfo *node;
    bool member, alive = false, changed = false;

    if (!priv || !priv->node_gids || num_gids > TCP_MAX_NODE_GIDS)
        return false;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(node_id));
    member = node != NULL;
    if (member) {
        alive = node->is_alive;
        changed = tcp_gid_claims_record(priv, node_id, gids, num_gids, alive);
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (changed)
        tcp_gid_claims_report(priv, node_id, gids, num_gids, alive);
    return member;
}

/*
 * Discard what @node_id claimed, once it has been evicted from mesh_nodes
 * (tcp_evict_dead_node()).  Unlike an empty advertisement, it logs nothing:
 * the node didn't withdraw anything, and the caller says why it is gone.
 */
void tcp_gid_claims_forget(TcpBackendPrivate *priv, uint32_t node_id)
{
    qemu_mutex_lock(&priv->gid_table_lock);
    if (g_hash_table_remove(priv->node_gids, GUINT_TO_POINTER(node_id)))
        tcp_gid_index_rebuild(priv);
    qemu_mutex_unlock(&priv->gid_table_lock);
}

/*
 * Worker: drop the claims of every node but this one that the first
 * @num_nodes entries of a MESH_TOPOLOGY update from the manager leave out.
 *
 * A node evicted from the mesh (tcp_evict_dead_node()) is advertised as
 * owning nothing once, when it is evicted, and never again: the GID tables
 * that follow carry only the nodes still in the mesh.  A worker that wasn't
 * connected to the manager just then, and registers again later, would keep
 * the node's claims for good, contesting those of its guest under a new id.
 * The topology says who is in the mesh, so claims outside it are dropped.
 *
 * Only the claims of a node that has left the mesh are dropped: the manager
 * sends its topology updates and GID sets from one thread, in the order it
 * took them, each taken just before it is sent (broadcast_thread).  A GID
 * set with a node's claims was taken while the node was in mesh_nodes, so an
 * update without the node, sent after it, was taken after the node left.
 */
void tcp_gid_claims_prune(TcpBackendPrivate *priv,
                          const TcpMeshTopologyPayload *topo,
                          uint32_t num_nodes)
{
    uint32_t local = tcp_local_node(priv);
    GHashTableIter iter;
    gpointer key;
    guint dropped = 0;

    if (!priv->node_gids)
        return;
    if (num_nodes > G_N_ELEMENTS(topo->nodes))
        num_nodes = G_N_ELEMENTS(topo->nodes);

    qemu_mutex_lock(&priv->gid_table_lock);
    g_hash_table_iter_init(&iter, priv->node_gids);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        uint32_t node_id = GPOINTER_TO_UINT(key);
        bool listed = node_id == local;

        for (uint32_t i = 0; !listed && i < num_nodes; i++)
            listed = ntohl(topo->nodes[i].node_id) == node_id;
        if (!listed) {
            g_hash_table_iter_remove(&iter);
            dropped++;
        }
    }
    if (dropped)
        tcp_gid_index_rebuild(priv);
    qemu_mutex_unlock(&priv->gid_table_lock);

    /* Logged outside the lock: rdma_info_report() can block on stderr. */
    if (dropped)
        rdma_info_report("TCP: dropped the GIDs of %u node(s) no longer in "
                         "the mesh",
                         dropped);
}

/*
 * Returns whether the claims changed.  Logs nothing: on the manager it runs
 * under mesh_table_lock, called by tcp_node_set_alive(), and the caller
 * logs (tcp_gid_claims_report_alive()), outside the lock where it can.
 */
bool tcp_gid_claims_set_alive(TcpBackendPrivate *priv, uint32_t node_id,
                              bool alive)
{
    bool changed = false;

    if (!priv || !priv->node_gids)
        return false;

    qemu_mutex_lock(&priv->gid_table_lock);

    TcpNodeGids *claims =
        g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(node_id));
    if (claims && claims->alive != alive) {
        claims->alive = alive;
        tcp_gid_index_rebuild(priv);
        changed = true;
    }

    qemu_mutex_unlock(&priv->gid_table_lock);
    return changed;
}

/* Say that @node_id's claims have become live or dead. */
void tcp_gid_claims_report_alive(uint32_t node_id, bool alive)
{
    rdma_info_report("TCP: node %u's GIDs are %s", node_id,
                     alive ? "live again" : "unreachable while it is away");
}

/*
 * Resolve a destination GID to a mesh node id.
 *
 * An exact match on all 16 bytes against what the nodes advertised about
 * themselves (TCP_MSG_NODE_GIDS, sourced from ERNIC_TCP_GUEST_GIDS).  There
 * is deliberately no rule that derives a node id from part of a GID: the
 * guest kernel builds these addresses, nothing constrains them to encode a
 * node id, and guessing routed every peer to one node.
 *
 * Returns UINT32_MAX when the GID names no node this mesh knows.  Callers
 * must fail -- picking a default delivers the traffic to the wrong node.
 */
uint32_t tcp_resolve_node_from_gid(TcpBackendPrivate *priv,
                                   const union ibv_gid *dgid)
{
    if (!priv || !dgid || !priv->gid_owner)
        return UINT32_MAX;

    GBytes *key = g_bytes_new_static(dgid->raw, 16);
    gpointer value;
    gboolean found;

    qemu_mutex_lock(&priv->gid_table_lock);
    found = g_hash_table_lookup_extended(priv->gid_owner, key, NULL, &value);
    qemu_mutex_unlock(&priv->gid_table_lock);
    g_bytes_unref(key);

    /* A contested GID is stored as UINT32_MAX, so it falls out here as
     * unresolved without a separate case. */
    return found ? GPOINTER_TO_UINT(value) : UINT32_MAX;
}

/*
 * Build the advertisement for @node_id's GIDs.  The whole fixed-size struct
 * goes on the wire including the unused slots, so it is zeroed first -- the
 * same stack-disclosure trap tcp_send_mesh_topology() guards against.
 */
void tcp_fill_node_gids(TcpNodeGidsPayload *out, uint32_t node_id,
                        const uint8_t (*gids)[16], uint32_t num_gids)
{
    memset(out, 0, sizeof(*out));
    if (num_gids > TCP_MAX_NODE_GIDS)
        num_gids = TCP_MAX_NODE_GIDS;
    out->node_id = htonl(node_id);
    out->num_gids = htonl(num_gids);
    for (uint32_t i = 0; i < num_gids; i++)
        memcpy(out->gids[i], gids[i], 16);
}

/* Send one advertisement on an already-locked-free connection. */
void tcp_send_node_gids(TcpBackendPrivate *priv, TcpConnection *conn,
                        const TcpNodeGidsPayload *payload)
{
    if (!conn)
        return;

    /* Checked under the lock tcp_retire_conn() changes them under */
    qemu_mutex_lock(&conn->lock);
    if (atomic_load(&conn->is_connected) && conn->sockfd >= 0) {
        tcp_send_message(
            priv, conn, TCP_MSG_NODE_GIDS, payload, sizeof(*payload),
            atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
            tcp_local_node(priv), conn->node_id, 0, 0);
    }
    qemu_mutex_unlock(&conn->lock);
}

/*
 * Record this node's own GIDs and tell the mesh about them.
 *
 * Called at registration, and again whenever the set changes -- a guest that
 * brings its RDMA device up after the mesh has formed, or that is given an
 * address later, has GIDs nobody has heard of, and a peer cannot resolve what
 * was never advertised.  TCP_MSG_NODE_GIDS already means "node N's current
 * set" and its handler replaces rather than merges, so re-advertising is the
 * whole update protocol: no new message type, no version bump.
 *
 * Claims are recorded locally as well as sent.  A QP whose destination is this
 * node's own guest has to resolve to the local node rather than fail, and on
 * the manager nothing else would record them at all.
 *
 * Silent on a worker before registration: tcp_local_node() is 0 until the
 * manager answers, and node 0 is the manager's own id, so claiming under it
 * would collide with the manager's claims and leave both contested.
 * Registration calls this again, so nothing is lost.  The manager's id is
 * fixed at 0 from tcp_init(), so it never waits.
 */
void tcp_advertise_local_gids(TcpBackendPrivate *priv)
{
    if (!priv)
        return;
    if (!priv->is_manager && !tcp_worker_registered(priv))
        return;

    uint32_t node = tcp_local_node(priv);

    /* Held across both the snapshot and the send; see the field's comment. */
    qemu_mutex_lock(&priv->gid_advertise_lock);

    /*
     * local_gids is read on the main thread and on a receive thread, so copy
     * it under the lock that guards it.  The copy, not the field, is what the
     * rest of this function uses: tcp_send_node_gids() takes the connection's
     * lock, which a sender holds while it waits for a peer to read, so lock
     * cannot be held that long.
     */
    uint8_t gids[TCP_MAX_NODE_GIDS][16];
    uint32_t num_gids;

    qemu_mutex_lock(&priv->lock);
    num_gids = priv->num_local_gids;
    memcpy(gids, priv->local_gids, sizeof(gids));
    qemu_mutex_unlock(&priv->lock);

    tcp_gid_claims_set(priv, node, tcp_const_gids(gids), num_gids);

    if (priv->is_manager) {
        /* The manager is the authority: its broadcaster tells every worker */
        tcp_broadcast_node_gids(priv, node);
        qemu_mutex_unlock(&priv->gid_advertise_lock);
        return;
    }

    /*
     * With no connection to the manager, nothing is sent: the answer to the
     * next registration advertises the set again.
     */
    TcpNodeGidsPayload mine;
    TcpConnection *mgr = tcp_worker_manager_conn(priv);

    tcp_fill_node_gids(&mine, node, tcp_const_gids(gids), num_gids);
    tcp_send_node_gids(priv, mgr, &mine);
    tcp_connection_unref(mgr);

    qemu_mutex_unlock(&priv->gid_advertise_lock);
}

/*
 * Claim @gid for this node, or withdraw it, and re-advertise.
 *
 * The backend's add-GID op used to overwrite the caller's GID with a
 * synthetic one holding only the local node id, which claimed nothing:
 * tcp_gid_claims_set() was never called and the mesh's only GID source was
 * ERNIC_TCP_GUEST_GIDS, read once at startup.  A guest address that appeared
 * later resolved nowhere, so a peer-addressed datagram could not find its
 * node.
 */
int tcp_claim_gid(TcpBackendPrivate *priv, const union ibv_gid *gid, bool add)
{
    uint32_t i;
    bool changed = false;

    if (!priv || !gid)
        return -EINVAL;

    qemu_mutex_lock(&priv->lock);

    for (i = 0; i < priv->num_local_gids; i++) {
        if (memcmp(priv->local_gids[i], gid->raw, 16) == 0)
            break;
    }

    if (add) {
        if (i == priv->num_local_gids) {
            if (priv->num_local_gids >= TCP_MAX_NODE_GIDS) {
                qemu_mutex_unlock(&priv->lock);
                rdma_error_report(
                    "TCP: cannot claim another GID, the %d-entry table is full",
                    TCP_MAX_NODE_GIDS);
                return -ENOSPC;
            }
            memcpy(priv->local_gids[priv->num_local_gids++], gid->raw, 16);
            changed = true;
        }
    } else if (i < priv->num_local_gids) {
        /* Order does not matter to a claim set, so close the hole with the
         * last entry rather than shifting the tail. */
        priv->num_local_gids--;
        memcpy(priv->local_gids[i], priv->local_gids[priv->num_local_gids], 16);
        memset(priv->local_gids[priv->num_local_gids], 0, 16);
        changed = true;
    }

    qemu_mutex_unlock(&priv->lock);

    if (changed)
        tcp_advertise_local_gids(priv);

    return 0;
}

/*
 * TCP_MSG_NODE_GIDS: a node's GID set.  On the manager, a worker's own
 * advertisement, which the manager records and fans out to the rest; on a
 * worker, the manager's word for what a node owns.
 */
void tcp_recv_node_gids(TcpConnection *conn, const TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;
    const TcpNodeGidsPayload *adv = msg->payload;
    uint32_t node_id, num_gids;

    if (!priv || !adv || msg->hdr.msg_len < sizeof(TcpNodeGidsPayload)) {
        rdma_error_report("TCP: Invalid NODE_GIDS payload");
        return;
    }

    node_id = ntohl(adv->node_id);
    num_gids = ntohl(adv->num_gids);

    if (num_gids > TCP_MAX_NODE_GIDS) {
        rdma_error_report("TCP: NODE_GIDS from node %u claims %u GIDs, max "
                          "is %d",
                          msg->hdr.src_node_id, num_gids, TCP_MAX_NODE_GIDS);
        return;
    }

    /*
     * Authorise on the connection, not on the message.  Both
     * hdr.src_node_id and the payload's node_id are written by the sender,
     * so comparing them to each other proves nothing: a worker can set both
     * to a peer's id and replace that peer's GIDs mesh-wide.  conn is what
     * the local node established.
     *
     * On the manager a worker speaks only for itself, and only once
     * registration has given its connection an id -- before that
     * conn->node_id is UINT32_MAX and the socket has proved nothing at all.
     *
     * On a worker the only legitimate source is the manager relaying on
     * others' behalf, which is the one connection this node opened itself,
     * once the manager has accepted its registration on it.  Matching on
     * src_node_id == 0 instead would let any peer claim to be the manager.
     */
    if (priv->is_manager) {
        if (conn->node_id == UINT32_MAX || conn->node_id != node_id) {
            rdma_error_report("TCP: connection for node %u tried to set the "
                              "GIDs of node %u; ignoring",
                              conn->node_id, node_id);
            return;
        }
    } else if (conn != atomic_load(&priv->manager_conn)) {
        rdma_error_report("TCP: NODE_GIDS for node %u from a connection that "
                          "is not the manager's, or before registration; "
                          "ignoring",
                          node_id);
        return;
    }

    if (!priv->is_manager) {
        tcp_gid_claims_set(priv, node_id, adv->gids, num_gids);
        return;
    }

    if (!tcp_gid_claims_set_if_member(priv, node_id, adv->gids, num_gids)) {
        rdma_info_report("TCP: ignoring the GIDs of node %u, which has been "
                         "evicted from the mesh",
                         node_id);
        return;
    }

    /*
     * Manager fans a worker's advertisement out to the rest, and back to the
     * worker itself: a dead node's comes back empty, which is how it learns
     * that its claims don't count until it answers.
     */
    tcp_broadcast_node_gids(priv, node_id);
}
