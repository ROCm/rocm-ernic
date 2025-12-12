/*
 * RDMA Backend: TCP/IP Network - Multi-Node Support
 *
 * TCP/IP backend for connecting multiple rocm_ernic server instances
 * over network in a mesh topology. Enables RDMA operations between
 * multiple servers without physical hardware.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_backend.h"
#include "rdma_rm.h"
#include "rdma_utils.h"
#include "standard-headers/rdma/vmw_pvrdma-abi.h"
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

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

#define TCP_PROTOCOL_MAGIC   0x52444D41 /* "RDMA" */
#define TCP_PROTOCOL_VERSION 2          /* Multi-node version */

typedef enum {
    TCP_MSG_HANDSHAKE = 1,
    TCP_MSG_HANDSHAKE_RESP,
    TCP_MSG_POST_SEND,
    TCP_MSG_POST_RECV,
    TCP_MSG_COMPLETION,
    TCP_MSG_DATA,
    TCP_MSG_QP_STATE_INIT,
    TCP_MSG_QP_STATE_RTR,
    TCP_MSG_QP_STATE_RTS,
    TCP_MSG_QUERY_REMOTE_CONN_INFO,
    TCP_MSG_REMOTE_CONN_INFO_RESP,
} TcpMsgType;

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
 * TCP Backend Data Structures
 */

typedef struct {
    uint32_t handle;
} TcpPD;

typedef struct {
    uint32_t handle;
    void *virt;
    size_t length;
    uint64_t guest_start;
    int access_flags;
    uint32_t lkey;
    uint32_t rkey;
    uint32_t pd_handle;
} TcpMR;

typedef struct {
    enum ibv_wc_status status;
    uint64_t wr_id;
    uint32_t byte_len;
    uint32_t qp_num;
    enum ibv_wc_opcode opcode;
} TcpCompletion;

typedef struct {
    uint32_t handle;
    int cqe;
    GQueue *completions; /* Queue of TcpCompletion */
    QemuMutex lock;
} TcpCQ;

typedef struct {
    void *addr;
    void *host_addr; /* translated host pointer from MR */
    uint32_t length;
    uint32_t lkey;
} TcpSGE;

typedef struct {
    uint64_t wr_id;
    uint32_t num_sge;
    TcpSGE sge[32]; /* Max SGEs */
} TcpWR;

/* Buffered data waiting for receive WR */
typedef struct {
    uint32_t src_node_id;
    uint32_t src_qpn;
    uint32_t length;
    void *data;
} TcpPendingData;

/* Forward declaration */
typedef struct TcpBackendPrivate TcpBackendPrivate;

/* Per-connection state */
typedef struct {
    uint32_t node_id;
    int sockfd;
    char *remote_host;
    uint16_t remote_port;
    QemuThread recv_thread;
    bool is_connected;
    bool recv_thread_running;
    QemuMutex lock;
    TcpBackendPrivate *priv;
} TcpConnection;

typedef struct {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;
    uint32_t qkey;
    uint32_t pd_handle;

    /* Connection info */
    uint32_t remote_node_id;
    uint32_t remote_qpn;
    union ibv_gid remote_gid;
    uint32_t rq_psn;
    uint32_t sq_psn;

    /* Remote connection info */
    uint64_t remote_addr;
    uint32_t remote_rkey;
    uint64_t local_addr;
    uint32_t local_rkey;

    /* Associated CQs */
    TcpCQ *scq;
    TcpCQ *rcq;

    /* Backend device reference */
    RdmaBackendDev *backend_dev;

    /* Work queues */
    GQueue *send_queue;
    GQueue *recv_queue;

    /* Pending data buffer for when no recv WR is available */
    GQueue *pending_data; /* Queue of buffered data payloads */

    QemuMutex lock;
} TcpQP;

/* Multi-node configuration modes */
typedef enum {
    TCP_MODE_LEGACY,
    TCP_MODE_MESH,
} TcpMode;

struct TcpBackendPrivate {
    /* Node identity */
    uint32_t local_node_id; /* This node's ID */
    TcpMode mode;           /* Operation mode */

    /* Multi-connection support */
    GHashTable *connections; /* node_id -> TcpConnection* */
    QemuMutex conn_table_lock;

    /* Legacy single connection (for backward compatibility) */
    TcpConnection *legacy_conn;

    /* Listen socket (for accepting connections) */
    int listen_fd;
    uint16_t listen_port;
    bool is_listening;
    QemuThread accept_thread;
    bool accept_thread_running;

    /* Resource tracking */
    GHashTable *pds; /* handle -> TcpPD */
    GHashTable *mrs; /* handle -> TcpMR */
    GHashTable *cqs; /* handle -> TcpCQ */
    GHashTable *qps; /* qpn -> TcpQP */

    /* Handle generators */
    uint32_t next_pd_handle;
    uint32_t next_mr_handle;
    uint32_t next_cq_handle;
    uint32_t next_qpn;

    /* QP pairing */
    GHashTable *qp_pairs; /* local_qpn -> remote_qpn */

    /* Sequence number for protocol */
    uint32_t next_seq;

    QemuMutex lock;
};

/*
 * Helper Functions
 */

static TcpBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    if (!backend_dev) {
        return NULL;
    }
    return (TcpBackendPrivate *)backend_dev->backend_private;
}

static void tcp_wr_map_sge(TcpQP *tqp, TcpWR *wr, struct ibv_sge *sge,
                           uint32_t num_sge)
{
    RdmaDeviceResources *res;
    if (!tqp || !wr || !tqp->backend_dev) {
        return;
    }

    res = tqp->backend_dev->rdma_dev_res;
    wr->num_sge = num_sge;
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        wr->sge[i].addr = (void *)(uintptr_t)sge[i].addr;
        wr->sge[i].length = sge[i].length;
        wr->sge[i].lkey = sge[i].lkey;
        wr->sge[i].host_addr = NULL;

        if (sge[i].length == 0) {
            continue;
        }

        RdmaRmMR *mr = rdma_rm_get_mr(res, sge[i].lkey);
        if (!mr) {
            rdma_error_report("TCP: Invalid lkey 0x%x for SGE %u",
                              sge[i].lkey, i);
            wr->sge[i].length = 0;
            continue;
        }

        uint64_t guest_addr = sge[i].addr;
        if (guest_addr < mr->start ||
            guest_addr + sge[i].length > mr->start + mr->length) {
            rdma_error_report(
                "TCP: SGE %u out of MR bounds addr=0x%lx len=%u mr=[0x%lx..0x%lx]",
                i, (unsigned long)guest_addr, sge[i].length,
                (unsigned long)mr->start,
                (unsigned long)(mr->start + mr->length));
            wr->sge[i].length = 0;
            continue;
        }

        wr->sge[i].host_addr =
            (char *)mr->virt + (guest_addr - mr->start);
    }
}

