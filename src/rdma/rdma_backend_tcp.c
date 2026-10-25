/*
 * RDMA Backend: TCP/IP Network - Multi-Node Support
 *
 * TCP/IP backend for connecting multiple rocm_ernic server instances
 * over network in a mesh topology. Enables RDMA operations between
 * multiple servers without physical hardware.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_tcp_priv.h"
#include "rdma_backend_defs.h"
#include "rdma_backend.h"
#include "rdma_rm_defs.h"
#include "rdma_utils.h"
#include "rocm_ernic_compat.h"
#include "ernic_time.h"
#include "parse_int.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <inttypes.h>

/* Linux gives EWOULDBLOCK the same value as EAGAIN, so errno checks in
 * this file test EAGAIN alone. */
_Static_assert(EWOULDBLOCK == EAGAIN, "EWOULDBLOCK must equal EAGAIN");

/* Tunable defaults -- overridable via env vars */
#define TCP_DEFAULT_LISTEN_BACKLOG    32
#define TCP_DEFAULT_HEALTH_INTERVAL_S 5

static int tcp_env_int(const char *name, int fallback)
{
    const char *val = getenv(name);
    if (val) {
        int v;
        if (ernic_parse_int(val, &v) && v > 0) {
            return v;
        }
    }
    return fallback;
}

/*
 * View a GID array as const, for the functions that only read one.  Before
 * C23, ISO C does not convert uint8_t (*)[16] to const uint8_t (*)[16]
 * implicitly, because the const qualifies the array's elements rather than
 * the array.  Taking the array through a typed parameter keeps its [16]
 * shape checked, which a bare cast at each call site would not.
 */
static inline const uint8_t (*tcp_const_gids(uint8_t (*gids)[16]))[16]
{
    return (const uint8_t(*)[16])gids;
}

/*
 * TCP Backend Data Structures
 */

/*
 * SRQ bookkeeping for the TCP backend.
 *
 * Geometry only, with no receive ring: nothing in this backend serves an SRQ
 * receive. rdma_rm_alloc_srq() still reaches create/destroy here, so the
 * object has to exist and carry its attributes, but the ionic device draws
 * shared receives from ionic_datapath.c's rings and never comes through this
 * path. The queue that used to live here had no producer and no consumer.
 */
typedef struct {
    uint32_t max_wr;
    uint32_t max_sge;
    uint32_t srq_limit;
    QemuMutex lock;
} TcpSRQ;

/* Receive buffer pool; see TcpBufPool */

void tcp_bufpool_init(TcpBufPool *pool)
{
    memset(pool, 0, sizeof(*pool));
    qemu_mutex_init(&pool->lock);
    for (int i = 0; i < TCP_BUFPOOL_SLOTS; i++) {
        pool->bufs[i] = g_malloc(TCP_BUFPOOL_BUFSZ);
        pool->count++;
    }
}

void tcp_bufpool_destroy(TcpBufPool *pool)
{
    for (int i = 0; i < TCP_BUFPOOL_SLOTS; i++) {
        g_free(pool->bufs[i]);
        pool->bufs[i] = NULL;
    }
    pool->count = 0;
    qemu_mutex_destroy(&pool->lock);
}

static void *tcp_bufpool_get(TcpBufPool *pool, size_t len)
{
    if (len > TCP_BUFPOOL_BUFSZ) {
        return g_malloc(len);
    }
    qemu_mutex_lock(&pool->lock);
    if (pool->count > 0) {
        pool->count--;
        void *buf = pool->bufs[pool->head];
        pool->bufs[pool->head] = NULL;
        pool->head = (pool->head + 1) % TCP_BUFPOOL_SLOTS;
        qemu_mutex_unlock(&pool->lock);
        return buf;
    }
    qemu_mutex_unlock(&pool->lock);
    return g_malloc(len);
}

static void tcp_bufpool_put(TcpBufPool *pool, void *buf, size_t len)
{
    if (len > TCP_BUFPOOL_BUFSZ || !buf) {
        g_free(buf);
        return;
    }
    qemu_mutex_lock(&pool->lock);
    if (pool->count < TCP_BUFPOOL_SLOTS) {
        int slot = (pool->head + pool->count) % TCP_BUFPOOL_SLOTS;
        pool->bufs[slot] = buf;
        pool->count++;
        qemu_mutex_unlock(&pool->lock);
        return;
    }
    qemu_mutex_unlock(&pool->lock);
    g_free(buf);
}

/*
 * Helper Functions
 */

/*
 * Whether a worker has its node id.  Until it does, its id reads as 0, the
 * manager's, which it must not present as its own.
 */
static inline bool tcp_worker_registered(TcpBackendPrivate *priv)
{
    return tcp_local_node(priv) != 0;
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

/* Forward declarations */
static void tcp_sendq_close(TcpConnection *conn);
static void tcp_stop_send_thread(TcpConnection *conn);
static void tcp_advertise_local_gids(TcpBackendPrivate *priv);
static void tcp_send_node_gids(TcpBackendPrivate *priv, TcpConnection *conn,
                               const TcpNodeGidsPayload *payload);
static int tcp_send_handshake(TcpConnection *conn, uint32_t local_node_id,
                              TcpMsgType msg_type);

static TcpBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    if (!backend_dev) {
        return NULL;
    }
    return (TcpBackendPrivate *)backend_dev->backend_private;
}

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
         * ids are allocated monotonically and a dead node is never evicted
         * -- own its guest's GID instead of deadlocking against the id it
         * used to have.
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
 * Record @node_id's advertised set, replacing whatever it claimed before.
 * An empty set withdraws its claims, which is how the manager revokes.
 */
void tcp_gid_claims_set(TcpBackendPrivate *priv, uint32_t node_id,
                        const uint8_t (*gids)[16], uint32_t num_gids)
{
    if (!priv || !priv->node_gids || num_gids > TCP_MAX_NODE_GIDS)
        return;

    qemu_mutex_lock(&priv->gid_table_lock);

    if (num_gids == 0) {
        g_hash_table_remove(priv->node_gids, GUINT_TO_POINTER(node_id));
    } else {
        TcpNodeGids *claims = g_new0(TcpNodeGids, 1);

        claims->num_gids = num_gids;
        claims->alive = true; /* it just advertised, so it is answering */
        for (uint32_t i = 0; i < num_gids; i++)
            memcpy(claims->gids[i], gids[i], 16);
        g_hash_table_insert(priv->node_gids, GUINT_TO_POINTER(node_id), claims);
    }

    tcp_gid_index_rebuild(priv);

    qemu_mutex_unlock(&priv->gid_table_lock);

    /* Logged outside the lock: rdma_info_report() can block on stderr. */
    char buf[TCP_GID_STR_LEN];

    if (num_gids == 0) {
        rdma_info_report("TCP: node %u withdrew its GIDs", node_id);
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

void tcp_gid_claims_set_alive(TcpBackendPrivate *priv, uint32_t node_id,
                              bool alive)
{
    bool changed = false;

    if (!priv || !priv->node_gids)
        return;

    qemu_mutex_lock(&priv->gid_table_lock);

    TcpNodeGids *claims =
        g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(node_id));
    if (claims && claims->alive != alive) {
        claims->alive = alive;
        tcp_gid_index_rebuild(priv);
        changed = true;
    }

    qemu_mutex_unlock(&priv->gid_table_lock);

    /* Logged outside the lock: rdma_info_report() can block on stderr. */
    if (changed)
        rdma_info_report("TCP: node %u's GIDs are %s", node_id,
                         alive ? "live again" : "unreachable while it is away");
}

/*
 * Free conn.  Called only by the last tcp_connection_unref().
 *
 * By then a connection whose receive or send thread was started has been
 * retired, which joined them (see TcpConnection).  A thread still running
 * means a reference was dropped without retiring the connection, a bug; the
 * thread is stopped and joined here rather than left running on freed
 * memory.  The socket is shut down first, as tcp_retire_conn() does, so the
 * join can't wait on the peer.
 */
static void tcp_connection_free(TcpConnection *conn)
{
    bool recv_running = atomic_exchange(&conn->recv_thread_running, false);
    bool send_running;

    qemu_mutex_lock(&conn->sendq_lock);
    send_running = conn->send_thread_started;
    qemu_mutex_unlock(&conn->sendq_lock);

    if (recv_running || send_running) {
        rdma_error_report("TCP: freeing the connection to node %u with its "
                          "threads running",
                          conn->node_id);
        if (conn->sockfd >= 0) {
            shutdown(conn->sockfd, SHUT_RDWR);
        }
        if (recv_running) {
            qemu_thread_join(&conn->recv_thread);
        }
    }
    tcp_stop_send_thread(conn);

    if (conn->sockfd >= 0) {
        close(conn->sockfd);
    }

    g_free(conn->remote_host);
    qemu_mutex_destroy(&conn->lock);
    qemu_cond_destroy(&conn->sendq_cond);
    qemu_mutex_destroy(&conn->sendq_lock);
    g_free(conn);
}

/*
 * Take another reference to conn.  The caller already holds one, or found
 * conn under the lock of a table that holds one.  Returns conn.
 */
TcpConnection *tcp_connection_ref(TcpConnection *conn)
{
    atomic_fetch_add(&conn->refcount, 1);
    return conn;
}

/* Drop a reference to conn, freeing it with the last.  Accepts NULL. */
void tcp_connection_unref(TcpConnection *conn)
{
    if (conn && atomic_fetch_sub(&conn->refcount, 1) == 1) {
        tcp_connection_free(conn);
    }
}

/* GDestroyNotify adapter for tcp_connection_unref() */
void tcp_connection_destroy_notify(gpointer data)
{
    tcp_connection_unref(data);
}

/* A new connection, holding one reference, the caller's. */
TcpConnection *tcp_connection_new(uint32_t node_id, const char *host,
                                  uint16_t port)
{
    TcpConnection *conn = g_new0(TcpConnection, 1);
    conn->node_id = node_id;
    conn->sockfd = -1;
    conn->remote_host = g_strdup(host);
    conn->remote_port = port;
    atomic_init(&conn->refcount, 1);
    atomic_init(&conn->is_connected, false);
    atomic_init(&conn->recv_thread_running, false);
    atomic_init(&conn->recv_thread_exited, false);
    atomic_init(&conn->last_rx_ms, ernic_monotonic_ms());
    qemu_mutex_init(&conn->lock);
    qemu_mutex_init(&conn->sendq_lock);
    qemu_cond_init(&conn->sendq_cond);
    g_queue_init(&conn->sendq);
    return conn;
}

/*
 * Whether conn is dead: its receive thread has returned, because the peer
 * hung up, a read failed, or the peer was refused.  Nothing more can come on
 * it, and it can't be sent on, but it keeps its descriptor until whoever
 * takes it out of its table retires it.
 */
bool tcp_conn_dead(TcpConnection *conn)
{
    return atomic_load(&conn->recv_thread_exited);
}

/*
 * The connection to node_id, with a reference the caller drops with
 * tcp_connection_unref(), or NULL.
 */
TcpConnection *tcp_connection_lookup(TcpBackendPrivate *priv, uint32_t node_id)
{
    TcpConnection *conn;

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id));
    if (conn) {
        tcp_connection_ref(conn);
    }
    qemu_mutex_unlock(&priv->conn_table_lock);

    return conn;
}

/*
 * Every connection in connections, each with a reference, which freeing the
 * array drops.  For sending to each with conn_table_lock dropped: a send can
 * wait on its peer, and every lookup on the data path takes the lock.
 */
static GPtrArray *tcp_conns_snapshot(TcpBackendPrivate *priv)
{
    GPtrArray *conns =
        g_ptr_array_new_with_free_func(tcp_connection_destroy_notify);
    GHashTableIter iter;
    gpointer value;

    qemu_mutex_lock(&priv->conn_table_lock);
    g_hash_table_iter_init(&iter, priv->connections);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        g_ptr_array_add(conns, tcp_connection_ref(value));
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    return conns;
}

/*
 * Whether connections has an entry for node_id that isn't dead, as it stands
 * now.  A dead one is replaced when the node connects again.
 */
bool tcp_has_live_connection(TcpBackendPrivate *priv, uint32_t node_id)
{
    TcpConnection *conn;
    bool live;

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id));
    live = conn && !tcp_conn_dead(conn);
    qemu_mutex_unlock(&priv->conn_table_lock);

    return live;
}

/*
 * The connection to send node_id's traffic on, with a reference the caller
 * drops with tcp_connection_unref(), or NULL: the direct connection if it is
 * up, or else a worker's connection to the manager, which relays to the
 * other workers.
 */
TcpConnection *tcp_route_to_node(TcpBackendPrivate *priv, uint32_t node_id)
{
    TcpConnection *conn = tcp_connection_lookup(priv, node_id);

    if (conn && atomic_load(&conn->is_connected)) {
        return conn;
    }
    tcp_connection_unref(conn);

    conn = tcp_worker_manager_conn(priv);
    if (conn && atomic_load(&conn->is_connected)) {
        return conn;
    }
    tcp_connection_unref(conn);
    return NULL;
}

/*
 * Publish conn as node_id's connection, the table taking over the caller's
 * reference.  Refused, returning false, if node_id already has one:
 * inserting over it would drop the old entry's reference without retiring
 * it.  The caller holds conn_table_lock.
 */
static bool tcp_conn_table_add(TcpBackendPrivate *priv, uint32_t node_id,
                               TcpConnection *conn)
{
    if (g_hash_table_contains(priv->connections, GUINT_TO_POINTER(node_id))) {
        rdma_error_report("TCP: node %u already has a connection", node_id);
        return false;
    }
    g_hash_table_insert(priv->connections, GUINT_TO_POINTER(node_id), conn);
    return true;
}

/*
 * Close conn's send queue as it leaves connections or manager_conn, under
 * conn_table_lock, which guards both.  tcp_backend_ionic_cancel() finds a
 * connection only there, so a queue left open until the connection is
 * retired, after the lock is dropped, could start a request the cancel had
 * taken back everywhere else.  What was queued is dropped, as retiring the
 * connection would drop it; the message being written goes out, as it would
 * have anyway.
 */
static void tcp_conn_unpublished(TcpConnection *conn)
{
    qemu_mutex_lock(&conn->sendq_lock);
    tcp_sendq_close(conn);
    qemu_mutex_unlock(&conn->sendq_lock);
}

/*
 * Take node_id's entry out of connections if it is still conn, handing the
 * table's reference to the caller, who then retires conn and drops the
 * reference with no lock held (see TcpConnection).  Its send queue is closed
 * at once (tcp_conn_unpublished()).  Returns whether it was taken.  The
 * caller holds conn_table_lock.
 */
bool tcp_conn_table_take(TcpBackendPrivate *priv, uint32_t node_id,
                         TcpConnection *conn)
{
    if (g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id)) !=
        conn) {
        return false;
    }
    g_hash_table_steal(priv->connections, GUINT_TO_POINTER(node_id));
    tcp_conn_unpublished(conn);
    return true;
}

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

MeshNodeInfo *tcp_mesh_node_info_new(uint32_t node_id, const char *hostname,
                                     uint16_t port)
{
    if (!hostname || !*hostname) {
        rdma_error_report("TCP: Invalid hostname for node %u", node_id);
        return NULL;
    }

    MeshNodeInfo *node = g_new0(MeshNodeInfo, 1);
    if (!node) {
        rdma_error_report("TCP: Failed to allocate mesh node info for node %u",
                          node_id);
        return NULL;
    }

    node->node_id = node_id;
    node->hostname = g_strdup(hostname);
    if (!node->hostname) {
        rdma_error_report("TCP: Failed to duplicate hostname '%s' for node %u",
                          hostname, node_id);
        g_free(node);
        return NULL;
    }
    node->port = port;
    node->is_alive = true;
    node->last_heartbeat_ms = ernic_monotonic_ms();
    return node;
}

/*
 * Whether a registration can't be given node_id: a node has it, or a
 * connection does.  The caller holds mesh_table_lock and conn_table_lock.
 */
