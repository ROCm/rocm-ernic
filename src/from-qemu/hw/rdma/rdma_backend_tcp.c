/*
 * RDMA Backend: TCP/IP Network
 *
 * TCP/IP backend for connecting two rocm_ernic server instances over network.
 * Implements RDMA operations over TCP socket with custom protocol.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_backend.h"
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
 * TCP Backend Protocol
 *
 * Messages are sent over TCP with a fixed header:
 *   uint32_t magic;      // Protocol magic (0x52444D41 = "RDMA")
 *   uint32_t msg_type;  // Message type
 *   uint32_t msg_len;   // Payload length
 *   uint32_t seq;       // Sequence number
 *   uint8_t  payload[]; // Variable length payload
 */

#define TCP_PROTOCOL_MAGIC 0x52444D41 /* "RDMA" */

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
} __attribute__((packed)) TcpMsgHeader;

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
    uint32_t length;
    uint32_t lkey;
} TcpSGE;

typedef struct {
    uint64_t wr_id;
    uint32_t num_sge;
    TcpSGE sge[32]; /* Max SGEs */
} TcpWR;

typedef struct {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;
    uint32_t qkey;
    uint32_t pd_handle;

    /* Connection info */
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

    QemuMutex lock;
} TcpQP;

typedef struct {
    /* TCP connection */
    int sockfd;
    char *remote_host;
    uint16_t remote_port;
    bool is_connected;
    bool is_listening;
    int listen_fd;
    QemuMutex conn_lock;

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

    /* Thread for handling incoming messages */
    QemuThread recv_thread;
    bool recv_thread_running;

    QemuMutex lock;
} TcpBackendPrivate;

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

static int parse_tcp_config(const char *config, char **host, uint16_t *port,
                            bool *listen_mode)
{
    char *config_copy, *saveptr, *token;
    char *host_str = NULL;
    uint16_t port_val = 0;
    bool listen = false;
    const char *parse_start = config;

    if (!config) {
        rdma_error_report("TCP backend requires config: tcp:host:port or "
                          "tcp:listen:port");
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

    ret = listen(sockfd, 1);
    if (ret < 0) {
        rdma_error_report("TCP: Failed to listen on port %u: %s", port,
                          strerror(errno));
        close(sockfd);
        return -1;
    }

    rdma_info_report("TCP: Listening on port %u", port);
    return sockfd;
}

__attribute__((unused)) static int tcp_accept_connection(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int sockfd;
    int flags;

    sockfd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (sockfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            rdma_error_report("TCP: Failed to accept connection: %s",
                              strerror(errno));
        }
        return -1;
    }

    /* Set socket to non-blocking */
    flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    rdma_info_report("TCP: Accepted connection from %s:%u",
                     inet_ntoa(client_addr.sin_addr),
                     ntohs(client_addr.sin_port));
    return sockfd;
}

static int tcp_send_message(int sockfd, TcpMsgType msg_type,
                            const void *payload, size_t payload_len,
                            uint32_t seq)
{
    TcpMsgHeader hdr;
    ssize_t ret;
    size_t total_sent = 0;

    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(msg_type);
    hdr.msg_len = htonl(payload_len);
    hdr.seq = htonl(seq);

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

static void *tcp_recv_thread(void *opaque)
{
    TcpBackendPrivate *priv = (TcpBackendPrivate *)opaque;
    TcpMsgHeader hdr;
    void *payload = NULL;
    int ret;
    struct pollfd pfd;

    rdma_info_report("TCP: Receive thread started");

    pfd.fd = priv->sockfd;
    pfd.events = POLLIN;

    while (priv->recv_thread_running) {
        ret = poll(&pfd, 1, 100); /* 100ms timeout */
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            rdma_error_report("TCP: Poll error: %s", strerror(errno));
            break;
        }
        if (ret == 0) {
            continue; /* Timeout */
        }

        if (pfd.revents & POLLIN) {
            ret = tcp_recv_message(priv->sockfd, &hdr, &payload);
            if (ret == -EAGAIN) {
                continue;
            }
            if (ret < 0) {
                rdma_info_report("TCP: Receive error, closing connection");
                break;
            }

            /* Handle message based on type */
            switch (hdr.msg_type) {
            case TCP_MSG_POST_SEND:
            case TCP_MSG_POST_RECV:
            case TCP_MSG_COMPLETION:
            case TCP_MSG_DATA:
            case TCP_MSG_REMOTE_CONN_INFO_RESP:
                /* These will be handled by specific operations */
                /* For now, just log */
                rdma_info_report("TCP: Received message type %u, len %u",
                                 hdr.msg_type, hdr.msg_len);
                break;
            default:
                rdma_warn_report("TCP: Unknown message type %u", hdr.msg_type);
                break;
            }

            if (payload) {
                g_free(payload);
                payload = NULL;
            }
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            rdma_info_report("TCP: Connection closed or error");
            break;
        }
    }

    rdma_info_report("TCP: Receive thread exiting");
    return NULL;
}