static void tcp_wr_unmap_sge(TcpQP *tqp, TcpWR *wr)
{
    /* Nothing to do: SGEs use MR host pointers, no explicit unmap needed. */
    (void)tqp;
    (void)wr;
}

static void tcp_connection_free(TcpConnection *conn)
{
    if (!conn) {
        return;
    }

    if (conn->recv_thread_running) {
        conn->recv_thread_running = false;
        qemu_thread_join(&conn->recv_thread);
    }

    if (conn->sockfd >= 0) {
        close(conn->sockfd);
    }

    g_free(conn->remote_host);
    qemu_mutex_destroy(&conn->lock);
    g_free(conn);
}

static TcpConnection *tcp_connection_new(uint32_t node_id, const char *host,
                                         uint16_t port)
{
    TcpConnection *conn = g_new0(TcpConnection, 1);
    conn->node_id = node_id;
    conn->sockfd = -1;
    conn->remote_host = g_strdup(host);
    conn->remote_port = port;
    conn->is_connected = false;
    conn->recv_thread_running = false;
    qemu_mutex_init(&conn->lock);
    return conn;
}

/* Get connection for a given node ID */
static TcpConnection *tcp_get_connection(TcpBackendPrivate *priv,
                                         uint32_t node_id)
{
    TcpConnection *conn;

    if (priv->mode == TCP_MODE_LEGACY) {
        return priv->legacy_conn;
    }

    qemu_mutex_lock(&priv->conn_table_lock);
    conn = g_hash_table_lookup(priv->connections, GUINT_TO_POINTER(node_id));
    qemu_mutex_unlock(&priv->conn_table_lock);

    return conn;
}

/* Parse legacy config: "host:port" or "listen:port" */
static int parse_tcp_config_legacy(const char *config, char **host,
                                   uint16_t *port, bool *listen_mode)
{
    char *config_copy, *saveptr, *token;
    char *host_str = NULL;
    uint16_t port_val = 0;
    bool listen = false;
    const char *parse_start = config;

    if (!config) {
        rdma_error_report("TCP backend requires config");
        return -EINVAL;
    }

    /* Handle both "tcp:listen:5000" and "listen:5000" formats */
    if (!strncmp(config, "tcp:", 4)) {
        parse_start = config + 4; /* Skip "tcp:" */
    }

    config_copy = g_strdup(parse_start);
    token = strtok_r(config_copy, ":", &saveptr);

    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP backend: missing host or 'listen'");
        return -EINVAL;
    }

    if (!strcmp(token, "listen")) {
        listen = true;
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) {
            g_free(config_copy);
            rdma_error_report("TCP backend: missing port for listen mode");
            return -EINVAL;
        }
        port_val = (uint16_t)atoi(token);
    } else {
        host_str = g_strdup(token);
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) {
            g_free(config_copy);
            g_free(host_str);
            rdma_error_report("TCP backend: missing port");
            return -EINVAL;
        }
        port_val = (uint16_t)atoi(token);
    }

    if (port_val == 0) {
        g_free(config_copy);
        g_free(host_str);
        rdma_error_report("TCP backend: invalid port");
        return -EINVAL;
    }

    *host = host_str;
    *port = port_val;
    *listen_mode = listen;
    g_free(config_copy);
    return 0;
}

/* Parse mesh config:
 * "node:<node_id>:<num_nodes>:<base_port>[:hostname_pattern]" */
static int parse_tcp_config_mesh(const char *config, uint32_t *local_node_id,
                                 uint32_t *num_nodes, uint16_t *base_port,
                                 char **hostname_pattern)
{
    char *config_copy, *saveptr, *token;
    const char *parse_start = config;
    uint32_t node_id, num;
    uint16_t port;
    char *pattern = NULL;

    if (!strncmp(config, "tcp:", 4)) {
        parse_start = config + 4;
    }

    if (strncmp(parse_start, "node:", 5) != 0) {
        rdma_error_report("TCP mesh config must start with 'node:'");
        return -EINVAL;
    }

    parse_start += 5; /* Skip "node:" */
    config_copy = g_strdup(parse_start);

    /* Parse node_id */
    token = strtok_r(config_copy, ":", &saveptr);
    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP mesh: missing node_id");
        return -EINVAL;
    }
    node_id = (uint32_t)atoi(token);

    /* Parse num_nodes */
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP mesh: missing num_nodes");
        return -EINVAL;
    }
    num = (uint32_t)atoi(token);

    /* Parse base_port */
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) {
        g_free(config_copy);
        rdma_error_report("TCP mesh: missing base_port");
        return -EINVAL;
    }
    port = (uint16_t)atoi(token);

    /* Optional hostname pattern */
    token = strtok_r(NULL, ":", &saveptr);
    if (token) {
        pattern = g_strdup(token);
    } else {
        /* Default: use localhost for all nodes */
        pattern = g_strdup("localhost");
    }

    if (node_id >= num) {
        g_free(config_copy);
        g_free(pattern);
        rdma_error_report("TCP mesh: node_id (%u) must be < num_nodes (%u)",
                          node_id, num);
        return -EINVAL;
    }

    if (port == 0) {
        g_free(config_copy);
        g_free(pattern);
        rdma_error_report("TCP mesh: invalid base_port");
        return -EINVAL;
    }

    *local_node_id = node_id;
    *num_nodes = num;
    *base_port = port;
    *hostname_pattern = pattern;

    g_free(config_copy);
    return 0;
}

static int tcp_connect_to_remote(const char *host, uint16_t port)
{
    struct sockaddr_in server_addr;
    struct hostent *server;
    int sockfd;
    int ret;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        rdma_error_report("TCP: Failed to create socket: %s", strerror(errno));
        return -1;
    }

    server = gethostbyname(host);
    if (!server) {
        rdma_error_report("TCP: Failed to resolve host '%s': %s", host,
                          hstrerror(h_errno));
        close(sockfd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);

    ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        rdma_error_report("TCP: Failed to connect to %s:%u: %s", host, port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    /* Set socket to non-blocking */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    rdma_info_report("TCP: Connected to %s:%u", host, port);
    return sockfd;
}

static int tcp_listen_on_port(uint16_t port)
{
    struct sockaddr_in server_addr;
    int sockfd;
    int ret;
    int opt = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
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

    ret = listen(sockfd, 8); /* Allow up to 8 pending connections */
    if (ret < 0) {
        rdma_error_report("TCP: Failed to listen on port %u: %s", port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    rdma_info_report("TCP: Listening on port %u", port);
    return sockfd;
}

static int tcp_send_message(int sockfd, TcpMsgType msg_type,
                            const void *payload, size_t payload_len,
                            uint32_t seq, uint32_t src_node, uint32_t dst_node,
                            uint32_t src_qpn, uint32_t dst_qpn)
{
    TcpMsgHeader hdr;
    ssize_t ret;
    size_t total_sent = 0;

    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(msg_type);
    hdr.msg_len = htonl(payload_len);
    hdr.seq = htonl(seq);
    hdr.src_node_id = htonl(src_node);
    hdr.dst_node_id = htonl(dst_node);
    hdr.src_qpn = htonl(src_qpn);
    hdr.dst_qpn = htonl(dst_qpn);

    /* Send header */
    ret = send(sockfd, &hdr, sizeof(hdr), MSG_NOSIGNAL);
    if (ret != sizeof(hdr)) {
        if (ret < 0) {
            rdma_error_report("TCP: Failed to send header: %s",
                              strerror(errno));
        } else {
            rdma_error_report("TCP: Partial header send");
        }
        return -1;
    }

    /* Send payload if any */
    if (payload && payload_len > 0) {
        while (total_sent < payload_len) {
            ret = send(sockfd, (const char *)payload + total_sent,
                       payload_len - total_sent, MSG_NOSIGNAL);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000); /* Wait 1ms and retry */
                    continue;
                }
                rdma_error_report("TCP: Failed to send payload: %s",
                                  strerror(errno));
                return -1;
            }
            total_sent += ret;
        }
    }

    return 0;
}

