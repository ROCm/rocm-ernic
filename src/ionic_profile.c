/*
 * NIC profile loader: ini text -> struct ionic_profile.  See ionic_profile.h.
 *
 * glib's GKeyFile reads the values.  GKeyFile is lenient where a profile
 * must not be -- a repeated key silently replaces the first, and its errors
 * carry no line number -- so a line scanner runs first.  It checks the
 * structure, rejects repeats, and records the line of every section and
 * key, so that every error names the source, line and key.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

#include "ernic_err.h"
#include "ionic_profile.h"

/* ---- Schema ------------------------------------------------------------ */

enum key_kind {
    KEY_ASIC_TYPE,
    KEY_ASIC_REV,
    KEY_FW_VERSION,
    KEY_SPEED,
    KEY_XCVR,
    KEY_MTU,
    KEY_ETH_QUEUES,
    KEY_RDMA_EQS,
    KEY_INTR_VECTORS,
    KEY_STATS_INTERVAL,
};

static const struct schema_key {
    const char *group;
    const char *key;
    enum key_kind kind;
} schema[] = {
    {"identity", "asic_type", KEY_ASIC_TYPE},
    {"identity", "asic_rev", KEY_ASIC_REV},
    {"identity", "fw_version", KEY_FW_VERSION},
    {"link", "speed_mbps", KEY_SPEED},
    {"link", "xcvr", KEY_XCVR},
    {"link", "mtu_max", KEY_MTU},
    {"queues", "eth", KEY_ETH_QUEUES},
    {"queues", "rdma_eqs", KEY_RDMA_EQS},
    {"queues", "intr_vectors", KEY_INTR_VECTORS},
    {"stats", "interval_ms", KEY_STATS_INTERVAL},
};

#define SCHEMA_COUNT (sizeof(schema) / sizeof(schema[0]))

static const char *const known_groups = "identity, link, queues, stats";

static const char *const asic_names[] = {"none", "capri", "elba", "giglio",
                                         "salina"};
static const char *const xcvr_names[] = {"copper", "fiber", "none"};

/* IONIC_SPEED_* in the kernel's ionic_if.h, in Mbps. */
static const uint32_t speeds[] = {1000,  10000,  25000,  40000,
                                  50000, 100000, 200000, 400000};

#define N_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

static bool known_group(const char *group)
{
    for (size_t i = 0; i < SCHEMA_COUNT; i++)
        if (strcmp(schema[i].group, group) == 0)
            return true;
    return false;
}

static const struct schema_key *find_key(const char *group, const char *key)
{
    for (size_t i = 0; i < SCHEMA_COUNT; i++)
        if (strcmp(schema[i].group, group) == 0 &&
            strcmp(schema[i].key, key) == 0)
            return &schema[i];
    return NULL;
}

void ionic_profile_defaults(struct ionic_profile *p)
{
    memset(p, 0, sizeof(*p));
    p->identity.asic_type = IONIC_PROFILE_ASIC_NONE;
    p->identity.asic_rev = 0;
    memcpy(p->identity.fw_version, "rocm-ernic-1.0", sizeof("rocm-ernic-1.0"));
    p->link.speed_mbps = 100000;
    p->link.xcvr = IONIC_PROFILE_XCVR_COPPER;
    p->link.mtu_max = 9198;
    p->queues.eth = 4;
    p->queues.rdma_eqs = 32;
    p->queues.intr_vectors = 32;
    p->stats.interval_ms = 1000;
    memcpy(p->chain, "compat", sizeof("compat"));
}

/* ---- Line scanner ------------------------------------------------------ */

/* Longest section or key name; nothing in the schema comes close. */
#define NAME_MAX_LEN 63u

struct scanned {
    char group[NAME_MAX_LEN + 1];
    char key[NAME_MAX_LEN + 1]; /* "" for a [section] line */
    unsigned int line;
};

struct scan {
    struct scanned *items;
    size_t count;
    size_t cap;
};

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

