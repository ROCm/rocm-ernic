/*
 * Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
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

#ifndef __AMD_EMRDMA_DEV_API_H__
#define __AMD_EMRDMA_DEV_API_H__

#include <linux/types.h>

#include "amd_emrdma_verbs.h"

/*
 * AMD_EMRDMA version macros. Some new features require updates to AMD_EMRDMA_VERSION.
 * These macros allow us to check for different features if necessary.
 */

#define AMD_EMRDMA_ROCEV1_VERSION		17
#define AMD_EMRDMA_ROCEV2_VERSION		18
#define AMD_EMRDMA_PPN64_VERSION		19
#define AMD_EMRDMA_QPHANDLE_VERSION		20
#define AMD_EMRDMA_VERSION			AMD_EMRDMA_QPHANDLE_VERSION

#define AMD_EMRDMA_BOARD_ID			1
#define AMD_EMRDMA_REV_ID			1

/*
 * Masks and accessors for page directory, which is a two-level lookup:
 * page directory -> page table -> page. Only one directory for now, but we
 * could expand that easily. 9 bits for tables, 9 bits for pages, gives one
 * gigabyte for memory regions and so forth.
 */

#define AMD_EMRDMA_PDIR_SHIFT		18
#define AMD_EMRDMA_PTABLE_SHIFT		9
#define AMD_EMRDMA_PAGE_DIR_DIR(x)		(((x) >> AMD_EMRDMA_PDIR_SHIFT) & 0x1)
#define AMD_EMRDMA_PAGE_DIR_TABLE(x)	(((x) >> AMD_EMRDMA_PTABLE_SHIFT) & 0x1ff)
#define AMD_EMRDMA_PAGE_DIR_PAGE(x)		((x) & 0x1ff)
#define AMD_EMRDMA_PAGE_DIR_MAX_PAGES	(1 * 512 * 512)
#define AMD_EMRDMA_MAX_FAST_REG_PAGES	128

/*
 * Max MSI-X vectors.
 */

#define AMD_EMRDMA_MAX_INTERRUPTS	3

/* Register offsets within PCI resource on BAR1. */
#define AMD_EMRDMA_REG_VERSION	0x00	/* R: Version of device. */
#define AMD_EMRDMA_REG_DSRLOW	0x04	/* W: Device shared region low PA. */
#define AMD_EMRDMA_REG_DSRHIGH	0x08	/* W: Device shared region high PA. */
#define AMD_EMRDMA_REG_CTL		0x0c	/* W: AMD_EMRDMA_DEVICE_CTL */
#define AMD_EMRDMA_REG_REQUEST	0x10	/* W: Indicate device request. */
#define AMD_EMRDMA_REG_ERR		0x14	/* R: Device error. */
#define AMD_EMRDMA_REG_ICR		0x18	/* R: Interrupt cause. */
#define AMD_EMRDMA_REG_IMR		0x1c	/* R/W: Interrupt mask. */
#define AMD_EMRDMA_REG_MACL		0x20	/* R/W: MAC address low. */
#define AMD_EMRDMA_REG_MACH		0x24	/* R/W: MAC address high. */

/* Object flags. */
#define AMD_EMRDMA_CQ_FLAG_ARMED_SOL	BIT(0)	/* Armed for solicited-only. */
#define AMD_EMRDMA_CQ_FLAG_ARMED		BIT(1)	/* Armed. */
#define AMD_EMRDMA_MR_FLAG_DMA		BIT(0)	/* DMA region. */
#define AMD_EMRDMA_MR_FLAG_FRMR		BIT(1)	/* Fast reg memory region. */

/*
 * Atomic operation capability (masked versions are extended atomic
 * operations.
 */

#define AMD_EMRDMA_ATOMIC_OP_COMP_SWAP	BIT(0)	/* Compare and swap. */
#define AMD_EMRDMA_ATOMIC_OP_FETCH_ADD	BIT(1)	/* Fetch and add. */
#define AMD_EMRDMA_ATOMIC_OP_MASK_COMP_SWAP	BIT(2)	/* Masked compare and swap. */
#define AMD_EMRDMA_ATOMIC_OP_MASK_FETCH_ADD	BIT(3)	/* Masked fetch and add. */

/*
 * Base Memory Management Extension flags to support Fast Reg Memory Regions
 * and Fast Reg Work Requests. Each flag represents a verb operation and we
 * must support all of them to qualify for the BMME device cap.
 */

