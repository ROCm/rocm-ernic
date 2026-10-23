/*
 * p4_json.c — the bmv2 JSON walker
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json_object.h>
/* json_object_object_foreach expands to the link-hash walkers. */
#include <json-c/linkhash.h>
#include <json-c/json_tokener.h>

#include "p4_engine.h"
#include "p4_json.h"
#include "p4_program.h"
#include "parse_int.h"

/*
 * The whole document is handed to json-c in one call, so the input cap is
 * also the only bound on the size of the tree json-c builds from it.
 */
_Static_assert(P4_PROGRAM_MAX <= INT_MAX,
               "json_tokener_parse_ex takes an int length");

/* Walk state: the destination, and somewhere to record the first problem. */
struct p4_walk {
    struct p4_program *prog;
    char *err;
    size_t err_len;
    bool failed;
};

/*
 * Record the first failure and keep its reason.  Later failures are
 * discarded: the first is the one that explains the file, and a cascade of
 * consequences after it only buries it.
 */
static int p4_fail(struct p4_walk *w, int rc, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int p4_fail(struct p4_walk *w, int rc, const char *fmt, ...)
{
    va_list ap;

    if (!w->failed) {
        w->failed = true;
        if (w->err && w->err_len) {
            va_start(ap, fmt);
            /* Truncation is fine here; the message is for a human. */
            (void)vsnprintf(w->err, w->err_len, fmt, ap);
            va_end(ap);
        }
    }
    return rc;
}

/* ---- checked accessors -------------------------------------------------
 *
 * Every json-c convenience getter returns a plausible wrong answer rather
 * than an error when the type is not what was expected:
 * json_object_get_int64() coerces "5" to 5 and 5.9 to 5,
 * json_object_get_string() returns the serialisation of a non-string, and
 * json_object_get_boolean() is true for any non-empty string.  Nothing
 * below calls one without having checked the type first.
 */

static bool p4_get_obj(struct json_object *parent, const char *key,
                       struct json_object **out)
{
    struct json_object *v;

    if (!json_object_object_get_ex(parent, key, &v))
        return false;
    if (!json_object_is_type(v, json_type_object))
        return false;
    *out = v;
    return true;
}

static bool p4_get_array(struct json_object *parent, const char *key,
                         struct json_object **out, size_t cap, size_t *len)
{
    struct json_object *v;
    size_t n;

    if (!json_object_object_get_ex(parent, key, &v))
        return false;
    if (!json_object_is_type(v, json_type_array))
        return false;

    /* The length is checked against the cap before anything iterates, so a
     * huge array is refused rather than partially walked. */
    n = json_object_array_length(v);
    if (n > cap)
        return false;

    *out = v;
    *len = n;
    return true;
}

/*
 * Fetch a string, copying it rather than borrowing: every pointer json-c
 * hands back dies with the tree.
 *
 * The printable check is not cosmetic.  Names reach the log and the error
 * messages above, so a newline or an ANSI escape in one is log injection.
 */
static bool p4_get_str(struct json_object *parent, const char *key, char *out,
                       size_t out_cap)
{
    struct json_object *v;
    const char *s;
    int n;

    if (!json_object_object_get_ex(parent, key, &v))
        return false;
    /* Before get_string_len: on a null it returns NULL, and on an array it
     * returns the serialisation with a length of zero. */
    if (!json_object_is_type(v, json_type_string))
        return false;

    n = json_object_get_string_len(v);
    if (n <= 0 || (size_t)n >= out_cap)
        return false;

    s = json_object_get_string(v);
    if (!s)
        return false;
    /* An embedded NUL would make the rest of the name invisible to every
     * check below this one. */
    if (strlen(s) != (size_t)n)
        return false;

    for (int i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E)
            return false;
    }

    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    return true;
}

/*
 * Fetch an integer in [min, max].
 *
 * json_type_double is refused everywhere rather than rounded, which is also
 * what keeps NaN and Infinity out: json-c's tokener accepts all three as
 * doubles even under JSON_TOKENER_STRICT.
 */
static bool p4_get_u32(struct json_object *parent, const char *key,
                       uint32_t min, uint32_t max, uint32_t *out)
{
    struct json_object *v;
    int64_t raw;

    if (!json_object_object_get_ex(parent, key, &v))
        return false;
    if (!json_object_is_type(v, json_type_int))
        return false;

    errno = 0;
    raw = json_object_get_int64(v);
    if (errno != 0)
        return false;
    if (raw < 0 || (uint64_t)raw > (uint64_t)max || (uint64_t)raw < min)
        return false;

    *out = (uint32_t)raw;
    return true;
}

static bool p4_get_bool(struct json_object *parent, const char *key, bool *out)
{
    struct json_object *v;

    if (!json_object_object_get_ex(parent, key, &v))
        return false;
    if (!json_object_is_type(v, json_type_boolean))
        return false;
    *out = json_object_get_boolean(v) != 0;
    return true;
}

/*
 * True when @key is absent, or present as an empty array.  This is how a
 * construct that is understood but not implemented is detected: the program
 * may mention it as long as it does not use it.
 */
static bool p4_absent_or_empty(struct json_object *root, const char *key)
{
    struct json_object *v;

    if (!json_object_object_get_ex(root, key, &v))
        return true;
    if (json_object_is_type(v, json_type_null))
        return true;
    if (json_object_is_type(v, json_type_array))
        return json_object_array_length(v) == 0;
    if (json_object_is_type(v, json_type_object))
        return json_object_object_length(v) == 0;
    return false;
}

/*
 * Constructs this understands and refuses.  Each is named individually so a
 * rejection tells the user which P4 feature they used, rather than that the
 * file is unacceptable.
 */
static const char *const p4_unsupported_keys[] = {
    "header_stacks",       "header_union_types", "header_unions",
    "header_union_stacks", "parse_vsets",        "meter_arrays",
    "counter_arrays",      "register_arrays",    "learn_lists",
    "checksums",           "calculations",       "enums",
    "field_lists",
};

static int p4_check_unsupported(struct p4_walk *w, struct json_object *root)
{
    for (size_t i = 0;
         i < sizeof(p4_unsupported_keys) / sizeof(p4_unsupported_keys[0]);
         i++) {
        if (!p4_absent_or_empty(root, p4_unsupported_keys[i]))
            return p4_fail(w, -ENOSYS, "'%s' is not supported yet",
                           p4_unsupported_keys[i]);
    }
    return 0;
}

/*
 * __meta__.version is a two-element array.  The major number gates the
 * format: bmv2 raises it only on a backward-incompatible change, so a
 * different major is a file this cannot read at all, while any minor within
 * the major is readable.  The documented current version is 2.24 and the
 * format has carried major 2 since version 2.0.
 */
static int p4_check_meta(struct p4_walk *w, struct json_object *root)
{
    struct json_object *meta, *version, *elem;
    int64_t major, minor;

    if (!p4_get_obj(root, "__meta__", &meta))
        return p4_fail(w, -EINVAL, "missing or malformed '__meta__'");

    if (!json_object_object_get_ex(meta, "version", &version) ||
        !json_object_is_type(version, json_type_array) ||
        json_object_array_length(version) != 2)
        return p4_fail(w, -EINVAL,
                       "'__meta__.version' must be an array of two numbers");

    elem = json_object_array_get_idx(version, 0);
    if (!elem || !json_object_is_type(elem, json_type_int))
        return p4_fail(w, -EINVAL, "'__meta__.version[0]' is not an integer");
    major = json_object_get_int64(elem);

    elem = json_object_array_get_idx(version, 1);
    if (!elem || !json_object_is_type(elem, json_type_int))
        return p4_fail(w, -EINVAL, "'__meta__.version[1]' is not an integer");
    minor = json_object_get_int64(elem);

    if (major != 2)
        return p4_fail(w, -ENOSYS,
                       "JSON format major version %" PRId64 " is not supported",
                       major);
    if (minor < 0 || minor > 255)
        return p4_fail(w, -EINVAL,
                       "JSON format minor version %" PRId64 " is absurd",
                       minor);

    return 0;
}

/*
 * header_types: each is a name, an id, and a list of [name, width] (or
 * [name, width, signed]) tuples.  A width of "*" marks a variable-length
 * field, which this does not support; length_exp and max_length go with it
 * and are accepted only as null.
 */
