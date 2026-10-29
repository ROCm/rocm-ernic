/*
 * rdma_backend_tcp_priv.h - TCP backend internals, for its files and tests
 *
 * The mesh wire protocol, the backend's private state, and the functions its
 * files share and its unit tests drive directly. Only the backend's files,
 * rdma_backend_tcp.c and the tcp_*.c files beside it, and its unit tests
 * include this; the rest of the device reaches the backend through
 * rdma_backend_ops_tcp, and the mesh through its RdmaMeshOps.
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

#include "ernic_mesh.h"
#include "pvrdma/rdma_backend_defs.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"

/*
 * Locks
 *
 * The order the backend's mutexes are taken in: a thread holding one takes
 * only those below it.  This is the one place the order is given; each
 * field's comment says what its lock protects.  Two locks needn't ever be
 * held together; the order says which comes first if they are.  The locks'
 * declarations in TcpBackendPrivate and TcpConnection repeat it, for clang's
 * thread-safety analysis (see qemu/clang-tsa.h), which checks it on every
 * path at compile time, though only where it can see both locks held: not
 * across a callback, say.  CI's TSan builds also check it on the paths the
 * tests run, since ThreadSanitizer reports a lock-order inversion as a
 * possible deadlock.
 *
 *   gid_advertise_lock       this node's advertisement of its own GIDs
 *   registration_mutex       a worker's registration with the manager
 *   peer_lock                the peers a worker is to connect to
 *   mesh_table_lock          mesh_nodes, on the manager only
 *   conn_table_lock          connections, pending_conns and manager_conn
 *   gid_table_lock           node_gids and gid_owner
 *
 * Below those are the leaves, held one at a time, with nothing taken under
 * them:
 *
 *   lock                     handles, local GIDs, the ionic receive callback
 *                            and the calls to it under way
 *   TcpConnection.lock       a connection's socket
 *   TcpConnection.sendq_lock a connection's send queue
 *   health_check_lock        stopping the health check
 *   broadcast_lock           what the manager's broadcaster has to send
 *   TcpSRQ.lock              an SRQ's limit (rdma_backend_tcp.c)
 *
 * A connection's lock is held for the whole of a message, which can mean
 * waiting for a peer that has stopped reading, for up to
 * tcp_send_timeout_ms() without progress.  gid_advertise_lock is held across
 * those sends, to keep a snapshot and its sends together, so such a peer
 * holds it up as well.  The manager's broadcasts hold no lock across their
 * sends; one thread sends them all (see broadcast_thread), so such a peer
 * holds up only that thread.
 */

/*
 * TCP Backend Protocol (Multi-Node Extension)
 *
 * Each message is a TCP_MSG_HDR_LEN-byte header followed by msg_len bytes of
 * payload; the header and the payloads are laid out below, after TcpMsgType.
 * Every multi-byte field in them is big-endian.  The layouts are offsets
 * rather than packed structs: a message arrives in a byte buffer, so the
 * accessors in byteorder.h are the only sanctioned way to read and write
 * its fields.  TCP_MSG_IONIC's payload is the ionic data path's own, laid
 * out, little-endian, in ionic_datapath_priv.h.
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
#define TCP_SENDQ_REQUEST_BYTES ((size_t)2 * TCP_MAX_PAYLOAD_LEN)
#define TCP_SENDQ_REPLY_BYTES   ((size_t)4 * TCP_MAX_PAYLOAD_LEN)
/* So a limit less a message's length, at most TCP_MAX_PAYLOAD_LEN, can't wrap
 */
_Static_assert(TCP_SENDQ_REQUEST_BYTES >= TCP_MAX_PAYLOAD_LEN,
               "a request's queue limit must hold the largest message");
_Static_assert(TCP_SENDQ_REPLY_BYTES >= TCP_MAX_PAYLOAD_LEN,
               "a reply's queue limit must hold the largest message");

/* ionic sizes its own wire payload against this; keep the two in step. */
_Static_assert(ERNIC_MESH_MAX_MSG == TCP_MAX_PAYLOAD_LEN,
               "ERNIC_MESH_MAX_MSG must match TCP_MAX_PAYLOAD_LEN");
/*
 * The data path asks a peer for no more than ERNIC_MESH_REPLY_BUDGET of
 * replies at once.  The peer's requests on the connection come to at most
 * TCP_SENDQ_REQUEST_BYTES, so the budget fits beside them under
 * TCP_SENDQ_REPLY_BYTES, and the replies the data path is still waiting for
 * fit the queue (see remote_post() in ionic_datapath.c for when they share
 * it with others).
 */
_Static_assert(ERNIC_MESH_REPLY_BUDGET <=
                   TCP_SENDQ_REPLY_BYTES - TCP_SENDQ_REQUEST_BYTES,
               "ERNIC_MESH_REPLY_BUDGET must fit beside a full request queue");

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