#define AMD_EMRDMA_BMME_FLAG_LOCAL_INV	BIT(0)	/* Local Invalidate. */
#define AMD_EMRDMA_BMME_FLAG_REMOTE_INV	BIT(1)	/* Remote Invalidate. */
#define AMD_EMRDMA_BMME_FLAG_FAST_REG_WR	BIT(2)	/* Fast Reg Work Request. */

/*
 * GID types. The interpretation of the gid_types bit field in the device
 * capabilities will depend on the device mode. For now, the device only
 * supports RoCE as mode, so only the different GID types for RoCE are
 * defined.
 */

#define AMD_EMRDMA_GID_TYPE_FLAG_ROCE_V1	BIT(0)
#define AMD_EMRDMA_GID_TYPE_FLAG_ROCE_V2	BIT(1)

/*
 * Version checks. This checks whether each version supports specific
 * capabilities from the device.
 */

#define AMD_EMRDMA_IS_VERSION17(_dev)					\
	(_dev->dsr_version == AMD_EMRDMA_ROCEV1_VERSION &&			\
	 _dev->dsr->caps.gid_types == AMD_EMRDMA_GID_TYPE_FLAG_ROCE_V1)

#define AMD_EMRDMA_IS_VERSION18(_dev)					\
	(_dev->dsr_version >= AMD_EMRDMA_ROCEV2_VERSION &&			\
	 (_dev->dsr->caps.gid_types == AMD_EMRDMA_GID_TYPE_FLAG_ROCE_V1 ||  \
	  _dev->dsr->caps.gid_types == AMD_EMRDMA_GID_TYPE_FLAG_ROCE_V2))	\

#define AMD_EMRDMA_SUPPORTED(_dev)						\
	((_dev->dsr->caps.mode == AMD_EMRDMA_DEVICE_MODE_ROCE) &&		\
	 (AMD_EMRDMA_IS_VERSION17(_dev) || AMD_EMRDMA_IS_VERSION18(_dev)))

/*
 * Get capability values based on device version.
 */

#define AMD_EMRDMA_GET_CAP(_dev, _old_val, _val) \
	((AMD_EMRDMA_IS_VERSION18(_dev)) ? _val : _old_val)

enum amd_emrdma_pci_resource {
	AMD_EMRDMA_PCI_RESOURCE_MSIX,	/* BAR0: MSI-X, MMIO. */
	AMD_EMRDMA_PCI_RESOURCE_REG,	/* BAR1: Registers, MMIO. */
	AMD_EMRDMA_PCI_RESOURCE_UAR,	/* BAR2: UAR pages, MMIO, 64-bit. */
	AMD_EMRDMA_PCI_RESOURCE_LAST,	/* Last. */
};

enum amd_emrdma_device_ctl {
	AMD_EMRDMA_DEVICE_CTL_ACTIVATE,	/* Activate device. */
	AMD_EMRDMA_DEVICE_CTL_UNQUIESCE,	/* Unquiesce device. */
	AMD_EMRDMA_DEVICE_CTL_RESET,	/* Reset device. */
};

enum amd_emrdma_intr_vector {
	AMD_EMRDMA_INTR_VECTOR_RESPONSE,	/* Command response. */
	AMD_EMRDMA_INTR_VECTOR_ASYNC,	/* Async events. */
	AMD_EMRDMA_INTR_VECTOR_CQ,		/* CQ notification. */
	/* Additional CQ notification vectors. */
};

enum amd_emrdma_intr_cause {
	AMD_EMRDMA_INTR_CAUSE_RESPONSE	= (1 << AMD_EMRDMA_INTR_VECTOR_RESPONSE),
	AMD_EMRDMA_INTR_CAUSE_ASYNC		= (1 << AMD_EMRDMA_INTR_VECTOR_ASYNC),
	AMD_EMRDMA_INTR_CAUSE_CQ		= (1 << AMD_EMRDMA_INTR_VECTOR_CQ),
};

enum amd_emrdma_gos_bits {
	AMD_EMRDMA_GOS_BITS_UNK,		/* Unknown. */
	AMD_EMRDMA_GOS_BITS_32,		/* 32-bit. */
	AMD_EMRDMA_GOS_BITS_64,		/* 64-bit. */
};

enum amd_emrdma_gos_type {
	AMD_EMRDMA_GOS_TYPE_UNK,		/* Unknown. */
	AMD_EMRDMA_GOS_TYPE_LINUX,		/* Linux. */
};