static int p4_walk_header_types(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr;
    size_t n;

    if (!p4_get_array(root, "header_types", &arr, P4_MAX_HEADER_TYPES, &n))
        return p4_fail(w, -EINVAL,
                       "missing, malformed, or oversized 'header_types' "
                       "(at most %d)",
                       P4_MAX_HEADER_TYPES);

    for (size_t i = 0; i < n; i++) {
        struct json_object *ht = json_object_array_get_idx(arr, i);
        struct p4_header_type *dst = &w->prog->header_types[i];
        struct json_object *fields;
        size_t nf;
        uint32_t id;

        if (!ht || !json_object_is_type(ht, json_type_object))
            return p4_fail(w, -EINVAL, "header_types[%zu] is not an object", i);
        if (!p4_get_str(ht, "name", dst->name, sizeof(dst->name)))
            return p4_fail(w, -EINVAL, "header_types[%zu] has a bad name", i);
        if (!p4_get_u32(ht, "id", 0, P4_MAX_ID, &id))
            return p4_fail(w, -EINVAL, "header type '%s' has a bad id",
                           dst->name);
        dst->id = id;

        if (!p4_get_array(ht, "fields", &fields, P4_MAX_FIELDS, &nf))
            return p4_fail(w, -EINVAL,
                           "header type '%s' has bad or oversized fields "
                           "(at most %d)",
                           dst->name, P4_MAX_FIELDS);

        for (size_t f = 0; f < nf; f++) {
            struct json_object *tuple = json_object_array_get_idx(fields, f);
            struct json_object *fname, *fbits;
            struct p4_field *fd = &dst->fields[f];
            size_t tlen;
            const char *sn;
            int sl;
            int64_t bits;

            if (!tuple || !json_object_is_type(tuple, json_type_array))
                return p4_fail(w, -EINVAL, "'%s' field %zu is not a tuple",
                               dst->name, f);
            /* [name, bitwidth] or [name, bitwidth, signed]. */
            tlen = json_object_array_length(tuple);
            if (tlen != 2 && tlen != 3)
                return p4_fail(w, -EINVAL,
                               "'%s' field %zu has %zu elements, want 2 or 3",
                               dst->name, f, tlen);

            fname = json_object_array_get_idx(tuple, 0);
            if (!fname || !json_object_is_type(fname, json_type_string))
                return p4_fail(w, -EINVAL, "'%s' field %zu has no name",
                               dst->name, f);
            sl = json_object_get_string_len(fname);
            sn = json_object_get_string(fname);
            if (sl <= 0 || (size_t)sl >= sizeof(fd->name) || !sn ||
                strlen(sn) != (size_t)sl)
                return p4_fail(w, -EINVAL, "'%s' field %zu has a bad name",
                               dst->name, f);
            for (int c = 0; c < sl; c++) {
                if (sn[c] < 0x20 || sn[c] > 0x7E)
                    return p4_fail(w, -EINVAL,
                                   "'%s' field %zu has an unprintable name",
                                   dst->name, f);
            }
            memcpy(fd->name, sn, (size_t)sl);
            fd->name[sl] = '\0';

            fbits = json_object_array_get_idx(tuple, 1);
            /* A width of "*" is a variable-length field. Naming it is
             * worth more than "not an integer". */
            if (fbits && json_object_is_type(fbits, json_type_string))
                return p4_fail(w, -ENOSYS,
                               "'%s.%s' is variable length, which is not "
                               "supported",
                               dst->name, fd->name);
            if (!fbits || !json_object_is_type(fbits, json_type_int))
                return p4_fail(w, -EINVAL, "'%s.%s' has a bad width", dst->name,
                               fd->name);
            bits = json_object_get_int64(fbits);
            if (bits < 1 || bits > P4_MAX_FIELD_BITS)
                return p4_fail(w, -EINVAL,
                               "'%s.%s' is %" PRId64 " bits, want 1 to %d",
                               dst->name, fd->name, bits, P4_MAX_FIELD_BITS);

            /* Subtraction first: the sum must never be formed and then
             * checked, because it would wrap. */
            if ((uint32_t)bits > (uint32_t)UINT16_MAX - dst->total_bits)
                return p4_fail(w, -EINVAL, "header type '%s' is too wide",
                               dst->name);

            /*
             * A signed field is refused rather than loaded unsigned.
             * Nothing here carries signedness: values are zero-extended
             * and right-aligned by bits_get(), and value_cmp() orders
             * them with memcmp, so an int<8> holding 0xff would compare
             * greater than zero instead of less. Accepting the flag and
             * discarding it is the half-executed program this loader
             * exists to refuse.
             */
            if (tlen == 3) {
                json_object *fsign = json_object_array_get_idx(tuple, 2);

                if (fsign && json_object_is_type(fsign, json_type_boolean) &&
                    json_object_get_boolean(fsign))
                    return p4_fail(w, -ENOSYS,
                                   "'%s.%s' is signed, which is not "
                                   "supported",
                                   dst->name, fd->name);
            }

            fd->bits = (uint16_t)bits;
            fd->offset_bits = dst->total_bits;
            dst->total_bits = (uint16_t)(dst->total_bits + bits);
        }
        dst->nfields = (uint16_t)nf;

        /* Duplicate names or ids make every later reference ambiguous. */
        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->header_types[j].name, dst->name) == 0)
                return p4_fail(w, -EINVAL, "duplicate header type '%s'",
                               dst->name);
            if (w->prog->header_types[j].id == dst->id)
                return p4_fail(
                    w, -EINVAL, "header types '%s' and '%s' share id %u",
                    w->prog->header_types[j].name, dst->name, dst->id);
        }
    }

    w->prog->nheader_types = (uint16_t)n;
    return 0;
}

/* headers: instances, each naming a header_type that must already exist. */
static int p4_walk_headers(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr;
    size_t n;

    if (!p4_get_array(root, "headers", &arr, P4_MAX_HEADERS, &n))
        return p4_fail(w, -EINVAL,
                       "missing, malformed, or oversized 'headers' "
                       "(at most %d)",
                       P4_MAX_HEADERS);

    for (size_t i = 0; i < n; i++) {
        struct json_object *h = json_object_array_get_idx(arr, i);
        struct p4_header *dst = &w->prog->headers[i];
        char type_name[P4_MAX_NAME + 1];
        uint32_t id;

        if (!h || !json_object_is_type(h, json_type_object))
            return p4_fail(w, -EINVAL, "headers[%zu] is not an object", i);
        if (!p4_get_str(h, "name", dst->name, sizeof(dst->name)))
            return p4_fail(w, -EINVAL, "headers[%zu] has a bad name", i);
        if (!p4_get_u32(h, "id", 0, P4_MAX_ID, &id))
            return p4_fail(w, -EINVAL, "header '%s' has a bad id", dst->name);
        dst->id = id;
        if (!p4_get_str(h, "header_type", type_name, sizeof(type_name)))
            return p4_fail(w, -EINVAL, "header '%s' has a bad header_type",
                           dst->name);
        if (!p4_get_bool(h, "metadata", &dst->metadata))
            return p4_fail(w, -EINVAL, "header '%s' has a bad metadata flag",
                           dst->name);

        dst->header_type = P4_IDX_NONE;
        for (uint16_t t = 0; t < w->prog->nheader_types; t++) {
            if (strcmp(w->prog->header_types[t].name, type_name) == 0) {
                dst->header_type = t;
                break;
            }
        }
        if (dst->header_type == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "header '%s' names header type '%s', which does "
                           "not exist",
                           dst->name, type_name);

        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->headers[j].name, dst->name) == 0)
                return p4_fail(w, -EINVAL, "duplicate header '%s'", dst->name);
            if (w->prog->headers[j].id == dst->id)
                return p4_fail(w, -EINVAL, "headers '%s' and '%s' share id %u",
                               w->prog->headers[j].name, dst->name, dst->id);
        }
    }

    w->prog->nheaders = (uint16_t)n;
    return 0;
}

/* Resolve a header instance name to its index, or P4_IDX_NONE. */
static p4_idx_t p4_find_header(const struct p4_program *prog, const char *name)
{
    for (uint16_t i = 0; i < prog->nheaders; i++) {
        if (strcmp(prog->headers[i].name, name) == 0)
            return (p4_idx_t)i;
    }
    return P4_IDX_NONE;
}

/* Resolve ["header", "field"] to a reference, checking both halves. */
static bool p4_resolve_field(const struct p4_program *prog, const char *hname,
                             const char *fname, struct p4_field_ref *out)
{
    p4_idx_t h = p4_find_header(prog, hname);
    const struct p4_header_type *ht;

    if (h == P4_IDX_NONE)
        return false;
    ht = &prog->header_types[prog->headers[h].header_type];
    for (uint16_t f = 0; f < ht->nfields; f++) {
        if (strcmp(ht->fields[f].name, fname) == 0) {
            out->header = h;
            out->field = (p4_idx_t)f;
            return true;
        }
    }
    return false;
}

/*
 * The architecture's own metadata. v1model puts egress_spec and
 * ingress_port in a header instance called standard_metadata, and a
 * program addresses them like any other field, so they are found once here
 * rather than searched for on every packet.
 */
/*
 * Resolve one architecture field, requiring the width the architecture
 * gives it.
 *
 * p4_resolve_field() matches on name alone, so a program declaring
 * `bit<4> opcode` would resolve and then silently carry the low nibble of
 * every opcode.  A field the architecture defines has a width as much as
 * it has a name.
 */
static bool p4_resolve_arch_field(struct p4_walk *w, const char *hdr,
                                  const char *field, uint16_t bits,
                                  struct p4_field_ref *out)
{
    const struct p4_header_type *ht;

    out->header = P4_IDX_NONE;
    out->field = P4_IDX_NONE;

    if (!p4_resolve_field(w->prog, hdr, field, out))
        return false;

    ht = &w->prog->header_types[w->prog->headers[out->header].header_type];
    if (ht->fields[out->field].bits != bits) {
        /* Read the width before clearing the reference, not after: the
         * message needs the field this used to point at. */
        uint16_t found = ht->fields[out->field].bits;

        out->header = P4_IDX_NONE;
        out->field = P4_IDX_NONE;
        (void)p4_fail(w, -EINVAL, "'%s.%s' is %u bits; it must be %u", hdr,
                      field, found, bits);
        return false;
    }
    return true;
}

/*
 * The RDMA operation, for a program that wants to match on one.
 *
 * Entirely optional: a program with no rdma_metadata header is an
 * Ethernet program and the RDMA path never runs it.  The header must be
 * declared metadata, because it is architecture state rather than
 * anything that came off a wire -- a program that declared it otherwise
 * and listed it in the deparser would put that state on the wire.
 */
static int p4_resolve_rdma_metadata(struct p4_walk *w)
{
    static const char *const md = "rdma_metadata";
    static const char *const names[] = {"opcode", "qp_num", "length",
                                        "imm_data", "rkey"};
    static const uint16_t widths[] = {8, 24, 32, 32, 32};
    struct p4_field_ref *const refs[] = {
        &w->prog->rdma_opcode,   &w->prog->rdma_qp_num, &w->prog->rdma_length,
        &w->prog->rdma_imm_data, &w->prog->rdma_rkey,
    };
    /*
     * p4c qualifies a field of the user metadata struct with the struct's
     * name, so a program declaring rdma_metadata there really carries
     * "userMetadata.rdma_metadata".  Match the tail, since the prefix is
     * the compiler's business and not the architecture's.
     */
    p4_idx_t h = P4_IDX_NONE;
    const char *full = NULL;

    for (uint16_t i = 0; i < w->prog->nheaders; i++) {
        const char *n = w->prog->headers[i].name;
        size_t nl = strlen(n);
        size_t ml = strlen(md);

        if (strcmp(n, md) == 0 || (nl > ml && n[nl - ml - 1] == '.' &&
                                   strcmp(n + nl - ml, md) == 0)) {
            h = (p4_idx_t)i;
            full = n;
            break;
        }
    }
    if (h == P4_IDX_NONE)
        return 0;

    /*
     * The property that matters is that architecture state does not go on
     * a wire, so check the deparser rather than the metadata flag: p4c
     * marks a header-typed field of the metadata struct metadata:false,
     * because it has a validity bit, and a program written the ordinary
     * way would be refused for a reason that is not the real one.
     */
    for (uint16_t i = 0; i < w->prog->ndeparse_order; i++) {
        if (w->prog->deparse_order[i] == h)
            return p4_fail(w, -EINVAL,
                           "the deparser emits '%s'; it is architecture "
                           "state and must not go on a wire",
                           full);
    }

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        /* A field the program did not declare is simply one it never
         * reads; a field it declared at the wrong width is an error. */
        if (!p4_resolve_arch_field(w, full, names[i], widths[i], refs[i]) &&
            w->failed)
            return -EINVAL;
    }

    w->prog->has_rdma_metadata = true;
    return 0;
}

