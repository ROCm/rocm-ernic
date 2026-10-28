/*
 * RDMA Backend: TCP mesh - the manager
 *
 * What only the manager, node 0, does: keep the table of mesh nodes
 * (mesh_nodes), register workers and evict dead nodes to make room, tell the
 * workers what has changed from its broadcaster thread, check that each
 * worker is still answering from its health-check thread, and relay the data
 * path's messages between workers that have no connection of their own.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma/rdma_backend_tcp_priv.h"
#include "byteorder.h"
#include "ernic_time.h"
#include "pvrdma/rdma_utils.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <time.h>
#include <inttypes.h>

/* Mesh node management helpers */
void tcp_mesh_node_info_free(MeshNodeInfo *node)
{
    if (!node) {
        return;
    }
    g_free(node->hostname);
    g_free(node);
}

/* GDestroyNotify adapter for tcp_mesh_node_info_free() */
void tcp_mesh_node_info_destroy_notify(gpointer data)
{
    tcp_mesh_node_info_free(data);
}

/* A new node, or NULL if hostname is NULL or empty */
MeshNodeInfo *tcp_mesh_node_info_new(uint32_t node_id, const char *hostname,
                                     uint16_t port)
{
    if (!hostname || !*hostname) {
        rdma_error_report("TCP: Invalid hostname for node %u", node_id);
        return NULL;
    }

    MeshNodeInfo *node = g_new0(MeshNodeInfo, 1);

    node->node_id = node_id;
    node->hostname = g_strdup(hostname);
    node->port = port;
    node->is_alive = true;
    node->last_heartbeat_ms = ernic_monotonic_ms();
    return node;
}

/*
 * Declare @node alive or dead, to the mesh and to GID resolution at once:
 * its is_alive, and whether its claims take part in resolution.  Once the
 * node is in mesh_nodes, this is the only writer of is_alive, and the only
 * thing that changes the liveness of claims already recorded; claims
 * recorded later get is_alive (tcp_gid_claims_set_if_member()).  All of it
 * is under mesh_table_lock, so whoever holds that lock sees the two agree.
 *
 * Returns whether is_alive changed, and in *gids_changed, if it isn't NULL,
 * whether the claims did, which they don't if the node has none.  Logs
 * nothing; the caller says why, and tcp_gid_claims_report_alive() what
 * became of the claims.
 *
 * Caller holds mesh_table_lock; gid_table_lock is taken under it, the
 * documented order.
 */
bool tcp_node_set_alive(TcpBackendPrivate *priv, MeshNodeInfo *node, bool alive,
                        bool *gids_changed)
{
    bool changed = node->is_alive != alive;
    bool gids;

    node->is_alive = alive;
    gids = tcp_gid_claims_set_alive(priv, node->node_id, alive);
    if (gids_changed)
        *gids_changed = gids;
    return changed;
}

/*
 * Whether a registration can't be given node_id: a node has it, or a
 * connection does.  The caller holds mesh_table_lock and conn_table_lock.
 */
static bool tcp_node_id_taken(const TcpBackendPrivate *priv, uint32_t node_id)
    TSA_REQUIRES(priv->mesh_table_lock, priv->conn_table_lock)
{
    return g_hash_table_contains(priv->mesh_nodes, GUINT_TO_POINTER(node_id)) ||
           g_hash_table_contains(priv->connections, GUINT_TO_POINTER(node_id));
}

/*
 * Whether a registration from host:port asking for node_id can have it back.
 * A worker asks for its own id when it has given up on its connection to the
 * manager and connected again.  It gets the id if it is a worker in the mesh,
 * registered from that address, that can't be answering on the connection it
 * had: the connection is gone or has ended, or the health check has declared
 * the node dead.  The manager may not have noticed yet that the connection
 * has ended, so a worker that is refused for that asks again on its next
 * pass.  One refused with -ENOENT, its node evicted (tcp_evict_dead_node()),
 * doesn't, since the id is never given out again.
 *
 * If it can, takes the node's old connection, if it has one, out of
 * connections, for the caller to retire once it has dropped the locks, and
 * returns 0 with it in *old.  Otherwise returns the negative errno to refuse
 * with, and why in *why, for the caller to log once it has dropped them.  The
 * caller holds mesh_table_lock and conn_table_lock.
 *
 * Nothing is taken once teardown has begun.  tcp_stop_recv_threads() claims
 * each connection's thread under conn_table_lock and joins it afterwards,
 * through a pointer that holds no reference: taken out and retired here, the
 * connection would be freed under it.  stopping is set before that claim, and
 * read here under the same lock, so a registration either sees it or takes
 * the connection out before teardown can claim it.
 *
 * The address is a check against a mistake, not an attacker: a peer can
 * claim any address, and any peer can register.  A node's hostname and port
 * never change, and a worker keeps its listening port as long as it runs, so
 * another port is another process.
 */
static int tcp_register_again(TcpBackendPrivate *priv, uint32_t node_id,
                              const char *host, uint16_t port,
                              TcpConnection **old, const char **why)
    TSA_REQUIRES(priv->mesh_table_lock, priv->conn_table_lock)
{
    const MeshNodeInfo *node =
        g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(node_id));
    TcpConnection *conn;

    *old = NULL;
    if (atomic_load(&priv->stopping)) {
        *why = "the manager is shutting down";
        return -ESHUTDOWN;
    }
    if (!node || node_id == tcp_local_node(priv)) {
        *why = "no worker in the mesh has that id";
        return -ENOENT;
    }
    if (strcmp(node->hostname, host) != 0 || node->port != port) {
        *why = "the node registered from another address";
        return -EACCES;
    }
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id));
    if (conn && !tcp_conn_dead(conn) && node->is_alive) {
        *why = "the node is still connected";
        return -EBUSY;
    }
    if (conn) {
        tcp_conn_table_take(priv, node_id, conn);
        *old = conn;
    }
    return 0;
}

/*
 * Make room in a full mesh_nodes for a node registering for the first time,
 * by evicting the node that has been dead longest.  The broadcasts carry at
 * most TCP_MAX_MESH_NODES nodes, and a table past that leaves nodes out of
 * them, live ones as likely as dead (tcp_report_mesh_overflow()), so if no
 * node can be evicted, the caller refuses the new one.
 *
 * Only a dead node with no connection in connections, or one whose
 * connection has ended, is evicted.  A node declared dead whose connection
 * is still up may yet answer (tcp_node_still_silent()), and if it doesn't,
 * the health check takes the connection out first.  The connection can
 * leave the table before its receive thread is joined, so a message from
 * the node can still be handled after its eviction; the manager takes a
 * node's advertisement only while the node is in mesh_nodes
 * (tcp_gid_claims_set_if_member()), so one can't put back the claims its
 * eviction dropped.  The manager is never evicted.  The id isn't given out
 * again, since next_available_node_id only goes up, so a late message
 * naming it can't be taken for a new node's.
 *
 * Nothing is evicted once teardown has begun, as tcp_register_again() takes
 * nothing then, and for the same reason: tcp_stop_recv_threads() may have
 * claimed the node's connection's thread, keeping only a pointer to join it
 * through, and retiring the connection here would drop the table's
 * reference and free it under the join.  stopping is set before that claim
 * and read here under conn_table_lock, which the claim is made under.
 *
 * Returns the evicted node's id, or UINT32_MAX if mesh_nodes has room,
 * there is no node to evict, or teardown has begun.  An ended connection
 * the node still has in connections is taken out into *old, for the caller
 * to retire once it has dropped the locks.  The node's claims are the
 * caller's to forget, under mesh_table_lock (tcp_gid_claims_forget()).  The
 * caller holds mesh_table_lock and conn_table_lock.
 */
