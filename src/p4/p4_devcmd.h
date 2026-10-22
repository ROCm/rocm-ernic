/*
 * p4_devcmd.h — the guest's way in to the P4 engine
 *
 * Until now a P4 program could only be loaded by the operator, on the
 * server's command line, and a table entry could not be installed by
 * anyone at all: p4_engine_update() had no caller outside the unit tests.
 * This is the command surface that changes that, so a driver in the guest
 * can push a program down and program its tables.
 *
 * Shaped like pds_fwctl, not wire-identical to it.  Upstream pds_fwctl
 * (Documentation/userspace-api/fwctl/pds_fwctl.rst) binds to a
 * `pds_core.fwctl.N` auxiliary device and its commands never travel as a
 * top-level opcode: the driver wraps each one in PDS_AQ_CMD_CLIENT_CMD
 * (opcode 8) with a 60-byte inner payload, after registering for a
 * client id.  rocm-ernic emulates ionic, not pds_core, so there is no
 * pds_core admin queue to wrap into.  What carries over is the shape that
 * matters -- an endpoint id, an operation id, a scope the device checks,
 * and in/out buffers named by guest address -- and the opcode numbers,
 * which are pds_fwctl's own:
 *
 *     PDS_FWCTL_CMD_IDENT = 70, _RPC = 71, _QUERY = 72
 *         (include/linux/pds/pds_adminq.h)
 *
 * Keeping the numbers costs nothing and means a future pds_core-shaped
 * control device can present the same operations without renumbering.
 * docs/p4.rst records the divergence.
 *
 * Everything here crosses the guest boundary, so every field is
 * explicitly sized and little-endian on the wire, every structure has a
 * _Static_assert on its size so that padding introduced by a later edit
 * fails the build, and every response is memset before it is filled.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_DEVCMD_H
#define P4_DEVCMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

struct p4_engine;
struct p4_devcmd;

/*
 * The opcodes, as the guest sends them.  They arrive both through the
 * BAR0 devcmd window and over the Ethernet admin queue, as the RDMA
 * opcodes 50-53 do, because a driver that comes up before its admin queue
 * has only the former.
 */
#define IONIC_CMD_P4_IDENT 70
#define IONIC_CMD_P4_RPC   71
#define IONIC_CMD_P4_QUERY 72

/*
 * What a guest is allowed to do, and the operator's decision about it.
 *
 * A guest that can replace the P4 program can redirect every frame the
 * device handles and can decide which RDMA operations become fp8
 * reductions.  That is a large capability, and the whole point of a
 * scope is that it is not granted by default: the device declares a
 * required scope per operation and refuses a request that asks for less
 * than the operation needs, or more than the operator has allowed.
 *
 * Ordered, and compared with <=, so a wider grant includes a narrower
 * one.  The order is the axis that matters here -- how much of the
 * device's behaviour the request can change -- which is not the axis
 * upstream fwctl's enum fwctl_rpc_scope uses (CONFIGURATION,
 * DEBUG_READ_ONLY, DEBUG_WRITE, DEBUG_WRITE_FULL, graded by how much
 * debug exposure they carry).  Naming them separately is honest about
 * that; a mapping, if one is ever needed, belongs in the driver.
 */
enum p4_scope {
    P4_SCOPE_NONE = 0,  /* nothing: the surface answers no command */
    P4_SCOPE_READ = 1,  /* identify, enumerate, read counters */
    P4_SCOPE_WRITE = 2, /* install, modify and delete table entries */
    P4_SCOPE_LOAD = 3,  /* replace the running program */
};

/* Endpoints, and the operations each one offers. */
enum p4_devcmd_endpoint {
    P4_EP_PROGRAM = 1, /* the loaded program */
    P4_EP_TABLE = 2,   /* its table entries */
    P4_EP_STATS = 3,   /* the engine's counters */
};

enum p4_devcmd_op_program {
    P4_OP_LOAD_BEGIN = 1,
    P4_OP_LOAD_CHUNK = 2,
    P4_OP_LOAD_COMMIT = 3,
    P4_OP_LOAD_ABORT = 4,
};

