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
#include <sys/socket.h>
#include <sys/types.h>

#include "qemu/thread.h"
#include "rdma_backend_defs.h"
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
/*
 * How long a send may go without a byte leaving before it gives up and shuts
 * the connection down, unless ERNIC_TCP_SEND_TIMEOUT_MS says otherwise.  A
 * manager gives up in half the time (see tcp_send_timeout_ms()).
 */
#define TCP_DEFAULT_SEND_TIMEOUT_MS 10000u
/*
 * How many bytes of the data path's messages a connection queues for its
 * send thread.  A request that would take the queue past
 * TCP_SENDQ_REQUEST_BYTES is refused with -EAGAIN, and the data path tries it
 * again later; a reply, past TCP_SENDQ_REPLY_BYTES, is refused with -ENOBUFS.
 * Either fits one message of the largest size in an empty queue.
 */
#define TCP_SENDQ_REQUEST_BYTES (2u * TCP_MAX_PAYLOAD_LEN)
#define TCP_SENDQ_REPLY_BYTES   (4u * TCP_MAX_PAYLOAD_LEN)
/* So a limit less a message's length, at most TCP_MAX_PAYLOAD_LEN, can't wrap
 */
_Static_assert(TCP_SENDQ_REQUEST_BYTES >= TCP_MAX_PAYLOAD_LEN,
               "a request's queue limit must hold the largest message");
_Static_assert(TCP_SENDQ_REPLY_BYTES >= TCP_MAX_PAYLOAD_LEN,
               "a reply's queue limit must hold the largest message");

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
_Static_assert(sizeof(TcpMsgHeader) == 32,
               "TcpMsgHeader is on the wire and has changed size");

typedef struct {
    uint32_t node_id;
    uint32_t version;
} __attribute__((packed)) TcpHandshakePayload;
_Static_assert(sizeof(TcpHandshakePayload) == 8,
               "TcpHandshakePayload is on the wire and has changed size");

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
_Static_assert(sizeof(TcpMeshNodeInfo) == 266,
               "TcpMeshNodeInfo is on the wire and has changed size");

/* As many nodes as a topology broadcast can carry, and so as many as the
 * mesh can hold. */
#define TCP_MAX_MESH_NODES 64u

typedef struct {
    uint32_t num_nodes;
    TcpMeshNodeInfo nodes[TCP_MAX_MESH_NODES];
} __attribute__((packed)) TcpMeshTopologyPayload;
_Static_assert(sizeof(TcpMeshTopologyPayload) ==
                   4 + TCP_MAX_MESH_NODES * sizeof(TcpMeshNodeInfo),
               "TcpMeshTopologyPayload is on the wire and has changed size");

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
    /*
     * Whether the claimant is answering. A dead node's claims are kept but
     * take no part in resolution, so the node's addresses are unreachable
     * rather than contested while it is away, and come back with it
     * without it having to advertise them again. Protected by
     * gid_table_lock, like the rest of this structure, and deliberately
     * not read from mesh_nodes: that would mean taking mesh_table_lock
     * under gid_table_lock, the reverse of the order everything else
     * takes them in. On the manager it is a copy of the node's is_alive,
     * written with it by tcp_node_set_alive(); on a worker it is always
     * true, since the manager advertises a dead node as owning nothing.
     */
    bool alive;
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
 * A message waiting on a connection's send queue: a TCP_MSG_IONIC, or a
 * heartbeat or its answer, which has no payload (see tcp_send_heartbeat()).
 */
typedef struct {
    uint32_t msg_type; /* a TcpMsgType */
    uint32_t seq;
    uint32_t src_node;
    uint32_t dst_node;
    uint64_t tag; /* a TCP_MSG_IONIC's name, for tcp_sendq_cancel() */
    bool reply;   /* a TCP_MSG_IONIC that answers the peer's request */
    uint32_t len;
    uint8_t data[]; /* the data path's header and body, together */
} TcpQueuedMsg;