static uint32_t tcp_evict_dead_node(TcpBackendPrivate *priv,
                                    TcpConnection **old)
    TSA_REQUIRES(priv->mesh_table_lock, priv->conn_table_lock)
{
    GHashTableIter iter;
    gpointer value;
    uint32_t victim = UINT32_MAX;
    uint64_t victim_age = 0;
    /* Read under the lock, so no heartbeat is newer */
    uint64_t now_ms = ernic_monotonic_ms();
    TcpConnection *conn;

    *old = NULL;
    if (atomic_load(&priv->stopping) ||
        g_hash_table_size(priv->mesh_nodes) < TCP_MAX_MESH_NODES)
        return UINT32_MAX;

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const MeshNodeInfo *node = value;
        /* Unsigned, so it's right even if the clock was near zero */
        uint64_t age = now_ms - node->last_heartbeat_ms;

        if (node->is_alive || node->node_id == tcp_local_node(priv))
            continue;
        conn = g_hash_table_lookup(priv->connections,
                                   GUINT_TO_POINTER(node->node_id));
        if (conn && !tcp_conn_dead(conn))
            continue;
        /* The lower id, which registered first, breaks a tie */
        if (victim == UINT32_MAX || age > victim_age ||
            (age == victim_age && node->node_id < victim)) {
            victim = node->node_id;
            victim_age = age;
        }
    }
    if (victim == UINT32_MAX)
        return UINT32_MAX;

    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(victim));
    if (conn) {
        tcp_conn_table_take(priv, victim, conn);
        *old = conn;
    }
    g_hash_table_remove(priv->mesh_nodes, GUINT_TO_POINTER(victim));
    return victim;
}

/*
 * Collect the nodes whose connection has gone, so the caller can declare
 * them dead, withdrawing what they claimed, once it has dropped
 * mesh_table_lock (tcp_declare_dead_if_unreachable()).
 *
 * A worker that restarts registers again as a brand new node -- it asks for
 * an id of 0xFFFFFFFF, as on any first registration, and the address it
 * registers from is no help either, since its listening port is ephemeral
 * and changes with it.  (Only a worker that kept running asks for its own id
 * back.)  So rather than try to recognise it, drop the claims of whoever can
 * no longer be reached: a node with no live connection cannot be the one
 * answering for an address, whether or not it is the same guest coming
 * back. Left standing, its claims contest the ones the returning worker is
 * about to make, and a contested GID resolves to neither -- so the guest
 * would come back unreachable, and stay that way.
 *
 * Caller holds mesh_table_lock; tcp_connection_lookup() takes
 * conn_table_lock under it, which is the documented order.
 */
static uint32_t tcp_collect_dead_claimants(TcpBackendPrivate *priv,
                                           uint32_t out[TCP_MAX_MESH_NODES])
    TSA_REQUIRES(priv->mesh_table_lock)
{
    GHashTableIter iter;
    gpointer value;
    uint32_t n = 0;

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value) &&
           n < TCP_MAX_MESH_NODES) {
        const MeshNodeInfo *node = value;
        uint32_t node_id = node->node_id;
        TcpConnection *conn;

        if (node_id == tcp_local_node(priv))
            continue;

        conn = tcp_connection_lookup(priv, node_id);
        if (!conn || tcp_conn_dead(conn))
            out[n++] = node_id;
        if (conn)
            tcp_connection_unref(conn);
    }
    return n;
}

/*
 * Declare @node_id dead if it is still unreachable, which takes its claims
 * out of resolution, and say whether it was alive until now; if it was, the
 * caller asks the broadcaster to tell the workers (tcp_broadcast_node()),
 * once it may: a registration declares before it answers, and asks after.
 * Returns false when the node has a live connection again, leaving it alone,
 * when it was dead already, or when it has been evicted meanwhile.
 *
 * This is what the health check would decide on its next pass: a node with
 * no live connection can't be answering.  Deciding it here, as a node
 * registers, lets that node's claims take the GIDs at once if it is the
 * same guest come back under a new id.
 *
 * The re-check is the point.  tcp_collect_dead_claimants() runs under
 * mesh_table_lock and the caller then drops it, and in that window one of
 * the nodes it collected can register again under its own id: that
 * publishes a connection and declares the node alive (the REGISTER_NODE
 * handler, once tcp_register_again() has let it).  Declaring it dead on the
 * strength of the stale list would undo the revival, and the mesh would be
 * told the node is dead and owns nothing until it next answered a
 * heartbeat.  So the connection is looked at again here, under the lock a
 * registration publishes beneath, which makes the two atomic with respect
 * to each other.
 *
 * Lock order: conn_table_lock and gid_table_lock under mesh_table_lock (see
 * Locks in rdma_backend_tcp_priv.h), as in tcp_health_check_scan().
 */
bool tcp_declare_dead_if_unreachable(TcpBackendPrivate *priv, uint32_t node_id)
{
    MeshNodeInfo *node;
    TcpConnection *conn;
    bool declared = false, gids = false;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(node_id));
    conn = tcp_connection_lookup(priv, node_id);
    if (node && (!conn || tcp_conn_dead(conn)))
        declared = tcp_node_set_alive(priv, node, false, &gids);
    if (conn)
        tcp_connection_unref(conn);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (gids)
        tcp_gid_claims_report_alive(node_id, false);
    return declared;
}

/*
 * Answer a registration on conn by refusing it, with result, a negative
 * errno.  Under conn->lock, like every write to a connection's socket.  The
 * caller then stops reading conn, since nothing the peer sends now is of
 * use; the connection is still pending, and the next reap closes it.
 */
static void tcp_refuse_registration(TcpBackendPrivate *priv,
                                    TcpConnection *conn, uint32_t dst,
                                    int32_t result)
{
    uint8_t nak[TCP_REGISTER_RESP_LEN];

    memset(nak, 0, sizeof(nak));
    ernic_put_be32(nak + TCP_REGISTER_RESP_RESULT_OFF, (uint32_t)result);
    ernic_put_be32(nak + TCP_REGISTER_RESP_VERSION_OFF, TCP_PROTOCOL_VERSION);

    qemu_mutex_lock(&conn->lock);
    tcp_send_message(
        priv, conn, TCP_MSG_REGISTER_RESP, nak, sizeof(nak),
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
        tcp_local_node(priv), dst, 0, 0);
    qemu_mutex_unlock(&conn->lock);
}

/* A registration, as the manager reads it off the wire */
typedef struct {
    /* The worker's own id, to have it back, or 0xFFFFFFFF for a new one */
    uint32_t requested_id;
    char host[256];
    uint16_t port;
} TcpRegistration;

/*
 * What admitting a registration did (tcp_register_admit()), for
 * tcp_manager_recv_register() to finish once it has dropped the locks.
 */
typedef struct {
    uint32_t node_id; /* the id the node registers under */
    bool again;       /* it asked for its own id back */
    /* A refusal's negative errno, and why, or 0 if it wasn't refused */
    int refused;
    const char *why;
    /* The node's old connection, taken out of connections to be retired */
    TcpConnection *replaced;
    /* A dead node evicted to make room, or UINT32_MAX, and its connection */
    uint32_t evicted;
    TcpConnection *evicted_conn;
    bool gids_back;     /* a node registering again got its claims back */
    uint32_t num_nodes; /* the size of the mesh, for the answer */
    /*
     * Nodes with no live connection: collected when the node is admitted,
     * then, once tcp_register_declare_stale() has run, just those it
     * declared dead, for the mesh to be told once the answer has gone
     */
    uint32_t stale[TCP_MAX_MESH_NODES];
    uint32_t num_stale;
} TcpAdmission;

