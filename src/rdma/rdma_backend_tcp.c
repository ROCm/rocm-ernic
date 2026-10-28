/*
 * RDMA Backend: TCP/IP Network - Multi-Node Support
 *
 * TCP/IP backend for connecting multiple rocm_ernic server instances
 * over network in a mesh topology. Enables RDMA operations between
 * multiple servers without physical hardware.
 *
 * This file sets the backend up and tears it down, and holds the backend's
 * operations and the API the ionic data path calls.  The mesh itself is in
 * the tcp_*.c files beside it: tcp_wire.c (sockets and messages), tcp_conn.c
 * (connections and their threads), tcp_gid.c (which node owns each GID),
 * tcp_manager.c and tcp_worker.c (each role's part).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma/rdma_backend_ops.h"
#include "rdma/rdma_backend_tcp_priv.h"
#include "ernic_limits.h"
#include "ernic_mesh.h"
#include "ernic_time.h"
#include "parse_int.h"
#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_utils.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <unistd.h>
#include <inttypes.h>

/* Tunable default -- overridable via ERNIC_TCP_HEALTH_INTERVAL */
#define TCP_DEFAULT_HEALTH_INTERVAL_S 5

/* How often a receive thread offers the data path a message it refused */
#define TCP_IONIC_RETRY_US 1000u

/*
 * The value of the environment variable @name, if it is a positive integer,
 * or else @fallback.
 */
int tcp_env_int(const char *name, int fallback)
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
    QemuMutex lock; /* a leaf (see Locks in rdma_backend_tcp_priv.h) */
    uint32_t max_wr;
    uint32_t max_sge;
    uint32_t srq_limit TSA_GUARDED_BY(lock);
} TcpSRQ;

static TcpBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    if (!backend_dev) {
        return NULL;
    }
    return (TcpBackendPrivate *)backend_dev->backend_private;
}

/* @s past @prefix, or NULL if @s doesn't start with it */
static const char *tcp_skip_prefix(const char *s, const char *prefix)
{
    size_t len = strlen(prefix);

    return strncmp(s, prefix, len) == 0 ? s + len : NULL;
}

/*
 * The role a TCP backend config names: "manager:" or "worker:" at its
 * start, after an optional "tcp:" (the server passes what follows
 * "--backend tcp:", the tests the whole string).  Returns what follows the
 * role, setting *mode, or NULL, leaving *mode alone, if the config names
 * neither.  Only the prefix counts: "worker:mesh-manager:5000" is a worker
 * whose manager is mesh-manager.
 */
const char *tcp_config_role(const char *config, TcpMode *mode)
{
    const char *body = tcp_skip_prefix(config, "tcp:");
    const char *args;

    if (!body) {
        body = config;
    }

    args = tcp_skip_prefix(body, "manager:");
    if (args) {
        *mode = TCP_MODE_MANAGER;
        return args;
    }
    args = tcp_skip_prefix(body, "worker:");
    if (args) {
        *mode = TCP_MODE_WORKER;
    }
    return args;
}

/*
 * Split @s, "host:port", into a newly allocated *host and *port.  An IPv6
 * address goes in brackets, as in a URL ("[fe80::1%eth0]:5000"), and *host
 * is what's inside them.  A host outside brackets can't contain a colon, so a
 * bare IPv6 address, or anything after the port, is refused rather than split
 * somewhere else.  Returns 0, or -EINVAL having logged why, starting with
 * @who.
 */
int tcp_parse_host_port(const char *s, const char *who, char **host,
                        uint16_t *port)
{
    const char *start = s;
    const char *end;
    const char *port_str;
    const char *why;

    if (s[0] == '[') {
        start = s + 1;
        end = strchr(start, ']');
        if (!end || end[1] != ':') {
            why = "no ']:' after the IPv6 address";
            goto invalid;
        }
        /* Brackets are for IPv6 addresses, which always have a colon */
        if (memchr(start, ':', (size_t)(end - start)) == NULL ||
            memchr(start, '[', (size_t)(end - start)) != NULL) {
            why = "not an IPv6 address in the brackets";
            goto invalid;
        }
    } else {
        end = s + strcspn(s, ":[]");
        if (*end != ':') {
            why = *end ? "a bracket outside an IPv6 address" : "no port";
            goto invalid;
        }
    }

    if (end == start) {
        why = "no host";
        goto invalid;
    }

    port_str = end + (*end == ']' ? 2 : 1);
    if (strchr(port_str, ':')) {
        why = "more than one ':' (an IPv6 address goes in brackets)";
        goto invalid;
    }
    if (!ernic_parse_port(port_str, port)) {
        why = "an invalid port";
        goto invalid;
    }

    *host = g_strndup(start, (gsize)(end - start));
    return 0;

invalid:
    rdma_error_report("%s: invalid address \"%s\": %s; expected host:port or "
                      "[IPv6 address]:port",
                      who, s, why);
    return -EINVAL;
}

