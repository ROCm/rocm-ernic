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

#ifndef __AMD_EMRDMA_H__
#define __AMD_EMRDMA_H__

#include <linux/compiler.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>
#include "amd_emrdma-abi.h"

#include "amd_emrdma_ring.h"
#include "amd_emrdma_dev_api.h"
#include "amd_emrdma_verbs.h"

/* NOT the same as BIT_MASK(). */
#define AMD_EMRDMA_MASK(n) ((n << 1) - 1)

/*
 * VMware AMD_EMRDMA PCI device id.
 */
#define PCI_DEVICE_ID_VMWARE_AMD_EMRDMA	0x0820

#define AMD_EMRDMA_NUM_RING_PAGES		4
#define AMD_EMRDMA_QP_NUM_HEADER_PAGES	1

struct amd_emrdma_dev;

struct amd_emrdma_page_dir {
	dma_addr_t dir_dma;
	u64 *dir;
	int ntables;
	u64 **tables;
	u64 npages;
	void **pages;
};

struct amd_emrdma_cq {
	struct ib_cq ibcq;
	int offset;
	spinlock_t cq_lock; /* Poll lock. */
	struct amd_emrdma_uar_map *uar;
	struct ib_umem *umem;
	struct amd_emrdma_ring_state *ring_state;
	struct amd_emrdma_page_dir pdir;
	u32 cq_handle;
	bool is_kernel;
	refcount_t refcnt;
	struct completion free;
};

struct amd_emrdma_id_table {
	u32 last;
	u32 top;
	u32 max;
	u32 mask;
	spinlock_t lock; /* Table lock. */
	unsigned long *table;
};

struct amd_emrdma_uar_map {
	unsigned long pfn;
	void __iomem *map;
	int index;
};

struct amd_emrdma_uar_table {
	struct amd_emrdma_id_table tbl;
	int size;
};

struct amd_emrdma_ucontext {
	struct ib_ucontext ibucontext;
	struct amd_emrdma_dev *dev;
	struct amd_emrdma_uar_map uar;
	u64 ctx_handle;
};

struct amd_emrdma_pd {
	struct ib_pd ibpd;
	u32 pdn;
	u32 pd_handle;
	int privileged;
};

struct amd_emrdma_mr {
	u32 mr_handle;
	u64 iova;
	u64 size;
};

struct amd_emrdma_user_mr {
	struct ib_mr ibmr;
	struct ib_umem *umem;
	struct amd_emrdma_mr mmr;
	struct amd_emrdma_page_dir pdir;
	u64 *pages;
	u32 npages;
	u32 max_pages;
	u32 page_shift;
};

struct amd_emrdma_wq {
	struct amd_emrdma_ring *ring;
	spinlock_t lock; /* Work queue lock. */
	int wqe_cnt;
	int wqe_size;
	int max_sg;
	int offset;
};

struct amd_emrdma_ah {
	struct ib_ah ibah;
	struct amd_emrdma_av av;
};

struct amd_emrdma_srq {
	struct ib_srq ibsrq;
	int offset;
	spinlock_t lock; /* SRQ lock. */
	int wqe_cnt;
	int wqe_size;
	int max_gs;
	struct ib_umem *umem;
	struct amd_emrdma_ring_state *ring;
	struct amd_emrdma_page_dir pdir;
	u32 srq_handle;
	int npages;
	refcount_t refcnt;
	struct completion free;
};

struct amd_emrdma_qp {
	struct ib_qp ibqp;
	u32 qp_handle;
	u32 qkey;
	struct amd_emrdma_wq sq;
	struct amd_emrdma_wq rq;
	struct ib_umem *rumem;
	struct ib_umem *sumem;
	struct amd_emrdma_page_dir pdir;
	struct amd_emrdma_srq *srq;
	int npages;
	int npages_send;
	int npages_recv;
	u32 flags;
	u8 port;
	u8 state;
	bool is_kernel;
	struct mutex mutex; /* QP state mutex. */
	refcount_t refcnt;
	struct completion free;
};

struct amd_emrdma_dev {
	/* PCI device-related information. */
	struct ib_device ib_dev;
	struct pci_dev *pdev;
	void __iomem *regs;
	struct amd_emrdma_device_shared_region *dsr; /* Shared region pointer */
	dma_addr_t dsrbase; /* Shared region base address */
	void *cmd_slot;
	void *resp_slot;
	unsigned long flags;
	struct list_head device_link;
	unsigned int dsr_version;

	/* Locking and interrupt information. */
	spinlock_t cmd_lock; /* Command lock. */
	struct semaphore cmd_sema;
	struct completion cmd_done;
	unsigned int nr_vectors;

