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
 * Record the manager's answer to this worker's registration and wake
 * tcp_worker_register_with_manager(): the node id the manager assigned, or 0
 * if it refused the registration or its answer can't be used.  Only the
 * first answer counts.  Returns false, recording nothing, if the
 * registration has already been answered.
 *
 * The id is stored under the mutex, with the flag, because the waiter reads
 * both under it on another thread.
 */
static bool tcp_worker_registration_done(TcpBackendPrivate *priv,
                                         uint32_t node_id)
{
    bool first;

    qemu_mutex_lock(&priv->registration_mutex);
    first = !priv->registration_complete;
    if (first) {
        atomic_store(&priv->local_node_id, node_id);
        priv->registration_complete = true;
        qemu_cond_signal(&priv->registration_cond);
    }
    qemu_mutex_unlock(&priv->registration_mutex);
    return first;
}

/* Forward declarations */
static void tcp_broadcast_gid_table(TcpBackendPrivate *priv);
static void tcp_send_node_gids(TcpBackendPrivate *priv, TcpConnection *conn,
                               const TcpNodeGidsPayload *payload);
static int tcp_send_handshake(TcpConnection *conn, uint32_t local_node_id,
                              TcpMsgType msg_type);
static int tcp_worker_register_with_manager(TcpBackendPrivate *priv);

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

/*
 * Free conn.  Called only by the last tcp_connection_unref().
 *
 * By then a connection whose receive thread was started has been retired,
 * which joined the thread (see TcpConnection).  A thread still flagged
 * running means a reference was dropped without retiring the connection, a
 * bug; the thread is stopped and joined here rather than left running on
 * freed memory.  The socket is shut down first, as tcp_retire_conn() does,
 * so the join can't wait on the peer.
 */
static void tcp_connection_free(TcpConnection *conn)
{
    if (atomic_exchange(&conn->recv_thread_running, false)) {
        rdma_error_report("TCP: freeing the connection to node %u with its "
                          "receive thread running",
                          conn->node_id);
        if (conn->sockfd >= 0) {
            shutdown(conn->sockfd, SHUT_RDWR);
        }
        qemu_thread_join(&conn->recv_thread);
    }

    if (conn->sockfd >= 0) {
        close(conn->sockfd);
    }

    g_free(conn->remote_host);
    qemu_mutex_destroy(&conn->lock);
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
    qemu_mutex_init(&conn->lock);
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

    conn = priv->manager_conn;
    if (conn && atomic_load(&conn->is_connected)) {
        return tcp_connection_ref(conn);
    }
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
 * Take node_id's entry out of connections if it is still conn, handing the
 * table's reference to the caller, who then retires conn and drops the
 * reference with no lock held (see TcpConnection).  Returns whether it was
 * taken.  The caller holds conn_table_lock.
 */
bool tcp_conn_table_take(TcpBackendPrivate *priv, uint32_t node_id,
                         TcpConnection *conn)
{
    if (g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id)) !=
        conn) {
        return false;
    }
    g_hash_table_steal(priv->connections, GUINT_TO_POINTER(node_id));
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
    node->last_heartbeat = time(NULL);
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

/* Milliseconds on the monotonic clock, for timeouts. */
uint64_t tcp_monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * Wait for the non-blocking connect() in progress on sockfd to finish.
 * Returns 0 once it has succeeded, or -1 with errno set: to the connect's
 * own error, to ETIMEDOUT once tcp_monotonic_ms() reaches deadline_ms (if
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
        if (deadline_ms != 0 && tcp_monotonic_ms() >= deadline_ms) {
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
            (deadline_ms != 0 && tcp_monotonic_ms() >= deadline_ms))
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
 * on tcp_monotonic_ms()'s clock, gives up then, however many addresses host
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

static int tcp_listen_on_port(uint16_t port)
{
    struct sockaddr_in server_addr;
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
    server_addr.sin_port = htons(port);

    ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to bind to port %u: %s", port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    ret = listen(sockfd, tcp_env_int("ERNIC_TCP_LISTEN_BACKLOG",
                                     TCP_DEFAULT_LISTEN_BACKLOG));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to listen on port %u: %s", port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    rdma_info_report("TCP: Listening on port %u", port);
    return sockfd;
}

/*
 * The payload may be given in two parts so a caller that already holds a
 * header and a body separately does not have to splice them into one buffer
 * first -- sendmsg() sends them as one message either way.
 *
 * The socket is non-blocking, so when the peer isn't reading fast enough the
 * loop waits for room.  Once priv->stopping is set it fails instead of
 * waiting, possibly part-way through the message; teardown is about to shut
 * the socket down anyway.  Callers usually hold conn->lock, and some hold
 * conn_table_lock too, so without this a peer that stops reading would hold
 * those locks, and teardown, indefinitely.
 */
static int tcp_send_message2(TcpBackendPrivate *priv, int sockfd,
                             TcpMsgType msg_type, const void *payload,
                             size_t payload_len, const void *payload2,
                             size_t payload2_len, uint32_t seq,
                             uint32_t src_node, uint32_t dst_node,
                             uint32_t src_qpn, uint32_t dst_qpn)
{
    TcpMsgHeader hdr;
    ssize_t ret;

    /* The receiver drops the connection on anything larger. */
    if (payload_len > TCP_MAX_PAYLOAD_LEN ||
        payload2_len > TCP_MAX_PAYLOAD_LEN - payload_len) {
        rdma_error_report("TCP: payload too large: %zu + %zu", payload_len,
                          payload2_len);
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
        if (ret < 0) {
            if (errno == EAGAIN) {
                struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};

                if (atomic_load(&priv->stopping)) {
                    rdma_info_report("TCP: Stopped while sending message");
                    return -1;
                }
                poll(&pfd, 1, 5);
                continue;
            }
            rdma_error_report("TCP: Failed to send message: %s",
                              strerror(errno));
            return -1;
        }
        sent += (size_t)ret;
    }

    return 0;
}