static bool tcp_node_id_taken(TcpBackendPrivate *priv, uint32_t node_id)
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
 * pass.
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
 * Collect the nodes whose connection has gone, so the caller can withdraw
 * what they claimed once it has dropped mesh_table_lock.
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
 * Take @node_id out of resolution if it is still unreachable, and say
 * whether it was.  Returns false when the node has a live connection again,
 * leaving its claims alone.
 *
 * The re-check is the point.  tcp_collect_dead_claimants() runs under
 * mesh_table_lock and the caller then drops it, and in that window one of
 * the nodes it collected can register again under its own id: that
 * publishes a connection and restores the node's claims
 * (tcp_register_again()).  Withdrawing on the strength of the stale list
 * would undo the revival and leave the node is_alive with claims that are
 * not, which nothing recovers from -- the heartbeat handler only restores
 * claims on the transition out of dead, and the node never makes it again.
 * So the connection is looked at again here, under the lock a registration
 * publishes beneath, which makes the two atomic with respect to each other.
 *
 * Lock order: conn_table_lock and gid_table_lock under mesh_table_lock,
 * both as documented; see tcp_health_check_scan(), which does the same.
 */
bool tcp_withdraw_if_still_unreachable(TcpBackendPrivate *priv,
                                       uint32_t node_id)
{
    TcpConnection *conn;
    bool withdrawn = false;

    qemu_mutex_lock(&priv->mesh_table_lock);
    conn = tcp_connection_lookup(priv, node_id);
    if (!conn || tcp_conn_dead(conn)) {
        tcp_gid_claims_set_alive(priv, node_id, false);
        withdrawn = true;
    }
    if (conn)
        tcp_connection_unref(conn);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    return withdrawn;
}

/* Parse manager config: "manager:<ip>:<port>" or "manager:listen:<port>" */
static int parse_tcp_config_manager(const char *config, char **manager_host,
                                    uint16_t *manager_port, bool *listen_mode)
{
    char *config_copy, *saveptr, *token;
    const char *parse_start = config;
    char *host_str = NULL;
    uint16_t port_val = 0;
    bool listen = false;

    if (!strncmp(config, "tcp:", 4)) {
        parse_start = config + 4;
    }

    if (strncmp(parse_start, "manager:", 8) != 0) {
        rdma_error_report("TCP manager config must start with 'manager:'");
        return -EINVAL;
    }

    parse_start += 8; /* Skip "manager:" */
    config_copy = g_strdup(parse_start);
    token = strtok_r(config_copy, ":", &saveptr);

    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP manager: missing host or 'listen'");
        return -EINVAL;
    }

    if (!strcmp(token, "listen")) {
        listen = true;
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) {
            g_free(config_copy);
            rdma_error_report("TCP manager: missing port for listen mode");
            return -EINVAL;
        }
        if (!ernic_parse_port(token, &port_val)) {
            port_val = 0; /* rejected below by the port_val == 0 check */
        }
        host_str = g_strdup("0.0.0.0");
    } else {
        host_str = g_strdup(token);
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) {
            g_free(config_copy);
            g_free(host_str);
            rdma_error_report("TCP manager: missing port");
            return -EINVAL;
        }
        if (!ernic_parse_port(token, &port_val)) {
            port_val = 0; /* rejected below by the port_val == 0 check */
        }
    }

    if (port_val == 0) {
        g_free(config_copy);
        g_free(host_str);
        rdma_error_report("TCP manager: invalid port");
        return -EINVAL;
    }

    *manager_host = host_str;
    *manager_port = port_val;
    *listen_mode = listen;
    g_free(config_copy);
    return 0;
}

/* Parse worker config: "worker:<manager_ip>:<manager_port>" */
static int parse_tcp_config_worker(const char *config, char **manager_host,
                                   uint16_t *manager_port)
{
    char *config_copy, *saveptr, *token;
    const char *parse_start = config;
    char *host_str = NULL;
    uint16_t port_val = 0;

    if (!strncmp(config, "tcp:", 4)) {
        parse_start = config + 4;
    }

    if (strncmp(parse_start, "worker:", 7) != 0) {
        rdma_error_report("TCP worker config must start with 'worker:'");
        return -EINVAL;
    }

    parse_start += 7; /* Skip "worker:" */
    config_copy = g_strdup(parse_start);
    token = strtok_r(config_copy, ":", &saveptr);

    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP worker: missing manager host");
        return -EINVAL;
    }
    host_str = g_strdup(token);

    token = strtok_r(NULL, ":", &saveptr);
    if (!token) {
        g_free(config_copy);
        g_free(host_str);
        rdma_error_report("TCP worker: missing manager port");
        return -EINVAL;
    }
    if (!ernic_parse_port(token, &port_val)) {
        port_val = 0; /* rejected below by the port_val == 0 check */
    }

    if (port_val == 0) {
        g_free(config_copy);
        g_free(host_str);
        rdma_error_report("TCP worker: invalid manager port");
        return -EINVAL;
    }

    *manager_host = host_str;
    *manager_port = port_val;
    g_free(config_copy);
    return 0;
}

/*
 * Size the socket buffers.  Doing nothing is the fast default: Linux autotunes
 * up to tcp_wmem[2]/tcp_rmem[2], which are megabytes, whereas any explicit
 * SO_SNDBUF/SO_RCVBUF is both clamped to net.core.{w,r}mem_max -- typically
 * 208 KiB -- and latches SOCK_{SND,RCV}BUF_LOCK, so the buffer can never grow
 * again.  Asking for 4 MiB that way yields a socket an order of magnitude
 * smaller than leaving it alone, which throttles the large-message path.
 * ERNIC_TCP_SOCKBUF remains for hosts that really do want a fixed size.
 */
static void tcp_tune_sockbuf(int sockfd)
{
    const char *val = getenv("ERNIC_TCP_SOCKBUF");
    int bufsize, got;
    socklen_t len;

    if (!val || !ernic_parse_int(val, &bufsize) || bufsize <= 0)
        return;

    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    len = sizeof(got);
    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &got, &len) == 0 &&
        got / 2 < bufsize) {
        rdma_warn_report("TCP: ERNIC_TCP_SOCKBUF=%d clamped to %d by "
                         "net.core.wmem_max; raise it or unset the variable "
                         "to let the kernel autotune",
                         bufsize, got / 2);
    }
}

/*
 * Wait for the non-blocking connect() in progress on sockfd to finish.
 * Returns 0 once it has succeeded, or -1 with errno set: to the connect's
 * own error, to ETIMEDOUT once ernic_monotonic_ms() reaches deadline_ms (if
 * deadline_ms isn't zero), or to ECANCELED once *running is cleared (if
 * running isn't NULL).
 */
static int tcp_wait_connected(int sockfd, const atomic_bool *running,
                              uint64_t deadline_ms)
{
    for (;;) {
        struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};
        int err = 0;
        socklen_t len = sizeof(err);
        int ret;

        if (running && !atomic_load(running)) {
            errno = ECANCELED;
            return -1;
        }
        if (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        ret = poll(&pfd, 1, 100);
        if (ret < 0 && errno != EINTR) {
            return -1;
        }
        if (ret <= 0) {
            continue;
        }
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            return -1;
        }
        if (err != 0) {
            errno = err;
            return -1;
        }
        return 0;
    }
}

/*
 * Connect to each address in addrs in turn until one connects, all within
 * the one deadline (see tcp_wait_connected()).  Returns the socket,
 * non-blocking, or -1 with errno set by the last attempt.  Gives up on the
 * remaining addresses once the deadline passes or *running is cleared.
 */
int tcp_connect_any(const struct addrinfo *addrs, const atomic_bool *running,
                    uint64_t deadline_ms)
{
    int err = ENOENT;

    for (const struct addrinfo *rp = addrs; rp; rp = rp->ai_next) {
        int sockfd = socket(rp->ai_family,
                            rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                            rp->ai_protocol);
        if (sockfd < 0) {
            err = errno;
            continue;
        }

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0 ||
            (errno == EINPROGRESS &&
             tcp_wait_connected(sockfd, running, deadline_ms) == 0))
            return sockfd;

        err = errno;
        close(sockfd);
        /*
         * Any other failure, including the kernel giving up on this address
         * with ETIMEDOUT, moves on to the next address.
         */
        if (err == ECANCELED ||
            (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms))
            break;
    }
    errno = err;
    return -1;
}

/*
 * Connect to host:port and return the connected socket, non-blocking, or -1.
 *
 * The connect itself is non-blocking, so a host that doesn't answer doesn't
 * hold the caller until the kernel gives up on it, about two minutes.  A
 * caller that passes its thread's stop flag in running gets control back
 * within 100 ms of the flag being cleared, and a nonzero deadline_ms, a time
 * on ernic_monotonic_ms()'s clock, gives up then, however many addresses host
 * has.  With neither, it waits as long as a blocking connect() would.
 * Resolving host can still block.
 */
int tcp_connect_to_remote(const char *host, uint16_t port,
                          const atomic_bool *running, uint64_t deadline_ms)
{
    struct addrinfo hints, *res;
    char port_str[8];
    int sockfd;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        rdma_error_report("TCP: Failed to resolve host '%s': %s", host,
                          gai_strerror(rc));
        return -1;
    }

    sockfd = tcp_connect_any(res, running, deadline_ms);
    err = errno;
    freeaddrinfo(res);

    if (sockfd < 0 && err == ECANCELED) {
        rdma_info_report("TCP: Stopped while connecting to %s:%u", host, port);
        return -1;
    }
    if (sockfd < 0) {
        rdma_error_report("TCP: Failed to connect to %s:%u: %s", host, port,
                          strerror(err));
        return -1;
    }

    /* Disable Nagle -- send immediately */
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    tcp_tune_sockbuf(sockfd);

    rdma_info_report("TCP: Connected to %s:%u", host, port);
    return sockfd;
}

/*
 * Listen on *port, on every address.  If *port is 0, the kernel picks a free
 * port, which is stored in *port.  Asking for it on the socket that keeps it
 * is the only way to be sure no other process has taken it.
 */
static int tcp_listen_on_port(uint16_t *port)
{
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    int sockfd;
    int ret;
    int opt = 1;

    sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
        rdma_error_report("TCP: Failed to create listen socket: %s",
                          strerror(errno));
        return -1;
    }

    ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to set SO_REUSEADDR: %s",
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(*port);

    ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to bind to port %u: %s", *port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    ret = getsockname(sockfd, (struct sockaddr *)&server_addr, &addr_len);
    if (ret < 0) {
        rdma_error_report("TCP: Failed to read the listen socket's port: %s",
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    ret = listen(sockfd, tcp_env_int("ERNIC_TCP_LISTEN_BACKLOG",
                                     TCP_DEFAULT_LISTEN_BACKLOG));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to listen on port %u: %s",
                          ntohs(server_addr.sin_port), strerror(errno));
        close(sockfd);
        return -1;
    }

    *port = ntohs(server_addr.sin_port);
    rdma_info_report("TCP: Listening on port %u", *port);
    return sockfd;
}

/*
 * How long a send to a peer may go without a byte leaving before it gives up
 * (see tcp_send_message2()).  A manager gives up in half the time a worker
 * does, so that when a worker stops reading, the manager's relay to it gives
 * up, and the manager reads the sending worker again, before the sending
 * worker gives up on its connection to the manager.
 */
uint32_t tcp_send_timeout_ms(TcpBackendPrivate *priv)
{
    uint32_t ms =
        atomic_load_explicit(&priv->send_timeout_ms, memory_order_relaxed);

    return priv->is_manager ? ms / 2 + ms % 2 : ms;
}

/*
 * Send a message on conn.  The caller holds conn->lock, which serialises
 * writes to the socket, and has checked conn->sockfd under it.
 *
 * The payload may be given in two parts so a caller that already holds a
 * header and a body separately does not have to splice them into one buffer
 * first -- sendmsg() sends them as one message either way.
 *
 * The socket is non-blocking, so when the peer isn't reading fast enough the
 * loop waits for room, but only for tcp_send_timeout_ms() without a byte
 * leaving: a peer that stays connected but stops reading would otherwise
 * hold conn->lock, and everyone waiting for it, indefinitely.  Once
 * priv->stopping is set it fails instead of waiting at all; teardown is about
 * to shut the socket down anyway.
 *
 * A send that fails once part of the message may be on the wire leaves the
 * stream unusable, so it shuts the connection down.  Senders waiting for
 * conn->lock then fail at once with EPIPE rather than each waiting out a
 * timeout of its own, and the receive thread sees end-of-file and returns,
 * which has the connection retired: by the health check on a manager, by
 * the peer-connect thread's sweep on a worker, and, for a worker's
 * manager_conn, by its next pass, which registers again.
 */
static int tcp_send_message2(TcpBackendPrivate *priv, TcpConnection *conn,
                             TcpMsgType msg_type, const void *payload,
                             size_t payload_len, const void *payload2,
                             size_t payload2_len, uint32_t seq,
                             uint32_t src_node, uint32_t dst_node,
                             uint32_t src_qpn, uint32_t dst_qpn)
{
    int sockfd = conn->sockfd;
    uint32_t timeout_ms = tcp_send_timeout_ms(priv);
    uint64_t progress_ms;
    TcpMsgHeader hdr;
    ssize_t ret;

    /* The receiver drops the connection on anything larger. */
    if (payload_len > TCP_MAX_PAYLOAD_LEN ||
        payload2_len > TCP_MAX_PAYLOAD_LEN - payload_len) {
        rdma_error_report("TCP: payload too large: %zu + %zu", payload_len,
                          payload2_len);
        return -1;
    }
    if (sockfd < 0) {
        return -1;
    }

    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(msg_type);
    hdr.msg_len = htonl((uint32_t)(payload_len + payload2_len));
    hdr.seq = htonl(seq);
    hdr.src_node_id = htonl(src_node);
    hdr.dst_node_id = htonl(dst_node);
    hdr.src_qpn = htonl(src_qpn);
    hdr.dst_qpn = htonl(dst_qpn);

    struct iovec iov[3];
    int iovcnt = 1;
    size_t total = sizeof(hdr);

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);

    /* struct iovec serves readv() as well as sendmsg(), so iov_base is not
     * const even though sendmsg() only reads through it. */
    ROCM_ERNIC_WARN_CAST_AWAY_CONST_OFF
    if (payload && payload_len > 0) {
        iov[iovcnt].iov_base = (void *)payload;
        iov[iovcnt].iov_len = payload_len;
        total += payload_len;
        iovcnt++;
    }
    if (payload2 && payload2_len > 0) {
        iov[iovcnt].iov_base = (void *)payload2;
        iov[iovcnt].iov_len = payload2_len;
        total += payload2_len;
        iovcnt++;
    }
    ROCM_ERNIC_WARN_CAST_AWAY_CONST_ON

    size_t sent = 0;

    progress_ms = ernic_monotonic_ms();
    while (sent < total) {
        struct iovec cur[3];
        size_t cur_cnt = 0;
        size_t skip = sent;
        struct msghdr msg;

        for (int i = 0; i < iovcnt; i++) {
            if (skip >= iov[i].iov_len) {
                skip -= iov[i].iov_len;
                continue;
            }
            cur[cur_cnt].iov_base = (char *)iov[i].iov_base + skip;
            cur[cur_cnt].iov_len = iov[i].iov_len - skip;
            cur_cnt++;
            skip = 0;
        }

        /*
         * MSG_NOSIGNAL: a peer that has gone, or a socket that teardown has
         * shut down to wake this loop, fails with EPIPE rather than raising
         * a SIGPIPE that would kill the server.
         */
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = cur;
        msg.msg_iovlen = cur_cnt;
        ret = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 && errno == EAGAIN) {
            struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};

            if (atomic_load(&priv->stopping)) {
                rdma_info_report("TCP: Stopped while sending message");
                goto broken;
            }
            if (ernic_monotonic_ms() - progress_ms >= timeout_ms) {
                rdma_error_report("TCP: Node %u has read nothing for %" PRIu32
                                  " ms; closing the connection",
                                  conn->node_id, timeout_ms);
                goto broken;
            }
            poll(&pfd, 1, 5);
            continue;
        }
        if (ret < 0) {
            rdma_error_report("TCP: Failed to send message to node %u: %s",
                              conn->node_id, strerror(errno));
            goto broken;
        }
        sent += (size_t)ret;
        progress_ms = ernic_monotonic_ms();
    }

    return 0;

broken:
    shutdown(sockfd, SHUT_RDWR);
    return -1;
}

static int tcp_send_message(TcpBackendPrivate *priv, TcpConnection *conn,
                            TcpMsgType msg_type, const void *payload,
                            size_t payload_len, uint32_t seq, uint32_t src_node,
                            uint32_t dst_node, uint32_t src_qpn,
                            uint32_t dst_qpn)
{
    return tcp_send_message2(priv, conn, msg_type, payload, payload_len, NULL,
                             0, seq, src_node, dst_node, src_qpn, dst_qpn);
}

/*
 * Drop what is on conn's send queue and close it to more.  The caller holds
 * sendq_lock.
 */