	/* RDMA-related device information. */
	union ib_gid *sgid_tbl;
	struct amd_emrdma_ring_state *async_ring_state;
	struct amd_emrdma_page_dir async_pdir;
	struct amd_emrdma_ring_state *cq_ring_state;
	struct amd_emrdma_page_dir cq_pdir;
	struct amd_emrdma_cq **cq_tbl;
	spinlock_t cq_tbl_lock;
	struct amd_emrdma_srq **srq_tbl;
	spinlock_t srq_tbl_lock;
	struct amd_emrdma_qp **qp_tbl;
	spinlock_t qp_tbl_lock;
	struct amd_emrdma_uar_table uar_table;
	struct amd_emrdma_uar_map driver_uar;
	__be64 sys_image_guid;
	spinlock_t desc_lock; /* Device modification lock. */
	u32 port_cap_mask;
	struct mutex port_mutex; /* Port modification mutex. */
	bool ib_active;
	atomic_t num_qps;
	atomic_t num_cqs;
	atomic_t num_srqs;
	atomic_t num_pds;
	atomic_t num_ahs;

	/* Network device information. */
	struct net_device *netdev;
	struct notifier_block nb_netdev;
};

struct amd_emrdma_netdevice_work {
	struct work_struct work;
	struct net_device *event_netdev;
	unsigned long event;
};

static inline struct amd_emrdma_dev *to_vdev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct amd_emrdma_dev, ib_dev);
}

static inline struct
amd_emrdma_ucontext *to_vucontext(struct ib_ucontext *ibucontext)
{
	return container_of(ibucontext, struct amd_emrdma_ucontext, ibucontext);
}

static inline struct amd_emrdma_pd *to_vpd(struct ib_pd *ibpd)
{
	return container_of(ibpd, struct amd_emrdma_pd, ibpd);
}

static inline struct amd_emrdma_cq *to_vcq(struct ib_cq *ibcq)
{
	return container_of(ibcq, struct amd_emrdma_cq, ibcq);
}

static inline struct amd_emrdma_srq *to_vsrq(struct ib_srq *ibsrq)
{
	return container_of(ibsrq, struct amd_emrdma_srq, ibsrq);
}

static inline struct amd_emrdma_user_mr *to_vmr(struct ib_mr *ibmr)
{
	return container_of(ibmr, struct amd_emrdma_user_mr, ibmr);
}

static inline struct amd_emrdma_qp *to_vqp(struct ib_qp *ibqp)
{
	return container_of(ibqp, struct amd_emrdma_qp, ibqp);
}

static inline struct amd_emrdma_ah *to_vah(struct ib_ah *ibah)
{
	return container_of(ibah, struct amd_emrdma_ah, ibah);
}

static inline void amd_emrdma_write_reg(struct amd_emrdma_dev *dev, u32 reg, u32 val)
{
	writel(cpu_to_le32(val), dev->regs + reg);
}

static inline u32 amd_emrdma_read_reg(struct amd_emrdma_dev *dev, u32 reg)
{
	return le32_to_cpu(readl(dev->regs + reg));
}

static inline void amd_emrdma_write_uar_cq(struct amd_emrdma_dev *dev, u32 val)
{
	writel(cpu_to_le32(val), dev->driver_uar.map + AMD_EMRDMA_UAR_CQ_OFFSET);
}

static inline void amd_emrdma_write_uar_qp(struct amd_emrdma_dev *dev, u32 val)
{
	writel(cpu_to_le32(val), dev->driver_uar.map + AMD_EMRDMA_UAR_QP_OFFSET);
}

static inline void *amd_emrdma_page_dir_get_ptr(struct amd_emrdma_page_dir *pdir,
					    u64 offset)
{
	return pdir->pages[offset / PAGE_SIZE] + (offset % PAGE_SIZE);
}

static inline enum amd_emrdma_mtu ib_mtu_to_amd_emrdma(enum ib_mtu mtu)
{
	return (enum amd_emrdma_mtu)mtu;
}

static inline enum ib_mtu amd_emrdma_mtu_to_ib(enum amd_emrdma_mtu mtu)
{
	return (enum ib_mtu)mtu;
}

static inline enum amd_emrdma_port_state ib_port_state_to_amd_emrdma(
					enum ib_port_state state)
{
	return (enum amd_emrdma_port_state)state;
}

static inline enum ib_port_state amd_emrdma_port_state_to_ib(
					enum amd_emrdma_port_state state)
{
	return (enum ib_port_state)state;
}

static inline int amd_emrdma_port_cap_flags_to_ib(int flags)
{
	return flags;
}

static inline enum amd_emrdma_port_width ib_port_width_to_amd_emrdma(
					enum ib_port_width width)
{
	return (enum amd_emrdma_port_width)width;
}