/* Copy [@s, @e) into @out if it is a non-empty name that fits. */
static bool take_name(const char *s, const char *e, char *out)
{
    size_t n = (size_t)(e - s);

    if (n == 0 || n > NAME_MAX_LEN)
        return false;
    for (const char *c = s; c < e; c++)
        if (!name_char(*c))
            return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

static const struct scanned *scan_find(const struct scan *sc, const char *group,
                                       const char *key)
{
    for (size_t i = 0; i < sc->count; i++)
        if (strcmp(sc->items[i].group, group) == 0 &&
            strcmp(sc->items[i].key, key) == 0)
            return &sc->items[i];
    return NULL;
}

static bool scan_add(struct scan *sc, const char *group, const char *key,
                     unsigned int line)
{
    struct scanned *it;

    if (sc->count == sc->cap) {
        size_t cap = sc->cap ? sc->cap * 2 : 16;
        struct scanned *n = realloc(sc->items, cap * sizeof(*n));

        if (!n)
            return false;
        sc->items = n;
        sc->cap = cap;
    }
    it = &sc->items[sc->count++];
    /* take_name() bounded both. */
    memcpy(it->group, group, strlen(group) + 1);
    memcpy(it->key, key, strlen(key) + 1);
    it->line = line;
    return true;
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/*
 * Check the structure of @text and record every section and key line.
 * Only blank lines, '#' comments, "[section]" and "key = value" are
 * allowed, a key must sit in a section, and nothing may appear twice.
 */
static int scan_text(struct scan *sc, const char *source, const char *text,
                     size_t len, char *err, size_t err_len)
{
    char group[NAME_MAX_LEN + 1] = "";
    unsigned int line = 0;
    size_t pos = 0;

    while (pos < len) {
        const char *s = text + pos;
        const char *eol = memchr(s, '\n', len - pos);
        const char *e = eol ? eol : text + len;
        char name[NAME_MAX_LEN + 1];
        const struct scanned *prev;

        line++;
        pos = (size_t)(e - text) + (eol ? 1 : 0);
        if (memchr(s, '\0', (size_t)(e - s))) {
            ernic_set_err(err, err_len, "%s:%u: NUL byte in the text", source,
                          line);
            return -1;
        }
        while (s < e && is_blank(*s))
            s++;
        while (e > s && is_blank(e[-1]))
            e--;
        if (s == e || *s == '#')
            continue;

        if (*s == '[') {
            if (e[-1] != ']' || !take_name(s + 1, e - 1, name)) {
                ernic_set_err(
                    err, err_len,
                    "%s:%u: a section header is '[name]', with letters, "
                    "digits, '_', '-' or '.'",
                    source, line);
                return -1;
            }
            if (!known_group(name)) {
                ernic_set_err(
                    err, err_len,
                    "%s:%u: unknown section [%s] (known sections: %s)", source,
                    line, name, known_groups);
                return -1;
            }
            prev = scan_find(sc, name, "");
            if (prev) {
                ernic_set_err(
                    err, err_len,
                    "%s:%u: section [%s] appears twice (first at line %u)",
                    source, line, name, prev->line);
                return -1;
            }
            memcpy(group, name, sizeof(group));
            if (!scan_add(sc, group, "", line)) {
                ernic_set_err(err, err_len, "%s: out of memory", source);
                return -1;
            }
            continue;
        }

        const char *eq = memchr(s, '=', (size_t)(e - s));
        const char *ke = eq;

        while (ke && ke > s && is_blank(ke[-1]))
            ke--;
        if (!eq || !take_name(s, ke, name)) {
            ernic_set_err(err, err_len,
                          "%s:%u: expected '[section]', 'key = value' or a '#' "
                          "comment",
                          source, line);
            return -1;
        }
        if (group[0] == '\0') {
            ernic_set_err(err, err_len,
                          "%s:%u: key '%s' is outside any [section]", source,
                          line, name);
            return -1;
        }
        if (!find_key(group, name)) {
            ernic_set_err(err, err_len, "%s:%u: unknown key '%s.%s'", source,
                          line, group, name);
            return -1;
        }
        prev = scan_find(sc, group, name);
        if (prev) {
            ernic_set_err(err, err_len,
                          "%s:%u: key '%s.%s' is set twice (first at line %u)",
                          source, line, group, name, prev->line);
            return -1;
        }
        if (!scan_add(sc, group, name, line)) {
            ernic_set_err(err, err_len, "%s: out of memory", source);
            return -1;
        }
    }
    return 0;
}

/* ---- Values ------------------------------------------------------------ */

/* Digits only; -1 if not a number, -2 if it overflows. */
static int parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;

    if (!s || *s == '\0')
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        if (v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10)
            return -2;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 0;
}

static int pick_name(const char *const *names, size_t count, const char *v)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(names[i], v) == 0)
            return (int)i;
    return -1;
}