/*
 * Read a REGISTER_NODE's payload, of at least TCP_REGISTER_NODE_LEN_V3
 * bytes, into *reg.  Returns false, having logged why, if the peer doesn't
 * speak this protocol version, for the caller to refuse it.  A short payload
 * is one that predates the version field entirely; both cases get the same
 * answer, with a result the worker surfaces instead of timing out.
 */
static bool tcp_register_read(const TcpRecvMsg *msg, TcpRegistration *reg)
{
    const uint8_t *payload = msg->payload;
    const char *hostname;
    uint32_t peer_version = 0;
    size_t host_len;

    if (msg->hdr.msg_len >= TCP_REGISTER_NODE_LEN) {
        peer_version = ernic_get_be32(payload + TCP_REGISTER_NODE_VERSION_OFF);
    }

    if (peer_version != TCP_PROTOCOL_VERSION) {
        rdma_error_report("TCP: refusing registration from a v%u peer "
                          "(this mesh is v%u); a peer that predates the GID "
                          "advertisement routes every destination to one "
                          "default node",
                          peer_version, TCP_PROTOCOL_VERSION);
        return false;
    }

    reg->requested_id =
        ernic_get_be32(payload + TCP_REGISTER_NODE_REQUESTED_OFF);
    reg->port = ernic_get_be16(payload + TCP_REGISTER_NODE_PORT_OFF);
    /* The hostname comes off the wire and need not be terminated, so bound
     * the read to the field. */
    hostname = (const char *)payload + TCP_REGISTER_NODE_HOSTNAME_OFF;
    host_len = strnlen(hostname, sizeof(reg->host) - 1);
    memcpy(reg->host, hostname, host_len);
    reg->host[host_len] = '\0';
    return true;
}

/*
 * Make a node for a registration that asks for a new id, under the next id
 * no node or connection has, evicting a dead node if the mesh is full.
 * Returns whether it did, with the id in adm->node_id.  If not, adm->refused
 * is the negative errno to refuse with, and adm->why says why, or 0 if no
 * node could be made, which is logged here.  The caller holds
 * mesh_table_lock and conn_table_lock.
 */
static bool tcp_register_new_node(TcpBackendPrivate *priv,
                                  const TcpRegistration *reg, TcpAdmission *adm)
    TSA_REQUIRES(priv->mesh_table_lock, priv->conn_table_lock)
{
    uint32_t assigned_id = priv->next_available_node_id;
    MeshNodeInfo *node;

    while (tcp_node_id_taken(priv, assigned_id)) {
        assigned_id++;
    }

    node = tcp_mesh_node_info_new(assigned_id, reg->host, reg->port);
    if (!node) {
        rdma_error_report("TCP: Failed to create mesh node info for worker "
                          "node %u",
                          assigned_id);
        return false;
    }
    /* After the id is chosen, so it can't be the evicted one */
    adm->evicted = tcp_evict_dead_node(priv, &adm->evicted_conn);
    /*
     * The broadcasts carry no more nodes than the cap, so a node past it
     * would leave others out of the mesh.  The id isn't used up, since
     * nothing was given it.
     */
    if (adm->evicted == UINT32_MAX &&
        g_hash_table_size(priv->mesh_nodes) >= TCP_MAX_MESH_NODES) {
        tcp_mesh_node_info_free(node);
        adm->why = "the mesh is full, and has no dead node that can be "
                   "evicted";
        adm->refused = -ENOSPC;
        return false;
    }
    priv->next_available_node_id = assigned_id + 1;
    g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(assigned_id), node);
    adm->node_id = assigned_id;
    return true;
}

/*
 * The part of a registration on conn that is done under mesh_table_lock and
 * conn_table_lock: give the node its id, and move conn, with its reference,
 * from pending_conns to connections.  A new node gets the next id no node or
 * connection has.  A worker that asks for an id is asking for its own back,
 * having connected again (tcp_register_again()); it gets that or nothing,
 * never another, which would leave it claiming its GIDs under two ids.
 *
 * Returns whether conn was registered, with *adm filled in for
 * tcp_manager_recv_register() to finish.  If not, conn is still pending, so
 * the reap closes it, and adm->refused is the negative errno to refuse it
 * with, or 0 if it gets no answer.
 */
static bool tcp_register_admit(TcpBackendPrivate *priv, TcpConnection *conn,
                               const TcpRegistration *reg, TcpAdmission *adm)
{
    bool pending, admitted = false;

    memset(adm, 0, sizeof(*adm));
    adm->node_id = reg->requested_id;
    adm->again = reg->requested_id != 0xFFFFFFFF;
    adm->evicted = UINT32_MAX;

    qemu_mutex_lock(&priv->mesh_table_lock);

    /*
     * Nodes that can no longer be reached, declared dead, which withdraws
     * their GIDs, once the tables are unlocked and before the answer goes
     * (tcp_register_declare_stale()). A worker that restarted is among them,
     * under the id it had before, and it is about to claim those GIDs again
     * under its new one.
     *
     * Before conn_table_lock, not under it: the lookups inside take that
     * lock themselves.
     */
    adm->num_stale = tcp_collect_dead_claimants(priv, adm->stale);

    qemu_mutex_lock(&priv->conn_table_lock);

    /*
     * Only a connection on the pending list can register, and only once:
     * taking it off the list claims the registration.  A second
     * REGISTER_NODE on the same socket would otherwise put the connection in
     * the table under a second id, and teardown would free it once for each.
     */
    pending = g_ptr_array_remove_fast(priv->pending_conns, conn);
    if (pending && adm->again) {
        adm->refused = tcp_register_again(priv, reg->requested_id, reg->host,
                                          reg->port, &adm->replaced, &adm->why);
        admitted = adm->refused == 0;
    } else if (pending) {
        admitted = tcp_register_new_node(priv, reg, adm);
    }

    if (admitted) {
        /*
         * The connection takes its reference from pending_conns with it into
         * connections, and can't be refused: the id has no entry there, or
         * had one that tcp_register_again() just took out.
         */
        conn->node_id = adm->node_id;
        tcp_conn_table_add(priv, adm->node_id, conn);
    } else if (pending) {
        /* Still unregistered, so the reap closes it, or teardown finds it */
        g_ptr_array_add(priv->pending_conns, conn);
    }
    qemu_mutex_unlock(&priv->conn_table_lock);

    if (admitted && adm->again) {
        MeshNodeInfo *node = g_hash_table_lookup(
            priv->mesh_nodes, GUINT_TO_POINTER(adm->node_id));

        node->last_heartbeat_ms = ernic_monotonic_ms();
        /*
         * The node's addresses are its own again, without its advertising
         * them: a node the health check declared dead had its claims taken
         * out of resolution, and the broadcast that follows would otherwise
         * tell the workers it owns nothing until its advertisement came.
         * Under mesh_table_lock, with the connection published, so
         * tcp_declare_dead_if_unreachable() can't act on a stale finding of
         * it dead in between.
         */
        tcp_node_set_alive(priv, node, true, &adm->gids_back);
    }
    if (admitted) {
        /*
         * Under mesh_table_lock, so the node's claims go with its entry, as
         * far as anything that takes the lock can tell.
         */
        if (adm->evicted != UINT32_MAX)
            tcp_gid_claims_forget(priv, adm->evicted);
        /* For the answer, read under the lock that guards the table */
        adm->num_nodes = g_hash_table_size(priv->mesh_nodes);
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (!pending) {
        rdma_error_report("TCP: ignoring REGISTER_NODE on node %u's "
                          "connection, which is not waiting to register",
                          conn->node_id);
    }
    return admitted;
}

/*
 * Declare dead the nodes a registration admitted as adm->node_id found
 * unreachable, keeping in adm->stale just those it declared, for
 * tcp_register_tell_mesh() to tell the mesh about.
 *
 * Before the answer, not after: the answer is what lets the worker claim its
 * GIDs, and a worker that restarted claims the ones its old id still holds.
 * Declared after, the old id's claims would still resolve, to a node that
 * isn't there, for anyone who looked between the answer and the declaration
 * (the restart in test_gsi_mesh).
 *
 * Nodes that can't be reached are declared dead, keeping their claims,
 * rather than discarded: that is enough to take a node out of resolution, so
 * it stops contesting the GIDs the registering node is about to claim, and
 * it leaves the claims to restore if the node answers a heartbeat again, or
 * registers again under its own id, rather than having restarted.  The new
 * node's claims come in its advertisement, which its receive thread reads
 * only after this.
 */
static void tcp_register_declare_stale(TcpBackendPrivate *priv,
                                       TcpAdmission *adm)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < adm->num_stale; i++) {
        uint32_t id = adm->stale[i];

        /* An evicted node's claims are gone already */
        if (id == adm->node_id || id == adm->evicted)
            continue;
        if (tcp_declare_dead_if_unreachable(priv, id))
            adm->stale[n++] = id;
    }
    adm->num_stale = n;
}

