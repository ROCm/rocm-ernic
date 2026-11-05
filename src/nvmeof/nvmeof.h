/*
 * nvmeof.h — NVMe and NVMe over Fabrics wire definitions
 *
 * Just the structures and constants the in-process controller in
 * nvmeof_target.c needs, transcribed from the NVMe 1.4 and NVMe-oF 1.1
 * specifications.  Offsets are spelled out as macros rather than packed
 * structs: every field on the wire is little-endian regardless of host
 * byte order, and the capsules arrive as byte buffers scattered out of
 * guest memory, so the accessors in byteorder.h are the only sanctioned
 * way to read them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_H
#define NVMEOF_H

#include <stdint.h>

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
#define NVME_SQE_CDW2_OFF   8
#define NVME_SQE_CDW3_OFF   12
#define NVME_SQE_CDW10_OFF  40
#define NVME_SQE_CDW11_OFF  44
#define NVME_SQE_CDW12_OFF  48
#define NVME_SQE_CDW13_OFF  52
#define NVME_SQE_CDW14_OFF  56
#define NVME_SQE_CDW15_OFF  60

/* Read, Write and Write Zeroes: Force Unit Access, in cdw12 */
#define NVME_RW_FUA (1U << 30)

/* Namespace Identification Descriptor types (Identify CNS 03h). */
#define NVME_NIDT_UUID 3
#define NVME_NIDT_CSI  4

/* Command Set Identifiers. */
#define NVME_CSI_NVM 0
#define NVME_CSI_KV  1

/*
 * NVM Express Key Value Command Set opcodes.
 *
 * STORE and RETRIEVE collide with WRITE and READ in the block command set,
 * which is why a namespace must declare its CSI and why these are only ever
 * dispatched for one that declared KV.
 */
#define NVME_KV_CMD_STORE    0x01
#define NVME_KV_CMD_RETRIEVE 0x02
#define NVME_KV_CMD_LIST     0x06
#define NVME_KV_CMD_DELETE   0x10
#define NVME_KV_CMD_EXIST    0x14

/* KV status codes, generic status type. */
#define NVME_SC_KV_INVALID_VALUE_SIZE 0x85
#define NVME_SC_KV_INVALID_KEY_SIZE   0x86
#define NVME_SC_KV_KEY_NOT_EXIST      0x87
#define NVME_SC_KV_UNRECOVERED        0x88
#define NVME_SC_KV_KEY_EXISTS         0x89

/* Store Option bits in CDW11[15:08]. */
#define NVME_KV_STORE_OPT_NO_OVERWRITE 0x01 /* fail if the key exists */
#define NVME_KV_STORE_OPT_MUST_EXIST   0x02 /* fail if it does not */

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
    /*
     * I/O Command Set Independent Identify Namespace.  The host asks for
     * this instead of CNS 00h once it has selected CC.CSS = all-supported,
     * because CNS 00h is NVM-command-set data and means nothing for a
     * namespace that speaks another one.
     */
    NVME_ID_CNS_NS_CS_INDEP = 0x08,
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

