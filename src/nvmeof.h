/*
 * nvmeof.h — NVMe and NVMe over Fabrics wire definitions
 *
 * Just the structures and constants the in-process controller in
 * nvmeof_target.c needs, transcribed from the NVMe 1.4 and NVMe-oF 1.1
 * specifications.  Offsets are spelled out as macros rather than packed
 * structs: every field on the wire is little-endian regardless of host
 * byte order, and the capsules arrive as byte buffers scattered out of
 * guest memory, so the accessors below are the only sanctioned way to
 * read them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_H
#define NVMEOF_H

#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Capsule geometry
 * -------------------------------------------------------------------------
 */

#define NVME_SQE_SIZE 64 /* command capsule header */
#define NVME_CQE_SIZE 16 /* response capsule */

/* Identify and Get Log Page payloads are one 4 KiB page. */
#define NVME_IDENTIFY_SIZE 4096

/* NQNs are 223 characters plus a NUL, but the Connect data carries them in
 * 256-byte fields, so size buffers for the field rather than the limit. */
#define NVMEOF_NQN_FIELD 256
#define NVMEOF_NQN_MAX   224

#define NVMEOF_DISCOVERY_NQN "nqn.2014-08.org.nvmexpress.discovery"

/* -------------------------------------------------------------------------
 * Little-endian accessors
 * -------------------------------------------------------------------------
 */

static inline uint16_t nvme_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t nvme_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t nvme_get_le64(const uint8_t *p)
{
    return (uint64_t)nvme_get_le32(p) | ((uint64_t)nvme_get_le32(p + 4) << 32);
}