/*
 * Parse a manager config, after its "manager:" (see tcp_config_role()):
 * "<host>:<port>" or "listen:<port>".  *manager_host is left NULL for
 * "listen:".
 */
static int parse_tcp_config_manager(const char *args, char **manager_host,
                                    uint16_t *manager_port, bool *listen_mode)
{
    const char *port_str = tcp_skip_prefix(args, "listen:");

    if (port_str || strcmp(args, "listen") == 0) {
        if (!port_str || !ernic_parse_port(port_str, manager_port)) {
            rdma_error_report("TCP manager: invalid port for listen mode: "
                              "\"%s\"",
                              args);
            return -EINVAL;
        }
        *manager_host = NULL;
        *listen_mode = true;
        return 0;
    }

    *listen_mode = false;
    return tcp_parse_host_port(args, "TCP manager", manager_host, manager_port);
}

/*
 * Start a backend thread named name (cut short to QEMU_THREAD_NAME_LEN),
 * whose stop flag is *running.  The flag is set first, because the thread
 * exits as soon as it finds it clear.  If the thread can't be created (the
 * process is out of threads or memory), the flag is cleared again and false
 * is returned: every stop joins only when the flag is set, and a join of a
 * thread that was never created is undefined behavior.
 *
 * No stop may run between the set and the clear.  Callers hold the lock the
 * stop takes first, or start the thread before the stop can reach it.
 */
bool tcp_start_thread(QemuThread *thread, const char *name, void *(*fn)(void *),
                      void *arg, atomic_bool *running)
{
    atomic_store(running, true);
    if (qemu_thread_create(thread, name, fn, arg) != 0) {
        atomic_store(running, false);
        rdma_error_report("TCP: Failed to start thread %s", name);
        return false;
    }
    return true;
}

/*
 * Backend Lifecycle
 */

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
 * receive threads.  The broadcaster goes with them.  The receive threads
 * ask it for passes, which once it has stopped only adds to what is left
 * unsent.
 */
void tcp_stop_threads(TcpBackendPrivate *priv)
{
    atomic_store(&priv->stopping, true);
    tcp_stop_health_check_thread(priv);
    tcp_stop_broadcast_thread(priv);
    tcp_stop_accept_thread(priv);
    tcp_stop_peer_thread(priv);
    tcp_stop_recv_threads(priv);
}

/*
 * Stop every thread of the backend's, and free priv and everything in it:
 * nothing may be freed while a backend thread can still use it.  For a
 * backend tcp_init_with_interval() set up, or failed to.
 */
