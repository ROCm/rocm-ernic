/*
 * RDMA Backend: TCP mesh - connections
 *
 * TcpConnection and its life: reference counting, the connections table, the
 * send queue and its thread, retiring a connection, and stopping every
 * connection's threads at teardown.  Also the two threads that read the
 * mesh: the receive thread, one per connection, which hands each message to
 * its type's handler (tcp_recv_dispatch()), and the accept thread, which
 * hands each incoming connection to the manager or the worker.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

/* accept4() is a GNU extension */
#define _GNU_SOURCE

#include "rdma/rdma_backend_tcp_priv.h"
#include "ernic_time.h"
#include "pvrdma/rdma_utils.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <poll.h>

static void tcp_sendq_close(TcpConnection *conn) TSA_REQUIRES(conn->sendq_lock);
static void tcp_stop_send_thread(TcpConnection *conn)
    TSA_EXCLUDES(conn->sendq_lock);

/*
 * conn->sockfd, read without conn->lock, which only a thread that nothing can
 * change it under may do.  Once conn is published, only tcp_retire_conn()
 * changes it, and only one thread retires a connection (see TcpConnection),
 * so that thread may, as may conn's receive thread, which retiring joins
 * before the change, teardown once the threads that retire connections have
 * stopped, and whoever drops the last reference.
 */