static int tcp_send_message(TcpBackendPrivate *priv, int sockfd,
                            TcpMsgType msg_type, const void *payload,
                            size_t payload_len, uint32_t seq, uint32_t src_node,
                            uint32_t dst_node, uint32_t src_qpn,
                            uint32_t dst_qpn)
{
    return tcp_send_message2(priv, sockfd, msg_type, payload, payload_len, NULL,
                             0, seq, src_node, dst_node, src_qpn, dst_qpn);
}

/*
 * Read from a non-blocking socket into buf until it holds len bytes, of which
 * the first got are already there, waiting for each part as it comes.  A peer
 * can withhold the rest, or trickle it, indefinitely, so the wait gives up
 * once *running is cleared, if running isn't NULL (otherwise the thread's stop
 * would wait on the peer), or once tcp_monotonic_ms() reaches deadline_ms, if
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
        if (deadline_ms != 0 && tcp_monotonic_ms() >= deadline_ms) {
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
    if (qemu_thread_create(thread, name, fn, arg, QEMU_THREAD_JOINABLE) != 0) {
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
 * deadline_ms (on tcp_monotonic_ms()'s clock) or when *running is cleared.
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
 * straight after.
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
        tcp_worker_connect_peer(priv, target, &priv->peer_thread_running,
                                tcp_monotonic_ms() +
                                    (uint64_t)priv->health_check_interval_sec *
                                        1000u);
    }
}

/*
 * Worker peer-connect thread.  Connects to the peers in the latest topology
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
                    TcpRegisterRespPayload nak;

                    rdma_error_report(
                        "TCP: refusing registration from a v%u peer "
                        "(this mesh is v%u); a peer that predates the GID "
                        "advertisement routes every destination to one "
                        "default node",
                        peer_version, TCP_PROTOCOL_VERSION);

                    memset(&nak, 0, sizeof(nak));
                    nak.result = htonl((uint32_t)(int32_t)-EPROTO);
                    nak.version = htonl(TCP_PROTOCOL_VERSION);

                    qemu_mutex_lock(&conn->lock);
                    tcp_send_message(
                        priv, conn->sockfd, TCP_MSG_REGISTER_RESP, &nak,
                        sizeof(nak),
                        atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                  memory_order_relaxed),
                        tcp_local_node(priv), hdr.src_node_id, 0, 0);
                    qemu_mutex_unlock(&conn->lock);

                    /*
                     * Nothing the peer sends now is of use, so stop reading
                     * it.  The connection is still pending, and the next
                     * reap closes it.
                     */
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

                qemu_mutex_lock(&priv->mesh_table_lock);
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

                /* Assign node ID */
                uint32_t assigned_id = requested_id;
                if (requested_id == 0xFFFFFFFF ||
                    tcp_node_id_taken(priv, requested_id)) {
                    /* Auto-assign: find next available ID */
                    assigned_id = priv->next_available_node_id;
                    while (tcp_node_id_taken(priv, assigned_id)) {
                        assigned_id++;
                    }
                    priv->next_available_node_id = assigned_id + 1;
                }

                /* Create mesh node info */
                MeshNodeInfo *node = tcp_mesh_node_info_new(
                    assigned_id, worker_host, worker_port);
                if (!node) {
                    rdma_error_report("TCP: Failed to create mesh node info "
                                      "for worker node %u",
                                      assigned_id);
                    /* Still unregistered, so teardown has to find it */
                    g_ptr_array_add(priv->pending_conns, conn);
                    qemu_mutex_unlock(&priv->conn_table_lock);
                    qemu_mutex_unlock(&priv->mesh_table_lock);
                    break;
                }

                /* Update connection with assigned node ID */
                conn->node_id = assigned_id;

                /*
                 * Add to the mesh and connections tables.  The connection
                 * takes its reference from pending_conns with it, and can't
                 * be refused: assigned_id is in neither table.
                 */
                g_hash_table_insert(priv->mesh_nodes,
                                    GUINT_TO_POINTER(assigned_id), node);
                tcp_conn_table_add(priv, assigned_id, conn);

                qemu_mutex_unlock(&priv->conn_table_lock);
                qemu_mutex_unlock(&priv->mesh_table_lock);

                rdma_info_report("TCP: Registered node %u at %s:%u",
                                 assigned_id, worker_host, worker_port);

                /* Send registration response.  Zeroed first: the whole
                 * struct goes on the wire, so any field not assigned below
                 * would put stack contents there. */
                TcpRegisterRespPayload resp;
                memset(&resp, 0, sizeof(resp));
                resp.assigned_node_id = htonl(assigned_id);
                resp.num_nodes = htonl(g_hash_table_size(priv->mesh_nodes));
                resp.result = htonl(0); /* Success */
                resp.version = htonl(TCP_PROTOCOL_VERSION);

                /*
                 * conn->lock serialises writes to this socket.
                 * The data path holds it across multi-megabyte
                 * sendmsg()s, so an unlocked control message here
                 * would interleave into the middle of one and the
                 * peer would read a header from the payload.
                 * Released before broadcasting: that path takes
                 * conn locks of its own and this mutex is not
                 * recursive.
                 */
                qemu_mutex_lock(&conn->lock);
                tcp_send_message(priv, conn->sockfd, TCP_MSG_REGISTER_RESP,
                                 &resp, sizeof(resp),
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
                 * Only the manager assigns this node its id, and only once.
                 * The answer rewrites the id and the GIDs this node claims,
                 * so one accepted from a peer, or a second one, would let
                 * another node take this one's place in the mesh.
                 * manager_conn is the connection this node opened to the
                 * manager itself; a peer can't be it.
                 */
                if (conn != priv->manager_conn) {
                    rdma_error_report("TCP: REGISTER_RESP from a connection "
                                      "that is not the manager; ignoring");
                    break;
                }

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
                if (!tcp_worker_registration_done(priv, assigned_id)) {
                    rdma_error_report("TCP: ignoring a REGISTER_RESP after "
                                      "registration was answered");
                    break;
                }
                if (assigned_id == 0) {
                    break;
                }

                rdma_info_report(
                    "TCP: Registered with manager, assigned node_id=%u",
                    assigned_id);

                /*
                 * The node id is only known now, so this is the first point
                 * at which this node can say which GIDs are its own.  Record
                 * them locally too: a QP whose destination is this node's own
                 * guest has to resolve to the local node, not fail.
                 */
                tcp_gid_claims_set(priv, assigned_id,
                                   tcp_const_gids(priv->local_gids),
                                   priv->num_local_gids);

                TcpNodeGidsPayload mine;
                tcp_fill_node_gids(&mine, assigned_id,
                                   tcp_const_gids(priv->local_gids),
                                   priv->num_local_gids);
                tcp_send_node_gids(priv, conn, &mine);
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
                 */
                if (conn != priv->manager_conn) {
                    rdma_error_report("TCP: MESH_TOPOLOGY from a connection "
                                      "that is not the manager; ignoring");
                    break;
                }

                /*
                 * Which peers this node connects to depends on its id.  The
                 * manager publishes a new worker's connection before it
                 * answers the registration, so the broadcast for another
                 * node's registration can come first.  It follows its answer
                 * with a broadcast of its own, so nothing is lost.
                 */
                if (!tcp_worker_registered(priv)) {
                    rdma_info_report("TCP: Ignoring a mesh topology update "
                                     "that came before registration");
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
                 * this node opened itself.  Matching on src_node_id == 0
                 * instead would let any peer claim to be the manager.
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
                } else if (conn != priv->manager_conn) {
                    rdma_error_report("TCP: NODE_GIDS for node %u from a "
                                      "connection that is not the manager; "
                                      "ignoring",
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
                     * registered node's or a reconnect to one, and carries
                     * that node's id.
                     */
                    bool revived = false;

                    qemu_mutex_lock(&priv->mesh_table_lock);
                    MeshNodeInfo *node = g_hash_table_lookup(
                        priv->mesh_nodes, GUINT_TO_POINTER(conn->node_id));
                    if (node) {
                        node->last_heartbeat = time(NULL);
                        revived = !node->is_alive;
                        node->is_alive = true;
                    }
                    qemu_mutex_unlock(&priv->mesh_table_lock);

                    /* Serialised against the data path; see the
                     * REGISTER_RESP send above. */
                    qemu_mutex_lock(&conn->lock);
                    tcp_send_message(
                        priv, conn->sockfd, TCP_MSG_HEARTBEAT_RESP, NULL, 0,
                        atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                  memory_order_relaxed),
                        tcp_local_node(priv), conn->node_id, 0, 0);
                    qemu_mutex_unlock(&conn->lock);

                    /*
                     * The rest of the mesh was told the node is dead, and
                     * won't connect to it until it hears otherwise.
                     */
                    if (revived) {
                        rdma_info_report("TCP: Node %u is answering again",
                                         conn->node_id);
                        tcp_broadcast_mesh_topology(priv);
                    }
                } else {
                    /* Worker received heartbeat probe from manager —
                     * echo it back so the manager refreshes our
                     * liveness timestamp. */
                    qemu_mutex_lock(&conn->lock);
                    tcp_send_message(
                        priv, conn->sockfd, TCP_MSG_HEARTBEAT, NULL, 0,
                        atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                  memory_order_relaxed),
                        tcp_local_node(priv), hdr.src_node_id, 0, 0);
                    qemu_mutex_unlock(&conn->lock);
                }
                break;
            }

            case TCP_MSG_HEARTBEAT_RESP: {
                /* Acknowledgement — no action needed. */
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
                        if (fwd->sockfd >= 0) {
                            tcp_send_message(priv, fwd->sockfd, TCP_MSG_IONIC,
                                             payload, hdr.msg_len, hdr.seq,
                                             hdr.src_node_id, hdr.dst_node_id,
                                             hdr.src_qpn, hdr.dst_qpn);
                            relayed = true;
                        }
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
    int ret = tcp_send_message(conn->priv, conn->sockfd, msg_type, &payload,
                               sizeof(payload), 1, local_node_id, conn->node_id,
                               0, 0);
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
    uint64_t now_ms = tcp_monotonic_ms();

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

        if (sockfd < 0) {
            if (errno == EAGAIN) {
                usleep(100000); /* 100ms */
                continue;
            }
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
            usleep(100000); /* 100ms */
            continue;
        }

        /* Disable Nagle -- send immediately */
        int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        tcp_tune_sockbuf(sockfd);

        rdma_info_report("TCP: Accepted connection from %s:%u",
                         inet_ntoa(client_addr.sin_addr),
                         ntohs(client_addr.sin_port));

        if (priv->is_manager) {
            /* Make room before counting what's waiting */
            tcp_reap_pending_conns(priv);

            /* Manager mode: accept connection and let receive thread handle
             * REGISTER_NODE.  The node id is unknown until registration. */
            conn =
                tcp_connection_new(UINT32_MAX, inet_ntoa(client_addr.sin_addr),
                                   ntohs(client_addr.sin_port));
            conn->sockfd = sockfd;
            atomic_store(&conn->is_connected, true);
            conn->priv = priv;
            conn->accepted_ms = tcp_monotonic_ms();

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
                tcp_monotonic_ms() + TCP_HANDSHAKE_TIMEOUT_MS;
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
                    if (tcp_monotonic_ms() >= deadline_ms) {
                        rdma_error_report("TCP: Timeout waiting for handshake "
                                          "from incoming connection");
                        close(sockfd);
                        goto next_connection;
                    }
                    usleep(100000); /* 100ms */
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
            conn = tcp_connection_new(remote_node_id,
                                      inet_ntoa(client_addr.sin_addr),
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

    qemu_mutex_lock(&priv->topology_broadcast_lock);
    qemu_mutex_lock(&priv->mesh_table_lock);

    /* Build topology payload */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->mesh_nodes);

    while (g_hash_table_iter_next(&iter, &key, &value) && num_nodes < 64) {
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

    /* Broadcast to all connected workers */
    qemu_mutex_lock(&priv->conn_table_lock);
    GHashTableIter conn_iter;
    g_hash_table_iter_init(&conn_iter, priv->connections);

    while (g_hash_table_iter_next(&conn_iter, &key, &value)) {
        TcpConnection *conn = (TcpConnection *)value;
        if (!conn) {
            continue;
        }
        /* Checked under the lock tcp_retire_conn() changes them under */
        qemu_mutex_lock(&conn->lock);
        if (atomic_load(&conn->is_connected) && conn->sockfd >= 0) {
            tcp_send_message(priv, conn->sockfd, TCP_MSG_MESH_TOPOLOGY, &topo,
                             sizeof(TcpMeshTopologyPayload),
                             atomic_fetch_add_explicit(&priv->next_seq, 1,
                                                       memory_order_relaxed),
                             tcp_local_node(priv), conn->node_id, 0, 0);
        }
        qemu_mutex_unlock(&conn->lock);
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
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
            priv, conn->sockfd, TCP_MSG_NODE_GIDS, payload, sizeof(*payload),
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
static void tcp_broadcast_gid_table(TcpBackendPrivate *priv)
{
    if (!priv->is_manager || !priv->node_gids || !priv->mesh_nodes)
        return;

    /* Held across both the snapshot and the sends; see the field's comment. */
    qemu_mutex_lock(&priv->gid_broadcast_lock);

    struct {
        uint32_t node_id;
        uint8_t gids[TCP_MAX_NODE_GIDS][16];
        uint32_t num_gids;
    } per_node[64];
    uint32_t nodes = 0;

    /*
     * Snapshot under the mesh and claim locks; the sends are not, because
     * tcp_send_node_gids() takes connection locks that the data path holds
     * across multi-megabyte writes.
     */
    qemu_mutex_lock(&priv->mesh_table_lock);
    qemu_mutex_lock(&priv->gid_table_lock);

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, &key, &value) &&
           nodes < G_N_ELEMENTS(per_node)) {
        uint32_t nid = GPOINTER_TO_UINT(key);
        const TcpNodeGids *claims =
            g_hash_table_lookup(priv->node_gids, GUINT_TO_POINTER(nid));

        per_node[nodes].node_id = nid;
        per_node[nodes].num_gids = claims ? claims->num_gids : 0;
        for (uint32_t i = 0; i < per_node[nodes].num_gids; i++)
            memcpy(per_node[nodes].gids[i], claims->gids[i], 16);
        nodes++;
    }

    qemu_mutex_unlock(&priv->gid_table_lock);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    for (uint32_t i = 0; i < nodes; i++) {
        TcpNodeGidsPayload payload;

        tcp_fill_node_gids(&payload, per_node[i].node_id,
                           tcp_const_gids(per_node[i].gids),
                           per_node[i].num_gids);

        qemu_mutex_lock(&priv->conn_table_lock);
        GHashTableIter conn_iter;
        gpointer ck, cv;
        g_hash_table_iter_init(&conn_iter, priv->connections);
        while (g_hash_table_iter_next(&conn_iter, &ck, &cv)) {
            TcpConnection *conn = (TcpConnection *)cv;

            if (conn)
                tcp_send_node_gids(priv, conn, &payload);
        }
        qemu_mutex_unlock(&priv->conn_table_lock);
    }

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
 * the caller the only one, and teardown's tcp_claim_recv_thread() runs only
 * once every thread that does that has stopped.  The thread is joined only if
 * this call is the one that clears its flag, and the socket is shut down
 * before the join so a send the thread is blocked in can't hold it up.  The
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
    }

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
 * intervals.  The caller holds mesh_table_lock.
 */
static bool tcp_node_silent(const TcpBackendPrivate *priv,
                            const MeshNodeInfo *node, time_t now)
{
    return now - node->last_heartbeat >
           (time_t)priv->health_check_interval_sec * 3;
}

/*
 * The part of a health-check pass that needs mesh_table_lock: note each
 * other node's connection, taking a reference to it, declare dead the nodes
 * whose connection has ended and those that have stopped answering, and pick
 * the ones due a reconnect attempt.  Returns true if a node was newly
 * declared dead, which the rest of the mesh has to be told.
 */
static bool tcp_health_check_scan(TcpBackendPrivate *priv, GArray *items,
                                  time_t now)
{
    bool newly_dead = false;
    GHashTableIter iter;
    gpointer value;

    qemu_mutex_lock(&priv->mesh_table_lock);
    g_hash_table_iter_init(&iter, priv->mesh_nodes);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        MeshNodeInfo *node = (MeshNodeInfo *)value;
        TcpHealthCheckItem item = {node, NULL, false, false};
        time_t dead_sec = now - node->last_heartbeat;

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
                newly_dead = true;
            }
            item.disconnected = true;
        }

        if (tcp_node_silent(priv, node, now)) {
            if (node->is_alive) {
                rdma_warn_report("TCP: Node %u failed health check (last "
                                 "heartbeat: %ld seconds ago)",
                                 node->node_id, (long)dead_sec);
                node->is_alive = false;
                newly_dead = true;
            }

            /* Attempt reconnection with exponential backoff (max 60 s) */
            time_t backoff = 1;
            while (backoff * 2 <= dead_sec && backoff < 60)
                backoff *= 2;
            if (backoff > 60)
                backoff = 60;
            item.reconnect =
                (node->reconnect_deferred != 0 || dead_sec % backoff == 0) &&
                node->hostname != NULL;
        } else {
            node->reconnect_deferred = 0;
        }

        /* Every reference taken above lands in items, to be released */
        if (item.conn || item.reconnect)
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
 * Retire the connections the scan found ended, and take them out of
 * connections.  Otherwise each would keep its descriptor, in CLOSE_WAIT,
 * until a reconnect to the node succeeds, which may be never.  Threads that
 * still hold one see their sends on it fail, and the data path no longer
 * finds it.
 */
static void tcp_health_check_retire(TcpBackendPrivate *priv, GArray *items)
{
    for (guint i = 0; i < items->len; i++) {
        TcpHealthCheckItem *item = &g_array_index(items, TcpHealthCheckItem, i);

        if (!item->disconnected)
            continue;
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
 * Deferred reconnects first, the longest deferred first.  g_array_sort() is
 * stable, so the rest keep the scan's order.
 */
static gint tcp_health_check_item_cmp(gconstpointer a, gconstpointer b)
{
    uint64_t x = ((const TcpHealthCheckItem *)a)->node->reconnect_deferred;
    uint64_t y = ((const TcpHealthCheckItem *)b)->node->reconnect_deferred;

    if (x == y)
        return 0;
    if (x == 0)
        return 1;
    if (y == 0)
        return -1;
    return x < y ? -1 : 1;
}

/*
 * Whether node is still silent.  The scan's verdict can be out of date by
 * the time it's acted on: a node that answers a heartbeat late, on the
 * connection a reconnect would retire, has its heartbeat recorded while the
 * reconnect is under way.
 */
static bool tcp_node_still_silent(TcpBackendPrivate *priv, MeshNodeInfo *node)
{
    bool silent;

    qemu_mutex_lock(&priv->mesh_table_lock);
    silent = tcp_node_silent(priv, node, time(NULL));
    qemu_mutex_unlock(&priv->mesh_table_lock);
    if (!silent)
        rdma_info_report("TCP: Node %u has answered; not reconnecting",
                         node->node_id);
    return silent;
}

/*
 * Reconnect to item->node, retiring item->conn, giving up at deadline_ms
 * (on tcp_monotonic_ms()'s clock).  Called with no lock held: the connect
 * can take until the deadline, the handshake can wait on the peer, and
 * retiring the old connection joins its receive thread, whose handlers take
 * mesh_table_lock.  Returns true if the node has a new connection.
 *
 * The old connection leaves the connections table before the new one is
 * made, so its end-of-file reaches the node ahead of the new handshake, and
 * a reconnect that fails leaves no entry for the next attempt to retire
 * again.  A thread still holding the old connection keeps it, retired, until
 * it drops its reference.
 *
 * A node that answers before the connect, or during it, is left alone.  One
 * that answers once its old connection is being retired gets the new
 * connection anyway; if that fails, the node goes silent again, so a later
 * pass reconnects to it.
 */
bool tcp_health_check_reconnect(TcpBackendPrivate *priv,
                                const TcpHealthCheckItem *item,
                                uint64_t deadline_ms)
{
    MeshNodeInfo *node = item->node;

    if (!tcp_node_still_silent(priv, node))
        return false;

    rdma_info_report("TCP: Attempting reconnect to node %u (%s:%u)",
                     node->node_id, node->hostname, node->port);
    int fd = tcp_connect_to_remote(node->hostname, node->port,
                                   &priv->health_check_running, deadline_ms);
    if (fd < 0)
        return false;
    if (!tcp_node_still_silent(priv, node)) {
        close(fd);
        return false;
    }
    atomic_fetch_add_explicit(&priv->tcp_stats.reconnect_attempts, 1,
                              memory_order_relaxed);

    if (item->conn) {
        tcp_health_check_unpublish(priv, node->node_id, item->conn);
    }

    TcpConnection *new_conn =
        tcp_connection_new(node->node_id, node->hostname, node->port);
    new_conn->priv = priv;
    new_conn->sockfd = fd;
    atomic_store(&new_conn->is_connected, true);

    if (tcp_send_handshake(new_conn, tcp_local_node(priv), TCP_MSG_HANDSHAKE) <
        0) {
        rdma_error_report("TCP: Failed to send handshake after reconnect to "
                          "node %u",
                          node->node_id);
        tcp_connection_unref(new_conn);
        return false;
    }

    char tname[32];
    snprintf(tname, sizeof(tname), "tcp-recv-%u", node->node_id);
    if (!tcp_start_recv_thread(new_conn, tname)) {
        tcp_connection_unref(new_conn);
        return false;
    }

    /*
     * Both tables together, so no thread finds one updated and not the
     * other.  Only this thread changes a registered node's entry, which it
     * has just removed, so the add isn't refused in practice; if it were,
     * the new connection is dropped rather than the one in the table.
     */
    qemu_mutex_lock(&priv->mesh_table_lock);
    qemu_mutex_lock(&priv->conn_table_lock);
    if (!tcp_conn_table_add(priv, node->node_id, new_conn)) {
        qemu_mutex_unlock(&priv->conn_table_lock);
        qemu_mutex_unlock(&priv->mesh_table_lock);
        tcp_retire_conn(new_conn);
        tcp_connection_unref(new_conn);
        return false;
    }
    qemu_mutex_unlock(&priv->conn_table_lock);
    node->is_alive = true;
    node->last_heartbeat = time(NULL);
    qemu_mutex_unlock(&priv->mesh_table_lock);

    rdma_info_report("TCP: Reconnected to node %u", node->node_id);
    atomic_fetch_add_explicit(&priv->tcp_stats.reconnect_successes, 1,
                              memory_order_relaxed);
    return true;
}

/*
 * The reconnect phase of a health-check pass: try the items due a
 * reconnect, until deadline_ms (on tcp_monotonic_ms()'s clock) or a stop.
 * Returns true if a node was reconnected.
 *
 * A node whose turn comes after the deadline is deferred: it is due on the
 * next pass whatever its backoff, and goes first, after any node deferred
 * before it.  The first in line is always tried, so every deferred node
 * moves up a place each pass until it has its turn.  Otherwise a node that
 * never answers would use up every pass's time, and the nodes behind it
 * would never be tried.
 */
bool tcp_health_check_reconnects(TcpBackendPrivate *priv, GArray *items,
                                 uint64_t deadline_ms)
{
    bool reconnected = false;

    g_array_sort(items, tcp_health_check_item_cmp);
    for (guint i = 0;
         i < items->len && atomic_load(&priv->health_check_running); i++) {
        const TcpHealthCheckItem *item =
            &g_array_index(items, TcpHealthCheckItem, i);

        if (!item->reconnect)
            continue;
        if (tcp_monotonic_ms() >= deadline_ms) {
            /* Already in line, it keeps its place */
            if (item->node->reconnect_deferred == 0)
                item->node->reconnect_deferred = ++priv->reconnect_deferrals;
            continue;
        }
        item->node->reconnect_deferred = 0;
        if (tcp_health_check_reconnect(priv, item, deadline_ms))
            reconnected = true;
    }
    return reconnected;
}

/*
 * Manager health check thread.  Each pass releases the connections that are
 * done waiting to register, retires the connections of nodes that have
 * hung up, sends every other node a heartbeat, declares dead the nodes that
 * have hung up or haven't answered for three intervals, and tries to
 * reconnect to them.
 *
 * Only the scan holds mesh_table_lock.  The rest of the pass can wait on a
 * peer for as long as a connect takes, which would keep registrations
 * waiting, and a reconnect joins the old connection's receive thread, which
 * takes mesh_table_lock to record a heartbeat; holding the lock across the
 * join would deadlock with it.
 */
void *tcp_manager_health_check_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(TcpHealthCheckItem));

    rdma_info_report("TCP: Manager health check thread started");

    while (atomic_load(&priv->health_check_running)) {
        /* The accept thread reaps too, but only when a peer connects */
        tcp_reap_pending_conns(priv);

        bool newly_dead = tcp_health_check_scan(priv, items, time(NULL));
        tcp_health_check_retire(priv, items);

        for (guint i = 0; i < items->len; i++) {
            const TcpHealthCheckItem *item =
                &g_array_index(items, TcpHealthCheckItem, i);
            TcpConnection *conn = item->conn;

            if (!conn)
                continue;
            qemu_mutex_lock(&conn->lock);
            if (atomic_load(&conn->is_connected) && conn->sockfd >= 0) {
                tcp_send_message(priv, conn->sockfd, TCP_MSG_HEARTBEAT, NULL, 0,
                                 atomic_fetch_add_explicit(
                                     &priv->next_seq, 1, memory_order_relaxed),
                                 tcp_local_node(priv), item->node->node_id, 0,
                                 0);
            }
            qemu_mutex_unlock(&conn->lock);
        }

        if (newly_dead)
            tcp_broadcast_mesh_topology(priv);

        /*
         * All of the pass's reconnects share one interval.  No heartbeat
         * goes out until the next pass, an interval after this one ends,
         * and a node is declared dead after three intervals without one.
         * With the reconnects bounded, the answer to this pass's heartbeat,
         * or the registration of a node that joins meanwhile, is less than
         * three intervals old when the next pass looks at it.
         */
        if (tcp_health_check_reconnects(
                priv, items,
                tcp_monotonic_ms() +
                    (uint64_t)priv->health_check_interval_sec * 1000u))
            tcp_broadcast_mesh_topology(priv);
        tcp_health_check_release(items);

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

/* Worker registers with manager */
static int tcp_worker_register_with_manager(TcpBackendPrivate *priv)
{
    TcpRegisterNodePayload reg;
    int ret;
    char hostname[256];

    /* Get local hostname */
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        strncpy(hostname, "localhost", sizeof(hostname) - 1);
    }
    hostname[sizeof(hostname) - 1] = '\0';

    /* Connect to manager */
    priv->manager_conn =
        tcp_connection_new(0, priv->manager_host, priv->manager_port);
    priv->manager_conn->priv = priv;
    priv->manager_conn->sockfd =
        tcp_connect_to_remote(priv->manager_host, priv->manager_port, NULL, 0);

    if (priv->manager_conn->sockfd < 0) {
        rdma_error_report("TCP: Failed to connect to manager at %s:%u",
                          priv->manager_host, priv->manager_port);
        tcp_connection_unref(priv->manager_conn);
        priv->manager_conn = NULL;
        return -1;
    }

    atomic_store(&priv->manager_conn->is_connected, true);

    /* Start receive thread for manager connection */
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "tcp-mgr-recv");
    if (!tcp_start_recv_thread(priv->manager_conn, thread_name)) {
        tcp_connection_unref(priv->manager_conn);
        priv->manager_conn = NULL;
        return -1;
    }

    /* Prepare registration payload */
    memset(&reg, 0, sizeof(reg));
    g_strlcpy(reg.hostname, hostname, sizeof(reg.hostname));
    reg.port = htons(priv->listen_port);
    reg.requested_node_id = htonl(0xFFFFFFFF); /* Auto-assign */
    reg.version = htonl(TCP_PROTOCOL_VERSION);

    /* Send registration request, under conn->lock like every write to a
     * connection's socket: its receive thread is already running and
     * writes to the socket too. */
    qemu_mutex_lock(&priv->manager_conn->lock);
    ret = tcp_send_message(
        priv, priv->manager_conn->sockfd, TCP_MSG_REGISTER_NODE, &reg,
        sizeof(reg),
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed), 0,
        0, 0, 0);
    qemu_mutex_unlock(&priv->manager_conn->lock);
    if (ret < 0) {
        rdma_error_report("TCP: Failed to send registration request");
        return -1;
    }

    /* Wait for registration to complete (handled by receive thread) */
    qemu_mutex_lock(&priv->registration_mutex);
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += TCP_REGISTER_TIMEOUT_MS / 1000;

    while (!priv->registration_complete) {
        if (!qemu_cond_timedwait_until(&priv->registration_cond,
                                       &priv->registration_mutex, &deadline) &&
            !priv->registration_complete) {
            qemu_mutex_unlock(&priv->registration_mutex);
            rdma_error_report("TCP: Registration timeout");
            return -1;
        }
    }

    /* The receive thread logged why if the manager refused this node */
    bool success = tcp_worker_registered(priv);
    qemu_mutex_unlock(&priv->registration_mutex);

    if (!success) {
        rdma_error_report("TCP: Registration failed");
        return -1;
    }

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
 * gives up within 100 ms of the flag being cleared.  The flag is cleared
 * under peer_lock, with peer_cond signalled, so the thread can't miss it.  A
 * no-op when the thread was never started.
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
    qemu_thread_join(&priv->peer_thread);
}