enum amd_emrdma_device_mode {
	AMD_EMRDMA_DEVICE_MODE_ROCE,	/* RoCE. */
	AMD_EMRDMA_DEVICE_MODE_IWARP,	/* iWarp. */
	AMD_EMRDMA_DEVICE_MODE_IB,		/* InfiniBand. */
};

struct amd_emrdma_gos_info {
	u32 gos_bits:2;			/* W: AMD_EMRDMA_GOS_BITS_ */
	u32 gos_type:4;			/* W: AMD_EMRDMA_GOS_TYPE_ */
	u32 gos_ver:16;			/* W: Guest OS version. */
	u32 gos_misc:10;		/* W: Other. */
	u32 pad;			/* Pad to 8-byte alignment. */
};

struct amd_emrdma_device_caps {
	u64 fw_ver;				/* R: Query device. */
	__be64 node_guid;
	__be64 sys_image_guid;
	u64 max_mr_size;
	u64 page_size_cap;
	u64 atomic_arg_sizes;			/* EX verbs. */
	u32 ex_comp_mask;			/* EX verbs. */
	u32 device_cap_flags2;			/* EX verbs. */
	u32 max_fa_bit_boundary;		/* EX verbs. */
	u32 log_max_atomic_inline_arg;		/* EX verbs. */
	u32 vendor_id;
	u32 vendor_part_id;
	u32 hw_ver;
	u32 max_qp;
	u32 max_qp_wr;
	u32 device_cap_flags;
	u32 max_sge;
	u32 max_sge_rd;
	u32 max_cq;
	u32 max_cqe;
	u32 max_mr;
	u32 max_pd;
	u32 max_qp_rd_atom;
	u32 max_ee_rd_atom;
	u32 max_res_rd_atom;
	u32 max_qp_init_rd_atom;
	u32 max_ee_init_rd_atom;
	u32 max_ee;
	u32 max_rdd;
	u32 max_mw;
	u32 max_raw_ipv6_qp;
	u32 max_raw_ethy_qp;
	u32 max_mcast_grp;
	u32 max_mcast_qp_attach;
	u32 max_total_mcast_qp_attach;
	u32 max_ah;
	u32 max_fmr;
	u32 max_map_per_fmr;
	u32 max_srq;
	u32 max_srq_wr;
	u32 max_srq_sge;
	u32 max_uar;
	u32 gid_tbl_len;
	u16 max_pkeys;
	u8  local_ca_ack_delay;
	u8  phys_port_cnt;
	u8  mode;				/* AMD_EMRDMA_DEVICE_MODE_ */
	u8  atomic_ops;				/* AMD_EMRDMA_ATOMIC_OP_* bits */
	u8  bmme_flags;				/* FRWR Mem Mgmt Extensions */
	u8  gid_types;				/* AMD_EMRDMA_GID_TYPE_FLAG_ */
	u32 max_fast_reg_page_list_len;
};

struct amd_emrdma_ring_page_info {
	u32 num_pages;				/* Num pages incl. header. */
	u32 reserved;				/* Reserved. */
	u64 pdir_dma;				/* Page directory PA. */
};

#pragma pack(push, 1)

struct amd_emrdma_device_shared_region {
	u32 driver_version;			/* W: Driver version. */
	u32 pad;				/* Pad to 8-byte align. */
	struct amd_emrdma_gos_info gos_info;	/* W: Guest OS information. */
	u64 cmd_slot_dma;			/* W: Command slot address. */
	u64 resp_slot_dma;			/* W: Response slot address. */
	struct amd_emrdma_ring_page_info async_ring_pages;
						/* W: Async ring page info. */
	struct amd_emrdma_ring_page_info cq_ring_pages;
						/* W: CQ ring page info. */
	union {
		u32 uar_pfn;			/* W: UAR pageframe. */
		u64 uar_pfn64;			/* W: 64-bit UAR page frame. */
	};
	struct amd_emrdma_device_caps caps;		/* R: Device capabilities. */
};

#pragma pack(pop)

