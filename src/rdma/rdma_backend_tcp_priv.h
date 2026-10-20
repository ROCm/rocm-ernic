/*
 * rdma_backend_tcp_priv.h - TCP backend internals, for its unit tests
 *
 * The mesh wire protocol, the backend's private state, and the functions its
 * unit tests drive directly. Only rdma_backend_tcp.c and its unit tests
 * include this; the rest of the device reaches the backend through
 * rdma_backend_ops_tcp and the tcp_backend_*() functions in rdma_backend.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef RDMA_BACKEND_TCP_PRIV_H
#define RDMA_BACKEND_TCP_PRIV_H

#include <glib.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "qemu/thread.h"
#include "rdma_backend.h"
#include "rocm_ernic_compat.h"

/*
 * TCP Backend Protocol (Multi-Node Extension)
 *
 * Messages are sent over TCP with a fixed header:
 *   uint32_t magic;        // Protocol magic (0x52444D41 = "RDMA")
 *   uint32_t msg_type;     // Message type
 *   uint32_t msg_len;      // Payload length
 *   uint32_t seq;          // Sequence number
 *   uint32_t src_node_id;  // Source node ID (NEW)
 *   uint32_t dst_node_id;  // Destination node ID (NEW)
 *   uint32_t src_qpn;      // Source QP number (NEW)
 *   uint32_t dst_qpn;      // Destination QP number (NEW)
 *   uint8_t  payload[];    // Variable length payload
 */

#define TCP_PROTOCOL_MAGIC 0x52444D41 /* "RDMA" */
/* v4: TCP_MSG_NODE_GIDS, and registration carries a version. */
#define TCP_PROTOCOL_VERSION 4u
#define TCP_MAX_PAYLOAD_LEN  (16u << 20) /* 16 MiB */
/* How long a peer that connects to a worker has to send its whole handshake */
#define TCP_HANDSHAKE_TIMEOUT_MS 5000u
/*
 * How long a peer that connects to a manager has to register.  A worker
 * registers as soon as it has connected, and gives up waiting for the answer
 * after this long itself.
 */
#define TCP_REGISTER_TIMEOUT_MS 5000u
/*
 * How many connections a manager keeps waiting to register at once.  A worker
 * registers within milliseconds of connecting, so only a mesh's worth of
 * workers starting together comes near it.  A connection past it is closed.
 */
#define TCP_MAX_PENDING_CONNS 64u

/* ionic sizes its own wire payload against this; keep the two in step. */
_Static_assert(IONIC_MESH_MAX_MSG == TCP_MAX_PAYLOAD_LEN,
               "IONIC_MESH_MAX_MSG must match TCP_MAX_PAYLOAD_LEN");

typedef enum {
    TCP_MSG_HANDSHAKE = 1,
    TCP_MSG_HANDSHAKE_RESP,
    /*
     * Retired: the PVRDMA device's sends, receives and their completions,
     * which went with that device, and QP state and connection info, which
     * nothing ever sent.  The ionic data path carries its traffic in
     * TCP_MSG_IONIC.  The ordinals stay reserved, as for 18 to 20 below.
     */
    TCP_MSG_RESERVED_3,
    TCP_MSG_RESERVED_4,
    TCP_MSG_RESERVED_5,
    TCP_MSG_RESERVED_6,
    TCP_MSG_RESERVED_7,
    TCP_MSG_RESERVED_8,
    TCP_MSG_RESERVED_9,
    TCP_MSG_RESERVED_10,
    TCP_MSG_RESERVED_11,
    /* Manager/Worker mesh discovery messages */
    TCP_MSG_REGISTER_NODE,
    TCP_MSG_REGISTER_RESP,
    TCP_MSG_MESH_TOPOLOGY,
    TCP_MSG_HEARTBEAT,
    TCP_MSG_HEARTBEAT_RESP,
    TCP_MSG_NODE_STATUS,
    /*
     * Retired: DHCP request/response and raw Ethernet frame relay.  Guest
     * TCP/IP rides the host TAP/bridge, so none of the three was ever sent.
     * The ordinals stay reserved -- renumbering what follows would break the
     * wire protocol against an unupgraded peer.
     */
    TCP_MSG_RESERVED_18,
    TCP_MSG_RESERVED_19,
    TCP_MSG_RESERVED_20,
    /* Retired: the PVRDMA device's RDMA writes and reads, as for 3 to 11 */
    TCP_MSG_RESERVED_21,
    TCP_MSG_RESERVED_22,
    TCP_MSG_RESERVED_23,
    /*
     * Opaque node-to-node payload for the ionic data path.  The mesh only
     * routes it; ionic_datapath.c owns the contents, because ionic resolves
     * rkeys against its own guest-physical MR table rather than through
     * rdma_rm.
     */
    TCP_MSG_IONIC,
    /*
     * A node's own guest RoCE GIDs, so peers can map a destination GID back
     * to a node id.  Appended last: every ordinal above is already on the
     * wire and must not move.
     */
    TCP_MSG_NODE_GIDS,
} TcpMsgType;