static int tcp_recv_message(int sockfd, TcpMsgHeader *hdr, void **payload)
{
    ssize_t ret;
    size_t total_recv = 0;

    /* Receive header */
    while (total_recv < sizeof(*hdr)) {
        ret = recv(sockfd, (char *)hdr + total_recv, sizeof(*hdr) - total_recv,
                   0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -EAGAIN;
            }
            rdma_error_report("TCP: Failed to receive header: %s",
                              strerror(errno));
            return -1;
        }
        if (ret == 0) {
            rdma_info_report("TCP: Connection closed by peer");
            return -1;
        }
        total_recv += ret;
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
    if (hdr->msg_len > 0) {
        *payload = g_malloc(hdr->msg_len);
        total_recv = 0;
        while (total_recv < hdr->msg_len) {
            ret = recv(sockfd, (char *)*payload + total_recv,
                       hdr->msg_len - total_recv, 0);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                rdma_error_report("TCP: Failed to receive payload: %s",
                                  strerror(errno));
                g_free(*payload);
                *payload = NULL;
                return -1;
            }
            if (ret == 0) {
                rdma_info_report("TCP: Connection closed during payload");
                g_free(*payload);
                *payload = NULL;
                return -1;
            }
            total_recv += ret;
        }
    } else {
        *payload = NULL;
    }

    return 0;
}

/* Per-connection receive thread */
static void *tcp_recv_thread_per_conn(void *opaque)
{
    TcpConnection *conn = (TcpConnection *)opaque;
    TcpMsgHeader hdr;
    void *payload = NULL;
    int ret;
    struct pollfd pfd;

    rdma_info_report("TCP: Receive thread started for node %u", conn->node_id);

    pfd.fd = conn->sockfd;
    pfd.events = POLLIN;

    while (conn->recv_thread_running) {
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
            ret = tcp_recv_message(conn->sockfd, &hdr, &payload);
            if (ret == -EAGAIN) {
                continue;
            }
            if (ret < 0) {
                rdma_info_report("TCP: Receive error on node %u, closing",
                                 conn->node_id);
                break;
            }

            /* Handle message based on type */
            switch (hdr.msg_type) {
            case TCP_MSG_HANDSHAKE:
                rdma_info_report("TCP: Received handshake from node %u",
                                 hdr.src_node_id);
                break;
                
            case TCP_MSG_POST_SEND: {
                /* Handle incoming send - match with posted receive and complete */
                rdma_info_report("TCP: Received POST_SEND from node %u, "
                                 "qpn %u->%u",
                                 hdr.src_node_id, hdr.src_qpn, hdr.dst_qpn);
                
                TcpBackendPrivate *priv = conn->priv;
                if (!priv) break;
                
                qemu_mutex_lock(&priv->lock);
                TcpQP *tqp = g_hash_table_lookup(priv->qps, 
                                                  GUINT_TO_POINTER(hdr.dst_qpn));
                if (tqp && tqp->rcq) {
                    /* Get the work request data */
                    TcpWR *send_wr = (TcpWR *)payload;
                    if (send_wr && hdr.msg_len >= sizeof(TcpWR)) {
                        /* Pop a receive WR if available */
                        TcpWR *recv_wr = g_queue_pop_head(tqp->recv_queue);
                        if (recv_wr) {
                            /* We'll get the actual data in the next DATA message
                             * For now, just mark that we're expecting data */
                            rdma_info_report("TCP: Matched send with recv, "
                                             "expecting data");
                            /* Store the receive WR for when data arrives */
                            g_queue_push_tail(tqp->recv_queue, recv_wr);
                        } else {
                            rdma_info_report("TCP: No receive posted for incoming send");
                        }
                    }
                }
                qemu_mutex_unlock(&priv->lock);
                break;
            }
            
            case TCP_MSG_DATA: {
                /* Handle incoming data - complete the receive */
                rdma_info_report("TCP: Received DATA from node %u, len %u, "
                                 "qpn %u->%u",
                                 hdr.src_node_id, hdr.msg_len,
                                 hdr.src_qpn, hdr.dst_qpn);
                
                TcpBackendPrivate *priv = conn->priv;
                if (!priv) break;
                
                qemu_mutex_lock(&priv->lock);
                TcpQP *tqp = g_hash_table_lookup(priv->qps,
                                                  GUINT_TO_POINTER(hdr.dst_qpn));
                if (tqp && tqp->rcq) {
                    /* Pop the receive WR */
                    TcpWR *recv_wr = g_queue_pop_head(tqp->recv_queue);
                    if (recv_wr && payload && hdr.msg_len > 0) {
                        /* Copy data to receive buffer */
                        uint32_t bytes_copied = 0;
                        for (uint32_t i = 0; i < recv_wr->num_sge && 
                                              bytes_copied < hdr.msg_len; i++) {
                            uint32_t to_copy = recv_wr->sge[i].length;
                            if (bytes_copied + to_copy > hdr.msg_len) {
                                to_copy = hdr.msg_len - bytes_copied;
                            }
                            void *host_buf = recv_wr->sge[i].host_addr;
                            if (host_buf && to_copy > 0) {
                                memcpy(host_buf, 
                                       (char *)payload + bytes_copied,
                                       to_copy);
                                bytes_copied += to_copy;
                            }
                        }
                        
                        rdma_info_report("TCP: Copied %u bytes to receive buffer",
                                         bytes_copied);
                        
                        /* Post completion directly to guest */
                        qemu_mutex_unlock(&priv->lock);
                        rdma_backend_complete_work(IBV_WC_SUCCESS, 0, bytes_copied,
                                                   hdr.dst_qpn, IBV_WC_RECV,
                                                   (void *)recv_wr->wr_id);
                        qemu_mutex_lock(&priv->lock);
                        
                        rdma_info_report("TCP: Delivered receive completion, "
                                         "wr_id=%lu, bytes=%u",
                                         recv_wr->wr_id, bytes_copied);
                        
                        tcp_wr_unmap_sge(tqp, recv_wr);
                        g_free(recv_wr);
                        
                        /* Send completion ACK back to sender */
                        TcpConnection *src_conn = tcp_get_connection(priv, 
                                                                      hdr.src_node_id);
                        if (src_conn && src_conn->is_connected) {
                            qemu_mutex_lock(&src_conn->lock);
                            if (src_conn->sockfd >= 0) {
                                tcp_send_message(src_conn->sockfd, 
                                                 TCP_MSG_COMPLETION, NULL, 0,
                                                 hdr.seq, priv->local_node_id,
                                                 hdr.src_node_id, hdr.dst_qpn,
                                                 hdr.src_qpn);
                                rdma_info_report("TCP: Sent completion ACK to node %u",
                                                 hdr.src_node_id);
                            }
                            qemu_mutex_unlock(&src_conn->lock);
                        }
                    } else if (payload && hdr.msg_len > 0) {
                        /* No recv WR available - buffer the data */
                        rdma_info_report("TCP: No recv WR available, buffering %u bytes",
                                         hdr.msg_len);
                        TcpPendingData *pending = g_new0(TcpPendingData, 1);
                        pending->src_node_id = hdr.src_node_id;
                        pending->src_qpn = hdr.src_qpn;
                        pending->length = hdr.msg_len;
                        pending->data = g_malloc(hdr.msg_len);
                        memcpy(pending->data, payload, hdr.msg_len);
                        g_queue_push_tail(tqp->pending_data, pending);
                    } else {
                        rdma_info_report("TCP: No receive WR and no valid data");
                    }
                }
                qemu_mutex_unlock(&priv->lock);
                break;
            }
            
            case TCP_MSG_COMPLETION: {
                /* Handle completion ACK from receiver */
                rdma_info_report("TCP: Received COMPLETION from node %u for qpn %u",
                                 hdr.src_node_id, hdr.dst_qpn);
                
                TcpBackendPrivate *priv = conn->priv;
                if (!priv) break;
                
                qemu_mutex_lock(&priv->lock);
                TcpQP *tqp = g_hash_table_lookup(priv->qps,
                                                  GUINT_TO_POINTER(hdr.dst_qpn));
                if (tqp && tqp->scq) {
                    /* Pop the send WR and post completion directly to guest */
                    TcpWR *send_wr = g_queue_pop_head(tqp->send_queue);
                    if (send_wr) {
                        uint64_t wr_id = send_wr->wr_id;
                        g_free(send_wr);
                        
                        qemu_mutex_unlock(&priv->lock);
                        rdma_backend_complete_work(IBV_WC_SUCCESS, 0, 0,
                                                   hdr.dst_qpn, IBV_WC_SEND,
                                                   (void *)wr_id);
                        qemu_mutex_lock(&priv->lock);
                        
                        rdma_info_report("TCP: Delivered send completion, wr_id=%lu",
                                         wr_id);
                    }
                }
                qemu_mutex_unlock(&priv->lock);
                break;
            }
            
            case TCP_MSG_POST_RECV:
            case TCP_MSG_REMOTE_CONN_INFO_RESP:
                rdma_info_report("TCP: Received msg type %u from node %u",
                                 hdr.msg_type, hdr.src_node_id);
                break;
            default:
                rdma_warn_report("TCP: Unknown message type %u from node %u",
                                 hdr.msg_type, hdr.src_node_id);
                break;
            }

            if (payload) {
                g_free(payload);
                payload = NULL;
            }
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            rdma_info_report("TCP: Connection to node %u closed or error",
                             conn->node_id);
            break;
        }
    }

    rdma_info_report("TCP: Receive thread exiting for node %u", conn->node_id);
    return NULL;
}