enum p4_devcmd_op_table {
    P4_OP_ENTRY_UPDATE = 1, /* insert, modify or delete, per the record */
};

enum p4_devcmd_op_stats {
    P4_OP_STATS_READ = 1,
};

/*
 * Status codes, returned in byte 0 of the 16-byte completion.
 *
 * Deliberately a small closed set rather than an errno: an errno means
 * different numbers on different architectures, and this one crosses to
 * a guest that may not share ours.
 */
enum p4_devcmd_status {
    P4_STS_OK = 0,
    P4_STS_EOPCODE = 1,  /* no such endpoint or operation */
    P4_STS_EPERM = 2,    /* the operator has not granted that scope */
    P4_STS_EINVAL = 3,   /* a malformed request */
    P4_STS_ERANGE = 4,   /* a length or address outside what is allowed */
    P4_STS_EFAULT = 5,   /* guest memory could not be read or written */
    P4_STS_ENOENT = 6,   /* no program loaded */
    P4_STS_EEXIST = 7,   /* the entry is already there */
    P4_STS_ENOSPC = 8,   /* the table is full */
    P4_STS_EBUSY = 9,    /* a load is already in progress */
    P4_STS_EDEVICE = 10, /* the engine refused it; see the server log */
};

/* ---- IDENT (70) ------------------------------------------------------ */

/*
 * The command.  @len and @ident_pa name a buffer in guest memory that the
 * device fills with a struct p4_devcmd_ident.
 */
struct p4_devcmd_ident_cmd {
    uint8_t opcode; /* IONIC_CMD_P4_IDENT */
    uint8_t rsvd;
    uint8_t version; /* the ABI the driver speaks; 0 today */
    uint8_t rsvd2;
    uint32_t len; /* little-endian */
    uint64_t ident_pa;
    uint8_t rsvd3[48];
};
_Static_assert(sizeof(struct p4_devcmd_ident_cmd) == 64,
               "P4 IDENT command must fill the 64-byte devcmd buffer");

/*
 * What the device is and what this guest may do with it.
 *
 * @scope is the operator's grant, so a driver can tell the difference
 * between an operation this build does not have and one it is simply not
 * allowed to perform, and say which in its own log rather than making the
 * user guess from -EPERM.
 */
struct p4_devcmd_ident {
    uint32_t abi_version;  /* 1 */
    uint32_t scope;        /* enum p4_scope, the operator's grant */
    uint32_t max_program;  /* bytes; P4_PROGRAM_MAX */
    uint32_t max_chunk;    /* bytes a single LOAD_CHUNK may carry */
    uint32_t max_request;  /* bytes any one RPC request buffer may be */
    uint32_t max_response; /* bytes any one RPC response buffer may be */
    uint32_t endpoints;    /* how many QUERY(ROOT) will return */
    uint32_t rsvd;
};
_Static_assert(sizeof(struct p4_devcmd_ident) == 32,
               "struct p4_devcmd_ident has padding or has changed size");

/* ---- QUERY (72) ------------------------------------------------------ */

enum p4_query_entity {
    P4_QUERY_ROOT = 0,     /* -> the endpoint ids */
    P4_QUERY_ENDPOINT = 1, /* -> the operations of @ep, with their scopes */
};

struct p4_devcmd_query_cmd {
    uint8_t opcode; /* IONIC_CMD_P4_QUERY */
    uint8_t entity; /* enum p4_query_entity */
    uint8_t version;
    uint8_t rsvd;
    uint32_t buf_len;
    uint64_t buf_pa;
    uint32_t ep; /* when @entity is P4_QUERY_ENDPOINT */
    uint8_t rsvd2[44];
};
_Static_assert(sizeof(struct p4_devcmd_query_cmd) == 64,
               "P4 QUERY command must fill the 64-byte devcmd buffer");