/* Media and data integrity (SCT 2) status codes */
enum {
    NVME_SC_WRITE_FAULT = 0x80,
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
    if (dnr) {
        f |= NVME_STATUS_DNR;
    }
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

#define NVME_SGL_ADDR_OFF   0
#define NVME_SGL_LENGTH_OFF 8 /* le32, or le24 in a keyed block */
#define NVME_SGL_KEY_OFF    11
#define NVME_SGL_TYPE_OFF   15
#define NVME_SGL_TYPE_SHIFT 4

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

/* CAP: MQES in bits 15:0, then these */
#define NVME_CAP_CQR      (UINT64_C(1) << 16) /* contiguous queues required */
#define NVME_CAP_TO_SHIFT 24                  /* timeout, in 500 ms units */
#define NVME_CAP_CSS_NVM  (UINT64_C(1) << 37) /* the NVM command set */
/* CSS bit 6: one or more other I/O command sets */
#define NVME_CAP_CSS_OTHER (UINT64_C(1) << 43)

/* VS, and Identify Controller's VER: major, minor, tertiary */
#define NVME_VERSION_1_4 0x00010400U

#define NVME_CC_ENABLE       0x1U
#define NVME_CC_SHN_MASK     0xc000U
#define NVME_CC_SHN_NORMAL   0x4000U
#define NVME_CSTS_RDY        0x1U
#define NVME_CSTS_SHST_MASK  0xcU
#define NVME_CSTS_SHST_CMPLT 0x8U

/* -------------------------------------------------------------------------
 * Identify Controller data structure (CNS 01h): the fields this controller
 * sets, which leaves the rest zero.  The ASCII fields are padded with
 * spaces, not NUL-terminated.
 *
 *   [0:1]       le16 vid: PCI vendor id
 *   [2:3]       le16 ssvid: PCI subsystem vendor id
 *   [4:23]      sn: serial number, ASCII
 *   [24:63]     mn: model number, ASCII
 *   [64:71]     fr: firmware revision, ASCII
 *   [72]        rab: recommended arbitration burst
 *   [77]        mdts: maximum data transfer size
 *   [78:79]     le16 cntlid
 *   [80:83]     le32 ver
 *   [111]       cntrltype
 *   [256:257]   le16 oacs: optional admin commands
 *   [259]       aerl: asynchronous event request limit
 *   [261]       lpa: log page attributes
 *   [262]       elpe: error log page entries
 *   [320:321]   le16 kas: keep alive granularity, in 100 ms units
 *   [512]       sqes: SQ entry size, log2 maximum and required
 *   [513]       cqes: CQ entry size, likewise
 *   [514:515]   le16 maxcmd
 *   [516:519]   le32 nn: number of namespaces
 *   [520:521]   le16 oncs: optional NVM commands
 *   [524]       fna: format NVM attributes
 *   [525]       vwc: volatile write cache
 *   [536:539]   le32 sgls: SGL support
 *   [768:1023]  subnqn, NUL-terminated
 *   [1792:1795] le32 ioccsz: I/O command capsule size, in 16 bytes (NVMe-oF)
 *   [1796:1799] le32 iorcsz: I/O response capsule size, in 16 bytes
 *   [1800:1801] le16 icdoff: in-capsule data offset
 *   [1802]      fcatt: fabrics controller attributes
 *   [1803]      msdbd: maximum SGL data block descriptors
 * -------------------------------------------------------------------------
 */

#define NVME_ID_CTRL_VID_OFF       0
#define NVME_ID_CTRL_SSVID_OFF     2
#define NVME_ID_CTRL_SN_OFF        4
#define NVME_ID_CTRL_SN_LEN        20
#define NVME_ID_CTRL_MN_OFF        24
#define NVME_ID_CTRL_MN_LEN        40
#define NVME_ID_CTRL_FR_OFF        64
#define NVME_ID_CTRL_FR_LEN        8
#define NVME_ID_CTRL_RAB_OFF       72
#define NVME_ID_CTRL_MDTS_OFF      77
#define NVME_ID_CTRL_CNTLID_OFF    78
#define NVME_ID_CTRL_VER_OFF       80
#define NVME_ID_CTRL_CNTRLTYPE_OFF 111
#define NVME_ID_CTRL_OACS_OFF      256
#define NVME_ID_CTRL_AERL_OFF      259
#define NVME_ID_CTRL_LPA_OFF       261
#define NVME_ID_CTRL_ELPE_OFF      262
#define NVME_ID_CTRL_KAS_OFF       320
#define NVME_ID_CTRL_SQES_OFF      512
#define NVME_ID_CTRL_CQES_OFF      513
#define NVME_ID_CTRL_MAXCMD_OFF    514
#define NVME_ID_CTRL_NN_OFF        516
#define NVME_ID_CTRL_ONCS_OFF      520
#define NVME_ID_CTRL_FNA_OFF       524
#define NVME_ID_CTRL_VWC_OFF       525
#define NVME_ID_CTRL_SGLS_OFF      536
#define NVME_ID_CTRL_SUBNQN_OFF    768
#define NVME_ID_CTRL_IOCCSZ_OFF    1792
#define NVME_ID_CTRL_IORCSZ_OFF    1796
#define NVME_ID_CTRL_ICDOFF_OFF    1800
#define NVME_ID_CTRL_FCATT_OFF     1802
#define NVME_ID_CTRL_MSDBD_OFF     1803

enum {
    NVME_CTRL_TYPE_IO = 1,
    NVME_CTRL_TYPE_DISCOVERY = 2,
};

/* LPA: the Commands Supported and Effects log page */
#define NVME_LPA_CMD_EFFECTS 0x02U

#define NVME_ONCS_WRITE_ZEROES 0x0008U

/* VWC: a volatile write cache, and bits 2:1 = 10b, Flush needs a
 * namespace's own NSID rather than the broadcast one */
#define NVME_VWC_PRESENT      0x01U
#define NVME_VWC_NO_FLUSH_ALL 0x04U

/* SGLS: supported, keyed descriptors, and in-capsule data by offset */
#define NVME_SGLS_SUPPORTED (1U << 0)
#define NVME_SGLS_KEYED     (1U << 2)
#define NVME_SGLS_OFFSET    (1U << 20)

/* SQES and CQES: the log2 entry size, maximum in the high nibble and
 * required in the low */
#define NVME_ID_CTRL_SQES ((6U << 4) | 6U) /* 64 bytes */
#define NVME_ID_CTRL_CQES ((4U << 4) | 4U) /* 16 bytes */

/* -------------------------------------------------------------------------
 * Identify Namespace data structure (CNS 00h), the fields this controller
 * sets, with one LBA format
 *
 *   [0:7]     le64 nsze: namespace size, in blocks
 *   [8:15]    le64 ncap: capacity
 *   [16:23]   le64 nuse: utilization
 *   [24]      nsfeat
 *   [25]      nlbaf: number of LBA formats, less one
 *   [26]      flbas: the LBA format in use
 *   [27]      mc: metadata capabilities
 *   [28]      dpc: data protection capabilities
 *   [29]      dps: data protection settings
 *   [128:131] lbaf0: le16 metadata size, the log2 block size, and relative
 *             performance
 * -------------------------------------------------------------------------
 */

#define NVME_ID_NS_NSZE_OFF   0
#define NVME_ID_NS_NCAP_OFF   8
#define NVME_ID_NS_NUSE_OFF   16
#define NVME_ID_NS_NSFEAT_OFF 24
#define NVME_ID_NS_NLBAF_OFF  25
#define NVME_ID_NS_FLBAS_OFF  26
#define NVME_ID_NS_MC_OFF     27
#define NVME_ID_NS_DPC_OFF    28
#define NVME_ID_NS_DPS_OFF    29
#define NVME_ID_NS_LBAF0_OFF  128
#define NVME_LBAF_MS_OFF      0
#define NVME_LBAF_LBADS_OFF   2
#define NVME_LBAF_RP_OFF      3

/* -------------------------------------------------------------------------
 * Namespace Identification Descriptor (CNS 03h)
 *
 *   [0]     nidt: the identifier's type
 *   [1]     nidl: its length
 *   [4:...] the identifier
 * -------------------------------------------------------------------------
 */

#define NVME_NS_DESC_NIDT_OFF 0
#define NVME_NS_DESC_NIDL_OFF 1
#define NVME_NS_DESC_NID_OFF  4

#define NVME_NIDL_UUID 16
#define NVME_NIDL_CSI  1

/* -------------------------------------------------------------------------
 * I/O Command Set Independent Identify Namespace data structure (CNS 08h)
 *
 *   [0]     nsfeat
 *   [1]     nmic: multipath and sharing
 *   [2]     rescap: reservation capabilities
 *   [3]     fpi: format progress indicator
 *   [4:7]   le32 anagrpid
 *   [8]     nsattr
 *   [10:11] le16 nvmsetid
 *   [12:13] le16 endgid
 *   [14]    nstat: namespace status
 * -------------------------------------------------------------------------
 */

#define NVME_ID_NS_CSI_NSFEAT_OFF   0
#define NVME_ID_NS_CSI_NMIC_OFF     1
#define NVME_ID_NS_CSI_RESCAP_OFF   2
#define NVME_ID_NS_CSI_FPI_OFF      3
#define NVME_ID_NS_CSI_ANAGRPID_OFF 4
#define NVME_ID_NS_CSI_NSATTR_OFF   8
#define NVME_ID_NS_CSI_NVMSETID_OFF 10
#define NVME_ID_NS_CSI_ENDGID_OFF   12
#define NVME_ID_NS_CSI_NSTAT_OFF    14

#define NVME_NSTAT_NRDY 0x01U /* the namespace is ready */

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