/* Send handshake to remote node */
static int tcp_send_handshake(TcpConnection *conn, uint32_t local_node_id,
                              TcpMsgType msg_type)
{
    TcpHandshakePayload payload;
    payload.node_id = htonl(local_node_id);
    payload.version = htonl(TCP_PROTOCOL_VERSION);

    return tcp_send_message(conn->sockfd, msg_type, &payload, sizeof(payload),
                            1, local_node_id, conn->node_id, 0, 0);
}

/* Accept thread - accepts incoming connections and adds them to table */
static void *tcp_accept_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    int sockfd;
    int flags;
    TcpMsgHeader hdr;
    void *payload = NULL;
    TcpHandshakePayload *hs_payload;
    TcpConnection *conn;
    char thread_name[32];

    rdma_info_report("TCP: Accept thread started");

    while (priv->accept_thread_running) {
        client_len = sizeof(client_addr);
        sockfd = accept(priv->listen_fd, (struct sockaddr *)&client_addr,
                        &client_len);

        if (sockfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); /* 100ms */
                continue;
            }
            if (!priv->accept_thread_running) {
                break;
            }
            rdma_error_report("TCP: Failed to accept connection: %s",
                              strerror(errno));
            continue;
        }

        /* Set socket to non-blocking */
        flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

        rdma_info_report("TCP: Accepted connection from %s:%u",
                         inet_ntoa(client_addr.sin_addr),
                         ntohs(client_addr.sin_port));

        /* Receive handshake to identify remote node (with retry for non-blocking) */
        int retry_count = 0;
        int max_retries = 50; /* 5 seconds with 100ms sleep */
        int recv_ret;
        
        while (retry_count < max_retries) {
            recv_ret = tcp_recv_message(sockfd, &hdr, &payload);
            if (recv_ret == 0) {
                /* Message received successfully */
                if (hdr.msg_type == TCP_MSG_HANDSHAKE) {
                    /* Success! */
                    break;
                } else {
                    rdma_error_report("TCP: Expected handshake, got message type %u",
                                      hdr.msg_type);
                    close(sockfd);
                    if (payload) {
                        g_free(payload);
                    }
                    goto next_connection;
                }
            } else if (recv_ret == -EAGAIN) {
                /* Socket would block, retry */
                usleep(100000); /* 100ms */
                retry_count++;
                continue;
            } else {
                /* Real error */
                rdma_error_report("TCP: Error receiving handshake from incoming connection");
                close(sockfd);
                if (payload) {
                    g_free(payload);
                }
                goto next_connection;
            }
        }
        
        if (retry_count >= max_retries) {
            rdma_error_report("TCP: Timeout waiting for handshake from incoming connection");
            close(sockfd);
            if (payload) {
                g_free(payload);
            }
            continue;
        }

        hs_payload = (TcpHandshakePayload *)payload;
        uint32_t remote_node_id = ntohl(hs_payload->node_id);

        rdma_info_report("TCP: Handshake from node %u", remote_node_id);

        /* Create connection object */
        conn = g_new0(TcpConnection, 1);
        conn->node_id = remote_node_id;
        conn->sockfd = sockfd;
        conn->remote_host = g_strdup(inet_ntoa(client_addr.sin_addr));
        conn->remote_port = ntohs(client_addr.sin_port);
        conn->is_connected = true;
        conn->priv = priv;
        qemu_mutex_init(&conn->lock);

        /* Add to connection table */
        qemu_mutex_lock(&priv->conn_table_lock);
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(remote_node_id),
                            conn);
        qemu_mutex_unlock(&priv->conn_table_lock);

        /* Send handshake response */
        tcp_send_handshake(conn, priv->local_node_id, TCP_MSG_HANDSHAKE_RESP);

        /* Start receive thread for this connection */
        snprintf(thread_name, sizeof(thread_name), "tcp-recv-%u",
                 remote_node_id);
        conn->recv_thread_running = true;
        qemu_thread_create(&conn->recv_thread, thread_name,
                           tcp_recv_thread_per_conn, conn,
                           QEMU_THREAD_JOINABLE);

        g_free(payload);
        payload = NULL;
        