/*
 * Backend Lifecycle
 */

static int tcp_init(RdmaBackendDev *backend_dev, const char *config)
{
    TcpBackendPrivate *priv;
    char *host = NULL;
    uint16_t port = 0;
    bool listen_mode = false;
    int ret;

    rdma_info_report("TCP backend: Initializing");

    ret = parse_tcp_config(config, &host, &port, &listen_mode);
    if (ret < 0) {
        return ret;
    }

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

    priv->next_pd_handle = 1;
    priv->next_mr_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = 100;
    priv->next_seq = 1;

    priv->remote_host = host;
    priv->remote_port = port;
    priv->is_listening = listen_mode;

    qemu_mutex_init(&priv->lock);
    qemu_mutex_init(&priv->conn_lock);

    if (listen_mode) {
        priv->listen_fd = tcp_listen_on_port(port);
        if (priv->listen_fd < 0) {
            g_free(host);
            g_hash_table_destroy(priv->pds);
            g_hash_table_destroy(priv->mrs);
            g_hash_table_destroy(priv->cqs);
            g_hash_table_destroy(priv->qps);
            g_hash_table_destroy(priv->qp_pairs);
            qemu_mutex_destroy(&priv->lock);
            qemu_mutex_destroy(&priv->conn_lock);
            g_free(priv);
            return -1;
        }
        priv->sockfd = -1;
        priv->is_connected = false;
    } else {
        priv->sockfd = tcp_connect_to_remote(host, port);
        if (priv->sockfd < 0) {
            g_free(host);
            g_hash_table_destroy(priv->pds);
            g_hash_table_destroy(priv->mrs);
            g_hash_table_destroy(priv->cqs);
            g_hash_table_destroy(priv->qps);
            g_hash_table_destroy(priv->qp_pairs);
            qemu_mutex_destroy(&priv->lock);
            qemu_mutex_destroy(&priv->conn_lock);
            g_free(priv);
            return -1;
        }
        priv->is_connected = true;
        priv->listen_fd = -1;

        /* Start receive thread */
        priv->recv_thread_running = true;
        qemu_thread_create(&priv->recv_thread, "tcp-recv", tcp_recv_thread,
                           priv, QEMU_THREAD_JOINABLE);
    }

    backend_dev->backend_private = priv;

    rdma_info_report("TCP backend: Initialized successfully (%s mode)",
                     listen_mode ? "listen" : "connect");
    return 0;
}