static void tcp_priv_free(TcpBackendPrivate *priv)
{
    tcp_stop_threads(priv);
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
    }
    qemu_cond_destroy(&priv->health_check_cond);
    qemu_mutex_destroy(&priv->health_check_lock);
    qemu_cond_destroy(&priv->peer_cond);
    qemu_mutex_destroy(&priv->peer_lock);
    tcp_free_pending_conns(priv);

    /* With the threads stopped, nothing else can reach priv */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_array_free(priv->peer_targets, TRUE);
    if (priv->mesh_nodes) {
        g_hash_table_destroy(priv->mesh_nodes);
    }
    g_hash_table_destroy(priv->connections);
    g_hash_table_destroy(priv->node_gids);
    g_hash_table_destroy(priv->gid_owner);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    if (priv->mesh_table_lock.initialized) {
        qemu_mutex_destroy(&priv->mesh_table_lock);
    }
    tcp_connection_unref(atomic_load(&priv->manager_conn));
    qemu_cond_destroy(&priv->registration_cond);
    qemu_mutex_destroy(&priv->registration_mutex);
    g_free(priv->manager_host);
    qemu_cond_destroy(&priv->ionic_recv_idle);
    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_table_lock);
    qemu_mutex_destroy(&priv->gid_table_lock);
    qemu_mutex_destroy(&priv->gid_advertise_lock);
    tcp_broadcaster_destroy(priv);
    g_free(priv);
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
    TcpMode mode;
    const char *args;
    int ret;

    /* NULL for a bare "--backend tcp", which names no role */
    if (!config) {
        rdma_error_report("TCP backend: No config. Must be "
                          "tcp:manager:... or tcp:worker:...");
        return -EINVAL;
    }

    args = tcp_config_role(config, &mode);
    if (!args) {
        rdma_error_report("TCP backend: Invalid config. Must be "
                          "tcp:manager:... or tcp:worker:...");
        return -EINVAL;
    }

    rdma_info_report("TCP backend: Initializing");

    priv = g_new0(TcpBackendPrivate, 1);

    /*
     * No other thread can reach priv until the first is started, below, so
     * the fields the backend's locks guard are set up without them.
     */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->connections = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, tcp_connection_destroy_notify);
    priv->pending_conns = g_ptr_array_new();
    priv->node_gids =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->gid_owner = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            tcp_gid_key_free, NULL);

    priv->next_pd_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100;
    atomic_init(&priv->next_seq, 1);
    atomic_init(&priv->tcp_stats.peer_connect_attempts, 0);
    atomic_init(&priv->tcp_stats.manager_reconnects, 0);

    priv->listen_fd = -1;
    atomic_init(&priv->accept_thread_running, false);
    atomic_init(&priv->stopping, false);
    /* tcp_env_int() only returns positive values */
    atomic_init(&priv->send_timeout_ms,
                (uint32_t)tcp_env_int("ERNIC_TCP_SEND_TIMEOUT_MS",
                                      (int)TCP_DEFAULT_SEND_TIMEOUT_MS));

    qemu_mutex_init(&priv->lock);
    qemu_cond_init(&priv->ionic_recv_idle);
    qemu_mutex_init(&priv->conn_table_lock);
    qemu_mutex_init(&priv->gid_table_lock);
    qemu_mutex_init(&priv->gid_advertise_lock);
    tcp_broadcaster_init(priv);

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
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    if (mode == TCP_MODE_MANAGER) {
        /* Manager mode: tcp:manager:<host>:<port> or ...:listen:<port> */
        char *manager_host = NULL;
        uint16_t manager_port = 0;
        bool listen_mode = false;

        priv->is_manager = true;
        atomic_store(&priv->local_node_id, 0); /* Manager is always node 0 */

        /* The manager's id is fixed, so it can claim its GIDs immediately;
         * a worker has to wait for REGISTER_RESP.  Still before any thread. */
        ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
        tcp_gid_claims_set(priv, 0, tcp_const_gids(priv->local_gids),
                           priv->num_local_gids);
        ROCM_ERNIC_WARN_THREAD_SAFETY_ON

        ret = parse_tcp_config_manager(args, &manager_host, &manager_port,
                                       &listen_mode);
        if (ret < 0) {
            goto error;
        }

        /*
         * The mesh nodes table, with the manager itself in it, under the
         * configured host, or "localhost" in listen mode, which names none
         */
        GHashTable *mesh_nodes =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                  tcp_mesh_node_info_destroy_notify);
        qemu_mutex_init(&priv->mesh_table_lock);
        ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
        priv->mesh_nodes = mesh_nodes;
        ROCM_ERNIC_WARN_THREAD_SAFETY_ON

        MeshNodeInfo *manager_node = tcp_mesh_node_info_new(
            0, listen_mode ? "localhost" : manager_host, manager_port);
        if (!manager_node) {
            g_free(manager_host);
            goto error;
        }
        g_hash_table_insert(mesh_nodes, GUINT_TO_POINTER(0), manager_node);

        rdma_info_report("TCP backend: Manager mode on port %u", manager_port);

        /* Start listening */
        priv->listen_port = manager_port;
        priv->listen_fd = tcp_listen_on_port(&priv->listen_port);
        if (priv->listen_fd < 0) {
            g_free(manager_host);
            goto error;
        }

        /* Start the accept, health check and broadcaster threads */
        if (!tcp_start_thread(&priv->accept_thread, "tcp-accept",
                              tcp_accept_thread, priv,
                              &priv->accept_thread_running) ||
            !tcp_start_thread(&priv->health_check_thread, "tcp-health",
                              tcp_manager_health_check_thread, priv,
                              &priv->health_check_running) ||
            !tcp_start_thread(&priv->broadcast_thread, "tcp-broadcast",
                              tcp_broadcast_thread, priv,
                              &priv->broadcast_running)) {
            g_free(manager_host);
            goto error;
        }

        g_free(manager_host);
        rdma_info_report("TCP backend: Manager initialized");

    } else {
        /* Worker mode: tcp:worker:<manager_host>:<manager_port> */
        char *manager_host = NULL;
        uint16_t manager_port = 0;

        priv->is_manager = false;

        ret = tcp_parse_host_port(args, "TCP worker", &manager_host,
                                  &manager_port);
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
    }

    backend_dev->backend_private = priv;
    return 0;