/*
 * The message header, TCP_MSG_HDR_LEN bytes:
 *
 *   [0:3]   be32 magic (TCP_PROTOCOL_MAGIC)
 *   [4:7]   be32 msg_type (TcpMsgType)
 *   [8:11]  be32 msg_len, the payload's length
 *   [12:15] be32 seq
 *   [16:19] be32 src_node_id
 *   [20:23] be32 dst_node_id
 *   [24:27] be32 src_qpn
 *   [28:31] be32 dst_qpn
 */
#define TCP_MSG_HDR_LEN          32
#define TCP_MSG_HDR_MAGIC_OFF    0
#define TCP_MSG_HDR_TYPE_OFF     4
#define TCP_MSG_HDR_MSG_LEN_OFF  8
#define TCP_MSG_HDR_SEQ_OFF      12
#define TCP_MSG_HDR_SRC_NODE_OFF 16
#define TCP_MSG_HDR_DST_NODE_OFF 20
#define TCP_MSG_HDR_SRC_QPN_OFF  24
#define TCP_MSG_HDR_DST_QPN_OFF  28
_Static_assert(TCP_MSG_HDR_DST_QPN_OFF + 4 == TCP_MSG_HDR_LEN,
               "the message header is on the wire and has changed size");

/*
 * The header as the code uses it, in host byte order; tcp_msg_hdr_put() and
 * tcp_msg_hdr_get() convert it to and from the wire.
 */
typedef struct {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t seq;
    uint32_t src_node_id;
    uint32_t dst_node_id;
    uint32_t src_qpn;
    uint32_t dst_qpn;
} TcpMsgHeader;

/*
 * The size of a payload's hostname field.  The name ends at a NUL within the
 * field, so it is at most TCP_HOSTNAME_LEN - 1 characters, and a field with
 * no NUL is refused (tcp_hostname_get()) rather than cut short.
 */
#define TCP_HOSTNAME_LEN 256

/*
 * TCP_MSG_HANDSHAKE and TCP_MSG_HANDSHAKE_RESP, TCP_HANDSHAKE_LEN bytes:
 *
 *   [0:3]   be32 node_id
 *   [4:7]   be32 version
 */
#define TCP_HANDSHAKE_LEN         8
#define TCP_HANDSHAKE_NODE_ID_OFF 0
#define TCP_HANDSHAKE_VERSION_OFF 4

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
 *
 * TCP_MSG_REGISTER_NODE, TCP_REGISTER_NODE_LEN bytes:
 *
 *   [0:255]   hostname, terminated within the field
 *   [256:257] be16 port the worker listens on
 *   [258:261] be32 requested_node_id, 0xFFFFFFFF for a new one
 *   [262:265] be32 version (appended in v4)
 */
#define TCP_REGISTER_NODE_LEN           266
#define TCP_REGISTER_NODE_HOSTNAME_OFF  0
#define TCP_REGISTER_NODE_PORT_OFF      256
#define TCP_REGISTER_NODE_REQUESTED_OFF 258
#define TCP_REGISTER_NODE_VERSION_OFF   262

/*
 * TCP_MSG_REGISTER_RESP, TCP_REGISTER_RESP_LEN bytes:
 *
 *   [0:3]   be32 assigned_node_id
 *   [4:7]   be32 num_nodes
 *   [8:11]  be32 result, an int32_t: 0 = success, negative = error
 *   [12:15] be32 version (appended in v4)
 */
#define TCP_REGISTER_RESP_LEN           16
#define TCP_REGISTER_RESP_NODE_ID_OFF   0
#define TCP_REGISTER_RESP_NUM_NODES_OFF 4
#define TCP_REGISTER_RESP_RESULT_OFF    8
#define TCP_REGISTER_RESP_VERSION_OFF   12

/* Sizes before the version field, i.e. what a pre-v4 peer sends. */
#define TCP_REGISTER_NODE_LEN_V3 262u
#define TCP_REGISTER_RESP_LEN_V3 12u

_Static_assert(TCP_REGISTER_NODE_VERSION_OFF == TCP_REGISTER_NODE_LEN_V3 &&
                   TCP_REGISTER_NODE_LEN == TCP_REGISTER_NODE_LEN_V3 + 4,
               "the version field must be appended, not inserted");
_Static_assert(TCP_REGISTER_RESP_VERSION_OFF == TCP_REGISTER_RESP_LEN_V3 &&
                   TCP_REGISTER_RESP_LEN == TCP_REGISTER_RESP_LEN_V3 + 4,
               "the version field must be appended, not inserted");
_Static_assert(TCP_REGISTER_NODE_PORT_OFF == TCP_HOSTNAME_LEN,
               "the port follows the hostname");

/* As many nodes as a topology broadcast can carry, and so as many as the
 * mesh can hold. */