static void tcp_fini(RdmaBackendDev *backend_dev)
{
    TcpBackendPrivate *priv = get_private(backend_dev);

    if (!priv) {
        return;
    }

    rdma_info_report("TCP backend: Cleaning up");

    /* Stop receive thread */
    if (priv->recv_thread_running) {
        priv->recv_thread_running = false;
        qemu_thread_join(&priv->recv_thread);
    }

    /* Close connections */
    qemu_mutex_lock(&priv->conn_lock);
    if (priv->sockfd >= 0) {
        close(priv->sockfd);
        priv->sockfd = -1;
    }
    if (priv->listen_fd >= 0) {
        close(priv->listen_fd);
        priv->listen_fd = -1;
    }
    qemu_mutex_unlock(&priv->conn_lock);

    g_free(priv->remote_host);

    g_hash_table_destroy(priv->pds);
    g_hash_table_destroy(priv->mrs);
    g_hash_table_destroy(priv->cqs);
    g_hash_table_destroy(priv->qps);
    g_hash_table_destroy(priv->qp_pairs);

    qemu_mutex_destroy(&priv->lock);
    qemu_mutex_destroy(&priv->conn_lock);

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
    attr->gid_tbl_len = 1;
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
        g_hash_table_remove(priv->cqs, GUINT_TO_POINTER(handle));
        g_free(tcq);
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
    tqp->send_queue = g_queue_new();
    tqp->recv_queue = g_queue_new();
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
        }
        tqp->rq_psn = rq_psn;
        if (qkey_set) {
            tqp->qkey = qkey;
        }
    }
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("TCP: QP %u -> RTR (remote qpn=%u)", qpn, dqpn);
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
    uint32_t seq;
    int ret;

    if (!priv || !priv->is_connected) {
        rdma_error_report("TCP: Not connected, cannot post send");
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (!tqp) {
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    wr = g_new0(TcpWR, 1);
    wr->wr_id = (uint64_t)(uintptr_t)ctx;
    wr->num_sge = num_sge;
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        wr->sge[i].addr = (void *)(uintptr_t)sge[i].addr;
        wr->sge[i].length = sge[i].length;
        wr->sge[i].lkey = sge[i].lkey;
    }
    g_queue_push_tail(tqp->send_queue, wr);
    seq = priv->next_seq++;
    qemu_mutex_unlock(&priv->lock);

    /* Send POST_SEND message over TCP */
    qemu_mutex_lock(&priv->conn_lock);
    if (priv->sockfd >= 0) {
        /* Send work request */
        ret = tcp_send_message(priv->sockfd, TCP_MSG_POST_SEND, wr,
                               sizeof(*wr) + sizeof(TcpSGE) * num_sge, seq);
        if (ret < 0) {
            rdma_error_report("TCP: Failed to send POST_SEND");
        }

        /* Send data payloads */
        for (uint32_t i = 0; i < num_sge; i++) {
            if (sge[i].addr && sge[i].length > 0) {
                ret = tcp_send_message(priv->sockfd, TCP_MSG_DATA,
                                       (const void *)(uintptr_t)sge[i].addr,
                                       sge[i].length, seq);
                if (ret < 0) {
                    rdma_error_report("TCP: Failed to send data");
                }
            }
        }
    }
    qemu_mutex_unlock(&priv->conn_lock);

    rdma_info_report("TCP: Posted send to QP %u, %u SGEs", qpn, num_sge);
}

static void tcp_post_recv(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                          uint8_t qp_type, struct ibv_sge *sge,
                          uint32_t num_sge, void *ctx)
{
    TcpBackendPrivate *priv = get_private(backend_dev);
    uint32_t qpn = (uint32_t)(uintptr_t)qp->ibqp;
    TcpQP *tqp;
    TcpWR *wr;
    uint32_t seq;

    if (!priv) {
        return;
    }

    qemu_mutex_lock(&priv->lock);
    tqp = g_hash_table_lookup(priv->qps, GUINT_TO_POINTER(qpn));
    if (!tqp) {
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    wr = g_new0(TcpWR, 1);
    wr->wr_id = (uint64_t)(uintptr_t)ctx;
    wr->num_sge = num_sge;
    for (uint32_t i = 0; i < num_sge && i < 32; i++) {
        wr->sge[i].addr = (void *)(uintptr_t)sge[i].addr;
        wr->sge[i].length = sge[i].length;
        wr->sge[i].lkey = sge[i].lkey;
    }
    g_queue_push_tail(tqp->recv_queue, wr);
    seq = priv->next_seq++;
    qemu_mutex_unlock(&priv->lock);

    /* Send POST_RECV message over TCP */
    qemu_mutex_lock(&priv->conn_lock);
    if (priv->sockfd >= 0 && priv->is_connected) {
        tcp_send_message(priv->sockfd, TCP_MSG_POST_RECV, wr,
                         sizeof(*wr) + sizeof(TcpSGE) * num_sge, seq);
    }
    qemu_mutex_unlock(&priv->conn_lock);

    rdma_info_report("TCP: Posted recv to QP %u, %u SGEs", qpn, num_sge);
}

/*
 * GID Management
 */

static int tcp_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       union ibv_gid *gid)
{
    rdma_info_report("TCP: Added GID");
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