static int p4_resolve_std_metadata(struct p4_walk *w)
{
    static const char *const std = "standard_metadata";

    if (!p4_resolve_field(w->prog, std, "egress_spec", &w->prog->egress_spec))
        return p4_fail(w, -EINVAL,
                       "no '%s.egress_spec'; a program without it cannot say "
                       "where a packet goes",
                       std);
    /* ingress_port is optional: a program may never read it. */
    if (!p4_resolve_field(w->prog, std, "ingress_port",
                          &w->prog->ingress_port)) {
        w->prog->ingress_port.header = P4_IDX_NONE;
        w->prog->ingress_port.field = P4_IDX_NONE;
    }
    return 0;
}

/* Read a two-element ["header", "field"] array into names. */
static bool p4_pair_strings(struct json_object *arr, char *a, size_t acap,
                            char *b, size_t bcap)
{
    struct json_object *e;
    const char *s;
    int n;

    if (!arr || !json_object_is_type(arr, json_type_array) ||
        json_object_array_length(arr) != 2)
        return false;

    for (int i = 0; i < 2; i++) {
        char *dst = (i == 0) ? a : b;
        size_t cap = (i == 0) ? acap : bcap;

        e = json_object_array_get_idx(arr, (size_t)i);
        if (!e || !json_object_is_type(e, json_type_string))
            return false;
        n = json_object_get_string_len(e);
        s = json_object_get_string(e);
        if (n <= 0 || (size_t)n >= cap || !s || strlen(s) != (size_t)n)
            return false;
        memcpy(dst, s, (size_t)n);
        dst[n] = '\0';
    }
    return true;
}

/*
 * One parse state's operations. Only extract is supported, and only with a
 * "regular" parameter: extract_VL takes a computed length, which needs the
 * expression machinery this does not have.
 */
static int p4_walk_parser_ops(struct p4_walk *w, struct json_object *st,
                              struct p4_parse_state *dst)
{
    struct json_object *ops;
    size_t n;

    if (!p4_get_array(st, "parser_ops", &ops, P4_MAX_PARSER_OPS, &n))
        return p4_fail(w, -EINVAL,
                       "state '%s' has bad or oversized parser_ops "
                       "(at most %d)",
                       dst->name, P4_MAX_PARSER_OPS);

    for (size_t i = 0; i < n; i++) {
        struct json_object *op = json_object_array_get_idx(ops, i);
        struct json_object *params, *p0;
        char opname[P4_MAX_NAME + 1];
        char ptype[P4_MAX_NAME + 1];
        char pval[P4_MAX_NAME + 1];
        size_t np;

        if (!op || !json_object_is_type(op, json_type_object))
            return p4_fail(w, -EINVAL, "state '%s' op %zu is not an object",
                           dst->name, i);
        if (!p4_get_str(op, "op", opname, sizeof(opname)))
            return p4_fail(w, -EINVAL, "state '%s' op %zu has no name",
                           dst->name, i);
        if (strcmp(opname, "extract") != 0)
            return p4_fail(w, -ENOSYS,
                           "parser operation '%s' in state '%s' is not "
                           "supported",
                           opname, dst->name);

        if (!p4_get_array(op, "parameters", &params, 2, &np) || np != 1)
            return p4_fail(w, -EINVAL,
                           "extract in state '%s' wants exactly one "
                           "parameter",
                           dst->name);
        p0 = json_object_array_get_idx(params, 0);
        if (!p0 || !json_object_is_type(p0, json_type_object) ||
            !p4_get_str(p0, "type", ptype, sizeof(ptype)) ||
            !p4_get_str(p0, "value", pval, sizeof(pval)))
            return p4_fail(w, -EINVAL,
                           "extract in state '%s' has a bad parameter",
                           dst->name);
        if (strcmp(ptype, "regular") != 0)
            return p4_fail(w, -ENOSYS,
                           "extract of a '%s' parameter in state '%s' is "
                           "not supported",
                           ptype, dst->name);

        dst->ops[i].kind = P4_POP_EXTRACT;
        dst->ops[i].header = p4_find_header(w->prog, pval);
        if (dst->ops[i].header == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "state '%s' extracts '%s', which is not a header",
                           dst->name, pval);
    }
    dst->nops = (uint16_t)n;
    return 0;
}

/* The select key: a list of field references whose widths give its width. */
static int p4_walk_transition_key(struct p4_walk *w, struct json_object *st,
                                  struct p4_parse_state *dst)
{
    struct json_object *keys;
    size_t n;
    uint32_t bits = 0;

    if (!p4_get_array(st, "transition_key", &keys, P4_MAX_TRANSITION_KEY, &n))
        return p4_fail(w, -EINVAL,
                       "state '%s' has a bad or oversized transition_key "
                       "(at most %d)",
                       dst->name, P4_MAX_TRANSITION_KEY);

    for (size_t i = 0; i < n; i++) {
        struct json_object *k = json_object_array_get_idx(keys, i);
        struct json_object *val;
        char ktype[P4_MAX_NAME + 1];
        char hname[P4_MAX_NAME + 1], fname[P4_MAX_NAME + 1];
        const struct p4_header_type *ht;

        if (!k || !json_object_is_type(k, json_type_object) ||
            !p4_get_str(k, "type", ktype, sizeof(ktype)))
            return p4_fail(w, -EINVAL, "state '%s' key %zu is malformed",
                           dst->name, i);
        if (strcmp(ktype, "field") != 0)
            return p4_fail(w, -ENOSYS,
                           "select on a '%s' in state '%s' is not supported",
                           ktype, dst->name);

        if (!json_object_object_get_ex(k, "value", &val) ||
            !p4_pair_strings(val, hname, sizeof(hname), fname, sizeof(fname)))
            return p4_fail(w, -EINVAL,
                           "state '%s' key %zu is not [header, field]",
                           dst->name, i);
        if (!p4_resolve_field(w->prog, hname, fname, &dst->key[i]))
            return p4_fail(w, -EINVAL,
                           "state '%s' selects on '%s.%s', which does not "
                           "exist",
                           dst->name, hname, fname);

        ht = &w->prog->header_types[w->prog->headers[dst->key[i].header]
                                        .header_type];
        bits += ht->fields[dst->key[i].field].bits;
    }

    /* The arms are compared against this many bytes, so a key wider than
     * the buffer they are stored in cannot be represented. */
    if (bits > (uint32_t)P4_MAX_KEY_BYTES * 8)
        return p4_fail(w, -EINVAL, "state '%s' selects on %u bits, at most %d",
                       dst->name, bits, P4_MAX_KEY_BYTES * 8);

    dst->nkey = (uint16_t)n;
    dst->key_bytes = (uint16_t)((bits + 7) / 8);
    return 0;
}

/*
 * The arms. Two encodings of a default arm are in the wild and both are
 * accepted: {"value": "default"} with no type, which bmv2's own
 * loopback.json emits, and {"type": "default", "value": null}, which p4c
 * emits for a program with a select.
 */
static int p4_walk_transitions(struct p4_walk *w, struct json_object *st,
                               struct p4_parse_state *dst)
{
    struct json_object *arr;
    size_t n;

    if (!p4_get_array(st, "transitions", &arr, P4_MAX_TRANSITIONS, &n))
        return p4_fail(w, -EINVAL,
                       "state '%s' has bad or oversized transitions "
                       "(at most %d)",
                       dst->name, P4_MAX_TRANSITIONS);

    for (size_t i = 0; i < n; i++) {
        struct json_object *t = json_object_array_get_idx(arr, i);
        struct p4_transition *td = &dst->transitions[i];
        struct json_object *val, *mask, *next;
        char type[P4_MAX_NAME + 1];
        bool has_type;

        if (!t || !json_object_is_type(t, json_type_object))
            return p4_fail(w, -EINVAL,
                           "state '%s' transition %zu is not an object",
                           dst->name, i);

        has_type = p4_get_str(t, "type", type, sizeof(type));
        if (!json_object_object_get_ex(t, "value", &val))
            return p4_fail(w, -EINVAL, "state '%s' transition %zu has no value",
                           dst->name, i);

        if (has_type && strcmp(type, "default") == 0) {
            td->is_default = true;
        } else if (!has_type && json_object_is_type(val, json_type_string) &&
                   strcmp(json_object_get_string(val), "default") == 0) {
            td->is_default = true;
        } else if (has_type && strcmp(type, "hexstr") != 0) {
            return p4_fail(w, -ENOSYS,
                           "transition of type '%s' in state '%s' is not "
                           "supported",
                           type, dst->name);
        } else {
            size_t vlen = 0;
            bool neg = false;
            const char *vs;

            if (!json_object_is_type(val, json_type_string))
                return p4_fail(w, -EINVAL,
                               "state '%s' transition %zu has a non-string "
                               "value",
                               dst->name, i);
            vs = json_object_get_string(val);
            if (!vs ||
                !ernic_parse_hexstr(vs, td->value, sizeof(td->value), &vlen,
                                    &neg) ||
                neg)
                return p4_fail(w, -EINVAL,
                               "state '%s' transition %zu has a bad value",
                               dst->name, i);
            if (vlen > dst->key_bytes)
                return p4_fail(w, -EINVAL,
                               "state '%s' transition %zu is wider than its "
                               "select key",
                               dst->name, i);

            if (json_object_object_get_ex(t, "mask", &mask) &&
                !json_object_is_type(mask, json_type_null)) {
                const char *ms;

                if (!json_object_is_type(mask, json_type_string))
                    return p4_fail(w, -EINVAL,
                                   "state '%s' transition %zu has a bad mask",
                                   dst->name, i);
                ms = json_object_get_string(mask);
                if (!ms ||
                    !ernic_parse_hexstr(ms, td->mask, sizeof(td->mask), &vlen,
                                        &neg) ||
                    neg || vlen > dst->key_bytes)
                    return p4_fail(w, -EINVAL,
                                   "state '%s' transition %zu has a bad mask",
                                   dst->name, i);
                td->has_mask = true;
            }
        }

        /* next_state is resolved in a second pass: a state may transition
         * to one that has not been read yet. */
        if (!json_object_object_get_ex(t, "next_state", &next))
            return p4_fail(w, -EINVAL,
                           "state '%s' transition %zu has no next_state",
                           dst->name, i);
        td->next_state = P4_IDX_NONE;
    }

    dst->ntransitions = (uint16_t)n;
    return 0;
}

/*
 * parsers: exactly one, whose states are read in two passes so a forward
 * reference resolves. Pass one fills each state and leaves every
 * next_state unset; pass two resolves them against the complete set of
 * names, so nothing in pass one can fail on a name it has not seen.
 */