next_connection:
        continue;
    }

    rdma_info_report("TCP: Accept thread exiting");
    return NULL;
}

/* Connect to peer nodes (for symmetric mesh topology) */
static int tcp_connect_to_peers(TcpBackendPrivate *priv, uint32_t num_nodes,
                                uint16_t base_port,
                                const char *hostname_pattern)
{
    char hostname[256];
    char thread_name[32];

    /* Only connect to nodes with lower IDs (prevent duplicate connections) */
    for (uint32_t peer_id = 0; peer_id < priv->local_node_id; peer_id++) {
        /* Generate hostname for this peer */
        if (strchr(hostname_pattern, '%')) {
            /* Pattern contains format specifier like "node%u.local" */
            snprintf(hostname, sizeof(hostname), hostname_pattern, peer_id);
        } else {
            /* Use pattern as-is (e.g., "localhost") */
            strncpy(hostname, hostname_pattern, sizeof(hostname) - 1);
        }

        uint16_t peer_port = base_port + peer_id;

        rdma_info_report("TCP: Connecting to node %u at %s:%u", peer_id,
                         hostname, peer_port);

        TcpConnection *conn = tcp_connection_new(peer_id, hostname, peer_port);
        conn->priv = priv;
        conn->sockfd = tcp_connect_to_remote(hostname, peer_port);

        if (conn->sockfd < 0) {
            rdma_error_report("TCP: Failed to connect to node %u", peer_id);
            tcp_connection_free(conn);
            return -1;
        }

        conn->is_connected = true;

        /* Send handshake */
        if (tcp_send_handshake(conn, priv->local_node_id, TCP_MSG_HANDSHAKE) <
            0) {
            rdma_error_report("TCP: Failed to send handshake to node %u",
                              peer_id);
            tcp_connection_free(conn);
            return -1;
        }

        /* Wait for handshake response with retry loop */
        TcpMsgHeader hdr;
        void *payload = NULL;
        int retry_count = 0;
        int max_retries = 50; /* 5 seconds with 100ms sleep */
        int ret;

        while (retry_count < max_retries) {
            ret = tcp_recv_message(conn->sockfd, &hdr, &payload);
            if (ret == 0) {
                /* Message received successfully */
                if (hdr.msg_type == TCP_MSG_HANDSHAKE_RESP) {
                    /* Success! */
                    break;
                } else {
                    rdma_error_report(
                        "TCP: Unexpected message type %u from node %u",
                        hdr.msg_type, peer_id);
                    if (payload) {
                        g_free(payload);
                    }
                    tcp_connection_free(conn);
                    return -1;
                }
            } else if (ret == -EAGAIN) {
                /* Socket would block, retry */
                usleep(100000); /* 100ms */
                retry_count++;
                continue;
            } else {
                /* Real error */
                rdma_error_report(
                    "TCP: Error receiving handshake response from "
                    "node %u",
                    peer_id);
                tcp_connection_free(conn);
                return -1;
            }
        }

        if (retry_count >= max_retries) {
            rdma_error_report(
                "TCP: Timeout waiting for handshake response from "
                "node %u",
                peer_id);
            tcp_connection_free(conn);
            return -1;
        }

        if (payload) {
            g_free(payload);
        }

        /* Add to connection table */
        qemu_mutex_lock(&priv->conn_table_lock);
        g_hash_table_insert(priv->connections, GUINT_TO_POINTER(peer_id), conn);
        qemu_mutex_unlock(&priv->conn_table_lock);

        /* Start receive thread */
        snprintf(thread_name, sizeof(thread_name), "tcp-recv-%u", peer_id);
        conn->recv_thread_running = true;
        qemu_thread_create(&conn->recv_thread, thread_name,
                           tcp_recv_thread_per_conn, conn,
                           QEMU_THREAD_JOINABLE);

        rdma_info_report("TCP: Connected to node %u", peer_id);
    }

    return 0;
}

/*
 * Backend Lifecycle
 */