static bool fw_version_ok(const char *v)
{
    size_t n = strlen(v);

    if (n == 0 || n > IONIC_PROFILE_FW_VERSION_MAX || v[0] == ' ' ||
        v[n - 1] == ' ')
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e ||
            v[i] == '\\')
            return false;
    return true;
}

/*
 * @raw as a number for @where: 0 with *@v set, 1 if it is a number too big
 * for 64 bits, or -1 with "integer expected" in @err.
 */
static int number(const char *raw, const char *where, uint64_t *v, char *err,
                  size_t err_len)
{
    int rc = parse_u64(raw, v);

    if (rc == -1)
        ernic_set_err(err, err_len, "%s: integer expected, got '%.40s'", where,
                      raw);
    return rc == -2 ? 1 : rc;
}

int ionic_profile_set_stats_interval(struct ionic_profile *p, const char *raw,
                                     const char *where, char *err,
                                     size_t err_len)
{
    uint64_t v = 0;
    int rc = number(raw, where, &v, err, err_len);

    if (rc < 0)
        return -1;
    if (rc != 0 || v < IONIC_PROFILE_STATS_INTERVAL_MIN ||
        v > IONIC_PROFILE_STATS_INTERVAL_MAX) {
        ernic_set_err(err, err_len, "%s: '%.40s' out of range %u..%u ms", where,
                      raw, IONIC_PROFILE_STATS_INTERVAL_MIN,
                      IONIC_PROFILE_STATS_INTERVAL_MAX);
        return -1;
    }
    p->stats.interval_ms = (uint32_t)v;
    return 0;
}

/*
 * @raw as a number in [@min, @max] for @where: 0 with *@out set, or -1
 * with the error in @err.
 */