static inline void nvme_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void nvme_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void nvme_put_le64(uint8_t *p, uint64_t v)
{
    nvme_put_le32(p, (uint32_t)v);
    nvme_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* -------------------------------------------------------------------------
 * Submission queue entry (command capsule header)
 *
 *   [0]     opcode
 *   [1]     flags (PSDT in bits 6..7)
 *   [2:3]   command identifier
 *   [4:7]   nsid
 *   [8:23]  reserved / metadata pointer
 *   [24:39] data pointer (PRP list or one SGL descriptor)
 *   [40:63] cdw10 .. cdw15
 * -------------------------------------------------------------------------
 */

#define NVME_SQE_OPCODE_OFF 0
#define NVME_SQE_FLAGS_OFF  1
#define NVME_SQE_CID_OFF    2
#define NVME_SQE_NSID_OFF   4
#define NVME_SQE_DPTR_OFF   24
#define NVME_SQE_CDW10_OFF  40
#define NVME_SQE_CDW11_OFF  44
#define NVME_SQE_CDW12_OFF  48
#define NVME_SQE_CDW13_OFF  52
#define NVME_SQE_CDW14_OFF  56
#define NVME_SQE_CDW15_OFF  60

/* Fabrics commands overlay the same 64 bytes with fctype at offset 4. */
#define NVMEOF_SQE_FCTYPE_OFF 4

/* nvmf_connect_command */
#define NVMEOF_CONNECT_RECFMT_OFF 40
#define NVMEOF_CONNECT_QID_OFF    42
#define NVMEOF_CONNECT_SQSIZE_OFF 44
#define NVMEOF_CONNECT_CATTR_OFF  46
#define NVMEOF_CONNECT_KATO_OFF   48

/* nvmf_property_get_command / nvmf_property_set_command */
#define NVMEOF_PROP_ATTRIB_OFF 40
#define NVMEOF_PROP_OFFSET_OFF 44
#define NVMEOF_PROP_VALUE_OFF  48

/* Connect data payload (1024 bytes, fetched through the command's dptr) */
#define NVMEOF_CONNECT_DATA_SIZE   1024
#define NVMEOF_CONNECT_HOSTID_OFF  0
#define NVMEOF_CONNECT_CNTLID_OFF  16
#define NVMEOF_CONNECT_SUBNQN_OFF  256
#define NVMEOF_CONNECT_HOSTNQN_OFF 512

/* -------------------------------------------------------------------------
 * Completion queue entry (response capsule)
 *
 *   [0:7]   command-specific result
 *   [8:9]   SQ head pointer
 *   [10:11] SQ identifier
 *   [12:13] command identifier
 *   [14:15] status field, shifted left one bit over the phase tag
 * -------------------------------------------------------------------------
 */

#define NVME_CQE_RESULT_OFF 0
#define NVME_CQE_SQHD_OFF   8
#define NVME_CQE_SQID_OFF   10
#define NVME_CQE_CID_OFF    12
#define NVME_CQE_STATUS_OFF 14

/* -------------------------------------------------------------------------
 * Opcodes
 * -------------------------------------------------------------------------
 */

/* Admin command set */
enum {
    NVME_ADMIN_DELETE_SQ = 0x00,
    NVME_ADMIN_CREATE_SQ = 0x01,
    NVME_ADMIN_GET_LOG_PAGE = 0x02,
    NVME_ADMIN_DELETE_CQ = 0x04,
    NVME_ADMIN_CREATE_CQ = 0x05,
    NVME_ADMIN_IDENTIFY = 0x06,
    NVME_ADMIN_ABORT = 0x08,
    NVME_ADMIN_SET_FEATURES = 0x09,
    NVME_ADMIN_GET_FEATURES = 0x0a,
    NVME_ADMIN_ASYNC_EVENT = 0x0c,
    NVME_ADMIN_KEEP_ALIVE = 0x18,
    NVME_ADMIN_FABRICS = 0x7f,
};

/* NVM (I/O) command set */
enum {
    NVME_CMD_FLUSH = 0x00,
    NVME_CMD_WRITE = 0x01,
    NVME_CMD_READ = 0x02,
    NVME_CMD_WRITE_ZEROES = 0x08,
    NVME_CMD_DSM = 0x09,
};

/* Fabrics command types */
enum {
    NVMEOF_FCTYPE_PROPERTY_SET = 0x00,
    NVMEOF_FCTYPE_CONNECT = 0x01,
    NVMEOF_FCTYPE_PROPERTY_GET = 0x04,
    NVMEOF_FCTYPE_AUTH_SEND = 0x05,
    NVMEOF_FCTYPE_AUTH_RECV = 0x06,
};

/* Identify CNS values */
enum {
    NVME_ID_CNS_NS = 0x00,
    NVME_ID_CNS_CTRL = 0x01,
    NVME_ID_CNS_NS_ACTIVE_LIST = 0x02,
    NVME_ID_CNS_NS_DESC_LIST = 0x03,
};

/* Log page identifiers */
enum {
    NVME_LOG_ERROR = 0x01,
    NVME_LOG_SMART = 0x02,
    NVME_LOG_FW_SLOT = 0x03,
    NVME_LOG_CHANGED_NS = 0x04,
    NVME_LOG_CMD_EFFECTS = 0x05,
    NVME_LOG_DISCOVERY = 0x70,
};

/* Feature identifiers */
enum {
    NVME_FEAT_NUM_QUEUES = 0x07,
    NVME_FEAT_ASYNC_EVENT = 0x0b,
    NVME_FEAT_KATO = 0x0f,
};

/* -------------------------------------------------------------------------
 * Status codes.  The response capsule carries (SCT << 8 | SC) shifted left
 * one bit; NVME_STATUS_DNR is the do-not-retry bit in the same field.
 * -------------------------------------------------------------------------
 */

enum {
    NVME_SCT_GENERIC = 0x0,
    NVME_SCT_CMD_SPECIFIC = 0x1,
    NVME_SCT_MEDIA = 0x2,
};

enum {
    NVME_SC_SUCCESS = 0x00,
    NVME_SC_INVALID_OPCODE = 0x01,
    NVME_SC_INVALID_FIELD = 0x02,
    NVME_SC_DATA_XFER_ERROR = 0x04,
    NVME_SC_INTERNAL = 0x06,
    NVME_SC_SGL_INVALID_TYPE = 0x11,
    NVME_SC_SGL_INVALID_DATA = 0x0f,
    NVME_SC_INVALID_NS = 0x0b,
    NVME_SC_LBA_RANGE = 0x80,
    NVME_SC_CAP_EXCEEDED = 0x81,
};

/* Command-specific (SCT 1) status codes used by Fabrics Connect */
enum {
    NVMEOF_SC_CONNECT_FORMAT = 0x80,
    NVMEOF_SC_CONNECT_CTRL_BUSY = 0x81,
    NVMEOF_SC_CONNECT_INVALID_PARAM = 0x82,
    NVMEOF_SC_CONNECT_RESTART_DISC = 0x83,
    NVMEOF_SC_CONNECT_INVALID_HOST = 0x84,
};

/* DNR is bit 14 of the status field, which is bit 15 once the field has been
 * shifted left over the phase tag.  Fold it in before the shift, not after --
 * after, 0x4000 is the More bit and the host retries a permanent failure. */
#define NVME_STATUS_DNR 0x4000

/* Pack an (SCT, SC) pair into the response capsule's status field. */
static inline uint16_t nvme_status(uint8_t sct, uint8_t sc, int dnr)
{
    uint16_t f = (uint16_t)(((uint16_t)sct << 8) | sc);
    if (dnr)
        f |= NVME_STATUS_DNR;
    return (uint16_t)(f << 1);
}

/* -------------------------------------------------------------------------
 * SGL descriptors.  Both forms are 16 bytes with the type byte last; the
 * high nibble selects the form and the low nibble its subtype.
 *
 *   data block   : le64 addr, le32 length, rsvd[3], type
 *   keyed block  : le64 addr, le24 length, le32 key, type
 * -------------------------------------------------------------------------
 */

enum {
    NVME_SGL_TYPE_DATA_BLOCK = 0x0,
    NVME_SGL_TYPE_BIT_BUCKET = 0x1,
    NVME_SGL_TYPE_SEGMENT = 0x2,
    NVME_SGL_TYPE_LAST_SEGMENT = 0x3,
    NVME_SGL_TYPE_KEYED_DATA = 0x4,
    NVME_SGL_TYPE_TRANSPORT_DATA = 0x5,
};

/* Subtype 0x1 on a data block means the address is an offset into the
 * capsule rather than a host address: that is in-capsule data. */
#define NVME_SGL_SUBTYPE_OFFSET 0x1

/* Controller Capabilities / Configuration / Status property offsets */
enum {
    NVMEOF_PROP_CAP = 0x00,
    NVMEOF_PROP_VS = 0x08,
    NVMEOF_PROP_CC = 0x14,
    NVMEOF_PROP_CSTS = 0x1c,
    NVMEOF_PROP_NSSR = 0x20,
};

#define NVME_CC_ENABLE       0x1u
#define NVME_CSTS_RDY        0x1u
#define NVME_CSTS_SHST_CMPLT 0x8u

/* -------------------------------------------------------------------------
 * Discovery log page (Get Log Page, LID 0x70)
 * -------------------------------------------------------------------------
 */

#define NVMEOF_DISC_HDR_SIZE   1024
#define NVMEOF_DISC_ENTRY_SIZE 1024

#define NVMEOF_DISC_GENCTR_OFF 0
#define NVMEOF_DISC_NUMREC_OFF 8
#define NVMEOF_DISC_RECFMT_OFF 16

/* nvmf_disc_rsp_page_entry field offsets */
#define NVMEOF_DISC_E_TRTYPE_OFF  0
#define NVMEOF_DISC_E_ADRFAM_OFF  1
#define NVMEOF_DISC_E_SUBTYPE_OFF 2
#define NVMEOF_DISC_E_TREQ_OFF    3
#define NVMEOF_DISC_E_PORTID_OFF  4
#define NVMEOF_DISC_E_CNTLID_OFF  6
#define NVMEOF_DISC_E_ASQSZ_OFF   8
#define NVMEOF_DISC_E_TRSVCID_OFF 32
#define NVMEOF_DISC_E_SUBNQN_OFF  256
#define NVMEOF_DISC_E_TRADDR_OFF  512
#define NVMEOF_DISC_E_TSAS_OFF    768

enum {
    NVMEOF_TRTYPE_RDMA = 1,
    NVMEOF_ADRFAM_IPV4 = 1,
    NVMEOF_SUBTYPE_DISCOVERY = 1,
    NVMEOF_SUBTYPE_NVME = 2,
    NVMEOF_RDMA_QPTYPE_CONNECTED = 1,
    NVMEOF_RDMA_PRTYPE_NONE = 1,
    NVMEOF_RDMA_CMS_RDMA_CM = 1,
};

#endif /* NVMEOF_H */