error:
    /* A worker that fails to register has already started the receive thread
     * for its manager connection, and one that fails to start its accept or
     * peer-connect thread has started others; tcp_priv_free() stops them. */
    tcp_priv_free(priv);
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
    tcp_priv_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int tcp_query_device(RdmaBackendDev *backend_dev,
                            struct ibv_device_attr *attr)
{
    (void)backend_dev;

    memset(attr, 0, sizeof(*attr));
    /*
     * The counts size rdma_rm's tables (rdma_rm_init()), and nothing else
     * reads them: what the guest sees comes from the LIF identity. Both are
     * the device's limits from ernic_limits.h; a table smaller than the
     * identity's count is the device's real ceiling. Leaving max_srq zero
     * once made a table of size zero, so the very first CREATE_SRQ failed
     * with
     *
     *   Table SRQ, failed to allocate, bitmap is full
     *
     * surfacing in the guest as ibv_create_srq() returning EINVAL on a
     * device that had just reported a non-zero max_srq.
     */
    attr->max_qp = ERNIC_MAX_QP;
    attr->max_qp_wr = 1024;
    attr->max_sge = ERNIC_MAX_SGE;
    attr->max_cq = ERNIC_MAX_CQ;
    attr->max_cqe = 8192;
    attr->max_mr = ERNIC_MAX_MR;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    /*
     * Note what max_srq does NOT do. No receive is served from here. Every
     * ionic receive, local and off the TCP mesh alike, is satisfied by
     * ionic_dp_deliver_recv() in ionic_datapath.c, reading either the
     * destination QP's own RQ ring or -- for a QP bound to one -- the SRQ's
     * ring, straight out of guest memory. This backend has no receive queues.
     *
     * So for ionic this value only sizes the rdma_rm table the adminq
     * handler allocates out of. The datapath half it used to disclaim is
     * done: ionic_datapath.c has an SRQ qtype, consumes the ring, and
     * tests/test_srq_transfer.c moves a payload across one.
     */
    attr->max_srq = ERNIC_MAX_SRQ;
    attr->max_srq_wr = 65535;
    attr->max_srq_sge = ERNIC_MAX_SRQ_SGE;
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

    (void)pd;

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
    (void)pd;
    (void)addr;
    (void)guest_start;
    (void)access;

    mr->ibmr = NULL;

    rdma_info_report("TCP: Created MR len=%zu", length);
    return 0;
}

static void tcp_destroy_mr(RdmaBackendMR *mr)
{
    (void)mr;

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

    (void)pd;
    (void)rcq;
    (void)srq;
    (void)max_send_wr;
    (void)max_recv_wr;
    (void)max_send_sge;
    (void)max_recv_sge;

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

    (void)dev_res;

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
    (void)qp_type;
    (void)qkey;

    if (!get_private(backend_dev)) {
        return -EINVAL;
    }

    rdma_info_report("TCP: QP %u -> INIT", (uint32_t)(uintptr_t)qp->ibqp);
    return 0;
}

