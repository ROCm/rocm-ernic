/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause)
 */
/*
 * Copyright (c) 2012-2016 AMD, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of EITHER the GNU General Public License
 * version 2 as published by the Free Software Foundation or the BSD
 * 2-Clause License. This program is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; WITHOUT EVEN THE IMPLIED
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License version 2 for more details at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program available in the file COPYING in the main
 * directory of this source tree.
 *
 * The BSD 2-Clause License
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AMD_EMRDMA_ABI_H__
#define __AMD_EMRDMA_ABI_H__

#include <linux/types.h>

#define AMD_EMRDMA_UVERBS_ABI_VERSION 3          /* ABI Version. */
#define AMD_EMRDMA_UAR_HANDLE_MASK    0x00FFFFFF /* Bottom 24 bits. */
#define AMD_EMRDMA_UAR_QP_OFFSET      0          /* QP doorbell. */
#define AMD_EMRDMA_UAR_QP_SEND        (1 << 30)  /* Send bit. */
#define AMD_EMRDMA_UAR_QP_RECV        (1 << 31)  /* Recv bit. */
#define AMD_EMRDMA_UAR_CQ_OFFSET      4          /* CQ doorbell. */
#define AMD_EMRDMA_UAR_CQ_ARM_SOL     (1 << 29)  /* Arm solicited bit. */
#define AMD_EMRDMA_UAR_CQ_ARM         (1 << 30)  /* Arm bit. */
#define AMD_EMRDMA_UAR_CQ_POLL        (1 << 31)  /* Poll bit. */
#define AMD_EMRDMA_UAR_SRQ_OFFSET     8          /* SRQ doorbell. */
#define AMD_EMRDMA_UAR_SRQ_RECV       (1 << 30)  /* Recv bit. */

enum amd_emrdma_wr_opcode {
    AMD_EMRDMA_WR_RDMA_WRITE,
    AMD_EMRDMA_WR_RDMA_WRITE_WITH_IMM,
    AMD_EMRDMA_WR_SEND,
    AMD_EMRDMA_WR_SEND_WITH_IMM,
    AMD_EMRDMA_WR_RDMA_READ,
    AMD_EMRDMA_WR_ATOMIC_CMP_AND_SWP,
    AMD_EMRDMA_WR_ATOMIC_FETCH_AND_ADD,
    AMD_EMRDMA_WR_LSO,
    AMD_EMRDMA_WR_SEND_WITH_INV,
    AMD_EMRDMA_WR_RDMA_READ_WITH_INV,
    AMD_EMRDMA_WR_LOCAL_INV,
    AMD_EMRDMA_WR_FAST_REG_MR,
    AMD_EMRDMA_WR_MASKED_ATOMIC_CMP_AND_SWP,
    AMD_EMRDMA_WR_MASKED_ATOMIC_FETCH_AND_ADD,
    AMD_EMRDMA_WR_BIND_MW,
    AMD_EMRDMA_WR_REG_SIG_MR,
    AMD_EMRDMA_WR_ERROR,
};

enum amd_emrdma_wc_status {
    AMD_EMRDMA_WC_SUCCESS,
    AMD_EMRDMA_WC_LOC_LEN_ERR,
    AMD_EMRDMA_WC_LOC_QP_OP_ERR,
    AMD_EMRDMA_WC_LOC_EEC_OP_ERR,
    AMD_EMRDMA_WC_LOC_PROT_ERR,
    AMD_EMRDMA_WC_WR_FLUSH_ERR,
    AMD_EMRDMA_WC_MW_BIND_ERR,
    AMD_EMRDMA_WC_BAD_RESP_ERR,
    AMD_EMRDMA_WC_LOC_ACCESS_ERR,
    AMD_EMRDMA_WC_REM_INV_REQ_ERR,
    AMD_EMRDMA_WC_REM_ACCESS_ERR,
    AMD_EMRDMA_WC_REM_OP_ERR,
    AMD_EMRDMA_WC_RETRY_EXC_ERR,
    AMD_EMRDMA_WC_RNR_RETRY_EXC_ERR,
    AMD_EMRDMA_WC_LOC_RDD_VIOL_ERR,
    AMD_EMRDMA_WC_REM_INV_RD_REQ_ERR,
    AMD_EMRDMA_WC_REM_ABORT_ERR,
    AMD_EMRDMA_WC_INV_EECN_ERR,
    AMD_EMRDMA_WC_INV_EEC_STATE_ERR,
    AMD_EMRDMA_WC_FATAL_ERR,
    AMD_EMRDMA_WC_RESP_TIMEOUT_ERR,
    AMD_EMRDMA_WC_GENERAL_ERR,
};

enum amd_emrdma_wc_opcode {
    AMD_EMRDMA_WC_SEND,
    AMD_EMRDMA_WC_RDMA_WRITE,
    AMD_EMRDMA_WC_RDMA_READ,
    AMD_EMRDMA_WC_COMP_SWAP,
    AMD_EMRDMA_WC_FETCH_ADD,
    AMD_EMRDMA_WC_BIND_MW,
    AMD_EMRDMA_WC_LSO,
    AMD_EMRDMA_WC_LOCAL_INV,
    AMD_EMRDMA_WC_FAST_REG_MR,
    AMD_EMRDMA_WC_MASKED_COMP_SWAP,
    AMD_EMRDMA_WC_MASKED_FETCH_ADD,
    AMD_EMRDMA_WC_RECV = 1 << 7,
    AMD_EMRDMA_WC_RECV_RDMA_WITH_IMM,
};