/*
 * Per-connection state.
 *
 * A connection is reference counted.  Whatever publishes it holds a
 * reference: its slot in connections or pending_conns, or priv->manager_conn.
 * So does a worker registering on a connection it has opened to the manager,
 * until the answer publishes it as manager_conn, or it retires it.
 * So does anything that goes on using it after dropping the lock it was found
 * under: a caller of tcp_connection_lookup() or tcp_route_to_node(), or a
 * health-check pass.  The last tcp_connection_unref() frees it.
 *
 * Neither its receive thread nor its send thread holds a reference of its
 * own.  Instead, a connection is retired, which joins both, before the
 * reference that published it is dropped, so the connection outlives its
 * threads.  To unpublish one, take
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
     * When the manager accepted the connection, on ernic_monotonic_ms()'s
     * clock, for the registration timeout.  Set before the connection goes
     * on pending_conns, and read under conn_table_lock.
     */
    uint64_t accepted_ms;
    /*
     * When a message last arrived on the connection, on ernic_monotonic_ms()'s
     * clock: set when the connection is made, and by its receive thread on
     * each message.  A worker reads its manager connection's to notice the
     * manager has gone silent (see tcp_worker_manager_pass()).
     */
    _Atomic uint64_t last_rx_ms;
    /*
     * Serialises writes to sockfd, and protects sockfd itself.  A writer
     * holds it for a whole message, which can mean waiting for the peer to
     * read, for up to tcp_send_timeout_ms() without progress.
     */
    QemuMutex lock;
    TcpBackendPrivate *priv;

    /*
     * The data path's messages, queued for send_thread, so the device's main
     * thread never waits for the peer (see tcp_backend_send_ionic_v()), and
     * heartbeats that couldn't be sent at once, at the head of the queue (see
     * tcp_send_heartbeat()).  Everything below is protected by sendq_lock,
     * which is taken with no other lock held, or under conn_table_lock, and
     * nothing is taken under it.  sendq_cond is signalled when a message is
     * queued and when the queue is closed.
     *
     * sendq_bytes counts each message's payload until it has been written.
     * sendq_closed is set, never cleared, when the connection leaves its
     * table or manager_conn (tcp_conn_unpublished()), when it is retired, or
     * when a send on it fails; nothing is queued after that, and what was
     * queued is dropped.  send_thread is started by the first message queued,
     * and
     * send_thread_started is set while there is a thread to join.
     */
    QemuMutex sendq_lock;
    QemuCond sendq_cond;
    GQueue sendq; /* of TcpQueuedMsg */
    size_t sendq_bytes;
    bool sendq_closed;
    bool send_thread_started;
    QemuThread send_thread;
};

/* Multi-node configuration modes */
typedef enum {
    TCP_MODE_MANAGER,
    TCP_MODE_WORKER,
} TcpMode;

/*
 * Mesh node information (for manager).  A node stays in mesh_nodes from
 * registration until teardown, or until a registration evicts it, dead, to
 * make room (tcp_evict_dead_node()), which frees it.  So a thread that found
 * it under mesh_table_lock must not keep the pointer after dropping the
 * lock; it keeps node_id, and looks the node up again.  node_id, hostname
 * and port are set before the node is published and never change; is_alive
 * and last_heartbeat_ms are protected by mesh_table_lock.  Once the node is
 * published, is_alive is written only by tcp_node_set_alive(), which keeps
 * the node's GID claims in step with it.  The node's connection is its entry
 * in connections.
 */
typedef struct {
    uint32_t node_id;
    char *hostname;
    uint16_t port;
    bool is_alive;
    /*
     * When the node last answered, on ernic_monotonic_ms()'s clock.  Read
     * the clock under the lock to compare with it, so no heartbeat recorded
     * meanwhile can be newer than the time it's compared with.
     */
    uint64_t last_heartbeat_ms;
} MeshNodeInfo;

struct TcpBackendPrivate {
    /* Node identity */
    /*
     * This node's ID.  The manager's is 0 from the start.  A worker's is 0,
     * which no worker is given, until the manager answers its first
     * registration: the receive thread for the registering connection stores
     * it, once, before tcp_init() starts the threads that introduce this node
     * to its peers.  A worker that registers again keeps it.  Atomic as a
     * precaution: no thread reads it while it can still change, but that
     * rests on the order tcp_init() starts threads in.
     */
    _Atomic uint32_t local_node_id;

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
     * gid_table_lock cannot do it: the sends take connection locks, which
     * a sender holds while it waits for a peer to read, so it has to be
     * dropped before sending.  Two manager receive threads would then
     * interleave, and an older snapshot could land after a newer one, leaving
     * workers with claims the manager has already replaced.
     */
    QemuMutex gid_broadcast_lock;

