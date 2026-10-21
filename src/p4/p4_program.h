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

#define P4_MAX_PARSE_STATES   64
#define P4_MAX_PARSER_OPS     16 /* per state */
#define P4_MAX_TRANSITIONS    32 /* per state */
#define P4_MAX_TRANSITION_KEY 8  /* fields per select */
#define P4_MAX_KEY_BYTES      16 /* total width of a select key */
#define P4_MAX_DEPARSE_ORDER  64

/* A reference to one field of one header instance, resolved at load. */
struct p4_field_ref {
    p4_idx_t header;
    p4_idx_t field;
};

/* Parser operations. Only extract is supported; the rest are named in the
 * rejection so a user learns which one they used. */
enum p4_parser_op_kind {
    P4_POP_EXTRACT = 0,
};

struct p4_parser_op {
    uint8_t kind;
    p4_idx_t header;
};

/*
 * One arm of a select. A default arm matches anything; the rest compare
 * the select key against @value, under @mask when the program gave one.
 * @next_state of P4_IDX_NONE means the parser accepts here.
 */
struct p4_transition {
    bool is_default;
    bool has_mask;
    uint8_t value[P4_MAX_KEY_BYTES];
    uint8_t mask[P4_MAX_KEY_BYTES];
    p4_idx_t next_state;
};

struct p4_parse_state {
    char name[P4_MAX_NAME + 1];
    uint16_t nops;
    uint16_t nkey;
    uint16_t ntransitions;
    uint16_t key_bytes; /* width of the select key, from its fields */
    struct p4_parser_op ops[P4_MAX_PARSER_OPS];
    struct p4_field_ref key[P4_MAX_TRANSITION_KEY];
    struct p4_transition transitions[P4_MAX_TRANSITIONS];
};

#define P4_MAX_ACTIONS      64
#define P4_MAX_RUNTIME_ARGS 8  /* per action */
#define P4_MAX_PRIMITIVES   32 /* per action */
#define P4_MAX_PRIM_PARAMS  4  /* per primitive */
#define P4_MAX_IMM_BYTES    16 /* widest literal an operand may carry */

/*
 * The primitives an action may use.  The list is an allow-list rather than
 * a deny-list: bmv2 has dozens, several of which abort the switch when they
 * fail, so anything not named here is refused by name.
 */
enum p4_prim_kind {
    P4_PRIM_ASSIGN = 0, /* assign, modify_field */
    P4_PRIM_ADD,        /* add, add_to_field */
    P4_PRIM_SUB,        /* subtract, subtract_from_field */
    P4_PRIM_DROP,       /* drop, mark_to_drop */
    P4_PRIM_NOP,        /* no_op */
};

enum p4_operand_kind {
    P4_OPD_FIELD = 0,
    P4_OPD_HEXSTR,
    P4_OPD_RUNTIME, /* an argument supplied with the table entry */
};

struct p4_operand {
    uint8_t kind;
    struct p4_field_ref field; /* P4_OPD_FIELD */
    p4_idx_t runtime;          /* P4_OPD_RUNTIME: index into runtime args */
    uint16_t imm_len;          /* P4_OPD_HEXSTR: bytes the digits needed */
    bool imm_negative;
    uint8_t imm[P4_MAX_IMM_BYTES];
};

struct p4_primitive {
    uint8_t kind;
    uint8_t nparams;
    struct p4_operand params[P4_MAX_PRIM_PARAMS];
};

struct p4_action {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint16_t nruntime;
    uint16_t nprimitives;
    /* Width of each runtime argument, so an entry's data can be checked. */
    uint16_t runtime_bits[P4_MAX_RUNTIME_ARGS];
    struct p4_primitive primitives[P4_MAX_PRIMITIVES];
};

struct p4_program {
    uint16_t nactions;
    struct p4_action actions[P4_MAX_ACTIONS];
    uint16_t nheader_types;
    uint16_t nheaders;
    uint16_t nparse_states;
    uint16_t ndeparse_order;
    p4_idx_t init_state;
    struct p4_header_type header_types[P4_MAX_HEADER_TYPES];
    struct p4_header headers[P4_MAX_HEADERS];
    struct p4_parse_state parse_states[P4_MAX_PARSE_STATES];
    /* Header instances the deparser emits, in order. */
    p4_idx_t deparse_order[P4_MAX_DEPARSE_ORDER];
};

#endif /* P4_PROGRAM_H */