/* Event types. Currently a 1:1 mapping with enum ib_event. */
enum amd_emrdma_eqe_type {
	AMD_EMRDMA_EVENT_CQ_ERR,
	AMD_EMRDMA_EVENT_QP_FATAL,
	AMD_EMRDMA_EVENT_QP_REQ_ERR,
	AMD_EMRDMA_EVENT_QP_ACCESS_ERR,
	AMD_EMRDMA_EVENT_COMM_EST,
	AMD_EMRDMA_EVENT_SQ_DRAINED,
	AMD_EMRDMA_EVENT_PATH_MIG,
	AMD_EMRDMA_EVENT_PATH_MIG_ERR,
	AMD_EMRDMA_EVENT_DEVICE_FATAL,
	AMD_EMRDMA_EVENT_PORT_ACTIVE,
	AMD_EMRDMA_EVENT_PORT_ERR,
	AMD_EMRDMA_EVENT_LID_CHANGE,
	AMD_EMRDMA_EVENT_PKEY_CHANGE,
	AMD_EMRDMA_EVENT_SM_CHANGE,
	AMD_EMRDMA_EVENT_SRQ_ERR,
	AMD_EMRDMA_EVENT_SRQ_LIMIT_REACHED,
	AMD_EMRDMA_EVENT_QP_LAST_WQE_REACHED,
	AMD_EMRDMA_EVENT_CLIENT_REREGISTER,
	AMD_EMRDMA_EVENT_GID_CHANGE,
};

/* Event queue element. */
struct amd_emrdma_eqe {
	u32 type;	/* Event type. */
	u32 info;	/* Handle, other. */
};

/* CQ notification queue element. */
struct amd_emrdma_cqne {
	u32 info;	/* Handle */
};

enum {
	AMD_EMRDMA_CMD_FIRST,
	AMD_EMRDMA_CMD_QUERY_PORT = AMD_EMRDMA_CMD_FIRST,
	AMD_EMRDMA_CMD_QUERY_PKEY,
	AMD_EMRDMA_CMD_CREATE_PD,
	AMD_EMRDMA_CMD_DESTROY_PD,
	AMD_EMRDMA_CMD_CREATE_MR,
	AMD_EMRDMA_CMD_DESTROY_MR,
	AMD_EMRDMA_CMD_CREATE_CQ,
	AMD_EMRDMA_CMD_RESIZE_CQ,
	AMD_EMRDMA_CMD_DESTROY_CQ,
	AMD_EMRDMA_CMD_CREATE_QP,
	AMD_EMRDMA_CMD_MODIFY_QP,
	AMD_EMRDMA_CMD_QUERY_QP,
	AMD_EMRDMA_CMD_DESTROY_QP,
	AMD_EMRDMA_CMD_CREATE_UC,
	AMD_EMRDMA_CMD_DESTROY_UC,
	AMD_EMRDMA_CMD_CREATE_BIND,
	AMD_EMRDMA_CMD_DESTROY_BIND,
	AMD_EMRDMA_CMD_CREATE_SRQ,
	AMD_EMRDMA_CMD_MODIFY_SRQ,
	AMD_EMRDMA_CMD_QUERY_SRQ,
	AMD_EMRDMA_CMD_DESTROY_SRQ,
	AMD_EMRDMA_CMD_MAX,
};

enum {
	AMD_EMRDMA_CMD_FIRST_RESP = (1 << 31),
	AMD_EMRDMA_CMD_QUERY_PORT_RESP = AMD_EMRDMA_CMD_FIRST_RESP,
	AMD_EMRDMA_CMD_QUERY_PKEY_RESP,
	AMD_EMRDMA_CMD_CREATE_PD_RESP,
	AMD_EMRDMA_CMD_DESTROY_PD_RESP_NOOP,
	AMD_EMRDMA_CMD_CREATE_MR_RESP,
	AMD_EMRDMA_CMD_DESTROY_MR_RESP_NOOP,
	AMD_EMRDMA_CMD_CREATE_CQ_RESP,
	AMD_EMRDMA_CMD_RESIZE_CQ_RESP,
	AMD_EMRDMA_CMD_DESTROY_CQ_RESP_NOOP,
	AMD_EMRDMA_CMD_CREATE_QP_RESP,
	AMD_EMRDMA_CMD_MODIFY_QP_RESP,
	AMD_EMRDMA_CMD_QUERY_QP_RESP,
	AMD_EMRDMA_CMD_DESTROY_QP_RESP,
	AMD_EMRDMA_CMD_CREATE_UC_RESP,
	AMD_EMRDMA_CMD_DESTROY_UC_RESP_NOOP,
	AMD_EMRDMA_CMD_CREATE_BIND_RESP_NOOP,
	AMD_EMRDMA_CMD_DESTROY_BIND_RESP_NOOP,
	AMD_EMRDMA_CMD_CREATE_SRQ_RESP,
	AMD_EMRDMA_CMD_MODIFY_SRQ_RESP,
	AMD_EMRDMA_CMD_QUERY_SRQ_RESP,
	AMD_EMRDMA_CMD_DESTROY_SRQ_RESP,
	AMD_EMRDMA_CMD_MAX_RESP,
};