/* The ordinals are on the wire; see the reserved ones above. */
_Static_assert(TCP_MSG_REGISTER_NODE == 12 && TCP_MSG_IONIC == 24 &&
                   TCP_MSG_NODE_GIDS == 25,
               "a TcpMsgType ordinal moved");

typedef struct {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t seq;
    uint32_t src_node_id;
    uint32_t dst_node_id;
    uint32_t src_qpn;
    uint32_t dst_qpn;
} __attribute__((packed)) TcpMsgHeader;

typedef struct {
    uint32_t node_id;
    uint32_t version;
} __attribute__((packed)) TcpHandshakePayload;

/*
 * Manager/Worker mesh discovery payloads.
 *
 * Both carry a protocol version appended in v4.  Registration had no
 * version check at all before that: only the peer-to-peer handshake was
 * gated, and a manager never exchanges one -- it spawns a receive thread
 * and waits for REGISTER_NODE.  A node running the pre-v4 GID heuristic
 * could therefore join a v4 mesh, never advertise its GIDs, and keep
 * routing every peer to one default node on its own side.
 *
 * The field is appended rather than inserted so a pre-v4 payload is an
 * exact prefix, which is what lets the lengths below tell "older peer"
 * from "corrupt message" and report the former precisely.
 */
typedef struct {
    char hostname[256];
    uint16_t port;
    uint32_t requested_node_id; /* 0xFFFFFFFF for auto-assign */
    uint32_t version;           /* appended in v4 */
} __attribute__((packed)) TcpRegisterNodePayload;

typedef struct {
    uint32_t assigned_node_id;
    uint32_t num_nodes;
    uint32_t result;  /* int32_t on the wire: 0 = success, negative = error */
    uint32_t version; /* appended in v4 */
} __attribute__((packed)) TcpRegisterRespPayload;

/* Sizes before the version field, i.e. what a pre-v4 peer sends. */
#define TCP_REGISTER_NODE_LEN_V3 262u
#define TCP_REGISTER_RESP_LEN_V3 12u

_Static_assert(sizeof(TcpRegisterNodePayload) == TCP_REGISTER_NODE_LEN_V3 + 4,
               "the version field must be appended, not inserted");
_Static_assert(sizeof(TcpRegisterRespPayload) == TCP_REGISTER_RESP_LEN_V3 + 4,
               "the version field must be appended, not inserted");

typedef struct {
    uint32_t node_id;
    char hostname[256];
    uint16_t port;
    uint8_t is_alive;
    uint8_t reserved[3];
} __attribute__((packed)) TcpMeshNodeInfo;

typedef struct {
    uint32_t num_nodes;
    TcpMeshNodeInfo nodes[64]; /* Max 64 nodes */
} __attribute__((packed)) TcpMeshTopologyPayload;

/*
 * A node's own guest RoCE GIDs, advertised so peers can map a destination
 * GID back to a node id.  A guest typically has two -- an IPv6 link-local
 * and whatever global address it is configured with -- so the cap leaves
 * room without making the message worth fragmenting.
 */
#define TCP_MAX_NODE_GIDS 8

typedef struct {
    uint32_t node_id;
    uint32_t num_gids;
    uint8_t gids[TCP_MAX_NODE_GIDS][16];
} __attribute__((packed)) TcpNodeGidsPayload;

_Static_assert(sizeof(TcpNodeGidsPayload) == 8 + TCP_MAX_NODE_GIDS * 16,
               "TcpNodeGidsPayload must be packed with no padding");