/*
 * Answer, on conn, a registration admitted as adm->node_id.
 */
static void tcp_register_answer(TcpBackendPrivate *priv, TcpConnection *conn,
                                const TcpAdmission *adm)
{
    /* Zeroed first: the whole payload goes on the wire, so any field not
     * assigned below would put stack contents there. */
    uint8_t resp[TCP_REGISTER_RESP_LEN];

    memset(resp, 0, sizeof(resp));
    ernic_put_be32(resp + TCP_REGISTER_RESP_NODE_ID_OFF, adm->node_id);
    ernic_put_be32(resp + TCP_REGISTER_RESP_NUM_NODES_OFF, adm->num_nodes);
    ernic_put_be32(resp + TCP_REGISTER_RESP_RESULT_OFF, 0); /* Success */
    ernic_put_be32(resp + TCP_REGISTER_RESP_VERSION_OFF, TCP_PROTOCOL_VERSION);

    /*
     * conn->lock serialises writes to this socket.  The send thread holds it
     * across the data path's multi-megabyte messages, so an unlocked control
     * message here would interleave into the middle of one and the peer
     * would read a header from the payload.
     */
    qemu_mutex_lock(&conn->lock);
    tcp_send_message(
        priv, conn, TCP_MSG_REGISTER_RESP, resp, sizeof(resp),
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
        tcp_local_node(priv), adm->node_id, 0, 0);
    qemu_mutex_unlock(&conn->lock);
}

/*
 * Have the broadcaster tell the mesh what a registration admitted as
 * adm->node_id changed.  Called only once the answer has gone: a worker
 * ignores what the manager sends it before the answer, and a pass could
 * otherwise reach the new connection, which is in the table already, before
 * the answer did.
 */
static void tcp_register_tell_mesh(TcpBackendPrivate *priv,
                                   const TcpAdmission *adm)
{
    /* The nodes tcp_register_declare_stale() declared dead */
    for (uint32_t i = 0; i < adm->num_stale; i++) {
        rdma_info_report("TCP: node %u is gone; declared it dead as node %u "
                         "registers",
                         adm->stale[i], adm->node_id);
        tcp_broadcast_node(priv, adm->stale[i]);
    }

    /*
     * The updated topology, and the whole GID table to the new node, which
     * missed every advertisement sent before it registered.  Its own GIDs
     * follow once it sends them.  A node that registered again owns its old
     * ones again, which the others hear too.
     *
     * An evicted node is no longer in the table, so it gets an empty set of
     * its own.  The workers were told it owned nothing when it was declared
     * dead, unless it was evicted before that broadcast took its snapshot.
     */
    tcp_broadcast_topology(priv);
    tcp_broadcast_gid_table_to(priv, adm->node_id);
    if (adm->gids_back)
        tcp_broadcast_node_gids(priv, adm->node_id);
    if (adm->evicted != UINT32_MAX)
        tcp_broadcast_node_gids(priv, adm->evicted);
}

/*
 * TCP_MSG_REGISTER_NODE: a worker registers on conn, as a new node or, having
 * connected again, under its own id.  Returns whether to hang up, as for a
 * refused registration: nothing the peer sends now is of use, and the
 * connection is still pending, so the next reap closes it.
 */
bool tcp_manager_recv_register(TcpConnection *conn, const TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;
    TcpRegistration reg;
    TcpAdmission adm;

    if (!priv || !priv->is_manager) {
        rdma_error_report("TCP: REGISTER_NODE received by non-manager");
        return false;
    }

    if (!msg->payload || msg->hdr.msg_len < TCP_REGISTER_NODE_LEN_V3) {
        rdma_error_report("TCP: Invalid REGISTER_NODE payload");
        return false;
    }

    if (!tcp_register_read(msg, &reg)) {
        tcp_refuse_registration(priv, conn, msg->hdr.src_node_id, -EPROTO);
        return true;
    }

    if (!tcp_register_admit(priv, conn, &reg, &adm)) {
        if (adm.refused == 0) {
            return false;
        }
        if (adm.again) {
            rdma_error_report("TCP: refusing a registration from %s:%u as "
                              "node %u: %s",
                              reg.host, reg.port, reg.requested_id, adm.why);
        } else {
            rdma_error_report("TCP: refusing a registration from %s:%u as a "
                              "new node: %s",
                              reg.host, reg.port, adm.why);
        }
        tcp_refuse_registration(priv, conn, msg->hdr.src_node_id, adm.refused);
        return true;
    }

    if (adm.replaced) {
        tcp_retire_conn(adm.replaced);
        tcp_connection_unref(adm.replaced);
    }
    if (adm.evicted_conn) {
        tcp_retire_conn(adm.evicted_conn);
        tcp_connection_unref(adm.evicted_conn);
    }
    if (adm.evicted != UINT32_MAX)
        rdma_info_report("TCP: the mesh is full; evicted node %u, the node "
                         "dead longest, to make room for node %u",
                         adm.evicted, adm.node_id);
    if (adm.again) {
        rdma_info_report("TCP: Node %u registered again from %s:%u",
                         adm.node_id, reg.host, reg.port);
        if (adm.gids_back)
            tcp_gid_claims_report_alive(adm.node_id, true);
    } else {
        rdma_info_report("TCP: Registered node %u at %s:%u", adm.node_id,
                         reg.host, reg.port);
    }

    tcp_register_declare_stale(priv, &adm);
    tcp_register_answer(priv, conn, &adm);
    tcp_register_tell_mesh(priv, &adm);
    return false;
}

/*
 * TCP_MSG_HEARTBEAT on the manager: a worker's answer to the manager's
 * heartbeat, or a worker's own, when it has heard nothing from the manager
 * for an interval, which the manager answers: the answer is what tells the
 * worker the manager is still there.  Either refreshes the worker's liveness
 * timestamp, and revives it if the health check had declared it dead.
 *
 * The worker is the one this connection belongs to, not the one the header
 * names: the sender writes src_node_id, and trusting it would let any node
 * keep another, dead, node alive.  Every connection that gets this far is a
 * registered node's, and carries that node's id.
 */
