/*
 * libFuzzer harness for dhcp_server_process().
 *
 * dhcp_server_process() parses a DHCP request packet (untrusted wire data)
 * against a server context and writes a response. The context is created
 * once in LLVMFuzzerInitialize(); each input drives the packet parser.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dhcp_server.h"

static DhcpServer *g_server;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    /* 10.0.0.1/24, gw 10.0.0.1, dns 8.8.8.8, pool 10.0.0.2–10.0.0.254. */
    g_server = dhcp_server_create(
        htonl(0x0a000001), htonl(0xffffff00),
        htonl(0x0a000001), htonl(0x08080808),
        htonl(0x0a000002), htonl(0x0a0000fe),
        3600);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_server) {
        return 0;
    }

    /* Exact-size heap copy of the request so ASan catches any field access
     * past the supplied request_len (i.e. the parser trusting the buffer is
     * at least sizeof(struct dhcp_packet) without checking). */
    uint8_t *request = malloc(size ? size : 1);
    if (!request) {
        return 0;
    }
    memcpy(request, data, size);

    struct dhcp_packet *response = malloc(sizeof(struct dhcp_packet));
    if (!response) {
        free(request);
        return 0;
    }

    (void)dhcp_server_process(g_server,
                              (const struct dhcp_packet *)request, size,
                              response, sizeof(struct dhcp_packet));

    free(response);
    free(request);
    return 0;
}