/* A node's advertised set, as held in memory rather than on the wire. */
typedef struct {
    uint32_t num_gids;
    uint8_t gids[TCP_MAX_NODE_GIDS][16];
} TcpNodeGids;

/*
 * Receive buffer pool -- avoids per-message g_malloc
 * on the hot path.
 */
#define TCP_BUFPOOL_SLOTS 64
#define TCP_BUFPOOL_BUFSZ (64u * 1024)

typedef struct {
    void *bufs[TCP_BUFPOOL_SLOTS];
    int head;
    int count;
    QemuMutex lock;
} TcpBufPool;

typedef struct TcpBackendPrivate TcpBackendPrivate;

/*
 * Per-connection state.
 *
 * A connection is reference counted.  Whatever publishes it holds a
 * reference: its slot in connections or pending_conns, or priv->manager_conn.
 * So does anything that goes on using it after dropping the lock it was found
 * under: a caller of tcp_connection_lookup() or tcp_route_to_node(), or a
 * health-check pass.  The last tcp_connection_unref() frees it.
 *
 * The receive thread holds no reference of its own.  Instead, a connection
 * is retired, which joins the thread, before the reference that published it
 * is dropped, so the connection outlives its thread.  To unpublish one, take
 * it out of its table under the table's lock, which hands the table's
 * reference to the caller, then retire it and drop that reference with no
 * lock held.  Only one thread can take it out, so only one retires it, which
 * tcp_retire_conn() relies on.  Nothing may remove or replace an entry in
 * connections in a way that runs the table's destroy notifier, which would
 * drop the reference without retiring the connection; only teardown destroys
 * the table, once every receive thread has been joined.
 */
typedef struct TcpConnection TcpConnection;

struct TcpConnection {
    uint32_t node_id;
    int sockfd;
    char *remote_host;
    uint16_t remote_port;
    QemuThread recv_thread;
    /*
     * References held; see above.  Plain, sequentially consistent, atomic
     * operations: the decrement that reaches zero must see every other
     * holder's use of the connection, which needs at least acquire-release,
     * and nothing here is hot enough to argue for exactly that.
     */
    _Atomic uint32_t refcount;
    /*
     * Whether the connection can be sent on.  tcp_retire_conn() clears it,
     * under lock, with sockfd, and the receive thread clears it as it
     * returns, but senders read it without the lock to pick a connection, so
     * it is atomic.  It is only a hint: a connection can be retired just
     * after it was picked.  sockfd is the authority, read only under lock,
     * and a sender checks it there, in the same critical section as the send.
     */
    _Atomic bool is_connected;
    /*
     * Stop flag for recv_thread, polled by tcp_recv_thread_per_conn() while
     * a teardown path clears it from another thread. Atomic because a plain
     * bool read and written concurrently is a data race: the compiler is
     * free to hoist the load out of the poll loop, in which case the thread
     * never sees the stop and the matching join hangs.
     *
     * The plain assignments below are sequentially consistent, which is
     * stronger than this flag needs -- it guards no data of its own, and the
     * qemu_thread_create()/qemu_thread_join() pair bracketing the thread's
     * life supplies the actual memory barriers. At a 100ms poll interval the
     * difference is not worth spelling out explicit relaxed accesses at
     * every site.
     */
    _Atomic bool recv_thread_running;
    /*
     * Set by recv_thread as it returns, whatever the reason: the peer hung
     * up, a read failed, the peer was refused, or the thread was stopped.
     * Tells the threads that retire connections that this one is done with
     * (see tcp_conn_dead()); the thread still has to be joined.
     */
    _Atomic bool recv_thread_exited;
    /*
     * When the manager accepted the connection, on tcp_monotonic_ms()'s
     * clock, for the registration timeout.  Set before the connection goes
     * on pending_conns, and read under conn_table_lock.
     */
    uint64_t accepted_ms;
    QemuMutex lock;
    TcpBackendPrivate *priv;
};

/* Multi-node configuration modes */
typedef enum {
    TCP_MODE_MANAGER,
    TCP_MODE_WORKER,
} TcpMode;