#define TCP_MAX_MESH_NODES 64u

/*
 * TCP_MSG_MESH_TOPOLOGY, TCP_TOPOLOGY_LEN bytes, whatever num_nodes is:
 *
 *   [0:3]   be32 num_nodes
 *   [4:]    TCP_MAX_MESH_NODES node entries, the first num_nodes in use and
 *           the rest zero
 *
 * Node entry @i is at TCP_TOPOLOGY_NODE_OFF(@i), TCP_MESH_NODE_LEN bytes:
 *
 *   [0:3]     be32 node_id
 *   [4:259]   hostname, terminated within the field
 *   [260:261] be16 port
 *   [262]     is_alive, 1 or 0
 *   [263:265] reserved, zero
 */
#define TCP_MESH_NODE_LEN          266
#define TCP_MESH_NODE_ID_OFF       0
#define TCP_MESH_NODE_HOSTNAME_OFF 4
#define TCP_MESH_NODE_PORT_OFF     260
#define TCP_MESH_NODE_ALIVE_OFF    262
_Static_assert(TCP_MESH_NODE_PORT_OFF ==
                   TCP_MESH_NODE_HOSTNAME_OFF + TCP_HOSTNAME_LEN,
               "the port follows the hostname");

#define TCP_TOPOLOGY_NUM_NODES_OFF 0
#define TCP_TOPOLOGY_NODE_OFF(i)   (4 + (size_t)(i) * TCP_MESH_NODE_LEN)
#define TCP_TOPOLOGY_LEN           TCP_TOPOLOGY_NODE_OFF(TCP_MAX_MESH_NODES)
_Static_assert(TCP_TOPOLOGY_LEN == 17028,
               "the topology payload is on the wire and has changed size");

/*
 * A node's own guest RoCE GIDs, advertised so peers can map a destination
 * GID back to a node id.  A guest typically has two -- an IPv6 link-local
 * and whatever global address it is configured with -- so the cap leaves
 * room without making the message worth fragmenting.
 */
#define TCP_MAX_NODE_GIDS 8

/*
 * TCP_MSG_NODE_GIDS, TCP_NODE_GIDS_LEN bytes, whatever num_gids is:
 *
 *   [0:3]   be32 node_id
 *   [4:7]   be32 num_gids
 *   [8:]    TCP_MAX_NODE_GIDS 16-byte GIDs, the first num_gids in use and
 *           the rest zero
 */
#define TCP_NODE_GIDS_LEN         (8 + TCP_MAX_NODE_GIDS * ERNIC_GID_LEN)
#define TCP_NODE_GIDS_NODE_ID_OFF 0
#define TCP_NODE_GIDS_NUM_OFF     4
#define TCP_NODE_GIDS_GIDS_OFF    8

/* A NODE_GIDS payload, ready to send: its bytes, as they go on the wire. */
typedef struct {
    uint8_t bytes[TCP_NODE_GIDS_LEN];
} TcpNodeGidsWire;

/* A node's advertised set, as held in memory rather than on the wire. */
typedef struct {
    uint32_t num_gids;
    uint8_t gids[TCP_MAX_NODE_GIDS][ERNIC_GID_LEN];
    /*
     * Whether the claimant is answering. A dead node's claims are kept but
     * take no part in resolution, so the node's addresses are unreachable
     * rather than contested while it is away, and come back with it
     * without it having to advertise them again. Protected by
     * gid_table_lock, like the rest of this structure, and deliberately
     * not read from mesh_nodes: that would mean taking mesh_table_lock
     * under gid_table_lock, the reverse of the lock order (see Locks
     * above). On the manager it is a copy of the node's is_alive,
     * written with it by tcp_node_set_alive(); on a worker it is always
     * true, since the manager advertises a dead node as owning nothing.
     */
    bool alive;
} TcpNodeGids;

