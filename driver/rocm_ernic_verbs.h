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

#ifndef __AMD_EMRDMA_VERBS_H__
#define __AMD_EMRDMA_VERBS_H__

#include <linux/version.h>
#include <linux/types.h>

union amd_emrdma_gid {
    u8 raw[16];
    struct {
        __be64 subnet_prefix;
        __be64 interface_id;
    } global;
};

enum amd_emrdma_link_layer {
    AMD_EMRDMA_LINK_LAYER_UNSPECIFIED,
    AMD_EMRDMA_LINK_LAYER_INFINIBAND,
    AMD_EMRDMA_LINK_LAYER_ETHERNET,
};

enum amd_emrdma_mtu {
    AMD_EMRDMA_MTU_256 = 1,
    AMD_EMRDMA_MTU_512 = 2,
    AMD_EMRDMA_MTU_1024 = 3,
    AMD_EMRDMA_MTU_2048 = 4,
    AMD_EMRDMA_MTU_4096 = 5,
};

enum amd_emrdma_port_state {
    AMD_EMRDMA_PORT_NOP = 0,
    AMD_EMRDMA_PORT_DOWN = 1,
    AMD_EMRDMA_PORT_INIT = 2,
    AMD_EMRDMA_PORT_ARMED = 3,
    AMD_EMRDMA_PORT_ACTIVE = 4,
    AMD_EMRDMA_PORT_ACTIVE_DEFER = 5,
};

enum amd_emrdma_port_cap_flags {
    AMD_EMRDMA_PORT_SM = 1 << 1,
    AMD_EMRDMA_PORT_NOTICE_SUP = 1 << 2,
    AMD_EMRDMA_PORT_TRAP_SUP = 1 << 3,
    AMD_EMRDMA_PORT_OPT_IPD_SUP = 1 << 4,
    AMD_EMRDMA_PORT_AUTO_MIGR_SUP = 1 << 5,
    AMD_EMRDMA_PORT_SL_MAP_SUP = 1 << 6,
    AMD_EMRDMA_PORT_MKEY_NVRAM = 1 << 7,
    AMD_EMRDMA_PORT_PKEY_NVRAM = 1 << 8,
    AMD_EMRDMA_PORT_LED_INFO_SUP = 1 << 9,
    AMD_EMRDMA_PORT_SM_DISABLED = 1 << 10,
    AMD_EMRDMA_PORT_SYS_IMAGE_GUID_SUP = 1 << 11,
    AMD_EMRDMA_PORT_PKEY_SW_EXT_PORT_TRAP_SUP = 1 << 12,
    AMD_EMRDMA_PORT_EXTENDED_SPEEDS_SUP = 1 << 14,
    AMD_EMRDMA_PORT_CM_SUP = 1 << 16,
    AMD_EMRDMA_PORT_SNMP_TUNNEL_SUP = 1 << 17,
    AMD_EMRDMA_PORT_REINIT_SUP = 1 << 18,
    AMD_EMRDMA_PORT_DEVICE_MGMT_SUP = 1 << 19,
    AMD_EMRDMA_PORT_VENDOR_CLASS_SUP = 1 << 20,
    AMD_EMRDMA_PORT_DR_NOTICE_SUP = 1 << 21,
    AMD_EMRDMA_PORT_CAP_MASK_NOTICE_SUP = 1 << 22,
    AMD_EMRDMA_PORT_BOOT_MGMT_SUP = 1 << 23,
    AMD_EMRDMA_PORT_LINK_LATENCY_SUP = 1 << 24,
    AMD_EMRDMA_PORT_CLIENT_REG_SUP = 1 << 25,
    AMD_EMRDMA_PORT_IP_BASED_GIDS = 1 << 26,
    AMD_EMRDMA_PORT_CAP_FLAGS_MAX = AMD_EMRDMA_PORT_IP_BASED_GIDS,
};

enum amd_emrdma_port_width {
    AMD_EMRDMA_WIDTH_1X = 1,
    AMD_EMRDMA_WIDTH_4X = 2,
    AMD_EMRDMA_WIDTH_8X = 4,
    AMD_EMRDMA_WIDTH_12X = 8,
};

enum amd_emrdma_port_speed {
    AMD_EMRDMA_SPEED_SDR = 1,
    AMD_EMRDMA_SPEED_DDR = 2,
    AMD_EMRDMA_SPEED_QDR = 4,
    AMD_EMRDMA_SPEED_FDR10 = 8,
    AMD_EMRDMA_SPEED_FDR = 16,
    AMD_EMRDMA_SPEED_EDR = 32,
};