/*
 * Mesh node information (for manager).  A node stays in mesh_nodes from
 * registration until teardown, so a thread that found it under
 * mesh_table_lock can keep using the pointer after dropping the lock.
 * node_id, hostname and port are set before the node is published and never
 * change; is_alive and last_heartbeat are protected by mesh_table_lock.  The
 * node's connection is its entry in connections.
 */
typedef struct {
    uint32_t node_id;
    char *hostname;
    uint16_t port;
    bool is_alive;
    time_t last_heartbeat;
    /*
     * Nonzero while the node is due a reconnect that a pass ran out of time
     * for: the value of reconnect_deferrals when it was deferred, so the
     * nodes deferred longest go first.  Used only by the health-check
     * thread, so it needs no lock.
     */
    uint64_t reconnect_deferred;
} MeshNodeInfo;

struct TcpBackendPrivate {
    /* Node identity */
    /*
     * This node's ID.  The manager's is 0 from the start.  A worker's is 0,
     * which no worker is given, until the manager answers its registration:
     * the receive thread for manager_conn stores it, once, before tcp_init()
     * starts the threads that introduce this node to its peers (see
     * tcp_worker_registration_done()).  Atomic as a precaution: no thread
     * reads it while it can still change, but that rests on the order
     * tcp_init() starts threads in.
     */
    _Atomic uint32_t local_node_id;
    TcpMode mode; /* Operation mode */

    /*
     * Multi-connection support: node_id -> TcpConnection*, each entry
     * holding a reference (see TcpConnection for how entries are removed).
     */
    GHashTable *connections;
    /*
     * Connections the manager has accepted that haven't registered, or were
     * refused, at most TCP_MAX_PENDING_CONNS of them, each holding a
     * reference.  Registration moves a connection, and its reference, from
     * here to connections, and only a connection still here can register.
     * tcp_reap_pending_conns() releases the ones that are done with, and
     * teardown stops the receive threads of the rest and releases them.
     */
    GPtrArray *pending_conns;
    QemuMutex conn_table_lock; /* Protects connections and pending_conns */

    /*
     * What each node says it owns: node_id -> TcpNodeGids.  This is the
     * authoritative record, kept per node rather than per GID so that a
     * claim is never lost by another node making the same one.
     */
    GHashTable *node_gids;

    /*
     * Derived from node_gids, rebuilt on every change: the full 16 bytes of
     * a GID -> the single node that claims it, or UINT32_MAX when none or
     * more than one does.  Lookups are on the RTR and UD send paths, so
     * they index rather than scan.
     *
     * A destination GID absent here, or contested, names no node this mesh
     * can route to, and the caller must fail rather than pick one.  Used in
     * both modes, so the lock is its own -- mesh_table_lock is only
     * initialised on the manager.
     */
    GHashTable *gid_owner;
    QemuMutex gid_table_lock;

    /*
     * Serialises a whole broadcast -- snapshot and fan-out together.
     * gid_table_lock cannot do it: the sends take connection locks that the
     * data path holds across multi-megabyte writes, so it has to be dropped
     * before sending.  Two manager receive threads would then interleave,
     * and an older snapshot could land after a newer one, leaving workers
     * with claims the manager has already replaced.
     */
    QemuMutex gid_broadcast_lock;

    /* This node's own GIDs, from ERNIC_TCP_GUEST_GIDS. */
    uint8_t local_gids[TCP_MAX_NODE_GIDS][16];
    uint32_t num_local_gids;

    /* Listen socket (for accepting connections) */
    int listen_fd;
    uint16_t listen_port;
    bool is_listening;
    QemuThread accept_thread;
    atomic_bool accept_thread_running;

    /*
     * Set at the start of teardown and never cleared.  From then on a send
     * that would have to wait for the peer fails instead (see
     * tcp_send_message2()), so a thread blocked sending to a peer that has
     * stopped reading lets go of the locks it holds, and teardown's joins
     * don't wait on that peer.
     */
    atomic_bool stopping;

