/*
 * RDMA Backend: TCP mesh - a worker
 *
 * What only a worker does: register with the manager, and register again
 * when its connection to the manager is lost; take the manager's word for
 * who is in the mesh; connect to its peers from its peer-connect thread; and
 * accept the connections of the peers that connect to it.
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
#include "ernic_time.h"
#include "qemu/thread.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <inttypes.h>

/*
 * The worker's connection to the manager, with a reference the caller drops
 * with tcp_connection_unref(), or NULL if it has none: the manager has none,
 * and a worker has none while it registers again.
 */
TcpConnection *tcp_worker_manager_conn(TcpBackendPrivate *priv)
{
    TcpConnection *conn;

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = atomic_load(&priv->manager_conn);
    if (conn) {
        tcp_connection_ref(conn);
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    return conn;
}

/*
 * Take the manager's answer, on conn, to this worker's registration, and wake
 * tcp_worker_register_with_manager(): *assigned_id is the node id the manager
 * gave, or 0 if it refused the registration or its answer can't be used.
 * Only an answer on the connection registering, and only the first, counts.
 * Returns false, taking nothing, otherwise.
 *
 * A first registration gives this node its id.  A later one, on a new
 * connection after the last one was given up on, must give it the id it
 * already has, or it is refused here, and *assigned_id set to 0, and the
 * worker doesn't register again (see registers_again).
 *
 * An accepted answer publishes conn as manager_conn, before the waiter wakes
 * and before this thread reads the next message on conn: the manager follows
 * the answer with the topology and the GID table, which a worker takes only
 * from manager_conn.  manager_conn is NULL until then: the registering
 * thread gave up on the last connection before it connected again.
 */
static bool tcp_worker_registration_done(TcpBackendPrivate *priv,
                                         TcpConnection *conn,
                                         uint32_t *assigned_id)
{
    uint32_t local = tcp_local_node(priv);
    uint32_t offered = *assigned_id;
    bool taken;

    if (local != 0 && offered != local) {
        *assigned_id = 0;
    }

    qemu_mutex_lock(&priv->registration_mutex);
    taken = conn == priv->registering && !priv->registration_complete;
    if (taken) {
        if (offered != 0 && *assigned_id == 0) {
            priv->registers_again = false;
        }
        if (*assigned_id != 0) {
            atomic_store(&priv->local_node_id, *assigned_id);
            qemu_mutex_lock(&priv->conn_table_lock);
            atomic_store(&priv->manager_conn, tcp_connection_ref(conn));
            qemu_mutex_unlock(&priv->conn_table_lock);
        }
        priv->registration_complete = true;
        priv->registration_accepted = *assigned_id != 0;
        qemu_cond_signal(&priv->registration_cond);
    }
    qemu_mutex_unlock(&priv->registration_mutex);

    if (taken && offered != 0 && *assigned_id == 0) {
        rdma_error_report("TCP: the manager registered this node again as "
                          "node %u, but it is node %u; refusing.  It predates "
                          "workers registering again, so this node won't try "
                          "again, and hears nothing from the mesh until it "
                          "restarts",
                          offered, local);
    }
    return taken;
}

/*
 * End the registration under way, on priv->registering, and return whether
 * the manager accepted it.  If wait, its answer is waited for first, for up
 * to TCP_REGISTER_TIMEOUT_MS, or until *running is cleared, if running isn't
 * NULL; tcp_stop_peer_thread() signals registration_cond as it clears the
 * peer-connect thread's.  An answer that comes once this has returned is
 * ignored, and one that was taken was taken before this looked, since both
 * happen under registration_mutex.
 */
static bool tcp_worker_end_registration(TcpBackendPrivate *priv,
                                        const atomic_bool *running, bool wait)
{
    struct timespec deadline;
    bool accepted;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += TCP_REGISTER_TIMEOUT_MS / 1000;

    qemu_mutex_lock(&priv->registration_mutex);
    while (wait && !priv->registration_complete &&
           (!running || atomic_load(running))) {
        if (!qemu_cond_timedwait_until(&priv->registration_cond,
                                       &priv->registration_mutex, &deadline)) {
            break;
        }
    }
    bool unanswered = wait && !priv->registration_complete;
    accepted = priv->registration_complete && priv->registration_accepted;
    priv->registering = NULL;
    qemu_mutex_unlock(&priv->registration_mutex);

    if (unanswered && running && !atomic_load(running)) {
        rdma_info_report("TCP: Stopped while registering");
    } else if (unanswered) {
        rdma_error_report("TCP: Registration timeout");
    }
    return accepted;
}

/*
 * Connect to the manager and register: as a new node, asking for any id, if
 * this node has none yet, and otherwise under the id it has.  The connect
 * gives up at deadline_ms (on ernic_monotonic_ms()'s clock) if it isn't zero,
 * and it and the wait for the answer give up when *running is cleared, if
 * running isn't NULL (see tcp_connect_to_remote()).
 *
 * Returns 0 once the manager has accepted the registration, whose answer
 * published the new connection as manager_conn
 * (tcp_worker_registration_done()), or -1, the new connection retired.  The
 * caller has no manager_conn: tcp_init() hasn't had one yet, and the
 * peer-connect thread has given up on the last one.
 */
int tcp_worker_register_with_manager(TcpBackendPrivate *priv,
                                     const atomic_bool *running,
                                     uint64_t deadline_ms)
{
    TcpRegisterNodePayload reg;
    TcpConnection *conn;
    char hostname[256];
    uint32_t local = tcp_local_node(priv);
    int fd, ret;

    /* Get local hostname */
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        strncpy(hostname, "localhost", sizeof(hostname) - 1);
    }
    hostname[sizeof(hostname) - 1] = '\0';

    fd = tcp_connect_to_remote(priv->manager_host, priv->manager_port, running,
                               deadline_ms);
    if (fd < 0) {
        char *addr = tcp_addr_str(priv->manager_host, priv->manager_port);

        rdma_error_report("TCP: Failed to connect to manager at %s", addr);
        g_free(addr);
        return -1;
    }
    conn = tcp_connection_new(0, priv->manager_host, priv->manager_port);
    conn->priv = priv;
    conn->sockfd = fd;
    atomic_store(&conn->is_connected, true);

    /* Before the receive thread starts, so it can take the answer */
    qemu_mutex_lock(&priv->registration_mutex);
    priv->registering = conn;
    priv->registration_complete = false;
    priv->registration_accepted = false;
    qemu_mutex_unlock(&priv->registration_mutex);

    if (!tcp_start_recv_thread(conn, "tcp-mgr-recv")) {
        qemu_mutex_lock(&priv->registration_mutex);
        priv->registering = NULL;
        qemu_mutex_unlock(&priv->registration_mutex);
        tcp_connection_unref(conn);
        return -1;
    }

    /* Prepare registration payload */
    memset(&reg, 0, sizeof(reg));
    g_strlcpy(reg.hostname, hostname, sizeof(reg.hostname));
    reg.port = htons(priv->listen_port);
    reg.requested_node_id = htonl(local != 0 ? local : 0xFFFFFFFF);
    reg.version = htonl(TCP_PROTOCOL_VERSION);

    /* Send registration request, under conn->lock like every write to a
     * connection's socket: its receive thread is already running and
     * writes to the socket too. */
    qemu_mutex_lock(&conn->lock);
    ret = tcp_send_message(
        priv, conn, TCP_MSG_REGISTER_NODE, &reg, sizeof(reg),
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
        local, 0, 0, 0);
    qemu_mutex_unlock(&conn->lock);
    if (ret < 0) {
        rdma_error_report("TCP: Failed to send registration request");
    }

    /* The receive thread logged why if the manager refused this node */
    if (!tcp_worker_end_registration(priv, running, ret == 0)) {
        rdma_error_report("TCP: Registration failed");
        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
        return -1;
    }

    /* manager_conn holds its own reference now */
    tcp_connection_unref(conn);
    rdma_info_report("TCP: Successfully registered with manager");
    return 0;
}