typedef struct TcpBackendPrivate TcpBackendPrivate;
typedef struct TcpConnection TcpConnection;

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
    /*
     * The locks, declared in the order they are taken (see Locks above),
     * and before the fields they guard, which the thread-safety analysis
     * needs (see qemu/clang-tsa.h).  The leaves follow gid_table_lock.
     *
     * gid_advertise_lock serialises a whole advertisement of this node's own
     * claims -- snapshot and record or send together.  The snapshot is taken
     * under lock, which has to be dropped before sending, so without this an
     * advertisement from the main thread (a guest adding or removing an
     * address) and one from a receive thread (registration completing) could
     * land out of order, leaving the manager holding a claim set this node has
     * already replaced.
     *
     * mesh_table_lock is only initialised on the manager.
     */
    QemuMutex gid_advertise_lock;
    QemuMutex registration_mutex TSA_ACQUIRED_AFTER(gid_advertise_lock);
    QemuMutex peer_lock TSA_ACQUIRED_AFTER(registration_mutex);
    QemuMutex mesh_table_lock TSA_ACQUIRED_AFTER(peer_lock);
    QemuMutex conn_table_lock TSA_ACQUIRED_AFTER(mesh_table_lock);
    QemuMutex gid_table_lock TSA_ACQUIRED_AFTER(conn_table_lock);
    QemuMutex lock TSA_ACQUIRED_AFTER(gid_table_lock);
    QemuMutex health_check_lock TSA_ACQUIRED_AFTER(gid_table_lock);
    QemuMutex broadcast_lock TSA_ACQUIRED_AFTER(gid_table_lock);

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
    GHashTable *connections TSA_GUARDED_BY(conn_table_lock);
    /*
     * Connections the manager has accepted that haven't registered, or were
     * refused, at most TCP_MAX_PENDING_CONNS of them, each holding a
     * reference.  Registration moves a connection, and its reference, from
     * here to connections, and only a connection still here can register.
     * tcp_reap_pending_conns() releases the ones that are done with, and
     * teardown stops the receive threads of the rest and releases them.
     */
    GPtrArray *pending_conns TSA_GUARDED_BY(conn_table_lock);

    /*
     * What each node says it owns: node_id -> TcpNodeGids.  This is the
     * authoritative record, kept per node rather than per GID so that a
     * claim is never lost by another node making the same one.
     */
    GHashTable *node_gids TSA_GUARDED_BY(gid_table_lock);

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
    GHashTable *gid_owner TSA_GUARDED_BY(gid_table_lock);

    /*
     * This node's own GIDs, seeded from ERNIC_TCP_GUEST_GIDS and then changed
     * by the guest through tcp_claim_gid().  Both are protected by lock.
     */
    uint8_t local_gids[TCP_MAX_NODE_GIDS][ERNIC_GID_LEN] TSA_GUARDED_BY(lock);
    uint32_t num_local_gids TSA_GUARDED_BY(lock);

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
    GHashTable *mesh_nodes TSA_GUARDED_BY(mesh_table_lock);
    /*
     * The manager's broadcaster thread, which tells the workers what has
     * changed: the topology, and a node's GID set, to every connection, and
     * the whole GID table to the connection of a node that has just
     * registered (tcp_broadcast_pass()).  A registration, a node's
     * advertisement, a node coming back to life, and the health check only
     * ask for these (tcp_broadcast_topology() and the rest), which never
     * waits for a peer, and broadcast_cond wakes the thread.
     *
     * A pass takes everything asked so far, then snapshots the tables, so
     * whatever is asked while it sends is sent by the next pass, and what is
     * asked twice before a pass is sent once.  Being one thread, it sends
     * its snapshots in the order it took them.  Sent from the threads that
     * ask, an older snapshot could be sent after a newer one: a node's death
     * after its revival, say, leaving the workers believing it dead, or a
     * topology that leaves out a node after a GID set the node claimed while
     * in it, which has the workers drop the claims (tcp_gid_claims_prune()).
     *
     * broadcast_running is cleared under broadcast_lock, with broadcast_cond
     * signalled, to stop the thread; see tcp_stop_broadcast_thread().  What
     * follows it is protected by broadcast_lock: broadcast_topology is set
     * when the topology has changed, broadcast_gids holds the ids of the
     * nodes whose GID sets have, and broadcast_tables those of the nodes
     * whose connections need the whole table.  broadcasts_asked counts the
     * asks, and broadcasts_sent how many of them the passes that have
     * finished took, so the thread has work while they differ, and the tests
     * can wait for them to agree.
     */
    QemuThread broadcast_thread;
    atomic_bool broadcast_running;
    QemuCond broadcast_cond;
    bool broadcast_topology TSA_GUARDED_BY(broadcast_lock);
    /* a set of node ids */
    GHashTable *broadcast_gids TSA_GUARDED_BY(broadcast_lock);
    /* a set of node ids */
    GHashTable *broadcast_tables TSA_GUARDED_BY(broadcast_lock);
    uint64_t broadcasts_asked TSA_GUARDED_BY(broadcast_lock);
    uint64_t broadcasts_sent TSA_GUARDED_BY(broadcast_lock);
    QemuThread health_check_thread;
    /*
     * Cleared under health_check_lock, with health_check_cond signalled, to
     * stop the thread; see tcp_stop_health_check_thread().  The thread also
     * reads it without the lock at the top of each pass.
     */
    atomic_bool health_check_running;
    QemuCond health_check_cond;
    /*
     * Also a worker's: how often its peer-connect thread runs, and so how
     * soon it notices that the manager has gone silent.
     */
    uint32_t health_check_interval_sec; /* Default: 5 seconds */
    /* For auto-assigning node IDs */
    uint32_t next_available_node_id TSA_GUARDED_BY(mesh_table_lock);

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
     * is stopped.  The answer publishes manager_conn under conn_table_lock
     * while holding it.
     *
     * registers_again, also under registration_mutex, is cleared for good
     * when the manager answers this worker's registering again with another
     * id, or refuses it with -ENOENT.  Only a manager that predates
     * registering again does the first: it gives a worker that asks for a
     * taken id a new one, so each time the worker asked, the mesh would gain
     * a node.  The second means the manager has no node with this worker's
     * id, which it never gives out again, so every later try would be
     * refused too.
     */
    TcpConnection *registering TSA_GUARDED_BY(registration_mutex);
    bool registration_complete TSA_GUARDED_BY(registration_mutex);
    bool registration_accepted TSA_GUARDED_BY(registration_mutex);
    bool registers_again TSA_GUARDED_BY(registration_mutex);
    QemuCond registration_cond;

    /*
     * The peers this worker is to connect to, as TcpPeerTarget, from the
     * latest topology update.  A receive thread replaces them and sets
     * peer_targets_changed; the peer-connect thread connects to them.  Both
     * are protected by peer_lock.  The peer-connect thread holds it with
     * conn_table_lock to compare the connections table with the targets and
     * change it.
     */
    GArray *peer_targets TSA_GUARDED_BY(peer_lock);
    bool peer_targets_changed TSA_GUARDED_BY(peer_lock);
    QemuCond peer_cond; /* signalled on an update, and to stop the thread */
    QemuThread peer_thread;
    /*
     * Cleared under peer_lock, with peer_cond signalled, to stop the thread;
     * see tcp_stop_peer_thread().  The thread also reads it without the lock
     * between connects.
     */
    atomic_bool peer_thread_running;

    /* Handle generators, protected by lock */
    uint32_t next_pd_handle TSA_GUARDED_BY(lock);
    uint32_t next_cq_handle TSA_GUARDED_BY(lock);
    uint32_t next_qpn TSA_GUARDED_BY(lock);

    /* Sequence number for protocol */
    _Atomic uint32_t next_seq;

    /* Sink for TCP_MSG_IONIC, registered by the ionic data path.  Called on
     * a receive thread, so the callee must hand off to its own thread; it
     * takes the payload with it, so a large one isn't copied, or refuses it
     * until it has room (see tcp_recv_ionic()).  Both are protected by
     * lock. */
    ernic_mesh_recv_fn ionic_recv_fn TSA_GUARDED_BY(lock);
    void *ionic_recv_opaque TSA_GUARDED_BY(lock);
    /* How many receive threads are calling ionic_recv_fn, which they do
     * without lock; ionic_recv_idle is signalled, with lock, when that falls
     * to 0.  tcp_mesh_set_recv_cb() waits on it, so that once it has
     * returned nothing is still calling the callback it replaced, and the
     * data path can free what that callback uses. */
    uint32_t ionic_recv_calls TSA_GUARDED_BY(lock);
    QemuCond ionic_recv_idle;

    /* TCP-layer counters */
    struct {
        _Atomic uint64_t peer_connect_attempts; /* a worker's, to its peers */
        /* A worker's, registering again with the manager */
        _Atomic uint64_t manager_reconnects;
    } tcp_stats;
};

