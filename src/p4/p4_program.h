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
#include <stddef.h>
#include <stdint.h>

/* Longest name accepted for any object in the program. */
#define P4_MAX_NAME 63

/*
 * True when the @n bytes at @s are all printable ASCII.
 *
 * Names come from the program document and from a guest's table updates,
 * and both kinds reach the log and the error messages.  A newline or an
 * ANSI escape in one is log injection, so every name taken from outside is
 * held to this before anything compares, stores or prints it.
 */
static inline bool p4_name_printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* Through unsigned char, so a byte past 0x7F is refused whether
         * or not plain char is signed. */
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

#define P4_MAX_HEADER_TYPES 64
#define P4_MAX_FIELDS       64 /* per header type */
#define P4_MAX_HEADERS      64
#define P4_MAX_FIELD_BITS   512

/*
 * The packet header vector: the storage the executor gives the headers and
 * metadata, in bytes.  The caps above allow programs far larger than this,
 * so the loader lays the headers out and refuses a program they overflow.
 */
#define P4_PHV_BYTES 1024

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
    uint16_t phv_offset; /* byte offset of its storage in the PHV */
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

/*
 * Widest table key field or action argument, in bytes: what a table entry
 * carries (P4_MATCH_BYTES in p4_table.h).  128 bits covers an IPv6
 * address, which is the widest thing a real program matches on.  A field
 * wider than this can still be parsed, deparsed and assigned; the loader
 * refuses one as a table key, and an action argument wider than this,
 * because no entry could fully specify either.
 */
#define P4_MAX_ENTRY_BYTES 16

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
    P4_PRIM_EXTERN,     /* _<Type>_<method>: a call out of the pipeline */
};

#define P4_MAX_EXTERNS 8

/*
 * The extern types this target provides.  A program may only instantiate
 * one of these; p4c will compile any extern it is told to with
 * --emit-externs, so the loader is what decides which actually exist.
 */
enum p4_extern_type {
    P4_EXTERN_FP8 = 0,
};

/* The methods those types have, as one space: a primitive names both. */
enum p4_extern_method {
    P4_EXTERN_FP8_MAP = 0,
    P4_EXTERN_FP8_CVT = 1,
};

struct p4_extern_inst {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint8_t type;
};

enum p4_operand_kind {
    P4_OPD_FIELD = 0,
    P4_OPD_HEXSTR,
    P4_OPD_RUNTIME, /* an argument supplied with the table entry */
    P4_OPD_EXTERN,  /* the instance a call is made on */
};

struct p4_operand {
    uint8_t kind;
    struct p4_field_ref field; /* P4_OPD_FIELD */
    p4_idx_t runtime;          /* P4_OPD_RUNTIME: index into runtime args */
    p4_idx_t inst;             /* P4_OPD_EXTERN: index into externs */
    uint16_t imm_len;          /* P4_OPD_HEXSTR: bytes the digits needed */
    uint8_t imm[P4_MAX_IMM_BYTES];
};

