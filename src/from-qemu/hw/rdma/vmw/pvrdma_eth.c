/*
 * ROCm ERNIC Ethernet Support
 *
 * Simple Ethernet layer implementation - Linux kernel handles TCP/IP.
 * Intercepts Ethernet frames from VM and processes them for rdma_cm.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pvrdma.h"
#include "pvrdma_eth.h"
#include "../../../../rocm_ernic_eth.h"
#include "../rdma_backend.h"
#include "../rdma_rm.h"
#include "../../utils/net_headers.h"
#include "../../utils/dhcp_server.h"
#include "../../utils/dhcp_proxy.h"
#include "../../utils/eth_rx_inject.h"
#include "../../utils/tcp_conn.h"
#include "../../utils/rdma_cm_proto.h"
#include "hw/rdma/rdma.h" /* For rdma_pci_dma_map/unmap */
#include "hw/pci/pci.h"   /* For PCIDevice and pci_dma_sync */
#include <string.h>
#include <glib.h>
#include <inttypes.h>
#include <arpa/inet.h>

/* Get Ethernet state from PVRDMA device */
PVRDMAEthState *get_eth_state(PVRDMADev *dev)
{
    /* Store Ethernet state in device structure - add to PVRDMADev later */
    /* For now, use a static structure */
    static PVRDMAEthState eth_state = {0};
    return &eth_state;
}

/**
 * Read Ethernet register
 */
uint64_t pvrdma_eth_regs_read(PVRDMADev *dev, hwaddr addr)
{
    PVRDMAEthState *eth = get_eth_state(dev);
    uint32_t val = 0;

    if (!eth) {
        rdma_warn_report("Ethernet state is NULL, returning 0 for addr=0x%lx",
                         addr);
        return 0;
    }

    switch (addr) {
    case ROCM_ERNIC_ETH_CTL:
        val = eth->ctl;
        break;
    case ROCM_ERNIC_ETH_STATUS:
        val = eth->status;
        /* Always report link up */
        val |= ROCM_ERNIC_ETH_STATUS_LINK_UP;
        break;
    case ROCM_ERNIC_ETH_TX_BAL:
        val = (uint32_t)(eth->tx_base & 0xFFFFFFFF);
        break;
    case ROCM_ERNIC_ETH_TX_BAH:
        val = (uint32_t)(eth->tx_base >> 32);
        break;
    case ROCM_ERNIC_ETH_TX_LEN:
        val = eth->tx_len;
        break;
    case ROCM_ERNIC_ETH_TX_HEAD:
        val = eth->tx_head;
        break;
    case ROCM_ERNIC_ETH_TX_TAIL:
        val = eth->tx_tail;
        break;
    case ROCM_ERNIC_ETH_RX_BAL:
        val = (uint32_t)(eth->rx_base & 0xFFFFFFFF);
        break;
    case ROCM_ERNIC_ETH_RX_BAH:
        val = (uint32_t)(eth->rx_base >> 32);
        break;
    case ROCM_ERNIC_ETH_RX_LEN:
        val = eth->rx_len;
        break;
    case ROCM_ERNIC_ETH_RX_HEAD:
        val = eth->rx_head;
        break;
    case ROCM_ERNIC_ETH_RX_TAIL:
        val = eth->rx_tail;
        break;
    case ROCM_ERNIC_ETH_ICR:
        val = eth->icr;
        eth->icr = 0; /* Clear on read */
        break;
    case ROCM_ERNIC_ETH_IMR:
        val = eth->imr;
        break;
    case ROCM_ERNIC_ETH_MAC0:
        /* Return MAC address bytes 0-3 as little-endian uint32_t */
        val = dev->mac_addr[0] | (dev->mac_addr[1] << 8) |
              (dev->mac_addr[2] << 16) | (dev->mac_addr[3] << 24);
        break;
    case ROCM_ERNIC_ETH_MAC1:
        /* Return MAC address bytes 4-5 as little-endian uint32_t */
        val = dev->mac_addr[4] | (dev->mac_addr[5] << 8);
        break;
    default:
        val = 0;
        break;
    }

    return val;
}

/**
 * Write Ethernet register
 */
