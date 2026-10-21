/*
 * p4_program.h — the loaded form of a bmv2 program
 *
 * One heap allocation, no internal pointers, every cross-reference a small
 * integer index resolved at load time.  That shape is deliberate: the
 * program is built from an untrusted document, so a flat blob with no
 * pointers in it cannot represent a cycle, has exactly one free, and lets
 * the executor index without re-validating a dereference on every packet.
 *
 * Every array is fixed and capped.  Raising a cap later is a recompile;
 * lowering one breaks a program that used to load, so they are set
 * deliberately rather than discovered.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_PROGRAM_H
#define P4_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "p4_json.h" /* P4_MAX_NAME */

#define P4_MAX_HEADER_TYPES 64
#define P4_MAX_FIELDS       64 /* per header type */
#define P4_MAX_HEADERS      64
#define P4_MAX_FIELD_BITS   512

/*
 * Index of a missing reference.  Every index field is this width, so a
 * comparison against it is never silently false through promotion -- an
 * index stored in a narrower type would compare against a truncated
 * sentinel and never match.
 */
typedef uint16_t p4_idx_t;
#define P4_IDX_NONE ((p4_idx_t)0xFFFFu)

/* No id may collide with the sentinel, and ids are never used as indices. */
#define P4_MAX_ID 65534u

struct p4_field {
    char name[P4_MAX_NAME + 1];
    uint16_t bits;
    uint16_t offset_bits; /* within its header */
};

struct p4_header_type {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint16_t nfields;
    uint16_t total_bits;
    struct p4_field fields[P4_MAX_FIELDS];
};

struct p4_header {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    p4_idx_t header_type; /* index into header_types */
    bool metadata;
};

struct p4_program {
    uint16_t nheader_types;
    uint16_t nheaders;
    struct p4_header_type header_types[P4_MAX_HEADER_TYPES];
    struct p4_header headers[P4_MAX_HEADERS];
};

#endif /* P4_PROGRAM_H */