void tcp_manager_recv_heartbeat(TcpConnection *conn)
{
    TcpBackendPrivate *priv = conn->priv;
    bool revived = false, gids_back = false;
    MeshNodeInfo *node;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node =
        g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(conn->node_id));
    if (node) {
        node->last_heartbeat_ms = ernic_monotonic_ms();
        revived = tcp_node_set_alive(priv, node, true, &gids_back);
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);

    /*
     * Without waiting for the connection's lock: this thread waiting for
     * another to write to the node would stop reading from it, and a node
     * doing the same would leave both sockets full, until a send timed out.
     */
    tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT_RESP, conn->node_id);

    /*
     * The rest of the mesh was told the node is dead, and won't connect to
     * it until it hears otherwise.
     */
    if (revived) {
        rdma_info_report("TCP: Node %u is answering again", conn->node_id);
        if (gids_back)
            tcp_gid_claims_report_alive(conn->node_id, true);
        tcp_broadcast_node(priv, conn->node_id);
    }
}

/*
 * Manager: relay a TCP_MSG_IONIC from one worker to another, the one it is
 * addressed to, which the sender has no connection of its own to.
 */
void tcp_manager_relay_ionic(TcpBackendPrivate *priv, const TcpRecvMsg *msg)
{
    const TcpMsgHeader *hdr = &msg->hdr;
    TcpConnection *fwd = tcp_connection_lookup(priv, hdr->dst_node_id);
    bool relayed = false;

    if (fwd && atomic_load(&fwd->is_connected)) {
        qemu_mutex_lock(&fwd->lock);
        /*
         * On the sender's receive thread, so while the send waits that node
         * isn't read.  The manager's shorter timeout (tcp_send_timeout_ms())
         * is what keeps a destination that has stopped reading from costing
         * the sender its connection to the manager.
         */
        relayed =
            fwd->sockfd >= 0 &&
            tcp_send_message(priv, fwd, TCP_MSG_IONIC, msg->payload,
                             hdr->msg_len, hdr->seq, hdr->src_node_id,
                             hdr->dst_node_id, hdr->src_qpn, hdr->dst_qpn) == 0;
        qemu_mutex_unlock(&fwd->lock);
    }
    tcp_connection_unref(fwd);
    if (!relayed) {
        rdma_error_report("TCP: cannot relay ionic message from node %u to "
                          "node %u",
                          hdr->src_node_id, hdr->dst_node_id);
    }
}

/*
 * Free the manager's pending connections that are done with: those whose
 * receive thread has exited, because the peer hung up or was refused, and
 * those that haven't registered within TCP_REGISTER_TIMEOUT_MS of being
 * accepted.  Otherwise each would hold a descriptor and a thread until
 * teardown, and a port probe, or a peer that connects and says nothing,
 * would leave one behind every time.
 *
 * A connection is claimed by taking it off pending_conns under
 * conn_table_lock, which also keeps it from registering, so the accept and
 * health-check threads, which both call this, never reap the same one.  It
 * is retired, which stops and joins its thread, and the list's reference is
 * dropped, without the lock, which the thread may be waiting for.
 */
void tcp_reap_pending_conns(TcpBackendPrivate *priv)
{
    GPtrArray *reaped = g_ptr_array_new();

    qemu_mutex_lock(&priv->conn_table_lock);
    /* Read under the lock, so no connection on the list is newer */
    uint64_t now_ms = ernic_monotonic_ms();

    for (guint i = 0; i < priv->pending_conns->len;) {
        TcpConnection *conn = g_ptr_array_index(priv->pending_conns, i);

        if (tcp_conn_dead(conn) ||
            (now_ms >= conn->accepted_ms &&
             now_ms - conn->accepted_ms >= TCP_REGISTER_TIMEOUT_MS)) {
            /* Moves the last connection to i, which is looked at next */
            g_ptr_array_remove_index_fast(priv->pending_conns, i);
            g_ptr_array_add(reaped, conn);
        } else {
            i++;
        }
    }
    qemu_mutex_unlock(&priv->conn_table_lock);

    for (guint i = 0; i < reaped->len; i++) {
        TcpConnection *conn = g_ptr_array_index(reaped, i);
        char *addr = tcp_addr_str(conn->remote_host, conn->remote_port);

        rdma_info_report("TCP: Closing the connection from %s, which didn't "
                         "register",
                         addr);
        g_free(addr);
        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
    }
    g_ptr_array_free(reaped, TRUE);
}

/*
 * Manager: take a connection the accept thread has accepted, on sockfd, from
 * host:port.  Its receive thread handles the REGISTER_NODE that moves it to
 * connections; until then it waits on pending_conns, with the node id
 * unknown.  Closed instead if TCP_MAX_PENDING_CONNS are waiting already.
 */
void tcp_manager_accept(TcpBackendPrivate *priv, int sockfd, const char *host,
                        uint16_t port)
{
    TcpConnection *conn;
    char *addr;

    /* Make room before counting what's waiting */
    tcp_reap_pending_conns(priv);

    conn = tcp_connection_new(priv, UINT32_MAX, host, port, sockfd);
    conn->accepted_ms = ernic_monotonic_ms();

    /*
     * Start receive thread - it will handle REGISTER_NODE, which moves the
     * connection to the connections table.  The lock is held until the
     * connection is on the pending list, so the move can't happen before
     * it's there.
     */
    qemu_mutex_lock(&priv->conn_table_lock);
    if (priv->pending_conns->len >= TCP_MAX_PENDING_CONNS) {
        qemu_mutex_unlock(&priv->conn_table_lock);
        addr = tcp_addr_str(host, port);
        rdma_error_report("TCP: %u connections are already waiting to "
                          "register; closing the one from %s",
                          TCP_MAX_PENDING_CONNS, addr);
        g_free(addr);
        tcp_connection_unref(conn);
        return;
    }
    if (!tcp_start_recv_thread(conn, "tcp-recv-pending")) {
        qemu_mutex_unlock(&priv->conn_table_lock);
        tcp_connection_unref(conn);
        return;
    }
    g_ptr_array_add(priv->pending_conns, conn);
    qemu_mutex_unlock(&priv->conn_table_lock);
}

/*
 * A broadcast carries at most TCP_MAX_MESH_NODES nodes.  Registration keeps
 * mesh_nodes within that, evicting a dead node or refusing the new one, so a
 * table past it is a bug.  The nodes past the cap -- live nodes as likely as
 * dead ones, in hash-table order -- drop out of the topology and the GID
 * table, which breaks the mesh with no other sign, so say so.  @table_size is
 * the size of mesh_nodes the broadcast found.
 */
static void tcp_report_mesh_overflow(const char *broadcast, guint table_size)
{
    rdma_error_report("TCP: the mesh table holds %u nodes, but the %s carries "
                      "at most %u; %u nodes were left out",
                      table_size, broadcast, TCP_MAX_MESH_NODES,
                      table_size - TCP_MAX_MESH_NODES);
}

/*
 * Manager: send the mesh topology to every connection.  Called only by a
 * broadcast pass.
 */