static void tcp_sendq_close(TcpConnection *conn)
{
    TcpQueuedMsg *msg;

    conn->sendq_closed = true;
    while ((msg = g_queue_pop_head(&conn->sendq)) != NULL) {
        g_free(msg);
    }
    conn->sendq_bytes = 0;
    qemu_cond_signal(&conn->sendq_cond);
}

/*
 * A connection's send thread: writes the messages tcp_conn_enqueue() queues,
 * in order, after any heartbeat tcp_send_heartbeat() has put ahead of them,
 * each under conn->lock like every other write to the socket.  A
 * write that fails, including one that times out on a peer that has stopped
 * reading, shuts the connection down (see tcp_send_message2()); the queue is
 * then closed, and the thread returns.  It also returns when
 * tcp_stop_send_thread() closes the queue.  Either way the thread is joined
 * by tcp_stop_send_thread(), when the connection is retired.
 *
 * A message is taken off the queue to be written, which closing the queue
 * then can't free under the write, but stays in sendq_bytes until it has
 * been written, so the queue's limits count it until it is on the wire.
 */
static void *tcp_send_thread(void *opaque)
{
    TcpConnection *conn = opaque;

    qemu_mutex_lock(&conn->sendq_lock);
    while (!conn->sendq_closed) {
        TcpQueuedMsg *msg = g_queue_pop_head(&conn->sendq);
        int ret = -1;

        if (!msg) {
            qemu_cond_wait(&conn->sendq_cond, &conn->sendq_lock);
            continue;
        }
        qemu_mutex_unlock(&conn->sendq_lock);

        qemu_mutex_lock(&conn->lock);
        if (conn->sockfd >= 0) {
            ret = tcp_send_message2(conn->priv, conn, msg->msg_type, msg->data,
                                    msg->len, NULL, 0, msg->seq, msg->src_node,
                                    msg->dst_node, 0, 0);
        }
        qemu_mutex_unlock(&conn->lock);

        qemu_mutex_lock(&conn->sendq_lock);
        /* Closing the queue zeroes the count, this message's part included */
        if (!conn->sendq_closed) {
            conn->sendq_bytes -= msg->len;
        }
        g_free(msg);
        if (ret < 0) {
            tcp_sendq_close(conn);
        }
    }
    qemu_mutex_unlock(&conn->sendq_lock);
    return NULL;
}

/*
 * Start conn's send thread unless it is running.  Returns 0, or -ENOMEM if it
 * couldn't be started.  The caller holds sendq_lock, and has checked the
 * queue isn't closed: a closed queue's thread may have been joined already.
 */
static int tcp_sendq_start(TcpConnection *conn)
{
    if (conn->send_thread_started) {
        return 0;
    }
    if (qemu_thread_create(&conn->send_thread, tcp_send_thread, conn) != 0) {
        rdma_error_report("TCP: Failed to start the send thread for node %u",
                          conn->node_id);
        return -ENOMEM;
    }
    conn->send_thread_started = true;
    return 0;
}

/*
 * A message to queue on conn, with len bytes of payload left for the caller
 * to fill in.
 */
static TcpQueuedMsg *tcp_queued_msg_new(TcpConnection *conn,
                                        TcpMsgType msg_type, uint32_t dst_node,
                                        size_t len)
{
    TcpQueuedMsg *msg = g_malloc(sizeof(*msg) + len);

    msg->msg_type = msg_type;
    msg->seq = atomic_fetch_add_explicit(&conn->priv->next_seq, 1,
                                         memory_order_relaxed);
    msg->src_node = tcp_local_node(conn->priv);
    msg->dst_node = dst_node;
    msg->tag = 0;
    msg->reply = false;
    msg->len = (uint32_t)len;
    return msg;
}

/*
 * Queue a TCP_MSG_IONIC to dst_node on conn, for its send thread to write,
 * without waiting for the peer: hdr and body are copied.  tag is the caller's
 * name for a request, which tcp_sendq_cancel() matches.  The first message
 * queued starts the thread.
 *
 * Returns 0 once queued, or a negative errno:
 *   -EINVAL   hdr and body together don't fit TCP_MAX_PAYLOAD_LEN
 *   -EAGAIN   a request (!reply) that would take the queue past
 *             TCP_SENDQ_REQUEST_BYTES, for the caller to try again later
 *   -ENOBUFS  a reply that would take it past TCP_SENDQ_REPLY_BYTES
 *   -EPIPE    the connection is retired, or a send on it has failed
 *   -ENOMEM   the send thread couldn't be started
 * An empty queue takes any one message.  A reply has more room than a
 * request: it answers a request the peer has already sent, and the peer
 * can't move on without it.
 */
int tcp_conn_enqueue(TcpConnection *conn, const void *hdr, size_t hdr_len,
                     const void *body, size_t body_len, uint32_t dst_node,
                     uint64_t tag, bool reply)
{
    size_t limit = reply ? TCP_SENDQ_REPLY_BYTES : TCP_SENDQ_REQUEST_BYTES;
    TcpQueuedMsg *msg;
    size_t len;
    int ret = 0;

    /* Each checked on its own before the sum, which could wrap */
    if (hdr_len > TCP_MAX_PAYLOAD_LEN ||
        body_len > TCP_MAX_PAYLOAD_LEN - hdr_len) {
        return -EINVAL;
    }
    len = hdr_len + body_len;

    qemu_mutex_lock(&conn->sendq_lock);
    if (conn->sendq_closed) {
        ret = -EPIPE;
    } else if (conn->sendq_bytes != 0 && conn->sendq_bytes > limit - len) {
        /* limit - len can't wrap: each limit holds the largest message */
        ret = reply ? -ENOBUFS : -EAGAIN;
    } else {
        ret = tcp_sendq_start(conn);
    }
    if (ret != 0) {
        qemu_mutex_unlock(&conn->sendq_lock);
        return ret;
    }

    msg = tcp_queued_msg_new(conn, TCP_MSG_IONIC, dst_node, len);
    msg->tag = tag;
    msg->reply = reply;
    memcpy(msg->data, hdr, hdr_len);
    if (body_len > 0) {
        memcpy(msg->data + hdr_len, body, body_len);
    }
    g_queue_push_tail(&conn->sendq, msg);
    conn->sendq_bytes += len;
    qemu_cond_signal(&conn->sendq_cond);
    qemu_mutex_unlock(&conn->sendq_lock);
    return 0;
}

/*
 * Whether conn's send queue already holds a msg_type, a heartbeat or its
 * answer.  They are pushed at the head and data at the tail, so they are all
 * in the run at the head.  The caller holds sendq_lock.
 */
static bool tcp_sendq_has_heartbeat(TcpConnection *conn, TcpMsgType msg_type)
{
    for (GList *link = conn->sendq.head; link != NULL; link = link->next) {
        const TcpQueuedMsg *msg = link->data;

        if (msg->msg_type == TCP_MSG_IONIC) {
            break;
        }
        if (msg->msg_type == msg_type) {
            return true;
        }
    }
    return false;
}

/*
 * Queue msg_type, a heartbeat or its answer, to dst_node at the head of
 * conn's send queue, so the send thread writes it as soon as it is done with
 * the message it is writing, ahead of the data path's.  One of each type
 * waiting is enough, and keeps a peer that sends heartbeats faster than it
 * reads the answers from growing the queue without limit; another is
 * dropped.  Returns 0, -EPIPE if the queue is closed, or -ENOMEM if the send
 * thread couldn't be started.
 */
static int tcp_sendq_push_heartbeat(TcpConnection *conn, TcpMsgType msg_type,
                                    uint32_t dst_node)
{
    int ret;

    qemu_mutex_lock(&conn->sendq_lock);
    ret = conn->sendq_closed ? -EPIPE : tcp_sendq_start(conn);
    if (ret == 0 && !tcp_sendq_has_heartbeat(conn, msg_type)) {
        g_queue_push_head(&conn->sendq,
                          tcp_queued_msg_new(conn, msg_type, dst_node, 0));
        qemu_cond_signal(&conn->sendq_cond);
    }
    qemu_mutex_unlock(&conn->sendq_lock);
    return ret;
}

/*
 * Drop the requests on conn's send queue whose tag matches tag in the bits
 * mask selects, which no send thread has started to write.  The one being
 * written, if it is one, is off the queue and goes out whole: a message the
 * peer has started to read can't be taken back.
 */
static void tcp_sendq_cancel(TcpConnection *conn, uint64_t tag, uint64_t mask)
{
    qemu_mutex_lock(&conn->sendq_lock);
    for (GList *link = conn->sendq.head; link != NULL;) {
        GList *next = link->next;
        TcpQueuedMsg *msg = link->data;

        if (msg->msg_type == TCP_MSG_IONIC && !msg->reply &&
            (msg->tag & mask) == (tag & mask)) {
            conn->sendq_bytes -= msg->len;
            g_queue_delete_link(&conn->sendq, link);
            g_free(msg);
        }
        link = next;
    }
    qemu_mutex_unlock(&conn->sendq_lock);
}

/*
 * Send dst_node msg_type, a heartbeat or the answer to one, on conn without
 * waiting for anything: not for conn->lock, which another thread holds for
 * the whole of each message it sends, and not for room in the socket.
 * Either could be a wait for a peer that has stopped reading, which would
 * hold up the health check's heartbeats to every node after it, a worker's
 * check on its manager, or a receive thread, which would stop reading.
 *
 * When the lock is free and the socket has room, the heartbeat is written at
 * once.  Otherwise it goes at the head of the send queue
 * (tcp_sendq_push_heartbeat()), for the send thread to write as soon as the
 * message it is writing is done.  Skipping it instead wouldn't do: under
 * steady traffic the lock is seldom free and the socket seldom has room, and
 * a manager hears that a worker is alive only from its heartbeats.  A peer
 * that has stopped reading never gets it; the send that times out on it
 * shuts the connection down.
 *
 * A socket with room for any of the header has room for all of it: poll()
 * reports a TCP socket writable only once a good part of its buffer is free.
 *
 * Returns false if the connection can't be used any more: it is retired, or
 * the send failed, which shuts it down.
 */
static bool tcp_send_heartbeat(TcpBackendPrivate *priv, TcpConnection *conn,
                               TcpMsgType msg_type, uint32_t dst_node)
{
    if (qemu_mutex_trylock(&conn->lock) == 0) {
        int ret = 1; /* not sent: no room */

        if (conn->sockfd < 0) {
            ret = -1;
        } else {
            struct pollfd pfd = {.fd = conn->sockfd, .events = POLLOUT};

            if (poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT)) {
                ret = tcp_send_message(
                    priv, conn, msg_type, NULL, 0,
                    atomic_fetch_add_explicit(&priv->next_seq, 1,
                                              memory_order_relaxed),
                    tcp_local_node(priv), dst_node, 0, 0);
            }
        }
        qemu_mutex_unlock(&conn->lock);
        if (ret <= 0) {
            return ret == 0;
        }
    }
    return tcp_sendq_push_heartbeat(conn, msg_type, dst_node) == 0;
}

/*
 * Close conn's send queue, dropping what is on it, and join its send thread,
 * if it was started.  Nothing is queued afterwards.  The caller has shut the
 * socket down first if the thread may be writing, so a write waiting on the
 * peer fails at once.  Safe to call more than once.
 */
static void tcp_stop_send_thread(TcpConnection *conn)
{
    bool join;

    qemu_mutex_lock(&conn->sendq_lock);
    tcp_sendq_close(conn);
    join = conn->send_thread_started;
    conn->send_thread_started = false;
    qemu_mutex_unlock(&conn->sendq_lock);

    if (join) {
        qemu_thread_join(&conn->send_thread);
    }
}

/*
 * Read from a non-blocking socket into buf until it holds len bytes, of which
 * the first got are already there, waiting for each part as it comes.  A peer
 * can withhold the rest, or trickle it, indefinitely, so the wait gives up
 * once *running is cleared, if running isn't NULL (otherwise the thread's stop
 * would wait on the peer), or once ernic_monotonic_ms() reaches deadline_ms, if
 * deadline_ms isn't zero.  what names the part being read, for the log.
 */
static int tcp_recv_rest(int sockfd, void *buf, size_t len, size_t got,
                         atomic_bool *running, uint64_t deadline_ms,
                         const char *what)
{
    ssize_t ret;

    while (got < len) {
        if (running && !atomic_load(running)) {
            rdma_info_report("TCP: Stopped while receiving %s", what);
            return -1;
        }
        if (deadline_ms != 0 && ernic_monotonic_ms() >= deadline_ms) {
            rdma_error_report("TCP: Timed out receiving %s", what);
            return -1;
        }
        ret = recv(sockfd, (char *)buf + got, len - got, 0);
        if (ret < 0) {
            if (errno == EAGAIN) {
                struct pollfd pfd = {.fd = sockfd, .events = POLLIN};
                poll(&pfd, 1, 5);
                continue;
            }
            rdma_error_report("TCP: Failed to receive %s: %s", what,
                              strerror(errno));
            return -1;
        }
        if (ret == 0) {
            rdma_info_report("TCP: Connection closed during %s", what);
            return -1;
        }
        got += (size_t)ret;
    }

    return 0;
}

/*
 * Receive one message.  The socket is non-blocking.  If nothing has arrived,
 * this returns -EAGAIN for the caller to wait.  Once any of the message has,
 * it reads the whole message, waiting for the rest with tcp_recv_rest():
 * returning early would lose the bytes already read, and the next call would
 * start in the middle of the message.  A header can straddle a TCP segment
 * boundary, so this happens to the header as well as to the payload.  running
 * and deadline_ms bound that wait, as tcp_recv_rest() describes.
 *
 * *payload is set on every return, to NULL unless a payload was received, so
 * a caller that frees it after a failure never frees a buffer from an earlier
 * message.
 */
int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload,
                     TcpBufPool *pool, atomic_bool *running,
                     uint64_t deadline_ms)
{
    ssize_t ret;

    *payload = NULL;

    /* Receive header */
    ret = recv(sockfd, hdr, sizeof(*hdr), 0);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return -EAGAIN;
        }
        rdma_error_report("TCP: Failed to receive header: %s", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        rdma_info_report("TCP: Connection closed by peer");
        return -1;
    }
    if (tcp_recv_rest(sockfd, hdr, sizeof(*hdr), (size_t)ret, running,
                      deadline_ms, "header") < 0) {
        return -1;
    }

    /* Convert from network byte order */
    hdr->magic = ntohl(hdr->magic);
    hdr->msg_type = ntohl(hdr->msg_type);
    hdr->msg_len = ntohl(hdr->msg_len);
    hdr->seq = ntohl(hdr->seq);
    hdr->src_node_id = ntohl(hdr->src_node_id);
    hdr->dst_node_id = ntohl(hdr->dst_node_id);
    hdr->src_qpn = ntohl(hdr->src_qpn);
    hdr->dst_qpn = ntohl(hdr->dst_qpn);

    /* Validate magic */
    if (hdr->magic != TCP_PROTOCOL_MAGIC) {
        rdma_error_report("TCP: Invalid protocol magic: 0x%x", hdr->magic);
        return -1;
    }

    /* Allocate and receive payload */
    if (hdr->msg_len > TCP_MAX_PAYLOAD_LEN) {
        rdma_error_report("TCP: payload too large: %u", hdr->msg_len);
        return -1;
    }
    if (hdr->msg_len > 0) {
        *payload =
            pool ? tcp_bufpool_get(pool, hdr->msg_len) : g_malloc(hdr->msg_len);
        if (tcp_recv_rest(sockfd, *payload, hdr->msg_len, 0, running,
                          deadline_ms, "payload") < 0) {
            g_free(*payload);
            *payload = NULL;
            return -1;
        }
    }

    return 0;
}


/*
 * Start a backend thread whose stop flag is *running.  The flag is set
 * first, because the thread exits as soon as it finds it clear.  If the
 * thread can't be created (the process is out of threads or memory), the
 * flag is cleared again and false is returned: every stop joins only when
 * the flag is set, and a join of a thread that was never created is
 * undefined behavior.
 *
 * No stop may run between the set and the clear.  Callers hold the lock the
 * stop takes first, or start the thread before the stop can reach it.
 */
bool tcp_start_thread(QemuThread *thread, const char *name, void *(*fn)(void *),
                      void *arg, atomic_bool *running)
{
    atomic_store(running, true);
    if (qemu_thread_create(thread, fn, arg) != 0) {
        atomic_store(running, false);
        rdma_error_report("TCP: Failed to start thread %s", name);
        return false;
    }
    return true;
}