/*
 * The peers a worker is to connect to, out of the first num_nodes entries of
 * a MESH_TOPOLOGY update, appended to targets: each node the manager reports
 * alive whose id is higher than this one's.  Of each pair of workers, only
 * the one with the lower id connects, so the two don't connect to each other
 * at once; the manager, node 0, is already connected through manager_conn.
 */
static void tcp_topology_targets(TcpBackendPrivate *priv,
                                 const TcpMeshTopologyPayload *topo,
                                 uint32_t num_nodes, GArray *targets)
{
    uint32_t local = tcp_local_node(priv);

    for (uint32_t i = 0; i < num_nodes && i < G_N_ELEMENTS(topo->nodes); i++) {
        const TcpMeshNodeInfo *entry = &topo->nodes[i];
        uint32_t peer_id = ntohl(entry->node_id);
        TcpPeerTarget target;

        if (peer_id == local) {
            continue; /* Skip self */
        }

        if (local > peer_id) {
            rdma_info_report("TCP: Skipping connection to peer %u (will accept "
                             "incoming connection from lower-ID node)",
                             peer_id);
            continue;
        }

        /* A connect to a node the manager can't reach would only wait */
        if (!entry->is_alive) {
            rdma_info_report("TCP: Skipping connection to peer %u, which the "
                             "manager reports dead",
                             peer_id);
            continue;
        }

        target.node_id = peer_id;
        /* Not g_strlcpy(): the peer's hostname may not be terminated */
        strncpy(target.host, entry->hostname, sizeof(target.host) - 1);
        target.host[sizeof(target.host) - 1] = '\0';
        target.port = ntohs(entry->port);
        g_array_append_val(targets, target);
    }
}

