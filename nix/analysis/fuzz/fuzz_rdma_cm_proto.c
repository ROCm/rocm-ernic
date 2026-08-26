/*
 * libFuzzer harness for rdma_cm_process_message().
 *
 * rdma_cm_process_message() is the loopback rdma_cm responder: it parses a
 * raw TCP payload (attacker-influenced wire data) and writes a response.
 * It is a pure function with no device state, which makes it the cleanest,
 * highest-value fuzz target in the tree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rdma_cm_proto.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Copy the input into an exact-size heap buffer so AddressSanitizer
     * flags any over-read of the payload by the parser (a real bug),
     * rather than silently reading adjacent stack/globals. */
    uint8_t *payload = malloc(size ? size : 1);
    if (!payload) {
        return 0;
    }
    memcpy(payload, data, size);

    /* Bound the response buffer so the parser's own max_response_len
     * handling is exercised; ASan guards writes past it. */
    enum { RESP_CAP = 2048 };
    uint8_t *response = malloc(RESP_CAP);
    if (!response) {
        free(payload);
        return 0;
    }

    (void)rdma_cm_process_message(payload, size, response, RESP_CAP);

    free(response);
    free(payload);
    return 0;
}