    /* Manager-specific fields */
    bool is_manager;
    GHashTable *mesh_nodes; /* node_id -> MeshNodeInfo* */
    /* Protects mesh_nodes; taken before conn_table_lock when both are held */
    QemuMutex mesh_table_lock;
    /*
     * Serialises a whole topology broadcast, snapshot and fan-out together,
     * as gid_broadcast_lock does for the GID table.  The health check, a
     * registration, and a node coming back to life each broadcast from their
     * own thread, and without it an older snapshot could be sent after a
     * newer one: a node's death after its revival, say, leaving the workers
     * believing it dead.  Taken with no other lock held.
     */
    QemuMutex topology_broadcast_lock;
    QemuThread health_check_thread;
    /*
     * Cleared under health_check_lock, with health_check_cond signalled, to
     * stop the thread; see tcp_stop_health_check_thread().  The thread also
     * reads it without the lock at the top of each pass.
     */
    atomic_bool health_check_running;
    QemuMutex health_check_lock;
    QemuCond health_check_cond;
    uint32_t health_check_interval_sec; /* Default: 5 seconds */
    uint32_t next_available_node_id;    /* For auto-assigning node IDs */
    /* Reconnects deferred so far; health-check thread only (see MeshNodeInfo)
     */
    uint64_t reconnect_deferrals;

    /* Worker-specific fields */
    char *manager_host;
    uint16_t manager_port;
    /*
     * Connection to the manager, holding a reference.  Set by tcp_init()
     * before it starts any thread but this connection's own receive thread,
     * and released only at teardown, so every thread can read it without a
     * lock.
     */
    TcpConnection *manager_conn;
    /*
     * Set when the manager has answered this worker's registration, whether
     * it gave the worker an id or not; see tcp_worker_registration_done().
     * Protected by registration_mutex, and registration_cond is signalled
     * when it is set.
     */
    bool registration_complete;
    QemuCond registration_cond;
    QemuMutex registration_mutex;

    /*
     * The peers this worker is to connect to, as TcpPeerTarget, from the
     * latest topology update.  A receive thread replaces them and sets
     * peer_targets_changed; the peer-connect thread connects to them.  Both
     * are protected by peer_lock, which is taken before conn_table_lock when
     * both are held: the peer-connect thread holds both to compare the
     * connections table with the targets and change it.
     */
    GArray *peer_targets;
    bool peer_targets_changed;
    QemuMutex peer_lock;
    QemuCond peer_cond; /* signalled on an update, and to stop the thread */
    QemuThread peer_thread;
    /*
     * Cleared under peer_lock, with peer_cond signalled, to stop the thread;
     * see tcp_stop_peer_thread().  The thread also reads it without the lock
     * between connects.
     */
    atomic_bool peer_thread_running;

    /* Handle generators, protected by lock */
    uint32_t next_pd_handle;
    uint32_t next_cq_handle;
    uint32_t next_qpn;

    /* Sequence number for protocol */
    _Atomic uint32_t next_seq;

    /* Receive buffer pool */
    TcpBufPool recv_pool;

    /* Sink for TCP_MSG_IONIC, registered by the ionic data path.  Called on
     * a receive thread, so the callee must hand off to its own thread.  Both
     * are protected by lock. */
    tcp_ionic_recv_fn ionic_recv_fn;
    void *ionic_recv_opaque;

    /* TCP-layer counters */
    struct {
        _Atomic uint64_t reconnect_attempts;
        _Atomic uint64_t reconnect_successes;
        _Atomic uint64_t peer_connect_attempts; /* a worker's, to its peers */
    } tcp_stats;

    QemuMutex lock;
};

/* This node's ID; see local_node_id for why it is atomic. */
static inline uint32_t tcp_local_node(TcpBackendPrivate *priv)
{
    return atomic_load(&priv->local_node_id);
}

/* 8 groups of 4 hex digits, 7 separators, NUL. */
#define TCP_GID_STR_LEN 40

/* A peer a worker is to connect to, picked from a MESH_TOPOLOGY update. */
typedef struct {
    uint32_t node_id;
    char host[256];
    uint16_t port;
} TcpPeerTarget;

/*
 * One node's part in a health-check pass.  Decided under mesh_table_lock by
 * tcp_health_check_scan() and carried out without it, which is safe because
 * node outlives the pass (see MeshNodeInfo), and conn, the node's connection
 * when the pass began, holds a reference that tcp_health_check_release()
 * drops at the end of the pass.
 */
typedef struct {
    MeshNodeInfo *node;
    TcpConnection *conn;
    bool reconnect;    /* due a reconnect attempt */
    bool disconnected; /* conn's receive thread has exited: retire conn */
} TcpHealthCheckItem;

