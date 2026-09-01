/*
 * Unit tests for dhcp_server_process().
 *
 * Regression coverage for an out-of-bounds read on short requests: the
 * parser (and dhcp_find_option) treat the request as a full, fixed-size
 * dhcp_packet, so a request shorter than sizeof(struct dhcp_packet) was
 * read past its end. Each request is placed in an exact-size heap buffer,
 * so when built with AddressSanitizer any over-read fails the test.
 *
 * Table-driven: negative (NULL, zero-length, and lengths just below the
 * struct size are rejected), boundary (exactly the struct size is
 * accepted), and a positive well-formed DISCOVER that must still yield an
 * OFFER after the fix.
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dhcp_server.h"

#define NULL_REQUEST ((size_t) - 1)

static DhcpServer *make_server(void)
{
    return dhcp_server_create(htonl(0x0a000001), htonl(0xffffff00),
                              htonl(0x0a000001), htonl(0x08080808),
                              htonl(0x0a000002), htonl(0x0a0000fe), 3600);
}

/* Build a minimal valid DHCP DISCOVER into a full-size packet. */
static void build_discover(struct dhcp_packet *p)
{
    memset(p, 0, sizeof(*p));
    p->op = 1;    /* BOOTREQUEST */
    p->htype = 1; /* Ethernet */
    p->hlen = 6;
    p->xid = htonl(0x12345678);
    static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(p->chaddr, mac, sizeof(mac));

    uint8_t *o = p->options;
    o[0] = 0x63;
    o[1] = 0x82;
    o[2] = 0x53;
    o[3] = 0x63; /* magic cookie */
    o[4] = DHCP_OPT_MSG_TYPE;
    o[5] = 1;
    o[6] = DHCP_MSG_DISCOVER;
    o[7] = DHCP_OPT_END;
}

struct testcase {
    const char *name;
    size_t request_len;  /* NULL_REQUEST => NULL pointer */
    int valid_discover;  /* fill a real DISCOVER (implies full length) */
    int expect_response; /* 1 => return > 0, 0 => return == 0 */
};

static int run_case(DhcpServer *server, const struct testcase *tc)
{
    struct dhcp_packet response;
    size_t ret;

    if (tc->request_len == NULL_REQUEST) {
        ret = dhcp_server_process(server, NULL, 0, &response, sizeof(response));
        if (ret != 0) {
            printf("FAIL %-22s: NULL request should return 0, got %zu\n",
                   tc->name, ret);
            return 1;
        }
        return 0;
    }

    /* Exact-size allocation so ASan brackets the request precisely. */
    size_t len = tc->request_len;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) {
        printf("FAIL %-22s: allocation failed\n", tc->name);
        return 1;
    }

    if (tc->valid_discover) {
        /* valid_discover cases use a full-size packet. */
        build_discover((struct dhcp_packet *)buf);
    } else {
        memset(buf, 0xAB, len);
    }

    ret = dhcp_server_process(server, (const struct dhcp_packet *)buf, len,
                              &response, sizeof(response));
    free(buf);

    int fail = 0;
    if (tc->expect_response && ret == 0) {
        printf("FAIL %-22s: expected a response, got 0\n", tc->name);
        fail = 1;
    }
    if (!tc->expect_response && ret != 0) {
        printf("FAIL %-22s: expected 0, got %zu\n", tc->name, ret);
        fail = 1;
    }
    return fail;
}

int main(void)
{
    DhcpServer *server = make_server();
    if (!server) {
        printf("FAIL: dhcp_server_create returned NULL\n");
        return 1;
    }

    const size_t full = sizeof(struct dhcp_packet);
    const struct testcase cases[] = {
        /* negative — rejected without reading past the buffer */
        {"null-request", NULL_REQUEST, 0, 0},
        {"zero-length", 0, 0, 0}, /* exact fuzzer input */
        {"len-1", 1, 0, 0},
        {"len-8", 8, 0, 0},
        {"len-full-minus-1", full - 1, 0, 0}, /* boundary just below */
        /* boundary — exactly the struct size is accepted (no crash) */
        {"len-full-garbage", full, 0, 0},
        /* positive — a well-formed DISCOVER still yields an OFFER */
        {"valid-discover", full, 1, 1},
    };

    int failures = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        failures += run_case(server, &cases[i]);
    }

    dhcp_server_destroy(server);

    if (failures) {
        printf("dhcp_server_process: %d/%zu case(s) FAILED\n", failures, n);
        return 1;
    }
    printf("dhcp_server_process: all %zu cases passed\n", n);
    return 0;
}