static int p4_walk_parser(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr, *parser, *states;
    char init[P4_MAX_NAME + 1];
    size_t n, ns;

    if (!p4_get_array(root, "parsers", &arr, 2, &n))
        return p4_fail(w, -EINVAL, "missing or malformed 'parsers'");
    if (n != 1)
        return p4_fail(w, -ENOSYS, "%zu parsers; exactly one is supported", n);

    parser = json_object_array_get_idx(arr, 0);
    if (!parser || !json_object_is_type(parser, json_type_object))
        return p4_fail(w, -EINVAL, "parsers[0] is not an object");
    if (!p4_get_str(parser, "init_state", init, sizeof(init)))
        return p4_fail(w, -EINVAL, "the parser has no init_state");

    if (!p4_get_array(parser, "parse_states", &states, P4_MAX_PARSE_STATES,
                      &ns))
        return p4_fail(w, -EINVAL, "bad or oversized parse_states (at most %d)",
                       P4_MAX_PARSE_STATES);
    if (ns == 0)
        return p4_fail(w, -EINVAL, "the parser has no states");

    for (size_t i = 0; i < ns; i++) {
        struct json_object *st = json_object_array_get_idx(states, i);
        struct p4_parse_state *dst = &w->prog->parse_states[i];
        int rc;

        if (!st || !json_object_is_type(st, json_type_object))
            return p4_fail(w, -EINVAL, "parse_states[%zu] is not an object", i);
        if (!p4_get_str(st, "name", dst->name, sizeof(dst->name)))
            return p4_fail(w, -EINVAL, "parse_states[%zu] has a bad name", i);

        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->parse_states[j].name, dst->name) == 0)
                return p4_fail(w, -EINVAL, "duplicate parse state '%s'",
                               dst->name);
        }

        rc = p4_walk_parser_ops(w, st, dst);
        if (rc)
            return rc;
        rc = p4_walk_transition_key(w, st, dst);
        if (rc)
            return rc;
        rc = p4_walk_transitions(w, st, dst);
        if (rc)
            return rc;
    }
    w->prog->nparse_states = (uint16_t)ns;

    /* Pass two: resolve next_state now every name is known. */
    for (size_t i = 0; i < ns; i++) {
        struct json_object *st = json_object_array_get_idx(states, i);
        struct json_object *tarr;
        struct p4_parse_state *dst = &w->prog->parse_states[i];
        size_t nt;

        if (!p4_get_array(st, "transitions", &tarr, P4_MAX_TRANSITIONS, &nt))
            return p4_fail(w, -EINVAL, "state '%s' changed shape", dst->name);

        for (size_t t = 0; t < nt; t++) {
            struct json_object *tr = json_object_array_get_idx(tarr, t);
            struct json_object *next;
            const char *name;
            p4_idx_t idx;

            if (!tr || !json_object_object_get_ex(tr, "next_state", &next))
                return p4_fail(w, -EINVAL, "state '%s' changed shape",
                               dst->name);
            /* null means the parser accepts on this arm. */
            if (json_object_is_type(next, json_type_null))
                continue;
            if (!json_object_is_type(next, json_type_string))
                return p4_fail(w, -EINVAL,
                               "state '%s' arm %zu has a bad next_state",
                               dst->name, t);
            name = json_object_get_string(next);
            idx = P4_IDX_NONE;
            for (uint16_t j = 0; j < w->prog->nparse_states; j++) {
                if (strcmp(w->prog->parse_states[j].name, name) == 0) {
                    idx = (p4_idx_t)j;
                    break;
                }
            }
            if (idx == P4_IDX_NONE)
                return p4_fail(w, -EINVAL,
                               "state '%s' transitions to '%s', which does "
                               "not exist",
                               dst->name, name);
            dst->transitions[t].next_state = idx;
        }
    }

    w->prog->init_state = P4_IDX_NONE;
    for (uint16_t j = 0; j < w->prog->nparse_states; j++) {
        if (strcmp(w->prog->parse_states[j].name, init) == 0) {
            w->prog->init_state = j;
            break;
        }
    }
    if (w->prog->init_state == P4_IDX_NONE)
        return p4_fail(w, -EINVAL,
                       "init_state '%s' is not one of the parse states", init);

    return 0;
}

/* deparsers: exactly one, naming the header instances it emits in order. */
static int p4_walk_deparser(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr, *dep, *order;
    size_t n, no;

    if (!p4_get_array(root, "deparsers", &arr, 2, &n))
        return p4_fail(w, -EINVAL, "missing or malformed 'deparsers'");
    if (n != 1)
        return p4_fail(w, -ENOSYS, "%zu deparsers; exactly one is supported",
                       n);

    dep = json_object_array_get_idx(arr, 0);
    if (!dep || !json_object_is_type(dep, json_type_object))
        return p4_fail(w, -EINVAL, "deparsers[0] is not an object");

    if (!p4_get_array(dep, "order", &order, P4_MAX_DEPARSE_ORDER, &no))
        return p4_fail(w, -EINVAL,
                       "bad or oversized deparser order (at most %d)",
                       P4_MAX_DEPARSE_ORDER);

    for (size_t i = 0; i < no; i++) {
        struct json_object *e = json_object_array_get_idx(order, i);
        const char *name;
        p4_idx_t h;
        int sl;

        if (!e || !json_object_is_type(e, json_type_string))
            return p4_fail(w, -EINVAL, "deparser order %zu is not a name", i);
        sl = json_object_get_string_len(e);
        name = json_object_get_string(e);
        if (sl <= 0 || sl > P4_MAX_NAME || !name || strlen(name) != (size_t)sl)
            return p4_fail(w, -EINVAL, "deparser order %zu has a bad name", i);

        h = p4_find_header(w->prog, name);
        if (h == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "the deparser emits '%s', which is not a header",
                           name);
        /* Emitting one header twice would duplicate it on the wire. */
        for (size_t j = 0; j < i; j++) {
            if (w->prog->deparse_order[j] == h)
                return p4_fail(w, -EINVAL,
                               "the deparser emits '%s' more than once", name);
        }
        w->prog->deparse_order[i] = h;
    }
    w->prog->ndeparse_order = (uint16_t)no;
    return 0;
}

/*
 * The primitives an action may use, and how many parameters each takes.
 * bmv2 names several of these two ways, so both spellings map to one kind.
 *
 * drop takes no parameter in older output and one -- standard_metadata --
 * in newer, where p4c emits mark_to_drop(standard_metadata). Both arities
 * are accepted and the parameter ignored, because the drop is expressed by
 * the primitive, not by what it is handed.
 */
static const struct {
    const char *name;
    uint8_t kind;
    uint8_t min_params;
    uint8_t max_params;
} p4_primitives[] = {
    {"assign", P4_PRIM_ASSIGN, 2, 2},
    {"modify_field", P4_PRIM_ASSIGN, 2, 2},
    {"add", P4_PRIM_ADD, 3, 3},
    {"add_to_field", P4_PRIM_ADD, 2, 2},
    {"subtract", P4_PRIM_SUB, 3, 3},
    {"subtract_from_field", P4_PRIM_SUB, 2, 2},
    {"drop", P4_PRIM_DROP, 0, 1},
    {"mark_to_drop", P4_PRIM_DROP, 0, 1},
    {"no_op", P4_PRIM_NOP, 0, 0},
    /* The call form p4c emits for an extern: _<Type>_<method>, with the
     * instance as the first parameter. */
    {"_Fp8_map", P4_PRIM_EXTERN, 2, 2},
};

/* One operand of a primitive. */
static int p4_walk_operand(struct p4_walk *w, struct json_object *p,
                           const struct p4_action *act, struct p4_operand *dst,
                           const char *where)
{
    struct json_object *val;
    char type[P4_MAX_NAME + 1];

    if (!p || !json_object_is_type(p, json_type_object) ||
        !p4_get_str(p, "type", type, sizeof(type)))
        return p4_fail(w, -EINVAL, "%s has a malformed parameter", where);
    if (!json_object_object_get_ex(p, "value", &val))
        return p4_fail(w, -EINVAL, "%s has a parameter with no value", where);

    if (strcmp(type, "field") == 0) {
        char hname[P4_MAX_NAME + 1], fname[P4_MAX_NAME + 1];

        if (!p4_pair_strings(val, hname, sizeof(hname), fname, sizeof(fname)))
            return p4_fail(w, -EINVAL, "%s names a field as %s", where,
                           "something other than [header, field]");
        if (!p4_resolve_field(w->prog, hname, fname, &dst->field))
            return p4_fail(w, -EINVAL, "%s uses '%s.%s', which does not exist",
                           where, hname, fname);
        dst->kind = P4_OPD_FIELD;
        return 0;
    }

    if (strcmp(type, "hexstr") == 0) {
        size_t n = 0;
        bool neg = false;
        const char *vs;

        if (!json_object_is_type(val, json_type_string))
            return p4_fail(w, -EINVAL, "%s has a non-string literal", where);
        vs = json_object_get_string(val);
        if (!vs ||
            !ernic_parse_hexstr(vs, dst->imm, sizeof(dst->imm), &n, &neg))
            return p4_fail(w, -EINVAL, "%s has a malformed literal", where);
        dst->kind = P4_OPD_HEXSTR;
        dst->imm_len = (uint16_t)n;
        dst->imm_negative = neg;
        return 0;
    }

    if (strcmp(type, "extern") == 0) {
        const char *iname;

        if (!json_object_is_type(val, json_type_string))
            return p4_fail(w, -EINVAL, "%s names an extern badly", where);
        iname = json_object_get_string(val);
        dst->inst = P4_IDX_NONE;
        for (uint16_t i = 0; iname && i < w->prog->nexterns; i++) {
            if (strcmp(w->prog->externs[i].name, iname) == 0) {
                dst->inst = (p4_idx_t)i;
                break;
            }
        }
        if (dst->inst == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "%s calls on '%s', which was never instantiated",
                           where, iname ? iname : "");
        dst->kind = P4_OPD_EXTERN;
        return 0;
    }

    if (strcmp(type, "runtime_data") == 0) {
        int64_t idx;

        if (!json_object_is_type(val, json_type_int))
            return p4_fail(w, -EINVAL,
                           "%s names a runtime argument by something other "
                           "than an index",
                           where);
        idx = json_object_get_int64(val);
        /* An index at or past the count would read an argument the table
         * entry never supplies. */
        if (idx < 0 || idx >= act->nruntime)
            return p4_fail(w, -EINVAL,
                           "%s uses runtime argument %" PRId64 " of %u", where,
                           idx, act->nruntime);
        dst->kind = P4_OPD_RUNTIME;
        dst->runtime = (p4_idx_t)idx;
        return 0;
    }

    /* register, expression, extern, header, calculation, ... */
    return p4_fail(w, -ENOSYS,
                   "%s uses a '%s' parameter, which is not "
                   "supported",
                   where, type);
}