/*
 * Refuse a connected QP whose destination GID no mesh node owns.  Nothing is
 * recorded: the ionic data path resolves the destination for itself, through
 * tcp_mesh_node_from_gid().
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

    (void)sgid_idx;
    (void)rq_psn;
    (void)qkey;
    (void)qkey_set;

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

/*
 * GID Management
 */

static int tcp_get_backend_gid_index(RdmaBackendDev *backend_dev, int sgid_idx)
{
    (void)backend_dev;

    return sgid_idx;
}

/*
 * SRQ Operations
 */

static int tcp_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                          uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit)
{
    TcpSRQ *tsrq = g_new0(TcpSRQ, 1);

    (void)pd;

    tsrq->max_wr = max_wr;
    tsrq->max_sge = max_sge;
    qemu_mutex_init(&tsrq->lock);
    /* No other thread can reach tsrq yet */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    tsrq->srq_limit = srq_limit;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

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
    qemu_mutex_lock(&tsrq->lock);
    attr->srq_limit = tsrq->srq_limit;
    qemu_mutex_unlock(&tsrq->lock);
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

static uint32_t tcp_mesh_local_node(RdmaBackendDev *backend_dev)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv)
        return UINT32_MAX;
    return tcp_local_node(priv);
}

static uint32_t tcp_mesh_node_from_gid(RdmaBackendDev *backend_dev,
                                       const union ibv_gid *dgid)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv)
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
 */
static int tcp_mesh_claim_gid(RdmaBackendDev *backend_dev,
                              const union ibv_gid *gid, bool add)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv)
        return 0;

    return tcp_claim_gid(priv, gid, add);
}

/*
 * Returns once no receive thread is still calling the callback this
 * replaces, so that a caller clearing it may free what it uses.  Never call
 * it from the callback, which would wait for itself.
 */
static void tcp_mesh_set_recv_cb(RdmaBackendDev *backend_dev,
                                 ernic_mesh_recv_fn fn, void *opaque)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv)
        return;

    qemu_mutex_lock(&priv->lock);
    priv->ionic_recv_fn = fn;
    priv->ionic_recv_opaque = opaque;
    /* A call that began before this has the old callback; one that begins
     * now gets the new one.  The callback only queues the message or
     * refuses it, so the wait is short. */
    while (priv->ionic_recv_calls > 0)
        qemu_cond_wait(&priv->ionic_recv_idle, &priv->lock);
    qemu_mutex_unlock(&priv->lock);
}

/*
 * TCP_MSG_IONIC: hand a message addressed to this node to the ionic data
 * path, which takes the payload with it.  On the manager, a message
 * addressed to another node is relayed to it (tcp_manager_relay_ionic()).
 *
 * The data path refuses a message while its queue is full, and this thread
 * offers it again every TCP_IONIC_RETRY_US, reading nothing more from the
 * peer meanwhile.  That is the mesh's flow control: the socket fills, then
 * the peer's, and the peer's send queue stops taking the guest's requests.
 * The data path empties its queue on every pass, so the wait is short; see
 * dp_mesh_recv().  A stopped thread gives the message up.  The callback is
 * looked up afresh each time, so one the data path has unregistered meanwhile
 * isn't called, and each call is counted in ionic_recv_calls, so that the
 * data path can't be freed while one is under way.
 */
void tcp_recv_ionic(const TcpConnection *conn, TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;
    ernic_mesh_recv_fn fn;
    void *fn_opaque;

    if (!priv || !msg->payload || msg->hdr.msg_len == 0)
        return;

    if (msg->hdr.dst_node_id != tcp_local_node(priv)) {
        /* Worker-to-worker: relay to the addressed node. */
        if (priv->is_manager)
            tcp_manager_relay_ionic(priv, msg);
        return;
    }

    for (;;) {
        bool taken;

        qemu_mutex_lock(&priv->lock);
        fn = priv->ionic_recv_fn;
        fn_opaque = priv->ionic_recv_opaque;
        if (fn)
            priv->ionic_recv_calls++;
        qemu_mutex_unlock(&priv->lock);

        if (!fn) {
            rdma_error_report("TCP: ionic message from node %u with no data "
                              "path registered",
                              msg->hdr.src_node_id);
            return;
        }
        taken =
            fn(fn_opaque, msg->hdr.src_node_id, msg->payload, msg->hdr.msg_len);

        qemu_mutex_lock(&priv->lock);
        if (--priv->ionic_recv_calls == 0)
            qemu_cond_broadcast(&priv->ionic_recv_idle);
        qemu_mutex_unlock(&priv->lock);

        if (taken) {
            /* fn owns the payload now, and frees it */
            msg->payload = NULL;
            return;
        }
        /* The receive thread frees the payload */
        if (!atomic_load(&conn->recv_thread_running) ||
            atomic_load(&priv->stopping))
            return;
        ernic_sleep_us(TCP_IONIC_RETRY_US);
    }
}