static void tcp_send_mesh_topology(TcpBackendPrivate *priv)
{
    /* The whole payload goes on the wire, including the node slots past
     * num_nodes, so it MUST be zeroed to avoid sending stack contents when
     * there are unused nodes.
     */
    uint8_t topo[TCP_TOPOLOGY_LEN];
    memset(topo, 0, sizeof(topo));
    uint32_t num_nodes = 0;
    guint overflow = 0; /* the size of mesh_nodes, if past the cap */

    qemu_mutex_lock(&priv->mesh_table_lock);

    /* Build topology payload */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->mesh_nodes);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (num_nodes == TCP_MAX_MESH_NODES) {
            overflow = g_hash_table_size(priv->mesh_nodes);
            break;
        }

        const MeshNodeInfo *node = value;
        uint8_t *entry = topo + TCP_TOPOLOGY_NODE_OFF(num_nodes);

        ernic_put_be32(entry + TCP_MESH_NODE_ID_OFF, node->node_id);
        /* Terminated within the field, by the zeroing above */
        memcpy(entry + TCP_MESH_NODE_HOSTNAME_OFF, node->hostname,
               strnlen(node->hostname, TCP_HOSTNAME_LEN - 1));
        ernic_put_be16(entry + TCP_MESH_NODE_PORT_OFF, node->port);
        entry[TCP_MESH_NODE_ALIVE_OFF] = node->is_alive ? 1 : 0;
        num_nodes++;
    }

    ernic_put_be32(topo + TCP_TOPOLOGY_NUM_NODES_OFF, num_nodes);

    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (overflow) {
        tcp_report_mesh_overflow("topology broadcast", overflow);
    }

    /* Broadcast to all connected workers */
    GPtrArray *conns = tcp_conns_snapshot(priv);

    for (guint i = 0; i < conns->len; i++) {
        TcpConnection *conn = g_ptr_array_index(conns, i);

        /* Checked under the lock tcp_retire_conn() changes them under */
        qemu_mutex_lock(&conn->lock);
        if (atomic_load(&conn->is_connected) && conn->sockfd >= 0) {
            tcp_send_message(priv, conn, TCP_MSG_MESH_TOPOLOGY, topo,
                             sizeof(topo),
                             atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                       memory_order_relaxed),
                             tcp_local_node(priv), conn->node_id, 0, 0);
        }
        qemu_mutex_unlock(&conn->lock);
    }
    g_ptr_array_free(conns, TRUE);

    rdma_info_report("TCP: Broadcast mesh topology to %u nodes", num_nodes);
}

/*
 * Manager: put @node_id's GID set, as the manager advertises it, in @out.  A
 * node that isn't in mesh_nodes, having been evicted, or that is dead, is
 * advertised as owning nothing.  Workers keep no liveness of their own --
 * they take the manager's word for who owns what -- so this is how they stop
 * resolving a node that is away, and how they stop seeing its claims contest
 * those of the same guest returning under a new node id.  The caller holds
 * mesh_table_lock and gid_table_lock.
 */
static void tcp_snapshot_node_gids(const TcpBackendPrivate *priv,
                                   uint32_t node_id, TcpNodeGidsWire *out)
    TSA_REQUIRES(priv->mesh_table_lock, priv->gid_table_lock)
{
    const TcpNodeGids *claims = NULL;

    if (g_hash_table_contains(priv->mesh_nodes, GUINT_TO_POINTER(node_id)))
        claims =
            g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(node_id));
    if (claims && claims->alive)
        tcp_fill_node_gids(out, node_id, claims->gids, claims->num_gids);
    else
        tcp_fill_node_gids(out, node_id, NULL, 0);
}

/*
 * Manager: send the GID sets a broadcast pass took (tcp_broadcast_pass()).
 *
 * The connection of each node in @tables gets the whole table: the set of
 * every node in mesh_nodes, one message per node, including the nodes that
 * own nothing.  That is how a node that has just registered learns what was
 * advertised before it did.
 *
 * Then every connection gets the set of each node in @nodes, those whose
 * sets may have changed: a node whose claims were withdrawn, or that died or
 * was evicted, is sent as owning nothing, or the workers would keep
 * resolving its old GIDs.  Every connection includes the node the set is
 * about: that is how a claimant learns that the manager has it as owning
 * nothing, which it cannot work out alone.
 *
 * Each set changes on its own -- the workers derive who owns a contested GID
 * from the claims, so one node's set is never the answer to a change in
 * another's -- so a change costs one message to each connection, not one for
 * each node.
 */
static void tcp_send_gid_updates(TcpBackendPrivate *priv, const GArray *tables,
                                 const GArray *nodes)
{
    if (!tcp_has_gid_tables(priv) || !tcp_has_mesh_table(priv))
        return;

    /*
     * The whole table goes on the stack, in wire form, so that a slot
     * tcp_fill_node_gids() didn't zero would show up in the tests as stack
     * contents on the wire.
     */
    TcpNodeGidsWire table[TCP_MAX_MESH_NODES];
    uint32_t num_table = 0;
    guint overflow = 0; /* the size of mesh_nodes, if past the cap */
    TcpNodeGidsWire *changed = g_new(TcpNodeGidsWire, nodes->len);

    /*
     * Snapshot under the mesh and claim locks; the sends are not, because
     * tcp_send_node_gids() takes connection locks, which a sender holds
     * while it waits for a peer to read.
     */
    qemu_mutex_lock(&priv->mesh_table_lock);
    qemu_mutex_lock(&priv->gid_table_lock);

    if (tables->len > 0) {
        GHashTableIter iter;
        gpointer key;

        g_hash_table_iter_init(&iter, priv->mesh_nodes);
        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            if (num_table == G_N_ELEMENTS(table)) {
                overflow = g_hash_table_size(priv->mesh_nodes);
                break;
            }
            tcp_snapshot_node_gids(priv, GPOINTER_TO_UINT(key),
                                   &table[num_table++]);
        }
    }
    for (guint i = 0; i < nodes->len; i++)
        tcp_snapshot_node_gids(priv, g_array_index(nodes, uint32_t, i),
                               &changed[i]);

    qemu_mutex_unlock(&priv->gid_table_lock);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (overflow)
        tcp_report_mesh_overflow("GID table", overflow);

    for (guint i = 0; i < tables->len; i++) {
        TcpConnection *conn =
            tcp_connection_lookup(priv, g_array_index(tables, uint32_t, i));

        for (uint32_t n = 0; n < num_table; n++)
            tcp_send_node_gids(priv, conn, &table[n]);
        tcp_connection_unref(conn);
    }

    if (nodes->len > 0) {
        GPtrArray *conns = tcp_conns_snapshot(priv);

        for (guint i = 0; i < nodes->len; i++)
            for (guint c = 0; c < conns->len; c++)
                tcp_send_node_gids(priv, g_ptr_array_index(conns, c),
                                   &changed[i]);
        g_ptr_array_free(conns, TRUE);
    }
    g_free(changed);
}

/*
 * Set up the broadcaster's state, but not its thread, which a manager starts
 * once it has set up the rest; see broadcast_thread.
 */
void tcp_broadcaster_init(TcpBackendPrivate *priv)
{
    atomic_init(&priv->broadcast_running, false);
    qemu_mutex_init(&priv->broadcast_lock);
    qemu_cond_init(&priv->broadcast_cond);
    /* No other thread can reach priv yet */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->broadcast_topology = false;
    priv->broadcast_gids = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->broadcast_tables = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->broadcasts_asked = 0;
    priv->broadcasts_sent = 0;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
}

/* Free what tcp_broadcaster_init() set up; the thread must be stopped */
void tcp_broadcaster_destroy(TcpBackendPrivate *priv)
{
    /* Nor can any other thread reach priv now */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->broadcast_tables);
    g_hash_table_destroy(priv->broadcast_gids);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    qemu_cond_destroy(&priv->broadcast_cond);
    qemu_mutex_destroy(&priv->broadcast_lock);
}

/*
 * Manager: ask the broadcaster to send the topology to every connection if
 * @topology, @gids_of's GID set to every connection, and the whole GID table
 * to @table_to's connection, either of which can be UINT32_MAX for none.
 * Returns at once; the broadcaster's next pass sends them.  A worker has
 * nothing to broadcast, so on a worker this does nothing.
 */