/*
 * Start conn's receive thread.  On failure the caller frees conn rather than
 * publishing it.  Callers either hold conn_table_lock or start the thread
 * before tcp_stop_recv_threads() can reach conn.
 */
static bool tcp_start_recv_thread(TcpConnection *conn, const char *name)
{
    return tcp_start_thread(&conn->recv_thread, name, tcp_recv_thread_per_conn,
                            conn, &conn->recv_thread_running);
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
 * Answer a registration on conn by refusing it, with result, a negative
 * errno.  Under conn->lock, like every write to a connection's socket.  The
 * caller then stops reading conn, since nothing the peer sends now is of
 * use; the connection is still pending, and the next reap closes it.
 */
static void tcp_refuse_registration(TcpBackendPrivate *priv,
                                    TcpConnection *conn, uint32_t dst,
                                    int32_t result)
{
    TcpRegisterRespPayload nak;

    memset(&nak, 0, sizeof(nak));
    nak.result = htonl((uint32_t)result);
    nak.version = htonl(TCP_PROTOCOL_VERSION);

    qemu_mutex_lock(&conn->lock);
    tcp_send_message(
        priv, conn, TCP_MSG_REGISTER_RESP, &nak, sizeof(nak),
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
        tcp_local_node(priv), dst, 0, 0);
    qemu_mutex_unlock(&conn->lock);
}

/* Per-connection receive thread */
void *tcp_recv_thread_per_conn(void *opaque)
{
    TcpConnection *conn = (TcpConnection *)opaque;
    TcpMsgHeader hdr;
    void *payload = NULL;
    int ret;
    struct pollfd pfd;
    bool hang_up = false; /* done with the peer: close after this message */

    rdma_info_report("TCP: Receive thread started for node %u", conn->node_id);

    pfd.fd = conn->sockfd;
    pfd.events = POLLIN;

    while (atomic_load(&conn->recv_thread_running)) {
        ret = poll(&pfd, 1, 100); /* 100ms timeout */
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            rdma_error_report("TCP: Poll error on node %u: %s", conn->node_id,
                              strerror(errno));
            break;
        }
        if (ret == 0) {
            continue; /* Timeout */
        }

        if (pfd.revents & POLLIN) {
            ret = tcp_recv_message(conn->sockfd, &hdr, &payload,
                                   conn->priv ? &conn->priv->recv_pool : NULL,
                                   &conn->recv_thread_running, 0);
            if (ret == -EAGAIN) {
                continue;
            }
            if (ret < 0) {
                rdma_info_report("TCP: Receive error on node %u, closing",
                                 conn->node_id);
                break;
            }
            atomic_store(&conn->last_rx_ms, ernic_monotonic_ms());

            /*
             * A manager's connection that hasn't registered (its node id is
             * set only by registration, on this thread) may only register.
             * A worker registers before it sends anything else, so anything
             * else is a peer that isn't one, and it gets no further: a relay
             * or a reply sent for it could wait on a node that has stopped
             * reading, and the reaper that joins this thread would wait too.
             */
            if (conn->priv && conn->priv->is_manager &&
                conn->node_id == UINT32_MAX &&
                hdr.msg_type != TCP_MSG_REGISTER_NODE) {
                rdma_error_report("TCP: message type %u from %s:%u, which "
                                  "hasn't registered; closing the connection",
                                  hdr.msg_type, conn->remote_host,
                                  conn->remote_port);
                hang_up = true;
                goto message_done;
            }

            /* Handle message based on type */
            switch (hdr.msg_type) {
            case TCP_MSG_HANDSHAKE:
                rdma_info_report("TCP: Received handshake from node %u",
                                 hdr.src_node_id);
                break;

            case TCP_MSG_HANDSHAKE_RESP:
                rdma_info_report(
                    "TCP: Received handshake response from node %u",
                    hdr.src_node_id);
                /* Handshake complete, connection is established */
                break;

            case TCP_MSG_REGISTER_NODE: {
                /* Manager receives registration from worker */
                TcpBackendPrivate *priv = conn->priv;
                if (!priv || !priv->is_manager) {
                    rdma_error_report(
                        "TCP: REGISTER_NODE received by non-manager");
                    break;
                }

                if (!payload || hdr.msg_len < TCP_REGISTER_NODE_LEN_V3) {
                    rdma_error_report("TCP: Invalid REGISTER_NODE payload");
                    break;
                }

                /*
                 * Refuse a peer that does not speak this protocol version.
                 * A short payload is one that predates the version field
                 * entirely; both cases get the same answer, with a result
                 * the worker surfaces instead of timing out.
                 */
                uint32_t peer_version = 0;

                if (hdr.msg_len >= sizeof(TcpRegisterNodePayload)) {
                    peer_version =
                        ntohl(((TcpRegisterNodePayload *)payload)->version);
                }

                if (peer_version != TCP_PROTOCOL_VERSION) {
                    rdma_error_report(
                        "TCP: refusing registration from a v%u peer "
                        "(this mesh is v%u); a peer that predates the GID "
                        "advertisement routes every destination to one "
                        "default node",
                        peer_version, TCP_PROTOCOL_VERSION);
                    tcp_refuse_registration(priv, conn, hdr.src_node_id,
                                            -EPROTO);
                    hang_up = true;
                    break;
                }

                TcpRegisterNodePayload *reg = (TcpRegisterNodePayload *)payload;
                uint32_t requested_id = ntohl(reg->requested_node_id);
                uint16_t worker_port = ntohs(reg->port);
                char worker_host[256];
                /* The hostname comes off the wire and need not be
                 * terminated, so bound the read to the field. */
                size_t host_len =
                    strnlen(reg->hostname, sizeof(worker_host) - 1);
                memcpy(worker_host, reg->hostname, host_len);
                worker_host[host_len] = '\0';

                /* Nodes gone from the mesh, withdrawn after the unlock */
                uint32_t stale[TCP_MAX_MESH_NODES];
                uint32_t num_stale = 0;

                qemu_mutex_lock(&priv->mesh_table_lock);

                /*
                 * Nodes that can no longer be reached, whose GIDs are
                 * withdrawn once the tables are unlocked. A worker that
                 * restarted is among them, under the id it had before, and
                 * it is about to claim those GIDs again under its new one.
                 *
                 * Before conn_table_lock, not under it: the lookups inside
                 * take that lock themselves.
                 */
                num_stale = tcp_collect_dead_claimants(priv, stale);

                qemu_mutex_lock(&priv->conn_table_lock);

                /*
                 * Only a connection on the pending list can register, and
                 * only once: taking it off the list claims the registration.
                 * A second REGISTER_NODE on the same socket would otherwise
                 * put the connection in the table under a second id, and
                 * teardown would free it once for each.
                 */
                if (!g_ptr_array_remove_fast(priv->pending_conns, conn)) {
                    qemu_mutex_unlock(&priv->conn_table_lock);
                    qemu_mutex_unlock(&priv->mesh_table_lock);
                    rdma_error_report(
                        "TCP: ignoring REGISTER_NODE on node %u's connection, "
                        "which is not waiting to register",
                        conn->node_id);
                    break;
                }


                /*
                 * A new node gets the next id no node or connection has.  A
                 * worker that asks for an id is asking for its own back,
                 * having connected again (tcp_register_again()); it gets
                 * that or nothing, never another, which would leave it
                 * claiming its GIDs under two ids.
                 */
                uint32_t assigned_id = requested_id;
                bool again = requested_id != 0xFFFFFFFF;
                TcpConnection *replaced = NULL;
                const char *why = NULL;
                int refused = 0;

                if (again) {
                    refused =
                        tcp_register_again(priv, requested_id, worker_host,
                                           worker_port, &replaced, &why);
                } else {
                    assigned_id = priv->next_available_node_id;
                    while (tcp_node_id_taken(priv, assigned_id)) {
                        assigned_id++;
                    }
                    priv->next_available_node_id = assigned_id + 1;

                    MeshNodeInfo *node = tcp_mesh_node_info_new(
                        assigned_id, worker_host, worker_port);
                    if (!node) {
                        rdma_error_report("TCP: Failed to create mesh node "
                                          "info for worker node %u",
                                          assigned_id);
                        /* Still unregistered, so teardown has to find it */
                        g_ptr_array_add(priv->pending_conns, conn);
                        qemu_mutex_unlock(&priv->conn_table_lock);
                        qemu_mutex_unlock(&priv->mesh_table_lock);
                        break;
                    }
                    g_hash_table_insert(priv->mesh_nodes,
                                        GUINT_TO_POINTER(assigned_id), node);
                }

                if (refused != 0) {
                    /* Still unregistered, so the reap closes it */
                    g_ptr_array_add(priv->pending_conns, conn);
                    qemu_mutex_unlock(&priv->conn_table_lock);
                    qemu_mutex_unlock(&priv->mesh_table_lock);
                    rdma_error_report("TCP: refusing a registration from "
                                      "%s:%u as node %u: %s",
                                      worker_host, worker_port, requested_id,
                                      why);
                    tcp_refuse_registration(priv, conn, hdr.src_node_id,
                                            refused);
                    hang_up = true;
                    break;
                }

                /*
                 * The connection takes its reference from pending_conns
                 * with it into connections, and can't be refused:
                 * assigned_id has no entry there, or had one that
                 * tcp_register_again() just took out.
                 */
                conn->node_id = assigned_id;
                tcp_conn_table_add(priv, assigned_id, conn);
                qemu_mutex_unlock(&priv->conn_table_lock);

                if (again) {
                    MeshNodeInfo *node = g_hash_table_lookup(
                        priv->mesh_nodes, GUINT_TO_POINTER(assigned_id));

                    node->is_alive = true;
                    node->last_heartbeat_ms = ernic_monotonic_ms();
                    /*
                     * The node's addresses are its own again, without its
                     * advertising them: a node the health check declared
                     * dead had its claims taken out of resolution, and the
                     * broadcast below would otherwise tell the workers it
                     * owns nothing until its advertisement came.  Under
                     * mesh_table_lock, with the connection published, so
                     * tcp_withdraw_if_still_unreachable() can't act on a
                     * stale finding of it dead in between.
                     */
                    tcp_gid_claims_set_alive(priv, assigned_id, true);
                }
                /* For the answer, read under the lock that guards the table */
                uint32_t num_nodes = g_hash_table_size(priv->mesh_nodes);
                qemu_mutex_unlock(&priv->mesh_table_lock);

                if (replaced) {
                    tcp_retire_conn(replaced);
                    tcp_connection_unref(replaced);
                }
                if (again) {
                    rdma_info_report("TCP: Node %u registered again from "
                                     "%s:%u",
                                     assigned_id, worker_host, worker_port);
                } else {
                    rdma_info_report("TCP: Registered node %u at %s:%u",
                                     assigned_id, worker_host, worker_port);
                }

                /*
                 * Marked not-alive rather than discarded: that is enough to
                 * take the node out of resolution, so it stops contesting
                 * the GIDs the registering node is about to claim, and it
                 * leaves the claims to restore if the node answers a
                 * heartbeat again, or registers again under its own id,
                 * rather than having restarted.
                 *
                 * The whole table is broadcast below, so the withdrawal
                 * reaches the other workers with the rest of it.
                 */
                for (uint32_t i = 0; i < num_stale; i++) {
                    uint32_t id = stale[i];

                    if (id == assigned_id)
                        continue;
                    if (tcp_withdraw_if_still_unreachable(priv, id))
                        rdma_info_report("TCP: node %u is gone; withdrawing "
                                         "its GIDs as node %u registers",
                                         id, assigned_id);
                }

                /* Send registration response.  Zeroed first: the whole
                 * struct goes on the wire, so any field not assigned below
                 * would put stack contents there. */
                TcpRegisterRespPayload resp;
                memset(&resp, 0, sizeof(resp));
                resp.assigned_node_id = htonl(assigned_id);
                resp.num_nodes = htonl(num_nodes);
                resp.result = htonl(0); /* Success */
                resp.version = htonl(TCP_PROTOCOL_VERSION);

                /*
                 * conn->lock serialises writes to this socket.
                 * The send thread holds it across the data path's
                 * multi-megabyte messages, so an unlocked control
                 * message here would interleave into the middle of
                 * one and the peer would read a header from the
                 * payload.
                 * Released before broadcasting: that path takes
                 * conn locks of its own and this mutex is not
                 * recursive.
                 */
                qemu_mutex_lock(&conn->lock);
                tcp_send_message(priv, conn, TCP_MSG_REGISTER_RESP, &resp,
                                 sizeof(resp),
                                 atomic_fetch_add_explicit(
                                     &priv->next_seq, 1, memory_order_relaxed),
                                 tcp_local_node(priv), assigned_id, 0, 0);
                qemu_mutex_unlock(&conn->lock);

                /* Broadcast updated topology to all nodes */
                tcp_broadcast_mesh_topology(priv);

                /*
                 * The joining worker missed every advertisement sent before
                 * it registered, so replay the whole table to everyone.  Its
                 * own GIDs follow once it sends them.
                 */
                tcp_broadcast_gid_table(priv);
                break;
            }

            case TCP_MSG_REGISTER_RESP: {
                /* Worker receives registration response from manager */
                TcpBackendPrivate *priv = conn->priv;
                if (!priv || priv->is_manager) {
                    rdma_error_report("TCP: REGISTER_RESP received by manager");
                    break;
                }

                /*
                 * Only the manager assigns this node its id, and only once
                 * for each registration.  The answer sets the id and the
                 * GIDs this node claims, and which connection it takes the
                 * mesh's word from, so one accepted from a peer, or a second
                 * one, would let another node take this one's place in the
                 * mesh.  tcp_worker_registration_done() takes only the first
                 * answer on the connection this node opened to the manager
                 * itself to register; a peer's can't be it.
                 */
                bool first = !tcp_worker_registered(priv);

                /* The id to record: 0 if the answer refuses this node */
                uint32_t assigned_id = 0;

                if (!payload || hdr.msg_len < TCP_REGISTER_RESP_LEN_V3) {
                    rdma_error_report("TCP: Invalid REGISTER_RESP payload");
                } else {
                    TcpRegisterRespPayload *resp =
                        (TcpRegisterRespPayload *)payload;
                    uint32_t resp_id = ntohl(resp->assigned_node_id);
                    int32_t result = (int32_t)ntohl(resp->result);

                    /*
                     * The other half of the check the manager makes: a
                     * pre-v4 manager sends no version and would otherwise
                     * admit this node to a mesh whose resolver still
                     * guesses.  Refusing here is what makes the gate work in
                     * both directions, since an old manager cannot refuse
                     * us.
                     */
                    uint32_t mgr_version = 0;

                    if (hdr.msg_len >= sizeof(TcpRegisterRespPayload)) {
                        mgr_version = ntohl(resp->version);
                    }

                    if (mgr_version != TCP_PROTOCOL_VERSION) {
                        rdma_error_report(
                            "TCP: manager speaks v%u, this node speaks v%u; "
                            "refusing to join -- a pre-v%u mesh resolves "
                            "every destination GID to one default node",
                            mgr_version, TCP_PROTOCOL_VERSION,
                            TCP_PROTOCOL_VERSION);
                    } else if (result != 0) {
                        rdma_error_report("TCP: Registration failed: %d",
                                          result);
                    } else if (resp_id == 0 || resp_id == UINT32_MAX) {
                        /*
                         * 0 is the manager's own id, and UINT32_MAX means
                         * no node at all, to the GID table and to the data
                         * path (tcp_backend_local_node_id()).
                         */
                        rdma_error_report("TCP: manager assigned node id %u, "
                                          "which no worker can have",
                                          resp_id);
                    } else {
                        assigned_id = resp_id;
                    }
                }

                /*
                 * A refusal ends the wait too, so tcp_init() fails at once
                 * rather than at the registration timeout.
                 */
                if (!tcp_worker_registration_done(priv, conn, &assigned_id)) {
                    rdma_error_report("TCP: ignoring a REGISTER_RESP that "
                                      "answers no registration under way");
                    break;
                }
                if (assigned_id == 0) {
                    break;
                }

                if (first) {
                    rdma_info_report(
                        "TCP: Registered with manager, assigned node_id=%u",
                        assigned_id);
                } else {
                    rdma_info_report("TCP: Registered with the manager again "
                                     "as node %u",
                                     assigned_id);
                }

                /*
                 * The node id is only known now, so this is the first point
                 * at which this node can say which GIDs are its own.  On
                 * registering again, a change this node made to them while
                 * it had no connection to the manager was never sent.
                 */
                tcp_advertise_local_gids(priv);
                break;
            }

            case TCP_MSG_MESH_TOPOLOGY: {
                /* Worker receives mesh topology from manager */
                TcpBackendPrivate *priv = conn->priv;
                if (!priv || priv->is_manager) {
                    rdma_error_report("TCP: MESH_TOPOLOGY received by manager");
                    break;
                }

                if (!payload || hdr.msg_len < sizeof(TcpMeshTopologyPayload)) {
                    rdma_error_report("TCP: Invalid MESH_TOPOLOGY payload");
                    break;
                }

                /*
                 * Authorise on the connection, as for NODE_GIDS: only the
                 * manager says who is in the mesh.  An update decides which
                 * peers this worker connects to, and which of its
                 * connections it closes (tcp_worker_peer_sweep()), so one
                 * from a peer could take down the worker's links.
                 *
                 * That also ignores an update that comes before the answer
                 * to this node's registration, which is what publishes
                 * manager_conn.  Which peers this node connects to depends
                 * on its id.  The manager publishes a new worker's
                 * connection before it answers the registration, so the
                 * broadcast for another node's registration can come first.
                 * It follows its answer with a broadcast of its own, so
                 * nothing is lost.
                 */
                if (conn != atomic_load(&priv->manager_conn)) {
                    rdma_error_report("TCP: MESH_TOPOLOGY from a connection "
                                      "that is not the manager's, or before "
                                      "registration; ignoring");
                    break;
                }

                TcpMeshTopologyPayload *topo =
                    (TcpMeshTopologyPayload *)payload;
                uint32_t num_nodes = ntohl(topo->num_nodes);

                rdma_info_report("TCP: Received mesh topology with %u nodes",
                                 num_nodes);

                tcp_worker_set_peers(priv, topo, num_nodes);
                break;
            }

            case TCP_MSG_NODE_GIDS: {
                TcpBackendPrivate *priv = conn->priv;

                if (!priv || !payload ||
                    hdr.msg_len < sizeof(TcpNodeGidsPayload)) {
                    rdma_error_report("TCP: Invalid NODE_GIDS payload");
                    break;
                }

                const TcpNodeGidsPayload *adv =
                    (const TcpNodeGidsPayload *)payload;
                uint32_t node_id = ntohl(adv->node_id);
                uint32_t num_gids = ntohl(adv->num_gids);

                if (num_gids > TCP_MAX_NODE_GIDS) {
                    rdma_error_report("TCP: NODE_GIDS from node %u claims %u "
                                      "GIDs, max is %d",
                                      hdr.src_node_id, num_gids,
                                      TCP_MAX_NODE_GIDS);
                    break;
                }

                /*
                 * Authorise on the connection, not on the message.  Both
                 * hdr.src_node_id and the payload's node_id are written by
                 * the sender, so comparing them to each other proves
                 * nothing: a worker can set both to a peer's id and replace
                 * that peer's GIDs mesh-wide.  conn is what the local node
                 * established.
                 *
                 * On the manager a worker speaks only for itself, and only
                 * once registration has given its connection an id --
                 * before that conn->node_id is UINT32_MAX and the socket
                 * has proved nothing at all.
                 *
                 * On a worker the only legitimate source is the manager
                 * relaying on others' behalf, which is the one connection
                 * this node opened itself, once the manager has accepted its
                 * registration on it.  Matching on src_node_id == 0 instead
                 * would let any peer claim to be the manager.
                 */
                if (priv->is_manager) {
                    if (conn->node_id == UINT32_MAX ||
                        conn->node_id != node_id) {
                        rdma_error_report(
                            "TCP: connection for node %u tried to set the "
                            "GIDs of node %u; ignoring",
                            conn->node_id, node_id);
                        break;
                    }
                } else if (conn != atomic_load(&priv->manager_conn)) {
                    rdma_error_report("TCP: NODE_GIDS for node %u from a "
                                      "connection that is not the manager's, "
                                      "or before registration; ignoring",
                                      node_id);
                    break;
                }

                tcp_gid_claims_set(priv, node_id, adv->gids, num_gids);

                /* Manager fans a worker's advertisement out to the rest. */
                if (priv->is_manager)
                    tcp_broadcast_gid_table(priv);
                break;
            }

            case TCP_MSG_HEARTBEAT: {
                TcpBackendPrivate *priv = conn->priv;
                if (!priv)
                    break;

                if (priv->is_manager) {
                    /*
                     * Manager received heartbeat reply from worker —
                     * refresh the worker's liveness timestamp.  The worker
                     * is the one this connection belongs to, not the one
                     * the header names: the sender writes src_node_id, and
                     * trusting it would let any node keep another, dead,
                     * node alive.  Every connection that gets this far is a
                     * registered node's, and carries that node's id.
                     *
                     * A worker's own heartbeat, when it has heard nothing
                     * from the manager for an interval, comes here too: the
                     * answer is what tells it the manager is still there.
                     */
                    bool revived = false;

                    qemu_mutex_lock(&priv->mesh_table_lock);
                    MeshNodeInfo *node = g_hash_table_lookup(
                        priv->mesh_nodes, GUINT_TO_POINTER(conn->node_id));
                    if (node) {
                        node->last_heartbeat_ms = ernic_monotonic_ms();
                        revived = !node->is_alive;
                        node->is_alive = true;
                    }
                    qemu_mutex_unlock(&priv->mesh_table_lock);

                    /*
                     * Without waiting for the connection's lock: this thread
                     * waiting for another to write to the node would stop
                     * reading from it, and a node doing the same would leave
                     * both sockets full, until a send timed out.
                     */
                    tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT_RESP,
                                       conn->node_id);

                    /*
                     * The rest of the mesh was told the node is dead, and
                     * won't connect to it until it hears otherwise.
                     */
                    if (revived) {
                        rdma_info_report("TCP: Node %u is answering again",
                                         conn->node_id);
                        tcp_gid_claims_set_alive(priv, conn->node_id, true);
                        tcp_broadcast_mesh_topology(priv);
                        tcp_broadcast_gid_table(priv);
                    }
                } else {
                    /* Worker received heartbeat probe from manager —
                     * echo it back so the manager refreshes our
                     * liveness timestamp.  Without waiting for the lock,
                     * as the manager answers. */
                    tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT,
                                       hdr.src_node_id);
                }
                break;
            }

            case TCP_MSG_HEARTBEAT_RESP: {
                /*
                 * Acknowledgement — no action needed.  That it came at all,
                 * recorded in last_rx_ms as for any message, is what a
                 * worker that sent a heartbeat is waiting to see.
                 */
                break;
            }

            case TCP_MSG_IONIC: {
                TcpBackendPrivate *priv = conn->priv;

                if (!priv || !payload || hdr.msg_len == 0)
                    break;

                if (hdr.dst_node_id == tcp_local_node(priv)) {
                    tcp_ionic_recv_fn fn;
                    void *fn_opaque;

                    qemu_mutex_lock(&priv->lock);
                    fn = priv->ionic_recv_fn;
                    fn_opaque = priv->ionic_recv_opaque;
                    qemu_mutex_unlock(&priv->lock);

                    if (fn)
                        fn(fn_opaque, hdr.src_node_id, payload, hdr.msg_len);
                    else
                        rdma_error_report("TCP: ionic message from node %u "
                                          "with no data path registered",
                                          hdr.src_node_id);
                } else if (priv->is_manager) {
                    /* Worker-to-worker: relay to the addressed node. */
                    TcpConnection *fwd =
                        tcp_connection_lookup(priv, hdr.dst_node_id);
                    bool relayed = false;

                    if (fwd && atomic_load(&fwd->is_connected)) {
                        qemu_mutex_lock(&fwd->lock);
                        /*
                         * On this thread, so while the send waits this
                         * node isn't read.  The manager's shorter timeout
                         * (tcp_send_timeout_ms()) is what keeps a
                         * destination that has stopped reading from costing
                         * the sender its connection to the manager.
                         */
                        relayed =
                            fwd->sockfd >= 0 &&
                            tcp_send_message(priv, fwd, TCP_MSG_IONIC, payload,
                                             hdr.msg_len, hdr.seq,
                                             hdr.src_node_id, hdr.dst_node_id,
                                             hdr.src_qpn, hdr.dst_qpn) == 0;
                        qemu_mutex_unlock(&fwd->lock);
                    }
                    tcp_connection_unref(fwd);
                    if (!relayed) {
                        rdma_error_report("TCP: cannot relay ionic message "
                                          "from node %u to node %u",
                                          hdr.src_node_id, hdr.dst_node_id);
                    }
                }
                break;
            }

            default:
                rdma_warn_report("TCP: Unknown message type %u from node %u",
                                 hdr.msg_type, hdr.src_node_id);
                break;
            }

        message_done:
            if (payload) {
                if (conn->priv)
                    tcp_bufpool_put(&conn->priv->recv_pool, payload,
                                    hdr.msg_len);
                else
                    g_free(payload);
                payload = NULL;
            }

            if (hang_up)
                break;
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            rdma_info_report("TCP: Connection to node %u closed or error",
                             conn->node_id);
            break;
        }
    }

    rdma_info_report("TCP: Receive thread exiting for node %u", conn->node_id);
    /*
     * Senders then pass the connection over (a worker's go through the
     * manager's relay instead) rather than send into a socket the peer has
     * left.  Cleared first, so a thread that finds the connection dead also
     * finds it can't be sent on.
     */
    atomic_store(&conn->is_connected, false);
    atomic_store(&conn->recv_thread_exited, true);
    return NULL;
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
static void tcp_reap_pending_conns(TcpBackendPrivate *priv)
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

        rdma_info_report("TCP: Closing the connection from %s:%u, which "
                         "didn't register",
                         conn->remote_host, conn->remote_port);
        tcp_retire_conn(conn);
        tcp_connection_unref(conn);
    }
    g_ptr_array_free(reaped, TRUE);
}

