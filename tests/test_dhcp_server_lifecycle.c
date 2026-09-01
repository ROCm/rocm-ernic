/*
 * Lifecycle / leak tests for the DHCP server.
 *
 * The DHCP server is the object that leaked at shutdown (it was created in
 * pvrdma_device_realize but never destroyed in pvrdma_device_destroy).
 * This test exercises create -> allocate N leases -> destroy and is built
 * with AddressSanitizer + LeakSanitizer, so any allocation the destroy path
 * fails to release (the server struct, the allocation/lease hash tables and
 * their heap keys/values) fails the test.
 *
 * Table-driven over the number of distinct clients that obtain a lease
 * (zero, one, several, and a full pool), plus a destroy(NULL) corner case.
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

/* Small pool (10.0.0.2 .. 10.0.0.6) so "full pool" is cheap to fill. */
static DhcpServer *make_server(void)
{
    return dhcp_server_create(htonl(0x0a000001), htonl(0xffffff00),
                              htonl(0x0a000001), htonl(0x08080808),
                              htonl(0x0a000002), htonl(0x0a000006), 3600);
}

static void build_discover(struct dhcp_packet *p, uint8_t client)
{
    memset(p, 0, sizeof(*p));
    p->op = 1;
    p->htype = 1;
    p->hlen = 6;
    p->xid = htonl(0x1000 + client);
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, client};
    memcpy(p->chaddr, mac, sizeof(mac));

    uint8_t *o = p->options;
    o[0] = 0x63;
    o[1] = 0x82;
    o[2] = 0x53;
    o[3] = 0x63;
    o[4] = DHCP_OPT_MSG_TYPE;
    o[5] = 1;
    o[6] = DHCP_MSG_DISCOVER;
    o[7] = DHCP_OPT_END;
}

/* Create a server, hand out `clients` leases, then destroy it. */
static int lease_and_destroy(const char *name, int clients)
{
    DhcpServer *server = make_server();
    if (!server) {
        printf("FAIL %-18s: create returned NULL\n", name);
        return 1;
    }

    struct dhcp_packet request;
    struct dhcp_packet response;
    for (int i = 0; i < clients; i++) {
        build_discover(&request, (uint8_t)(i + 1));
        (void)dhcp_server_process(server, &request, sizeof(request), &response,
                                  sizeof(response));
    }

    dhcp_server_destroy(server);
    return 0;
}

int main(void)
{
    struct testcase {
        const char *name;
        int clients;
    } cases[] = {
        {"no-leases", 0}, /* create + destroy only */
        {"one-lease", 1}, {"several-leases", 3},
        {"full-pool", 5}, /* every pool address handed out */
        {"over-pool", 7}, /* more clients than addresses */
    };

    int failures = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        failures += lease_and_destroy(cases[i].name, cases[i].clients);
    }

    /* Corner case: destroying NULL must be a no-op. */
    dhcp_server_destroy(NULL);

    if (failures) {
        printf("dhcp_server_lifecycle: %d/%zu case(s) FAILED\n", failures, n);
        return 1;
    }
    printf("dhcp_server_lifecycle: all %zu cases passed\n", n);
    return 0;
}
