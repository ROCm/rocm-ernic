/*
 * rdma_cm Protocol Handler Implementation (loopback mode only)
 *
 * Stub CM responder used when a single VM talks to itself
 * (RDMA_BACKEND_TYPE_LOOPBACK).  It provides basic echo /
 * REQ-REP handling so that rdma_cm can complete a connection
 * without a real peer.
 *
 * This module is NOT used by the TCP mesh backend.  In
 * multi-VM mode, CM frames are forwarded through the mesh
 * and the guest TCP/CM stacks negotiate natively.
 *
 * Based on InfiniBand Subnet Administration (SA) protocol
 * format used by rdma_cm.
 *
 * References:
 * - Linux kernel: drivers/infiniband/core/cma.c
 * - IB Architecture Spec, Vol 1, Ch 15 (SA)
 * - RFC 5040 (RDMA Protocol Specification)
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rdma_cm_proto.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "net_headers.h"
#include <string.h>

/* InfiniBand SA (Subnet Administration) header format
 * This is the base format used by rdma_cm over TCP/IP */
struct ib_sa_hdr {
    uint8_t method;         /* SA method/class */
    uint8_t mgmt_class;     /* Management class */
    uint16_t class_version; /* Class version */
    uint8_t status;         /* Status code */
    uint8_t reserved[3];
    uint64_t tid;      /* Transaction ID */
    uint16_t attr_id;  /* Attribute ID */
    uint16_t attr_mod; /* Attribute modifier */
} __attribute__((packed));

/* Simplified rdma_cm message header (for compatibility) */
struct rdma_cm_msg_hdr {
    uint8_t type;    /* Message type */
    uint8_t version; /* Protocol version */
    uint16_t length; /* Message length */
} __attribute__((packed));

/* rdma_cm message types (simplified) */
#define RDMA_CM_MSG_REQ 0x01 /* Connection request */
#define RDMA_CM_MSG_REP 0x02 /* Connection reply */
#define RDMA_CM_MSG_REJ 0x03 /* Connection reject */
#define RDMA_CM_MSG_MRA 0x04 /* Connection accept */

/* InfiniBand SA method/class values */
#define IB_SA_METHOD_GET_TABLE       0x01
#define IB_SA_METHOD_GET_TABLE_RSP   0x81
#define IB_SA_METHOD_DELETE          0x15
#define IB_SA_METHOD_DELETE_RSP      0x95
#define IB_SA_METHOD_SEND            0x03
#define IB_SA_METHOD_TRAP            0x05
#define IB_SA_METHOD_REPORT          0x06
#define IB_SA_METHOD_SERVICE_REC     0x10
#define IB_SA_METHOD_SERVICE_REC_RSP 0x90

/* Management class values */
#define IB_MGMT_CLASS_SUBN_ADM        0x03
#define IB_MGMT_CLASS_SUBN_LID_Routed 0x04

/* Check if message follows SA protocol format */
static bool is_sa_protocol(const void *data, size_t len)
{
    if (len < sizeof(struct ib_sa_hdr)) {
        return false;
    }

    const struct ib_sa_hdr *hdr = (const struct ib_sa_hdr *)data;

    /* Check if it looks like SA protocol */
    /* SA protocol typically has mgmt_class = 0x03 or 0x04 */
    if (hdr->mgmt_class == IB_MGMT_CLASS_SUBN_ADM ||
        hdr->mgmt_class == IB_MGMT_CLASS_SUBN_LID_Routed) {
        return true;
    }

    return false;
}

/* Process rdma_cm message from TCP payload
 * Returns: response size, or 0 if no response needed
 */