/* Accept thread - accepts incoming connections and adds them to table */
void *tcp_accept_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    char client_host[INET_ADDRSTRLEN];
    int sockfd;
    TcpMsgHeader hdr;
    void *payload = NULL;
    TcpHandshakePayload *hs_payload;
    TcpConnection *conn, *old;
    char thread_name[32];

    rdma_info_report("TCP: Accept thread started");

    while (atomic_load(&priv->accept_thread_running)) {
        client_len = sizeof(client_addr);
        sockfd = accept4(priv->listen_fd, (struct sockaddr *)&client_addr,
                         &client_len, SOCK_CLOEXEC | SOCK_NONBLOCK);

        /* The listen socket is blocking, so there's no EAGAIN to retry */
        if (sockfd < 0) {
            if (!atomic_load(&priv->accept_thread_running)) {
                break;
            }
            rdma_error_report("TCP: Failed to accept connection: %s",
                              strerror(errno));
            /*
             * When the process is out of descriptors (EMFILE), the
             * connection stays queued and the next accept() fails at once
             * too, so retrying without a pause spins and floods the log.
             * Reaping may free some descriptors.
             */
            if (priv->is_manager) {
                tcp_reap_pending_conns(priv);
            }
            ernic_sleep_us(100000); /* 100 ms */
            continue;
        }

        /* Disable Nagle -- send immediately */
        int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        tcp_tune_sockbuf(sockfd);

        /* Can't fail for AF_INET into INET_ADDRSTRLEN bytes */
        if (!inet_ntop(AF_INET, &client_addr.sin_addr, client_host,
                       sizeof(client_host))) {
            rdma_error_report("TCP: Failed to format the address of an "
                              "incoming connection: %s",
                              strerror(errno));
            close(sockfd);
            continue;
        }

        rdma_info_report("TCP: Accepted connection from %s:%u", client_host,
                         ntohs(client_addr.sin_port));

        if (priv->is_manager) {
            /* Make room before counting what's waiting */
            tcp_reap_pending_conns(priv);

            /* Manager mode: accept connection and let receive thread handle
             * REGISTER_NODE.  The node id is unknown until registration. */
            conn = tcp_connection_new(UINT32_MAX, client_host,
                                      ntohs(client_addr.sin_port));
            conn->sockfd = sockfd;
            atomic_store(&conn->is_connected, true);
            conn->priv = priv;
            conn->accepted_ms = ernic_monotonic_ms();

            /*
             * Start receive thread - it will handle REGISTER_NODE, which
             * moves the connection to the connections table.  The lock is
             * held until the connection is on the pending list, so the move
             * can't happen before it's there.
             */
            snprintf(thread_name, sizeof(thread_name), "tcp-recv-pending");
            qemu_mutex_lock(&priv->conn_table_lock);
            if (priv->pending_conns->len >= TCP_MAX_PENDING_CONNS) {
                qemu_mutex_unlock(&priv->conn_table_lock);
                rdma_error_report("TCP: %u connections are already waiting "
                                  "to register; closing the one from %s:%u",
                                  TCP_MAX_PENDING_CONNS, conn->remote_host,
                                  conn->remote_port);
                tcp_connection_unref(conn);
                continue;
            }
            if (!tcp_start_recv_thread(conn, thread_name)) {
                qemu_mutex_unlock(&priv->conn_table_lock);
                tcp_connection_unref(conn);
                continue;
            }
            g_ptr_array_add(priv->pending_conns, conn);
            qemu_mutex_unlock(&priv->conn_table_lock);
        } else {
            /*
             * Worker mode: receive handshake to identify remote node.  The
             * deadline covers the whole handshake, including a wait inside
             * tcp_recv_message() for the rest of one that has started, so a
             * peer that sends only part of one holds up the peers behind it
             * no longer than one that sends nothing.
             */
            uint64_t deadline_ms =
                ernic_monotonic_ms() + TCP_HANDSHAKE_TIMEOUT_MS;
            int recv_ret;

            for (;;) {
                recv_ret =
                    tcp_recv_message(sockfd, &hdr, &payload, NULL,
                                     &priv->accept_thread_running, deadline_ms);
                if (recv_ret == 0) {
                    /* Message received successfully */
                    if (hdr.msg_type == TCP_MSG_HANDSHAKE) {
                        /* Success! */
                        break;
                    } else {
                        rdma_error_report(
                            "TCP: Expected handshake, got message type %u",
                            hdr.msg_type);
                        close(sockfd);
                        if (payload) {
                            g_free(payload);
                        }
                        goto next_connection;
                    }
                } else if (recv_ret == -EAGAIN) {
                    /*
                     * Nothing yet.  A peer can connect and send nothing, so
                     * check the flag before each wait, or a stop would wait
                     * out the deadline.
                     */
                    if (!atomic_load(&priv->accept_thread_running)) {
                        rdma_info_report(
                            "TCP: Stopped while waiting for a handshake");
                        close(sockfd);
                        goto next_connection;
                    }
                    if (ernic_monotonic_ms() >= deadline_ms) {
                        rdma_error_report("TCP: Timeout waiting for handshake "
                                          "from incoming connection");
                        close(sockfd);
                        goto next_connection;
                    }
                    /*
                     * Wait for it, but no more than 100 ms at a time:
                     * stopping the thread doesn't wake this poll().
                     */
                    struct pollfd pfd = {.fd = sockfd, .events = POLLIN};

                    poll(&pfd, 1, 100);
                    continue;
                } else {
                    /* An error, a hang-up, a stop, or the deadline */
                    rdma_error_report("TCP: Error receiving handshake from "
                                      "incoming connection");
                    close(sockfd);
                    if (payload) {
                        g_free(payload);
                    }
                    goto next_connection;
                }
            }

            /* The length is the peer's to choose, down to no payload at all */
            if (!payload || hdr.msg_len < sizeof(TcpHandshakePayload)) {
                rdma_error_report("TCP: Handshake payload is %u bytes, "
                                  "need %zu",
                                  hdr.msg_len, sizeof(TcpHandshakePayload));
                close(sockfd);
                g_free(payload);
                payload = NULL;
                continue;
            }

            hs_payload = (TcpHandshakePayload *)payload;
            uint32_t remote_node_id = ntohl(hs_payload->node_id);
            uint32_t remote_version = ntohl(hs_payload->version);

            rdma_info_report("TCP: Handshake from node %u (v%u)",
                             remote_node_id, remote_version);

            if (remote_version != TCP_PROTOCOL_VERSION) {
                rdma_error_report(
                    "TCP: Protocol version mismatch: local=%u remote=%u",
                    TCP_PROTOCOL_VERSION, remote_version);
                close(sockfd);
                g_free(payload);
                payload = NULL;
                continue;
            }

            /*
             * A live connection to the node is kept, and this one closed:
             * replacing it would let anyone who can reach this port take
             * over a working link by claiming its node id.  A dead one is
             * replaced, or the node could never connect again, and retired
             * once the lock is dropped, since that joins its thread.
             */
            qemu_mutex_lock(&priv->conn_table_lock);
            old = g_hash_table_lookup(priv->connections,
                                      GUINT_TO_POINTER(remote_node_id));
            if (old && !tcp_conn_dead(old)) {
                qemu_mutex_unlock(&priv->conn_table_lock);
                rdma_info_report(
                    "TCP: Connection to node %u already exists, closing "
                    "duplicate",
                    remote_node_id);
                close(sockfd);
                g_free(payload);
                payload = NULL;
                continue;
            }

            /* Create connection object */
            conn = tcp_connection_new(remote_node_id, client_host,
                                      ntohs(client_addr.sin_port));
            conn->sockfd = sockfd;
            atomic_store(&conn->is_connected, true);
            conn->priv = priv;

            /*
             * Start the receive thread before the connection is published,
             * so a connection without one is never in the table.
             */
            snprintf(thread_name, sizeof(thread_name), "tcp-recv-%u",
                     remote_node_id);
            if (!tcp_start_recv_thread(conn, thread_name)) {
                qemu_mutex_unlock(&priv->conn_table_lock);
                tcp_connection_unref(conn);
                g_free(payload);
                payload = NULL;
                continue;
            }

            /*
             * Add to connection table, which takes over the reference from
             * tcp_connection_new(), so take another for the response below.
             * Neither the take nor the add can fail: the lock has been held
             * since the lookup above.
             */
            if (old) {
                tcp_conn_table_take(priv, remote_node_id, old);
            }
            tcp_connection_ref(conn);
            tcp_conn_table_add(priv, remote_node_id, conn);
            qemu_mutex_unlock(&priv->conn_table_lock);

            if (old) {
                rdma_info_report("TCP: Replaced the dead connection from node "
                                 "%u",
                                 remote_node_id);
                tcp_retire_conn(old);
                tcp_connection_unref(old);
            }

            /* Send handshake response */
            tcp_send_handshake(conn, tcp_local_node(priv),
                               TCP_MSG_HANDSHAKE_RESP);
            tcp_connection_unref(conn);

            g_free(payload);
            payload = NULL;
        }

    next_connection:
        continue;
    }

    rdma_info_report("TCP: Accept thread exiting");
    return NULL;
}

