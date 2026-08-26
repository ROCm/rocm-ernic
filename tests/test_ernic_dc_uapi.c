/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * UAPI / ABI sanity tests for rocm_ernic Dynamic Connection fields.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <cmocka.h>

#ifndef offsetofend
#define offsetofend(TYPE, MEMBER) \
    (offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))
#endif

#include <linux/types.h>

#include "rocm_ernic-abi.h"

static void test_wr_send_dc_opcode(void **state)
{
    (void)state;
    assert_int_equal((int)ROCM_ERNIC_WR_SEND_DC, 17);
}

static void test_create_qp_ex_dc_offset(void **state)
{
    (void)state;

    assert_true(offsetof(struct rocm_ernic_create_qp, ex_mask) <
                offsetof(struct rocm_ernic_create_qp, dct_access_key));
    assert_true(sizeof(struct rocm_ernic_create_qp) >=
                offsetofend(struct rocm_ernic_create_qp, dct_access_key));
}

static void test_create_qp_resp_dc(void **state)
{
    (void)state;

    assert_true(offsetof(struct rocm_ernic_create_qp_resp, resp_ex_mask) <
                offsetof(struct rocm_ernic_create_qp_resp, dctn));
}

static void test_create_srq_resp_uar(void **state)
{
    (void)state;

    assert_true(sizeof(struct rocm_ernic_create_srq_resp) >=
                offsetof(struct rocm_ernic_create_srq_resp, uar_mmap_offset) +
                    sizeof(uint64_t));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_wr_send_dc_opcode),
        cmocka_unit_test(test_create_qp_ex_dc_offset),
        cmocka_unit_test(test_create_qp_resp_dc),
        cmocka_unit_test(test_create_srq_resp_uar),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