static void tcp_broadcast_ask(TcpBackendPrivate *priv, bool topology,
                              uint32_t gids_of, uint32_t table_to)
    TSA_EXCLUDES(priv->broadcast_lock)
{
    if (!priv->is_manager)
        return;

    qemu_mutex_lock(&priv->broadcast_lock);
    if (topology)
        priv->broadcast_topology = true;
    if (gids_of != UINT32_MAX)
        g_hash_table_add(priv->broadcast_gids, GUINT_TO_POINTER(gids_of));
    if (table_to != UINT32_MAX)
        g_hash_table_add(priv->broadcast_tables, GUINT_TO_POINTER(table_to));
    priv->broadcasts_asked++;
    qemu_cond_signal(&priv->broadcast_cond);
    qemu_mutex_unlock(&priv->broadcast_lock);
}

/* Manager: who is in the mesh, or whether one is alive, has changed */
void tcp_broadcast_topology(TcpBackendPrivate *priv)
{
    tcp_broadcast_ask(priv, true, UINT32_MAX, UINT32_MAX);
}

/* Manager: what @node_id owns may have changed */
void tcp_broadcast_node_gids(TcpBackendPrivate *priv, uint32_t node_id)
{
    tcp_broadcast_ask(priv, false, node_id, UINT32_MAX);
}

/*
 * Manager: @node_id has died, come back to life, or left the mesh, which
 * changes the topology, and what it owns if it claims anything
 */
void tcp_broadcast_node(TcpBackendPrivate *priv, uint32_t node_id)
{
    tcp_broadcast_ask(priv, true, node_id, UINT32_MAX);
}

/* Manager: @node_id has registered, and needs to know what everyone owns */
void tcp_broadcast_gid_table_to(TcpBackendPrivate *priv, uint32_t node_id)
{
    tcp_broadcast_ask(priv, false, UINT32_MAX, node_id);
}

/* Move the node ids in @set to @ids.  The caller holds broadcast_lock. */
static void tcp_broadcast_take_ids(GHashTable *set, GArray *ids)
{
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init(&iter, set);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        uint32_t id = GPOINTER_TO_UINT(key);

        g_array_append_val(ids, id);
    }
    g_hash_table_remove_all(set);
}

/*
 * Manager: send everything the broadcaster has been asked for since the last
 * pass: the topology, then the whole GID table to each node that needs it,
 * then each changed GID set.  Each is snapshotted just before it is sent,
 * after the asks are taken, so nothing asked is missed.
 *
 * Only the broadcaster thread calls this, so that what the passes snapshot
 * reaches each worker in the order it was taken, which
 * tcp_gid_claims_prune() relies on; a test that runs no broadcaster thread
 * may call it itself.
 */
void tcp_broadcast_pass(TcpBackendPrivate *priv)
{
    GArray *tables = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    GArray *nodes = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    bool topology;
    uint64_t asked;

    qemu_mutex_lock(&priv->broadcast_lock);
    topology = priv->broadcast_topology;
    priv->broadcast_topology = false;
    tcp_broadcast_take_ids(priv->broadcast_tables, tables);
    tcp_broadcast_take_ids(priv->broadcast_gids, nodes);
    asked = priv->broadcasts_asked;
    qemu_mutex_unlock(&priv->broadcast_lock);

    if (topology)
        tcp_send_mesh_topology(priv);
    if (tables->len > 0 || nodes->len > 0)
        tcp_send_gid_updates(priv, tables, nodes);

    qemu_mutex_lock(&priv->broadcast_lock);
    priv->broadcasts_sent = asked;
    qemu_mutex_unlock(&priv->broadcast_lock);

    g_array_free(nodes, TRUE);
    g_array_free(tables, TRUE);
}

/*
 * Manager broadcaster thread: wait to be asked, then send what was asked, a
 * pass at a time, until tcp_stop_broadcast_thread() clears the flag.  The
 * flag is checked under the lock the stop holds while clearing it, so a stop
 * can't slip in between the check and the wait and go unseen.
 */
void *tcp_broadcast_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;

    rdma_info_report("TCP: Manager broadcast thread started");

    for (;;) {
        qemu_mutex_lock(&priv->broadcast_lock);
        while (atomic_load(&priv->broadcast_running) &&
               priv->broadcasts_sent == priv->broadcasts_asked)
            qemu_cond_wait(&priv->broadcast_cond, &priv->broadcast_lock);
        qemu_mutex_unlock(&priv->broadcast_lock);

        if (!atomic_load(&priv->broadcast_running))
            break;
        tcp_broadcast_pass(priv);
    }

    rdma_info_report("TCP: Manager broadcast thread exiting");
    return NULL;
}

/*
 * Stop the manager's broadcaster thread and wait for it.  The thread waits
 * on broadcast_cond for something to send, so the flag is cleared under
 * broadcast_lock with the condition signalled, as for the health check.  A
 * pass under way sends what it took first, which with stopping set doesn't
 * wait for any peer.  What is left unsent stays unsent.  A no-op when the
 * thread was never started.
 */
void tcp_stop_broadcast_thread(TcpBackendPrivate *priv)
{
    if (!atomic_load(&priv->broadcast_running)) {
        return;
    }
    qemu_mutex_lock(&priv->broadcast_lock);
    atomic_store(&priv->broadcast_running, false);
    qemu_cond_signal(&priv->broadcast_cond);
    qemu_mutex_unlock(&priv->broadcast_lock);
    qemu_thread_join(&priv->broadcast_thread);
}

/*
 * Whether node has gone too long without a heartbeat, which is three
 * intervals.  The caller holds mesh_table_lock, and read now_ms under it.
 */
static bool tcp_node_silent(const TcpBackendPrivate *priv,
                            const MeshNodeInfo *node, uint64_t now_ms)
    TSA_REQUIRES(priv->mesh_table_lock)
{
    return now_ms - node->last_heartbeat_ms >
           (uint64_t)priv->health_check_interval_sec * 3000u;
}

/*
 * The part of a health-check pass that needs mesh_table_lock: note each
 * other node's connection, taking a reference to it, and declare dead the
 * nodes whose connection has ended and those that have stopped answering,
 * adding their ids to @dead, for the rest of the mesh to be told.
 */
static void tcp_health_check_scan(TcpBackendPrivate *priv, GArray *items,
                                  GArray *dead)
{
    GHashTableIter iter;
    gpointer value;

    qemu_mutex_lock(&priv->mesh_table_lock);
    /* Read under the lock, so no heartbeat is newer */
    uint64_t now_ms = ernic_monotonic_ms();

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        MeshNodeInfo *node = (MeshNodeInfo *)value;
        TcpHealthCheckItem item = {node->node_id, NULL, false, false};
        bool newly_dead = false, gids_gone = false;

        if (node->node_id == tcp_local_node(priv))
            continue;

        /* Takes conn_table_lock, in the documented order, after this one */
        item.conn = tcp_connection_lookup(priv, node->node_id);

        /*
         * The node hung up, or its connection failed.  Nothing more can
         * come on the connection, and it can't be sent anything, so there's
         * no need to wait for the node to go silent.
         */
        if (item.conn && tcp_conn_dead(item.conn)) {
            if (node->is_alive) {
                rdma_warn_report("TCP: Node %u disconnected", node->node_id);
                tcp_node_set_alive(priv, node, false, &gids_gone);
                newly_dead = true;
            }
            item.disconnected = true;
        }

        if (tcp_node_silent(priv, node, now_ms)) {
            if (node->is_alive) {
                rdma_warn_report("TCP: Node %u failed health check (last "
                                 "heartbeat: %" PRIu64 " seconds ago)",
                                 node->node_id,
                                 (now_ms - node->last_heartbeat_ms) / 1000u);
                tcp_node_set_alive(priv, node, false, &gids_gone);
                newly_dead = true;
            }
            item.silent = true;
        }
        if (gids_gone)
            tcp_gid_claims_report_alive(node->node_id, false);
        if (newly_dead)
            g_array_append_val(dead, node->node_id);

        /* Every reference taken above lands in items, to be released */
        if (item.conn)
            g_array_append_val(items, item);
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);
}

