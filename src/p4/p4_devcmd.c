/*
 * p4_devcmd.c — the guest's way in to the P4 engine
 *
 * See p4_devcmd.h for the wire format and why it is shaped like pds_fwctl
 * rather than being pds_fwctl.  This file is the device side of it, and
 * its whole job is to treat a guest as hostile: every length is checked
 * before it is used, every address is checked against what the guest may
 * reach, every response buffer is cleared before it is filled, and an
 * operation the operator has not granted is refused before it is parsed.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "ernic_dma.h"
#include "p4_devcmd.h"
#include "p4_engine.h"
#include "p4_json.h"
#include "p4_program.h"
#include "p4_table.h"

/*
 * The wire format repeats two of the engine's limits, because a guest
 * must not have to be recompiled when an internal constant moves.  These
 * assertions are what make the repetition safe: if the engine's limits
 * change, the build fails here rather than the ABI changing silently.
 */
_Static_assert(P4_DEVCMD_VALUE_MAX == P4_MATCH_BYTES,
               "the wire value width no longer matches the engine's");
_Static_assert(P4_DEVCMD_NAME_MAX == P4_MAX_NAME,
               "the wire name length no longer matches the engine's");
_Static_assert(P4_PORT_COUNT == 2, "struct p4_rpc_stats hard-codes two ports");
_Static_assert((int)P4_WIRE_UPDATE_INSERT == (int)P4_UPDATE_INSERT &&
                   (int)P4_WIRE_UPDATE_MODIFY == (int)P4_UPDATE_MODIFY &&
                   (int)P4_WIRE_UPDATE_DELETE == (int)P4_UPDATE_DELETE,
               "the wire update types no longer match the engine's");

/*
 * The most one RPC may move in each direction.
 *
 * A request is staged in a single heap buffer, so this is both the bound
 * on that allocation and the bound on how long a guest can keep the
 * vfio-user thread busy in one command.  A program arrives in chunks
 * precisely so that it does not have to fit here.
 */
#define P4_DEVCMD_REQ_MAX  (64u * 1024u)
#define P4_DEVCMD_RESP_MAX (4u * 1024u)

struct p4_devcmd {
    vfu_ctx_t *vfu_ctx;
    struct ernic_dma *dma;
    struct p4_engine *eng;
    enum p4_scope scope; /* the operator's grant; never widened */
};

/*
 * What each operation needs.  A table rather than a check inside each
 * handler, so that the scope a guest is refused for is the same one
 * QUERY advertises -- those two disagreeing is how a control plane comes
 * to believe it may do something the device will later refuse.
 */
struct p4_op_def {
    uint32_t ep;
    uint32_t op;
    enum p4_scope scope;
};

static const struct p4_op_def p4_ops[] = {
    {P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, P4_SCOPE_LOAD},
    {P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, P4_SCOPE_LOAD},
    {P4_EP_PROGRAM, P4_OP_LOAD_COMMIT, P4_SCOPE_LOAD},
    {P4_EP_PROGRAM, P4_OP_LOAD_ABORT, P4_SCOPE_LOAD},
    {P4_EP_TABLE, P4_OP_ENTRY_UPDATE, P4_SCOPE_WRITE},
    {P4_EP_STATS, P4_OP_STATS_READ, P4_SCOPE_READ},
};

static const uint32_t p4_endpoints[] = {P4_EP_PROGRAM, P4_EP_TABLE,
                                        P4_EP_STATS};

/* -------------------------------------------------------------------------
 * Scope
 * -------------------------------------------------------------------------
 */

bool p4_scope_parse(const char *name, enum p4_scope *out)
{
    static const struct {
        const char *name;
        enum p4_scope scope;
    } names[] = {
        {"none", P4_SCOPE_NONE},
        {"read", P4_SCOPE_READ},
        {"write", P4_SCOPE_WRITE},
        {"load", P4_SCOPE_LOAD},
    };

    if (!name || !out)
        return false;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i].name)) {
            *out = names[i].scope;
            return true;
        }
    }
    return false;
}

const char *p4_scope_name(enum p4_scope scope)
{
    switch (scope) {
    case P4_SCOPE_NONE:
        return "none";
    case P4_SCOPE_READ:
        return "read";
    case P4_SCOPE_WRITE:
        return "write";
    case P4_SCOPE_LOAD:
        return "load";
    default:
        /* Every enumerator is listed above, so this is only reachable
         * through a cast; it still needs a name rather than a crash. */
        return "?";
    }
}