/*
 * extern_instances: the objects a program instantiated.
 *
 * The type is allow-listed against what this target actually provides.
 * p4c will compile a call to any extern it is told about when given
 * --emit-externs, so the loader is the only thing that decides which ones
 * exist here; accepting an unknown type would load a program whose calls
 * go nowhere.
 */
static int p4_walk_externs(struct p4_walk *w, struct json_object *root)
{
    static const struct {
        const char *name;
        uint8_t type;
    } types[] = {
        {"Fp8", P4_EXTERN_FP8},
    };
    struct json_object *arr;
    size_t n;

    /* Absent is fine: most programs instantiate nothing. */
    if (!json_object_object_get_ex(root, "extern_instances", &arr))
        return 0;
    if (!p4_get_array(root, "extern_instances", &arr, P4_MAX_EXTERNS, &n))
        return p4_fail(w, -EINVAL,
                       "malformed or oversized 'extern_instances' (at most "
                       "%d)",
                       P4_MAX_EXTERNS);

    for (size_t i = 0; i < n; i++) {
        struct json_object *e = json_object_array_get_idx(arr, i);
        struct p4_extern_inst *dst = &w->prog->externs[i];
        struct json_object *attrs;
        char type[P4_MAX_NAME + 1];
        size_t nattr;
        uint32_t id;
        bool known = false;

        if (!e || !json_object_is_type(e, json_type_object))
            return p4_fail(w, -EINVAL,
                           "extern_instances[%zu] is not an "
                           "object",
                           i);
        if (!p4_get_str(e, "name", dst->name, sizeof(dst->name)))
            return p4_fail(w, -EINVAL, "extern_instances[%zu] has a bad name",
                           i);
        if (!p4_get_u32(e, "id", 0, P4_MAX_ID, &id))
            return p4_fail(w, -EINVAL, "extern '%s' has a bad id", dst->name);
        dst->id = id;

        if (!p4_get_str(e, "type", type, sizeof(type)))
            return p4_fail(w, -EINVAL, "extern '%s' has no type", dst->name);
        for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
            if (strcmp(types[t].name, type) == 0) {
                dst->type = types[t].type;
                known = true;
                break;
            }
        }
        if (!known)
            return p4_fail(w, -ENOSYS,
                           "extern '%s' is of type '%s', which this target "
                           "does not provide",
                           dst->name, type);

        /* No extern here takes an attribute, and one that arrived would
         * be a different object from the one being asked for. */
        if (p4_get_array(e, "attribute_values", &attrs, P4_MAX_EXTERNS,
                         &nattr) &&
            nattr != 0)
            return p4_fail(w, -ENOSYS,
                           "extern '%s' has attributes, which are not "
                           "supported",
                           dst->name);

        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->externs[j].name, dst->name) == 0)
                return p4_fail(w, -EINVAL, "duplicate extern '%s'", dst->name);
            if (w->prog->externs[j].id == dst->id)
                return p4_fail(w, -EINVAL, "externs '%s' and '%s' share id %u",
                               w->prog->externs[j].name, dst->name, dst->id);
        }
    }

    w->prog->nexterns = (uint16_t)n;
    return 0;
}

/* actions: runtime arguments first, since a primitive may name one. */
static int p4_walk_actions(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr;
    size_t n;

    if (!p4_get_array(root, "actions", &arr, P4_MAX_ACTIONS, &n))
        return p4_fail(w, -EINVAL,
                       "missing, malformed, or oversized 'actions' "
                       "(at most %d)",
                       P4_MAX_ACTIONS);

    for (size_t i = 0; i < n; i++) {
        struct json_object *a = json_object_array_get_idx(arr, i);
        struct p4_action *dst = &w->prog->actions[i];
        struct json_object *rt, *prims;
        size_t nrt, np;
        uint32_t id;

        if (!a || !json_object_is_type(a, json_type_object))
            return p4_fail(w, -EINVAL, "actions[%zu] is not an object", i);
        if (!p4_get_str(a, "name", dst->name, sizeof(dst->name)))
            return p4_fail(w, -EINVAL, "actions[%zu] has a bad name", i);
        if (!p4_get_u32(a, "id", 0, P4_MAX_ID, &id))
            return p4_fail(w, -EINVAL, "action '%s' has a bad id", dst->name);
        dst->id = id;

        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->actions[j].name, dst->name) == 0)
                return p4_fail(w, -EINVAL, "duplicate action '%s'", dst->name);
            if (w->prog->actions[j].id == dst->id)
                return p4_fail(w, -EINVAL, "actions '%s' and '%s' share id %u",
                               w->prog->actions[j].name, dst->name, dst->id);
        }

        if (!p4_get_array(a, "runtime_data", &rt, P4_MAX_RUNTIME_ARGS, &nrt))
            return p4_fail(w, -EINVAL,
                           "action '%s' has bad or oversized runtime_data "
                           "(at most %d)",
                           dst->name, P4_MAX_RUNTIME_ARGS);
        for (size_t r = 0; r < nrt; r++) {
            struct json_object *e = json_object_array_get_idx(rt, r);
            uint32_t bits;

            if (!e || !json_object_is_type(e, json_type_object) ||
                !p4_get_u32(e, "bitwidth", 1, P4_MAX_FIELD_BITS, &bits))
                return p4_fail(w, -EINVAL,
                               "action '%s' argument %zu has a bad width",
                               dst->name, r);
            dst->runtime_bits[r] = (uint16_t)bits;
        }
        dst->nruntime = (uint16_t)nrt;

        if (!p4_get_array(a, "primitives", &prims, P4_MAX_PRIMITIVES, &np))
            return p4_fail(w, -EINVAL,
                           "action '%s' has bad or oversized primitives "
                           "(at most %d)",
                           dst->name, P4_MAX_PRIMITIVES);

        for (size_t k = 0; k < np; k++) {
            struct json_object *pr = json_object_array_get_idx(prims, k);
            struct p4_primitive *pd = &dst->primitives[k];
            struct json_object *params;
            char opname[P4_MAX_NAME + 1];
            char where[2 * P4_MAX_NAME + 64];
            size_t nparams;
            size_t which;
            bool known = false;

            if (!pr || !json_object_is_type(pr, json_type_object))
                return p4_fail(w, -EINVAL,
                               "action '%s' primitive %zu is not an object",
                               dst->name, k);
            if (!p4_get_str(pr, "op", opname, sizeof(opname)))
                return p4_fail(w, -EINVAL,
                               "action '%s' primitive %zu has no op", dst->name,
                               k);

            for (which = 0;
                 which < sizeof(p4_primitives) / sizeof(p4_primitives[0]);
                 which++) {
                if (strcmp(p4_primitives[which].name, opname) == 0) {
                    known = true;
                    break;
                }
            }
            if (!known)
                return p4_fail(w, -ENOSYS,
                               "primitive '%s' in action '%s' is not "
                               "supported",
                               opname, dst->name);

            if (!p4_get_array(pr, "parameters", &params, P4_MAX_PRIM_PARAMS,
                              &nparams))
                return p4_fail(w, -EINVAL,
                               "'%s' in action '%s' has bad parameters", opname,
                               dst->name);
            if (nparams < p4_primitives[which].min_params ||
                nparams > p4_primitives[which].max_params)
                return p4_fail(w, -EINVAL,
                               "'%s' in action '%s' takes %zu parameters, "
                               "want %u to %u",
                               opname, dst->name, nparams,
                               p4_primitives[which].min_params,
                               p4_primitives[which].max_params);

            pd->kind = p4_primitives[which].kind;
            /* One method today, so the name that matched is the method. */
            pd->method = P4_EXTERN_FP8_MAP;

            /* drop's parameter, where there is one, says nothing the
             * primitive does not already say. */
            if (pd->kind == P4_PRIM_DROP || pd->kind == P4_PRIM_NOP) {
                pd->nparams = 0;
                continue;
            }

            (void)snprintf(where, sizeof(where), "'%s' in action '%s'", opname,
                           dst->name);
            for (size_t q = 0; q < nparams; q++) {
                int rc =
                    p4_walk_operand(w, json_object_array_get_idx(params, q),
                                    dst, &pd->params[q], where);
                if (rc)
                    return rc;
            }
            if (pd->kind == P4_PRIM_EXTERN) {
                /* A call's first parameter is the instance it is made on,
                 * and nothing else may be. */
                if (pd->params[0].kind != P4_OPD_EXTERN)
                    return p4_fail(w, -EINVAL,
                                   "%s does not name the extern it calls on",
                                   where);
                for (size_t q = 1; q < nparams; q++) {
                    if (pd->params[q].kind == P4_OPD_EXTERN)
                        return p4_fail(w, -EINVAL,
                                       "%s passes an extern as an argument",
                                       where);
                }
            } else {
                /* The destination of an assignment has to be somewhere a
                 * value can be put, and an extern is never one. */
                if (pd->params[0].kind != P4_OPD_FIELD)
                    return p4_fail(w, -EINVAL,
                                   "%s writes to something that is not a "
                                   "field",
                                   where);
            }
            pd->nparams = (uint8_t)nparams;
        }
        dst->nprimitives = (uint16_t)np;
    }

    w->prog->nactions = (uint16_t)n;
    return 0;
}

static const struct {
    const char *name;
    uint8_t op;
    bool unary;
} p4_expr_ops[] = {
    {"valid", P4_EXPR_VALID, true}, {"not", P4_EXPR_NOT, true},
    {"==", P4_EXPR_EQ, false},      {"!=", P4_EXPR_NE, false},
    {"<", P4_EXPR_LT, false},       {"<=", P4_EXPR_LE, false},
    {">", P4_EXPR_GT, false},       {">=", P4_EXPR_GE, false},
    {"and", P4_EXPR_AND, false},    {"or", P4_EXPR_OR, false},
};

static int p4_walk_expr(struct p4_walk *w, struct json_object *tv,
                        struct p4_conditional *cond, p4_idx_t *slot,
                        unsigned depth, const char *where);

/*
 * One side of an operator. A nested expression is allocated a node of its
 * own, which always lands after this one, keeping a child's index greater
 * than its parent's.
 */
