/*
 * Byte-order accessors for wire data held in byte buffers.
 *
 * Guest descriptors, admin commands, NVMe capsules, CM MADs and tokens all
 * arrive as (or are built in) plain byte buffers, with each field at a fixed
 * offset and in a fixed byte order. These read and write such a field at any
 * alignment, whatever the host's byte order, so a field never has to be
 * memcpy()'d into a local and converted in place. Compilers turn each into a
 * single load or store, byte-swapped where needed.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_BYTEORDER_H
#define ERNIC_BYTEORDER_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Little-endian
 * -------------------------------------------------------------------------
 */

static inline uint16_t ernic_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ernic_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t ernic_get_le64(const uint8_t *p)
{
    return (uint64_t)ernic_get_le32(p) |
           ((uint64_t)ernic_get_le32(p + 4) << 32);
}

static inline void ernic_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void ernic_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void ernic_put_le64(uint8_t *p, uint64_t v)
{
    ernic_put_le32(p, (uint32_t)v);
    ernic_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* -------------------------------------------------------------------------
 * Big-endian
 * -------------------------------------------------------------------------
 */

static inline uint16_t ernic_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t ernic_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t ernic_get_be64(const uint8_t *p)
{
    return ((uint64_t)ernic_get_be32(p) << 32) |
           (uint64_t)ernic_get_be32(p + 4);
}

static inline void ernic_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void ernic_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void ernic_put_be64(uint8_t *p, uint64_t v)
{
    ernic_put_be32(p, (uint32_t)(v >> 32));
    ernic_put_be32(p + 4, (uint32_t)v);
}

#endif /* ERNIC_BYTEORDER_H */