/*
 * Hand the peers in a MESH_TOPOLOGY update to the peer-connect thread, in
 * place of the last update's, and wake it.  Called on the receive thread the
 * update came on, which must not wait on a connect: it also answers the
 * manager's heartbeats.
 */
static void tcp_worker_set_peers(TcpBackendPrivate *priv,
                                 const TcpMeshTopologyPayload *topo,
                                 uint32_t num_nodes)
{
    GArray *targets = g_array_new(FALSE, FALSE, sizeof(TcpPeerTarget));
    GArray *old;

    tcp_topology_targets(priv, topo, num_nodes, targets);
    qemu_mutex_lock(&priv->peer_lock);
    old = priv->peer_targets;
    priv->peer_targets = targets;
    priv->peer_targets_changed = true;
    qemu_cond_signal(&priv->peer_cond);
    qemu_mutex_unlock(&priv->peer_lock);
    g_array_free(old, TRUE);
}

/*
 * Whether the latest topology update has the worker connect to node_id at
 * host and port.  The caller holds peer_lock.
 */
static bool tcp_worker_peer_named(TcpBackendPrivate *priv, uint32_t node_id,
                                  const char *host, uint16_t port)
{
    for (guint i = 0; i < priv->peer_targets->len; i++) {
        const TcpPeerTarget *latest =
            &g_array_index(priv->peer_targets, TcpPeerTarget, i);

        if (latest->node_id == node_id) {
            return latest->port == port && host &&
                   strcmp(latest->host, host) == 0;
        }
    }
    return false;
}

/*
 * Whether the latest topology update still has the worker connect to target,
 * at the same address.  The peer-connect thread works from a copy of the
 * targets taken at the start of its pass, and a connect can take an interval,
 * so an update that came meanwhile may have reported the peer dead.
 */
static bool tcp_worker_peer_current(TcpBackendPrivate *priv,
                                    const TcpPeerTarget *target)
{
    bool current;

    qemu_mutex_lock(&priv->peer_lock);
    current = tcp_worker_peer_named(priv, target->node_id, target->host,
                                    target->port);
    qemu_mutex_unlock(&priv->peer_lock);
    return current;
}

/*
 * Publish conn, a new connection to target that has sent its handshake, and
 * start its receive thread, unless the latest topology update no longer
 * names target, or the table has a live connection to it by then.  The
 * table takes over the caller's reference; if conn isn't published, it is
 * closed.  Returns true if it was published.
 *
 * peer_lock and conn_table_lock are held together from the check that
 * target is current to the insert, so an update can't come between them and
 * leave a connection to a peer it dropped.  A dead connection to target is
 * replaced, and retired once the locks are dropped, since retiring joins its
 * receive thread.
 */