/*
 * Clear conn's stop flag and queue its thread for a join, if it's running.
 * The flag alone isn't enough: the thread may be blocked sending to a peer
 * that has stopped reading, and that loop waits on the peer, not the flag.
 * Shutting the socket down fails any send in progress, or to come, with
 * EPIPE, and any receive with end-of-file.  The descriptor stays open, so
 * the thread can't end up using a reused one; it's closed when conn is
 * freed.
 */
static void tcp_claim_recv_thread(GPtrArray *batch, TcpConnection *conn)
{
    if (atomic_exchange(&conn->recv_thread_running, false)) {
        if (conn->sockfd >= 0) {
            shutdown(conn->sockfd, SHUT_RDWR);
        }
        g_ptr_array_add(batch, conn);
    }
}

/*
 * Stop every receive thread and wait for them, freeing nothing.  A receive
 * thread can reach any connection, and the mesh and GID tables, through
 * priv, so none of that may be freed while any of them still runs.  The
 * accept, health-check and peer-connect threads, which add connections with
 * threads of their own, must already be stopped.  No receive thread starts
 * another, but this repeats until a pass finds nothing left to stop anyway,
 * so one that did couldn't leave its thread running.
 */
void tcp_stop_recv_threads(TcpBackendPrivate *priv)
{
    for (;;) {
        GPtrArray *batch = g_ptr_array_new();
        GHashTableIter iter;
        gpointer value;

        qemu_mutex_lock(&priv->conn_table_lock);
        g_hash_table_iter_init(&iter, priv->connections);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            tcp_claim_recv_thread(batch, value);
        }
        for (guint i = 0; i < priv->pending_conns->len; i++) {
            tcp_claim_recv_thread(batch,
                                  g_ptr_array_index(priv->pending_conns, i));
        }
        if (priv->manager_conn) {
            tcp_claim_recv_thread(batch, priv->manager_conn);
        }
        qemu_mutex_unlock(&priv->conn_table_lock);

        guint stopped = batch->len;
        for (guint i = 0; i < stopped; i++) {
            TcpConnection *conn = g_ptr_array_index(batch, i);
            qemu_thread_join(&conn->recv_thread);
        }
        g_ptr_array_free(batch, TRUE);
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
 * sending to a peer that has stopped reading, holding that connection's lock
 * and possibly conn_table_lock (a broadcast sends under it).  The
 * health-check thread can then be waiting for the connection's lock, and
 * tcp_stop_recv_threads() for conn_table_lock, before it can shut any socket
 * down.  With the flag set, such a send fails within one poll interval and
 * releases its locks.
 *
 * The health-check, accept and peer-connect threads add connections, so
 * they go before the receive threads.
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
    atomic_init(&priv->tcp_stats.reconnect_attempts, 0);
    atomic_init(&priv->tcp_stats.reconnect_successes, 0);
    atomic_init(&priv->tcp_stats.peer_connect_attempts, 0);

    priv->listen_fd = -1;
    priv->is_listening = false;
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->stopping, false);

    qemu_mutex_init(&priv->lock);
    qemu_mutex_init(&priv->conn_table_lock);
    qemu_mutex_init(&priv->gid_table_lock);
    qemu_mutex_init(&priv->gid_broadcast_lock);
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
    priv->manager_conn = NULL;
    priv->registration_complete = false;
    qemu_cond_init(&priv->registration_cond);
    qemu_mutex_init(&priv->registration_mutex);

    /* Default mesh metadata */
    backend_dev->mesh_enabled = false;
    backend_dev->mesh_node_id = 0;
    backend_dev->mesh_num_nodes = 0;

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

        backend_dev->mesh_enabled = true;
        backend_dev->mesh_node_id = 0;
        backend_dev->mesh_num_nodes = 1; /* Will grow as workers register */

        rdma_info_report("TCP backend: Manager mode on port %u", manager_port);

        /* Start listening */
        priv->listen_port = manager_port;
        priv->listen_fd = tcp_listen_on_port(manager_port);
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

        /* Find an available port for this worker */
        priv->listen_port =
            0; /* Will be assigned by manager or use ephemeral */
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int test_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (test_fd >= 0) {
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = 0; /* Let OS assign */
            if (bind(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                if (getsockname(test_fd, (struct sockaddr *)&addr, &len) == 0) {
                    priv->listen_port = ntohs(addr.sin_port);
                }
            }
            close(test_fd);
        }

        /* Start listening on ephemeral port */
        priv->listen_fd = tcp_listen_on_port(priv->listen_port);
        if (priv->listen_fd < 0) {
            /* manager_host already assigned to priv->manager_host, will be
             * freed in error path */
            goto error;
        }
        priv->is_listening = true;

        /* Register with manager */
        ret = tcp_worker_register_with_manager(priv);
        if (ret < 0) {
            /* manager_host already assigned to priv->manager_host, will be
             * freed in error path */
            goto error;
        }

        /*
         * Start the accept and peer-connect threads only now that this node
         * has its id.  Both introduce the node to its peers with it, and
         * before registration it reads as 0, the manager's.  A peer that
         * connects before then waits in the listen socket's backlog.  This
         * also means every thread that reads manager_conn starts after it
         * is set.
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
    if (priv->manager_conn) {
        tcp_connection_unref(priv->manager_conn);
    }
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
    if (priv->manager_conn) {
        tcp_connection_unref(priv->manager_conn);
    }
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
    qemu_mutex_destroy(&priv->topology_broadcast_lock);

    g_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int tcp_query_port(RdmaBackendDev *backend_dev,
                          struct ibv_port_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->state = IBV_PORT_ACTIVE;
    attr->max_mtu = IBV_MTU_4096;
    attr->active_mtu = IBV_MTU_4096;
    /* Allow multiple GIDs so the driver can expose per-node mesh GIDs */
    attr->gid_tbl_len = MAX_PORT_GIDS;
    attr->port_cap_flags = IBV_PORT_CM_SUP;
    attr->max_msg_sz = 0x80000000;
    attr->pkey_tbl_len = 1;
    attr->active_width = 4; /* 4X */
    attr->active_speed = 4; /* 10 Gbps */
    attr->phys_state = 5;   /* LinkUp */
    return 0;
}

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

    pd->ibpd = (struct ibv_pd *)(uintptr_t)handle;

    rdma_info_report("TCP: Created PD handle %u", handle);
    return 0;
}