struct amd_emrdma_port_attr {
    enum amd_emrdma_port_state state;
    enum amd_emrdma_mtu max_mtu;
    enum amd_emrdma_mtu active_mtu;
    u32 gid_tbl_len;
    u32 port_cap_flags;
    u32 max_msg_sz;
    u32 bad_pkey_cntr;
    u32 qkey_viol_cntr;
    u16 pkey_tbl_len;
    u16 lid;
    u16 sm_lid;
    u8 lmc;
    u8 max_vl_num;
    u8 sm_sl;
    u8 subnet_timeout;
    u8 init_type_reply;
    u8 active_width;
    u8 active_speed;
    u8 phys_state;
    u8 reserved[2];
};

struct amd_emrdma_global_route {
    union amd_emrdma_gid dgid;
    u32 flow_label;
    u8 sgid_index;
    u8 hop_limit;
    u8 traffic_class;
    u8 reserved;
};

struct amd_emrdma_grh {
    __be32 version_tclass_flow;
    __be16 paylen;
    u8 next_hdr;
    u8 hop_limit;
    union amd_emrdma_gid sgid;
    union amd_emrdma_gid dgid;
};

enum amd_emrdma_ah_flags {
    AMD_EMRDMA_AH_GRH = 1,
};

enum amd_emrdma_rate {
    AMD_EMRDMA_RATE_PORT_CURRENT = 0,
    AMD_EMRDMA_RATE_2_5_GBPS = 2,
    AMD_EMRDMA_RATE_5_GBPS = 5,
    AMD_EMRDMA_RATE_10_GBPS = 3,
    AMD_EMRDMA_RATE_20_GBPS = 6,
    AMD_EMRDMA_RATE_30_GBPS = 4,
    AMD_EMRDMA_RATE_40_GBPS = 7,
    AMD_EMRDMA_RATE_60_GBPS = 8,
    AMD_EMRDMA_RATE_80_GBPS = 9,
    AMD_EMRDMA_RATE_120_GBPS = 10,
    AMD_EMRDMA_RATE_14_GBPS = 11,
    AMD_EMRDMA_RATE_56_GBPS = 12,
    AMD_EMRDMA_RATE_112_GBPS = 13,
    AMD_EMRDMA_RATE_168_GBPS = 14,
    AMD_EMRDMA_RATE_25_GBPS = 15,
    AMD_EMRDMA_RATE_100_GBPS = 16,
    AMD_EMRDMA_RATE_200_GBPS = 17,
    AMD_EMRDMA_RATE_300_GBPS = 18,
};

struct amd_emrdma_ah_attr {
    struct amd_emrdma_global_route grh;
    u16 dlid;
    u16 vlan_id;
    u8 sl;
    u8 src_path_bits;
    u8 static_rate;
    u8 ah_flags;
    u8 port_num;
    u8 dmac[6];
    u8 reserved;
};

enum amd_emrdma_cq_notify_flags {
    AMD_EMRDMA_CQ_SOLICITED = 1 << 0,
    AMD_EMRDMA_CQ_NEXT_COMP = 1 << 1,
    AMD_EMRDMA_CQ_SOLICITED_MASK =
        AMD_EMRDMA_CQ_SOLICITED | AMD_EMRDMA_CQ_NEXT_COMP,
    AMD_EMRDMA_CQ_REPORT_MISSED_EVENTS = 1 << 2,
};

struct amd_emrdma_qp_cap {
    u32 max_send_wr;
    u32 max_recv_wr;
    u32 max_send_sge;
    u32 max_recv_sge;
    u32 max_inline_data;
    u32 reserved;
};

enum amd_emrdma_sig_type {
    AMD_EMRDMA_SIGNAL_ALL_WR,
    AMD_EMRDMA_SIGNAL_REQ_WR,
};

enum amd_emrdma_qp_type {
    AMD_EMRDMA_QPT_SMI,
    AMD_EMRDMA_QPT_GSI,
    AMD_EMRDMA_QPT_RC,
    AMD_EMRDMA_QPT_UC,
    AMD_EMRDMA_QPT_UD,
    AMD_EMRDMA_QPT_RAW_IPV6,
    AMD_EMRDMA_QPT_RAW_ETHERTYPE,
    AMD_EMRDMA_QPT_RAW_PACKET = 8,
    AMD_EMRDMA_QPT_XRC_INI = 9,
    AMD_EMRDMA_QPT_XRC_TGT,
    AMD_EMRDMA_QPT_MAX,
};