    /*
     * Serialises a whole advertisement of this node's own claims -- snapshot
     * and send together -- for the same reason gid_broadcast_lock serialises
     * a broadcast.  The snapshot is taken under lock, which has to be dropped
     * before sending, so without this an advertisement from the main thread
     * (a guest adding or removing an address) and one from a receive thread
     * (registration completing) could land out of order, leaving the manager
     * holding a claim set this node has already replaced.
     *
     * Taken before lock, before gid_broadcast_lock, and before a connection
     * lock; nothing takes it while holding any of them.
     */
    QemuMutex gid_advertise_lock;

    /*
     * This node's own GIDs, seeded from ERNIC_TCP_GUEST_GIDS and then changed
     * by the guest through tcp_claim_gid().  Both are protected by lock.
     */
    uint8_t local_gids[TCP_MAX_NODE_GIDS][16];
    uint32_t num_local_gids;

    /* Listen socket (for accepting connections) */
    int listen_fd;
    uint16_t listen_port;
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

    /*
     * How long a send may go without progress, from ERNIC_TCP_SEND_TIMEOUT_MS
     * or TCP_DEFAULT_SEND_TIMEOUT_MS; see tcp_send_timeout_ms().  Atomic
     * only so the tests can shorten it on a running backend; it guards no
     * other data, so relaxed accesses are enough.
     */
    _Atomic uint32_t send_timeout_ms;

    /* Manager-specific fields */
    bool is_manager;
    /* node_id -> MeshNodeInfo*; at most TCP_MAX_MESH_NODES of them */
    GHashTable *mesh_nodes;
    /*
     * Protects mesh_nodes; taken before conn_table_lock or gid_table_lock
     * when it and either of them are held
     */
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
    /*
     * Also a worker's: how often its peer-connect thread runs, and so how
     * soon it notices that the manager has gone silent.
     */
    uint32_t health_check_interval_sec; /* Default: 5 seconds */
    uint32_t next_available_node_id;    /* For auto-assigning node IDs */