struct p4_primitive {
    uint8_t kind;
    uint8_t method; /* P4_PRIM_EXTERN: which one */
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

#define P4_MAX_PIPELINES     2
#define P4_MAX_TABLES        32 /* per pipeline */
#define P4_MAX_CONDITIONALS  32 /* per pipeline */
#define P4_MAX_NODES         (P4_MAX_TABLES + P4_MAX_CONDITIONALS)
#define P4_MAX_TABLE_KEY     8  /* key fields per table */
#define P4_MAX_TABLE_ACTIONS 16 /* actions per table */
#define P4_MAX_EXPR_NODES    64 /* per conditional */
#define P4_MAX_EXPR_DEPTH    8

/*
 * An aggregate ceiling on the entries all tables together may claim, in
 * every pipeline.
 * max_size is the one number in the document that names a large allocation
 * without being bounded by the size of the document: a few kilobytes of
 * JSON can otherwise ask for a gigabyte of host heap.
 */
#define P4_MAX_TOTAL_ENTRIES 65536u

/*
 * In increasing generality: a table is the most general of its keys' kinds,
 * and the loader relies on the order to check that.
 */
enum p4_match_kind {
    P4_MATCH_EXACT = 0,
    P4_MATCH_LPM,
    P4_MATCH_TERNARY,
};

/*
 * One key field.  p4c writes `field & constant` as the field with a
 * @mask, which bmv2 applies to the packet's key and to every entry's
 * before they meet; @mask is right-aligned, like a value.
 */
struct p4_table_key {
    uint8_t match_type;
    bool has_mask;
    struct p4_field_ref field;
    uint8_t mask[P4_MAX_ENTRY_BYTES];
};

/*
 * A table.  @next is the node each action leads to, in the same order as
 * @actions; @base_default_next is where a miss goes.  That last one is
 * undocumented and load-bearing: without it a miss falls off the end of
 * the pipeline, which is a wrong forwarding decision rather than a crash.
 */
struct p4_table {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint32_t max_size;
    uint8_t match_type;
    uint16_t nkey;
    uint16_t nactions;
    struct p4_table_key key[P4_MAX_TABLE_KEY];
    p4_idx_t lpm_key; /* into @key; P4_IDX_NONE when no key is LPM */
    p4_idx_t actions[P4_MAX_TABLE_ACTIONS]; /* into program actions */
    p4_idx_t next[P4_MAX_TABLE_ACTIONS];    /* into pipeline nodes */
    p4_idx_t base_default_next;
    p4_idx_t default_action; /* P4_IDX_NONE when the program gave none */
    /* Arguments for the default action, as the program fixed them. */
    uint16_t ndefault_data;
    uint16_t default_data_len[P4_MAX_RUNTIME_ARGS];
    uint8_t default_data[P4_MAX_RUNTIME_ARGS][P4_MAX_IMM_BYTES];
};

enum p4_expr_op {
    P4_EXPR_VALID = 0,
    P4_EXPR_EQ,
    P4_EXPR_NE,
    P4_EXPR_LT,
    P4_EXPR_LE,
    P4_EXPR_GT,
    P4_EXPR_GE,
    P4_EXPR_AND,
    P4_EXPR_OR,
    P4_EXPR_NOT,
};

enum p4_expr_operand_kind {
    P4_EO_NONE = 0, /* a unary operator's absent side */
    P4_EO_EXPR,
    P4_EO_FIELD,
    P4_EO_HEXSTR,
    P4_EO_HEADER,
    P4_EO_BOOL,
};

struct p4_expr_operand {
    uint8_t kind;
    bool bval;
    p4_idx_t expr;   /* P4_EO_EXPR: strictly greater than its parent */
    p4_idx_t header; /* P4_EO_HEADER */
    struct p4_field_ref field;
    uint16_t imm_len;
    uint8_t imm[P4_MAX_IMM_BYTES];
};

struct p4_expr_node {
    uint8_t op;
    struct p4_expr_operand left;
    struct p4_expr_operand right;
};

/*
 * A conditional.  The expression is a flat array whose root is index 0 and
 * in which a child's index is always greater than its parent's, so the
 * tree cannot contain a cycle and can be evaluated without recursion.
 */
struct p4_conditional {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint16_t nexpr;
    p4_idx_t true_next;
    p4_idx_t false_next;
    struct p4_expr_node expr[P4_MAX_EXPR_NODES];
};

enum p4_node_kind {
    P4_NODE_TABLE = 0,
    P4_NODE_CONDITIONAL,
};

/* One step of a pipeline: a table to apply or a condition to test. */
struct p4_node {
    uint8_t kind;
    p4_idx_t index; /* into tables[] or conditionals[] */
};

struct p4_pipeline {
    char name[P4_MAX_NAME + 1];
    uint32_t id;
    uint16_t ntables;
    uint16_t nconditionals;
    uint16_t nnodes;
    p4_idx_t init_node;
    struct p4_node nodes[P4_MAX_NODES];
    struct p4_table tables[P4_MAX_TABLES];
    struct p4_conditional conditionals[P4_MAX_CONDITIONALS];
};

struct p4_program {
    /*
     * The standard metadata fields the architecture gives meaning to.
     * The program addresses them like any other field, so they are
     * resolved once at load rather than searched for per packet.
     * egress_spec is how a program says where a packet goes, so a program
     * without it cannot forward and is refused.
     */
    struct p4_field_ref egress_spec;
    struct p4_field_ref ingress_port;

    /*
     * The RDMA operation a program may match on, when it declares a
     * header called rdma_metadata.  Every field is optional and any the
     * program does not declare stays P4_IDX_NONE, so a program reads only
     * what it asked for.  @has_rdma_metadata gates the whole RDMA path:
     * without the header the pipeline is never run for an operation, and
     * a program written only for Ethernet costs that path nothing.
     */
    uint16_t nexterns;
    struct p4_extern_inst externs[P4_MAX_EXTERNS];

    bool has_rdma_metadata;
    struct p4_field_ref rdma_opcode;
    struct p4_field_ref rdma_qp_num;
    struct p4_field_ref rdma_length;
    struct p4_field_ref rdma_imm_data;
    struct p4_field_ref rdma_rkey;

    uint16_t npipelines;
    struct p4_pipeline pipelines[P4_MAX_PIPELINES];
    uint16_t nactions;
    struct p4_action actions[P4_MAX_ACTIONS];
    uint16_t nheader_types;
    uint16_t nheaders;
    /* Bytes of the PHV the headers take, at most P4_PHV_BYTES. */
    uint16_t phv_bytes;
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