void pvrdma_eth_regs_write(PVRDMADev *dev, hwaddr addr, uint64_t val)
{
    PVRDMAEthState *eth = get_eth_state(dev);

    switch (addr) {
    case ROCM_ERNIC_ETH_CTL:
        eth->ctl = val;
        if (val & ROCM_ERNIC_ETH_CTL_RESET) {
            /* Software reset */
            eth->tx_head = 0;
            eth->tx_tail = 0;
            eth->rx_head = 0;
            eth->rx_tail = 0;
            eth->icr = 0;
            eth->ctl &= ~ROCM_ERNIC_ETH_CTL_RESET;
        }
        break;
    case ROCM_ERNIC_ETH_TX_BAL:
        eth->tx_base = (eth->tx_base & 0xFFFFFFFF00000000ULL) | val;
        break;
    case ROCM_ERNIC_ETH_TX_BAH:
        eth->tx_base = (eth->tx_base & 0xFFFFFFFFULL) | (val << 32);
        break;
    case ROCM_ERNIC_ETH_TX_LEN:
        eth->tx_len = val;
        break;
    case ROCM_ERNIC_ETH_TX_HEAD:
        eth->tx_head = val;
        break;
    case ROCM_ERNIC_ETH_TX_TAIL:
        eth->tx_tail = val;
        /* When tail is updated, process transmit descriptors */
        if (eth->tx_base != 0 && eth->tx_len != 0) {
            pvrdma_eth_process_tx(dev);
        }
        break;
    case ROCM_ERNIC_ETH_RX_BAL:
        eth->rx_base = (eth->rx_base & 0xFFFFFFFF00000000ULL) | val;
        break;
    case ROCM_ERNIC_ETH_RX_BAH:
        eth->rx_base = (eth->rx_base & 0xFFFFFFFFULL) | (val << 32);
        break;
    case ROCM_ERNIC_ETH_RX_LEN:
        eth->rx_len = val;
        break;
    case ROCM_ERNIC_ETH_RX_HEAD:
        eth->rx_head = val;
        break;
    case ROCM_ERNIC_ETH_RX_TAIL:
        eth->rx_tail = val;
        break;
    case ROCM_ERNIC_ETH_IMR:
        eth->imr = val;
        break;
    case ROCM_ERNIC_ETH_MAC0:
    case ROCM_ERNIC_ETH_MAC1:
        /* MAC address registers are read-only - ignore writes */
        break;
    default:
        break;
    }
}

/**
 * Process transmit descriptors - extract Ethernet frames from VM
 */
void pvrdma_eth_process_tx(PVRDMADev *dev)
{
    PVRDMAEthState *eth = get_eth_state(dev);
    struct rocm_ernic_eth_desc desc;
    void *desc_vaddr;
    void *packet_vaddr;
    uint32_t desc_idx;
    uint64_t desc_addr;

    if (!(eth->ctl & ROCM_ERNIC_ETH_CTL_TX_ENABLE)) {
        return;
    }

    /* Process descriptors from head to tail */
    while (eth->tx_head != eth->tx_tail) {
        PCIDevice *pci_dev = &dev->parent_obj;
        uint64_t desc_len = sizeof(desc);
        uint64_t packet_len;
        /* Calculate descriptor address */
        desc_idx = eth->tx_head;
        desc_addr =
            eth->tx_base + (desc_idx * sizeof(struct rocm_ernic_eth_desc));

        /* Map descriptor from guest memory */
        desc_vaddr = rdma_pci_dma_map(pci_dev, desc_addr, desc_len);
        if (!desc_vaddr) {
            rdma_error_report("Failed to map TX descriptor at 0x%" PRIx64,
                              desc_addr);
            break;
        }

        /* Read descriptor */
        memcpy(&desc, desc_vaddr, sizeof(desc));

        /* Map packet buffer from guest memory */
        packet_len = desc.length;
        if (packet_len == 0 || packet_len > 2048) {
            rdma_warn_report("TX: Invalid packet length %" PRIu64, packet_len);
            rdma_pci_dma_unmap(pci_dev, desc_vaddr, desc_len);
            eth->tx_head = (eth->tx_head + 1) % eth->tx_len;
            continue;
        }

        packet_vaddr = rdma_pci_dma_map(pci_dev, desc.addr, packet_len);
        if (!packet_vaddr) {
            rdma_error_report("Failed to map TX packet buffer at 0x%" PRIx64,
                              desc.addr);
            rdma_pci_dma_unmap(pci_dev, desc_vaddr, desc_len);
            break;
        }

        /* Process Ethernet frame */
        pvrdma_eth_rx_frame(dev, packet_vaddr, packet_len);
        dev->stats.total_ip_bytes_tx += packet_len;

        /* Mark descriptor as done */
        desc.status |= ROCM_ERNIC_ETH_DESC_STATUS_DD;
        memcpy(desc_vaddr, &desc, sizeof(desc));

        /* Sync descriptor write back to guest */
        pci_dma_sync(pci_dev, desc_addr, desc_len);

        /* Unmap buffers */
        rdma_pci_dma_unmap(pci_dev, packet_vaddr, packet_len);
        rdma_pci_dma_unmap(pci_dev, desc_vaddr, desc_len);

        /* Advance head pointer */
        eth->tx_head = (eth->tx_head + 1) % eth->tx_len;

        /* Set interrupt */
        eth->icr |= ROCM_ERNIC_ETH_ICR_TX_COMPLETE;
        if (eth->imr & ROCM_ERNIC_ETH_ICR_TX_COMPLETE) {
            post_interrupt(
                dev, INTR_VEC_CMD_RING); /* Use existing interrupt vector */
        }
    }
}