struct amd_emrdma_cmd_hdr {
	u64 response;		/* Key for response lookup. */
	u32 cmd;		/* AMD_EMRDMA_CMD_ */
	u32 reserved;		/* Reserved. */
};

struct amd_emrdma_cmd_resp_hdr {
	u64 response;		/* From cmd hdr. */
	u32 ack;		/* AMD_EMRDMA_CMD_XXX_RESP */
	u8 err;			/* Error. */
	u8 reserved[3];		/* Reserved. */
};

struct amd_emrdma_cmd_query_port {
	struct amd_emrdma_cmd_hdr hdr;
	u8 port_num;
	u8 reserved[7];
};

struct amd_emrdma_cmd_query_port_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	struct amd_emrdma_port_attr attrs;
};

struct amd_emrdma_cmd_query_pkey {
	struct amd_emrdma_cmd_hdr hdr;
	u8 port_num;
	u8 index;
	u8 reserved[6];
};

struct amd_emrdma_cmd_query_pkey_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u16 pkey;
	u8 reserved[6];
};

struct amd_emrdma_cmd_create_uc {
	struct amd_emrdma_cmd_hdr hdr;
	union {
		u32 pfn; /* UAR page frame number */
		u64 pfn64; /* 64-bit UAR page frame number */
	};
};

struct amd_emrdma_cmd_create_uc_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 ctx_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_destroy_uc {
	struct amd_emrdma_cmd_hdr hdr;
	u32 ctx_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_pd {
	struct amd_emrdma_cmd_hdr hdr;
	u32 ctx_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_pd_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 pd_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_destroy_pd {
	struct amd_emrdma_cmd_hdr hdr;
	u32 pd_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_mr {
	struct amd_emrdma_cmd_hdr hdr;
	u64 start;
	u64 length;
	u64 pdir_dma;
	u32 pd_handle;
	u32 access_flags;
	u32 flags;
	u32 nchunks;
};

struct amd_emrdma_cmd_create_mr_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 mr_handle;
	u32 lkey;
	u32 rkey;
	u8 reserved[4];
};

struct amd_emrdma_cmd_destroy_mr {
	struct amd_emrdma_cmd_hdr hdr;
	u32 mr_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_cq {
	struct amd_emrdma_cmd_hdr hdr;
	u64 pdir_dma;
	u32 ctx_handle;
	u32 cqe;
	u32 nchunks;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_cq_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 cq_handle;
	u32 cqe;
};

struct amd_emrdma_cmd_resize_cq {
	struct amd_emrdma_cmd_hdr hdr;
	u32 cq_handle;
	u32 cqe;
};

struct amd_emrdma_cmd_resize_cq_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 cqe;
	u8 reserved[4];
};

struct amd_emrdma_cmd_destroy_cq {
	struct amd_emrdma_cmd_hdr hdr;
	u32 cq_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_srq {
	struct amd_emrdma_cmd_hdr hdr;
	u64 pdir_dma;
	u32 pd_handle;
	u32 nchunks;
	struct amd_emrdma_srq_attr attrs;
	u8 srq_type;
	u8 reserved[7];
};

struct amd_emrdma_cmd_create_srq_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 srqn;
	u8 reserved[4];
};

struct amd_emrdma_cmd_modify_srq {
	struct amd_emrdma_cmd_hdr hdr;
	u32 srq_handle;
	u32 attr_mask;
	struct amd_emrdma_srq_attr attrs;
};

struct amd_emrdma_cmd_query_srq {
	struct amd_emrdma_cmd_hdr hdr;
	u32 srq_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_query_srq_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	struct amd_emrdma_srq_attr attrs;
};

struct amd_emrdma_cmd_destroy_srq {
	struct amd_emrdma_cmd_hdr hdr;
	u32 srq_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_qp {
	struct amd_emrdma_cmd_hdr hdr;
	u64 pdir_dma;
	u32 pd_handle;
	u32 send_cq_handle;
	u32 recv_cq_handle;
	u32 srq_handle;
	u32 max_send_wr;
	u32 max_recv_wr;
	u32 max_send_sge;
	u32 max_recv_sge;
	u32 max_inline_data;
	u32 lkey;
	u32 access_flags;
	u16 total_chunks;
	u16 send_chunks;
	u16 max_atomic_arg;
	u8 sq_sig_all;
	u8 qp_type;
	u8 is_srq;
	u8 reserved[3];
};

struct amd_emrdma_cmd_create_qp_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 qpn;
	u32 max_send_wr;
	u32 max_recv_wr;
	u32 max_send_sge;
	u32 max_recv_sge;
	u32 max_inline_data;
};

struct amd_emrdma_cmd_create_qp_resp_v2 {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 qpn;
	u32 qp_handle;
	u32 max_send_wr;
	u32 max_recv_wr;
	u32 max_send_sge;
	u32 max_recv_sge;
	u32 max_inline_data;
};

struct amd_emrdma_cmd_modify_qp {
	struct amd_emrdma_cmd_hdr hdr;
	u32 qp_handle;
	u32 attr_mask;
	struct amd_emrdma_qp_attr attrs;
};

struct amd_emrdma_cmd_query_qp {
	struct amd_emrdma_cmd_hdr hdr;
	u32 qp_handle;
	u32 attr_mask;
};

struct amd_emrdma_cmd_query_qp_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	struct amd_emrdma_qp_attr attrs;
};

struct amd_emrdma_cmd_destroy_qp {
	struct amd_emrdma_cmd_hdr hdr;
	u32 qp_handle;
	u8 reserved[4];
};

struct amd_emrdma_cmd_destroy_qp_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	u32 events_reported;
	u8 reserved[4];
};