static int p4_walk_expr_operand(struct p4_walk *w, struct json_object *tv,
                                struct p4_conditional *cond,
                                struct p4_expr_operand *dst, unsigned depth,
                                const char *where)
{
    struct json_object *val;
    char type[P4_MAX_NAME + 1];

    /* null is how a unary operator writes the side it does not use. */
    if (!tv || json_object_is_type(tv, json_type_null)) {
        dst->kind = P4_EO_NONE;
        return 0;
    }
    if (!json_object_is_type(tv, json_type_object) ||
        !p4_get_str(tv, "type", type, sizeof(type)))
        return p4_fail(w, -EINVAL, "%s has a malformed operand", where);
    if (!json_object_object_get_ex(tv, "value", &val))
        return p4_fail(w, -EINVAL, "%s has an operand with no value", where);

    if (strcmp(type, "expression") == 0) {
        dst->kind = P4_EO_EXPR;
        return p4_walk_expr(w, tv, cond, &dst->expr, depth + 1, where);
    }
    if (strcmp(type, "field") == 0) {
        char h[P4_MAX_NAME + 1], f[P4_MAX_NAME + 1];

        if (!p4_pair_strings(val, h, sizeof(h), f, sizeof(f)))
            return p4_fail(w, -EINVAL, "%s names a field badly", where);
        if (!p4_resolve_field(w->prog, h, f, &dst->field))
            return p4_fail(w, -EINVAL, "%s uses '%s.%s', which does not exist",
                           where, h, f);
        dst->kind = P4_EO_FIELD;
        return 0;
    }
    if (strcmp(type, "header") == 0) {
        const char *hn;

        if (!json_object_is_type(val, json_type_string))
            return p4_fail(w, -EINVAL, "%s names a header badly", where);
        hn = json_object_get_string(val);
        dst->header = hn ? p4_find_header(w->prog, hn) : P4_IDX_NONE;
        if (dst->header == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "%s uses header '%s', which does not "
                           "exist",
                           where, hn ? hn : "");
        dst->kind = P4_EO_HEADER;
        return 0;
    }
    if (strcmp(type, "hexstr") == 0) {
        size_t n = 0;
        bool neg = false;
        const char *vs;

        if (!json_object_is_type(val, json_type_string))
            return p4_fail(w, -EINVAL, "%s has a non-string literal", where);
        vs = json_object_get_string(val);
        if (!vs ||
            !ernic_parse_hexstr(vs, dst->imm, sizeof(dst->imm), &n, &neg) ||
            neg)
            return p4_fail(w, -EINVAL, "%s has a bad literal", where);
        dst->kind = P4_EO_HEXSTR;
        dst->imm_len = (uint16_t)n;
        return 0;
    }
    if (strcmp(type, "bool") == 0) {
        if (!json_object_is_type(val, json_type_boolean))
            return p4_fail(w, -EINVAL, "%s has a bad boolean", where);
        dst->kind = P4_EO_BOOL;
        dst->bval = json_object_get_boolean(val) != 0;
        return 0;
    }

    return p4_fail(w, -ENOSYS,
                   "%s uses a '%s' operand, which is not "
                   "supported",
                   where, type);
}

/*
 * An expression node. Depth is carried explicitly and bounded here: the
 * tokener's own depth limit bounds the document, but nothing bounds how
 * deep this walk recurses except this.
 */
static int p4_walk_expr(struct p4_walk *w, struct json_object *tv,
                        struct p4_conditional *cond, p4_idx_t *slot,
                        unsigned depth, const char *where)
{
    struct json_object *val, *left, *right, *opobj;
    struct p4_expr_node *node;
    const char *opname;
    p4_idx_t self;
    size_t which;
    bool known = false;
    int rc;

    if (depth > P4_MAX_EXPR_DEPTH)
        return p4_fail(w, -EINVAL, "%s nests more than %d deep", where,
                       P4_MAX_EXPR_DEPTH);
    if (cond->nexpr >= P4_MAX_EXPR_NODES)
        return p4_fail(w, -EINVAL, "%s has more than %d operators", where,
                       P4_MAX_EXPR_NODES);

    if (!json_object_object_get_ex(tv, "value", &val) ||
        !json_object_is_type(val, json_type_object))
        return p4_fail(w, -EINVAL, "%s is malformed", where);
    if (!json_object_object_get_ex(val, "op", &opobj) ||
        !json_object_is_type(opobj, json_type_string))
        return p4_fail(w, -EINVAL, "%s has no operator", where);
    opname = json_object_get_string(opobj);
    if (!opname)
        return p4_fail(w, -EINVAL, "%s has no operator", where);

    for (which = 0; which < sizeof(p4_expr_ops) / sizeof(p4_expr_ops[0]);
         which++) {
        if (strcmp(p4_expr_ops[which].name, opname) == 0) {
            known = true;
            break;
        }
    }
    if (!known)
        return p4_fail(w, -ENOSYS,
                       "%s uses operator '%s', which is not "
                       "supported",
                       where, opname);

    /* Claim this node before walking the children, so they land after it. */
    self = (p4_idx_t)cond->nexpr;
    cond->nexpr++;
    *slot = self;
    node = &cond->expr[self];
    node->op = p4_expr_ops[which].op;

    left = NULL;
    right = NULL;
    (void)json_object_object_get_ex(val, "left", &left);
    (void)json_object_object_get_ex(val, "right", &right);

    rc = p4_walk_expr_operand(w, left, cond, &node->left, depth, where);
    if (rc)
        return rc;
    rc = p4_walk_expr_operand(w, right, cond, &node->right, depth, where);
    if (rc)
        return rc;

    /* A unary operator carries its operand on the right and nothing on the
     * left; a binary one needs both. Either way round, a missing side
     * would be read as a value at evaluation time. */
    if (p4_expr_ops[which].unary) {
        if (node->left.kind != P4_EO_NONE || node->right.kind == P4_EO_NONE)
            return p4_fail(w, -EINVAL,
                           "%s: '%s' takes one operand, on the "
                           "right",
                           where, opname);
    } else if (node->left.kind == P4_EO_NONE ||
               node->right.kind == P4_EO_NONE) {
        return p4_fail(w, -EINVAL, "%s: '%s' takes two operands", where,
                       opname);
    }

    return 0;
}

static uint8_t p4_match_kind(const char *s, bool *ok)
{
    *ok = true;
    if (strcmp(s, "exact") == 0)
        return P4_MATCH_EXACT;
    if (strcmp(s, "lpm") == 0)
        return P4_MATCH_LPM;
    if (strcmp(s, "ternary") == 0)
        return P4_MATCH_TERNARY;
    *ok = false;
    return 0;
}

/* Find a pipeline node by name, over both tables and conditionals. */
static p4_idx_t p4_find_node(const struct p4_pipeline *pl, const char *name)
{
    for (uint16_t i = 0; i < pl->nnodes; i++) {
        const struct p4_node *n = &pl->nodes[i];
        const char *nm = (n->kind == P4_NODE_TABLE)
                             ? pl->tables[n->index].name
                             : pl->conditionals[n->index].name;

        if (strcmp(nm, name) == 0)
            return (p4_idx_t)i;
    }
    return P4_IDX_NONE;
}

/*
 * Resolve a "next" name to a node. A JSON null means the pipeline ends
 * here, which is P4_IDX_NONE -- distinct from a name that does not
 * resolve, which is an error.
 */
static int p4_resolve_next(struct p4_walk *w, const struct p4_pipeline *pl,
                           struct json_object *v, p4_idx_t *out,
                           const char *where)
{
    const char *name;

    if (!v || json_object_is_type(v, json_type_null)) {
        *out = P4_IDX_NONE;
        return 0;
    }
    if (!json_object_is_type(v, json_type_string))
        return p4_fail(w, -EINVAL, "%s has a bad next", where);
    name = json_object_get_string(v);
    *out = name ? p4_find_node(pl, name) : P4_IDX_NONE;
    if (*out == P4_IDX_NONE)
        return p4_fail(w, -EINVAL,
                       "%s points at '%s', which is not a table "
                       "or condition",
                       where, name ? name : "");
    return 0;
}