/*
 * A message waiting on a connection's send queue: a TCP_MSG_IONIC, or a
 * heartbeat or its answer, which has no payload (see tcp_send_heartbeat()).
 * It is from malloc(), not g_malloc(): see tcp_queued_msg_new().
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
struct TcpConnection {
    TcpBackendPrivate *priv;
    /*
     * Serialises writes to sockfd, and protects sockfd itself.  A writer
     * holds it for a whole message, which can mean waiting for the peer to
     * read, for up to tcp_send_timeout_ms() without progress.  It and
     * sendq_lock are leaves (see Locks).
     */
    QemuMutex lock TSA_ACQUIRED_AFTER(priv->gid_table_lock);
    QemuMutex sendq_lock TSA_ACQUIRED_AFTER(priv->gid_table_lock);

    uint32_t node_id;
    int sockfd TSA_GUARDED_BY(lock);
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
     * The data path's messages, queued for send_thread, so the device's main
     * thread never waits for the peer (see tcp_mesh_sendv()), and heartbeats
     * that couldn't be sent at once, at the head of the queue (see
     * tcp_send_heartbeat()).  Everything below is protected by sendq_lock.
     * sendq_cond is signalled when a message is queued and when the queue
     * is closed.
     *
     * sendq_bytes counts each message's payload until it has been written.
     * sendq_closed is set, never cleared, when the connection leaves its
     * table or manager_conn (tcp_conn_unpublished()), when it is retired, or
     * when a send on it fails; nothing is queued after that, and what was
     * queued is dropped.  send_thread is started by the first message queued,
     * and send_thread_started is set while there is a thread to join.
     */
    QemuCond sendq_cond;
    GQueue sendq TSA_GUARDED_BY(sendq_lock); /* of TcpQueuedMsg */
    size_t sendq_bytes TSA_GUARDED_BY(sendq_lock);
    bool sendq_closed TSA_GUARDED_BY(sendq_lock);
    bool send_thread_started TSA_GUARDED_BY(sendq_lock);
    QemuThread send_thread;
};