static int tcp_init(RdmaBackendDev *backend_dev, const char *config)
{
    TcpBackendPrivate *priv;
    int ret;

    rdma_info_report("TCP backend: Initializing");

    priv = g_new0(TcpBackendPrivate, 1);

    priv->pds =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->mrs =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    priv->cqs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_free);
    priv->qps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_free);
    priv->qp_pairs = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->connections =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              (GDestroyNotify)tcp_connection_free);

    priv->next_pd_handle = 1;
    priv->next_mr_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100;
    priv->next_seq = 1;

    priv->listen_fd = -1;
    priv->is_listening = false;
    priv->accept_thread_running = false;

    qemu_mutex_init(&priv->lock);
    qemu_mutex_init(&priv->conn_table_lock);

    /* Default mesh metadata */
    backend_dev->mesh_enabled = false;
    backend_dev->mesh_node_id = 0;
    backend_dev->mesh_num_nodes = 0;

    /* Determine mode from config string */
    if (strstr(config, "node:")) {
        /* Mesh mode: tcp:node:<id>:<num>:<port>[:pattern] */
        uint32_t num_nodes;
        uint16_t base_port;
        char *hostname_pattern = NULL;

        priv->mode = TCP_MODE_MESH;

        ret = parse_tcp_config_mesh(config, &priv->local_node_id, &num_nodes,
                                    &base_port, &hostname_pattern);
        if (ret < 0) {
            goto error;
        }

        backend_dev->mesh_enabled = true;
        backend_dev->mesh_node_id = (uint8_t)priv->local_node_id;
        backend_dev->mesh_num_nodes = (uint8_t)num_nodes;

        rdma_info_report("TCP backend: Mesh mode - node %u of %u",
                         priv->local_node_id, num_nodes);

        /* Start listening on our port */
        priv->listen_port = base_port + priv->local_node_id;
        priv->listen_fd = tcp_listen_on_port(priv->listen_port);
        if (priv->listen_fd < 0) {
            g_free(hostname_pattern);
            goto error;
        }
        priv->is_listening = true;

        /* Start accept thread */
        priv->accept_thread_running = true;
        qemu_thread_create(&priv->accept_thread, "tcp-accept",
                           tcp_accept_thread, priv, QEMU_THREAD_JOINABLE);

        /* Connect to peer nodes with lower IDs */
        ret =
            tcp_connect_to_peers(priv, num_nodes, base_port, hostname_pattern);
        g_free(hostname_pattern);
        if (ret < 0) {
            goto error;
        }

        rdma_info_report("TCP backend: Mesh initialized - %u connections",
                         g_hash_table_size(priv->connections));

    } else {
        /* Legacy mode: tcp:host:port or tcp:listen:port */
        char *host = NULL;
        uint16_t port = 0;
        bool listen_mode = false;

        priv->mode = TCP_MODE_LEGACY;
        priv->local_node_id = 0;
        backend_dev->mesh_enabled = false;
        backend_dev->mesh_node_id = 0;
        backend_dev->mesh_num_nodes = 0;

        ret = parse_tcp_config_legacy(config, &host, &port, &listen_mode);
        if (ret < 0) {
            goto error;
        }

        TcpConnection *conn = tcp_connection_new(1, host, port);
        conn->priv = priv;
        priv->legacy_conn = conn;

        if (listen_mode) {
            priv->listen_fd = tcp_listen_on_port(port);
            if (priv->listen_fd < 0) {
                g_free(host);
                goto error;
            }
            priv->is_listening = true;
            conn->is_connected = false;
        } else {
            conn->sockfd = tcp_connect_to_remote(host, port);
            if (conn->sockfd < 0) {
                g_free(host);
                goto error;
            }
            conn->is_connected = true;

            /* Start receive thread for legacy connection */
            conn->recv_thread_running = true;
            qemu_thread_create(&conn->recv_thread, "tcp-recv",
                               tcp_recv_thread_per_conn, conn,
                               QEMU_THREAD_JOINABLE);
        }

        g_free(host);
        rdma_info_report("TCP backend: Legacy mode (%s)",
                         listen_mode ? "listen" : "connect");
    }

    backend_dev->backend_private = priv;
    return 0;

error:
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
    }
    g_hash_table_destroy(priv->pds);
    g_hash_table_destroy(priv->mrs);
    g_hash_table_destroy(priv->cqs);
    g_hash_table_destroy(priv->qps);
    g_hash_table_destroy(priv->qp_pairs);
    g_hash_table_destroy(priv->connections);
    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_table_lock);
    g_free(priv);
    return -1;
}

static void tcp_fini(RdmaBackendDev *backend_dev)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv) {
        return;
    }

    rdma_info_report("TCP backend: Cleaning up");

    /* Stop accept thread */
    if (priv->accept_thread_running) {
        priv->accept_thread_running = false;
        qemu_thread_join(&priv->accept_thread);
    }

    /* Close listen socket */
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
        priv->listen_fd = -1;
    }

    /* Clean up all connections (threads are stopped in tcp_connection_free) */
    g_hash_table_destroy(priv->connections);

    /* Clean up legacy connection */
    if (priv->legacy_conn) {
        tcp_connection_free(priv->legacy_conn);
    }

    g_hash_table_destroy(priv->pds);
    g_hash_table_destroy(priv->mrs);
    g_hash_table_destroy(priv->cqs);
    g_hash_table_destroy(priv->qps);
    g_hash_table_destroy(priv->qp_pairs);

    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_table_lock);

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
    attr->active_mtu = IBV_MTU_1024;
    /* Allow multiple GIDs so the driver can expose per-node mesh GIDs */
    attr->gid_tbl_len = MAX_PORT_GIDS;
    attr->port_cap_flags = IBV_PORT_CM_SUP;
    attr->max_msg_sz = 0x80000000;
    attr->pkey_tbl_len = 1;
    attr->active_width = 4; /* 4X */
    attr->active_speed = 4; /* 10 Gbps */
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
    attr->max_mr = 1024;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    return 0;
}

/*
 * Protection Domain Operations
 */

static int tcp_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpPD *tpd = g_new0(TcpPD, 1);

    qemu_mutex_lock(&priv->lock);
    tpd->handle = priv->next_pd_handle++;
    g_hash_table_insert(priv->pds, GUINT_TO_POINTER(tpd->handle), tpd);
    qemu_mutex_unlock(&priv->lock);

    pd->ibpd = (struct ibv_pd *)(uintptr_t)tpd->handle;

    rdma_info_report("TCP: Created PD handle %u", tpd->handle);
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
    TcpMR *tmr = g_new0(TcpMR, 1);
    uint32_t pd_handle = (uint32_t)(uintptr_t)pd->ibpd;

    /* Find backend_dev from a global registry or use static */
    static uint32_t mr_counter = 1;
    static GHashTable *global_mrs = NULL;
    if (!global_mrs) {
        global_mrs =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }

    tmr->handle = mr_counter++;
    tmr->virt = addr;
    tmr->length = length;
    tmr->guest_start = guest_start;
    tmr->access_flags = access;
    tmr->lkey = tmr->handle;
    tmr->rkey = tmr->handle + 0x10000;
    tmr->pd_handle = pd_handle;

    g_hash_table_insert(global_mrs, GUINT_TO_POINTER(tmr->handle), tmr);

    mr->ibpd = pd->ibpd;
    mr->ibmr = (struct ibv_mr *)(uintptr_t)tmr->handle;

    rdma_info_report("TCP: Created MR handle %u, lkey=0x%x, rkey=0x%x, "
                     "len=%zu",
                     tmr->handle, tmr->lkey, tmr->rkey, length);
    return 0;
}

static void tcp_destroy_mr(RdmaBackendMR *mr)
{
    rdma_info_report("TCP: Destroyed MR");
}

static uint32_t tcp_mr_lkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    return handle; /* lkey = handle */
}

static uint32_t tcp_mr_rkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    return handle + 0x10000; /* rkey = handle + offset */
}

/*
 * Completion Queue Operations
 */