size_t rdma_cm_process_message(const void *tcp_payload, size_t payload_len,
                               void *response, size_t max_response_len)
{
    if (!tcp_payload || payload_len == 0 || !response ||
        max_response_len == 0) {
        return 0;
    }

    /* For very small messages (< 4 bytes), just echo them back */
    if (payload_len < 4) {
        rdma_info_report("rdma_cm: Received small message: payload_len=%zu",
                         payload_len);
        size_t copy_len =
            payload_len < max_response_len ? payload_len : max_response_len;
        memcpy(response, tcp_payload, copy_len);
        rdma_info_report("rdma_cm: Echoing small message as response");
        return copy_len;
    }

    rdma_info_report("rdma_cm: Received message: payload_len=%zu", payload_len);

    /* Log first 32 bytes of message for debugging */
    rdma_info_report(
        "rdma_cm: Message bytes (first 32): "
        "%02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x",
        ((const uint8_t *)tcp_payload)[0], ((const uint8_t *)tcp_payload)[1],
        ((const uint8_t *)tcp_payload)[2], ((const uint8_t *)tcp_payload)[3],
        ((const uint8_t *)tcp_payload)[4], ((const uint8_t *)tcp_payload)[5],
        ((const uint8_t *)tcp_payload)[6], ((const uint8_t *)tcp_payload)[7],
        ((const uint8_t *)tcp_payload)[8], ((const uint8_t *)tcp_payload)[9],
        ((const uint8_t *)tcp_payload)[10], ((const uint8_t *)tcp_payload)[11],
        ((const uint8_t *)tcp_payload)[12], ((const uint8_t *)tcp_payload)[13],
        ((const uint8_t *)tcp_payload)[14], ((const uint8_t *)tcp_payload)[15],
        ((const uint8_t *)tcp_payload)[16], ((const uint8_t *)tcp_payload)[17],
        ((const uint8_t *)tcp_payload)[18], ((const uint8_t *)tcp_payload)[19],
        ((const uint8_t *)tcp_payload)[20], ((const uint8_t *)tcp_payload)[21],
        ((const uint8_t *)tcp_payload)[22], ((const uint8_t *)tcp_payload)[23],
        ((const uint8_t *)tcp_payload)[24], ((const uint8_t *)tcp_payload)[25],
        ((const uint8_t *)tcp_payload)[26], ((const uint8_t *)tcp_payload)[27],
        ((const uint8_t *)tcp_payload)[28], ((const uint8_t *)tcp_payload)[29],
        ((const uint8_t *)tcp_payload)[30], ((const uint8_t *)tcp_payload)[31]);

    /* Try SA protocol format first */
    if (payload_len >= sizeof(struct ib_sa_hdr) &&
        is_sa_protocol(tcp_payload, payload_len)) {
        const struct ib_sa_hdr *req_hdr = (const struct ib_sa_hdr *)tcp_payload;

        rdma_info_report("rdma_cm: SA protocol message: method=0x%02x "
                         "mgmt_class=0x%02x attr_id=0x%04x",
                         req_hdr->method, req_hdr->mgmt_class,
                         ntohs(req_hdr->attr_id));

        /* For SA protocol, generate appropriate response */
        if (max_response_len >= sizeof(struct ib_sa_hdr)) {
            struct ib_sa_hdr *resp_hdr = (struct ib_sa_hdr *)response;

            /* Copy request header */
            memcpy(resp_hdr, req_hdr, sizeof(struct ib_sa_hdr));

            /* Convert request to response */
            if (req_hdr->method & 0x80) {
                /* Already a response, no need to respond */
                return 0;
            }

            /* Set response method (set high bit) */
            resp_hdr->method = req_hdr->method | 0x80;
            resp_hdr->status = 0; /* Success */

            /* Copy additional data if present */
            size_t copy_len = sizeof(struct ib_sa_hdr);
            if (payload_len > sizeof(struct ib_sa_hdr) &&
                max_response_len > sizeof(struct ib_sa_hdr)) {
                size_t data_len = payload_len - sizeof(struct ib_sa_hdr);
                size_t resp_data_len =
                    max_response_len - sizeof(struct ib_sa_hdr);
                size_t to_copy =
                    data_len < resp_data_len ? data_len : resp_data_len;
                memcpy((uint8_t *)response + sizeof(struct ib_sa_hdr),
                       (const uint8_t *)tcp_payload + sizeof(struct ib_sa_hdr),
                       to_copy);
                copy_len += to_copy;
            }

            rdma_info_report("rdma_cm: SA protocol response: method=0x%02x "
                             "len=%zu",
                             resp_hdr->method, copy_len);

            return copy_len;
        }
    }

    /* Check if this is a version string (e.g., "6.20" = 0x36 0x2e 0x32 0x30) */
    const uint8_t *payload_bytes = (const uint8_t *)tcp_payload;
    if (payload_len >= 4 && payload_bytes[0] == '6' &&
        payload_bytes[1] == '.' &&
        (payload_bytes[2] >= '0' && payload_bytes[2] <= '9') &&
        (payload_bytes[3] >= '0' && payload_bytes[3] <= '9')) {
        /* This looks like a version string - check if it's ASCII */
        bool is_version_string = true;
        for (size_t i = 0; i < payload_len && i < 16; i++) {
            if (payload_bytes[i] != 0 &&
                (payload_bytes[i] < 0x20 || payload_bytes[i] > 0x7e)) {
                is_version_string = false;
                break;
            }
        }

        if (is_version_string) {
            rdma_info_report("rdma_cm: Received version string: '%.*s'",
                             (int)payload_len < 16 ? (int)payload_len : 16,
                             (const char *)tcp_payload);

            /* For version strings, echo back or send acknowledgment */
            /* librdmacm might expect a version response or just ACK */
            /* Try sending back the same version string */
            size_t copy_len =
                payload_len < max_response_len ? payload_len : max_response_len;
            memcpy(response, tcp_payload, copy_len);
            rdma_info_report("rdma_cm: Echoing version string as response");
            return copy_len;
        }
    }

    /* Fall back to simplified format */
    if (payload_len >= sizeof(struct rdma_cm_msg_hdr)) {
        const struct rdma_cm_msg_hdr *req_hdr =
            (const struct rdma_cm_msg_hdr *)tcp_payload;

        rdma_info_report(
            "rdma_cm: Simplified format: type=0x%02x version=0x%02x "
            "length=%u",
            req_hdr->type, req_hdr->version, ntohs(req_hdr->length));

        /* For loopback mode, accept all connection requests */
        if (req_hdr->type == RDMA_CM_MSG_REQ) {
            struct rdma_cm_msg_hdr *resp_hdr =
                (struct rdma_cm_msg_hdr *)response;

            resp_hdr->type = RDMA_CM_MSG_REP; /* Reply/Accept */
            resp_hdr->version = req_hdr->version;
            resp_hdr->length = htons(sizeof(struct rdma_cm_msg_hdr));

            rdma_info_report(
                "rdma_cm: Accepted connection request (sending REPLY)");
            return sizeof(struct rdma_cm_msg_hdr);
        }

        /* For other message types, just acknowledge */
        if (req_hdr->type == RDMA_CM_MSG_REP ||
            req_hdr->type == RDMA_CM_MSG_MRA) {
            rdma_info_report("rdma_cm: Received connection reply/accept");
            return 0; /* No response needed */
        }
    }

    /* For unknown message formats, send a generic response */
    rdma_info_report("rdma_cm: Unknown message format, sending generic "
                     "response");

    /* Send a response - echo the request with success indication */
    size_t copy_len =
        payload_len < max_response_len ? payload_len : max_response_len;
    if (copy_len < 4) {
        copy_len = 4;
    }

    memcpy(response, tcp_payload, copy_len);

    /* Try to set response indicator if possible */
    if (copy_len >= 2) {
        uint8_t *resp_bytes = (uint8_t *)response;
        uint8_t original_type = resp_bytes[0];

        /* For ASCII messages starting with printable chars, don't modify */
        if (original_type >= 0x20 && original_type <= 0x7e) {
            /* Likely ASCII text - just echo it back */
            rdma_info_report("rdma_cm: Echoing ASCII message as-is");
        } else if (original_type < 0x80) {
            /* For binary message types, set response bit */
            resp_bytes[0] |= 0x80; /* Set response bit */
        }
    }

    /* Log response bytes for debugging */
    char resp_hex[128] = {0};
    size_t hex_len = 0;
    for (size_t i = 0; i < copy_len && i < 16 && hex_len < sizeof(resp_hex) - 3;
         i++) {
        hex_len += snprintf(resp_hex + hex_len, sizeof(resp_hex) - hex_len,
                            "%02x ", ((uint8_t *)response)[i]);
    }

    rdma_info_report(
        "rdma_cm: Sending generic response: len=%zu first_byte=0x%02x "
        "bytes=%s",
        copy_len, ((uint8_t *)response)[0], resp_hex);

    return copy_len;
}