enum amd_emrdma_qp_create_flags {
    AMD_EMRDMA_QP_CREATE_IPOAMD_EMRDMA_UD_LSO = 1 << 0,
    AMD_EMRDMA_QP_CREATE_BLOCK_MULTICAST_LOOPBACK = 1 << 1,
};

enum amd_emrdma_qp_attr_mask {
    AMD_EMRDMA_QP_STATE = 1 << 0,
    AMD_EMRDMA_QP_CUR_STATE = 1 << 1,
    AMD_EMRDMA_QP_EN_SQD_ASYNC_NOTIFY = 1 << 2,
    AMD_EMRDMA_QP_ACCESS_FLAGS = 1 << 3,
    AMD_EMRDMA_QP_PKEY_INDEX = 1 << 4,
    AMD_EMRDMA_QP_PORT = 1 << 5,
    AMD_EMRDMA_QP_QKEY = 1 << 6,
    AMD_EMRDMA_QP_AV = 1 << 7,
    AMD_EMRDMA_QP_PATH_MTU = 1 << 8,
    AMD_EMRDMA_QP_TIMEOUT = 1 << 9,
    AMD_EMRDMA_QP_RETRY_CNT = 1 << 10,
    AMD_EMRDMA_QP_RNR_RETRY = 1 << 11,
    AMD_EMRDMA_QP_RQ_PSN = 1 << 12,
    AMD_EMRDMA_QP_MAX_QP_RD_ATOMIC = 1 << 13,
    AMD_EMRDMA_QP_ALT_PATH = 1 << 14,
    AMD_EMRDMA_QP_MIN_RNR_TIMER = 1 << 15,
    AMD_EMRDMA_QP_SQ_PSN = 1 << 16,
    AMD_EMRDMA_QP_MAX_DEST_RD_ATOMIC = 1 << 17,
    AMD_EMRDMA_QP_PATH_MIG_STATE = 1 << 18,
    AMD_EMRDMA_QP_CAP = 1 << 19,
    AMD_EMRDMA_QP_DEST_QPN = 1 << 20,
    AMD_EMRDMA_QP_ATTR_MASK_MAX = AMD_EMRDMA_QP_DEST_QPN,
};

enum amd_emrdma_qp_state {
    AMD_EMRDMA_QPS_RESET,
    AMD_EMRDMA_QPS_INIT,
    AMD_EMRDMA_QPS_RTR,
    AMD_EMRDMA_QPS_RTS,
    AMD_EMRDMA_QPS_SQD,
    AMD_EMRDMA_QPS_SQE,
    AMD_EMRDMA_QPS_ERR,
};

enum amd_emrdma_mig_state {
    AMD_EMRDMA_MIG_MIGRATED,
    AMD_EMRDMA_MIG_REARM,
    AMD_EMRDMA_MIG_ARMED,
};

enum amd_emrdma_mw_type {
    AMD_EMRDMA_MW_TYPE_1 = 1,
    AMD_EMRDMA_MW_TYPE_2 = 2,
};

struct amd_emrdma_srq_attr {
    u32 max_wr;
    u32 max_sge;
    u32 srq_limit;
    u32 reserved;
};

struct amd_emrdma_qp_attr {
    enum amd_emrdma_qp_state qp_state;
    enum amd_emrdma_qp_state cur_qp_state;
    enum amd_emrdma_mtu path_mtu;
    enum amd_emrdma_mig_state path_mig_state;
    u32 qkey;
    u32 rq_psn;
    u32 sq_psn;
    u32 dest_qp_num;
    u32 qp_access_flags;
    u16 pkey_index;
    u16 alt_pkey_index;
    u8 en_sqd_async_notify;
    u8 sq_draining;
    u8 max_rd_atomic;
    u8 max_dest_rd_atomic;
    u8 min_rnr_timer;
    u8 port_num;
    u8 timeout;
    u8 retry_cnt;
    u8 rnr_retry;
    u8 alt_port_num;
    u8 alt_timeout;
    u8 reserved[5];
    struct amd_emrdma_qp_cap cap;
    struct amd_emrdma_ah_attr ah_attr;
    struct amd_emrdma_ah_attr alt_ah_attr;
};