/**
 * Handle rdma_cm TCP (port 18515) locally for loopback mode.
 *
 * In loopback mode there is no peer VM, so we emulate a
 * minimal TCP state machine and CM responder inside the
 * server process.  This path is NOT used by the TCP mesh
 * backend -- there CM frames are forwarded to the peer.
 */
static void pvrdma_eth_handle_cm_loopback(PVRDMADev *dev,
                                          struct eth_header *eth_hdr,
                                          struct ip_header *ip_hdr,
                                          struct tcp_header *tcp_hdr,
                                          const void *frame_data, size_t len,
                                          uint16_t src_port, uint16_t dst_port)
{
    if (!dev->tcp_connections) {
        dev->tcp_connections = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)tcp_conn_free);
    }

    uint32_t src_ip = ip_hdr->src_ip;
    uint32_t dst_ip = ip_hdr->dst_ip;
    uint16_t local_port =
        (dst_port == TCP_PORT_RDMA_CM) ? TCP_PORT_RDMA_CM : src_port;
    uint16_t remote_port =
        (dst_port == TCP_PORT_RDMA_CM) ? src_port : TCP_PORT_RDMA_CM;
    uint32_t local_ip = dst_ip;
    uint32_t remote_ip = src_ip;
    bool is_server = (dst_port == TCP_PORT_RDMA_CM);

    uint8_t flags = tcp_hdr->flags;
    uint32_t seq = ntohl(tcp_hdr->seq);
    uint32_t ack = ntohl(tcp_hdr->ack);

    rdma_info_report("TCP: Packet received: flags=0x%02x seq=%u "
                     "ack=%u src_port=%u dst_port=%u",
                     flags, seq, ack, src_port, dst_port);

    TcpConnection *conn = tcp_conn_find(dev->tcp_connections, local_ip,
                                        remote_ip, local_port, remote_port);

    if (!conn) {
        conn = tcp_conn_find(dev->tcp_connections, remote_ip, local_ip,
                             remote_port, local_port);
        if (conn) {
            rdma_info_report("TCP: Found connection via reverse "
                             "lookup");
        }
    }

    if (!conn) {
        uint64_t key = ((uint64_t)local_ip << 32) | remote_ip |
                       ((uint64_t)local_port << 48) |
                       ((uint64_t)remote_port << 32);
        rdma_info_report("TCP: Creating new connection: "
                         "local=%08x:%u remote=%08x:%u "
                         "is_server=%d",
                         local_ip, local_port, remote_ip, remote_port,
                         is_server);
        conn = tcp_conn_create(local_ip, remote_ip, local_port, remote_port,
                               is_server);
        g_hash_table_insert(dev->tcp_connections, GUINT_TO_POINTER(key), conn);
    } else {
        rdma_info_report("TCP: Found existing connection: "
                         "state=%d",
                         conn->state);
    }

    size_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
    size_t tcp_payload_offset = sizeof(struct eth_header) + 20 + tcp_hdr_len;
    size_t tcp_payload_len = len - tcp_payload_offset;
    const void *tcp_payload = NULL;

    if (tcp_payload_len > 0 && tcp_payload_len <= len) {
        tcp_payload = (const uint8_t *)frame_data + tcp_payload_offset;
    }

    rdma_info_report("TCP: Processing packet: state=%d "
                     "flags=0x%02x seq=%u ack=%u payload_len=%zu",
                     conn->state, flags, seq, ack, tcp_payload_len);

    int need_response = tcp_conn_process_packet(conn, tcp_hdr, seq, ack, flags);

    if (conn->state == TCP_STATE_ESTABLISHED && tcp_payload_len > 0) {
        conn->local_ack = seq + tcp_payload_len;
        rdma_info_report("TCP: Updated local_ack to %u "
                         "(seq=%u + payload=%zu)",
                         conn->local_ack, seq, tcp_payload_len);
    }

    rdma_info_report("TCP: After processing: state=%d "
                     "need_response=%d local_ack=%u",
                     conn->state, need_response, conn->local_ack);

    uint8_t rdma_cm_response[256];
    size_t rdma_cm_resp_len = 0;

    if (conn->state == TCP_STATE_ESTABLISHED && tcp_payload &&
        tcp_payload_len > 0 && (flags & TCP_FLAG_PSH)) {
        rdma_info_report("TCP: Connection ESTABLISHED, "
                         "processing rdma_cm message");
        rdma_cm_resp_len =
            rdma_cm_process_message(tcp_payload, tcp_payload_len,
                                    rdma_cm_response, sizeof(rdma_cm_response));

        if (rdma_cm_resp_len > 0) {
            need_response = 1;
            rdma_info_report("TCP: rdma_cm response generated: "
                             "%zu bytes",
                             rdma_cm_resp_len);
        }
    } else if (conn->state != TCP_STATE_ESTABLISHED) {
        rdma_info_report("TCP: Connection not ESTABLISHED "
                         "(state=%d), skipping rdma_cm "
                         "processing",
                         conn->state);
    } else if (!(flags & TCP_FLAG_PSH)) {
        rdma_info_report("TCP: No PSH flag, skipping rdma_cm "
                         "processing");
    }

    if (need_response) {
        uint8_t resp_frame[1514];
        uint32_t resp_len = 0;
        uint8_t resp_flags = TCP_FLAG_ACK;

        if (conn->state == TCP_STATE_SYN_RECEIVED) {
            resp_flags |= TCP_FLAG_SYN;
        }

        if (rdma_cm_resp_len > 0) {
            resp_flags |= TCP_FLAG_PSH;
        }

        rdma_info_report("TCP: Generating response: flags=0x%02x "
                         "state=%d local_seq=%u local_ack=%u "
                         "payload_len=%zu",
                         resp_flags, conn->state, conn->local_seq,
                         conn->local_ack, rdma_cm_resp_len);
        int gen_ret = tcp_conn_generate_response(
            conn, resp_flags, rdma_cm_resp_len > 0 ? rdma_cm_response : NULL,
            rdma_cm_resp_len, resp_frame, sizeof(resp_frame), &resp_len);
        if (gen_ret == 0) {
            struct eth_header *resp_eth = (struct eth_header *)resp_frame;
            memcpy(resp_eth->dst_mac, eth_hdr->src_mac, 6);
            memcpy(resp_eth->src_mac, dev->mac_addr, 6);

            int ret = eth_rx_inject_frame(dev, resp_frame, resp_len);
            if (ret != 0) {
                rdma_error_report("TCP: Failed to inject response "
                                  "frame: %d (flags=0x%02x len=%u)",
                                  ret, resp_flags, resp_len);
            } else {
                rdma_info_report("TCP: Sent TCP response: "
                                 "flags=0x%02x len=%u",
                                 resp_flags, resp_len);
            }
        } else {
            rdma_error_report("TCP: Failed to generate response: "
                              "%d",
                              gen_ret);
        }
    }
}