bool tcp_worker_publish_peer(TcpBackendPrivate *priv,
                             const TcpPeerTarget *target, TcpConnection *conn)
{
    TcpConnection *old = NULL;
    bool published = false;
    char thread_name[32];

    snprintf(thread_name, sizeof(thread_name), "tcp-recv-%u", target->node_id);

    qemu_mutex_lock(&priv->peer_lock);
    qemu_mutex_lock(&priv->conn_table_lock);
    if (!tcp_worker_peer_named(priv, target->node_id, target->host,
                               target->port)) {
        rdma_info_report("TCP: Peer node %u was dropped from the topology "
                         "meanwhile; closing the new connection",
                         target->node_id);
    } else {
        old = g_hash_table_lookup(priv->connections,
                                  GUINT_TO_POINTER(target->node_id));
        if (old && !tcp_conn_dead(old)) {
            rdma_info_report("TCP: Peer node %u connected to this node "
                             "meanwhile; closing the new connection",
                             target->node_id);
        } else if (tcp_start_recv_thread(conn, thread_name)) {
            /* Neither can fail: the lock has been held since the lookup */
            if (old) {
                tcp_conn_table_take(priv, target->node_id, old);
            }
            tcp_conn_table_add(priv, target->node_id, conn);
            published = true;
        }
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    qemu_mutex_unlock(&priv->peer_lock);

    if (!published) {
        tcp_connection_unref(conn);
        return false;
    }
    if (old) {
        rdma_info_report("TCP: Replaced the dead connection to peer node %u",
                         target->node_id);
        tcp_retire_conn(old);
        tcp_connection_unref(old);
    }
    return true;
}

/* Send handshake to remote node */
static int tcp_send_handshake(TcpConnection *conn, uint32_t local_node_id,
                              TcpMsgType msg_type)
{
    TcpHandshakePayload payload;
    payload.node_id = htonl(local_node_id);
    payload.version = htonl(TCP_PROTOCOL_VERSION);

    /*
     * The connection is published in priv->conn_table before
     * the handshake is sent, so another thread can already be
     * writing to this socket.  Serialise like every other
     * sender.  No caller holds conn->lock, and this mutex is
     * not recursive.
     */
    qemu_mutex_lock(&conn->lock);
    int ret =
        tcp_send_message(conn->priv, conn, msg_type, &payload, sizeof(payload),
                         1, local_node_id, conn->node_id, 0, 0);
    qemu_mutex_unlock(&conn->lock);
    return ret;
}

/*
 * Connect a worker to target and publish the connection, giving up at
 * deadline_ms (on ernic_monotonic_ms()'s clock) or when *running is cleared.
 * Returns true if the connection was published.
 *
 * Called on the peer-connect thread without conn_table_lock, which every
 * lookup on the data path takes: the connect can take until the deadline,
 * and the handshake can wait on the peer.  The lock is taken only to
 * publish.
 *
 * Either of the checks the pass made can be out of date once the connect
 * returns: an update can have dropped the peer, and the accept thread can
 * have published the peer's own connection to this node.  Both are checked
 * again before the handshake, which has the peer publish the connection, so
 * a connection closed before then costs the peer nothing.
 * tcp_worker_publish_peer() checks them once more, under the locks it
 * publishes under.  A connection it closes after the handshake dies on the
 * peer's side, which replaces it, or closes it within an interval.
 */
static bool tcp_worker_connect_peer(TcpBackendPrivate *priv,
                                    const TcpPeerTarget *target,
                                    const atomic_bool *running,
                                    uint64_t deadline_ms)
{
    atomic_fetch_add_explicit(&priv->tcp_stats.peer_connect_attempts, 1,
                              memory_order_relaxed);
    rdma_info_report("TCP: Connecting to peer node %u at %s:%u",
                     target->node_id, target->host, target->port);

    int fd =
        tcp_connect_to_remote(target->host, target->port, running, deadline_ms);
    if (fd < 0) {
        rdma_error_report("TCP: Failed to connect to peer %u", target->node_id);
        return false;
    }

    TcpConnection *peer_conn =
        tcp_connection_new(target->node_id, target->host, target->port);
    peer_conn->priv = priv;
    peer_conn->sockfd = fd;
    atomic_store(&peer_conn->is_connected, true);

    if (!tcp_worker_peer_current(priv, target)) {
        rdma_info_report("TCP: Peer node %u was dropped from the topology "
                         "meanwhile; closing the new connection",
                         target->node_id);
        tcp_connection_unref(peer_conn);
        return false;
    }
    if (tcp_has_live_connection(priv, target->node_id)) {
        rdma_info_report("TCP: Peer node %u connected to this node meanwhile; "
                         "closing the new connection",
                         target->node_id);
        tcp_connection_unref(peer_conn);
        return false;
    }

    /* Before the connection is published, so nothing goes out ahead of it */
    if (tcp_send_handshake(peer_conn, tcp_local_node(priv), TCP_MSG_HANDSHAKE) <
        0) {
        rdma_error_report("TCP: Failed to send handshake to peer %u",
                          target->node_id);
        tcp_connection_unref(peer_conn);
        return false;
    }

    if (!tcp_worker_publish_peer(priv, target, peer_conn)) {
        return false;
    }
    rdma_info_report("TCP: Connected to peer node %u", target->node_id);
    return true;
}

/*
 * Take out of connections the connections this worker is done with, and
 * retire them:
 *
 *   - every dead one, whatever its node.  Otherwise its descriptor would
 *     wait in CLOSE_WAIT until the node connected again, and a node this
 *     worker connects to would never be connected to again.
 *   - every one to a node with a higher id, which this worker connects to,
 *     that the latest topology update no longer names, or names at another
 *     address: the manager has reported the node dead, or it has come back
 *     elsewhere.  A link that died without a hang-up, in a crash or a
 *     partition, would otherwise look connected until a send failed, and
 *     the node would not be connected to again when it came back.
 *
 * Under peer_lock and conn_table_lock, so the connections are compared with
 * the latest targets, and with neither held to retire them, since that joins
 * their receive threads.
 */
static void tcp_worker_peer_sweep(TcpBackendPrivate *priv)
{
    GPtrArray *swept = g_ptr_array_new();
    uint32_t local = tcp_local_node(priv);
    GHashTableIter iter;
    gpointer key, value;

    qemu_mutex_lock(&priv->peer_lock);
    qemu_mutex_lock(&priv->conn_table_lock);
    g_hash_table_iter_init(&iter, priv->connections);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TcpConnection *conn = value;
        uint32_t node_id = GPOINTER_TO_UINT(key);

        if (tcp_conn_dead(conn)) {
            rdma_info_report("TCP: Closing the connection to node %u, which "
                             "has ended",
                             node_id);
        } else if (node_id > local &&
                   !tcp_worker_peer_named(priv, node_id, conn->remote_host,
                                          conn->remote_port)) {
            rdma_info_report("TCP: Closing the connection to node %u, which "
                             "the manager no longer reports alive at %s:%u",
                             node_id,
                             conn->remote_host ? conn->remote_host : "",
                             conn->remote_port);
        } else {
            continue;
        }
        /* Hands the table's reference over, as tcp_conn_table_take() does */
        g_hash_table_iter_steal(&iter);
        tcp_conn_unpublished(conn);
        g_ptr_array_add(swept, conn);
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    qemu_mutex_unlock(&priv->peer_lock);

    for (guint i = 0; i < swept->len; i++) {
        TcpConnection *conn = g_ptr_array_index(swept, i);

        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
    }
    g_ptr_array_free(swept, TRUE);
}

/*
 * One pass of the peer-connect thread: close the connections this worker is
 * done with (see tcp_worker_peer_sweep()), then connect to each of targets
 * that this worker has no live connection to.  Every one is tried, and each
 * connect gives up after a health-check interval, so a peer that doesn't
 * answer delays the ones after it by no more than that, and can't keep them
 * from connecting.  A target that an update during the pass no longer names
 * is skipped; one the update added is left to the next pass, which comes
 * straight after.  Before each connect, the worker's connection to the
 * manager is checked (tcp_worker_manager_pass()).
 */
static void tcp_worker_peer_pass(TcpBackendPrivate *priv, const GArray *targets)
{
    tcp_worker_peer_sweep(priv);

    for (guint i = 0;
         i < targets->len && atomic_load(&priv->peer_thread_running); i++) {
        const TcpPeerTarget *target = &g_array_index(targets, TcpPeerTarget, i);

        if (tcp_has_live_connection(priv, target->node_id) ||
            !tcp_worker_peer_current(priv, target)) {
            continue;
        }
        /* The last connect can have taken an interval */
        tcp_worker_manager_pass(priv);
        tcp_worker_connect_peer(priv, target, &priv->peer_thread_running,
                                ernic_monotonic_ms() +
                                    (uint64_t)priv->health_check_interval_sec *
                                        1000u);
    }
}

/*
 * The manager's part of a pass of the peer-connect thread.  A worker owns its
 * link to the manager; the manager never connects to a worker.  So when the
 * connection has ended, or nothing has come on it for three intervals, the
 * worker gives up on it, and connects and registers again, under the id it
 * has, giving up on the connect after an interval.  A check that finds
 * nothing has come for an interval first sends the manager a heartbeat
 * (tcp_send_heartbeat()), which the manager answers, so a manager
 * whose own interval is longer than this worker's isn't taken for silent.
 * For that, checks have to come less than two intervals apart, so a pass
 * makes one before each connect to a peer, which can take an interval.
 *
 * Until it has registered again the worker has no connection to the manager:
 * it hears nothing of the mesh, and can reach only the nodes it has a
 * connection of its own to.  A registration the manager refuses, as it does
 * while it still has the node's last connection, is tried again on the next
 * pass.
 *
 * Only this thread clears manager_conn, and nothing sets it while it is set,
 * so the connection given up on is the one taken out.  The thread is stopped
 * before the receive threads at teardown, so it never takes one out from
 * under tcp_stop_recv_threads().
 */
void tcp_worker_manager_pass(TcpBackendPrivate *priv)
{
    uint64_t interval_ms = (uint64_t)priv->health_check_interval_sec * 1000u;
    TcpConnection *conn = tcp_worker_manager_conn(priv);

    if (conn) {
        /* Before the clock, so no message recorded meanwhile is newer */
        uint64_t last_ms = atomic_load(&conn->last_rx_ms);
        uint64_t quiet_ms = ernic_monotonic_ms() - last_ms;
        TcpConnection *old;

        bool dead = tcp_conn_dead(conn);
        bool broken = false;

        if (!dead && quiet_ms <= 3 * interval_ms) {
            /* The manager is node 0 */
            if (quiet_ms < interval_ms ||
                tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT, 0)) {
                tcp_connection_unref(conn);
                return;
            }
            broken = true;
        }

        if (dead) {
            rdma_warn_report("TCP: The connection to the manager has ended; "
                             "connecting to it again");
        } else if (broken) {
            rdma_warn_report("TCP: Failed to send the manager a heartbeat; "
                             "connecting to it again");
        } else {
            rdma_warn_report("TCP: Nothing from the manager for %" PRIu64
                             " s; connecting to it again",
                             quiet_ms / 1000u);
        }

        /* Takes over manager_conn's reference */
        qemu_mutex_lock(&priv->conn_table_lock);
        old = atomic_exchange(&priv->manager_conn, NULL);
        if (old) {
            tcp_conn_unpublished(old);
        }
        qemu_mutex_unlock(&priv->conn_table_lock);
        if (old) {
            tcp_retire_conn(old);
            tcp_connection_unref(old);
        }
        tcp_connection_unref(conn);
    }

    qemu_mutex_lock(&priv->registration_mutex);
    bool again = priv->registers_again;
    qemu_mutex_unlock(&priv->registration_mutex);
    if (!again) {
        return;
    }
    atomic_fetch_add_explicit(&priv->tcp_stats.manager_reconnects, 1,
                              memory_order_relaxed);
    tcp_worker_register_with_manager(priv, &priv->peer_thread_running,
                                     ernic_monotonic_ms() + interval_ms);
}