/* One table's key, actions and size. The next pointers come later. */
static int p4_walk_table(struct p4_walk *w, struct json_object *t,
                         struct p4_pipeline *pl, struct p4_table *dst,
                         uint32_t *total_entries)
{
    struct json_object *key, *acts, *v;
    char mt[P4_MAX_NAME + 1];
    size_t nk, na;
    uint32_t id, max_size;
    bool ok, flag;

    if (!p4_get_str(t, "name", dst->name, sizeof(dst->name)))
        return p4_fail(w, -EINVAL, "a table has a bad name");
    if (!p4_get_u32(t, "id", 0, P4_MAX_ID, &id))
        return p4_fail(w, -EINVAL, "table '%s' has a bad id", dst->name);
    dst->id = id;

    if (!p4_get_str(t, "type", mt, sizeof(mt)))
        return p4_fail(w, -EINVAL, "table '%s' has no type", dst->name);
    if (strcmp(mt, "simple") != 0)
        return p4_fail(w, -ENOSYS,
                       "table '%s' is of type '%s', which is not "
                       "supported",
                       dst->name, mt);

    if (!p4_get_str(t, "match_type", mt, sizeof(mt)))
        return p4_fail(w, -EINVAL, "table '%s' has no match_type", dst->name);
    dst->match_type = p4_match_kind(mt, &ok);
    if (!ok)
        return p4_fail(w, -ENOSYS,
                       "table '%s' matches '%s', which is not "
                       "supported",
                       dst->name, mt);

    /*
     * The mere presence of "entries" makes a table immutable in bmv2 and
     * makes its size the number of entries given, so an empty array is a
     * permanently empty table rather than a harmless default. Refuse it
     * either way: there is no control plane to populate one yet.
     */
    if (json_object_object_get_ex(t, "entries", &v) &&
        !json_object_is_type(v, json_type_null))
        return p4_fail(w, -ENOSYS,
                       "table '%s' has const entries, which are "
                       "not supported",
                       dst->name);

    /* Features that change what a match means, or what happens after it. */
    if (p4_get_bool(t, "with_counters", &flag) && flag)
        return p4_fail(w, -ENOSYS, "table '%s' has direct counters", dst->name);
    if (p4_get_bool(t, "support_timeout", &flag) && flag)
        return p4_fail(w, -ENOSYS, "table '%s' ages its entries", dst->name);
    if (json_object_object_get_ex(t, "direct_meters", &v) &&
        !json_object_is_type(v, json_type_null))
        return p4_fail(w, -ENOSYS, "table '%s' has a direct meter", dst->name);

    if (!p4_get_u32(t, "max_size", 0, P4_MAX_TOTAL_ENTRIES, &max_size))
        return p4_fail(w, -EINVAL,
                       "table '%s' has a bad max_size (at most "
                       "%u)",
                       dst->name, P4_MAX_TOTAL_ENTRIES);
    /*
     * Subtraction first. max_size is the one number here that names a
     * large allocation without the document's own size bounding it.
     */
    if (max_size > P4_MAX_TOTAL_ENTRIES - *total_entries)
        return p4_fail(w, -EINVAL,
                       "the tables ask for more than %u entries between them",
                       P4_MAX_TOTAL_ENTRIES);
    *total_entries += max_size;
    dst->max_size = max_size;

    if (!p4_get_array(t, "key", &key, P4_MAX_TABLE_KEY, &nk))
        return p4_fail(w, -EINVAL,
                       "table '%s' has a bad or oversized key "
                       "(at most %d)",
                       dst->name, P4_MAX_TABLE_KEY);
    for (size_t i = 0; i < nk; i++) {
        struct json_object *kf = json_object_array_get_idx(key, i);
        struct json_object *tgt;
        char h[P4_MAX_NAME + 1], f[P4_MAX_NAME + 1];

        if (!kf || !json_object_is_type(kf, json_type_object) ||
            !p4_get_str(kf, "match_type", mt, sizeof(mt)))
            return p4_fail(w, -EINVAL, "table '%s' key %zu is malformed",
                           dst->name, i);
        dst->key[i].match_type = p4_match_kind(mt, &ok);
        if (!ok)
            return p4_fail(w, -ENOSYS,
                           "table '%s' key %zu matches '%s', which is not "
                           "supported",
                           dst->name, i, mt);
        if (!json_object_object_get_ex(kf, "target", &tgt) ||
            !p4_pair_strings(tgt, h, sizeof(h), f, sizeof(f)))
            return p4_fail(w, -EINVAL, "table '%s' key %zu has a bad target",
                           dst->name, i);
        if (!p4_resolve_field(w->prog, h, f, &dst->key[i].field))
            return p4_fail(w, -EINVAL,
                           "table '%s' matches on '%s.%s', which does not "
                           "exist",
                           dst->name, h, f);
    }
    dst->nkey = (uint16_t)nk;

    if (!p4_get_array(t, "actions", &acts, P4_MAX_TABLE_ACTIONS, &na))
        return p4_fail(w, -EINVAL,
                       "table '%s' has bad or oversized actions (at most %d)",
                       dst->name, P4_MAX_TABLE_ACTIONS);
    if (na == 0)
        return p4_fail(w, -EINVAL, "table '%s' has no actions", dst->name);
    for (size_t i = 0; i < na; i++) {
        struct json_object *e = json_object_array_get_idx(acts, i);
        const char *name;

        if (!e || !json_object_is_type(e, json_type_string))
            return p4_fail(w, -EINVAL, "table '%s' action %zu is not a name",
                           dst->name, i);
        name = json_object_get_string(e);
        dst->actions[i] = P4_IDX_NONE;
        for (uint16_t a = 0; name && a < w->prog->nactions; a++) {
            if (strcmp(w->prog->actions[a].name, name) == 0) {
                dst->actions[i] = (p4_idx_t)a;
                break;
            }
        }
        if (dst->actions[i] == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "table '%s' names action '%s', which does not "
                           "exist",
                           dst->name, name ? name : "");
        dst->next[i] = P4_IDX_NONE;
    }
    dst->nactions = (uint16_t)na;
    dst->base_default_next = P4_IDX_NONE;
    dst->default_action = P4_IDX_NONE;

    /*
     * default_entry is what a table does on a miss. Without it, and
     * without a control plane to install entries, a table does nothing at
     * all, so the program would parse and deparse and decide nothing.
     */
    if (p4_get_obj(t, "default_entry", &v)) {
        struct json_object *data;
        uint32_t aid;
        size_t ndata;

        if (!p4_get_u32(v, "action_id", 0, P4_MAX_ID, &aid))
            return p4_fail(w, -EINVAL, "table '%s' has a bad default action",
                           dst->name);
        for (uint16_t a = 0; a < w->prog->nactions; a++) {
            if (w->prog->actions[a].id == aid) {
                dst->default_action = (p4_idx_t)a;
                break;
            }
        }
        if (dst->default_action == P4_IDX_NONE)
            return p4_fail(w, -EINVAL,
                           "table '%s' defaults to action id %u, which does "
                           "not exist",
                           dst->name, aid);
        /* The default action has to be one the table may actually run. */
        ok = false;
        for (uint16_t i = 0; i < dst->nactions; i++) {
            if (dst->actions[i] == dst->default_action) {
                ok = true;
                break;
            }
        }
        if (!ok)
            return p4_fail(w, -EINVAL,
                           "table '%s' defaults to '%s', which is not one of "
                           "its actions",
                           dst->name,
                           w->prog->actions[dst->default_action].name);

        if (!p4_get_array(v, "action_data", &data, P4_MAX_RUNTIME_ARGS, &ndata))
            return p4_fail(w, -EINVAL, "table '%s' has bad default action data",
                           dst->name);
        /* Too few or too many arguments and the action would read one the
         * entry never supplied. */
        if (ndata != w->prog->actions[dst->default_action].nruntime)
            return p4_fail(w, -EINVAL,
                           "table '%s' gives %zu arguments to '%s', which "
                           "takes %u",
                           dst->name, ndata,
                           w->prog->actions[dst->default_action].name,
                           w->prog->actions[dst->default_action].nruntime);
        for (size_t i = 0; i < ndata; i++) {
            struct json_object *e = json_object_array_get_idx(data, i);
            size_t blen = 0;
            bool neg = false;
            const char *vs;

            if (!e || !json_object_is_type(e, json_type_string))
                return p4_fail(w, -EINVAL,
                               "table '%s' default argument %zu is not a "
                               "literal",
                               dst->name, i);
            vs = json_object_get_string(e);
            if (!vs ||
                !ernic_parse_hexstr(vs, dst->default_data[i], P4_MAX_IMM_BYTES,
                                    &blen, &neg) ||
                neg)
                return p4_fail(w, -EINVAL,
                               "table '%s' default argument %zu is malformed",
                               dst->name, i);
            dst->default_data_len[i] = (uint16_t)blen;
        }
        dst->ndefault_data = (uint16_t)ndata;
    }

    (void)pl;
    return 0;
}

/*
 * next_tables is keyed either by action name or by __HIT__/__MISS__, never
 * both: a map carrying each would say two different things about where a
 * hit goes.
 */
static int p4_walk_next_tables(struct p4_walk *w, struct json_object *t,
                               struct p4_pipeline *pl, struct p4_table *dst)
{
    struct json_object *nt, *v;
    bool by_hit = false, by_action = false;
    char where[P4_MAX_NAME + 48];

    (void)snprintf(where, sizeof(where), "table '%s' next_tables", dst->name);

    if (!p4_get_obj(t, "next_tables", &nt))
        return p4_fail(w, -EINVAL, "%s is missing or malformed", where);

    json_object_object_foreach(nt, key, val)
    {
        if (strcmp(key, "__HIT__") == 0 || strcmp(key, "__MISS__") == 0)
            by_hit = true;
        else
            by_action = true;
        (void)val;
    }
    if (by_hit && by_action)
        return p4_fail(w, -EINVAL,
                       "%s mixes __HIT__/__MISS__ with action "
                       "names",
                       where);

    if (by_hit) {
        p4_idx_t hit = P4_IDX_NONE, miss = P4_IDX_NONE;
        int rc;

        if (json_object_object_get_ex(nt, "__HIT__", &v)) {
            rc = p4_resolve_next(w, pl, v, &hit, where);
            if (rc)
                return rc;
        }
        if (json_object_object_get_ex(nt, "__MISS__", &v)) {
            rc = p4_resolve_next(w, pl, v, &miss, where);
            if (rc)
                return rc;
        }
        /* Every action goes the same way on a hit. */
        for (uint16_t i = 0; i < dst->nactions; i++)
            dst->next[i] = hit;
        dst->base_default_next = miss;
        return 0;
    }