static inline enum ib_port_width amd_emrdma_port_width_to_ib(
					enum amd_emrdma_port_width width)
{
	return (enum ib_port_width)width;
}

static inline enum amd_emrdma_port_speed ib_port_speed_to_amd_emrdma(
					enum ib_port_speed speed)
{
	return (enum amd_emrdma_port_speed)speed;
}

static inline enum ib_port_speed amd_emrdma_port_speed_to_ib(
					enum amd_emrdma_port_speed speed)
{
	return (enum ib_port_speed)speed;
}

static inline int ib_qp_attr_mask_to_amd_emrdma(int attr_mask)
{
	return attr_mask & AMD_EMRDMA_MASK(AMD_EMRDMA_QP_ATTR_MASK_MAX);
}

static inline enum amd_emrdma_mig_state ib_mig_state_to_amd_emrdma(
					enum ib_mig_state state)
{
	return (enum amd_emrdma_mig_state)state;
}

static inline enum ib_mig_state amd_emrdma_mig_state_to_ib(
					enum amd_emrdma_mig_state state)
{
	return (enum ib_mig_state)state;
}

static inline int ib_access_flags_to_amd_emrdma(int flags)
{
	return flags;
}

static inline int amd_emrdma_access_flags_to_ib(int flags)
{
	return flags & AMD_EMRDMA_MASK(AMD_EMRDMA_ACCESS_FLAGS_MAX);
}

static inline enum amd_emrdma_qp_type ib_qp_type_to_amd_emrdma(enum ib_qp_type type)
{
	return (enum amd_emrdma_qp_type)type;
}

static inline enum amd_emrdma_qp_state ib_qp_state_to_amd_emrdma(enum ib_qp_state state)
{
	return (enum amd_emrdma_qp_state)state;
}

static inline enum ib_qp_state amd_emrdma_qp_state_to_ib(enum amd_emrdma_qp_state state)
{
	return (enum ib_qp_state)state;
}

static inline enum amd_emrdma_wr_opcode ib_wr_opcode_to_amd_emrdma(enum ib_wr_opcode op)
{
	switch (op) {
	case IB_WR_RDMA_WRITE:
		return AMD_EMRDMA_WR_RDMA_WRITE;
	case IB_WR_RDMA_WRITE_WITH_IMM:
		return AMD_EMRDMA_WR_RDMA_WRITE_WITH_IMM;
	case IB_WR_SEND:
		return AMD_EMRDMA_WR_SEND;
	case IB_WR_SEND_WITH_IMM:
		return AMD_EMRDMA_WR_SEND_WITH_IMM;
	case IB_WR_RDMA_READ:
		return AMD_EMRDMA_WR_RDMA_READ;
	case IB_WR_ATOMIC_CMP_AND_SWP:
		return AMD_EMRDMA_WR_ATOMIC_CMP_AND_SWP;
	case IB_WR_ATOMIC_FETCH_AND_ADD:
		return AMD_EMRDMA_WR_ATOMIC_FETCH_AND_ADD;
	case IB_WR_LSO:
		return AMD_EMRDMA_WR_LSO;
	case IB_WR_SEND_WITH_INV:
		return AMD_EMRDMA_WR_SEND_WITH_INV;
	case IB_WR_RDMA_READ_WITH_INV:
		return AMD_EMRDMA_WR_RDMA_READ_WITH_INV;
	case IB_WR_LOCAL_INV:
		return AMD_EMRDMA_WR_LOCAL_INV;
	case IB_WR_REG_MR:
		return AMD_EMRDMA_WR_FAST_REG_MR;
	case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
		return AMD_EMRDMA_WR_MASKED_ATOMIC_CMP_AND_SWP;
	case IB_WR_MASKED_ATOMIC_FETCH_AND_ADD:
		return AMD_EMRDMA_WR_MASKED_ATOMIC_FETCH_AND_ADD;
	case IB_WR_REG_MR_INTEGRITY:
		return AMD_EMRDMA_WR_REG_SIG_MR;
	default:
		return AMD_EMRDMA_WR_ERROR;
	}
}

static inline enum ib_wc_status amd_emrdma_wc_status_to_ib(
					enum amd_emrdma_wc_status status)
{
	return (enum ib_wc_status)status;
}