/**
 * Receive Ethernet frame from VM - parse and handle TCP/IP for rdma_cm
 */
void pvrdma_eth_rx_frame(PVRDMADev *dev, const void *frame_data, size_t len)
{
    struct eth_header *eth_hdr = NULL;
    struct ip_header *ip_hdr = NULL;
    struct udp_header *udp_hdr = NULL;
    struct tcp_header *tcp_hdr = NULL;
    size_t udp_offset = 0, tcp_offset = 0;

    if (!dev || !frame_data || len < sizeof(struct eth_header)) {
        return;
    }

    /* Parse Ethernet header */
    if (!parse_eth_header(frame_data, len, &eth_hdr)) {
        return;
    }

    uint16_t ethertype = ntohs(eth_hdr->ethertype);

    /* Hot path -- no logging */

    /* Handle ARP packets */
    if (ethertype == ETH_ETHERTYPE_ARP) {
        if (len < sizeof(struct eth_header) + sizeof(struct arp_header)) {
            rdma_warn_report("ARP: Packet too short: len=%zu", len);
            return;
        }

        const struct arp_header *arp_hdr =
            (const struct arp_header *)((uint8_t *)frame_data +
                                        sizeof(struct eth_header));

        /* Only handle Ethernet/IPv4 ARP */
        if (ntohs(arp_hdr->hw_type) != 1 ||                   /* Ethernet */
            ntohs(arp_hdr->proto_type) != ETH_ETHERTYPE_IP || /* IPv4 */
            arp_hdr->hw_addr_len != 6 || arp_hdr->proto_addr_len != 4) {
            return;
        }

        uint16_t arp_op = ntohs(arp_hdr->op);

        /* Get server IP from DHCP server if available */
        uint32_t server_ip = inet_addr("192.168.100.1");
        if (dev->dhcp_server) {
            DhcpServer *dhcp = (DhcpServer *)dev->dhcp_server;
            server_ip = dhcp->server_ip;
        }

        struct in_addr server_addr;
        server_addr.s_addr = server_ip;

        /* Log all ARP requests for debugging */
        if (arp_op == ARP_OP_REQUEST) {
            struct in_addr target_addr, sender_addr;
            target_addr.s_addr = arp_hdr->target_proto_addr;
            sender_addr.s_addr = arp_hdr->sender_proto_addr;
            rdma_info_report(
                "ARP: Received request: who has %s? tell %s (MAC: "
                "%02x:%02x:%02x:%02x:%02x:%02x) [server IP: %s]",
                inet_ntoa(target_addr), inet_ntoa(sender_addr),
                arp_hdr->sender_hw_addr[0], arp_hdr->sender_hw_addr[1],
                arp_hdr->sender_hw_addr[2], arp_hdr->sender_hw_addr[3],
                arp_hdr->sender_hw_addr[4], arp_hdr->sender_hw_addr[5],
                inet_ntoa(server_addr));
        }

        /* Handle ARP request for server IP */
        if (arp_op == ARP_OP_REQUEST &&
            arp_hdr->target_proto_addr == server_ip) {
            rdma_info_report("ARP: Received request for %08x, sending reply",
                             server_ip);

            /* Construct ARP reply */
            uint8_t resp_frame[sizeof(struct eth_header) +
                               sizeof(struct arp_header)];
            struct eth_header *resp_eth = (struct eth_header *)resp_frame;
            struct arp_header *resp_arp =
                (struct arp_header *)(resp_frame + sizeof(struct eth_header));

            /* Ethernet header */
            memcpy(resp_eth->dst_mac, eth_hdr->src_mac, 6);
            /* Use device MAC address if set, otherwise use default MAC */
            uint8_t src_mac[6];
            memcpy(src_mac, dev->mac_addr, 6);
            memcpy(resp_eth->src_mac, src_mac, 6);
            resp_eth->ethertype = htons(ETH_ETHERTYPE_ARP);

            /* ARP header */
            resp_arp->hw_type = htons(1);                   /* Ethernet */
            resp_arp->proto_type = htons(ETH_ETHERTYPE_IP); /* IPv4 */
            resp_arp->hw_addr_len = 6;
            resp_arp->proto_addr_len = 4;
            resp_arp->op = htons(ARP_OP_REPLY);

            /* Sender (us): use device MAC and server IP */
            memcpy(resp_arp->sender_hw_addr, src_mac, 6);
            resp_arp->sender_proto_addr = server_ip;

            /* Target (requestor): use their MAC and IP */
            memcpy(resp_arp->target_hw_addr, arp_hdr->sender_hw_addr, 6);
            resp_arp->target_proto_addr = arp_hdr->sender_proto_addr;

            /* Inject ARP reply */
            int ret = eth_rx_inject_frame(dev, resp_frame, sizeof(resp_frame));
            if (ret != 0) {
                rdma_error_report("ARP: Failed to inject reply frame: %d", ret);
            } else {
                rdma_info_report(
                    "ARP: Sent reply: MAC %02x:%02x:%02x:%02x:%02x:%02x -> IP "
                    "%08x",
                    resp_arp->sender_hw_addr[0], resp_arp->sender_hw_addr[1],
                    resp_arp->sender_hw_addr[2], resp_arp->sender_hw_addr[3],
                    resp_arp->sender_hw_addr[4], resp_arp->sender_hw_addr[5],
                    server_ip);
            }
            return;
        }
    }

    /* Handle IP packets */
    if (ethertype == ETH_ETHERTYPE_IP) {
        if (!parse_ip_header(frame_data, len, eth_hdr, &ip_hdr)) {
            return;
        }

        /* Get server IP from DHCP server if available */
        uint32_t server_ip = inet_addr("192.168.100.1");
        if (dev->dhcp_server) {
            DhcpServer *dhcp = (DhcpServer *)dev->dhcp_server;
            server_ip = dhcp->server_ip;
        }

        /* Handle UDP (DHCP) */
        if (ip_hdr->protocol == IP_PROTOCOL_UDP) {
            if (!parse_udp_header(frame_data, len, ip_hdr, &udp_hdr,
                                  &udp_offset)) {
                return;
            }

            uint16_t dst_port = ntohs(udp_hdr->dst_port);
            uint16_t src_port = ntohs(udp_hdr->src_port);

            /* Handle DHCP packets */
            if (dst_port == UDP_PORT_DHCP_SERVER ||
                src_port == UDP_PORT_DHCP_CLIENT) {
                rdma_info_report("DHCP: Received UDP packet: src_port=%u "
                                 "dst_port=%u len=%zu",
                                 src_port, dst_port, len);
                size_t dhcp_offset = get_udp_payload_offset(udp_offset);
                /* Minimum DHCP packet size is 240 bytes (fixed header) */
                size_t min_dhcp_size = 240;
                rdma_info_report(
                    "DHCP: udp_offset=%zu dhcp_offset=%zu min_dhcp_size=%zu",
                    udp_offset, dhcp_offset, min_dhcp_size);
                if (len < dhcp_offset + min_dhcp_size) {
                    rdma_warn_report("DHCP: Packet too short: len=%zu need=%zu "
                                     "(offset=%zu + min_size=%zu)",
                                     len, dhcp_offset + min_dhcp_size,
                                     dhcp_offset, min_dhcp_size);
                    return;
                }

                const struct dhcp_packet *dhcp_req =
                    (const struct dhcp_packet *)((uint8_t *)frame_data +
                                                 dhcp_offset);

                rdma_info_report("DHCP: Processing request: op=%u xid=0x%08x",
                                 dhcp_req->op, ntohl(dhcp_req->xid));

                /* Process DHCP request */
                struct dhcp_packet dhcp_resp;
                size_t resp_len = 0;

                if (dev->dhcp_server) {
                    /* Loopback mode or TCP manager mode: use local DHCP server
                     */
                    rdma_info_report("DHCP: Using local DHCP server");
                    rdma_info_report("DHCP: Calling dhcp_server_process: "
                                     "request_len=%zu max_response_len=%zu",
                                     len - dhcp_offset, sizeof(dhcp_resp));
                    resp_len = dhcp_server_process(
                        (DhcpServer *)dev->dhcp_server, dhcp_req,
                        len - dhcp_offset, &dhcp_resp, sizeof(dhcp_resp));
                    rdma_info_report("DHCP: Server response: len=%zu",
                                     resp_len);
                } else if (dev->dhcp_proxy) {
                    /* TCP worker mode: forward to manager via proxy */
                    resp_len = dhcp_proxy_forward_request(
                        (DhcpProxy *)dev->dhcp_proxy, dhcp_req,
                        len - dhcp_offset, &dhcp_resp, sizeof(dhcp_resp));
                } else if (dev->backend_dev.backend_type ==
                           RDMA_BACKEND_TYPE_TCP) {
                    /* TCP worker mode: try to initialize proxy if manager
                     * connection is ready */
                    /* Forward declare TcpBackendPrivate structure */
                    typedef struct {
                        void *backend_dev;
                        void *manager_conn;
                    } TcpBackendPrivateStub;

                    TcpBackendPrivateStub *tcp_priv =
                        (TcpBackendPrivateStub *)
                            dev->backend_dev.backend_private;

                    /* Access manager_conn->sockfd */
                    typedef struct {
                        int sockfd;
                    } TcpConnectionStub;

                    if (tcp_priv && tcp_priv->manager_conn) {
                        TcpConnectionStub *manager_conn =
                            (TcpConnectionStub *)tcp_priv->manager_conn;
                        if (manager_conn->sockfd >= 0) {
                            uint32_t server_ip = inet_addr("192.168.100.1");
                            dev->dhcp_proxy = dhcp_proxy_create(
                                manager_conn->sockfd, server_ip);
                            if (dev->dhcp_proxy) {
                                rdma_info_report(
                                    "DHCP proxy initialized for TCP worker "
                                    "mode");
                                resp_len = dhcp_proxy_forward_request(
                                    (DhcpProxy *)dev->dhcp_proxy, dhcp_req,
                                    len - dhcp_offset, &dhcp_resp,
                                    sizeof(dhcp_resp));
                            }
                        }
                    }
                }

                if (resp_len > 0) {
                    /* Construct DHCP response Ethernet frame */
                    uint8_t resp_frame[1514]; /* Max Ethernet frame size */
                    struct eth_header *resp_eth =
                        (struct eth_header *)resp_frame;
                    struct ip_header *resp_ip =
                        (struct ip_header *)(resp_frame +
                                             sizeof(struct eth_header));
                    struct udp_header *resp_udp =
                        (struct udp_header *)(resp_frame +
                                              sizeof(struct eth_header) + 20);
                    struct dhcp_packet *resp_dhcp =
                        (struct dhcp_packet *)(resp_frame +
                                               sizeof(struct eth_header) + 20 +
                                               8);

                    /* Ethernet header */
                    memcpy(resp_eth->dst_mac, eth_hdr->src_mac, 6);
                    /* Use device MAC address if set, otherwise use default MAC
                     */
                    memcpy(resp_eth->src_mac, dev->mac_addr, 6);
                    resp_eth->ethertype = htons(ETH_ETHERTYPE_IP);

                    /* IP header */
                    resp_ip->version_ihl = 0x45; /* IPv4, 5 words */
                    resp_ip->tos = 0;
                    resp_ip->total_len = htons(20 + 8 + resp_len);
                    resp_ip->id = htons(0);
                    resp_ip->frag_off = 0;
                    resp_ip->ttl = 64;
                    resp_ip->protocol = IP_PROTOCOL_UDP;
                    resp_ip->checksum =
                        0; /* Calculate after setting all fields */
                    resp_ip->src_ip = ip_hdr->dst_ip;
                    resp_ip->dst_ip = ip_hdr->src_ip;

                    /* Calculate IP checksum */
                    resp_ip->checksum = ip_checksum(resp_ip, 20);

                    /* UDP header */
                    resp_udp->src_port = htons(UDP_PORT_DHCP_SERVER);
                    resp_udp->dst_port = htons(UDP_PORT_DHCP_CLIENT);
                    resp_udp->len = htons(8 + resp_len);
                    resp_udp->checksum =
                        0; /* Calculate after setting all fields */

                    /* DHCP response */
                    memcpy(resp_dhcp, &dhcp_resp, resp_len);

                    /* Calculate UDP checksum (includes pseudo-header) */
                    resp_udp->checksum =
                        udp_checksum(resp_ip, resp_udp, resp_dhcp, resp_len);

                    size_t resp_frame_len =
                        sizeof(struct eth_header) + 20 + 8 + resp_len;

                    /* Inject response into RX descriptors */
                    rdma_info_report(
                        "DHCP: Injecting response frame: %zu bytes",
                        resp_frame_len);
                    int ret =
                        eth_rx_inject_frame(dev, resp_frame, resp_frame_len);
                    if (ret != 0) {
                        rdma_error_report(
                            "DHCP: Failed to inject response frame: %d", ret);
                    } else {
                        rdma_info_report(
                            "DHCP: Response frame injected successfully");
                    }
                } else {
                    rdma_warn_report(
                        "DHCP: No response generated (resp_len=%zu)", resp_len);
                }
                return;
            }
        }

        /* Handle ICMP (ping) */
        if (ip_hdr->protocol == IP_PROTOCOL_ICMP) {
            size_t ip_hdr_len = (ip_hdr->version_ihl & 0x0F) * 4;
            size_t icmp_offset = sizeof(struct eth_header) + ip_hdr_len;
            if (len < icmp_offset + sizeof(struct icmp_header)) {
                rdma_warn_report("ICMP: Packet too short: len=%zu need=%zu",
                                 len, icmp_offset + sizeof(struct icmp_header));
                return;
            }

            const struct icmp_header *icmp_hdr =
                (const struct icmp_header *)((uint8_t *)frame_data +
                                             icmp_offset);

            /* Handle ICMP echo request (ping) */
            struct in_addr dst_addr, src_addr, srv_addr;
            dst_addr.s_addr = ip_hdr->dst_ip;
            src_addr.s_addr = ip_hdr->src_ip;
            srv_addr.s_addr = server_ip;
            char dst_str[16], src_str[16], srv_str[16];
            strncpy(dst_str, inet_ntoa(dst_addr), sizeof(dst_str) - 1);
            dst_str[sizeof(dst_str) - 1] = '\0';
            strncpy(src_str, inet_ntoa(src_addr), sizeof(src_str) - 1);
            src_str[sizeof(src_str) - 1] = '\0';
            strncpy(srv_str, inet_ntoa(srv_addr), sizeof(srv_str) - 1);
            srv_str[sizeof(srv_str) - 1] = '\0';
            rdma_info_report("ICMP: Received packet: type=%u code=%u dst=%s "
                             "src=%s server=%s match=%s",
                             icmp_hdr->type, icmp_hdr->code, dst_str, src_str,
                             srv_str,
                             (ip_hdr->dst_ip == server_ip) ? "YES" : "NO");

            if (icmp_hdr->type == ICMP_TYPE_ECHO_REQUEST &&
                ip_hdr->dst_ip == server_ip) {
                rdma_info_report("ICMP: Received echo request from %s to %s",
                                 src_str, dst_str);

                /* Calculate ICMP payload length */
                uint16_t ip_total_len = ntohs(ip_hdr->total_len);
                size_t icmp_payload_len =
                    ip_total_len - ip_hdr_len - sizeof(struct icmp_header);

                /* Construct ICMP echo reply */
                uint8_t resp_frame[1514];
                struct eth_header *resp_eth = (struct eth_header *)resp_frame;
                struct ip_header *resp_ip =
                    (struct ip_header *)(resp_frame +
                                         sizeof(struct eth_header));
                struct icmp_header *resp_icmp =
                    (struct icmp_header *)(resp_frame +
                                           sizeof(struct eth_header) +
                                           ip_hdr_len);

                /* Ethernet header */
                memcpy(resp_eth->dst_mac, eth_hdr->src_mac, 6);
                /* Use device MAC address if set, otherwise use default MAC */
                memcpy(resp_eth->src_mac, dev->mac_addr, 6);
                resp_eth->ethertype = htons(ETH_ETHERTYPE_IP);
                rdma_info_report("ICMP: Reply Ethernet header: "
                                 "dst_mac=%02x:%02x:%02x:%02x:%02x:%02x "
                                 "src_mac=%02x:%02x:%02x:%02x:%02x:%02x",
                                 resp_eth->dst_mac[0], resp_eth->dst_mac[1],
                                 resp_eth->dst_mac[2], resp_eth->dst_mac[3],
                                 resp_eth->dst_mac[4], resp_eth->dst_mac[5],
                                 resp_eth->src_mac[0], resp_eth->src_mac[1],
                                 resp_eth->src_mac[2], resp_eth->src_mac[3],
                                 resp_eth->src_mac[4], resp_eth->src_mac[5]);

                /* IP header */
                resp_ip->version_ihl = 0x45; /* IPv4, 5 words */
                resp_ip->tos = 0;
                resp_ip->total_len = htons(
                    ip_hdr_len + sizeof(struct icmp_header) + icmp_payload_len);
                resp_ip->id = htons(0);
                resp_ip->frag_off = 0;
                resp_ip->ttl = 64;
                resp_ip->protocol = IP_PROTOCOL_ICMP;
                resp_ip->checksum = 0;
                resp_ip->src_ip = ip_hdr->dst_ip; /* Server IP */
                resp_ip->dst_ip = ip_hdr->src_ip; /* Client IP */
                resp_ip->checksum = ip_checksum(resp_ip, ip_hdr_len);

                /* ICMP header */
                resp_icmp->type = ICMP_TYPE_ECHO_REPLY;
                resp_icmp->code = 0;
                resp_icmp->checksum = 0; /* Calculate after copying data */
                resp_icmp->identifier = icmp_hdr->identifier;
                resp_icmp->sequence = icmp_hdr->sequence;

                /* Copy ICMP payload (echo data) */
                if (icmp_payload_len > 0 &&
                    len >= icmp_offset + sizeof(struct icmp_header) +
                               icmp_payload_len) {
                    const void *icmp_payload = (const uint8_t *)frame_data +
                                               icmp_offset +
                                               sizeof(struct icmp_header);
                    void *resp_payload =
                        (uint8_t *)resp_frame + sizeof(struct eth_header) +
                        ip_hdr_len + sizeof(struct icmp_header);
                    memcpy(resp_payload, icmp_payload, icmp_payload_len);
                }

                /* Calculate ICMP checksum */
                uint16_t icmp_len =
                    sizeof(struct icmp_header) + icmp_payload_len;
                resp_icmp->checksum = 0;
                uint32_t icmp_sum = 0;
                /* Calculate checksum byte-by-byte to avoid alignment issues */
                const uint8_t *icmp_bytes = (const uint8_t *)resp_icmp;
                /* Process pairs of bytes */
                for (size_t i = 0; i + 1 < icmp_len; i += 2) {
                    icmp_sum +=
                        ((uint16_t)icmp_bytes[i] << 8) | icmp_bytes[i + 1];
                }
                /* Handle odd-length case: add last byte as high byte */
                if (icmp_len % 2) {
                    icmp_sum += ((uint16_t)icmp_bytes[icmp_len - 1]) << 8;
                }
                /* Fold carry bits */
                while (icmp_sum >> 16) {
                    icmp_sum = (icmp_sum & 0xFFFF) + (icmp_sum >> 16);
                }
                resp_icmp->checksum = htons(~(uint16_t)icmp_sum);

                size_t resp_frame_len =
                    sizeof(struct eth_header) + ip_hdr_len + icmp_len;

                /* Inject ICMP echo reply */
                int ret = eth_rx_inject_frame(dev, resp_frame, resp_frame_len);
                if (ret != 0) {
                    rdma_error_report("ICMP: Failed to inject echo reply: %d",
                                      ret);
                } else {
                    struct in_addr resp_src_addr, resp_dst_addr;
                    resp_src_addr.s_addr = resp_ip->src_ip;
                    resp_dst_addr.s_addr = resp_ip->dst_ip;
                    char resp_src_str[16], resp_dst_str[16];
                    strncpy(resp_src_str, inet_ntoa(resp_src_addr),
                            sizeof(resp_src_str) - 1);
                    resp_src_str[sizeof(resp_src_str) - 1] = '\0';
                    strncpy(resp_dst_str, inet_ntoa(resp_dst_addr),
                            sizeof(resp_dst_str) - 1);
                    resp_dst_str[sizeof(resp_dst_str) - 1] = '\0';
                    rdma_info_report(
                        "ICMP: Sent echo reply: %zu bytes (src=%s dst=%s "
                        "type=%u id=%u seq=%u checksum=0x%04x)",
                        resp_frame_len, resp_src_str, resp_dst_str,
                        resp_icmp->type, ntohs(resp_icmp->identifier),
                        ntohs(resp_icmp->sequence), ntohs(resp_icmp->checksum));
                }
                return;
            }
        }

        /* Handle TCP (rdma_cm on port 18515) */
        if (ip_hdr->protocol == IP_PROTOCOL_TCP) {
            if (!parse_tcp_header(frame_data, len, ip_hdr, &tcp_hdr,
                                  &tcp_offset)) {
                return;
            }

            uint16_t dst_port = ntohs(tcp_hdr->dst_port);
            uint16_t src_port = ntohs(tcp_hdr->src_port);

            /* Handle rdma_cm protocol on port 18515 */
            if (dst_port == TCP_PORT_RDMA_CM || src_port == TCP_PORT_RDMA_CM) {
                if (dev->backend_dev.backend_type == RDMA_BACKEND_TYPE_TCP) {
                    /*
                     * Multi-VM mesh: forward CM frames to
                     * the peer via tcp_backend_send_eth_frame
                     * and let the guest TCP/CM stacks handle
                     * the connection natively.
                     */
                    rdma_info_report("TCP CM: Forwarding CM frame "
                                     "(ports %u->%u) to mesh",
                                     src_port, dst_port);
                } else {
                    /* Loopback: handle CM locally via stub */
                    pvrdma_eth_handle_cm_loopback(dev, eth_hdr, ip_hdr, tcp_hdr,
                                                  frame_data, len, src_port,
                                                  dst_port);
                    return;
                }
            }
        }
    }

    /* Default: forward frame to all mesh peers */
    if (dev->backend_dev.backend_type == RDMA_BACKEND_TYPE_TCP &&
        dev->backend_dev.backend_private) {
        tcp_backend_send_eth_frame(&dev->backend_dev, frame_data, len);
    }
}