    for (uint16_t i = 0; i < dst->nactions; i++) {
        const char *aname = w->prog->actions[dst->actions[i]].name;
        int rc;

        if (!json_object_object_get_ex(nt, aname, &v))
            return p4_fail(w, -EINVAL, "%s does not say where '%s' goes", where,
                           aname);
        rc = p4_resolve_next(w, pl, v, &dst->next[i], where);
        if (rc)
            return rc;
    }
    /* Any key that is not one of the table's actions names something that
     * will never be consulted, which is a program that does not mean what
     * it says. */
    json_object_object_foreach(nt, key2, val2)
    {
        bool found = false;

        (void)val2;
        for (uint16_t i = 0; i < dst->nactions; i++) {
            if (strcmp(w->prog->actions[dst->actions[i]].name, key2) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return p4_fail(w, -EINVAL,
                           "%s names '%s', which is not one of "
                           "the table's actions",
                           where, key2);
    }

    if (json_object_object_get_ex(t, "base_default_next", &v))
        return p4_resolve_next(w, pl, v, &dst->base_default_next, where);
    return 0;
}

/*
 * The pipeline graph must be acyclic: a cycle would be an infinite loop on
 * the thread that also services the vfio-user poll loop. Iterative
 * depth-first search with an explicit stack and colours, so a deep graph
 * cannot overflow the C stack either.
 */
static int p4_check_acyclic(struct p4_walk *w, const struct p4_pipeline *pl)
{
    enum { WHITE = 0, GREY, BLACK };
    uint8_t colour[P4_MAX_NODES];
    p4_idx_t stack[P4_MAX_NODES + 1];
    uint16_t edge[P4_MAX_NODES + 1];
    int sp;

    memset(colour, WHITE, sizeof(colour));

    for (uint16_t start = 0; start < pl->nnodes; start++) {
        if (colour[start] != WHITE)
            continue;
        sp = 0;
        stack[0] = start;
        edge[0] = 0;
        colour[start] = GREY;

        while (sp >= 0) {
            p4_idx_t cur = stack[sp];
            const struct p4_node *n = &pl->nodes[cur];
            p4_idx_t next = P4_IDX_NONE;
            uint16_t nedges;
            bool have = false;

            if (n->kind == P4_NODE_TABLE) {
                const struct p4_table *tb = &pl->tables[n->index];

                nedges = (uint16_t)(tb->nactions + 1);
                while (edge[sp] < nedges && !have) {
                    next = (edge[sp] < tb->nactions) ? tb->next[edge[sp]]
                                                     : tb->base_default_next;
                    edge[sp]++;
                    have = (next != P4_IDX_NONE);
                }
            } else {
                const struct p4_conditional *c = &pl->conditionals[n->index];

                nedges = 2;
                while (edge[sp] < nedges && !have) {
                    next = (edge[sp] == 0) ? c->true_next : c->false_next;
                    edge[sp]++;
                    have = (next != P4_IDX_NONE);
                }
            }

            if (!have) {
                colour[cur] = BLACK;
                sp--;
                continue;
            }
            if (colour[next] == GREY) {
                const struct p4_node *bad = &pl->nodes[next];

                return p4_fail(w, -EINVAL, "pipeline '%s' loops back to '%s'",
                               pl->name,
                               bad->kind == P4_NODE_TABLE
                                   ? pl->tables[bad->index].name
                                   : pl->conditionals[bad->index].name);
            }
            if (colour[next] == WHITE) {
                colour[next] = GREY;
                sp++;
                stack[sp] = next;
                edge[sp] = 0;
            }
        }
    }
    return 0;
}

/* One pipeline: its tables and conditionals, then the graph between them. */
static int p4_walk_pipeline(struct p4_walk *w, struct json_object *p,
                            struct p4_pipeline *pl)
{
    struct json_object *tables, *conds, *v;
    char init[P4_MAX_NAME + 1];
    size_t nt, nc;
    uint32_t id, total_entries = 0;
    int rc;

    if (!p4_get_str(p, "name", pl->name, sizeof(pl->name)))
        return p4_fail(w, -EINVAL, "a pipeline has a bad name");
    if (!p4_get_u32(p, "id", 0, P4_MAX_ID, &id))
        return p4_fail(w, -EINVAL, "pipeline '%s' has a bad id", pl->name);
    pl->id = id;

    if (!p4_absent_or_empty(p, "action_profiles"))
        return p4_fail(w, -ENOSYS, "pipeline '%s' uses an action profile",
                       pl->name);

    if (!p4_get_array(p, "tables", &tables, P4_MAX_TABLES, &nt))
        return p4_fail(w, -EINVAL,
                       "pipeline '%s' has bad or oversized tables (at most "
                       "%d)",
                       pl->name, P4_MAX_TABLES);
    if (!p4_get_array(p, "conditionals", &conds, P4_MAX_CONDITIONALS, &nc))
        return p4_fail(w, -EINVAL,
                       "pipeline '%s' has bad or oversized conditionals (at "
                       "most %d)",
                       pl->name, P4_MAX_CONDITIONALS);

    for (size_t i = 0; i < nt; i++) {
        struct json_object *t = json_object_array_get_idx(tables, i);

        if (!t || !json_object_is_type(t, json_type_object))
            return p4_fail(w, -EINVAL,
                           "pipeline '%s' table %zu is not an "
                           "object",
                           pl->name, i);
        rc = p4_walk_table(w, t, pl, &pl->tables[i], &total_entries);
        if (rc)
            return rc;
        pl->nodes[pl->nnodes].kind = P4_NODE_TABLE;
        pl->nodes[pl->nnodes].index = (p4_idx_t)i;
        pl->nnodes++;
    }
    pl->ntables = (uint16_t)nt;

    for (size_t i = 0; i < nc; i++) {
        struct json_object *c = json_object_array_get_idx(conds, i);
        struct p4_conditional *cd = &pl->conditionals[i];
        struct json_object *expr;
        char where[P4_MAX_NAME + 48];
        p4_idx_t root = P4_IDX_NONE;

        if (!c || !json_object_is_type(c, json_type_object))
            return p4_fail(w, -EINVAL,
                           "pipeline '%s' conditional %zu is not an object",
                           pl->name, i);
        if (!p4_get_str(c, "name", cd->name, sizeof(cd->name)))
            return p4_fail(w, -EINVAL, "a conditional has a bad name");
        if (!p4_get_u32(c, "id", 0, P4_MAX_ID, &id))
            return p4_fail(w, -EINVAL, "conditional '%s' has a bad id",
                           cd->name);
        cd->id = id;

        (void)snprintf(where, sizeof(where), "condition '%s'", cd->name);
        if (!json_object_object_get_ex(c, "expression", &expr))
            return p4_fail(w, -EINVAL, "%s has no expression", where);
        rc = p4_walk_expr(w, expr, cd, &root, 0, where);
        if (rc)
            return rc;
        if (root != 0)
            return p4_fail(w, -EINVAL, "%s has a misplaced root", where);

        pl->nodes[pl->nnodes].kind = P4_NODE_CONDITIONAL;
        pl->nodes[pl->nnodes].index = (p4_idx_t)i;
        pl->nnodes++;
    }
    pl->nconditionals = (uint16_t)nc;

    /* Every node is named now, so forward references resolve. */
    for (size_t i = 0; i < nt; i++) {
        rc = p4_walk_next_tables(w, json_object_array_get_idx(tables, i), pl,
                                 &pl->tables[i]);
        if (rc)
            return rc;
    }
    for (size_t i = 0; i < nc; i++) {
        struct json_object *c = json_object_array_get_idx(conds, i);
        struct p4_conditional *cd = &pl->conditionals[i];
        char where[P4_MAX_NAME + 48];

        (void)snprintf(where, sizeof(where), "condition '%s'", cd->name);
        /* json_object_object_get_ex() writes its output only when it finds
         * the key, so each lookup starts from NULL -- which is also how an
         * absent next is meant to read: the pipeline ends there. */
        v = NULL;
        (void)json_object_object_get_ex(c, "true_next", &v);
        rc = p4_resolve_next(w, pl, v, &cd->true_next, where);
        if (rc)
            return rc;
        v = NULL;
        (void)json_object_object_get_ex(c, "false_next", &v);
        rc = p4_resolve_next(w, pl, v, &cd->false_next, where);
        if (rc)
            return rc;
    }

    if (!p4_get_str(p, "init_table", init, sizeof(init))) {
        /* A pipeline with nothing in it has no entry point, which is how
         * p4c writes an empty egress. */
        if (pl->nnodes == 0) {
            pl->init_node = P4_IDX_NONE;
            return 0;
        }
        return p4_fail(w, -EINVAL, "pipeline '%s' has no init_table", pl->name);
    }
    pl->init_node = p4_find_node(pl, init);
    if (pl->init_node == P4_IDX_NONE)
        return p4_fail(w, -EINVAL,
                       "pipeline '%s' starts at '%s', which is not one of "
                       "its nodes",
                       pl->name, init);

    return p4_check_acyclic(w, pl);
}

static int p4_walk_pipelines(struct p4_walk *w, struct json_object *root)
{
    struct json_object *arr;
    size_t n;
    bool have_ingress = false;

    if (!p4_get_array(root, "pipelines", &arr, P4_MAX_PIPELINES, &n))
        return p4_fail(w, -EINVAL,
                       "missing, malformed, or oversized 'pipelines' (at "
                       "most %d)",
                       P4_MAX_PIPELINES);

    for (size_t i = 0; i < n; i++) {
        struct json_object *p = json_object_array_get_idx(arr, i);
        int rc;

        if (!p || !json_object_is_type(p, json_type_object))
            return p4_fail(w, -EINVAL, "pipelines[%zu] is not an object", i);
        rc = p4_walk_pipeline(w, p, &w->prog->pipelines[i]);
        if (rc)
            return rc;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(w->prog->pipelines[j].name,
                       w->prog->pipelines[i].name) == 0)
                return p4_fail(w, -EINVAL, "duplicate pipeline '%s'",
                               w->prog->pipelines[i].name);
        }
        if (strcmp(w->prog->pipelines[i].name, "ingress") == 0)
            have_ingress = true;
    }
    w->prog->npipelines = (uint16_t)n;

    if (!have_ingress)
        return p4_fail(w, -EINVAL, "no 'ingress' pipeline");
    return 0;
}

int p4_json_parse(const void *buf, size_t len, struct p4_program **out,
                  char *err, size_t err_len)
{
    struct json_tokener *tok;
    struct json_object *root = NULL;
    struct p4_walk w;
    int rc;

    if (err && err_len)
        err[0] = '\0';

    if (!buf || !out || len == 0 || len > P4_PROGRAM_MAX)
        return -EINVAL;

    memset(&w, 0, sizeof(w));
    w.err = err;
    w.err_len = err_len;

    tok = json_tokener_new_ex(P4_JSON_MAX_DEPTH);
    if (!tok)
        return -ENOMEM;
    json_tokener_set_flags(tok,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);

    root = json_tokener_parse_ex(tok, (const char *)buf, (int)len);

    if (!root || json_tokener_get_error(tok) != json_tokener_success) {
        rc = p4_fail(&w, -EINVAL, "not valid JSON: %s",
                     json_tokener_error_desc(json_tokener_get_error(tok)));
        goto out;
    }
    /*
     * Not about trailing garbage, which the tokener already refuses without
     * ALLOW_TRAILING_CHARS: this catches a raw NUL part way through the
     * input, where a valid prefix parses and reports success while the rest
     * of the buffer is never seen.
     */
    if ((size_t)json_tokener_get_parse_end(tok) != len) {
        rc = p4_fail(&w, -EINVAL, "input continues past the end of the JSON");
        goto out;
    }
    /* STRICT accepts a bare value at the top level; this is what does not. */
    if (!json_object_is_type(root, json_type_object)) {
        rc = p4_fail(&w, -EINVAL, "the top level must be a JSON object");
        goto out;
    }

    rc = p4_check_meta(&w, root);
    if (rc)
        goto out;

    rc = p4_check_unsupported(&w, root);
    if (rc)
        goto out;

    w.prog = calloc(1, sizeof(*w.prog));
    if (!w.prog) {
        rc = -ENOMEM;
        goto out;
    }

    rc = p4_walk_header_types(&w, root);
    if (rc)
        goto out;
    rc = p4_walk_headers(&w, root);
    if (rc)
        goto out;
    rc = p4_resolve_std_metadata(&w);
    if (rc)
        goto out;
    rc = p4_walk_parser(&w, root);
    if (rc)
        goto out;
    rc = p4_walk_deparser(&w, root);
    if (rc)
        goto out;
    /* After the deparser, because the check that architecture state does
     * not go on a wire needs the emit order to exist. */
    rc = p4_resolve_rdma_metadata(&w);
    if (rc)
        goto out;
    rc = p4_walk_externs(&w, root);
    if (rc)
        goto out;
    rc = p4_walk_actions(&w, root);
    if (rc)
        goto out;
    rc = p4_walk_pipelines(&w, root);
    if (rc)
        goto out;

    /*
     * The program is loaded.  Executing it is a separate matter: nothing
     * runs a pipeline yet, so a loaded program still forwards nothing.
     */
    rc = 0;

out:
    if (rc != 0) {
        free(w.prog);
    } else {
        *out = w.prog;
    }
    /* Exactly one put, on every path: json_object_put is NULL safe. */
    json_object_put(root);
    json_tokener_free(tok);
    return rc;
}

void p4_json_free(struct p4_program *prog)
{
    free(prog);
}