/*
 * Worker peer-connect thread.  Keeps the worker's connection to the manager
 * (tcp_worker_manager_pass()), connects to the peers in the latest topology
 * update, and closes the connections the worker is done with: a pass comes
 * as soon as an update arrives, and otherwise a health-check interval
 * (ERNIC_TCP_HEALTH_INTERVAL) after the last one, so a peer that couldn't be
 * reached, or whose connection has ended, is tried again without waiting for
 * the mesh to change.
 *
 * The connects are made here rather than on the receive thread that gets the
 * update, because that thread also answers the manager's heartbeats, and a
 * connect to a peer that doesn't answer can take until the kernel gives up.
 */
void *tcp_worker_peer_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    GArray *targets = g_array_new(FALSE, FALSE, sizeof(TcpPeerTarget));

    rdma_info_report("TCP: Peer-connect thread started");

    while (atomic_load(&priv->peer_thread_running)) {
        tcp_worker_manager_pass(priv);

        qemu_mutex_lock(&priv->peer_lock);
        g_array_set_size(targets, 0);
        g_array_append_vals(targets, priv->peer_targets->data,
                            priv->peer_targets->len);
        priv->peer_targets_changed = false;
        qemu_mutex_unlock(&priv->peer_lock);

        tcp_worker_peer_pass(priv, targets);

        /*
         * Wait out the interval, or until an update arrives or
         * tcp_stop_peer_thread() clears the flag.  Both are checked under
         * the lock they're set under, so neither can slip in between the
         * check and the wait and go unseen.
         */
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += priv->health_check_interval_sec;
        qemu_mutex_lock(&priv->peer_lock);
        while (atomic_load(&priv->peer_thread_running) &&
               !priv->peer_targets_changed) {
            if (!qemu_cond_timedwait_until(&priv->peer_cond, &priv->peer_lock,
                                           &deadline)) {
                break;
            }
        }
        qemu_mutex_unlock(&priv->peer_lock);
    }

    g_array_free(targets, TRUE);
    rdma_info_report("TCP: Peer-connect thread exiting");
    return NULL;
}