static int tcp_conn_sockfd_unlocked(const TcpConnection *conn)
{
    int fd;

    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    fd = conn->sockfd;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    return fd;
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
    int fd = tcp_conn_sockfd_unlocked(conn);
    bool send_running;

    qemu_mutex_lock(&conn->sendq_lock);
    send_running = conn->send_thread_started;
    qemu_mutex_unlock(&conn->sendq_lock);

    if (recv_running || send_running) {
        rdma_error_report("TCP: freeing the connection to node %u with its "
                          "threads running",
                          conn->node_id);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
        if (recv_running) {
            qemu_thread_join(&conn->recv_thread);
        }
    }
    tcp_stop_send_thread(conn);

    if (fd >= 0) {
        close(fd);
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

/*
 * A new connection to node_id at host:port, on sockfd, which it takes over,
 * holding one reference, the caller's.
 */
TcpConnection *tcp_connection_new(TcpBackendPrivate *priv, uint32_t node_id,
                                  const char *host, uint16_t port, int sockfd)
{
    TcpConnection *conn = g_new0(TcpConnection, 1);
    conn->priv = priv;
    conn->node_id = node_id;
    conn->remote_host = g_strdup(host);
    conn->remote_port = port;
    atomic_init(&conn->refcount, 1);
    atomic_init(&conn->is_connected, true);
    atomic_init(&conn->recv_thread_running, false);
    atomic_init(&conn->recv_thread_exited, false);
    atomic_init(&conn->last_rx_ms, ernic_monotonic_ms());
    qemu_mutex_init(&conn->lock);
    qemu_mutex_init(&conn->sendq_lock);
    qemu_cond_init(&conn->sendq_cond);
    /* No other thread can reach conn yet */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    conn->sockfd = sockfd;
    g_queue_init(&conn->sendq);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON
    return conn;
}

/*
 * Whether conn is dead: its receive thread has returned, because the peer
 * hung up, a read failed, or the peer was refused.  Nothing more can come on
 * it, and it can't be sent on, but it keeps its descriptor until whoever
 * takes it out of its table retires it.
 */
bool tcp_conn_dead(const TcpConnection *conn)
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
GPtrArray *tcp_conns_snapshot(TcpBackendPrivate *priv)
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
bool tcp_conn_table_add(TcpBackendPrivate *priv, uint32_t node_id,
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
 * conn_table_lock, which guards both.  tcp_mesh_cancel() finds a connection
 * only there, so a queue left open until the connection is retired, after
 * the lock is dropped, could start a request the cancel had taken back
 * everywhere else.  What was queued is dropped, as retiring the connection
 * would drop it; the message being written goes out, as it would have
 * anyway.
 */
void tcp_conn_unpublished(TcpConnection *conn)
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
    int fd = tcp_conn_sockfd_unlocked(conn);

    if (atomic_exchange(&conn->recv_thread_running, false)) {
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
        qemu_thread_join(&conn->recv_thread);
    } else if (fd >= 0) {
        /* For the send thread, which may be waiting on the peer */
        shutdown(fd, SHUT_RDWR);
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
 * Drop what is on conn's send queue and close it to more.  The caller holds
 * sendq_lock.
 */
static void tcp_sendq_close(TcpConnection *conn)
{
    TcpQueuedMsg *msg;

    conn->sendq_closed = true;
    while ((msg = g_queue_pop_head(&conn->sendq)) != NULL) {
        free(msg);
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
        free(msg);
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
static int tcp_sendq_start(TcpConnection *conn) TSA_REQUIRES(conn->sendq_lock)
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
 * to fill in, or NULL if out of memory.  len is at most TCP_MAX_PAYLOAD_LEN,
 * so the size can't wrap.
 *
 * The guest chooses len, so running out of memory here mustn't abort the
 * server, as g_malloc() would.  The message is from malloc() rather than
 * g_try_malloc() so that a test can fail the allocation.
 */
static TcpQueuedMsg *tcp_queued_msg_new(const TcpConnection *conn,
                                        TcpMsgType msg_type, uint32_t dst_node,
                                        size_t len)
{
    TcpQueuedMsg *msg = malloc(sizeof(*msg) + len);

    if (!msg) {
        return NULL;
    }
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
 *   -ENOMEM   the send thread couldn't be started, or there was no memory
 *             for the message
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
    if (!msg) {
        qemu_mutex_unlock(&conn->sendq_lock);
        return -ENOMEM;
    }
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
static bool tcp_sendq_has_heartbeat(const TcpConnection *conn,
                                    TcpMsgType msg_type)
    TSA_REQUIRES(conn->sendq_lock)
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
 * thread couldn't be started or there was no memory for the message.
 */
static int tcp_sendq_push_heartbeat(TcpConnection *conn, TcpMsgType msg_type,
                                    uint32_t dst_node)
    TSA_EXCLUDES(conn->sendq_lock)
{
    int ret;

    qemu_mutex_lock(&conn->sendq_lock);
    ret = conn->sendq_closed ? -EPIPE : tcp_sendq_start(conn);
    if (ret == 0 && !tcp_sendq_has_heartbeat(conn, msg_type)) {
        TcpQueuedMsg *msg = tcp_queued_msg_new(conn, msg_type, dst_node, 0);

        if (msg) {
            g_queue_push_head(&conn->sendq, msg);
            qemu_cond_signal(&conn->sendq_cond);
        } else {
            ret = -ENOMEM;
        }
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
void tcp_sendq_cancel(TcpConnection *conn, uint64_t tag, uint64_t mask)
{
    qemu_mutex_lock(&conn->sendq_lock);
    for (GList *link = conn->sendq.head; link != NULL;) {
        GList *next = link->next;
        TcpQueuedMsg *msg = link->data;

        if (msg->msg_type == TCP_MSG_IONIC && !msg->reply &&
            (msg->tag & mask) == (tag & mask)) {
            conn->sendq_bytes -= msg->len;
            g_queue_delete_link(&conn->sendq, link);
            free(msg);
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
 * the send failed, which shuts it down.  It also returns false if the
 * heartbeat couldn't be queued for want of a send thread or memory.
 */
bool tcp_send_heartbeat(TcpBackendPrivate *priv, TcpConnection *conn,
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
 * Start conn's receive thread.  On failure the caller frees conn rather than
 * publishing it.  Callers either hold conn_table_lock or start the thread
 * before tcp_stop_recv_threads() can reach conn.
 */
bool tcp_start_recv_thread(TcpConnection *conn, const char *name)
{
    return tcp_start_thread(&conn->recv_thread, name, tcp_recv_thread_per_conn,
                            conn, &conn->recv_thread_running);
}

/*
 * TCP_MSG_HEARTBEAT.  On the manager it is a worker saying it is alive
 * (tcp_manager_recv_heartbeat()).
 */
static void tcp_recv_heartbeat(TcpConnection *conn, const TcpRecvMsg *msg)
{
    TcpBackendPrivate *priv = conn->priv;

    if (!priv)
        return;

    if (priv->is_manager) {
        tcp_manager_recv_heartbeat(conn);
        return;
    }

    /* Worker received heartbeat probe from manager — echo it back so the
     * manager refreshes our liveness timestamp.  Without waiting for the
     * lock, as the manager answers. */
    tcp_send_heartbeat(priv, conn, TCP_MSG_HEARTBEAT, msg->hdr.src_node_id);
}

/*
 * Handle one message that has come on conn, and return whether to hang up:
 * to close the connection after it, since nothing more the peer sends is of
 * use.  Each message type has a handler of its own, in the file of the role
 * that takes it.
 *
 * A manager's connection that hasn't registered (its node id is set only by
 * registration, on this thread) may only register.  A worker registers
 * before it sends anything else, so anything else is a peer that isn't one,
 * and it gets no further: a relay or a reply sent for it could wait on a
 * node that has stopped reading, and the reaper that joins this thread would
 * wait too.
 */
static bool tcp_recv_dispatch(TcpConnection *conn, TcpRecvMsg *msg)
{
    const TcpMsgHeader *hdr = &msg->hdr;

    if (conn->priv && conn->priv->is_manager && conn->node_id == UINT32_MAX &&
        hdr->msg_type != TCP_MSG_REGISTER_NODE) {
        rdma_error_report("TCP: message type %u from %s:%u, which "
                          "hasn't registered; closing the connection",
                          hdr->msg_type, conn->remote_host, conn->remote_port);
        return true;
    }

    switch (hdr->msg_type) {
    case TCP_MSG_HANDSHAKE:
        rdma_info_report("TCP: Received handshake from node %u",
                         hdr->src_node_id);
        return false;

    case TCP_MSG_HANDSHAKE_RESP:
        /* Handshake complete, connection is established */
        rdma_info_report("TCP: Received handshake response from node %u",
                         hdr->src_node_id);
        return false;

    case TCP_MSG_REGISTER_NODE:
        return tcp_manager_recv_register(conn, msg);

    case TCP_MSG_REGISTER_RESP:
        tcp_worker_recv_register_resp(conn, msg);
        return false;

    case TCP_MSG_MESH_TOPOLOGY:
        tcp_worker_recv_topology(conn, msg);
        return false;

    case TCP_MSG_NODE_GIDS:
        tcp_recv_node_gids(conn, msg);
        return false;

    case TCP_MSG_HEARTBEAT:
        tcp_recv_heartbeat(conn, msg);
        return false;

    case TCP_MSG_HEARTBEAT_RESP:
        /*
         * Acknowledgement — no action needed.  That it came at all, recorded
         * in last_rx_ms as for any message, is what a worker that sent a
         * heartbeat is waiting to see.
         */
        return false;

    case TCP_MSG_IONIC:
        tcp_recv_ionic(conn, msg);
        return false;

    default:
        rdma_warn_report("TCP: Unknown message type %u from node %u",
                         hdr->msg_type, hdr->src_node_id);
        return false;
    }
}

/*
 * Per-connection receive thread: reads each message as it comes, and hands
 * it to tcp_recv_dispatch(), until it is stopped, the peer hangs up, a read
 * fails, or the handler is done with the peer.
 */
void *tcp_recv_thread_per_conn(void *opaque)
{
    TcpConnection *conn = (TcpConnection *)opaque;
    TcpRecvMsg msg;
    int ret;
    struct pollfd pfd;
    bool hang_up;

    rdma_info_report("TCP: Receive thread started for node %u", conn->node_id);

    pfd.fd = tcp_conn_sockfd_unlocked(conn);
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
            ret = tcp_recv_message(pfd.fd, &msg.hdr, &msg.payload,
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

            hang_up = tcp_recv_dispatch(conn, &msg);
            /* NULL if the handler took it */
            free(msg.payload);

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

/*
 * Accept thread - accepts incoming connections, and hands each to the
 * manager (tcp_manager_accept()), to register, or to the worker
 * (tcp_worker_accept()), to say with a handshake which peer it is.
 */
void *tcp_accept_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    struct sockaddr_storage client_addr;
    socklen_t client_len;
    char client_host[INET6_ADDRSTRLEN];
    uint16_t client_port;
    char *client_str;
    int sockfd;

    rdma_info_report("TCP: Accept thread started");

    while (atomic_load(&priv->accept_thread_running)) {
        memset(&client_addr, 0, sizeof(client_addr));
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

        /* Can't fail: the listen socket is IPv4 or IPv6 */
        if (!tcp_format_peer_addr(&client_addr, client_host,
                                  sizeof(client_host), &client_port)) {
            rdma_error_report("TCP: Failed to format the address of an "
                              "incoming connection: %s",
                              strerror(errno));
            close(sockfd);
            continue;
        }

        client_str = tcp_addr_str(client_host, client_port);
        rdma_info_report("TCP: Accepted connection from %s", client_str);
        g_free(client_str);

        /* Either takes over sockfd */
        if (priv->is_manager) {
            tcp_manager_accept(priv, sockfd, client_host, client_port);
        } else {
            tcp_worker_accept(priv, sockfd, client_host, client_port);
        }
    }

    rdma_info_report("TCP: Accept thread exiting");
    return NULL;
}

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
    int fd = tcp_conn_sockfd_unlocked(conn);
    bool send_running;

    qemu_mutex_lock(&conn->sendq_lock);
    tcp_sendq_close(conn);
    send_running = conn->send_thread_started;
    conn->send_thread_started = false;
    qemu_mutex_unlock(&conn->sendq_lock);

    if ((recv_running || send_running) && fd >= 0) {
        shutdown(fd, SHUT_RDWR);
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
    GPtrArray *pending;

    /* Every thread that can reach the list has stopped */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    pending = priv->pending_conns;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    for (guint i = 0; i < pending->len; i++) {
        tcp_connection_unref(g_ptr_array_index(pending, i));
    }
    g_ptr_array_free(pending, TRUE);
}