enum amd_emrdma_send_flags {
    AMD_EMRDMA_SEND_FENCE = 1 << 0,
    AMD_EMRDMA_SEND_SIGNALED = 1 << 1,
    AMD_EMRDMA_SEND_SOLICITED = 1 << 2,
    AMD_EMRDMA_SEND_INLINE = 1 << 3,
    AMD_EMRDMA_SEND_IP_CSUM = 1 << 4,
    AMD_EMRDMA_SEND_FLAGS_MAX = AMD_EMRDMA_SEND_IP_CSUM,
};

enum amd_emrdma_access_flags {
    AMD_EMRDMA_ACCESS_LOCAL_WRITE = 1 << 0,
    AMD_EMRDMA_ACCESS_REMOTE_WRITE = 1 << 1,
    AMD_EMRDMA_ACCESS_REMOTE_READ = 1 << 2,
    AMD_EMRDMA_ACCESS_REMOTE_ATOMIC = 1 << 3,
    AMD_EMRDMA_ACCESS_MW_BIND = 1 << 4,
    AMD_EMRDMA_ZERO_BASED = 1 << 5,
    AMD_EMRDMA_ACCESS_ON_DEMAND = 1 << 6,
    AMD_EMRDMA_ACCESS_FLAGS_MAX = AMD_EMRDMA_ACCESS_ON_DEMAND,
};

int amd_emrdma_query_device(struct ib_device *ibdev,
                            struct ib_device_attr *props,
                            struct ib_udata *udata);
int amd_emrdma_query_port(struct ib_device *ibdev, u32 port,
                          struct ib_port_attr *props);
int amd_emrdma_query_gid(struct ib_device *ibdev, u32 port, int index,
                         union ib_gid *gid);
int amd_emrdma_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
                          u16 *pkey);
enum rdma_link_layer amd_emrdma_port_link_layer(struct ib_device *ibdev,
                                                u32 port);
int amd_emrdma_modify_port(struct ib_device *ibdev, u32 port, int mask,
                           struct ib_port_modify *props);
int amd_emrdma_mmap(struct ib_ucontext *context, struct vm_area_struct *vma);
int amd_emrdma_alloc_ucontext(struct ib_ucontext *uctx, struct ib_udata *udata);
void amd_emrdma_dealloc_ucontext(struct ib_ucontext *context);
int amd_emrdma_alloc_pd(struct ib_pd *pd, struct ib_udata *udata);
int amd_emrdma_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata);
struct ib_mr *amd_emrdma_get_dma_mr(struct ib_pd *pd, int acc);
struct ib_mr *amd_emrdma_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
                                     u64 virt_addr, int access_flags,
                                     struct ib_udata *udata);
int amd_emrdma_dereg_mr(struct ib_mr *mr, struct ib_udata *udata);
struct ib_mr *amd_emrdma_alloc_mr(struct ib_pd *pd, enum ib_mr_type mr_type,
                                  u32 max_num_sg);
int amd_emrdma_map_mr_sg(struct ib_mr *ibmr, struct scatterlist *sg,
                         int sg_nents, unsigned int *sg_offset);
int amd_emrdma_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
                         struct uverbs_attr_bundle *attrs);
#else
                         struct ib_udata *udata);
#endif
int amd_emrdma_destroy_cq(struct ib_cq *cq, struct ib_udata *udata);
int amd_emrdma_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc);
int amd_emrdma_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags flags);
int amd_emrdma_create_ah(struct ib_ah *ah, struct rdma_ah_init_attr *init_attr,
                         struct ib_udata *udata);
int amd_emrdma_destroy_ah(struct ib_ah *ah, u32 flags);

int amd_emrdma_create_srq(struct ib_srq *srq,
                          struct ib_srq_init_attr *init_attr,
                          struct ib_udata *udata);
int amd_emrdma_modify_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr,
                          enum ib_srq_attr_mask attr_mask,
                          struct ib_udata *udata);
int amd_emrdma_query_srq(struct ib_srq *srq, struct ib_srq_attr *srq_attr);
int amd_emrdma_destroy_srq(struct ib_srq *srq, struct ib_udata *udata);

int amd_emrdma_create_qp(struct ib_qp *qp, struct ib_qp_init_attr *init_attr,
                         struct ib_udata *udata);
int amd_emrdma_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
                         int attr_mask, struct ib_udata *udata);
int amd_emrdma_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
                        int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr);
int amd_emrdma_destroy_qp(struct ib_qp *qp, struct ib_udata *udata);
int amd_emrdma_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
                         const struct ib_send_wr **bad_wr);
int amd_emrdma_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
                         const struct ib_recv_wr **bad_wr);

#endif /* __AMD_EMRDMA_VERBS_H__ */
