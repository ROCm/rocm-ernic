/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef ROCM_ERNIC_DC_H
#define ROCM_ERNIC_DC_H

#include <infiniband/verbs.h>
#include <stdint.h>

enum rocm_ernic_dc_role {
    ROCM_ERNIC_DC_ROLE_NONE = 0,
    ROCM_ERNIC_DC_ROLE_DCT = 1,
    ROCM_ERNIC_DC_ROLE_DCI = 2,
};

struct rocm_ernic_dc_dct_init {
    uint64_t access_key;
    uint8_t port_num;
    uint8_t reserved[7];
};

struct rocm_ernic_dc_dci_init {
    uint8_t reserved[8];
};

struct ibv_qp *rocm_ernic_dc_create_dct(struct ibv_pd *pd,
                                         struct ibv_qp_init_attr *attr,
                                         const struct rocm_ernic_dc_dct_init *dct);

struct ibv_qp *rocm_ernic_dc_create_dci(struct ibv_pd *pd,
                                       struct ibv_qp_init_attr *attr,
                                       const struct rocm_ernic_dc_dci_init *dci);

int rocm_ernic_dc_post_send(struct ibv_qp *qp, uint64_t wr_id,
                            const struct ibv_sge *sg_list, int num_sge,
                            uint32_t remote_dctn, uint32_t dc_access_key,
                            uint32_t ah_id);

uint32_t rocm_ernic_dc_get_dctn(const struct ibv_qp *qp);

#endif /* ROCM_ERNIC_DC_H */