struct amd_emrdma_cmd_create_bind {
	struct amd_emrdma_cmd_hdr hdr;
	u32 mtu;
	u32 vlan;
	u32 index;
	u8 new_gid[16];
	u8 gid_type;
	u8 reserved[3];
};

struct amd_emrdma_cmd_destroy_bind {
	struct amd_emrdma_cmd_hdr hdr;
	u32 index;
	u8 dest_gid[16];
	u8 reserved[4];
};

union amd_emrdma_cmd_req {
	struct amd_emrdma_cmd_hdr hdr;
	struct amd_emrdma_cmd_query_port query_port;
	struct amd_emrdma_cmd_query_pkey query_pkey;
	struct amd_emrdma_cmd_create_uc create_uc;
	struct amd_emrdma_cmd_destroy_uc destroy_uc;
	struct amd_emrdma_cmd_create_pd create_pd;
	struct amd_emrdma_cmd_destroy_pd destroy_pd;
	struct amd_emrdma_cmd_create_mr create_mr;
	struct amd_emrdma_cmd_destroy_mr destroy_mr;
	struct amd_emrdma_cmd_create_cq create_cq;
	struct amd_emrdma_cmd_resize_cq resize_cq;
	struct amd_emrdma_cmd_destroy_cq destroy_cq;
	struct amd_emrdma_cmd_create_qp create_qp;
	struct amd_emrdma_cmd_modify_qp modify_qp;
	struct amd_emrdma_cmd_query_qp query_qp;
	struct amd_emrdma_cmd_destroy_qp destroy_qp;
	struct amd_emrdma_cmd_create_bind create_bind;
	struct amd_emrdma_cmd_destroy_bind destroy_bind;
	struct amd_emrdma_cmd_create_srq create_srq;
	struct amd_emrdma_cmd_modify_srq modify_srq;
	struct amd_emrdma_cmd_query_srq query_srq;
	struct amd_emrdma_cmd_destroy_srq destroy_srq;
};

union amd_emrdma_cmd_resp {
	struct amd_emrdma_cmd_resp_hdr hdr;
	struct amd_emrdma_cmd_query_port_resp query_port_resp;
	struct amd_emrdma_cmd_query_pkey_resp query_pkey_resp;
	struct amd_emrdma_cmd_create_uc_resp create_uc_resp;
	struct amd_emrdma_cmd_create_pd_resp create_pd_resp;
	struct amd_emrdma_cmd_create_mr_resp create_mr_resp;
	struct amd_emrdma_cmd_create_cq_resp create_cq_resp;
	struct amd_emrdma_cmd_resize_cq_resp resize_cq_resp;
	struct amd_emrdma_cmd_create_qp_resp create_qp_resp;
	struct amd_emrdma_cmd_create_qp_resp_v2 create_qp_resp_v2;
	struct amd_emrdma_cmd_query_qp_resp query_qp_resp;
	struct amd_emrdma_cmd_destroy_qp_resp destroy_qp_resp;
	struct amd_emrdma_cmd_create_srq_resp create_srq_resp;
	struct amd_emrdma_cmd_query_srq_resp query_srq_resp;
};

#endif /* __AMD_EMRDMA_DEV_API_H__ */