/* This node's ID; see local_node_id for why it is atomic. */
static inline uint32_t tcp_local_node(const TcpBackendPrivate *priv)
{
    return atomic_load(&priv->local_node_id);
}

/*
 * Whether a worker has its node id.  Until it does, its id reads as 0, the
 * manager's, which it must not present as its own.
 */
static inline bool tcp_worker_registered(const TcpBackendPrivate *priv)
{
    return tcp_local_node(priv) != 0;
}

/*
 * Whether priv has the GID tables, node_gids and gid_owner, or the mesh
 * table, which only a manager has.  tcp_init() makes them before any other
 * thread can reach priv, and they stay until teardown, so whether they exist
 * is read without the lock that guards what is in them.  A unit test's priv
 * may lack them, and then the lock too.
 */
static inline bool tcp_has_gid_tables(const TcpBackendPrivate *priv)
{
    bool has;

    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    has = priv->node_gids && priv->gid_owner;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    return has;
}

static inline bool tcp_has_mesh_table(const TcpBackendPrivate *priv)
{
    bool has;

    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    has = priv->mesh_nodes != NULL;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    return has;
}

/*
 * View a GID array as const, for the functions that only read one.  Before
 * C23, ISO C does not convert uint8_t (*)[N] to const uint8_t (*)[N]
 * implicitly, because the const qualifies the array's elements rather than
 * the array.  Taking the array through a typed parameter keeps its
 * [ERNIC_GID_LEN] shape checked, which a bare cast at each call site would
 * not.
 */
static inline const uint8_t (
    *tcp_const_gids(uint8_t (*gids)[ERNIC_GID_LEN]))[ERNIC_GID_LEN]
{
    return (const uint8_t(*)[ERNIC_GID_LEN])gids;
}

/*
 * A message a receive thread has read, as its handler gets it: the header,
 * in host byte order, and the payload, msg_len bytes from malloc(), or NULL
 * if there is none.  A handler may take the payload, setting it to NULL; the
 * receive thread frees what is left.
 */
typedef struct {
    TcpMsgHeader hdr;
    void *payload;
} TcpRecvMsg;

/* 8 groups of 4 hex digits, 7 separators, NUL. */
#define TCP_GID_STR_LEN 40

/* A peer a worker is to connect to, picked from a MESH_TOPOLOGY update. */
typedef struct {
    uint32_t node_id;
    char host[TCP_HOSTNAME_LEN];
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
 * The functions the backend's files share, and those its unit tests drive
 * directly, by the file that defines them, where each is documented.
 * tcp_init(), tcp_fini() and the other entry points the device uses are
 * reached through rdma_backend_ops_tcp instead.
 */

/* rdma_backend_tcp.c: setup, teardown, and the data path's messages */
int tcp_env_int(const char *name, int fallback);
const char *tcp_config_role(const char *config, TcpMode *mode);
int tcp_parse_host_port(const char *s, const char *who, char **host,
                        uint16_t *port);
int tcp_init_with_interval(RdmaBackendDev *backend_dev, const char *config,
                           uint32_t health_interval_s);
bool tcp_start_thread(QemuThread *thread, const char *name, void *(*fn)(void *),
                      void *arg, atomic_bool *running);
void tcp_stop_threads(TcpBackendPrivate *priv);
void tcp_recv_ionic(const TcpConnection *conn, TcpRecvMsg *msg);

/* tcp_wire.c: sockets, and messages on them */
void tcp_tune_sockbuf(int sockfd);
char *tcp_addr_str(const char *host, uint16_t port);
int tcp_connect_any(const struct addrinfo *addrs, const atomic_bool *running,
                    uint64_t deadline_ms);
int tcp_connect_to_remote(const char *host, uint16_t port,
                          const atomic_bool *running, uint64_t deadline_ms);
int tcp_listen_socket(int family, uint16_t *port);
int tcp_listen_on_port(uint16_t *port);
bool tcp_format_peer_addr(const struct sockaddr_storage *ss, char *host,
                          size_t len, uint16_t *port);
uint32_t tcp_send_timeout_ms(const TcpBackendPrivate *priv);
void tcp_msg_hdr_put(uint8_t *wire, const TcpMsgHeader *hdr);
void tcp_msg_hdr_get(TcpMsgHeader *hdr, const uint8_t *wire);
bool tcp_hostname_get(char *host, const uint8_t *wire);
int tcp_send_message2(const TcpBackendPrivate *priv, TcpConnection *conn,
                      TcpMsgType msg_type, const void *payload,
                      size_t payload_len, const void *payload2,
                      size_t payload2_len, uint32_t seq, uint32_t src_node,
                      uint32_t dst_node, uint32_t src_qpn, uint32_t dst_qpn)
    TSA_REQUIRES(conn->lock);
int tcp_send_message(const TcpBackendPrivate *priv, TcpConnection *conn,
                     TcpMsgType msg_type, const void *payload,
                     size_t payload_len, uint32_t seq, uint32_t src_node,
                     uint32_t dst_node, uint32_t src_qpn, uint32_t dst_qpn)
    TSA_REQUIRES(conn->lock);
int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload,
                     const atomic_bool *running, uint64_t deadline_ms);