static int ranged(const char *raw, const char *where, uint32_t min,
                  uint32_t max, uint32_t *out, char *err, size_t err_len)
{
    uint64_t v = 0;
    int rc = number(raw, where, &v, err, err_len);

    if (rc < 0)
        return -1;
    if (rc != 0 || v < min || v > max) {
        ernic_set_err(err, err_len, "%s: '%.40s' out of range %u..%u", where,
                      raw, min, max);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int ionic_profile_set_eth_queues(struct ionic_profile *p, const char *raw,
                                 const char *where, char *err, size_t err_len)
{
    return ranged(raw, where, IONIC_PROFILE_ETH_QUEUES_MIN,
                  IONIC_PROFILE_ETH_QUEUES_MAX, &p->queues.eth, err, err_len);
}

/* "auto" has to fit the MSI-X table for every valid queues.eth and
 * queues.rdma_eqs. */
_Static_assert(1u + IONIC_PROFILE_ETH_QUEUES_MAX + IONIC_PROFILE_RDMA_EQS_MAX <=
                   IONIC_PROFILE_INTR_VECTORS_MAX,
               "intr_vectors = auto must not exceed the most vectors allowed");

uint32_t ionic_profile_intr_vectors(const struct ionic_profile *p)
{
    if (p->queues.intr_vectors != IONIC_PROFILE_INTR_VECTORS_AUTO)
        return p->queues.intr_vectors;
    /* At most 1 + 64 + 32 = 97, inside IONIC_PROFILE_INTR_VECTORS_MAX. */
    return 1u + p->queues.eth + p->queues.rdma_eqs;
}

int ionic_profile_warning(const struct ionic_profile *p, char *msg,
                          size_t msg_len)
{
    uint32_t want = 1u + p->queues.eth + p->queues.rdma_eqs;
    uint32_t have = ionic_profile_intr_vectors(p);

    if (have >= want)
        return 0;
    ernic_set_err(
        msg, msg_len,
        "queues.intr_vectors = %" PRIu32 " is less than 1 + queues.eth + "
        "queues.rdma_eqs = %" PRIu32 "; a guest with enough vCPUs halves "
        "its RDMA EQs or Ethernet queues to fit",
        have, want);
    return 1;
}

/* Store one key's value in @q.  @where is "source:line: group.key". */
static int set_value(struct ionic_profile *q, const struct schema_key *k,
                     const char *raw, const char *str, const char *where,
                     char *err, size_t err_len)
{
    uint64_t v = 0;
    int rc, idx;

    switch (k->kind) {
    case KEY_ASIC_TYPE:
        idx = pick_name(asic_names, N_ELEMS(asic_names), raw);
        if (idx < 0) {
            ernic_set_err(
                err, err_len,
                "%s: '%.40s' is not one of none, capri, elba, giglio, "
                "salina",
                where, raw);
            return -1;
        }
        q->identity.asic_type = (uint8_t)idx;
        return 0;

    case KEY_XCVR:
        idx = pick_name(xcvr_names, N_ELEMS(xcvr_names), raw);
        if (idx < 0) {
            ernic_set_err(err, err_len,
                          "%s: '%.40s' is not one of copper, fiber, none",
                          where, raw);
            return -1;
        }
        q->link.xcvr = (uint8_t)idx;
        return 0;

    case KEY_FW_VERSION:
        if (strlen(str) > IONIC_PROFILE_FW_VERSION_MAX) {
            ernic_set_err(err, err_len, "%s: %zu characters; at most %u", where,
                          strlen(str), IONIC_PROFILE_FW_VERSION_MAX);
            return -1;
        }
        if (!fw_version_ok(str)) {
            ernic_set_err(
                err, err_len,
                "%s: must be 1 to %u printable ASCII characters other "
                "than '\\', with no leading or trailing space",
                where, IONIC_PROFILE_FW_VERSION_MAX);
            return -1;
        }
        memcpy(q->identity.fw_version, str, strlen(str) + 1);
        return 0;

    case KEY_ASIC_REV:
        rc = number(raw, where, &v, err, err_len);
        if (rc < 0)
            return -1;
        if (rc != 0 || v > 255) {
            ernic_set_err(err, err_len, "%s: '%.40s' out of range 0..255",
                          where, raw);
            return -1;
        }
        q->identity.asic_rev = (uint8_t)v;
        return 0;

    case KEY_SPEED:
        rc = number(raw, where, &v, err, err_len);
        if (rc < 0)
            return -1;
        for (size_t i = 0; rc == 0 && i < N_ELEMS(speeds); i++)
            if (v == speeds[i]) {
                q->link.speed_mbps = speeds[i];
                return 0;
            }
        ernic_set_err(
            err, err_len,
            "%s: '%.40s' is not a supported speed (1000, 10000, 25000, "
            "40000, 50000, 100000, 200000 or 400000 Mbps)",
            where, raw);
        return -1;

    case KEY_MTU:
        rc = number(raw, where, &v, err, err_len);
        if (rc < 0)
            return -1;
        if (rc != 0 || v < IONIC_PROFILE_MTU_MIN || v > IONIC_PROFILE_MTU_MAX) {
            ernic_set_err(err, err_len, "%s: '%.40s' out of range %u..%u",
                          where, raw, IONIC_PROFILE_MTU_MIN,
                          IONIC_PROFILE_MTU_MAX);
            return -1;
        }
        q->link.mtu_max = (uint32_t)v;
        return 0;

    case KEY_ETH_QUEUES:
        return ionic_profile_set_eth_queues(q, raw, where, err, err_len);

    case KEY_RDMA_EQS:
        return ranged(raw, where, IONIC_PROFILE_RDMA_EQS_MIN,
                      IONIC_PROFILE_RDMA_EQS_MAX, &q->queues.rdma_eqs, err,
                      err_len);

    case KEY_INTR_VECTORS:
        if (strcmp(raw, "auto") == 0) {
            q->queues.intr_vectors = IONIC_PROFILE_INTR_VECTORS_AUTO;
            return 0;
        }
        if (parse_u64(raw, &v) == -1) {
            ernic_set_err(err, err_len,
                          "%s: 'auto' or an integer expected, got '%.40s'",
                          where, raw);
            return -1;
        }
        return ranged(raw, where, IONIC_PROFILE_INTR_VECTORS_MIN,
                      IONIC_PROFILE_INTR_VECTORS_MAX, &q->queues.intr_vectors,
                      err, err_len);

    case KEY_STATS_INTERVAL:
        return ionic_profile_set_stats_interval(q, raw, where, err, err_len);

    default:
        ernic_set_err(err, err_len, "%s: no handler for this key", where);
        return -1;
    }
}

int ionic_profile_apply_text(struct ionic_profile *p, const char *source,
                             const char *text, size_t len, char *err,
                             size_t err_len)
{
    struct scan sc = {NULL, 0, 0};
    struct ionic_profile q = *p;
    GKeyFile *kf = NULL;
    GError *gerr = NULL;
    int rc = -1;

    if (!source)
        source = "profile";
    if (!text && len) {
        ernic_set_err(err, err_len, "%s: no text", source);
        return -1;
    }
    if (scan_text(&sc, source, text, len, err, err_len) != 0)
        goto out;

    kf = g_key_file_new();
    if (len &&
        !g_key_file_load_from_data(kf, text, len, G_KEY_FILE_NONE, &gerr)) {
        ernic_set_err(err, err_len, "%s: %s", source,
                      gerr ? gerr->message : "not a valid key file");
        goto out;
    }

    for (size_t i = 0; i < sc.count; i++) {
        const struct scanned *it = &sc.items[i];
        const struct schema_key *k;
        char where[256];
        gchar *raw, *str;
        int set;

        if (it->key[0] == '\0')
            continue;
        k = find_key(it->group, it->key);
        raw = g_key_file_get_value(kf, it->group, it->key, NULL);
        str = g_key_file_get_string(kf, it->group, it->key, NULL);
        snprintf(where, sizeof(where), "%s:%u: %s.%s", source, it->line,
                 it->group, it->key);
        if (k && raw && str) {
            /* GKeyFile keeps a value's trailing blanks, and a stray one
             * at the end of a line is invisible. */
            set = set_value(&q, k, g_strstrip(raw), g_strstrip(str), where, err,
                            err_len);
        } else {
            ernic_set_err(err, err_len, "%s: value could not be read", where);
            set = -1;
        }
        g_free(raw);
        g_free(str);
        if (set != 0)
            goto out;
    }
    *p = q;
    rc = 0;
out:
    if (gerr)
        g_error_free(gerr);
    if (kf)
        g_key_file_free(kf);
    free(sc.items);
    return rc;
}

/* ---- Layers ------------------------------------------------------------ */

static bool is_path(const char *spec)
{
    size_t n = strlen(spec);

    return strchr(spec, '/') != NULL ||
           (n >= 4 && strcmp(spec + n - 4, ".ini") == 0);
}

/* Read the whole of @path into a fresh buffer of *@len bytes. */
static char *read_file(const char *path, size_t *len, char *err, size_t err_len)
{
    struct stat st;
    char *buf = NULL;
    size_t got = 0;
    /* A FIFO must not block before descriptor-based file-type validation. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0) {
        ernic_set_err(err, err_len, "cannot open NIC profile '%s': %s", path,
                      strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ernic_set_err(err, err_len, "NIC profile '%s' is not a regular file",
                      path);
        goto fail;
    }
    if (st.st_size > (off_t)IONIC_PROFILE_FILE_MAX) {
        ernic_set_err(err, err_len, "NIC profile '%s' is larger than %u bytes",
                      path, IONIC_PROFILE_FILE_MAX);
        goto fail;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        ernic_set_err(err, err_len, "NIC profile '%s': out of memory", path);
        goto fail;
    }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ernic_set_err(
                err, err_len, "cannot read NIC profile '%s': %s", path,
                n < 0 ? strerror(errno) : "file shrank while reading");
            goto fail;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);
    *len = got;
    return buf;
fail:
    free(buf);
    close(fd);
    return NULL;
}

int ionic_profile_apply(struct ionic_profile *p, const char *spec, char *err,
                        size_t err_len)
{
    struct ionic_profile q = *p;
    size_t used = strlen(q.chain);
    char *file = NULL;
    const char *text;
    size_t len;
    int rc;

    if (!spec || spec[0] == '\0') {
        ernic_set_err(err, err_len, "empty NIC profile name");
        return -1;
    }
    if (used + 3 + strlen(spec) >= sizeof(q.chain)) {
        ernic_set_err(err, err_len, "NIC profile '%s': too many profile layers",
                      spec);
        return -1;
    }
    if (is_path(spec)) {
        file = read_file(spec, &len, err, err_len);
        if (!file)
            return -1;
        text = file;
    } else {
        text = ionic_profile_builtin(spec);
        if (!text) {
            ernic_set_err(
                err, err_len,
                "unknown NIC profile '%s' (built-in profiles: %s; a file "
                "path needs a '/' or an .ini suffix)",
                spec, ionic_profile_builtin_names());
            return -1;
        }
        len = strlen(text);
    }
    rc = ionic_profile_apply_text(&q, spec, text, len, err, err_len);
    free(file);
    if (rc != 0)
        return -1;
    /* The length check above leaves room for " + ", @spec and the NUL. */
    if (used) {
        memcpy(q.chain + used, " + ", 3);
        used += 3;
    }
    memcpy(q.chain + used, spec, strlen(spec) + 1);
    *p = q;
    return 0;
}

/* ---- Output ------------------------------------------------------------ */

static const char *name_or(const char *const *names, size_t count, size_t i)
{
    return i < count ? names[i] : "?";
}

/* Every section's values, which the hash covers. */
static int format_values(const struct ionic_profile *p, char *buf, size_t len)
{
    char vectors[16];

    if (p->queues.intr_vectors == IONIC_PROFILE_INTR_VECTORS_AUTO)
        snprintf(vectors, sizeof(vectors), "auto");
    else
        snprintf(vectors, sizeof(vectors), "%" PRIu32, p->queues.intr_vectors);
    return snprintf(
        buf, len,
        "[identity]\n"
        "asic_type = %s\n"
        "asic_rev = %u\n"
        "fw_version = %s\n"
        "\n"
        "[link]\n"
        "speed_mbps = %" PRIu32 "\n"
        "xcvr = %s\n"
        "mtu_max = %" PRIu32 "\n"
        "\n"
        "[queues]\n"
        "eth = %" PRIu32 "\n"
        "rdma_eqs = %" PRIu32 "\n"
        "intr_vectors = %s\n"
        "\n"
        "[stats]\n"
        "interval_ms = %" PRIu32 "\n",
        name_or(asic_names, N_ELEMS(asic_names), p->identity.asic_type),
        (unsigned int)p->identity.asic_rev, p->identity.fw_version,
        p->link.speed_mbps,
        name_or(xcvr_names, N_ELEMS(xcvr_names), p->link.xcvr), p->link.mtu_max,
        p->queues.eth, p->queues.rdma_eqs, vectors, p->stats.interval_ms);
}

uint64_t ionic_profile_hash(const struct ionic_profile *p)
{
    char buf[512];
    int n = format_values(p, buf, sizeof(buf));
    uint64_t h = 0xcbf29ce484222325ull; /* FNV-1a 64 offset basis */

    if (n < 0 || (size_t)n >= sizeof(buf))
        return 0;
    for (int i = 0; i < n; i++) {
        h ^= (uint8_t)buf[i];
        h *= 0x100000001b3ull; /* FNV-1a 64 prime */
    }
    return h;
}

int ionic_profile_format(const struct ionic_profile *p, char *buf, size_t len)
{
    char chain[IONIC_PROFILE_CHAIN_MAX];
    char values[512];
    int n;

    /* A file name could hold a newline, which would end the comment. */
    snprintf(chain, sizeof(chain), "%s", p->chain);
    for (char *c = chain; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f)
            *c = '?';
    n = format_values(p, values, sizeof(values));
    if (n < 0 || (size_t)n >= sizeof(values))
        return -1;
    return snprintf(buf, len,
                    "# NIC profile: %s\n"
                    "# hash: 0x%016" PRIx64 "\n"
                    "\n"
                    "%s",
                    chain, ionic_profile_hash(p), values);
}