/*
 * Stop a worker's peer-connect thread and wait for it.  The thread spends
 * most of its time waiting out an interval, and the rest connecting, which
 * gives up within 100 ms of the flag being cleared, or waiting for the
 * answer to a registration.  The flag is cleared under peer_lock, with
 * peer_cond signalled, so the thread can't miss it, and registration_cond is
 * signalled under registration_mutex, which the thread checks the flag under
 * before it waits for an answer.  A no-op when the thread was never started.
 */
void tcp_stop_peer_thread(TcpBackendPrivate *priv)
{
    if (!atomic_load(&priv->peer_thread_running)) {
        return;
    }
    qemu_mutex_lock(&priv->peer_lock);
    atomic_store(&priv->peer_thread_running, false);
    qemu_cond_signal(&priv->peer_cond);
    qemu_mutex_unlock(&priv->peer_lock);
    qemu_mutex_lock(&priv->registration_mutex);
    qemu_cond_signal(&priv->registration_cond);
    qemu_mutex_unlock(&priv->registration_mutex);
    qemu_thread_join(&priv->peer_thread);
}

/*
 * The node id a REGISTER_RESP gives this node, or 0, having logged why, if it
 * refuses the node or its answer can't be used.
 */
static uint32_t tcp_register_resp_id(const TcpRecvMsg *msg)
{
    const TcpRegisterRespPayload *resp = msg->payload;
    uint32_t resp_id;
    int32_t result;

    if (!resp || msg->hdr.msg_len < TCP_REGISTER_RESP_LEN_V3) {
        rdma_error_report("TCP: Invalid REGISTER_RESP payload");
        return 0;
    }

    resp_id = ntohl(resp->assigned_node_id);
    result = (int32_t)ntohl(resp->result);

    /*
     * The other half of the check the manager makes: a pre-v4 manager sends
     * no version and would otherwise admit this node to a mesh whose
     * resolver still guesses.  Refusing here is what makes the gate work in
     * both directions, since an old manager cannot refuse us.
     */
    uint32_t mgr_version = 0;

    if (msg->hdr.msg_len >= sizeof(TcpRegisterRespPayload)) {
        mgr_version = ntohl(resp->version);
    }

    if (mgr_version != TCP_PROTOCOL_VERSION) {
        rdma_error_report("TCP: manager speaks v%u, this node speaks v%u; "
                          "refusing to join -- a pre-v%u mesh resolves "
                          "every destination GID to one default node",
                          mgr_version, TCP_PROTOCOL_VERSION,
                          TCP_PROTOCOL_VERSION);
        return 0;
    }
    if (result < 0 && result != INT32_MIN) {
        /* A negative errno; INT32_MIN has no negation */
        rdma_error_report("TCP: Registration failed: %s", strerror(-result));
        return 0;
    }
    if (result != 0) {
        rdma_error_report("TCP: Registration failed: result %" PRId32, result);
        return 0;
    }
    if (resp_id == 0 || resp_id == UINT32_MAX) {
        /*
         * 0 is the manager's own id, and UINT32_MAX means no node at all, to
         * the GID table and to the data path (tcp_backend_local_node_id()).
         */
        rdma_error_report("TCP: manager assigned node id %u, which no worker "
                          "can have",
                          resp_id);
        return 0;
    }
    return resp_id;
}