static inline int amd_emrdma_wc_opcode_to_ib(unsigned int opcode)
{
	switch (opcode) {
	case AMD_EMRDMA_WC_SEND:
		return IB_WC_SEND;
	case AMD_EMRDMA_WC_RDMA_WRITE:
		return IB_WC_RDMA_WRITE;
	case AMD_EMRDMA_WC_RDMA_READ:
		return IB_WC_RDMA_READ;
	case AMD_EMRDMA_WC_COMP_SWAP:
		return IB_WC_COMP_SWAP;
	case AMD_EMRDMA_WC_FETCH_ADD:
		return IB_WC_FETCH_ADD;
	case AMD_EMRDMA_WC_LOCAL_INV:
		return IB_WC_LOCAL_INV;
	case AMD_EMRDMA_WC_FAST_REG_MR:
		return IB_WC_REG_MR;
	case AMD_EMRDMA_WC_MASKED_COMP_SWAP:
		return IB_WC_MASKED_COMP_SWAP;
	case AMD_EMRDMA_WC_MASKED_FETCH_ADD:
		return IB_WC_MASKED_FETCH_ADD;
	case AMD_EMRDMA_WC_RECV:
		return IB_WC_RECV;
	case AMD_EMRDMA_WC_RECV_RDMA_WITH_IMM:
		return IB_WC_RECV_RDMA_WITH_IMM;
	default:
		return IB_WC_SEND;
	}
}

static inline int amd_emrdma_wc_flags_to_ib(int flags)
{
	return flags;
}

static inline int ib_send_flags_to_amd_emrdma(int flags)
{
	return flags & AMD_EMRDMA_MASK(AMD_EMRDMA_SEND_FLAGS_MAX);
}

static inline int amd_emrdma_network_type_to_ib(enum amd_emrdma_network_type type)
{
	switch (type) {
	case AMD_EMRDMA_NETWORK_ROCE_V1:
		return RDMA_NETWORK_ROCE_V1;
	case AMD_EMRDMA_NETWORK_IPV4:
		return RDMA_NETWORK_IPV4;
	case AMD_EMRDMA_NETWORK_IPV6:
		return RDMA_NETWORK_IPV6;
	default:
		return RDMA_NETWORK_IPV6;
	}
}

void amd_emrdma_qp_cap_to_ib(struct ib_qp_cap *dst,
			 const struct amd_emrdma_qp_cap *src);
void ib_qp_cap_to_amd_emrdma(struct amd_emrdma_qp_cap *dst,
			 const struct ib_qp_cap *src);
void amd_emrdma_gid_to_ib(union ib_gid *dst, const union amd_emrdma_gid *src);
void ib_gid_to_amd_emrdma(union amd_emrdma_gid *dst, const union ib_gid *src);
void amd_emrdma_global_route_to_ib(struct ib_global_route *dst,
			       const struct amd_emrdma_global_route *src);
void ib_global_route_to_amd_emrdma(struct amd_emrdma_global_route *dst,
			       const struct ib_global_route *src);
void amd_emrdma_ah_attr_to_rdma(struct rdma_ah_attr *dst,
			    const struct amd_emrdma_ah_attr *src);
void rdma_ah_attr_to_amd_emrdma(struct amd_emrdma_ah_attr *dst,
			    const struct rdma_ah_attr *src);
u8 ib_gid_type_to_amd_emrdma(enum ib_gid_type gid_type);

int amd_emrdma_uar_table_init(struct amd_emrdma_dev *dev);
void amd_emrdma_uar_table_cleanup(struct amd_emrdma_dev *dev);

int amd_emrdma_uar_alloc(struct amd_emrdma_dev *dev, struct amd_emrdma_uar_map *uar);
void amd_emrdma_uar_free(struct amd_emrdma_dev *dev, struct amd_emrdma_uar_map *uar);

void _amd_emrdma_flush_cqe(struct amd_emrdma_qp *qp, struct amd_emrdma_cq *cq);

int amd_emrdma_page_dir_init(struct amd_emrdma_dev *dev, struct amd_emrdma_page_dir *pdir,
			 u64 npages, bool alloc_pages);
void amd_emrdma_page_dir_cleanup(struct amd_emrdma_dev *dev,
			     struct amd_emrdma_page_dir *pdir);
int amd_emrdma_page_dir_insert_dma(struct amd_emrdma_page_dir *pdir, u64 idx,
			       dma_addr_t daddr);
int amd_emrdma_page_dir_insert_umem(struct amd_emrdma_page_dir *pdir,
				struct ib_umem *umem, u64 offset);
dma_addr_t amd_emrdma_page_dir_get_dma(struct amd_emrdma_page_dir *pdir, u64 idx);
int amd_emrdma_page_dir_insert_page_list(struct amd_emrdma_page_dir *pdir,
				     u64 *page_list, int num_pages);

int amd_emrdma_cmd_post(struct amd_emrdma_dev *dev, union amd_emrdma_cmd_req *req,
		    union amd_emrdma_cmd_resp *rsp, unsigned resp_code);

#endif /* __AMD_EMRDMA_H__ */