static void tcp_destroy_pd(RdmaBackendPD *pd)
{
    rdma_info_report("TCP: Destroyed PD");
}

/*
 * Memory Region Operations
 */

static int tcp_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                         size_t length, uint64_t guest_start, int access)
{
    (void)access;

    mr->ibpd = pd->ibpd;
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

static uint32_t tcp_mr_rkey(const RdmaBackendMR *mr)
{
    (void)mr;
    return 0;
}

/*
 * Completion Queue Operations
 */

static int tcp_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                         int cqe)
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

    rdma_info_report("TCP: Created CQ handle %u, cqe=%d", handle, cqe);
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

    qp->ibpd = pd->ibpd;
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

static int tcp_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       union ibv_gid *gid)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (priv) {
        uint32_t local = tcp_local_node(priv);

        memset(gid, 0, sizeof(*gid));
        gid->raw[15] = (uint8_t)local;
        rdma_info_report("TCP: Added GID with node_id=%u", local);
    } else {
        rdma_info_report("TCP: Added GID (no priv)");
    }

    return 0;
}

static int tcp_del_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       int gid_idx)
{
    rdma_info_report("TCP: Deleted GID index %d", gid_idx);
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

    .query_port = tcp_query_port,
    .query_device = tcp_query_device,

    .create_pd = tcp_create_pd,
    .destroy_pd = tcp_destroy_pd,

    .create_mr = tcp_create_mr,
    .destroy_mr = tcp_destroy_mr,
    .mr_lkey = tcp_mr_lkey,
    .mr_rkey = tcp_mr_rkey,

    .create_cq = tcp_create_cq,
    .destroy_cq = tcp_destroy_cq,

    .create_qp = tcp_create_qp,
    .destroy_qp = tcp_destroy_qp,
    .qpn = tcp_qpn,

    .qp_state_init = tcp_qp_state_init,
    .qp_state_rtr = tcp_qp_state_rtr,
    .qp_state_rts = tcp_qp_state_rts,
    .query_qp = tcp_query_qp,

    .add_gid = tcp_add_gid,
    .del_gid = tcp_del_gid,
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
 * Unicast an opaque ionic message to @dst_node.  Unlike Ethernet frames these
 * carry RDMA payloads and completions, so a drop is not recoverable by a
 * guest retransmit: send blocking (tcp_send_message polls on EAGAIN) and
 * report failure to the caller, which turns it into an error completion.
 */
int tcp_backend_send_ionic_v(RdmaBackendDev *backend_dev, uint32_t dst_node,
                             const void *hdr, size_t hdr_len, const void *body,
                             size_t body_len)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *conn;
    size_t len = hdr_len + body_len;
    uint32_t local;
    int rc;

    if (!priv || backend_dev->backend_type != RDMA_BACKEND_TYPE_TCP || !hdr ||
        hdr_len == 0 || len > TCP_MAX_PAYLOAD_LEN)
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

    /* Retired since it was picked: no socket left to send on */
    qemu_mutex_lock(&conn->lock);
    if (conn->sockfd < 0) {
        qemu_mutex_unlock(&conn->lock);
        tcp_connection_unref(conn);
        rdma_error_report("TCP: connection to node %u closed before an ionic "
                          "message could be sent",
                          dst_node);
        return -ENOTCONN;
    }
    rc = tcp_send_message2(
        priv, conn->sockfd, TCP_MSG_IONIC, hdr, hdr_len, body, body_len,
        atomic_fetch_add_explicit(&priv->next_seq, 1, memory_order_relaxed),
        local, dst_node, 0, 0);
    qemu_mutex_unlock(&conn->lock);
    tcp_connection_unref(conn);

    return rc < 0 ? -EIO : 0;
}