/*
 * TCP_MSG_REGISTER_RESP: the manager's answer, on conn, to this worker's
 * registration.
 *
 * Only the manager assigns this node its id, and only once for each
 * registration.  The answer sets the id and the GIDs this node claims, and
 * which connection it takes the mesh's word from, so one accepted from a
 * peer, or a second one, would let another node take this one's place in the
 * mesh.  tcp_worker_registration_done() takes only the first answer on the
 * connection this node opened to the manager itself to register; a peer's
 * can't be it.
 */
void tcp_worker_recv_register_resp(TcpConnection *conn, const TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;
    uint32_t assigned_id;
    bool first;

    if (!priv || priv->is_manager) {
        rdma_error_report("TCP: REGISTER_RESP received by manager");
        return;
    }

    first = !tcp_worker_registered(priv);
    /* The id to record: 0 if the answer refuses this node */
    assigned_id = tcp_register_resp_id(msg);

    /*
     * A refusal ends the wait too, so tcp_init() fails at once rather than
     * at the registration timeout.
     */
    if (!tcp_worker_registration_done(priv, conn, &assigned_id)) {
        rdma_error_report("TCP: ignoring a REGISTER_RESP that answers no "
                          "registration under way");
        return;
    }
    if (assigned_id == 0) {
        return;
    }

    if (first) {
        rdma_info_report("TCP: Registered with manager, assigned node_id=%u",
                         assigned_id);
    } else {
        rdma_info_report("TCP: Registered with the manager again as node %u",
                         assigned_id);
    }

    /*
     * The node id is only known now, so this is the first point at which
     * this node can say which GIDs are its own.  On registering again, a
     * change this node made to them while it had no connection to the
     * manager was never sent.
     */
    tcp_advertise_local_gids(priv);
}

/*
 * TCP_MSG_MESH_TOPOLOGY: who is in the mesh, from the manager.  The claims
 * of the nodes it leaves out are dropped, and the peers in it handed to the
 * peer-connect thread.
 */
void tcp_worker_recv_topology(TcpConnection *conn, const TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;
    const TcpMeshTopologyPayload *topo = msg->payload;
    uint32_t num_nodes;

    if (!priv || priv->is_manager) {
        rdma_error_report("TCP: MESH_TOPOLOGY received by manager");
        return;
    }

    if (!topo || msg->hdr.msg_len < sizeof(TcpMeshTopologyPayload)) {
        rdma_error_report("TCP: Invalid MESH_TOPOLOGY payload");
        return;
    }

    /*
     * Authorise on the connection, as for NODE_GIDS: only the manager says
     * who is in the mesh.  An update decides which peers this worker
     * connects to, and which of its connections it closes
     * (tcp_worker_peer_sweep()), so one from a peer could take down the
     * worker's links.
     *
     * That also ignores an update that comes before the answer to this
     * node's registration, which is what publishes manager_conn.  Which
     * peers this node connects to depends on its id.  The manager publishes
     * a new worker's connection before it answers the registration, so the
     * broadcast for another node's registration can come first.  It follows
     * its answer with a broadcast of its own, so nothing is lost.
     */
    if (conn != atomic_load(&priv->manager_conn)) {
        rdma_error_report("TCP: MESH_TOPOLOGY from a connection that is not "
                          "the manager's, or before registration; ignoring");
        return;
    }

    num_nodes = ntohl(topo->num_nodes);

    rdma_info_report("TCP: Received mesh topology with %u nodes", num_nodes);

    tcp_gid_claims_prune(priv, topo, num_nodes);
    tcp_worker_set_peers(priv, topo, num_nodes);
}

/*
 * Read the handshake that a peer connecting to this worker sends to say
 * which node it is, from sockfd, into *node_id and *version.  Returns false,
 * having logged why, if none comes.
 *
 * The deadline covers the whole handshake, including a wait inside
 * tcp_recv_message() for the rest of one that has started, so a peer that
 * sends only part of one holds up the peers behind it no longer than one
 * that sends nothing.
 */