/*
 * tcp_conn.c: connections, their send queues and threads, and the accept
 * thread; see TcpConnection for the reference counting rules
 */
TcpConnection *tcp_connection_new(TcpBackendPrivate *priv, uint32_t node_id,
                                  const char *host, uint16_t port, int sockfd);
TcpConnection *tcp_connection_ref(TcpConnection *conn);
void tcp_connection_unref(TcpConnection *conn);
void tcp_connection_destroy_notify(gpointer data);
bool tcp_conn_dead(const TcpConnection *conn);
TcpConnection *tcp_connection_lookup(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->conn_table_lock);
GPtrArray *tcp_conns_snapshot(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->conn_table_lock);
bool tcp_has_live_connection(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->conn_table_lock);
TcpConnection *tcp_route_to_node(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->conn_table_lock);
bool tcp_conn_table_add(TcpBackendPrivate *priv, uint32_t node_id,
                        TcpConnection *conn)
    TSA_REQUIRES(priv->conn_table_lock);
void tcp_conn_unpublished(TcpConnection *conn) TSA_EXCLUDES(conn->sendq_lock);
bool tcp_conn_table_take(TcpBackendPrivate *priv, uint32_t node_id,
                         TcpConnection *conn)
    TSA_REQUIRES(priv->conn_table_lock);
void tcp_retire_conn(TcpConnection *conn)
    TSA_EXCLUDES(conn->lock, conn->sendq_lock);
int tcp_conn_enqueue(TcpConnection *conn, const void *hdr, size_t hdr_len,
                     const void *body, size_t body_len, uint32_t dst_node,
                     uint64_t tag, bool reply) TSA_EXCLUDES(conn->sendq_lock);
void tcp_sendq_cancel(TcpConnection *conn, uint64_t tag, uint64_t mask)
    TSA_EXCLUDES(conn->sendq_lock);
bool tcp_send_heartbeat(TcpBackendPrivate *priv, TcpConnection *conn,
                        TcpMsgType msg_type, uint32_t dst_node)
    TSA_EXCLUDES(conn->lock, conn->sendq_lock);
bool tcp_start_recv_thread(TcpConnection *conn, const char *name);
void *tcp_recv_thread_per_conn(void *opaque);
void *tcp_accept_thread(void *opaque);
void tcp_stop_accept_thread(TcpBackendPrivate *priv);
void tcp_stop_recv_threads(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->conn_table_lock);
void tcp_free_pending_conns(TcpBackendPrivate *priv);

/*
 * tcp_gid.c: GIDs, the GID -> node table, and the messages that advertise a
 * node's GIDs
 */
char *tcp_gid_str(const uint8_t gid[ERNIC_GID_LEN], char buf[TCP_GID_STR_LEN]);
uint32_t tcp_parse_guest_gids(const char *val,
                              uint8_t out[TCP_MAX_NODE_GIDS][ERNIC_GID_LEN]);
void tcp_gid_key_free(gpointer key);
bool tcp_gid_claims_set(TcpBackendPrivate *priv, uint32_t node_id,
                        const uint8_t (*gids)[ERNIC_GID_LEN], uint32_t num_gids)
    TSA_EXCLUDES(priv->gid_table_lock);
void tcp_gid_claims_forget(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->gid_table_lock);
void tcp_gid_claims_prune(TcpBackendPrivate *priv, const uint8_t *topo,
                          uint32_t num_nodes)
    TSA_EXCLUDES(priv->gid_table_lock);
/*
 * Mark @node_id's claims live or dead. A node the manager has declared
 * dead keeps its claims but resolves to nothing; see TcpNodeGids.alive.
 * Returns whether they changed, which they don't if the node has none. On
 * the manager, call tcp_node_set_alive() instead, which keeps the node's
 * is_alive in step.
 */
bool tcp_gid_claims_set_alive(TcpBackendPrivate *priv, uint32_t node_id,
                              bool alive) TSA_EXCLUDES(priv->gid_table_lock);