/*
 * The answer, written into the guest buffer.
 *
 * @num_entries counts entries that FIT the buffer the guest supplied; the
 * device truncates and reports @total so the guest can retry with a
 * larger one.  Upstream's driver walks num_entries straight out of the
 * DMA buffer without checking it against the length it asked for
 * (drivers/fwctl/pds/main.c), which is a trap worth not building: a
 * device that over-reports there walks the host off the end of a page.
 */
struct p4_devcmd_query_data {
    uint8_t version;
    uint8_t entry_size;
    uint8_t rsvd[2];
    uint32_t num_entries; /* entries present in this buffer */
    uint32_t total;       /* entries that exist */
    uint32_t rsvd2;
    /* followed by @num_entries entries of @entry_size bytes */
};
_Static_assert(sizeof(struct p4_devcmd_query_data) == 16,
               "struct p4_devcmd_query_data has padding or has changed size");

/* QUERY(ROOT) entry. */
struct p4_query_endpoint {
    uint32_t id; /* enum p4_devcmd_endpoint */
};
_Static_assert(sizeof(struct p4_query_endpoint) == 4,
               "struct p4_query_endpoint has padding");

/* QUERY(ENDPOINT) entry: an operation and the scope it needs. */
struct p4_query_operation {
    uint32_t id;
    uint8_t scope; /* enum p4_scope */
    uint8_t rsvd[3];
};
_Static_assert(sizeof(struct p4_query_operation) == 8,
               "struct p4_query_operation has padding");

/* ---- RPC (71) -------------------------------------------------------- */

/*
 * One call on an endpoint.
 *
 * The request and the response are separate buffers because they are
 * separate permissions: the device reads one and writes the other, and
 * nothing is gained by letting a guest point both at the same place.
 */
struct p4_devcmd_rpc_cmd {
    uint8_t opcode; /* IONIC_CMD_P4_RPC */
    uint8_t scope;  /* enum p4_scope: what the caller is asking for */
    uint8_t rsvd[2];
    uint32_t ep;
    uint32_t op;
    uint32_t req_len;
    uint64_t req_pa;
    uint32_t resp_len;
    uint32_t rsvd2;
    uint64_t resp_pa;
    uint8_t rsvd3[24];
};
_Static_assert(sizeof(struct p4_devcmd_rpc_cmd) == 64,
               "P4 RPC command must fill the 64-byte devcmd buffer");

/*
 * The completion.  @resp_len is how much the device actually wrote, which
 * for a truncated answer is less than the buffer the guest offered.
 */
struct p4_devcmd_rpc_comp {
    uint8_t status; /* enum p4_devcmd_status */
    uint8_t rsvd[3];
    uint32_t resp_len;
    uint8_t rsvd2[8];
};
_Static_assert(sizeof(struct p4_devcmd_rpc_comp) == 16,
               "P4 RPC completion must fill the 16-byte completion buffer");

/* ---- The RPC payloads ------------------------------------------------ */

/* P4_EP_PROGRAM / P4_OP_LOAD_BEGIN: how long the program will be. */
struct p4_rpc_load_begin {
    uint32_t total_len;
    uint32_t rsvd;
};
_Static_assert(sizeof(struct p4_rpc_load_begin) == 8,
               "struct p4_rpc_load_begin has padding");

/*
 * P4_EP_PROGRAM / P4_OP_LOAD_CHUNK: @len bytes at @offset of the program.
 * The bytes follow this header in the same request buffer, so one DMA
 * read gets both and a chunk cannot name memory its header did not.
 */
struct p4_rpc_load_chunk {
    uint64_t offset;
    uint32_t len;
    uint32_t rsvd;
    /* followed by @len bytes */
};
_Static_assert(sizeof(struct p4_rpc_load_chunk) == 16,
               "struct p4_rpc_load_chunk has padding");