    /* Worker-specific fields */
    char *manager_host;
    uint16_t manager_port;
    /*
     * The worker's connection to the manager, holding a reference, or NULL
     * while there is none.  The answer to a registration publishes the
     * connection it came on here (see tcp_worker_registration_done());
     * the peer-connect thread clears it when it gives up on the connection,
     * before it registers again; teardown releases it.  Changed only under
     * conn_table_lock.
     *
     * Read under conn_table_lock to take a reference to it, since a load
     * followed by a ref could race the last unref of a connection being
     * replaced.  A receive thread only compares its own connection with it,
     * which needs no lock: the load is atomic, and its own connection is
     * alive, so it can't be mistaken for a freed one at the same address.
     */
    TcpConnection *_Atomic manager_conn;
    /*
     * The registration under way, all protected by registration_mutex:
     * registering is the connection whose REGISTER_RESP is awaited, with no
     * reference of its own (the thread registering holds one), or NULL;
     * registration_complete is set once that answer has come, and
     * registration_accepted if it registered this node.  registration_cond
     * is signalled when the answer comes, and when the peer-connect thread
     * is stopped.  Taken before conn_table_lock, which the answer publishes
     * manager_conn under.
     *
     * registers_again, also under registration_mutex, is cleared for good
     * when the manager answers this worker's registering again with another
     * id.  Only a manager that predates registering again does that: it
     * gives a worker that asks for a taken id a new one, so each time the
     * worker asked, the mesh would gain a node.
     */
    TcpConnection *registering;
    bool registration_complete;
    bool registration_accepted;
    bool registers_again;
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
        _Atomic uint64_t peer_connect_attempts; /* a worker's, to its peers */
        /* A worker's, registering again with the manager */
        _Atomic uint64_t manager_reconnects;
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
 * tcp_health_check_scan() and carried out without it.  That is why it names
 * the node by id: the node can be evicted, and freed, before the pass is
 * done with it (see MeshNodeInfo).  conn, the node's connection when the
 * pass began, holds a reference that tcp_health_check_release() drops at the
 * end of the pass.
 */
typedef struct {
    uint32_t node_id;
    TcpConnection *conn;
    bool disconnected; /* conn's receive thread has exited: retire conn */
    bool silent;       /* the node stopped answering: retire conn */
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
uint32_t tcp_parse_guest_gids(const char *val,
                              uint8_t out[TCP_MAX_NODE_GIDS][16]);
void tcp_gid_key_free(gpointer key);
/*
 * Mark @node_id's claims live or dead. A node the manager has declared
 * dead keeps its claims but resolves to nothing; see TcpNodeGids.alive.
 * Returns whether they changed, which they don't if the node has none. On
 * the manager, call tcp_node_set_alive() instead, which keeps the node's
 * is_alive in step.
 */
bool tcp_gid_claims_set_alive(TcpBackendPrivate *priv, uint32_t node_id,
                              bool alive);

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
int tcp_conn_enqueue(TcpConnection *conn, const void *hdr, size_t hdr_len,
                     const void *body, size_t body_len, uint32_t dst_node,
                     uint64_t tag, bool reply);
void tcp_broadcast_gid_table(TcpBackendPrivate *priv);

/* The manager's table of mesh nodes */
MeshNodeInfo *tcp_mesh_node_info_new(uint32_t node_id, const char *hostname,
                                     uint16_t port);
void tcp_mesh_node_info_free(MeshNodeInfo *node);
void tcp_mesh_node_info_destroy_notify(gpointer data);
bool tcp_node_set_alive(TcpBackendPrivate *priv, MeshNodeInfo *node, bool alive,
                        bool *gids_changed);
void tcp_broadcast_mesh_topology(TcpBackendPrivate *priv);

/* Sockets and messages */
uint32_t tcp_send_timeout_ms(TcpBackendPrivate *priv);
int tcp_connect_any(const struct addrinfo *addrs, const atomic_bool *running,
                    uint64_t deadline_ms);
int tcp_connect_to_remote(const char *host, uint16_t port,
                          const atomic_bool *running, uint64_t deadline_ms);
int tcp_listen_socket(int family, uint16_t *port);
bool tcp_format_peer_addr(const struct sockaddr_storage *ss, char *host,
                          size_t len, uint16_t *port);
int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload,
                     TcpBufPool *pool, atomic_bool *running,
                     uint64_t deadline_ms);

/* Setup */
const char *tcp_config_role(const char *config, TcpMode *mode);
int tcp_parse_host_port(const char *s, const char *who, char **host,
                        uint16_t *port);
int tcp_init_with_interval(RdmaBackendDev *backend_dev, const char *config,
                           uint32_t health_interval_s);

/* Threads, and what they do */
bool tcp_start_thread(QemuThread *thread, const char *name, void *(*fn)(void *),
                      void *arg, atomic_bool *running);
void *tcp_accept_thread(void *opaque);
void *tcp_recv_thread_per_conn(void *opaque);
void *tcp_worker_peer_thread(void *opaque);
bool tcp_worker_publish_peer(TcpBackendPrivate *priv,
                             const TcpPeerTarget *target, TcpConnection *conn);
void *tcp_manager_health_check_thread(void *opaque);
/*
 * Declare a node dead, which withdraws its GID claims, only if it is still
 * unreachable; false when it has a live connection again, which a stale
 * list cannot be trusted for, or was dead already.
 */
bool tcp_declare_dead_if_unreachable(TcpBackendPrivate *priv, uint32_t node_id);
void tcp_health_check_retire(TcpBackendPrivate *priv, GArray *items);
TcpConnection *tcp_worker_manager_conn(TcpBackendPrivate *priv);
int tcp_worker_register_with_manager(TcpBackendPrivate *priv,
                                     const atomic_bool *running,
                                     uint64_t deadline_ms);
void tcp_worker_manager_pass(TcpBackendPrivate *priv);

/* Teardown */
void tcp_stop_accept_thread(TcpBackendPrivate *priv);
void tcp_stop_health_check_thread(TcpBackendPrivate *priv);
void tcp_stop_peer_thread(TcpBackendPrivate *priv);
void tcp_stop_recv_threads(TcpBackendPrivate *priv);
void tcp_free_pending_conns(TcpBackendPrivate *priv);
void tcp_stop_threads(TcpBackendPrivate *priv);

#endif /* RDMA_BACKEND_TCP_PRIV_H */