void tcp_gid_claims_report_alive(uint32_t node_id, bool alive);
uint32_t tcp_resolve_node_from_gid(TcpBackendPrivate *priv,
                                   const union ibv_gid *dgid)
    TSA_EXCLUDES(priv->gid_table_lock);
void tcp_fill_node_gids(TcpNodeGidsWire *out, uint32_t node_id,
                        const uint8_t (*gids)[ERNIC_GID_LEN],
                        uint32_t num_gids);
void tcp_send_node_gids(TcpBackendPrivate *priv, TcpConnection *conn,
                        const TcpNodeGidsWire *payload)
    TSA_EXCLUDES(conn->lock);
void tcp_advertise_local_gids(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->gid_advertise_lock, priv->lock);
int tcp_claim_gid(TcpBackendPrivate *priv, const union ibv_gid *gid, bool add)
    TSA_EXCLUDES(priv->gid_advertise_lock, priv->lock);
void tcp_recv_node_gids(const TcpConnection *conn, const TcpRecvMsg *msg);

/*
 * tcp_manager.c: the table of mesh nodes, registration, the broadcasts (see
 * broadcast_thread), the health check, and the relay
 */
MeshNodeInfo *tcp_mesh_node_info_new(uint32_t node_id, const char *hostname,
                                     uint16_t port);
void tcp_mesh_node_info_free(MeshNodeInfo *node);
void tcp_mesh_node_info_destroy_notify(gpointer data);
bool tcp_node_set_alive(TcpBackendPrivate *priv, MeshNodeInfo *node, bool alive,
                        bool *gids_changed) TSA_REQUIRES(priv->mesh_table_lock);
/*
 * Declare a node dead, which withdraws its GID claims, only if it is still
 * unreachable; false when it has a live connection again, which a stale
 * list cannot be trusted for, or was dead already.  The caller asks for the
 * broadcast that tells the workers.
 */
bool tcp_declare_dead_if_unreachable(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->mesh_table_lock);
bool tcp_manager_recv_register(TcpConnection *conn, const TcpRecvMsg *msg);
void tcp_manager_recv_heartbeat(TcpConnection *conn);
void tcp_manager_relay_ionic(TcpBackendPrivate *priv, const TcpRecvMsg *msg);
void tcp_reap_pending_conns(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->conn_table_lock);
void tcp_manager_accept(TcpBackendPrivate *priv, int sockfd, const char *host,
                        uint16_t port) TSA_EXCLUDES(priv->conn_table_lock);
void tcp_broadcaster_init(TcpBackendPrivate *priv);
void tcp_broadcaster_destroy(TcpBackendPrivate *priv);
void tcp_broadcast_topology(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->broadcast_lock);
void tcp_broadcast_node_gids(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->broadcast_lock);
void tcp_broadcast_node(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->broadcast_lock);
void tcp_broadcast_gid_table_to(TcpBackendPrivate *priv, uint32_t node_id)
    TSA_EXCLUDES(priv->broadcast_lock);
void tcp_broadcast_pass(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->broadcast_lock);
void *tcp_broadcast_thread(void *opaque);
void tcp_stop_broadcast_thread(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->broadcast_lock);
void tcp_health_check_retire(TcpBackendPrivate *priv, GArray *items)
    TSA_EXCLUDES(priv->mesh_table_lock, priv->conn_table_lock);
void *tcp_manager_health_check_thread(void *opaque);
void tcp_stop_health_check_thread(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->health_check_lock);

/*
 * tcp_worker.c: registration with the manager, the peer-connect thread, and
 * the peers that connect to the worker
 */
TcpConnection *tcp_worker_manager_conn(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->conn_table_lock);
int tcp_worker_register_with_manager(TcpBackendPrivate *priv,
                                     const atomic_bool *running,
                                     uint64_t deadline_ms)
    TSA_EXCLUDES(priv->registration_mutex);
bool tcp_worker_publish_peer(TcpBackendPrivate *priv,
                             const TcpPeerTarget *target, TcpConnection *conn)
    TSA_EXCLUDES(priv->peer_lock, priv->conn_table_lock);
void tcp_worker_manager_pass(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->conn_table_lock, priv->registration_mutex);
void *tcp_worker_peer_thread(void *opaque);
void tcp_stop_peer_thread(TcpBackendPrivate *priv)
    TSA_EXCLUDES(priv->peer_lock, priv->registration_mutex);
void tcp_worker_recv_register_resp(TcpConnection *conn, const TcpRecvMsg *msg);
void tcp_worker_recv_topology(const TcpConnection *conn, const TcpRecvMsg *msg);
void tcp_worker_accept(TcpBackendPrivate *priv, int sockfd, const char *host,
                       uint16_t port);

#endif /* RDMA_BACKEND_TCP_PRIV_H */