static int tcp_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                         int cqe)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    TcpCQ *tcq = g_new0(TcpCQ, 1);

    qemu_mutex_lock(&priv->lock);
    tcq->handle = priv->next_cq_handle++;
    tcq->cqe = cqe;
    tcq->completions = g_queue_new();
    qemu_mutex_init(&tcq->lock);
    g_hash_table_insert(priv->cqs, GUINT_TO_POINTER(tcq->handle), tcq);
    qemu_mutex_unlock(&priv->lock);

    cq->backend_dev = backend_dev;
    cq->ibcq = (struct ibv_cq *)(uintptr_t)tcq->handle;

    rdma_info_report("TCP: Created CQ handle %u, cqe=%d", tcq->handle, cqe);
    return 0;
}

static void tcp_destroy_cq(RdmaBackendCQ *cq)
{
    TcpBackendPrivate *priv = get_private(cq->backend_dev);
    uint32_t handle = (uint32_t)(uintptr_t)cq->ibcq;
    TcpCQ *tcq;

    if (!priv) {
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tcq = g_hash_table_lookup(priv->cqs, GUINT_TO_POINTER(handle));
    if (tcq) {
        /* Free all completions */
        while (!g_queue_is_empty(tcq->completions)) {
            TcpCompletion *comp = g_queue_pop_head(tcq->completions);
            g_free(comp);
        }
        g_queue_free(tcq->completions);
        qemu_mutex_destroy(&tcq->lock);
        /* Note: g_hash_table_remove will automatically free tcq via g_free
         * since the hash table was created with g_free as the value_destroy_func */
        g_hash_table_remove(priv->cqs, GUINT_TO_POINTER(handle));
    }
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("TCP: Destroyed CQ handle %u", handle);
}

static void tcp_poll_cq(RdmaDeviceResources *rdma_dev_res, RdmaBackendCQ *cq)
{
    TcpBackendPrivate *priv = get_private(cq->backend_dev);
    uint32_t handle = (uint32_t)(uintptr_t)cq->ibcq;
    TcpCQ *tcq;
    TcpCompletion *comp;

    if (!priv) {
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tcq = g_hash_table_lookup(priv->cqs, GUINT_TO_POINTER(handle));
    if (!tcq) {
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    qemu_mutex_lock(&tcq->lock);
    comp = g_queue_pop_head(tcq->completions);
    qemu_mutex_unlock(&tcq->lock);
    qemu_mutex_unlock(&priv->lock);

    if (comp) {
        /* Post completion to device */
        rdma_backend_complete_work(comp->status, 0, comp->byte_len,
                                   comp->qp_num, comp->opcode,
                                   (void *)(uintptr_t)comp->wr_id);
        g_free(comp);
    }
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
    TcpQP *tqp = g_new0(TcpQP, 1);
    uint32_t scq_handle = (uint32_t)(uintptr_t)scq->ibcq;
    uint32_t rcq_handle = (uint32_t)(uintptr_t)rcq->ibcq;

    qemu_mutex_lock(&priv->lock);
    tqp->qpn = priv->next_qpn++;
    tqp->qp_type = qp_type;
    tqp->state = IBV_QPS_RESET;
    tqp->pd_handle = (uint32_t)(uintptr_t)pd->ibpd;
    tqp->remote_node_id = 1; /* Default to node 1 in legacy mode */
    tqp->send_queue = g_queue_new();
    tqp->recv_queue = g_queue_new();
    tqp->pending_data = g_queue_new();
    qemu_mutex_init(&tqp->lock);

    /* Look up CQs */
    tqp->scq = g_hash_table_lookup(priv->cqs, GUINT_TO_POINTER(scq_handle));
    tqp->rcq = g_hash_table_lookup(priv->cqs, GUINT_TO_POINTER(rcq_handle));
    tqp->backend_dev = scq->backend_dev;

    g_hash_table_insert(priv->qps, GUINT_TO_POINTER(tqp->qpn), tqp);
    qemu_mutex_unlock(&priv->lock);

    qp->ibpd = pd->ibpd;
    qp->ibqp = (struct ibv_qp *)(uintptr_t)tqp->qpn;

    rdma_info_report("TCP: Created QP qpn=%u, type=%u", tqp->qpn, qp_type);
    return 0;
}

static void tcp_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    /* Simplified implementation - QP cleanup handled by resource manager */
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
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    TcpQP *tqp;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (tqp) {
        tqp->state = IBV_QPS_INIT;
        tqp->qkey = qkey;
    }
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("TCP: QP %u -> INIT", qpn);
    return 0;
}

static int tcp_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                            uint8_t qp_type, uint8_t sgid_idx,
                            union ibv_gid *dgid, uint32_t dqpn, uint32_t rq_psn,
                            uint32_t qkey, bool qkey_set)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    TcpQP *tqp;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (tqp) {
        tqp->state = IBV_QPS_RTR;
        tqp->remote_qpn = dqpn;
        if (dgid) {
            memcpy(&tqp->remote_gid, dgid, sizeof(*dgid));
            /* Extract node ID from GID if in mesh mode */
            /* For now, use a simple encoding: node_id in last byte */
            if (priv->mode == TCP_MODE_MESH) {
                tqp->remote_node_id = dgid->raw[15];
            }
        }
        tqp->rq_psn = rq_psn;
        if (qkey_set) {
            tqp->qkey = qkey;
        }
    }
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("TCP: QP %u -> RTR (remote qpn=%u, node=%u)", qpn, dqpn,
                     tqp ? tqp->remote_node_id : 0);
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

static void tcp_query_remote_conn_info(RdmaBackendQP *qp, uint64_t *remote_addr,
                                       uint32_t *rkey)
{
    (void)qp; /* QP info would be queried via TCP protocol */

    if (remote_addr) {
        *remote_addr = 0;
    }
    if (rkey) {
        *rkey = 0;
    }

    /* Query remote connection info via TCP protocol */
    rdma_info_report("TCP: Query remote conn info");
}

/*
 * Data Path Operations
 */

static void tcp_post_send(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                          uint8_t qp_type, struct ibv_sge *sge,
                          uint32_t num_sge, uint8_t sgid_idx,
                          union ibv_gid *sgid, union ibv_gid *dgid,
                          uint32_t dqpn, uint32_t dqkey, void *ctx)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    TcpQP *tqp;
    TcpWR *wr;
    TcpConnection *conn;
    uint32_t seq;
    int ret;

    rdma_info_report("TCP: >>> post_send CALLED for QPN %u", qpn);

    if (!priv) {
        rdma_error_report("TCP: Invalid backend");
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (!tqp) {
        qemu_mutex_unlock(&priv->lock);
        rdma_error_report("TCP: QP %u not found", qpn);
        return;
    }

    /* Get remote node ID and connection */
    uint32_t dst_node = tqp->remote_node_id;

    wr = g_new0(TcpWR, 1);
    wr->wr_id = (uint64_t)(uintptr_t)ctx;
    wr->num_sge = 0;
    tcp_wr_map_sge(tqp, wr, sge, num_sge);
    g_queue_push_tail(tqp->send_queue, wr);
    seq = priv->next_seq++;
    qemu_mutex_unlock(&priv->lock);

    /* Get connection for destination node */
    conn = tcp_get_connection(priv, dst_node);
    if (!conn || !conn->is_connected) {
        rdma_error_report("TCP: No connection to node %u", dst_node);
        return;
    }

    /* Send POST_SEND message over TCP */
    qemu_mutex_lock(&conn->lock);
    if (conn->sockfd >= 0) {
        /* Send work request */
        ret = tcp_send_message(conn->sockfd, TCP_MSG_POST_SEND, wr,
                               sizeof(*wr), seq,
                               priv->local_node_id, dst_node, qpn,
                               tqp->remote_qpn);
        if (ret < 0) {
            rdma_error_report("TCP: Failed to send POST_SEND to node %u",
                              dst_node);
        }

        /* Send data payloads */
        for (uint32_t i = 0; i < num_sge; i++) {
            void *host_buf = (i < 32) ? wr->sge[i].host_addr : NULL;
            uint32_t len = (i < 32) ? wr->sge[i].length : 0;
            if (host_buf && len > 0) {
                ret = tcp_send_message(conn->sockfd, TCP_MSG_DATA,
                                       host_buf, len, seq, priv->local_node_id,
                                       dst_node, qpn, tqp->remote_qpn);
                if (ret < 0) {
                    rdma_error_report("TCP: Failed to send data to node %u",
                                      dst_node);
                }
            }
        }
    }
    qemu_mutex_unlock(&conn->lock);

    /* Done with mapped send buffers */
    tcp_wr_unmap_sge(tqp, wr);

    rdma_info_report("TCP: Posted send QP %u -> node %u, %u SGEs", qpn,
                     dst_node, num_sge);
}

static void tcp_post_recv(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                          uint8_t qp_type, struct ibv_sge *sge,
                          uint32_t num_sge, void *ctx)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    TcpQP *tqp;
    TcpWR *wr;
    TcpConnection *conn;
    uint32_t seq = 0;

    rdma_info_report("TCP: >>> post_recv CALLED for QPN %u", qpn);

    if (!priv) {
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (!tqp) {
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    uint32_t dst_node = tqp->remote_node_id;

    wr = g_new0(TcpWR, 1);
    wr->wr_id = (uint64_t)(uintptr_t)ctx;
    wr->num_sge = 0;
    tcp_wr_map_sge(tqp, wr, sge, num_sge);
    
    /* Check if there's pending data waiting for this recv WR */
    TcpPendingData *pending = g_queue_pop_head(tqp->pending_data);
    if (pending) {
        /* We have buffered data - process it immediately */
        rdma_info_report("TCP: Processing buffered data (%u bytes) for new recv WR",
                         pending->length);
        
        uint32_t bytes_copied = 0;
        for (uint32_t i = 0; i < num_sge && bytes_copied < pending->length; i++) {
            uint32_t to_copy = sge[i].length;
            if (bytes_copied + to_copy > pending->length) {
                to_copy = pending->length - bytes_copied;
            }
            void *host_buf = wr->sge[i].host_addr;
            if (host_buf && to_copy > 0) {
                memcpy(host_buf, 
                       (char *)pending->data + bytes_copied,
                       to_copy);
                bytes_copied += to_copy;
            }
        }
        
        /* Post completion directly */
        qemu_mutex_unlock(&priv->lock);
        rdma_backend_complete_work(IBV_WC_SUCCESS, 0, bytes_copied,
                                   qpn, IBV_WC_RECV, ctx);
        qemu_mutex_lock(&priv->lock);
        
        rdma_info_report("TCP: Completed buffered recv, wr_id=%lu, bytes=%u",
                         (uint64_t)(uintptr_t)ctx, bytes_copied);
        
        tcp_wr_unmap_sge(tqp, wr);
        g_free(wr);

        /* Send completion ACK */
        TcpConnection *src_conn = tcp_get_connection(priv, pending->src_node_id);
        if (src_conn && src_conn->is_connected) {
            qemu_mutex_lock(&src_conn->lock);
            if (src_conn->sockfd >= 0) {
                tcp_send_message(src_conn->sockfd, TCP_MSG_COMPLETION, NULL, 0,
                                 seq, priv->local_node_id, pending->src_node_id,
                                 qpn, pending->src_qpn);
            }
            qemu_mutex_unlock(&src_conn->lock);
        }
        
        g_free(pending->data);
        g_free(pending);
        g_free(wr);
        qemu_mutex_unlock(&priv->lock);
        
        rdma_info_report("TCP: Posted recv (used buffered data) to QP %u", qpn);
        return;
    }
    
    /* No pending data - queue the WR normally */
    g_queue_push_tail(tqp->recv_queue, wr);
    seq = priv->next_seq++;
    qemu_mutex_unlock(&priv->lock);

    /* Get connection */
    conn = tcp_get_connection(priv, dst_node);
    if (!conn || !conn->is_connected) {
        return;
    }

    /* Send POST_RECV message over TCP */
    qemu_mutex_lock(&conn->lock);
    if (conn->sockfd >= 0) {
        tcp_send_message(conn->sockfd, TCP_MSG_POST_RECV, wr,
                         sizeof(*wr), seq,
                         priv->local_node_id, dst_node, qpn, tqp->remote_qpn);
    }
    qemu_mutex_unlock(&conn->lock);

    rdma_info_report("TCP: Posted recv to QP %u, %u SGEs", qpn, num_sge);
}

/*
 * GID Management
 */

static int tcp_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       union ibv_gid *gid)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    /* In mesh mode, encode local node ID in GID */
    if (priv && priv->mode == TCP_MODE_MESH) {
        memset(gid, 0, sizeof(*gid));
        gid->raw[15] = (uint8_t)priv->local_node_id;
        rdma_info_report("TCP: Added GID with node_id=%u", priv->local_node_id);
    } else {
        rdma_info_report("TCP: Added GID");
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
    .poll_cq = tcp_poll_cq,

    .create_qp = tcp_create_qp,
    .destroy_qp = tcp_destroy_qp,
    .qpn = tcp_qpn,

    .qp_state_init = tcp_qp_state_init,
    .qp_state_rtr = tcp_qp_state_rtr,
    .qp_state_rts = tcp_qp_state_rts,
    .query_qp = tcp_query_qp,
    .query_remote_conn_info = tcp_query_remote_conn_info,

    .post_send = tcp_post_send,
    .post_recv = tcp_post_recv,

    .add_gid = tcp_add_gid,
    .del_gid = tcp_del_gid,
    .get_backend_gid_index = tcp_get_backend_gid_index,

    /* SRQ operations not supported */
    .create_srq = NULL,
    .destroy_srq = NULL,
    .query_srq = NULL,
    .modify_srq = NULL,
    .post_srq_recv = NULL,
};