/*
 * Manager/Worker Helper Functions
 */

/*
 * A broadcast carries at most TCP_MAX_MESH_NODES nodes.  If mesh_nodes holds
 * more, the ones past the cap -- live nodes as likely as dead ones, in
 * hash-table order -- drop out of the topology and the GID table, which
 * breaks the mesh with no other sign, so say so.  @table_size is the size of
 * mesh_nodes the broadcast found.
 */
static void tcp_report_mesh_overflow(const char *broadcast, guint table_size)
{
    rdma_error_report("TCP: the mesh table holds %u nodes, but the %s carries "
                      "at most %u; %u nodes were left out",
                      table_size, broadcast, TCP_MAX_MESH_NODES,
                      table_size - TCP_MAX_MESH_NODES);
}

/*
 * Broadcast mesh topology to all connected workers (manager only).  The
 * caller holds no lock; see topology_broadcast_lock.
 */
void tcp_broadcast_mesh_topology(TcpBackendPrivate *priv)
{
    if (!priv->is_manager) {
        return;
    }

    /* The whole payload goes on the wire, including the node slots past
     * num_nodes, so the struct MUST be zeroed to avoid sending stack
     * contents when there are unused nodes.
     */
    TcpMeshTopologyPayload topo;
    memset(&topo, 0, sizeof(topo));
    uint32_t num_nodes = 0;
    guint overflow = 0; /* the size of mesh_nodes, if past the cap */

    qemu_mutex_lock(&priv->topology_broadcast_lock);
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

        MeshNodeInfo *node = (MeshNodeInfo *)value;
        topo.nodes[num_nodes].node_id = htonl(node->node_id);
        strncpy(topo.nodes[num_nodes].hostname, node->hostname, 255);
        topo.nodes[num_nodes].hostname[255] = '\0';
        topo.nodes[num_nodes].port = htons(node->port);
        topo.nodes[num_nodes].is_alive = node->is_alive ? 1 : 0;
        topo.nodes[num_nodes].reserved[0] = 0;
        topo.nodes[num_nodes].reserved[1] = 0;
        topo.nodes[num_nodes].reserved[2] = 0;
        num_nodes++;
    }

    topo.num_nodes = htonl(num_nodes);

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
            tcp_send_message(priv, conn, TCP_MSG_MESH_TOPOLOGY, &topo,
                             sizeof(TcpMeshTopologyPayload),
                             atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                       memory_order_relaxed),
                             tcp_local_node(priv), conn->node_id, 0, 0);
        }
        qemu_mutex_unlock(&conn->lock);
    }
    g_ptr_array_free(conns, TRUE);
    qemu_mutex_unlock(&priv->topology_broadcast_lock);

    rdma_info_report("TCP: Broadcast mesh topology to %u nodes", num_nodes);
}

/*
 * Build the advertisement for @node_id's GIDs.  The whole fixed-size struct
 * goes on the wire including the unused slots, so it is zeroed first -- the
 * same stack-disclosure trap tcp_broadcast_mesh_topology() guards against.
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
static void tcp_advertise_local_gids(TcpBackendPrivate *priv)
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
     * rest of this function uses: tcp_broadcast_gid_table() and
     * tcp_send_node_gids() take connection locks, which a sender holds
     * while it waits for a peer to read, so lock cannot be held that long.
     */
    uint8_t gids[TCP_MAX_NODE_GIDS][16];
    uint32_t num_gids;

    qemu_mutex_lock(&priv->lock);
    num_gids = priv->num_local_gids;
    memcpy(gids, priv->local_gids, sizeof(gids));
    qemu_mutex_unlock(&priv->lock);

    tcp_gid_claims_set(priv, node, tcp_const_gids(gids), num_gids);

    if (priv->is_manager) {
        /* The manager is the authority: push the whole table, which is also
         * how a claimant learns its advertisement was rejected. */
        tcp_broadcast_gid_table(priv);
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

/* Send one advertisement on an already-locked-free connection. */
static void tcp_send_node_gids(TcpBackendPrivate *priv, TcpConnection *conn,
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
 * Manager: push the authoritative GID set of every known node to every
 * connection, one message per node.
 *
 * Every node, not every node with claims: a node whose claims were all
 * withdrawn or all lost to a conflict needs an empty set sent for it, or
 * workers keep resolving its old GIDs forever.  And every connection,
 * including the node the message is about: that is how a claimant learns
 * its own advertisement was rejected, which it cannot work out alone.
 *
 * Called whenever claims change and when a worker joins, so a late arrival
 * learns about the nodes that registered before it.
 */
void tcp_broadcast_gid_table(TcpBackendPrivate *priv)
{
    if (!priv->is_manager || !priv->node_gids || !priv->mesh_nodes)
        return;

    /* Held across both the snapshot and the sends; see the field's comment. */
    qemu_mutex_lock(&priv->gid_broadcast_lock);

    struct {
        uint32_t node_id;
        uint8_t gids[TCP_MAX_NODE_GIDS][16];
        uint32_t num_gids;
    } per_node[TCP_MAX_MESH_NODES];
    uint32_t nodes = 0;
    guint overflow = 0; /* the size of mesh_nodes, if past the cap */

    /*
     * Snapshot under the mesh and claim locks; the sends are not, because
     * tcp_send_node_gids() takes connection locks, which a sender holds
     * while it waits for a peer to read.
     */
    qemu_mutex_lock(&priv->mesh_table_lock);
    qemu_mutex_lock(&priv->gid_table_lock);

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (nodes == G_N_ELEMENTS(per_node)) {
            overflow = g_hash_table_size(priv->mesh_nodes);
            break;
        }

        uint32_t nid = GPOINTER_TO_UINT(key);
        const TcpNodeGids *claims =
            g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(nid));

        per_node[nodes].node_id = nid;
        /*
         * A dead node is advertised as owning nothing. Workers keep no
         * liveness of their own -- they take the manager's word for who
         * owns what -- so this is how they stop resolving a node that is
         * away, and how they stop seeing its claims contest those of the
         * same guest returning under a new node id.
         */
        per_node[nodes].num_gids =
            claims && claims->alive ? claims->num_gids : 0;
        for (uint32_t i = 0; i < per_node[nodes].num_gids; i++)
            memcpy(per_node[nodes].gids[i], claims->gids[i], 16);
        nodes++;
    }

    qemu_mutex_unlock(&priv->gid_table_lock);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    if (overflow)
        tcp_report_mesh_overflow("GID broadcast", overflow);

    GPtrArray *conns = tcp_conns_snapshot(priv);

    for (uint32_t i = 0; i < nodes; i++) {
        TcpNodeGidsPayload payload;

        tcp_fill_node_gids(&payload, per_node[i].node_id,
                           tcp_const_gids(per_node[i].gids),
                           per_node[i].num_gids);

        for (guint c = 0; c < conns->len; c++)
            tcp_send_node_gids(priv, g_ptr_array_index(conns, c), &payload);
    }
    g_ptr_array_free(conns, TRUE);

    qemu_mutex_unlock(&priv->gid_broadcast_lock);
}

/*
 * Stop conn's receive thread and close its socket, but leave conn allocated
 * for the threads still holding references to it, whose sends on it now
 * fail.  The caller has taken conn out of the table that published it, or
 * never published it, and holds a reference (see TcpConnection).  It holds
 * no lock a receive thread takes, since the join waits for the thread.
 *
 * Safe to call again on a connection already retired, but not by two threads
 * at once: the unlocked read of sockfd for the shutdown relies on no other
 * thread closing it meanwhile.  Taking conn out of its table is what makes
 * the caller the only one, and teardown's tcp_claim_conn_threads() runs only
 * once every thread that does that has stopped.  The receive thread is
 * joined only if this call is the one that clears its flag, and the send
 * thread only if it was started and not yet joined.  The socket is shut down
 * before the joins so a send either thread is blocked in can't hold them up,
 * and the send queue is closed, so nothing is queued afterwards.  The
 * descriptor is closed under conn->lock because senders read conn->sockfd
 * under that lock; one that read it just before an unlocked close could send
 * on the descriptor after it was reused for another connection.
 */
void tcp_retire_conn(TcpConnection *conn)
{
    if (atomic_exchange(&conn->recv_thread_running, false)) {
        if (conn->sockfd >= 0) {
            shutdown(conn->sockfd, SHUT_RDWR);
        }
        qemu_thread_join(&conn->recv_thread);
    } else if (conn->sockfd >= 0) {
        /* For the send thread, which may be waiting on the peer */
        shutdown(conn->sockfd, SHUT_RDWR);
    }
    tcp_stop_send_thread(conn);

    qemu_mutex_lock(&conn->lock);
    if (conn->sockfd >= 0) {
        close(conn->sockfd);
    }
    conn->sockfd = -1;
    atomic_store(&conn->is_connected, false);
    qemu_mutex_unlock(&conn->lock);
}

/*
 * Whether node has gone too long without a heartbeat, which is three
 * intervals.  The caller holds mesh_table_lock, and read now_ms under it.
 */
static bool tcp_node_silent(const TcpBackendPrivate *priv,
                            const MeshNodeInfo *node, uint64_t now_ms)
{
    return now_ms - node->last_heartbeat_ms >
           (uint64_t)priv->health_check_interval_sec * 3000u;
}

/*
 * The part of a health-check pass that needs mesh_table_lock: note each
 * other node's connection, taking a reference to it, and declare dead the
 * nodes whose connection has ended and those that have stopped answering.
 * Returns true if a node was newly declared dead, which the rest of the mesh
 * has to be told.
 */
static bool tcp_health_check_scan(TcpBackendPrivate *priv, GArray *items)
{
    bool newly_dead = false;
    GHashTableIter iter;
    gpointer value;

    qemu_mutex_lock(&priv->mesh_table_lock);
    /* Read under the lock, so no heartbeat is newer */
    uint64_t now_ms = ernic_monotonic_ms();

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        MeshNodeInfo *node = (MeshNodeInfo *)value;
        TcpHealthCheckItem item = {node, NULL, false, false};

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
                node->is_alive = false;
                tcp_gid_claims_set_alive(priv, node->node_id, false);
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
                node->is_alive = false;
                tcp_gid_claims_set_alive(priv, node->node_id, false);
                newly_dead = true;
            }
            item.silent = true;
        }

        /* Every reference taken above lands in items, to be released */
        if (item.conn)
            g_array_append_val(items, item);
    }
    qemu_mutex_unlock(&priv->mesh_table_lock);

    return newly_dead;
}

/*
 * Take conn, node_id's connection when the pass began, out of connections
 * and retire it, unless it has left the table already.  The caller keeps its
 * own reference.  Called with no lock held, since retiring joins the receive
 * thread, whose handlers take mesh_table_lock and conn_table_lock.
 */
static void tcp_health_check_unpublish(TcpBackendPrivate *priv,
                                       uint32_t node_id, TcpConnection *conn)
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
 * Whether node is still silent.  The scan's verdict can be out of date by
 * the time it's acted on: a node can answer a heartbeat late, on the
 * connection that would be retired.
 */
static bool tcp_node_still_silent(TcpBackendPrivate *priv, MeshNodeInfo *node)
{
    bool silent;

    qemu_mutex_lock(&priv->mesh_table_lock);
    silent = tcp_node_silent(priv, node, ernic_monotonic_ms());
    qemu_mutex_unlock(&priv->mesh_table_lock);
    if (!silent)
        rdma_info_report("TCP: Node %u has answered; keeping its connection",
                         node->node_id);
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
            !(item->silent && tcp_node_still_silent(priv, item->node)))
            continue;
        if (!item->disconnected)
            rdma_info_report("TCP: Closing the connection to node %u, which "
                             "has stopped answering",
                             item->node->node_id);
        tcp_health_check_unpublish(priv, item->node->node_id, item->conn);
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

    rdma_info_report("TCP: Manager health check thread started");

    while (atomic_load(&priv->health_check_running)) {
        /* The accept thread reaps too, but only when a peer connects */
        tcp_reap_pending_conns(priv);

        bool newly_dead = tcp_health_check_scan(priv, items);
        tcp_health_check_retire(priv, items);

        for (guint i = 0; i < items->len; i++) {
            const TcpHealthCheckItem *item =
                &g_array_index(items, TcpHealthCheckItem, i);
            TcpConnection *conn = item->conn;

            if (conn)
                tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT,
                                   item->node->node_id);
        }
        tcp_health_check_release(items);

        /*
         * A node the scan declared dead had its claims marked not-alive,
         * which takes it out of resolution without discarding what it
         * claimed. Discarding would be a one-way door: a node that is merely
         * silent for three intervals and then answers would come back live
         * and unreachable, and one that registers again would be advertised
         * as owning nothing until its own advertisement came.  The broadcast
         * tells the workers, who keep no liveness of their own.
         */
        if (newly_dead) {
            tcp_broadcast_mesh_topology(priv);
            tcp_broadcast_gid_table(priv);
        }

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

    g_array_free(items, TRUE);
    rdma_info_report("TCP: Manager health check thread exiting");
    return NULL;
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
        rdma_error_report("TCP: Failed to connect to manager at %s:%u",
                          priv->manager_host, priv->manager_port);
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
 * Backend Lifecycle
 */

/*
 * Stop the accept thread and wait for it.  The listen socket is blocking,
 * so clearing the flag alone leaves the thread parked in accept() until a
 * peer happens to connect, and the join (on SIGTERM, say) never returns.
 * shutdown() wakes a blocked accept() with EINVAL, and the loop then sees
 * the flag clear and exits; the socket itself stays open for the caller to
 * close.  A no-op when the thread was never started.
 */