static bool tcp_worker_recv_handshake(TcpBackendPrivate *priv, int sockfd,
                                      uint32_t *node_id, uint32_t *version)
{
    uint64_t deadline_ms = ernic_monotonic_ms() + TCP_HANDSHAKE_TIMEOUT_MS;
    const TcpHandshakePayload *hs;
    TcpMsgHeader hdr;
    void *payload;
    int ret;

    for (;;) {
        ret = tcp_recv_message(sockfd, &hdr, &payload,
                               &priv->accept_thread_running, deadline_ms);
        if (ret == 0) {
            break;
        }
        if (ret != -EAGAIN) {
            /* An error, a hang-up, a stop, or the deadline */
            rdma_error_report("TCP: Error receiving handshake from incoming "
                              "connection");
            return false;
        }
        /*
         * Nothing yet.  A peer can connect and send nothing, so check the
         * flag before each wait, or a stop would wait out the deadline.
         */
        if (!atomic_load(&priv->accept_thread_running)) {
            rdma_info_report("TCP: Stopped while waiting for a handshake");
            return false;
        }
        if (ernic_monotonic_ms() >= deadline_ms) {
            rdma_error_report("TCP: Timeout waiting for handshake from "
                              "incoming connection");
            return false;
        }
        /*
         * Wait for it, but no more than 100 ms at a time: stopping the
         * thread doesn't wake this poll().
         */
        struct pollfd pfd = {.fd = sockfd, .events = POLLIN};

        poll(&pfd, 1, 100);
    }

    if (hdr.msg_type != TCP_MSG_HANDSHAKE) {
        rdma_error_report("TCP: Expected handshake, got message type %u",
                          hdr.msg_type);
        free(payload);
        return false;
    }

    /* The length is the peer's to choose, down to no payload at all */
    if (!payload || hdr.msg_len < sizeof(TcpHandshakePayload)) {
        rdma_error_report("TCP: Handshake payload is %u bytes, need %zu",
                          hdr.msg_len, sizeof(TcpHandshakePayload));
        free(payload);
        return false;
    }

    hs = payload;
    *node_id = ntohl(hs->node_id);
    *version = ntohl(hs->version);
    free(payload);
    return true;
}

/*
 * Worker: take a connection the accept thread has accepted, on sockfd, from
 * host:port, a peer with a lower id connecting to this node.  Once its
 * handshake has said which node it is, the connection is published, with a
 * receive thread, and the handshake answered.  Otherwise sockfd is closed.
 */
void tcp_worker_accept(TcpBackendPrivate *priv, int sockfd, const char *host,
                       uint16_t port)
{
    uint32_t remote_node_id, remote_version;
    TcpConnection *conn, *old;
    char thread_name[32];

    if (!tcp_worker_recv_handshake(priv, sockfd, &remote_node_id,
                                   &remote_version)) {
        close(sockfd);
        return;
    }

    rdma_info_report("TCP: Handshake from node %u (v%u)", remote_node_id,
                     remote_version);

    if (remote_version != TCP_PROTOCOL_VERSION) {
        rdma_error_report("TCP: Protocol version mismatch: local=%u remote=%u",
                          TCP_PROTOCOL_VERSION, remote_version);
        close(sockfd);
        return;
    }

    /*
     * A live connection to the node is kept, and this one closed: replacing
     * it would let anyone who can reach this port take over a working link
     * by claiming its node id.  A dead one is replaced, or the node could
     * never connect again, and retired once the lock is dropped, since that
     * joins its thread.
     */
    qemu_mutex_lock(&priv->conn_table_lock);
    old = g_hash_table_lookup(priv->connections,
                              GUINT_TO_POINTER(remote_node_id));
    if (old && !tcp_conn_dead(old)) {
        qemu_mutex_unlock(&priv->conn_table_lock);
        rdma_info_report("TCP: Connection to node %u already exists, closing "
                         "duplicate",
                         remote_node_id);
        close(sockfd);
        return;
    }

    /* Create connection object */
    conn = tcp_connection_new(remote_node_id, host, port);
    conn->sockfd = sockfd;
    atomic_store(&conn->is_connected, true);
    conn->priv = priv;

    /*
     * Start the receive thread before the connection is published, so a
     * connection without one is never in the table.
     */
    snprintf(thread_name, sizeof(thread_name), "tcp-recv-%u", remote_node_id);
    if (!tcp_start_recv_thread(conn, thread_name)) {
        qemu_mutex_unlock(&priv->conn_table_lock);
        tcp_connection_unref(conn);
        return;
    }

    /*
     * Add to connection table, which takes over the reference from
     * tcp_connection_new(), so take another for the response below.
     * Neither the take nor the add can fail: the lock has been held since
     * the lookup above.
     */
    if (old) {
        tcp_conn_table_take(priv, remote_node_id, old);
    }
    tcp_connection_ref(conn);
    tcp_conn_table_add(priv, remote_node_id, conn);
    qemu_mutex_unlock(&priv->conn_table_lock);

    if (old) {
        rdma_info_report("TCP: Replaced the dead connection from node %u",
                         remote_node_id);
        tcp_retire_conn(old);
        tcp_connection_unref(old);
    }

    /* Send handshake response */
    tcp_send_handshake(conn, tcp_local_node(priv), TCP_MSG_HANDSHAKE_RESP);
    tcp_connection_unref(conn);
}