/*
 * Take conn, node_id's connection when the pass began, out of connections
 * and retire it, unless it has left the table already.  The caller keeps its
 * own reference.  Called with no lock held, since retiring joins the receive
 * thread, whose handlers take mesh_table_lock and conn_table_lock.
 */
static void tcp_health_check_unpublish(TcpBackendPrivate *priv,
                                       uint32_t node_id, TcpConnection *conn)
    TSA_EXCLUDES(priv->mesh_table_lock, priv->conn_table_lock)
{
    bool taken;

    qemu_mutex_lock(&priv->conn_table_lock);
    taken = tcp_conn_table_take(priv, node_id, conn);
    qemu_mutex_unlock(&priv->conn_table_lock);

    /* Whoever took it out, if not this thread, retires it */
    if (taken) {
        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
    }
}

/*
 * Whether node_id is still silent.  The scan's verdict can be out of date by
 * the time it's acted on: a node can answer a heartbeat late, on the
 * connection that would be retired.  Or a registration can have evicted the
 * node (tcp_evict_dead_node()), taking the connection out of the table for
 * itself to retire if it was still there; there is then nothing left to do.
 */
static bool tcp_node_still_silent(TcpBackendPrivate *priv, uint32_t node_id)
{
    const MeshNodeInfo *node;
    bool evicted, silent = false;

    qemu_mutex_lock(&priv->mesh_table_lock);
    node = g_hash_table_lookup(priv->mesh_nodes, GUINT_TO_POINTER(node_id));
    evicted = !node;
    if (node)
        silent = tcp_node_silent(priv, node, ernic_monotonic_ms());
    qemu_mutex_unlock(&priv->mesh_table_lock);
    if (evicted)
        rdma_info_report("TCP: Node %u has been evicted from the mesh",
                         node_id);
    else if (!silent)
        rdma_info_report("TCP: Node %u has answered; keeping its connection",
                         node_id);
    return silent;
}

/*
 * Retire the connections the scan found ended, or whose node has stopped
 * answering, and take them out of connections.  Threads that still hold one
 * see their sends on it fail, and the data path no longer finds it.
 *
 * An ended connection would otherwise keep its descriptor, in CLOSE_WAIT,
 * until the node registered again, which may be never.  A silent node's
 * connection may never end at all: the node can have gone without hanging
 * up, in a partition, say, or a NAT that forgot the connection, or be a
 * process that has been paused.  Kept, it would stay published, and the node
 * couldn't register again (tcp_register_again()).  Retired, it ends on the
 * node's side, if the node is still there to see it, and the node connects
 * to the manager again; the manager never connects to a node itself.
 */
void tcp_health_check_retire(TcpBackendPrivate *priv, GArray *items)
{
    for (guint i = 0; i < items->len; i++) {
        TcpHealthCheckItem *item = &g_array_index(items, TcpHealthCheckItem, i);

        if (!item->disconnected &&
            !(item->silent && tcp_node_still_silent(priv, item->node_id)))
            continue;
        if (!item->disconnected)
            rdma_info_report("TCP: Closing the connection to node %u, which "
                             "has stopped answering",
                             item->node_id);
        tcp_health_check_unpublish(priv, item->node_id, item->conn);
        tcp_connection_unref(item->conn);
        item->conn = NULL;
    }
}

/* Drop the references the scan took, at the end of a pass. */
static void tcp_health_check_release(GArray *items)
{
    for (guint i = 0; i < items->len; i++) {
        TcpHealthCheckItem *item = &g_array_index(items, TcpHealthCheckItem, i);

        tcp_connection_unref(item->conn);
        item->conn = NULL;
    }
    g_array_set_size(items, 0);
}

/*
 * Manager health check thread.  Each pass releases the connections that are
 * done waiting to register, declares dead the nodes that have hung up or
 * haven't answered for three intervals, retires their connections, and sends
 * every other node a heartbeat.  A node whose connection is retired connects
 * and registers again itself, if it can (tcp_worker_manager_pass()).
 *
 * Only the scan holds mesh_table_lock.  Retiring a connection joins its
 * receive thread, which takes mesh_table_lock to record a heartbeat; holding
 * the lock across the join would deadlock with it.
 */
void *tcp_manager_health_check_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(TcpHealthCheckItem));
    GArray *dead = g_array_new(FALSE, FALSE, sizeof(uint32_t));

    rdma_info_report("TCP: Manager health check thread started");

    while (atomic_load(&priv->health_check_running)) {
        /* The accept thread reaps too, but only when a peer connects */
        tcp_reap_pending_conns(priv);

        tcp_health_check_scan(priv, items, dead);
        tcp_health_check_retire(priv, items);

        for (guint i = 0; i < items->len; i++) {
            const TcpHealthCheckItem *item =
                &g_array_index(items, TcpHealthCheckItem, i);
            TcpConnection *conn = item->conn;

            if (conn)
                tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT,
                                   item->node_id);
        }
        tcp_health_check_release(items);

        /*
         * A node the scan declared dead had its claims marked not-alive,
         * which takes it out of resolution without discarding what it
         * claimed. Discarding would be a one-way door: a node that is merely
         * silent for three intervals and then answers would come back live
         * and unreachable, and one that registers again would be advertised
         * as owning nothing until its own advertisement came.  The broadcast
         * tells the workers, who keep no liveness of their own.  It is asked
         * for once the node's connection is retired, so a worker told the
         * node is dead finds the manager has closed it.
         */
        for (guint i = 0; i < dead->len; i++)
            tcp_broadcast_node(priv, g_array_index(dead, uint32_t, i));
        g_array_set_size(dead, 0);

        /*
         * Wait out the interval, or until tcp_stop_health_check_thread()
         * clears the flag.  The flag is checked under the lock the stop
         * holds while clearing it, so a stop can't slip in between the
         * check and the wait and go unseen.
         */
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += priv->health_check_interval_sec;
        qemu_mutex_lock(&priv->health_check_lock);
        while (atomic_load(&priv->health_check_running)) {
            if (!qemu_cond_timedwait_until(&priv->health_check_cond,
                                           &priv->health_check_lock,
                                           &deadline)) {
                break;
            }
        }
        qemu_mutex_unlock(&priv->health_check_lock);
    }

    g_array_free(dead, TRUE);
    g_array_free(items, TRUE);
    rdma_info_report("TCP: Manager health check thread exiting");
    return NULL;
}

/*
 * Stop the manager's health-check thread and wait for it.  The thread spends
 * nearly all its time waiting out health_check_interval_sec, so clearing the
 * flag alone would leave the join (and a SIGTERM shutdown) waiting up to that
 * long; the signal ends the wait at once.  The flag is cleared under
 * health_check_lock so the thread can't miss the signal.  A no-op when the
 * thread was never started.
 */
void tcp_stop_health_check_thread(TcpBackendPrivate *priv)
{
    if (!atomic_load(&priv->health_check_running)) {
        return;
    }
    qemu_mutex_lock(&priv->health_check_lock);
    atomic_store(&priv->health_check_running, false);
    qemu_cond_signal(&priv->health_check_cond);
    qemu_mutex_unlock(&priv->health_check_lock);
    qemu_thread_join(&priv->health_check_thread);
}
