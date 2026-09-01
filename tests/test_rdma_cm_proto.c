/*
 * Unit tests for rdma_cm_process_message().
 *
 * Regression coverage for an out-of-bounds read in the debug hex-dump: a
 * payload of 4..31 bytes was previously read up to 32 bytes. Each payload
 * is placed in an exact-size heap buffer, so when the test binary is built
 * with AddressSanitizer any over-read fails the test.
 *
 * Table-driven: positive (small-message echo), negative (NULL / zero-length
 * rejected), and boundary/corner cases around the 4- and 32-byte edges.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdma_cm_proto.h"

#define RESP_CAP     2048
#define NULL_PAYLOAD ((size_t) - 1)

struct testcase {
    const char *name;
    size_t payload_len; /* NULL_PAYLOAD => pass a NULL pointer */
    int expect_echo;    /* small messages (<4B) are echoed back verbatim */
};

static int run_case(const struct testcase *tc)
{
    uint8_t response[RESP_CAP];

    if (tc->payload_len == NULL_PAYLOAD) {
        size_t ret = rdma_cm_process_message(NULL, 8, response, RESP_CAP);
        if (ret != 0) {
            printf("FAIL %-20s: NULL payload should return 0, got %zu\n",
                   tc->name, ret);
            return 1;
        }
        return 0;
    }

    /* Exact-size allocation so ASan brackets the payload precisely. */
    uint8_t *payload = malloc(tc->payload_len ? tc->payload_len : 1);
    if (!payload) {
        printf("FAIL %-20s: allocation failed\n", tc->name);
        return 1;
    }
    for (size_t i = 0; i < tc->payload_len; i++) {
        payload[i] = (uint8_t)(0xA0 + (i & 0x0F));
    }

    size_t ret =
        rdma_cm_process_message(payload, tc->payload_len, response, RESP_CAP);
    free(payload);

    int fail = 0;
    if (ret > RESP_CAP) {
        printf("FAIL %-20s: response %zu exceeds cap %d\n", tc->name, ret,
               RESP_CAP);
        fail = 1;
    }
    if (tc->payload_len == 0 && ret != 0) {
        printf("FAIL %-20s: zero-length should return 0, got %zu\n", tc->name,
               ret);
        fail = 1;
    }
    if (tc->expect_echo && ret != tc->payload_len) {
        printf("FAIL %-20s: expected echo of %zu bytes, got %zu\n", tc->name,
               tc->payload_len, ret);
        fail = 1;
    }
    return fail;
}

int main(void)
{
    static const struct testcase cases[] = {
        /* negative */
        {"null-payload", NULL_PAYLOAD, 0},
        {"zero-length", 0, 0},
        /* positive: small messages are echoed */
        {"len-1-echo", 1, 1},
        {"len-2-echo", 2, 1},
        {"len-3-echo", 3, 1},
        /* boundary/corner around the 4- and 32-byte dump edges */
        {"len-4-corner", 4, 0}, /* the exact fuzzer crash length */
        {"len-5", 5, 0},
        {"len-16", 16, 0},
        {"len-31-boundary", 31, 0}, /* one below the old 32-byte read */
        {"len-32-boundary", 32, 0},
        {"len-33", 33, 0},
        {"len-64", 64, 0},
        {"len-256", 256, 0},
    };

    int failures = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        failures += run_case(&cases[i]);
    }

    if (failures) {
        printf("rdma_cm_proto: %d/%zu case(s) FAILED\n", failures, n);
        return 1;
    }
    printf("rdma_cm_proto: all %zu cases passed\n", n);
    return 0;
}