/* The definition of @ep/@op, or NULL if this device has no such thing. */
static const struct p4_op_def *p4_find_op(uint32_t ep, uint32_t op)
{
    for (size_t i = 0; i < sizeof(p4_ops) / sizeof(p4_ops[0]); i++) {
        if (p4_ops[i].ep == ep && p4_ops[i].op == op)
            return &p4_ops[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Construction
 * -------------------------------------------------------------------------
 */

struct p4_devcmd *p4_devcmd_create(vfu_ctx_t *vfu_ctx, struct p4_engine *eng,
                                   enum p4_scope scope)
{
    struct p4_devcmd *pd;

    if (!vfu_ctx || !eng)
        return NULL;

    pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;
    pd->dma = ernic_dma_create();
    if (!pd->dma) {
        free(pd);
        return NULL;
    }

    pd->vfu_ctx = vfu_ctx;
    pd->eng = eng;
    pd->scope = scope;
    return pd;
}

void p4_devcmd_destroy(struct p4_devcmd *pd)
{
    if (!pd)
        return;
    ernic_dma_destroy(pd->dma);
    free(pd);
}

/* -------------------------------------------------------------------------
 * IDENT (70)
 * -------------------------------------------------------------------------
 */

static uint8_t handle_ident(struct p4_devcmd *pd, const uint8_t *cmd)
{
    struct p4_devcmd_ident_cmd c;
    struct p4_devcmd_ident id;
    uint32_t len;
    uint64_t pa;

    memcpy(&c, cmd, sizeof(c));
    len = le32toh(c.len);
    pa = le64toh(c.ident_pa);

    /* A driver that asks for less than the whole structure gets what it
     * asked for rather than an error: a newer device answering an older
     * driver is the ordinary case, and truncation is how that works. */
    if (len == 0 || len > sizeof(id))
        return P4_STS_EINVAL;

    memset(&id, 0, sizeof(id));
    id.abi_version = htole32(1);
    id.scope = htole32((uint32_t)pd->scope);
    id.max_program = htole32(P4_PROGRAM_MAX);
    id.max_chunk =
        htole32(P4_DEVCMD_REQ_MAX - (uint32_t)sizeof(struct p4_rpc_load_chunk));
    id.max_request = htole32(P4_DEVCMD_REQ_MAX);
    id.max_response = htole32(P4_DEVCMD_RESP_MAX);
    id.endpoints =
        htole32((uint32_t)(sizeof(p4_endpoints) / sizeof(p4_endpoints[0])));

    if (ernic_dma_write(pd->dma, pd->vfu_ctx, pa, &id, len) < 0)
        return P4_STS_EFAULT;

    return P4_STS_OK;
}

/* -------------------------------------------------------------------------
 * QUERY (72)
 * -------------------------------------------------------------------------
 */

static uint8_t handle_query(struct p4_devcmd *pd, const uint8_t *cmd)
{
    struct p4_devcmd_query_cmd c;
    uint8_t buf[sizeof(struct p4_devcmd_query_data) +
                sizeof(p4_ops) / sizeof(p4_ops[0]) *
                    sizeof(struct p4_query_operation)];
    struct p4_devcmd_query_data hdr;
    uint32_t buf_len, total = 0, fit, entry_size, want;
    uint8_t *out = buf + sizeof(hdr);
    size_t used = 0;

    memcpy(&c, cmd, sizeof(c));
    buf_len = le32toh(c.buf_len);

    if (buf_len < sizeof(hdr) || buf_len > P4_DEVCMD_RESP_MAX)
        return P4_STS_EINVAL;

    /*
     * Everything that leaves here is cleared first, header and entries
     * alike: the entries are written one field at a time out of a static
     * table, and the padding between them never is.
     */
    memset(buf, 0, sizeof(buf));
    memset(&hdr, 0, sizeof(hdr));

    switch (c.entity) {
    case P4_QUERY_ROOT:
        entry_size = (uint32_t)sizeof(struct p4_query_endpoint);
        total = (uint32_t)(sizeof(p4_endpoints) / sizeof(p4_endpoints[0]));
        break;
    case P4_QUERY_ENDPOINT:
        entry_size = (uint32_t)sizeof(struct p4_query_operation);
        for (size_t i = 0; i < sizeof(p4_ops) / sizeof(p4_ops[0]); i++) {
            if (p4_ops[i].ep == le32toh(c.ep))
                total++;
        }
        if (total == 0)
            return P4_STS_EOPCODE;
        break;
    default:
        return P4_STS_EINVAL;
    }

    /*
     * How many entries fit the buffer the guest offered, which is not the
     * same as how many exist.  Upstream's driver trusts the device's own
     * count here and walks that many entries out of a page it sized
     * itself; reporting both is what lets a guest retry rather than be
     * walked off the end of its buffer.
     */
    fit = (buf_len - (uint32_t)sizeof(hdr)) / entry_size;
    if (fit > total)
        fit = total;

    if (c.entity == P4_QUERY_ROOT) {
        for (uint32_t i = 0; i < fit; i++) {
            struct p4_query_endpoint e;

            memset(&e, 0, sizeof(e));
            e.id = htole32(p4_endpoints[i]);
            memcpy(out + used, &e, sizeof(e));
            used += sizeof(e);
        }
    } else {
        uint32_t n = 0;

        for (size_t i = 0; i < sizeof(p4_ops) / sizeof(p4_ops[0]) && n < fit;
             i++) {
            struct p4_query_operation e;

            if (p4_ops[i].ep != le32toh(c.ep))
                continue;
            memset(&e, 0, sizeof(e));
            e.id = htole32(p4_ops[i].op);
            e.scope = (uint8_t)p4_ops[i].scope;
            memcpy(out + used, &e, sizeof(e));
            used += sizeof(e);
            n++;
        }
    }

    hdr.version = 1;
    hdr.entry_size = (uint8_t)entry_size;
    hdr.num_entries = htole32(fit);
    hdr.total = htole32(total);
    memcpy(buf, &hdr, sizeof(hdr));

    want = (uint32_t)(sizeof(hdr) + used);
    if (ernic_dma_write(pd->dma, pd->vfu_ctx, le64toh(c.buf_pa), buf, want) < 0)
        return P4_STS_EFAULT;

    return P4_STS_OK;
}

/* -------------------------------------------------------------------------
 * RPC (71) payloads
 * -------------------------------------------------------------------------
 */

/* Map an engine errno onto the closed set the guest sees. */
static uint8_t p4_status_of(int rc)
{
    switch (rc) {
    case 0:
        return P4_STS_OK;
    case -EINVAL:
        return P4_STS_EINVAL;
    case -ENOENT:
        return P4_STS_ENOENT;
    case -EEXIST:
        return P4_STS_EEXIST;
    case -ENOSPC:
        return P4_STS_ENOSPC;
    case -EBUSY:
        return P4_STS_EBUSY;
    case -EFBIG:
    case -ERANGE:
        return P4_STS_ERANGE;
    default:
        return P4_STS_EDEVICE;
    }
}

static uint8_t rpc_load_begin(struct p4_devcmd *pd, const uint8_t *req,
                              uint32_t req_len)
{
    struct p4_rpc_load_begin b;

    if (req_len < sizeof(b))
        return P4_STS_EINVAL;
    memcpy(&b, req, sizeof(b));

    return p4_status_of(p4_engine_load_begin(pd->eng, le32toh(b.total_len)));
}

static uint8_t rpc_load_chunk(struct p4_devcmd *pd, const uint8_t *req,
                              uint32_t req_len)
{
    struct p4_rpc_load_chunk h;
    uint32_t len;

    if (req_len < sizeof(h))
        return P4_STS_EINVAL;
    memcpy(&h, req, sizeof(h));
    len = le32toh(h.len);

    /*
     * The bytes follow the header in the buffer already read, so the
     * check is that they are really there.  Subtract rather than add:
     * sizeof(h) + len would wrap for a len near UINT32_MAX and let a
     * short buffer look long enough.
     */
    if (len == 0 || len > req_len - (uint32_t)sizeof(h))
        return P4_STS_EINVAL;

    return p4_status_of(
        p4_engine_load_chunk(pd->eng, le64toh(h.offset), req + sizeof(h), len));
}

static uint8_t rpc_entry_update(struct p4_devcmd *pd, const uint8_t *req,
                                uint32_t req_len)
{
    struct p4_rpc_entry_update u;
    struct p4_update up;
    uint16_t nmatch, nparams;
    uint32_t need;
    const uint8_t *p;
    char err[160];
    int rc;

    if (req_len < sizeof(u))
        return P4_STS_EINVAL;
    memcpy(&u, req, sizeof(u));

    nmatch = le16toh(u.nmatch);
    nparams = le16toh(u.nparams);

    if (nmatch > P4_MAX_TABLE_KEY || nparams > P4_MAX_RUNTIME_ARGS)
        return P4_STS_EINVAL;

    /*
     * Both counts are bounded above, so this product cannot overflow a
     * uint32_t -- but it is written against the remaining length rather
     * than added to sizeof(u) for the same reason as the chunk above.
     */
    need = (uint32_t)nmatch * (uint32_t)sizeof(struct p4_rpc_entry_match) +
           (uint32_t)nparams * (uint32_t)sizeof(struct p4_rpc_entry_param);
    if (need > req_len - (uint32_t)sizeof(u))
        return P4_STS_EINVAL;

    memset(&up, 0, sizeof(up));
    up.type = u.type;
    up.priority = le32toh(u.priority);
    up.nmatch = nmatch;
    up.nparams = nparams;

    /*
     * The names arrive from a guest, so they are copied into a buffer one
     * byte longer and terminated here rather than trusted to be.  Both
     * fields are P4_MAX_NAME + 1 on the wire and in struct p4_update, and
     * the assertion at the top of this file is what keeps that true.
     */
    memcpy(up.table, u.table, P4_DEVCMD_NAME_MAX);
    up.table[P4_DEVCMD_NAME_MAX] = '\0';
    memcpy(up.action, u.action, P4_DEVCMD_NAME_MAX);
    up.action[P4_DEVCMD_NAME_MAX] = '\0';

    p = req + sizeof(u);
    for (uint16_t i = 0; i < nmatch; i++) {
        struct p4_rpc_entry_match m;

        memcpy(&m, p, sizeof(m));
        p += sizeof(m);
        if (le16toh(m.len) > P4_MATCH_BYTES)
            return P4_STS_EINVAL;
        up.match[i].len = le16toh(m.len);
        up.match[i].prefix_bits = le16toh(m.prefix_bits);
        memcpy(up.match[i].value, m.value, P4_MATCH_BYTES);
        memcpy(up.match[i].mask, m.mask, P4_MATCH_BYTES);
    }
    for (uint16_t i = 0; i < nparams; i++) {
        struct p4_rpc_entry_param a;

        memcpy(&a, p, sizeof(a));
        p += sizeof(a);
        if (le16toh(a.len) > P4_MATCH_BYTES)
            return P4_STS_EINVAL;
        up.params[i].len = le16toh(a.len);
        memcpy(up.params[i].value, a.value, P4_MATCH_BYTES);
    }

    err[0] = '\0';
    rc = p4_engine_update(pd->eng, &up, err, sizeof(err));
    if (rc < 0 && err[0]) {
        /* The engine says which table or action was wrong; the guest gets
         * only a status code, so the detail has to land in the log or it
         * is lost. */
        vfu_log(pd->vfu_ctx, LOG_WARNING,
                "p4_devcmd: guest table update refused: %s", err);
    }
    return p4_status_of(rc);
}

static uint8_t rpc_stats_read(struct p4_devcmd *pd, uint8_t *resp,
                              uint32_t resp_len, uint32_t *out_len)
{
    struct p4_stats es;
    struct p4_rpc_stats s;

    if (resp_len < sizeof(s))
        return P4_STS_ERANGE;

    p4_engine_get_stats(pd->eng, &es);

    memset(&s, 0, sizeof(s));
    for (size_t i = 0; i < P4_PORT_COUNT; i++) {
        s.frames_in[i] = htole64(es.frames_in[i]);
        s.frames_out[i] = htole64(es.frames_out[i]);
    }
    s.frames_dropped = htole64(es.frames_dropped);
    s.frames_too_big = htole64(es.frames_too_big);
    s.parse_errors = htole64(es.parse_errors);

    memcpy(resp, &s, sizeof(s));
    *out_len = (uint32_t)sizeof(s);
    return P4_STS_OK;
}

/* -------------------------------------------------------------------------
 * RPC (71)
 * -------------------------------------------------------------------------
 */

static uint8_t handle_rpc(struct p4_devcmd *pd, const uint8_t *cmd,
                          uint32_t *resp_written)
{
    struct p4_devcmd_rpc_cmd c;
    const struct p4_op_def *def;
    uint8_t *req = NULL, *resp = NULL;
    uint32_t req_len, resp_len, ep, op;
    uint8_t status;

    memcpy(&c, cmd, sizeof(c));
    ep = le32toh(c.ep);
    op = le32toh(c.op);
    req_len = le32toh(c.req_len);
    resp_len = le32toh(c.resp_len);

    def = p4_find_op(ep, op);
    if (!def)
        return P4_STS_EOPCODE;

    /*
     * Two separate checks, in this order.  The operator's grant comes
     * first because it is the one that must not be inferable: a guest
     * probing operations would otherwise learn which ones exist from the
     * difference between "not permitted" and "asked for too little".
     */
    if (pd->scope < def->scope || pd->scope == P4_SCOPE_NONE)
        return P4_STS_EPERM;
    if ((enum p4_scope)c.scope < def->scope)
        return P4_STS_EPERM;

    if (req_len > P4_DEVCMD_REQ_MAX || resp_len > P4_DEVCMD_RESP_MAX)
        return P4_STS_ERANGE;

    if (req_len) {
        req = malloc(req_len);
        if (!req)
            return P4_STS_EDEVICE;
        if (ernic_dma_read(pd->dma, pd->vfu_ctx, le64toh(c.req_pa), req,
                           req_len) < 0) {
            free(req);
            return P4_STS_EFAULT;
        }
    }
    if (resp_len) {
        /* Cleared, not merely allocated: a handler that fills less than
         * the whole buffer must not send the remainder of a heap block to
         * the guest. */
        resp = calloc(1, resp_len);
        if (!resp) {
            free(req);
            return P4_STS_EDEVICE;
        }
    }

    switch (ep) {
    case P4_EP_PROGRAM:
        switch (op) {
        case P4_OP_LOAD_BEGIN:
            status = rpc_load_begin(pd, req, req_len);
            break;
        case P4_OP_LOAD_CHUNK:
            status = rpc_load_chunk(pd, req, req_len);
            break;
        case P4_OP_LOAD_COMMIT:
            status = p4_status_of(p4_engine_load_commit(pd->eng));
            if (status != P4_STS_OK) {
                const char *why = p4_engine_load_error(pd->eng);

                /* The guest cannot be told which P4 feature it used, so
                 * the operator has to be able to find out. */
                vfu_log(pd->vfu_ctx, LOG_WARNING,
                        "p4_devcmd: guest program refused: %s",
                        why ? why : "(no reason given)");
            }
            break;
        case P4_OP_LOAD_ABORT:
            p4_engine_load_abort(pd->eng);
            status = P4_STS_OK;
            break;
        default:
            status = P4_STS_EOPCODE;
            break;
        }
        break;
    case P4_EP_TABLE:
        status = rpc_entry_update(pd, req, req_len);
        break;
    case P4_EP_STATS:
        status = rpc_stats_read(pd, resp, resp_len, resp_written);
        break;
    default:
        status = P4_STS_EOPCODE;
        break;
    }

    if (status == P4_STS_OK && *resp_written) {
        if (ernic_dma_write(pd->dma, pd->vfu_ctx, le64toh(c.resp_pa), resp,
                            *resp_written) < 0)
            status = P4_STS_EFAULT;
    }

    free(req);
    free(resp);
    return status;
}

/* -------------------------------------------------------------------------
 * Dispatch
 * -------------------------------------------------------------------------
 */

void p4_devcmd_dispatch(void *opaque, const uint8_t *cmd, uint8_t *comp)
{
    struct p4_devcmd *pd = opaque;
    struct p4_devcmd_rpc_comp rc;
    uint32_t resp_written = 0;
    uint8_t status;

    /* Goes straight to the guest, so it is cleared before it is filled
     * and the whole 16 bytes are written, padding included. */
    memset(&rc, 0, sizeof(rc));

    if (!pd) {
        rc.status = P4_STS_EDEVICE;
        memcpy(comp, &rc, sizeof(rc));
        return;
    }

    /*
     * A device the operator granted nothing answers nothing, including
     * IDENT: a guest that cannot act on the engine has no business
     * learning its limits, and a driver that gets a refusal here will not
     * go on to probe the rest.
     */
    if (pd->scope == P4_SCOPE_NONE) {
        rc.status = P4_STS_EPERM;
        memcpy(comp, &rc, sizeof(rc));
        return;
    }

    switch (cmd[0]) {
    case IONIC_CMD_P4_IDENT:
        status = handle_ident(pd, cmd);
        break;
    case IONIC_CMD_P4_QUERY:
        status = handle_query(pd, cmd);
        break;
    case IONIC_CMD_P4_RPC:
        status = handle_rpc(pd, cmd, &resp_written);
        break;
    default:
        vfu_log(pd->vfu_ctx, LOG_WARNING, "p4_devcmd: unknown opcode=%u",
                cmd[0]);
        status = P4_STS_EOPCODE;
        break;
    }

    rc.status = status;
    rc.resp_len = htole32(status == P4_STS_OK ? resp_written : 0);
    memcpy(comp, &rc, sizeof(rc));
}
