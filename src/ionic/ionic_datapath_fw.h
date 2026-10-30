/*
 * ionic_datapath_fw.h — the guest driver's data-path ABI
 *
 * The work request, completion and status encodings of the ionic driver's
 * ionic_fw.h, which the data path reads from and writes to guest rings.
 * Keep them in sync with the pinned kernel ref.  Only the data path's own
 * files include this: test_ionic_dp_errors.c writes out the values it uses
 * itself, so a change on either side fails a row instead of moving both at
 * once.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_FW_H
#define IONIC_DATAPATH_FW_H

/* enum ionic_v1_op */
#define IONIC_V1_OP_SEND           0
#define IONIC_V1_OP_SEND_INV       1
#define IONIC_V1_OP_SEND_IMM       2
#define IONIC_V1_OP_RDMA_READ      3
#define IONIC_V1_OP_RDMA_WRITE     4
#define IONIC_V1_OP_RDMA_WRITE_IMM 5
#define IONIC_V1_OP_ATOMIC_CS      6
#define IONIC_V1_OP_ATOMIC_FA      7
#define IONIC_V1_OP_REG_MR         8
#define IONIC_V1_OP_LOCAL_INV      9

/* enum ionic_v1_flag (be16 at WQE byte 10) */
#define IONIC_V1_FLAG_INL 0x0004U
#define IONIC_V1_FLAG_SIG 0x0008U

/*
 * The top four bits of the flags are the SGE list's format ("sgl spec
 * format" in ionic_fw.h). Zero is a plain list. A QP created with
 * speculative SGEs describes a work request with more than
 * IONIC_V1_SPEC_FIRST_SGE SGEs differently: a table of their lengths, be32
 * (SPEC32, up to eight SGEs) or be16 (SPEC16, more), fills the first
 * IONIC_V1_SPEC_FIRST_SGE SGE slots, and the SGEs follow it. The table
 * repeats the lengths the SGEs carry. The driver defines no other format.
 * ionic_rdma.ko and rdma-core's ionic provider both create a send or
 * receive queue that way when it takes three to eight SGEs (IONIC_SPEC_HIGH).
 */
#define IONIC_V1_FLAG_SPEC_MASK 0xf000U
#define IONIC_V1_FLAG_SPEC32    0x1000U
#define IONIC_V1_FLAG_SPEC16    0x2000U
#define IONIC_V1_SPEC_FIRST_SGE 2U

/*
 * struct ionic_v1_wqe:
 *   [0:7]   u64  wqe_id (native endian: SQ producer index / RQ meta index)
 *   [8]     u8   op
 *   [9]     u8   num_sge_key
 *   [10:11] be16 flags
 *   [12:15] be32 imm_data_key  (recv: total posted length)
 *   send common body:
 *     [16:19] be32 ah_id
 *     [20:23] be32 dest_qpn
 *     [24:27] be32 dest_qkey
 *     [28:31] be32 length
 *   recv body: [16:31] reserved
 *   [32...] payload: struct ionic_sge { be64 va; be32 len; be32 lkey; },
 *           after the speculative length table when the flags name one
 */
#define WQE_PLD_OFF      32
#define WQE_SEND_LEN_OFF 28

/* The datagram half of the send body: which address handle, and which
 * remote QPN. Connected sends ignore both. */
#define WQE_SEND_AH_OFF       16
#define WQE_SEND_DEST_QPN_OFF 20
#define WQE_SEND_QKEY_OFF     24

/* struct ionic_v1_common_bdy.rdma overlays the send body: two be32 halves of
 * the remote va at 16/20, high half first, so together one be64, then the
 * remote rkey, then the shared length. The atomic operands are split the
 * same way. */
#define WQE_RDMA_VA_OFF   16
#define WQE_RDMA_RKEY_OFF 24

/* ionic_v1_reg_mr_bdy, after the 16-byte base header: be64 va, be64 length,
 * be64 offset, be64 dma_addr, be32 map_count, be16 flags, u8 dir_size_log2,
 * u8 page_size_log2. */
#define WQE_REG_MR_VA_OFF         16
#define WQE_REG_MR_LENGTH_OFF     24
#define WQE_REG_MR_DMA_ADDR_OFF   40
#define WQE_REG_MR_MAP_COUNT_OFF  48
#define WQE_REG_MR_PAGE_SZ_L2_OFF 55
#define WQE_REG_MR_MIN_SZ         56

/* struct ionic_v1_atomic_bdy shares the first three fields with the rdma body
 * and then carries its operands and a single fixed 8-byte result SGE. */
#define WQE_ATOMIC_SWAP_ADD_OFF 28
#define WQE_ATOMIC_COMPARE_OFF  36
#define WQE_ATOMIC_SGE_OFF      48
#define WQE_ATOMIC_MIN_SZ       64 /* through the result SGE */

/* struct ionic_v1_cqe (32 bytes) */
#define CQE_SIZE          32
#define CQE_COLOR_BIT     0x01U
#define CQE_ERROR_BIT     0x02U
#define CQE_TYPE_RECV     (1U << 5)
#define CQE_TYPE_SEND_MSN (2U << 5)
#define CQE_TYPE_SEND_NPG (3U << 5)

/* enum ionic_v1_cqe_src_qpn_bits */
#define CQE_RECV_OP_SHIFT    24
#define CQE_RECV_OP_SEND     0
#define CQE_RECV_OP_SEND_INV 1
#define CQE_RECV_OP_SEND_IMM 2
#define CQE_RECV_OP_RDMA_IMM 3
#define CQE_RECV_IS_IPV4     (1U << (7 + CQE_RECV_OP_SHIFT))

/*
 * enum ionic_status from the guest driver's ionic_fw.h, which runs these
 * through ionic_to_ib_status() to reach an ib_wc_status.  LOCAL_LEN_ERR is a
 * receive buffer too small for the inbound message; LOCAL_PROT_ERR is an SGE
 * that does not lie inside the MR its own key names; MEM_MGMT_OPER_ERR is a
 * fast registration that bound nothing.
 */
#define IONIC_STS_OK                 0
#define IONIC_STS_LOCAL_LEN_ERR      1
#define IONIC_STS_LOCAL_QP_OPER_ERR  2
#define IONIC_STS_LOCAL_PROT_ERR     3
#define IONIC_STS_WQE_FLUSHED_ERR    4
#define IONIC_STS_MEM_MGMT_OPER_ERR  5
#define IONIC_STS_LOCAL_ACC_ERR      7
#define IONIC_STS_REMOTE_ACC_ERR     9
#define IONIC_STS_RETRY_EXCEEDED     11
#define IONIC_STS_RNR_RETRY_EXCEEDED 12

#endif /* IONIC_DATAPATH_FW_H */