/*
 * P4_EP_TABLE / P4_OP_ENTRY_UPDATE: one P4Runtime-shaped update.
 *
 * Names rather than numbers, matching struct p4_update: a control plane
 * that has the p4info has the names, and resolving them in the engine
 * means a wrong one is an error naming the table rather than an index
 * into another one.  The fields are laid out explicitly rather than by
 * embedding struct p4_update, which is a host structure whose layout is
 * nobody's business but ours.
 */
#define P4_DEVCMD_NAME_MAX  63
#define P4_DEVCMD_VALUE_MAX 16

/*
 * Mirrors struct p4_match_spec.  Which of these fields matters is decided
 * by the table's own key definition, not by the record: an exact field
 * reads @value, an LPM field reads @prefix_bits as well, a ternary field
 * reads @mask.  The device does not infer a match kind from which fields
 * are set, because a guest that set the wrong ones would then get a
 * different match than it asked for.
 */
struct p4_rpc_entry_match {
    uint16_t len;         /* significant bytes of @value and @mask */
    uint16_t prefix_bits; /* LPM only */
    uint8_t value[P4_DEVCMD_VALUE_MAX];
    uint8_t mask[P4_DEVCMD_VALUE_MAX];
};
_Static_assert(sizeof(struct p4_rpc_entry_match) == 36,
               "struct p4_rpc_entry_match has padding");

/* Mirrors struct p4_param: one argument of the action the entry runs. */
struct p4_rpc_entry_param {
    uint16_t len;
    uint16_t rsvd;
    uint8_t value[P4_DEVCMD_VALUE_MAX];
};
_Static_assert(sizeof(struct p4_rpc_entry_param) == 20,
               "struct p4_rpc_entry_param has padding");

struct p4_rpc_entry_update {
    uint8_t type; /* enum p4_update_type */
    uint8_t rsvd[3];
    uint32_t priority;
    uint16_t nmatch;
    uint16_t nparams;
    char table[P4_DEVCMD_NAME_MAX + 1];
    char action[P4_DEVCMD_NAME_MAX + 1];
    /* followed by @nmatch struct p4_rpc_entry_match, then @nparams
     * struct p4_rpc_entry_param */
};
_Static_assert(sizeof(struct p4_rpc_entry_update) == 140,
               "struct p4_rpc_entry_update has padding or has changed size");

/*
 * P4_EP_STATS / P4_OP_STATS_READ response.
 *
 * The engine's counters have had exactly one way out of the process until
 * now -- a line printed at shutdown -- which a guest cannot see and a
 * running test cannot read.
 */
/*
 * Mirrors struct p4_stats exactly, and deliberately adds nothing: the
 * engine keeps these seven counters and no others, and a wire structure
 * with a field the device cannot fill is a field that reads as zero
 * forever while looking like a measurement.  The two-element arrays are
 * P4_PORT_COUNT, which p4_devcmd.c asserts still is 2.
 */
struct p4_rpc_stats {
    uint64_t frames_in[2];
    uint64_t frames_out[2];
    uint64_t frames_dropped;
    uint64_t frames_too_big;
    uint64_t parse_errors;
};
_Static_assert(sizeof(struct p4_rpc_stats) == 56,
               "struct p4_rpc_stats has padding or has changed size");

/* ---- The module --------------------------------------------------------
 *
 * Created with the scope the operator granted, which it never widens.
 */
struct p4_devcmd *p4_devcmd_create(vfu_ctx_t *vfu_ctx, struct p4_engine *eng,
                                   enum p4_scope scope);
void p4_devcmd_destroy(struct p4_devcmd *pd);

/*
 * Handle one command.  Matches ionic_rdma_devcmd_fn_t: @cmd is the
 * 64-byte command and @comp the 16-byte completion, which this clears
 * before filling.
 */
void p4_devcmd_dispatch(void *opaque, const uint8_t *cmd, uint8_t *comp);

/* Parse an operator-supplied scope name. Returns false if it is not one. */
bool p4_scope_parse(const char *name, enum p4_scope *out);

/* The name of @scope, for a log line. */
const char *p4_scope_name(enum p4_scope scope);

#endif /* P4_DEVCMD_H */