void tcp_stop_accept_thread(TcpBackendPrivate *priv)
{
    if (!atomic_load(&priv->accept_thread_running)) {
        return;
    }
    atomic_store(&priv->accept_thread_running, false);
    if (priv->listen_fd >= 0) {
        shutdown(priv->listen_fd, SHUT_RDWR);
    }
    qemu_thread_join(&priv->accept_thread);
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
 * Clear conn's receive thread's stop flag, and close its send queue, and
 * queue each thread for a join, if it's running.  The flag alone isn't
 * enough: either thread may be blocked sending to a peer that has stopped
 * reading, and that loop waits on the peer, not the flag.  Shutting the
 * socket down fails any send in progress, or to come, with EPIPE, and any
 * receive with end-of-file.  The descriptor stays open, so neither thread
 * can end up using a reused one; it's closed when conn is freed.
 */
static void tcp_claim_conn_threads(GPtrArray *recv_batch, GPtrArray *send_batch,
                                   TcpConnection *conn)
{
    bool recv_running = atomic_exchange(&conn->recv_thread_running, false);
    bool send_running;

    qemu_mutex_lock(&conn->sendq_lock);
    tcp_sendq_close(conn);
    send_running = conn->send_thread_started;
    conn->send_thread_started = false;
    qemu_mutex_unlock(&conn->sendq_lock);

    if ((recv_running || send_running) && conn->sockfd >= 0) {
        shutdown(conn->sockfd, SHUT_RDWR);
    }
    if (recv_running) {
        g_ptr_array_add(recv_batch, conn);
    }
    if (send_running) {
        g_ptr_array_add(send_batch, conn);
    }
}

/*
 * Stop every receive thread and send thread and wait for them, freeing
 * nothing.  A receive thread can reach any connection, and the mesh and GID
 * tables, through priv, so none of that may be freed while any of them still
 * runs.  The accept, health-check and peer-connect threads, which add
 * connections with threads of their own, must already be stopped.  No
 * receive thread starts another, but this repeats until a pass finds nothing
 * left to stop anyway, so one that did couldn't leave its thread running.
 * Closing a send queue keeps its thread from being started again.
 */
void tcp_stop_recv_threads(TcpBackendPrivate *priv)
{
    for (;;) {
        GPtrArray *recv_batch = g_ptr_array_new();
        GPtrArray *send_batch = g_ptr_array_new();
        GHashTableIter iter;
        gpointer value;

        qemu_mutex_lock(&priv->conn_table_lock);
        g_hash_table_iter_init(&iter, priv->connections);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            tcp_claim_conn_threads(recv_batch, send_batch, value);
        }
        for (guint i = 0; i < priv->pending_conns->len; i++) {
            tcp_claim_conn_threads(recv_batch, send_batch,
                                   g_ptr_array_index(priv->pending_conns, i));
        }
        TcpConnection *mgr = atomic_load(&priv->manager_conn);
        if (mgr) {
            tcp_claim_conn_threads(recv_batch, send_batch, mgr);
        }
        qemu_mutex_unlock(&priv->conn_table_lock);

        guint stopped = recv_batch->len + send_batch->len;
        for (guint i = 0; i < recv_batch->len; i++) {
            TcpConnection *conn = g_ptr_array_index(recv_batch, i);
            qemu_thread_join(&conn->recv_thread);
        }
        for (guint i = 0; i < send_batch->len; i++) {
            TcpConnection *conn = g_ptr_array_index(send_batch, i);
            qemu_thread_join(&conn->send_thread);
        }
        g_ptr_array_free(recv_batch, TRUE);
        g_ptr_array_free(send_batch, TRUE);
        if (stopped == 0) {
            return;
        }
    }
}

/*
 * Release the pending connections, and the list; their threads must already
 * be stopped.
 */
void tcp_free_pending_conns(TcpBackendPrivate *priv)
{
    for (guint i = 0; i < priv->pending_conns->len; i++) {
        tcp_connection_unref(g_ptr_array_index(priv->pending_conns, i));
    }
    g_ptr_array_free(priv->pending_conns, TRUE);
}

/*
 * Stop every backend thread and wait for them, freeing nothing.
 *
 * stopping is set first, before any join.  Any thread can be blocked
 * sending to a peer that has stopped reading, holding that connection's
 * lock, for up to the send timeout, and a thread can be waiting for that
 * lock.  The receive thread doing so has to be joined before any socket is
 * shut down.  With the flag set, such a send fails within one poll interval
 * and releases its lock, rather than at the timeout.
 *
 * The health-check, accept and peer-connect threads add connections, and
 * the peer-connect thread replaces manager_conn, so they go before the
 * receive threads.
 */
void tcp_stop_threads(TcpBackendPrivate *priv)
{
    atomic_store(&priv->stopping, true);
    tcp_stop_health_check_thread(priv);
    tcp_stop_accept_thread(priv);
    tcp_stop_peer_thread(priv);
    tcp_stop_recv_threads(priv);
}

/*
 * tcp_init() with the health-check interval, in seconds, given rather than
 * read from ERNIC_TCP_HEALTH_INTERVAL, so the tests can set it without
 * calling setenv(), which isn't thread safe.  health_interval_s must be
 * positive.
 */
int tcp_init_with_interval(RdmaBackendDev *backend_dev, const char *config,
                           uint32_t health_interval_s)
{
    TcpBackendPrivate *priv;
    int ret;

    /* NULL for a bare "--backend tcp", which names no role */
    if (!config) {
        rdma_error_report("TCP backend: No config. Must be "
                          "tcp:manager:... or tcp:worker:...");
        return -EINVAL;
    }

    rdma_info_report("TCP backend: Initializing");

    priv = g_new0(TcpBackendPrivate, 1);

    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    priv->node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->gid_owner = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            tcp_gid_key_free, NULL);

    tcp_bufpool_init(&priv->recv_pool);

    priv->next_pd_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100;
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->tcp_stats.peer_connect_attempts, 0);
    atomic_init(&priv->tcp_stats.manager_reconnects, 0);

    priv->listen_fd = -1;
    priv->is_listening = false;
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->stopping, false);
    /* tcp_env_int() only returns positive values */
    atomic_init(&priv->send_timeout_ms,
                (uint32_t)tcp_env_int("ERNIC_TCP_SEND_TIMEOUT_MS",
                                      (int)TCP_DEFAULT_SEND_TIMEOUT_MS));

    qemu_mutex_init(&priv->lock);
    qemu_mutex_init(&priv->conn_table_lock);
    qemu_mutex_init(&priv->gid_table_lock);
    qemu_mutex_init(&priv->gid_broadcast_lock);
    qemu_mutex_init(&priv->gid_advertise_lock);
    qemu_mutex_init(&priv->topology_broadcast_lock);

    priv->num_local_gids =
        tcp_parse_guest_gids(getenv("ERNIC_TCP_GUEST_GIDS"), priv->local_gids);
    if (priv->num_local_gids == 0) {
        rdma_warn_report("TCP: ERNIC_TCP_GUEST_GIDS is unset or empty; this "
                         "node advertises no GIDs and peers will refuse to "
                         "open QPs to it");
    }

    /* Initialize manager/worker fields */
    priv->is_manager = false;
    priv->mesh_nodes = NULL;
    atomic_init(&priv->health_check_running, false);
    qemu_mutex_init(&priv->health_check_lock);
    qemu_cond_init(&priv->health_check_cond);
    priv->peer_targets = g_array_new(FALSE, FALSE, sizeof(TcpPeerTarget));
    priv->peer_targets_changed = false;
    qemu_mutex_init(&priv->peer_lock);
    qemu_cond_init(&priv->peer_cond);
    atomic_init(&priv->peer_thread_running, false);
    priv->health_check_interval_sec = health_interval_s;
    priv->next_available_node_id = 1; /* Manager starts assigning from 1 */
    priv->manager_host = NULL;
    priv->manager_port = 0;
    atomic_init(&priv->manager_conn, NULL);
    priv->registering = NULL;
    priv->registration_complete = false;
    priv->registration_accepted = false;
    priv->registers_again = true;
    qemu_cond_init(&priv->registration_cond);
    qemu_mutex_init(&priv->registration_mutex);

    /* Determine mode from config string */
    if (strstr(config, "manager:")) {
        /* Manager mode: tcp:manager:<ip>:<port> or tcp:manager:listen:<port> */
        char *manager_host = NULL;
        uint16_t manager_port = 0;
        bool listen_mode = false;

        priv->mode = TCP_MODE_MANAGER;
        priv->is_manager = true;
        atomic_store(&priv->local_node_id, 0); /* Manager is always node 0 */

        /* The manager's id is fixed, so it can claim its GIDs immediately;
         * a worker has to wait for REGISTER_RESP. */
        tcp_gid_claims_set(priv, 0, tcp_const_gids(priv->local_gids),
                           priv->num_local_gids);

        ret = parse_tcp_config_manager(config, &manager_host, &manager_port,
                                       &listen_mode);
        if (ret < 0) {
            goto error;
        }

        /* Initialize mesh nodes table */
        priv->mesh_nodes =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                  tcp_mesh_node_info_destroy_notify);
        if (!priv->mesh_nodes) {
            rdma_error_report("TCP: Failed to create mesh nodes table");
            g_free(manager_host);
            goto error;
        }
        qemu_mutex_init(&priv->mesh_table_lock);

        /* Add manager itself to mesh table */
        /* Use "localhost" in listen mode (0.0.0.0) so workers can connect */
        /* Otherwise use the configured hostname */
        const char *manager_hostname;
        if (listen_mode) {
            manager_hostname = "localhost";
        } else {
            if (!manager_host || !*manager_host) {
                rdma_error_report("TCP: Manager hostname is NULL or empty");
                g_free(manager_host);
                goto error;
            }
            manager_hostname = manager_host;
        }

        MeshNodeInfo *manager_node =
            tcp_mesh_node_info_new(0, manager_hostname, manager_port);
        if (!manager_node) {
            rdma_error_report("TCP: Failed to allocate manager node info");
            g_free(manager_host);
            goto error;
        }
        g_hash_table_insert(priv->mesh_nodes, GUINT_TO_POINTER(0),
                            manager_node);

        rdma_info_report("TCP backend: Manager mode on port %u", manager_port);

        /* Start listening */
        priv->listen_port = manager_port;
        priv->listen_fd = tcp_listen_on_port(&priv->listen_port);
        if (priv->listen_fd < 0) {
            g_free(manager_host);
            goto error;
        }
        priv->is_listening = true;

        /* Start the accept and health check threads */
        if (!tcp_start_thread(&priv->accept_thread, "tcp-accept",
                              tcp_accept_thread, priv,
                              &priv->accept_thread_running) ||
            !tcp_start_thread(&priv->health_check_thread, "tcp-health",
                              tcp_manager_health_check_thread, priv,
                              &priv->health_check_running)) {
            g_free(manager_host);
            goto error;
        }

        g_free(manager_host);
        rdma_info_report("TCP backend: Manager initialized");

    } else if (strstr(config, "worker:")) {
        /* Worker mode: tcp:worker:<manager_ip>:<manager_port> */
        char *manager_host = NULL;
        uint16_t manager_port = 0;

        priv->mode = TCP_MODE_WORKER;
        priv->is_manager = false;

        ret = parse_tcp_config_worker(config, &manager_host, &manager_port);
        if (ret < 0) {
            goto error;
        }

        priv->manager_host = manager_host;
        priv->manager_port = manager_port;

        /*
         * Listen on a port the kernel picks; registering tells the manager
         * which one, and the manager tells the other workers.
         */
        priv->listen_port = 0;
        priv->listen_fd = tcp_listen_on_port(&priv->listen_port);
        if (priv->listen_fd < 0) {
            /* manager_host already assigned to priv->manager_host, will be
             * freed in error path */
            goto error;
        }
        priv->is_listening = true;

        /*
         * Register with manager.  This connect, unlike the peer-connect
         * thread's later ones, has no deadline and can't be stopped.
         */
        ret = tcp_worker_register_with_manager(priv, NULL, 0);
        if (ret < 0) {
            /* manager_host already assigned to priv->manager_host, will be
             * freed in error path */
            goto error;
        }

        /*
         * Start the accept and peer-connect threads only now that this node
         * has its id.  Both introduce the node to its peers with it, and
         * before registration it reads as 0, the manager's.  A peer that
         * connects before then waits in the listen socket's backlog.  The
         * peer-connect thread is also what connects to the manager again if
         * the connection made here is lost.
         */
        if (!tcp_start_thread(&priv->accept_thread, "tcp-accept",
                              tcp_accept_thread, priv,
                              &priv->accept_thread_running) ||
            !tcp_start_thread(&priv->peer_thread, "tcp-peers",
                              tcp_worker_peer_thread, priv,
                              &priv->peer_thread_running)) {
            goto error;
        }

        rdma_info_report("TCP backend: Worker initialized, registered with "
                         "manager");

    } else {
        rdma_error_report("TCP backend: Invalid config. Must be "
                          "tcp:manager:... or tcp:worker:...");
        goto error;
    }

    backend_dev->backend_private = priv;
    return 0;

error:
    /* A worker that fails to register has already started the receive thread
     * for its manager connection, and one that fails to start its accept or
     * peer-connect thread has started others; they must be gone before priv
     * is freed under them. */
    tcp_stop_threads(priv);
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
    }
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    qemu_cond_destroy(&priv->peer_cond);
    qemu_mutex_destroy(&priv->peer_lock);
    g_array_free(priv->peer_targets, TRUE);
    tcp_free_pending_conns(priv);
    if (priv->mesh_nodes) {
        g_hash_table_destroy(priv->mesh_nodes);
    }
    if (priv->mesh_table_lock.initialized) {
        qemu_mutex_destroy(&priv->mesh_table_lock);
    }
    tcp_connection_unref(atomic_load(&priv->manager_conn));
    qemu_cond_destroy(&priv->registration_cond);
    qemu_mutex_destroy(&priv->registration_mutex);
    g_free(priv->manager_host);
    g_hash_table_destroy(priv->connections);
    g_hash_table_destroy(priv->node_gids);
    g_hash_table_destroy(priv->gid_owner);
    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->gid_table_lock);
    qemu_mutex_destroy(&priv->gid_broadcast_lock);
    qemu_mutex_destroy(&priv->gid_advertise_lock);
    qemu_mutex_destroy(&priv->topology_broadcast_lock);
    g_free(priv);
    return -1;
}

static int tcp_init(RdmaBackendDev *backend_dev, const char *config)
{
    /* tcp_env_int() only returns positive values */
    return tcp_init_with_interval(
        backend_dev, config,
        (uint32_t)tcp_env_int("ERNIC_TCP_HEALTH_INTERVAL",
                              TCP_DEFAULT_HEALTH_INTERVAL_S));
}

static void tcp_fini(RdmaBackendDev *backend_dev)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv) {
        return;
    }

    rdma_info_report("TCP backend: Cleaning up");

    /* Nothing below may be freed while a backend thread can still use it */
    tcp_stop_threads(priv);
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    qemu_cond_destroy(&priv->peer_cond);
    qemu_mutex_destroy(&priv->peer_lock);
    g_array_free(priv->peer_targets, TRUE);

    /* Close listen socket */
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
        priv->listen_fd = -1;
    }

    tcp_free_pending_conns(priv);

    /* Clean up manager/worker specific resources */
    if (priv->mesh_nodes) {
        g_hash_table_destroy(priv->mesh_nodes);
    }
    if (priv->mesh_table_lock.initialized) {
        qemu_mutex_destroy(&priv->mesh_table_lock);
    }
    tcp_connection_unref(atomic_load(&priv->manager_conn));
    qemu_cond_destroy(&priv->registration_cond);
    qemu_mutex_destroy(&priv->registration_mutex);
    g_free(priv->manager_host);

    g_hash_table_destroy(priv->connections);
    g_hash_table_destroy(priv->node_gids);
    g_hash_table_destroy(priv->gid_owner);

    tcp_bufpool_destroy(&priv->recv_pool);
    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->gid_table_lock);
    qemu_mutex_destroy(&priv->gid_broadcast_lock);
    qemu_mutex_destroy(&priv->gid_advertise_lock);
    qemu_mutex_destroy(&priv->topology_broadcast_lock);

    g_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int tcp_query_device(RdmaBackendDev *backend_dev,
                            struct ibv_device_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->max_qp = 1024;
    attr->max_qp_wr = 1024;
    attr->max_sge = 32;
    attr->max_cq = 1024;
    attr->max_cqe = 8192;
    /* As in the loopback backend: this sizes rdma_rm's MR table, and
     * a literal below MAX_MR silently caps it under what an
     * nvme-rdma connect registers up front. */
    attr->max_mr = MAX_MR;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    /*
     * This sizes rdma_rm's SRQ table (res_tbl_init in rdma_rm.c), and
     * nothing else reads it: what the guest sees as max_srq comes from the
     * LIF identity, which ionic_eth_emu.c fills from IONIC_EMU_SRQ_COUNT.
     * Leaving this zero while advertising a non-zero count there is not a
     * capability mismatch the guest can detect -- it is a table of size
     * zero, so the very first CREATE_SRQ fails with
     *
     *   Table SRQ, failed to allocate, bitmap is full
     *
     * surfacing in the guest as ibv_create_srq() returning EINVAL on a
     * device that just reported max_srq=32768. Keep the two in step.
     *
     * Note what this does NOT do. No receive is served from here. Every
     * ionic receive, local and off the TCP mesh alike, is satisfied by
     * deliver_recv() in ionic_datapath.c, reading either the destination
     * QP's own RQ ring or -- for a QP bound to one -- the SRQ's ring,
     * straight out of guest memory. This backend has no receive queues.
     *
     * So for ionic this value only sizes the rdma_rm table the adminq
     * handler allocates out of. The datapath half it used to disclaim is
     * done: ionic_datapath.c has an SRQ qtype, consumes the ring, and
     * tests/test_srq_transfer.c moves a payload across one.
     */
    attr->max_srq = IONIC_EMU_SRQ_COUNT;
    attr->max_srq_wr = 65535;
    attr->max_srq_sge = IONIC_MAX_SRQ_SGES;
    return 0;
}