enum amd_emrdma_wc_flags {
    AMD_EMRDMA_WC_GRH = 1 << 0,
    AMD_EMRDMA_WC_WITH_IMM = 1 << 1,
    AMD_EMRDMA_WC_WITH_INVALIDATE = 1 << 2,
    AMD_EMRDMA_WC_IP_CSUM_OK = 1 << 3,
    AMD_EMRDMA_WC_WITH_SMAC = 1 << 4,
    AMD_EMRDMA_WC_WITH_VLAN = 1 << 5,
    AMD_EMRDMA_WC_WITH_NETWORK_HDR_TYPE = 1 << 6,
    AMD_EMRDMA_WC_FLAGS_MAX = AMD_EMRDMA_WC_WITH_NETWORK_HDR_TYPE,
};

enum amd_emrdma_network_type {
    AMD_EMRDMA_NETWORK_IB,
    AMD_EMRDMA_NETWORK_ROCE_V1 = AMD_EMRDMA_NETWORK_IB,
    AMD_EMRDMA_NETWORK_IPV4,
    AMD_EMRDMA_NETWORK_IPV6
};

struct amd_emrdma_alloc_ucontext_resp {
    __u32 qp_tab_size;
    __u32 reserved;
};

struct amd_emrdma_alloc_pd_resp {
    __u32 pdn;
    __u32 reserved;
};

struct amd_emrdma_create_cq {
    __aligned_u64 buf_addr;
    __u32 buf_size;
    __u32 reserved;
};

struct amd_emrdma_create_cq_resp {
    __u32 cqn;
    __u32 reserved;
};

struct amd_emrdma_resize_cq {
    __aligned_u64 buf_addr;
    __u32 buf_size;
    __u32 reserved;
};

struct amd_emrdma_create_srq {
    __aligned_u64 buf_addr;
    __u32 buf_size;
    __u32 reserved;
};

struct amd_emrdma_create_srq_resp {
    __u32 srqn;
    __u32 reserved;
};

struct amd_emrdma_create_qp {
    __aligned_u64 rbuf_addr;
    __aligned_u64 sbuf_addr;
    __u32 rbuf_size;
    __u32 sbuf_size;
    __aligned_u64 qp_addr;
};

struct amd_emrdma_create_qp_resp {
    __u32 qpn;
    __u32 qp_handle;
};

/* AMD_EMRDMA masked atomic compare and swap */
struct amd_emrdma_ex_cmp_swap {
    __aligned_u64 swap_val;
    __aligned_u64 compare_val;
    __aligned_u64 swap_mask;
    __aligned_u64 compare_mask;
};

/* AMD_EMRDMA masked atomic fetch and add */
struct amd_emrdma_ex_fetch_add {
    __aligned_u64 add_val;
    __aligned_u64 field_boundary;
};

/* AMD_EMRDMA address vector. */
struct amd_emrdma_av {
    __u32 port_pd;
    __u32 sl_tclass_flowlabel;
    __u8 dgid[16];
    __u8 src_path_bits;
    __u8 gid_index;
    __u8 stat_rate;
    __u8 hop_limit;
    __u8 dmac[6];
    __u8 reserved[6];
};

/* AMD_EMRDMA scatter/gather entry */
struct amd_emrdma_sge {
    __aligned_u64 addr;
    __u32 length;
    __u32 lkey;
};

/* AMD_EMRDMA receive queue work request */
struct amd_emrdma_rq_wqe_hdr {
    __aligned_u64 wr_id; /* wr id */
    __u32 num_sge;       /* size of s/g array */
    __u32 total_len;     /* reserved */
};
/* Use amd_emrdma_sge (ib_sge) for receive queue s/g array elements. */

/* AMD_EMRDMA send queue work request */
struct amd_emrdma_sq_wqe_hdr {
    __aligned_u64 wr_id; /* wr id */
    __u32 num_sge;       /* size of s/g array */
    __u32 total_len;     /* reserved */
    __u32 opcode;        /* operation type */
    __u32 send_flags;    /* wr flags */
    union {
        __be32 imm_data;
        __u32 invalidate_rkey;
    } ex;
    __u32 reserved;
    union {
        struct {
            __aligned_u64 remote_addr;
            __u32 rkey;
            __u8 reserved[4];
        } rdma;
        struct {
            __aligned_u64 remote_addr;
            __aligned_u64 compare_add;
            __aligned_u64 swap;
            __u32 rkey;
            __u32 reserved;
        } atomic;
        struct {
            __aligned_u64 remote_addr;
            __u32 log_arg_sz;
            __u32 rkey;
            union {
                struct amd_emrdma_ex_cmp_swap cmp_swap;
                struct amd_emrdma_ex_fetch_add fetch_add;
            } wr_data;
        } masked_atomics;
        struct {
            __aligned_u64 iova_start;
            __aligned_u64 pl_pdir_dma;
            __u32 page_shift;
            __u32 page_list_len;
            __u32 length;
            __u32 access_flags;
            __u32 rkey;
            __u32 reserved;
        } fast_reg;
        struct {
            __u32 remote_qpn;
            __u32 remote_qkey;
            struct amd_emrdma_av av;
        } ud;
    } wr;
};
/* Use amd_emrdma_sge (ib_sge) for send queue s/g array elements. */

/* Completion queue element. */
struct amd_emrdma_cqe {
    __aligned_u64 wr_id;
    __aligned_u64 qp;
    __u32 opcode;
    __u32 status;
    __u32 byte_len;
    __be32 imm_data;
    __u32 src_qp;
    __u32 wc_flags;
    __u32 vendor_err;
    __u16 pkey_index;
    __u16 slid;
    __u8 sl;
    __u8 dlid_path_bits;
    __u8 port_num;
    __u8 smac[6];
    __u8 network_hdr_type;
    __u8 reserved2[6]; /* Pad to next power of 2 (64). */
};

#endif /* __AMD_EMRDMA_ABI_H__ */