/*
 * The functions below are documented at their definitions in
 * rdma_backend_tcp.c. tcp_init(), tcp_fini() and the other entry points the
 * device uses are reached through rdma_backend_ops_tcp instead.
 */

/* Receive buffer pool */
void tcp_bufpool_init(TcpBufPool *pool);
void tcp_bufpool_destroy(TcpBufPool *pool);

/* GIDs and the GID -> node table */
char *tcp_gid_str(const uint8_t gid[16], char buf[TCP_GID_STR_LEN]);
uint32_t tcp_parse_guest_gids(uint8_t out[TCP_MAX_NODE_GIDS][16]);
void tcp_gid_key_free(gpointer key);
void tcp_gid_claims_set(TcpBackendPrivate *priv, uint32_t node_id,
                        const uint8_t (*gids)[16], uint32_t num_gids);
void tcp_fill_node_gids(TcpNodeGidsPayload *out, uint32_t node_id,
                        const uint8_t (*gids)[16], uint32_t num_gids);
uint32_t tcp_resolve_node_from_gid(TcpBackendPrivate *priv,
                                   const union ibv_gid *dgid);

/* Connections; see TcpConnection for the reference counting rules */
TcpConnection *tcp_connection_new(uint32_t node_id, const char *host,
                                  uint16_t port);
TcpConnection *tcp_connection_ref(TcpConnection *conn);
void tcp_connection_unref(TcpConnection *conn);
void tcp_connection_destroy_notify(gpointer data);
bool tcp_conn_dead(TcpConnection *conn);
TcpConnection *tcp_connection_lookup(TcpBackendPrivate *priv, uint32_t node_id);
bool tcp_has_live_connection(TcpBackendPrivate *priv, uint32_t node_id);
TcpConnection *tcp_route_to_node(TcpBackendPrivate *priv, uint32_t node_id);
bool tcp_conn_table_take(TcpBackendPrivate *priv, uint32_t node_id,
                         TcpConnection *conn);
void tcp_retire_conn(TcpConnection *conn);

/* The manager's table of mesh nodes */
MeshNodeInfo *tcp_mesh_node_info_new(uint32_t node_id, const char *hostname,
                                     uint16_t port);
void tcp_mesh_node_info_free(MeshNodeInfo *node);
void tcp_mesh_node_info_destroy_notify(gpointer data);
void tcp_broadcast_mesh_topology(TcpBackendPrivate *priv);

/* Sockets and messages */
uint64_t tcp_monotonic_ms(void);
int tcp_connect_any(const struct addrinfo *addrs, const atomic_bool *running,
                    uint64_t deadline_ms);
int tcp_connect_to_remote(const char *host, uint16_t port,
                          const atomic_bool *running, uint64_t deadline_ms);
int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload,
                     TcpBufPool *pool, atomic_bool *running,
                     uint64_t deadline_ms);

/* Threads, and what they do */
bool tcp_start_thread(QemuThread *thread, const char *name, void *(*fn)(void *),
                      void *arg, atomic_bool *running);
void *tcp_accept_thread(void *opaque);
void *tcp_recv_thread_per_conn(void *opaque);
void *tcp_worker_peer_thread(void *opaque);
bool tcp_worker_publish_peer(TcpBackendPrivate *priv,
                             const TcpPeerTarget *target, TcpConnection *conn);
void *tcp_manager_health_check_thread(void *opaque);
bool tcp_health_check_reconnect(TcpBackendPrivate *priv,
                                const TcpHealthCheckItem *item,
                                uint64_t deadline_ms);
bool tcp_health_check_reconnects(TcpBackendPrivate *priv, GArray *items,
                                 uint64_t deadline_ms);

/* Teardown */
void tcp_stop_accept_thread(TcpBackendPrivate *priv);
void tcp_stop_health_check_thread(TcpBackendPrivate *priv);
void tcp_stop_peer_thread(TcpBackendPrivate *priv);
void tcp_stop_recv_threads(TcpBackendPrivate *priv);
void tcp_free_pending_conns(TcpBackendPrivate *priv);
void tcp_stop_threads(TcpBackendPrivate *priv);

#endif /* RDMA_BACKEND_TCP_PRIV_H */