/*
 * Whether a request of len bytes to dst_node would be queued now, rather
 * than refused with -EAGAIN, so the data path can find out before it gathers
 * the payload.  Only the main thread queues the data path's messages, so the
 * answer holds until it queues another, unless the route changes meanwhile.
 * True when there is no connection to queue on: the send then fails, which
 * the caller has to hear about.
 */
static bool tcp_mesh_has_room(RdmaBackendDev *backend_dev, uint32_t dst_node,
                              size_t len)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *conn;
    bool room;

    if (!priv)
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
 * to tcp_mesh_sendv(), matches tag in the bits mask selects, so none of them
 * reaches its peer: those of a QP that is flushing, or one request that has
 * timed out, whose work requests the data path completes now.  On a NIC, a
 * flushed work request isn't transmitted; one queued here hasn't been
 * either.
 *
 * Only what no send thread has started to write: the message being written,
 * and what is already in the socket or on its way, still arrive, as frames
 * already on the wire do.  Replies are left alone.  A connection that has
 * left the table, or manager_conn, had its queue closed as it left
 * (tcp_conn_unpublished()), so nothing more goes out on it to miss.  Called
 * on the device's main thread, the only one that queues the data path's
 * messages, so nothing that matches is queued meanwhile.
 */
static void tcp_mesh_cancel(RdmaBackendDev *backend_dev, uint64_t tag,
                            uint64_t mask)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *manager;
    GPtrArray *conns;

    if (!priv)
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
 * tag is the caller's name for a request, which tcp_mesh_cancel() matches;
 * a reply's is ignored, since a reply is never taken back.
 *
 * Returns 0 once queued, which doesn't mean the peer will get it: a message
 * still queued when the connection is shut down is lost, and the data path's
 * own timeout for the answer covers that.  Otherwise a negative errno:
 * -EAGAIN for a request (!reply) the queue has no room for yet, which the
 * caller tries again later; -ENOTCONN or -EPIPE when there is no connection
 * to send on; or what tcp_conn_enqueue() returns.
 */
static int tcp_mesh_sendv(RdmaBackendDev *backend_dev, uint32_t dst_node,
                          const void *hdr, size_t hdr_len, const void *body,
                          size_t body_len, uint64_t tag, bool reply)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpConnection *conn;
    uint32_t local;
    int rc;

    /* Each length checked on its own before the sum, which could wrap */
    if (!priv || !hdr || hdr_len == 0 || hdr_len > TCP_MAX_PAYLOAD_LEN ||
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

static const RdmaMeshOps tcp_mesh_ops = {
    .local_node = tcp_mesh_local_node,
    .node_from_gid = tcp_mesh_node_from_gid,
    .claim_gid = tcp_mesh_claim_gid,
    .sendv = tcp_mesh_sendv,
    .has_room = tcp_mesh_has_room,
    .cancel = tcp_mesh_cancel,
    .set_recv_cb = tcp_mesh_set_recv_cb,
};

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
    /*
     * No query_qp: the backend keeps no QP state, so the resource manager
     * answers from what it recorded. (A stub here once reported every QP as
     * RTS.)
     */

    .get_backend_gid_index = tcp_get_backend_gid_index,

    .create_srq = tcp_create_srq,
    .destroy_srq = tcp_destroy_srq,
    .query_srq = tcp_query_srq,
    .modify_srq = tcp_modify_srq,

    .mesh = &tcp_mesh_ops,
};
