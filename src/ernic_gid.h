/*
 * ernic_gid.h - IPv4 addresses in GIDs, and GIDs as text
 *
 * A RoCEv2 port with an IPv4 address advertises it as a GID in the
 * IPv4-mapped IPv6 form, ::ffff:a.b.c.d:
 *
 *   [0:9]   zero
 *   [10:11] 0xff 0xff
 *   [12:15] the IPv4 address, in network order
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_GID_H
#define ERNIC_GID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rocm_ernic_compat.h"

#define ERNIC_GID_V4_OFF 12U /* the IPv4 address, after the prefix */
#define ERNIC_GID_V4_LEN 4U

/* The bytes before the address in an IPv4-mapped GID. */
static inline const uint8_t *ernic_gid_v4_prefix(void)
{
    static const uint8_t prefix[ERNIC_GID_V4_OFF] = {0, 0, 0, 0, 0,    0,
                                                     0, 0, 0, 0, 0xff, 0xff};
    return prefix;
}

/**
 * ernic_gid_from_ipv4() - make the GID for an IPv4 address
 *
 * @param gid where to write the GID
 * @param addr the address, in network order
 */
static inline void ernic_gid_from_ipv4(uint8_t gid[ERNIC_GID_LEN],
                                       const uint8_t addr[ERNIC_GID_V4_LEN])
{
    memcpy(gid, ernic_gid_v4_prefix(), ERNIC_GID_V4_OFF);
    memcpy(gid + ERNIC_GID_V4_OFF, addr, ERNIC_GID_V4_LEN);
}

/**
 * ernic_gid_is_v4mapped() - whether a GID holds an IPv4 address
 *
 * @param gid the GID
 */
static inline bool ernic_gid_is_v4mapped(const uint8_t gid[ERNIC_GID_LEN])
{
    return memcmp(gid, ernic_gid_v4_prefix(), ERNIC_GID_V4_OFF) == 0;
}

/* Room for a GID as text, eight colon-separated groups of four hex
 * digits, and its NUL */
#define ERNIC_GID_STR_LEN 40

/**
 * ernic_gid_str() - write a GID as text, xxxx:xxxx:...:xxxx, for a log
 *
 * @param gid the GID
 * @param out where to write it, ERNIC_GID_STR_LEN bytes
 */
static inline void ernic_gid_str(const uint8_t gid[ERNIC_GID_LEN], char *out)
{
    static const char hex[] = "0123456789abcdef";
    char *p = out;

    for (size_t i = 0; i < ERNIC_GID_LEN; i++) {
        if (i > 0 && i % 2 == 0) {
            *p++ = ':';
        }
        *p++ = hex[gid[i] >> 4];
        *p++ = hex[gid[i] & 0xfU];
    }
    *p = '\0';
}

#endif /* ERNIC_GID_H */