/*
 * Protection Domain Operations
 *
 * The backend keeps no state for a PD, CQ or QP.  The ionic data path keeps
 * its own, and these only hand out the handles rdma_rm records.
 */

static int tcp_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t handle;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    handle = priv->next_pd_handle++;
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("TCP: Created PD handle %u", handle);
    return 0;
}

/*
 * Memory Region Operations
 */

static int tcp_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                         size_t length, uint64_t guest_start, int access)
{
    (void)access;

    mr->ibmr = NULL;

    rdma_info_report("TCP: Created MR len=%zu", length);
    return 0;
}

static void tcp_destroy_mr(RdmaBackendMR *mr)
{
    rdma_info_report("TCP: Destroyed MR");
}

static uint32_t tcp_mr_lkey(const RdmaBackendMR *mr)
{
    (void)mr;
    /* 0 has rdma_rm_alloc_mr() use the resource table handle as the lkey */
    return 0;
}

/*
 * Completion Queue Operations
 */

static int tcp_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                         uint32_t cqe)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t handle;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    handle = priv->next_cq_handle++;
    qemu_mutex_unlock(&priv->lock);

    cq->backend_dev = backend_dev;
    cq->ibcq = (struct ibv_cq *)(uintptr_t)handle;

    rdma_info_report("TCP: Created CQ handle %u, cqe=%" PRIu32, handle, cqe);
    return 0;
}

static void tcp_destroy_cq(RdmaBackendCQ *cq)
{
    rdma_info_report("TCP: Destroyed CQ handle %u",
                     (uint32_t)(uintptr_t)cq->ibcq);
}

/*
 * Queue Pair Operations
 */

static int tcp_create_qp(RdmaBackendQP *qp, uint8_t qp_type, RdmaBackendPD *pd,
                         RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                         RdmaBackendSRQ *srq, uint32_t max_send_wr,
                         uint32_t max_recv_wr, uint32_t max_send_sge,
                         uint32_t max_recv_sge)
{
    TcpBackendPrivate *priv = get_private(scq->backend_dev);
    uint32_t qpn;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    qpn = priv->next_qpn++;
    qemu_mutex_unlock(&priv->lock);

    qp->ibqp = (struct ibv_qp *)(uintptr_t)qpn;

    rdma_info_report("TCP: Created QP qpn=%u, type=%u", qpn, qp_type);
    return 0;
}

static void tcp_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    rdma_info_report("TCP: Destroyed QP qpn=%u", qpn);
}

static uint32_t tcp_qpn(const RdmaBackendQP *qp)
{
    return (uint32_t)(uintptr_t)qp->ibqp;
}

/*
 * QP State Transition Operations
 */

static int tcp_qp_state_init(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                             uint8_t qp_type, uint32_t qkey)
{
    if (!get_private(backend_dev)) {
        return -EINVAL;
    }

    rdma_info_report("TCP: QP %u -> INIT", (uint32_t)(uintptr_t)qp->ibqp);
    return 0;
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
 * Refuse a connected QP whose destination GID no mesh node owns.  Nothing is
 * recorded: the ionic data path resolves the destination for itself, through
 * tcp_backend_node_from_gid().
 */
static int tcp_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                            uint8_t qp_type, uint8_t sgid_idx,
                            union ibv_gid *dgid, uint32_t dqpn, uint32_t rq_psn,
                            uint32_t qkey, bool qkey_set)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    char gidbuf[TCP_GID_STR_LEN];
    uint32_t resolved;

    if (!priv) {
        return -EINVAL;
    }

    /*
     * An unreliable datagram QP has no destination at this point: UD names
     * its peer per work request, through the AH on the send, so the
     * transition carries a zero GID.  Requiring one here rejects every
     * ordinary UD QP.
     */
    if (qp_type == IBV_QPT_UD) {
        rdma_info_report("TCP: QP %u -> RTR as UD; destination resolved "
                         "per send",
                         qpn);
        return 0;
    }

    resolved = tcp_resolve_node_from_gid(priv, dgid);
    if (resolved == UINT32_MAX) {
        /*
         * No node owns this GID.  Fail the transition rather than route to an
         * arbitrary peer: rdma_rm_modify_qp() turns this into -EIO and the
         * guest sees the failure at connection setup, where it is
         * diagnosable, instead of silently writing into the wrong node.
         */
        rdma_error_report(
            "TCP: QP %u -> RTR refused: no mesh node owns destination "
            "GID %s (check ERNIC_TCP_GUEST_GIDS on every node)",
            qpn, dgid ? tcp_gid_str(dgid->raw, gidbuf) : "none");
        return -EINVAL;
    }

    rdma_info_report("TCP: QP %u -> RTR (remote qpn=%u, GID %s -> node %u%s)",
                     qpn, dqpn, tcp_gid_str(dgid->raw, gidbuf), resolved,
                     resolved == tcp_local_node(priv) ? ", local" : "");
    return 0;
}

static int tcp_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type, uint32_t sq_psn,
                            uint32_t qkey, bool qkey_set)
{
    (void)qp;
    (void)qp_type;
    (void)qkey;
    (void)qkey_set;
    /* Simplified implementation - QP state transitions handled internally */
    rdma_info_report("TCP: QP -> RTS (sq_psn=%u)", sq_psn);
    return 0;
}

static int tcp_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                        int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    (void)qp;
    (void)attr_mask;

    memset(attr, 0, sizeof(*attr));
    memset(init_attr, 0, sizeof(*init_attr));

    /* Simplified implementation */
    attr->qp_state = IBV_QPS_RTS;
    attr->cur_qp_state = IBV_QPS_RTS;

    return 0;
}

/*
 * GID Management
 */

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
static int tcp_claim_gid(TcpBackendPrivate *priv, const union ibv_gid *gid,
                         bool add)
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

static int tcp_get_backend_gid_index(RdmaBackendDev *backend_dev, int sgid_idx)
{
    return sgid_idx;
}

/*
 * SRQ Operations
 */

static int tcp_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                          uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit)
{
    TcpSRQ *tsrq = g_new0(TcpSRQ, 1);

    tsrq->max_wr = max_wr;
    tsrq->max_sge = max_sge;
    tsrq->srq_limit = srq_limit;
    qemu_mutex_init(&tsrq->lock);

    srq->ibsrq = (struct ibv_srq *)(uintptr_t)tsrq;
    rdma_info_report("TCP: Created SRQ max_wr=%u", max_wr);
    return 0;
}

static void tcp_destroy_srq(RdmaBackendSRQ *srq)
{
    TcpSRQ *tsrq = (TcpSRQ *)(uintptr_t)srq->ibsrq;
    if (!tsrq) {
        return;
    }

    qemu_mutex_destroy(&tsrq->lock);

    g_free(tsrq);
    srq->ibsrq = NULL;
    rdma_info_report("TCP: Destroyed SRQ");
}

static int tcp_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr)
{
    TcpSRQ *tsrq = (TcpSRQ *)(uintptr_t)srq->ibsrq;
    if (!tsrq) {
        return -EINVAL;
    }

    memset(attr, 0, sizeof(*attr));
    attr->max_wr = tsrq->max_wr;
    attr->max_sge = tsrq->max_sge;
    attr->srq_limit = tsrq->srq_limit;
    return 0;
}

static int tcp_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr,
                          int mask)
{
    TcpSRQ *tsrq = (TcpSRQ *)(uintptr_t)srq->ibsrq;
    if (!tsrq) {
        return -EINVAL;
    }

    qemu_mutex_lock(&tsrq->lock);
    if (mask & IBV_SRQ_LIMIT) {
        tsrq->srq_limit = attr->srq_limit;
    }
    qemu_mutex_unlock(&tsrq->lock);
    return 0;
}


/*
 * Backend Operations Structure
 */
const RdmaBackendOps rdma_backend_ops_tcp = {
    .name = "tcp",
    .type = RDMA_BACKEND_TYPE_TCP,

    .init = tcp_init,
    .fini = tcp_fini,

    .query_device = tcp_query_device,

    .create_pd = tcp_create_pd,

    .create_mr = tcp_create_mr,
    .destroy_mr = tcp_destroy_mr,
    .mr_lkey = tcp_mr_lkey,

    .create_cq = tcp_create_cq,
    .destroy_cq = tcp_destroy_cq,

    .create_qp = tcp_create_qp,
    .destroy_qp = tcp_destroy_qp,
    .qpn = tcp_qpn,

    .qp_state_init = tcp_qp_state_init,
    .qp_state_rtr = tcp_qp_state_rtr,
    .qp_state_rts = tcp_qp_state_rts,
    .query_qp = tcp_query_qp,

    .get_backend_gid_index = tcp_get_backend_gid_index,

    .create_srq = tcp_create_srq,
    .destroy_srq = tcp_destroy_srq,
    .query_srq = tcp_query_srq,
    .modify_srq = tcp_modify_srq,
};

uint32_t tcp_backend_local_node_id(RdmaBackendDev *backend_dev)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return UINT32_MAX;
    return tcp_local_node(priv);
}

uint32_t tcp_backend_node_from_gid(RdmaBackendDev *backend_dev,
                                   const union ibv_gid *dgid)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return UINT32_MAX;

    /* UINT32_MAX on a miss, with no default peer: see
     * tcp_resolve_node_from_gid(). The caller fails the command. */
    return tcp_resolve_node_from_gid(priv, dgid);
}

/*
 * Claim @gid for this node (@add) or withdraw it, and re-advertise to the
 * mesh, so that a peer can resolve an address that did not exist when the
 * mesh formed.
 *
 * No production caller yet: a guest's addresses still reach the mesh only
 * through ERNIC_TCP_GUEST_GIDS, read once at startup.  What is missing is
 * the dispatch from a guest GID-table update to here; the mesh side of it
 * works and is covered by the gsi-mesh test, which is the only caller.
 *
 * Harmless on a non-mesh backend: loopback and none have no peers to tell.
 */
int tcp_backend_claim_gid(RdmaBackendDev *backend_dev, const union ibv_gid *gid,
                          bool add)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return 0;

    return tcp_claim_gid(priv, gid, add);
}

void tcp_backend_set_ionic_recv_cb(RdmaBackendDev *backend_dev,
                                   tcp_ionic_recv_fn fn, void *opaque)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    /* Every backend has a private block, but only this one's is a
     * TcpBackendPrivate. The data path clears its callback at teardown
     * whatever the backend, and on any other backend these fields would lie
     * past the end of its private block. */
    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return;

    qemu_mutex_lock(&priv->lock);
    priv->ionic_recv_fn = fn;
    priv->ionic_recv_opaque = opaque;
    qemu_mutex_unlock(&priv->lock);
}

/*
 * Whether a request of len bytes to dst_node would be queued now, rather
 * than refused with -EAGAIN, so the data path can find out before it gathers
 * the payload.  Only the main thread queues the data path's messages, so the
 * answer holds until it queues another, unless the route changes meanwhile.
 * True when there is no connection to queue on: the send then fails, which
 * the caller has to hear about.
 */
bool tcp_backend_ionic_has_room(RdmaBackendDev *backend_dev, uint32_t dst_node,
                                size_t len)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *conn;
    bool room;

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return true;
    conn = tcp_route_to_node(priv, dst_node);
    if (!conn)
        return true;
    qemu_mutex_lock(&conn->sendq_lock);
    room = conn->sendq_closed || conn->sendq_bytes == 0 ||
           (len <= TCP_SENDQ_REQUEST_BYTES &&
            conn->sendq_bytes <= TCP_SENDQ_REQUEST_BYTES - len);
    qemu_mutex_unlock(&conn->sendq_lock);
    tcp_connection_unref(conn);
    return room;
}

/*
 * Take back the requests still queued on any connection whose tag, as given
 * to tcp_backend_send_ionic_v(), matches tag in the bits mask selects, so
 * none of them reaches its peer: those of a QP that is flushing, or one
 * request that has timed out, whose work requests the data path completes
 * now.  On a NIC, a flushed work request isn't transmitted; one queued here
 * hasn't been either.
 *
 * Only what no send thread has started to write: the message being written,
 * and what is already in the socket or on its way, still arrive, as frames
 * already on the wire do.  Replies are left alone.  A connection that has
 * left the table, or manager_conn, had its queue closed as it left
 * (tcp_conn_unpublished()), so nothing more goes out on it to miss.  Called
 * on the device's main thread, the only one that queues the data path's
 * messages, so nothing that matches is queued meanwhile.
 */
void tcp_backend_ionic_cancel(RdmaBackendDev *backend_dev, uint64_t tag,
                              uint64_t mask)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *manager;
    GPtrArray *conns;

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP)
        return;

    /* A worker's connection to the manager, which relays, isn't in the table */
    conns = tcp_conns_snapshot(priv);
    manager = tcp_worker_manager_conn(priv);
    if (manager)
        g_ptr_array_add(conns, manager);
    for (guint i = 0; i < conns->len; i++)
        tcp_sendq_cancel(g_ptr_array_index(conns, i), tag, mask);
    g_ptr_array_free(conns, TRUE);
}

/*
 * Unicast an opaque ionic message to @dst_node.  Called on the device's main
 * thread, which runs vfio-user, so it never waits for the peer: the message
 * is copied onto the connection's send queue, for its send thread to write
 * (tcp_conn_enqueue()).  A peer that stops reading holds up only that
 * thread, until the send times out and shuts the connection down.
 *
 * tag is the caller's name for a request, which tcp_backend_ionic_cancel()
 * matches; a reply's is ignored, since a reply is never taken back.
 *
 * Returns 0 once queued, which doesn't mean the peer will get it: a message
 * still queued when the connection is shut down is lost, and the data path's
 * own timeout for the answer covers that.  Otherwise a negative errno:
 * -EAGAIN for a request (!reply) the queue has no room for yet, which the
 * caller tries again later; -ENOTCONN or -EPIPE when there is no connection
 * to send on; or what tcp_conn_enqueue() returns.
 */
int tcp_backend_send_ionic_v(RdmaBackendDev *backend_dev, uint32_t dst_node,
                             const void *hdr, size_t hdr_len, const void *body,
                             size_t body_len, uint64_t tag, bool reply)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *conn;
    uint32_t local;
    int rc;

    /* Each length checked on its own before the sum, which could wrap */
    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP || !hdr ||
        hdr_len == 0 || hdr_len > TCP_MAX_PAYLOAD_LEN ||
        body_len > TCP_MAX_PAYLOAD_LEN - hdr_len)
        return -EINVAL;

    local = tcp_local_node(priv);
    if (dst_node == local)
        return -EINVAL;

    /* A worker may have no direct socket to another worker; the manager
     * relays. */
    conn = tcp_route_to_node(priv, dst_node);
    if (!conn) {
        rdma_error_report("TCP: no connection to node %u for ionic message",
                          dst_node);
        return -ENOTCONN;
    }

    rc = tcp_conn_enqueue(conn, hdr, hdr_len, body, body_len, dst_node, tag,
                          reply);
    tcp_connection_unref(conn);

    /* -EAGAIN is the caller's to handle, and comes often under load */
    if (rc == -EPIPE) {
        rdma_error_report("TCP: connection to node %u closed before an ionic "
                          "message could be sent",
                          dst_node);
    } else if (rc == -ENOBUFS) {
        rdma_error_report("TCP: no room to queue a reply to node %u, which "
                          "isn't reading fast enough; dropping it",
                          dst_node);
    }
    return rc;
}
