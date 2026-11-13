/*
 * nvmeof_target.c — in-process NVMe over Fabrics controller
 *
 * Implements the subset of NVMe 1.4 and NVMe-oF 1.1 that the Linux
 * nvme-rdma initiator exercises: Fabrics Connect and property access,
 * enough of the Admin command set to get through nvme_init_ctrl(), the
 * discovery log page so "nvme discover" works, and the NVM commands that
 * a filesystem actually issues.
 *
 * Two deliberate simplifications, both visible to the host through
 * Identify Controller so it never discovers them the hard way:
 *
 *   - ioccsz is 4 (64 bytes), so the host never uses in-capsule data for
 *     writes and every transfer arrives as a keyed SGL.  In-capsule data
 *     is still accepted if offered, because the admin Connect command's
 *     payload is the one case a host may send that way.
 *   - There is one controller record.  A second association reinitialises
 *     it, which is what a discover-then-connect sequence wants and what a
 *     reconnect after "nvme disconnect" wants.
 *
 * The namespace is an mmap: anonymous and lazily faulted by default, or
 * a shared mapping of a file when one is named.  Either way the bytes are
 * directly addressable, so reads and writes move data straight between
 * the mapping and guest memory with no bounce buffer.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "byteorder.h"
#include "ernic_err.h"
#include "ernic_log.h"
#include "nvmeof/nvmeof.h"
#include "nvmeof/nvmeof_kv.h"
#include "nvmeof/nvmeof_target.h"
#include "nvmeof/nvmeof_target_priv.h"
#include "net/net_headers.h"
#include "parse_int.h"

#define NVMEOF_DEFAULT_NQN        "nvmet-test"
#define NVMEOF_DEFAULT_SIZE       (UINT64_C(64) << 20)
#define NVMEOF_DEFAULT_BLOCK_SIZE 512U
#define NVMEOF_DEFAULT_PORT       4420
#define NVMEOF_DEFAULT_TRADDR     0xc0a8c801U /* 192.168.200.1 */
#define NVMEOF_DEFAULT_QUEUES     8
#define NVMEOF_DEFAULT_DEPTH      128

#define NVMEOF_CNTLID 1

/* Maximum Data Transfer Size, as 2^mdts pages of 4 KiB. */
#define NVMEOF_MDTS 5

/* What Identify Controller reports; see build_id_ctrl() */
#define NVMEOF_PCI_VENDOR_ID 0x1dd8 /* AMD Pensando */
#define NVMEOF_FW_REVISION   "1.0"
#define NVMEOF_RAB           6
#define NVMEOF_AERL          3
#define NVMEOF_KAS           10 /* 1 s, in 100 ms units */
#define NVMEOF_IOCCSZ        4  /* 64 bytes: no in-capsule data */
#define NVMEOF_IORCSZ        1  /* 16 bytes */
#define NVMEOF_MSDBD         1

/* CAP.TO: 7.5 seconds, in 500 ms units */
#define NVMEOF_CAP_TO 15U

/* A Get Log Page transfer larger than this is refused, 1 MiB */
#define NVMEOF_LOG_PAGE_MAX ((uint64_t)1 << 20)

/* The namespace UUID: bytes 6 and 8 carry its version and variant */
#define UUID_LEN         16
#define UUID_VERSION_OFF 6
#define UUID_VARIANT_OFF 8

struct nvmeof_ctrl {
    bool active;
    bool discovery;
    uint32_t cc;
    uint32_t csts;
    uint32_t kato;
    uint16_t num_io_queues;
    uint8_t hostid[16];
    char hostnqn[NVMEOF_NQN_FIELD];
};

struct nvmeof_queue {
    struct nvmeof_target *target;
    uint32_t handle;
    uint16_t qid;
    uint16_t sqsize;
    uint16_t sq_head;
    bool connected;
    bool in_use;
};

/*
 * A namespace's runtime state.  Exactly one of data (block) and kv (Key
 * Value) is live, and cfg.kv says which.
 */
struct nvmeof_ns {
    struct nvmeof_ns_cfg cfg; /* owns cfg.file */
    uint8_t *data;
    size_t len;
    int fd;               /* -1 for a RAM or KV namespace */
    struct nvmeof_kv *kv; /* non-NULL exactly when cfg.kv */
};

struct nvmeof_target {
    struct nvmeof_target_cfg cfg;
    struct nvmeof_target_stats stats;
    struct nvmeof_ctrl ctrl;

    struct nvmeof_ns ns[NVMEOF_MAX_NS];
    unsigned ns_count;

    uint64_t page_size;  /* msync() wants a page-aligned start */
    nvmeof_sync_fn sync; /* msync(), or a test's stand-in */

    struct nvmeof_queue *queues;
    unsigned queue_cap;
};

/*
 * The namespace with this id, or NULL for one the controller does not
 * have.  Linear because NVMEOF_MAX_NS is 8 and the list is walked once per
 * command; a map would cost more than it saves.
 */
static struct nvmeof_ns *ns_by_id(struct nvmeof_target *t, uint32_t nsid)
{
    for (unsigned i = 0; i < t->ns_count; i++) {
        if (t->ns[i].cfg.nsid == nsid) {
            return &t->ns[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

static void ns_cfg_defaults(struct nvmeof_ns_cfg *ns, uint32_t nsid)
{
    memset(ns, 0, sizeof(*ns));
    ns->nsid = nsid;
    ns->size = NVMEOF_DEFAULT_SIZE;
    ns->block_size = NVMEOF_DEFAULT_BLOCK_SIZE;
    ns->kv_max_keys = NVMEOF_KV_DEFAULT_KEYS;
    ns->kv_max_value = NVMEOF_KV_DEFAULT_VALUE;
}

void nvmeof_target_cfg_defaults(struct nvmeof_target_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* Each default fits its field. */
    (void)snprintf(cfg->subnqn, sizeof(cfg->subnqn), "%s", NVMEOF_DEFAULT_NQN);
    (void)snprintf(cfg->serial, sizeof(cfg->serial), "ERNIC0000000000001");
    (void)snprintf(cfg->model, sizeof(cfg->model), "rocm-ernic NVMe-oF target");
    cfg->traddr = NVMEOF_DEFAULT_TRADDR;
    cfg->trsvcid = NVMEOF_DEFAULT_PORT;
    cfg->max_queues = NVMEOF_DEFAULT_QUEUES;
    cfg->queue_depth = NVMEOF_DEFAULT_DEPTH;

    /* One block namespace, which is what "nvmeof:" with no options means. */
    cfg->ns_count = 1;
    ns_cfg_defaults(&cfg->ns[0], 1);
}

void nvmeof_target_cfg_free(struct nvmeof_target_cfg *cfg)
{
    for (unsigned i = 0; i < cfg->ns_count; i++) {
        free(cfg->ns[i].file);
        cfg->ns[i].file = NULL;
    }
}

/*
 * Whether @s fits an Identify Controller string field of @max bytes, which
 * holds ASCII padded with spaces: 1 to @max printable characters. Copying a
 * longer one would cut it short without saying so.
 */
static bool ident_string_ok(const char *s, size_t max)
{
    size_t n = strlen(s);

    if (n == 0 || n > max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < ' ' || c > '~') { /* printable ASCII */
            return false;
        }
    }
    return true;
}

static bool parse_ipv4(const char *s, uint32_t *out)
{
    struct in_addr a;

    if (inet_pton(AF_INET, s, &a) != 1) {
        return false;
    }
    *out = ntohl(a.s_addr);
    return true;
}

/*
 * Apply each key=value option in @opts to @cfg, stopping at the first bad
 * one, which @err names.  An option may be given more than once; the last
 * wins.  On failure @cfg may already hold some of the options.
 */
bool nvmeof_target_cfg_parse(struct nvmeof_target_cfg *cfg, const char *opts,
                             char *err, size_t errlen)
{
    if (opts == NULL || *opts == '\0') {
        return true;
    }

    /* strtok_r() writes NULs into the string it splits. */
    char *dup = strdup(opts);
    if (dup == NULL) {
        ernic_set_err(err, errlen, "out of memory");
        return false;
    }

    bool ok = true;
    char *save = NULL;
    /*
     * Per-namespace keys -- size, bs, file, kv, keys, maxval, nsid -- apply
     * to the namespace being defined.  "ns=N" opens another one, numbered
     * N, and everything after it describes that one.
     *
     * A separate key rather than a second nsid= because nsid= already means
     * "number the namespace I am describing", wherever it appears in the
     * string; making its second occurrence mean something else would change
     * what "size=1M,nsid=7" has always meant.  So:
     *
     *   kv=1,size=256M            one KV namespace, nsid 1   (unchanged)
     *   size=1M,nsid=7            one block namespace, nsid 7 (unchanged)
     *   size=1M,ns=2,kv=1,size=64K  block nsid 1 + KV nsid 2  (new)
     */
    struct nvmeof_ns_cfg *cur = &cfg->ns[0];
    /* strtok_r() skips empty fields, so "a=1,,b=2" is two options. */
    for (char *tok = strtok_r(dup, ",", &save); tok != NULL && ok;
         tok = strtok_r(NULL, ",", &save)) {
        /* Split at the first '=', so a value may itself contain one. */
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            ernic_set_err(err, errlen, "option '%s' is not key=value", tok);
            ok = false;
            break;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        /* Numbers go through parse_int.h, which refuses trailing text. */
        if (strcmp(key, "nqn") == 0) {
            /* NVMEOF_NQN_MAX counts the NUL; refuse rather than truncate. */
            if (*val == '\0' || strlen(val) >= NVMEOF_NQN_MAX) {
                ernic_set_err(err, errlen, "nqn must be 1..%d characters",
                              NVMEOF_NQN_MAX - 1);
                ok = false;
            } else {
                (void)snprintf(cfg->subnqn, sizeof(cfg->subnqn), "%s", val);
            }
        } else if (strcmp(key, "size") == 0) {
            if (!ernic_parse_size(val, &cur->size) || cur->size == 0) {
                ernic_set_err(err, errlen, "bad size '%s'", val);
                ok = false;
            } else {
                /* Lets size_image() tell size= from the default. */
                cur->size_set = true;
            }
        } else if (strcmp(key, "file") == 0) {
            if (*val == '\0') {
                ernic_set_err(err, errlen, "file needs a path");
                ok = false;
            } else {
                /* A repeated file= replaces the earlier path. */
                free(cur->file);
                cur->file = strdup(val);
                if (cur->file == NULL) {
                    ernic_set_err(err, errlen, "out of memory");
                    ok = false;
                }
            }
        } else if (strcmp(key, "bs") == 0) {
            uint32_t bs;
            if (!ernic_parse_u32(val, &bs) ||
                (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096)) {
                ernic_set_err(err, errlen,
                              "bs must be 512, 1024, 2048 or 4096 (got '%s')",
                              val);
                ok = false;
            } else {
                cur->block_size = bs;
            }
        } else if (strcmp(key, "nsid") == 0) {
            uint32_t v;
            /* NVMe reserves 0 (no namespace) and 0xffffffff (all). */
            if (!ernic_parse_u32(val, &v) || v == 0 || v == 0xffffffffU) {
                ernic_set_err(err, errlen, "bad nsid '%s'", val);
                ok = false;
            } else {
                /* Renames the namespace being defined; ns= is what opens
                 * another one. */
                cur->nsid = v;
            }
        } else if (strcmp(key, "ns") == 0) {
            uint32_t v;
            if (!ernic_parse_u32(val, &v) || v == 0 || v == 0xffffffffU) {
                ernic_set_err(err, errlen, "bad ns '%s'", val);
                ok = false;
            } else if (cfg->ns_count >= NVMEOF_MAX_NS) {
                ernic_set_err(err, errlen, "at most %d namespaces",
                              NVMEOF_MAX_NS);
                ok = false;
            } else {
                cur = &cfg->ns[cfg->ns_count++];
                ns_cfg_defaults(cur, v);
            }
        } else if (strcmp(key, "ip") == 0) {
            if (!parse_ipv4(val, &cfg->traddr)) {
                ernic_set_err(err, errlen, "bad IPv4 address '%s'", val);
                ok = false;
            }
        } else if (strcmp(key, "port") == 0) {
            uint32_t v;
            if (!ernic_parse_u32(val, &v) || v == 0 || v > 65535) {
                ernic_set_err(err, errlen, "bad port '%s'", val);
                ok = false;
            } else {
                cfg->trsvcid = (uint16_t)v;
            }
        } else if (strcmp(key, "queues") == 0) {
            uint32_t v;
            if (!ernic_parse_u32(val, &v) || v == 0 || v > NVMEOF_MAX_QUEUES) {
                /* A transport may allow fewer, and checks that itself. */
                ernic_set_err(err, errlen, "queues must be 1..%d (got '%s')",
                              NVMEOF_MAX_QUEUES, val);
                ok = false;
            } else {
                cfg->max_queues = (uint16_t)v;
            }
        } else if (strcmp(key, "kv") == 0) {
            uint32_t v = 0;
            if (!ernic_parse_u32(val, &v) || v > 1) {
                ernic_set_err(err, errlen, "kv must be 0 or 1 (got '%s')", val);
                ok = false;
            } else {
                cur->kv = v == 1;
            }
        } else if (strcmp(key, "keys") == 0) {
            uint32_t v = 0;
            if (!ernic_parse_u32(val, &v) || v == 0 || v > NVMEOF_KV_MAX_KEYS) {
                ernic_set_err(err, errlen, "keys must be 1..%u (got '%s')",
                              NVMEOF_KV_MAX_KEYS, val);
                ok = false;
            } else {
                cur->kv_max_keys = v;
            }
        } else if (strcmp(key, "maxval") == 0) {
            uint64_t v = 0;
            if (!ernic_parse_size(val, &v) || v == 0 || v > UINT32_MAX) {
                ernic_set_err(err, errlen, "bad maxval '%s'", val);
                ok = false;
            } else {
                cur->kv_max_value = (uint32_t)v;
            }
        } else if (strcmp(key, "model") == 0) {
            if (!ident_string_ok(val, sizeof(cfg->model) - 1)) {
                ernic_set_err(err, errlen,
                              "model must be 1..%zu printable ASCII "
                              "characters",
                              sizeof(cfg->model) - 1);
                ok = false;
            } else {
                (void)snprintf(cfg->model, sizeof(cfg->model), "%s", val);
            }
        } else if (strcmp(key, "serial") == 0) {
            if (!ident_string_ok(val, sizeof(cfg->serial) - 1)) {
                ernic_set_err(err, errlen,
                              "serial must be 1..%zu printable ASCII "
                              "characters",
                              sizeof(cfg->serial) - 1);
                ok = false;
            } else {
                (void)snprintf(cfg->serial, sizeof(cfg->serial), "%s", val);
            }
        } else {
            ernic_set_err(err, errlen, "unknown nvmeof option '%s'", key);
            ok = false;
        }
    }
    free(dup);

    /*
     * Checked after the loop because size= and bs= may come in either
     * order.  A file-backed namespace without size= gets its size from the
     * image later, and size_image() checks that one.
     */
    for (unsigned i = 0; ok && i < cfg->ns_count; i++) {
        const struct nvmeof_ns_cfg *n = &cfg->ns[i];

        /* A KV namespace stores values, not blocks, so "size" is a byte
         * capacity and owes the block size nothing.  Checking it anyway
         * would reject a perfectly good kv=1,size=100M. */
        if (!n->kv && n->size % n->block_size != 0) {
            ernic_set_err(err, errlen,
                          "nsid %u: size %" PRIu64
                          " is not a multiple of bs %u",
                          n->nsid, n->size, n->block_size);
            ok = false;
        } else if (n->kv && n->file != NULL) {
            /* The store is a hash table in anonymous memory; there is no
             * on-disk layout for it, and silently ignoring file= would lose
             * a user's data expectation rather than their option. */
            ernic_set_err(err, errlen,
                          "nsid %u: kv=1 has no file backing store; drop "
                          "file=",
                          n->nsid);
            ok = false;
        }

        /* Two namespaces with one id would make ns_by_id() arbitrary. */
        for (unsigned j = 0; ok && j < i; j++) {
            if (cfg->ns[j].nsid == n->nsid) {
                ernic_set_err(err, errlen, "nsid %u given twice", n->nsid);
                ok = false;
            }
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/*
 * Settle the namespace size against the image file, which may hold an
 * earlier run's data.  Without size=, an image that has data keeps its own
 * size; an empty or new one is sized to cfg.size.  A size= larger than the
 * image grows it, and one smaller is refused, as is an image that isn't a
 * whole number of blocks: either would lose the end of the data.
 */
static bool size_image(struct nvmeof_ns *ns, char *err, size_t errlen)
{
    struct stat st;

    if (fstat(ns->fd, &st) != 0) {
        ernic_set_err(err, errlen, "cannot stat %s: %s", ns->cfg.file,
                      strerror(errno));
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        ernic_set_err(err, errlen, "%s is not a regular file", ns->cfg.file);
        return false;
    }

    /* A regular file's st_size is never negative. */
    uint64_t have = (uint64_t)st.st_size;
    if (have != 0 && !ns->cfg.size_set) {
        if (have % ns->cfg.block_size != 0) {
            ernic_set_err(err, errlen,
                          "%s is %" PRIu64 " bytes, not a multiple of bs %u; "
                          "give a size= of whole blocks to grow it",
                          ns->cfg.file, have, ns->cfg.block_size);
            return false;
        }
        ns->cfg.size = have;
    } else if (have > ns->cfg.size) {
        ernic_set_err(err, errlen,
                      "%s is %" PRIu64 " bytes; size=%" PRIu64
                      " would truncate it",
                      ns->cfg.file, have, ns->cfg.size);
        return false;
    }

    /* ftruncate() takes the size as an off_t, which a size= past its range
     * would become negative in. */
    _Static_assert(sizeof(off_t) == sizeof(int64_t), "off_t is not 64 bits");
    if (ns->cfg.size > (uint64_t)INT64_MAX) {
        ernic_set_err(err, errlen,
                      "size=%" PRIu64 " is too large for a file; the most is "
                      "%" PRId64,
                      ns->cfg.size, INT64_MAX);
        return false;
    }

    ns->len = (size_t)ns->cfg.size;
    if (have < ns->cfg.size && ftruncate(ns->fd, (off_t)ns->len) != 0) {
        ernic_set_err(err, errlen, "cannot size %s to %zu bytes: %s",
                      ns->cfg.file, ns->len, strerror(errno));
        return false;
    }
    return true;
}

/*
 * Bring one namespace up.  A KV namespace has no block image to map: its
 * capacity lives in the store, whose entries are allocated as they arrive,
 * so mapping cfg.size bytes as well would reserve the capacity twice.
 */
static bool ns_create(struct nvmeof_ns *ns, const struct nvmeof_ns_cfg *cfg,
                      char *err, size_t errlen)
{
    ns->cfg = *cfg;
    ns->cfg.file = NULL;
    ns->fd = -1;
    ns->len = (size_t)cfg->size;

    if (cfg->kv) {
        ns->kv = nvmeof_kv_create(cfg->kv_max_keys, cfg->size);
        if (ns->kv == NULL) {
            ernic_set_err(err, errlen,
                          "nsid %u: cannot create a %u-key, %llu-byte KV store",
                          cfg->nsid, cfg->kv_max_keys,
                          (unsigned long long)cfg->size);
            return false;
        }
        ns->len = 0;
        return true;
    }

    if (cfg->file != NULL) {
        ns->cfg.file = strdup(cfg->file);
        if (ns->cfg.file == NULL) {
            ernic_set_err(err, errlen, "out of memory");
            return false;
        }
        /* With _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
         * open(path, flags, mode) overload, and android-cloexec-open looks
         * for O_CLOEXEC in its mode */
        /* NOLINTBEGIN(android-cloexec-open) */
        ns->fd =
            open(ns->cfg.file, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
        /* NOLINTEND(android-cloexec-open) */
        if (ns->fd < 0) {
            ernic_set_err(err, errlen, "cannot open %s: %s", ns->cfg.file,
                          strerror(errno));
            return false;
        }
        if (!size_image(ns, err, errlen)) {
            return false;
        }
        ns->data =
            mmap(NULL, ns->len, PROT_READ | PROT_WRITE, MAP_SHARED, ns->fd, 0);
    } else {
        ns->data = mmap(NULL, ns->len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    if (ns->data == MAP_FAILED) {
        ns->data = NULL;
        ernic_set_err(err, errlen,
                      "nsid %u: cannot map %zu bytes of namespace: %s",
                      cfg->nsid, ns->len, strerror(errno));
        return false;
    }
    return true;
}

struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen)
{
    struct nvmeof_target *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        ernic_set_err(err, errlen, "out of memory");
        return NULL;
    }

    t->cfg = *cfg;
    /* The namespaces own their paths; the controller copy must not alias
     * the caller's, which it is free to release once we return. */
    for (unsigned i = 0; i < NVMEOF_MAX_NS; i++) {
        t->cfg.ns[i].file = NULL;
        t->ns[i].fd = -1;
    }
    t->sync = msync;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        ernic_set_err(err, errlen, "cannot read the page size: %s",
                      strerror(errno));
        goto fail;
    }
    t->page_size = (uint64_t)page;

    t->ns_count = cfg->ns_count;
    for (unsigned i = 0; i < t->ns_count; i++) {
        if (!ns_create(&t->ns[i], &cfg->ns[i], err, errlen)) {
            goto fail;
        }
        /* The settled size, which an image file may have decided. */
        t->cfg.ns[i].size = t->ns[i].cfg.size;
    }

    t->queue_cap = (unsigned)cfg->max_queues + 1;
    t->queues = calloc(t->queue_cap, sizeof(*t->queues));
    if (t->queues == NULL) {
        ernic_set_err(err, errlen, "out of memory");
        goto fail;
    }

    return t;

fail:
    nvmeof_target_destroy(t);
    return NULL;
}

void nvmeof_target_destroy(struct nvmeof_target *t)
{
    if (t == NULL) {
        return;
    }
    for (unsigned i = 0; i < t->ns_count; i++) {
        struct nvmeof_ns *ns = &t->ns[i];
        if (ns->data != NULL) {
            munmap(ns->data, ns->len);
        }
        if (ns->fd >= 0) {
            close(ns->fd);
        }
        nvmeof_kv_destroy(ns->kv);
        free(ns->cfg.file);
    }
    free(t->queues);
    free(t);
}

const struct nvmeof_target_cfg *nvmeof_target_config(
    const struct nvmeof_target *t)
{
    return &t->cfg;
}

const struct nvmeof_target_stats *nvmeof_target_stats(
    const struct nvmeof_target *t)
{
    return &t->stats;
}

void nvmeof_target_set_sync(struct nvmeof_target *t, nvmeof_sync_fn fn)
{
    t->sync = fn;
}

/* ------------------------------------------------------------------ */
/* Queues                                                             */
/* ------------------------------------------------------------------ */

struct nvmeof_queue *nvmeof_target_find_queue(struct nvmeof_target *t,
                                              uint32_t handle)
{
    for (unsigned i = 0; i < t->queue_cap; i++) {
        if (t->queues[i].in_use && t->queues[i].handle == handle) {
            return &t->queues[i];
        }
    }
    return NULL;
}

struct nvmeof_queue *nvmeof_target_open_queue(struct nvmeof_target *t,
                                              uint32_t handle)
{
    struct nvmeof_queue *q = nvmeof_target_find_queue(t, handle);
    if (q != NULL) {
        return q;
    }
    for (unsigned i = 0; i < t->queue_cap; i++) {
        if (!t->queues[i].in_use) {
            q = &t->queues[i];
            memset(q, 0, sizeof(*q));
            q->target = t;
            q->handle = handle;
            q->in_use = true;
            return q;
        }
    }
    return NULL;
}

void nvmeof_target_close_queue(struct nvmeof_target *t, uint32_t handle)
{
    struct nvmeof_queue *q = nvmeof_target_find_queue(t, handle);
    if (q == NULL) {
        return;
    }
    bool was_admin = q->connected && q->qid == 0;
    memset(q, 0, sizeof(*q));

    /* Losing the admin queue tears down the association. */
    if (was_admin) {
        memset(&t->ctrl, 0, sizeof(t->ctrl));
        for (unsigned i = 0; i < t->queue_cap; i++) {
            memset(&t->queues[i], 0, sizeof(t->queues[i]));
        }
    }
}

unsigned nvmeof_target_queue_count(const struct nvmeof_target *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < t->queue_cap; i++) {
        n += t->queues[i].in_use ? 1 : 0;
    }
    return n;
}

bool nvmeof_queue_connected(const struct nvmeof_queue *q)
{
    return q->connected;
}

uint16_t nvmeof_queue_id(const struct nvmeof_queue *q)
{
    return q->qid;
}

/* ------------------------------------------------------------------ */
/* SGL handling                                                       */
/* ------------------------------------------------------------------ */

struct nvmeof_sgl {
    uint64_t addr;
    uint32_t length;
    uint32_t key;
    bool keyed;
    bool in_capsule;
};

static bool parse_sgl(const uint8_t *sqe, struct nvmeof_sgl *sgl)
{
    const uint8_t *d = sqe + NVME_SQE_DPTR_OFF;
    uint8_t type = d[NVME_SGL_TYPE_OFF] >> NVME_SGL_TYPE_SHIFT;
    uint8_t subtype = d[NVME_SGL_TYPE_OFF] & 0xf;

    memset(sgl, 0, sizeof(*sgl));
    sgl->addr = ernic_get_le64(d + NVME_SGL_ADDR_OFF);

    switch (type) {
    case NVME_SGL_TYPE_KEYED_DATA:
        sgl->keyed = true;
        /* le24, so the key's first byte is masked off */
        sgl->length = ernic_get_le32(d + NVME_SGL_LENGTH_OFF) & 0x00ffffffU;
        sgl->key = ernic_get_le32(d + NVME_SGL_KEY_OFF);
        return true;
    case NVME_SGL_TYPE_DATA_BLOCK:
    case NVME_SGL_TYPE_TRANSPORT_DATA:
        sgl->length = ernic_get_le32(d + NVME_SGL_LENGTH_OFF);
        sgl->in_capsule = (subtype == NVME_SGL_SUBTYPE_OFFSET) ||
                          (type == NVME_SGL_TYPE_TRANSPORT_DATA);
        return true;
    default:
        return false;
    }
}

/*
 * Move len bytes from the controller buffer src out to wherever the
 * command's SGL points.  In-capsule descriptors are input-only, so a
 * controller-to-host transfer through one is a malformed request.
 *
 * Only a keyed descriptor names host memory over RDMA.  A data block that
 * isn't in capsule carries a bare address and no key, and handing it to
 * the DMA ops with the key left at 0 would ask for that address as a raw
 * guest bus address, so it is refused here, in both directions.
 */
static uint16_t sgl_write(const struct nvmeof_sgl *sgl,
                          const struct nvmeof_dma_ops *dma, void *ctx,
                          const void *src, uint32_t len)
{
    if (len == 0) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    if (!sgl->keyed) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }
    if (sgl->length < len) {
        len = sgl->length;
    }
    if (len == 0) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
    }
    if (dma->to_host(ctx, sgl->key, sgl->addr, src, len) != len) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_DATA_XFER_ERROR, 1);
    }
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t sgl_read(const struct nvmeof_sgl *sgl, const uint8_t *capsule,
                         size_t capsule_len, const struct nvmeof_dma_ops *dma,
                         void *ctx, void *dst, uint32_t len)
{
    if (len == 0) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    if (sgl->length < len) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
    }
    if (sgl->in_capsule) {
        /*
         * addr is an offset past the 64-byte SQE, and the host's to choose:
         * added to the SQE's size first, one near 2^64 wrapped to a small
         * offset and read the command itself as its data.  The caller has
         * checked that the capsule holds an SQE.
         */
        size_t data_len = capsule_len - NVME_SQE_SIZE;
        if (sgl->addr > data_len || data_len - sgl->addr < len) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
        }
        memcpy(dst, capsule + NVME_SQE_SIZE + sgl->addr, len);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    if (!sgl->keyed) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }
    if (dma->from_host(ctx, sgl->key, sgl->addr, dst, len) != len) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_DATA_XFER_ERROR, 1);
    }
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

/* ------------------------------------------------------------------ */
/* Identify / log pages                                               */
/* ------------------------------------------------------------------ */

/* Pad with spaces rather than NULs: NVMe ASCII fields are not strings. */
static void ascii_field(uint8_t *dst, size_t len, const char *src)
{
    size_t n = strnlen(src, len);
    memcpy(dst, src, n);
    memset(dst + n, ' ', len - n);
}

static uint8_t lba_shift(uint32_t block_size)
{
    uint8_t s = 0;
    while ((1U << s) < block_size) {
        s++;
    }
    return s;
}

static void build_id_ctrl(const struct nvmeof_target *t, uint8_t *buf)
{
    const struct nvmeof_target_cfg *c = &t->cfg;

    memset(buf, 0, NVME_IDENTIFY_SIZE);
    ernic_put_le16(buf + NVME_ID_CTRL_VID_OFF, NVMEOF_PCI_VENDOR_ID);
    ernic_put_le16(buf + NVME_ID_CTRL_SSVID_OFF, NVMEOF_PCI_VENDOR_ID);
    ascii_field(buf + NVME_ID_CTRL_SN_OFF, NVME_ID_CTRL_SN_LEN, c->serial);
    ascii_field(buf + NVME_ID_CTRL_MN_OFF, NVME_ID_CTRL_MN_LEN, c->model);
    ascii_field(buf + NVME_ID_CTRL_FR_OFF, NVME_ID_CTRL_FR_LEN,
                NVMEOF_FW_REVISION);
    buf[NVME_ID_CTRL_RAB_OFF] = NVMEOF_RAB;
    buf[NVME_ID_CTRL_MDTS_OFF] = NVMEOF_MDTS;
    ernic_put_le16(buf + NVME_ID_CTRL_CNTLID_OFF, NVMEOF_CNTLID);
    ernic_put_le32(buf + NVME_ID_CTRL_VER_OFF, NVME_VERSION_1_4);

    buf[NVME_ID_CTRL_CNTRLTYPE_OFF] =
        t->ctrl.discovery ? NVME_CTRL_TYPE_DISCOVERY : NVME_CTRL_TYPE_IO;

    ernic_put_le16(buf + NVME_ID_CTRL_OACS_OFF, 0);
    buf[NVME_ID_CTRL_AERL_OFF] = NVMEOF_AERL;
    /* The effects log, and no extended data */
    buf[NVME_ID_CTRL_LPA_OFF] = NVME_LPA_CMD_EFFECTS;
    buf[NVME_ID_CTRL_ELPE_OFF] = 0;

    /* Keep Alive is mandatory over fabrics.  KAS is in 100ms units, so this
     * is a 1s granularity, matching what nvmet advertises; the host's default
     * KATO is 5s. */
    ernic_put_le16(buf + NVME_ID_CTRL_KAS_OFF, NVMEOF_KAS);

    buf[NVME_ID_CTRL_SQES_OFF] = NVME_ID_CTRL_SQES;
    buf[NVME_ID_CTRL_CQES_OFF] = NVME_ID_CTRL_CQES;
    ernic_put_le16(buf + NVME_ID_CTRL_MAXCMD_OFF, t->cfg.queue_depth);

    /* NN is the largest valid namespace id, not the count: the host uses
     * it to bound its scan, and an id above it is never probed. */
    uint32_t nn = 0;
    for (unsigned i = 0; i < t->ns_count; i++) {
        if (t->ns[i].cfg.nsid > nn) {
            nn = t->ns[i].cfg.nsid;
        }
    }
    ernic_put_le32(buf + NVME_ID_CTRL_NN_OFF, t->ctrl.discovery ? 0 : nn);
    ernic_put_le16(buf + NVME_ID_CTRL_ONCS_OFF, NVME_ONCS_WRITE_ZEROES);
    buf[NVME_ID_CTRL_FNA_OFF] = 0;

    /*
     * vwc: a file-backed namespace has one, the host page cache, so say so
     * (bit 0) or the host never sends Flush or sets FUA.  Bits 2:1 = 10b:
     * Flush needs the namespace's own NSID, not the broadcast one.  A RAM
     * namespace has no stable media to write back to.
     */
    bool file_backed = false;
    for (unsigned i = 0; i < t->ns_count; i++) {
        if (t->ns[i].fd >= 0) {
            file_backed = true;
        }
    }
    buf[NVME_ID_CTRL_VWC_OFF] =
        file_backed ? NVME_VWC_PRESENT | NVME_VWC_NO_FLUSH_ALL : 0;

    /*
     * sgls: supported (bit 0), keyed SGLs (bit 2, which nvme-rdma treats
     * as mandatory), and SGL address-as-offset (bit 20) for in-capsule.
     */
    ernic_put_le32(buf + NVME_ID_CTRL_SGLS_OFF,
                   NVME_SGLS_SUPPORTED | NVME_SGLS_KEYED | NVME_SGLS_OFFSET);

    /*
     * A discovery association has to name the discovery NQN here.  Linux
     * builds subsys->subnqn from this field and then rejects the controller
     * outright -- "Subsystem %s is not a discovery controller", EINVAL -- if
     * it says CNTRLTYPE is discovery while the NQN is an ordinary subsystem's.
     * Both fit: configuration refuses a longer NQN.
     */
    (void)snprintf((char *)buf + NVME_ID_CTRL_SUBNQN_OFF, NVMEOF_NQN_FIELD,
                   "%s", t->ctrl.discovery ? NVMEOF_DISCOVERY_NQN : c->subnqn);

    ernic_put_le32(buf + NVME_ID_CTRL_IOCCSZ_OFF, NVMEOF_IOCCSZ);
    ernic_put_le32(buf + NVME_ID_CTRL_IORCSZ_OFF, NVMEOF_IORCSZ);
    ernic_put_le16(buf + NVME_ID_CTRL_ICDOFF_OFF, 0);
    buf[NVME_ID_CTRL_FCATT_OFF] = 0;
    buf[NVME_ID_CTRL_MSDBD_OFF] = NVMEOF_MSDBD;
}

static void build_id_ns(const struct nvmeof_ns *ns, uint8_t *buf)
{
    uint64_t blocks = ns->cfg.size / ns->cfg.block_size;

    memset(buf, 0, NVME_IDENTIFY_SIZE);
    ernic_put_le64(buf + NVME_ID_NS_NSZE_OFF, blocks);
    ernic_put_le64(buf + NVME_ID_NS_NCAP_OFF, blocks);
    ernic_put_le64(buf + NVME_ID_NS_NUSE_OFF, blocks);
    buf[NVME_ID_NS_NSFEAT_OFF] = 0;
    buf[NVME_ID_NS_NLBAF_OFF] = 0; /* one format */
    buf[NVME_ID_NS_FLBAS_OFF] = 0; /* format 0 */
    buf[NVME_ID_NS_MC_OFF] = 0;
    buf[NVME_ID_NS_DPC_OFF] = 0;
    buf[NVME_ID_NS_DPS_OFF] = 0;

    /* LBA format 0: ms = 0, lbads = log2(block size), rp = best. */
    uint8_t *lbaf = buf + NVME_ID_NS_LBAF0_OFF;
    ernic_put_le16(lbaf + NVME_LBAF_MS_OFF, 0);
    lbaf[NVME_LBAF_LBADS_OFF] = lba_shift(ns->cfg.block_size);
    lbaf[NVME_LBAF_RP_OFF] = 0;
}

/*
 * Namespace Identification Descriptor list.  A UUID derived from the
 * subsystem NQN and the namespace ID keeps udev's by-id links stable
 * across restarts of the same server configuration.
 */
static void build_ns_desc(const struct nvmeof_target *t,
                          const struct nvmeof_ns *ns, uint8_t *buf)
{
    memset(buf, 0, NVME_IDENTIFY_SIZE);
    buf[NVME_NS_DESC_NIDT_OFF] = NVME_NIDT_UUID;
    buf[NVME_NS_DESC_NIDL_OFF] = NVME_NIDL_UUID;

    uint8_t *uuid = buf + NVME_NS_DESC_NID_OFF;
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    const char *p = t->cfg.subnqn;
    while (*p != '\0') {
        h = (h ^ (uint8_t)*p++) * UINT64_C(0x100000001b3);
    }
    h ^= ns->cfg.nsid;
    for (int i = 0; i < UUID_LEN; i++) {
        /* The hash's eight bytes, twice over, each mixed with i in both
         * nibbles */
        uuid[i] = (uint8_t)(h >> ((i % 8) * 8)) ^ (uint8_t)((i << 4) | i);
    }
    uuid[UUID_VERSION_OFF] =
        (uint8_t)((uuid[UUID_VERSION_OFF] & 0x0f) | 0x40); /* version 4 */
    uuid[UUID_VARIANT_OFF] =
        (uint8_t)((uuid[UUID_VARIANT_OFF] & 0x3f) | 0x80); /* variant 1 */

    /*
     * A Command Set Identifier descriptor after the UUID, on EVERY
     * namespace.  This one byte is what the host reads to decide which
     * command set the namespace speaks.
     *
     * For a KV namespace it is what makes Linux log "block device for nsid
     * %u not supported (csi 1)", hide the block device and keep the
     * namespace, so the guest gets /dev/ngXnY to issue KV commands through.
     *
     * For a block namespace it is not optional either, and that is less
     * obvious: once any namespace here speaks a second command set, CAP.CSS
     * bit 6 makes the host select CC.CSS = all-supported, and in that mode
     * it takes every namespace's command set from this descriptor rather
     * than assuming NVM.  Omitting it on the block namespace is exactly
     * what "Command set not reported for nsid:1" means, and the namespace
     * is then dropped.  Saying NVM explicitly costs nothing when the
     * controller has no KV namespace at all.
     */
    uint8_t *csi = uuid + NVME_NIDL_UUID;
    csi[NVME_NS_DESC_NIDT_OFF] = NVME_NIDT_CSI;
    csi[NVME_NS_DESC_NIDL_OFF] = NVME_NIDL_CSI;
    csi[NVME_NS_DESC_NID_OFF] = ns->cfg.kv ? NVME_CSI_KV : NVME_CSI_NVM;
}

/*
 * I/O Command Set Independent Identify Namespace (CNS 08h).  Everything
 * here is true of a namespace whatever command set it speaks, which is why
 * the host asks for it in place of CNS 00h once CC.CSS is all-supported.
 * Without an answer the namespace is skipped before its command set is ever
 * considered, and nothing is logged about why.
 */
static void build_id_ns_cs_indep(const struct nvmeof_ns *ns, uint8_t *buf)
{
    memset(buf, 0, NVME_IDENTIFY_SIZE);
    buf[NVME_ID_NS_CSI_NSFEAT_OFF] = 0;
    buf[NVME_ID_NS_CSI_NMIC_OFF] = 0;   /* no multipath */
    buf[NVME_ID_NS_CSI_RESCAP_OFF] = 0; /* no reservations */
    buf[NVME_ID_NS_CSI_FPI_OFF] = 0;
    ernic_put_le32(buf + NVME_ID_NS_CSI_ANAGRPID_OFF, 0);
    buf[NVME_ID_NS_CSI_NSATTR_OFF] = 0;
    ernic_put_le16(buf + NVME_ID_NS_CSI_NVMSETID_OFF, 0);
    ernic_put_le16(buf + NVME_ID_NS_CSI_ENDGID_OFF, 0);
    /*
     * NSTAT bit 0, NRDY.  A fabrics host treats a namespace that does not
     * say it is ready as one to retry later, so a zero here reads as a
     * device still coming up rather than one that is simply present.
     */
    buf[NVME_ID_NS_CSI_NSTAT_OFF] = NVME_NSTAT_NRDY;
    (void)ns;
}

static void build_discovery_log(const struct nvmeof_target *t, uint8_t *buf,
                                size_t len)
{
    memset(buf, 0, len);
    if (len < NVMEOF_DISC_HDR_SIZE) {
        return;
    }
    ernic_put_le64(buf + NVMEOF_DISC_GENCTR_OFF, 1);
    ernic_put_le64(buf + NVMEOF_DISC_NUMREC_OFF, 1);
    ernic_put_le16(buf + NVMEOF_DISC_RECFMT_OFF, 0);

    if (len < NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE) {
        return;
    }
    uint8_t *e = buf + NVMEOF_DISC_HDR_SIZE;
    e[NVMEOF_DISC_E_TRTYPE_OFF] = NVMEOF_TRTYPE_RDMA;
    e[NVMEOF_DISC_E_ADRFAM_OFF] = NVMEOF_ADRFAM_IPV4;
    e[NVMEOF_DISC_E_SUBTYPE_OFF] = NVMEOF_SUBTYPE_NVME;
    e[NVMEOF_DISC_E_TREQ_OFF] = 0;
    ernic_put_le16(e + NVMEOF_DISC_E_PORTID_OFF, 1);
    ernic_put_le16(e + NVMEOF_DISC_E_CNTLID_OFF, 0xffff); /* dynamic */
    ernic_put_le16(e + NVMEOF_DISC_E_ASQSZ_OFF, t->cfg.queue_depth);
    /* A port number and a configured NQN fit. */
    (void)snprintf((char *)e + NVMEOF_DISC_E_TRSVCID_OFF, 32, "%u",
                   t->cfg.trsvcid);
    (void)snprintf((char *)e + NVMEOF_DISC_E_SUBNQN_OFF, 256, "%s",
                   t->cfg.subnqn);
    ip_addr_str(t->cfg.traddr, (char *)e + NVMEOF_DISC_E_TRADDR_OFF);

    uint8_t *tsas = e + NVMEOF_DISC_E_TSAS_OFF;
    tsas[0] = NVMEOF_RDMA_QPTYPE_CONNECTED;
    tsas[1] = NVMEOF_RDMA_PRTYPE_NONE;
    tsas[2] = NVMEOF_RDMA_CMS_RDMA_CM;
    ernic_put_le16(tsas + 8, 0xffff); /* pkey */
}

/* ------------------------------------------------------------------ */
/* Fabrics commands                                                   */
/* ------------------------------------------------------------------ */

static uint64_t prop_cap(const struct nvmeof_target *t)
{
    uint64_t mqes = t->cfg.queue_depth > 0 ? t->cfg.queue_depth - 1U : 0;
    /*
     * CAP.CSS is a bitmap at bits 44:37.  Bit 0 is the NVM command set; bit
     * 6 says one or more *other* I/O command sets are supported, and
     * selecting one then happens through CC.CSS and Identify's CSI.
     *
     * A KV namespace needs bit 6, and the host enforces it: Linux sets
     * CC.CSS to NVME_CC_CSS_CSI only when it sees bit 6, and nvme_scan_ns()
     * then refuses any namespace whose CSI is not NVM with "command set not
     * reported for nsid: %d" -- which it reports even when the Identify
     * descriptor list named the command set perfectly well, because the
     * controller never said it could have one.
     */
    bool any_kv = false;
    for (unsigned i = 0; i < t->ns_count; i++) {
        if (t->ns[i].cfg.kv) {
            any_kv = true;
        }
    }
    /*
     * The NVM bit always: the controller understands the NVM command set
     * whether or not a namespace currently uses it.  The other-sets bit as
     * well once any namespace speaks another one, which makes Linux select
     * CC.CSS = 110b ("all supported I/O command sets") and then take each
     * namespace's command set from its own CSI descriptor.  That
     * per-namespace selection is what lets block and KV namespaces coexist
     * here; with it clear the host would read every namespace as NVM.
     */
    uint64_t css = NVME_CAP_CSS_NVM | (any_kv ? NVME_CAP_CSS_OTHER : 0);
    return mqes | NVME_CAP_CQR |
           ((uint64_t)NVMEOF_CAP_TO << NVME_CAP_TO_SHIFT) | css;
}

static uint16_t do_connect(struct nvmeof_queue *q, const uint8_t *sqe,
                           const uint8_t *capsule, size_t capsule_len,
                           const struct nvmeof_dma_ops *dma, void *ctx,
                           uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint16_t recfmt = ernic_get_le16(sqe + NVMEOF_CONNECT_RECFMT_OFF);
    uint16_t qid = ernic_get_le16(sqe + NVMEOF_CONNECT_QID_OFF);
    uint16_t sqsize = ernic_get_le16(sqe + NVMEOF_CONNECT_SQSIZE_OFF);
    uint32_t kato = ernic_get_le32(sqe + NVMEOF_CONNECT_KATO_OFF);

    if (recfmt != 0) {
        return nvme_status(NVME_SCT_CMD_SPECIFIC, NVMEOF_SC_CONNECT_FORMAT, 1);
    }
    if (q->connected) {
        return nvme_status(NVME_SCT_CMD_SPECIFIC, NVMEOF_SC_CONNECT_CTRL_BUSY,
                           1);
    }
    if (sqsize == 0 || sqsize >= t->cfg.queue_depth) {
        return nvme_status(NVME_SCT_CMD_SPECIFIC,
                           NVMEOF_SC_CONNECT_INVALID_PARAM, 1);
    }
    if (qid > t->cfg.max_queues) {
        return nvme_status(NVME_SCT_CMD_SPECIFIC,
                           NVMEOF_SC_CONNECT_INVALID_PARAM, 1);
    }

    struct nvmeof_sgl sgl;
    if (!parse_sgl(sqe, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }

    uint8_t data[NVMEOF_CONNECT_DATA_SIZE];
    uint16_t st =
        sgl_read(&sgl, capsule, capsule_len, dma, ctx, data, sizeof(data));
    if (st != 0) {
        return st;
    }

    char subnqn[NVMEOF_NQN_FIELD];
    char hostnqn[NVMEOF_NQN_FIELD];
    memcpy(subnqn, data + NVMEOF_CONNECT_SUBNQN_OFF, sizeof(subnqn) - 1);
    memcpy(hostnqn, data + NVMEOF_CONNECT_HOSTNQN_OFF, sizeof(hostnqn) - 1);
    subnqn[sizeof(subnqn) - 1] = '\0';
    hostnqn[sizeof(hostnqn) - 1] = '\0';

    bool discovery = strcmp(subnqn, NVMEOF_DISCOVERY_NQN) == 0;
    if (!discovery && strcmp(subnqn, t->cfg.subnqn) != 0) {
        return nvme_status(NVME_SCT_CMD_SPECIFIC,
                           NVMEOF_SC_CONNECT_INVALID_PARAM, 1);
    }

    if (qid == 0) {
        /*
         * A fresh admin Connect starts a new association; drop whatever
         * the previous one left behind rather than refusing the host.
         */
        for (unsigned i = 0; i < t->queue_cap; i++) {
            if (&t->queues[i] != q) {
                memset(&t->queues[i], 0, sizeof(t->queues[i]));
            }
        }
        memset(&t->ctrl, 0, sizeof(t->ctrl));
        t->ctrl.active = true;
        t->ctrl.discovery = discovery;
        t->ctrl.kato = kato;
        t->ctrl.num_io_queues = t->cfg.max_queues;
        memcpy(t->ctrl.hostid, data + NVMEOF_CONNECT_HOSTID_OFF, 16);
        /* hostnqn is NUL-terminated within a buffer of the same size. */
        (void)snprintf(t->ctrl.hostnqn, sizeof(t->ctrl.hostnqn), "%s", hostnqn);
    } else {
        if (!t->ctrl.active) {
            return nvme_status(NVME_SCT_CMD_SPECIFIC,
                               NVMEOF_SC_CONNECT_INVALID_PARAM, 1);
        }
        if (strcmp(t->ctrl.hostnqn, hostnqn) != 0) {
            return nvme_status(NVME_SCT_CMD_SPECIFIC,
                               NVMEOF_SC_CONNECT_INVALID_HOST, 1);
        }
    }

    q->qid = qid;
    q->sqsize = sqsize;
    q->sq_head = 0;
    q->connected = true;
    t->stats.connects++;

    /* Connect returns the controller ID in the low half of the result. */
    ernic_put_le16(rsp + NVME_CQE_RESULT_OFF, NVMEOF_CNTLID);
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t do_property_get(struct nvmeof_queue *q, const uint8_t *sqe,
                                uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint8_t attrib = sqe[NVMEOF_PROP_ATTRIB_OFF];
    uint32_t off = ernic_get_le32(sqe + NVMEOF_PROP_OFFSET_OFF);
    uint64_t v;

    switch (off) {
    case NVMEOF_PROP_CAP:
        if (attrib != 1) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        }
        v = prop_cap(t);
        break;
    case NVMEOF_PROP_VS:
        v = NVME_VERSION_1_4;
        break;
    case NVMEOF_PROP_CC:
        v = t->ctrl.cc;
        break;
    case NVMEOF_PROP_CSTS:
        v = t->ctrl.csts;
        break;
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    ernic_put_le64(rsp + NVME_CQE_RESULT_OFF, v);
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

/*
 * Write [off, off + len) of a file-backed namespace through to stable
 * media, for Flush, FUA and a shutdown.  msync(), not fdatasync(): POSIX
 * only promises that stores to a shared mapping reach the file through
 * msync().  It blocks the caller until writeback is done, and leaves errno
 * set if it fails.
 *
 * A failure sets DNR: Linux reports a writeback error once and may already
 * have dropped the dirty pages, so a retry could succeed with the data lost.
 */
static uint16_t ns_sync(const struct nvmeof_target *t,
                        const struct nvmeof_ns *ns, uint64_t off, uint64_t len)
{
    if (ns == NULL || ns->fd < 0) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    /* ns->data is page-aligned, being an mmap(). */
    uint64_t start = off - (off % t->page_size);
    if (t->sync(ns->data + start, (size_t)(off + len - start), MS_SYNC) != 0) {
        return nvme_status(NVME_SCT_MEDIA, NVME_SC_WRITE_FAULT, 1);
    }
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t do_property_set(const struct nvmeof_queue *q,
                                const uint8_t *sqe)
{
    struct nvmeof_target *t = q->target;
    uint32_t off = ernic_get_le32(sqe + NVMEOF_PROP_OFFSET_OFF);
    uint64_t val = ernic_get_le64(sqe + NVMEOF_PROP_VALUE_OFF);

    if (off != NVMEOF_PROP_CC) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    bool was_enabled = (t->ctrl.cc & NVME_CC_ENABLE) != 0;
    uint32_t old_shn = t->ctrl.cc & NVME_CC_SHN_MASK;
    t->ctrl.cc = (uint32_t)val;
    uint32_t shn = t->ctrl.cc & NVME_CC_SHN_MASK;

    /*
     * A shutdown notification tells the controller power may go once it
     * reports the shutdown complete, and a host needn't send Flush first,
     * so the volatile write cache is written back before CSTS.SHST says so.
     * Only a write that sets CC.SHN does it, not one that leaves it set.
     * A Property Set has no status that fits a failed writeback, so log it
     * and complete the shutdown anyway.
     */
    if (shn != 0 && old_shn == 0) {
        /* Every file-backed namespace, and each one named in its own
         * message: with more than one, "writing back failed" without the
         * nsid does not say which image is at risk. */
        for (unsigned i = 0; i < t->ns_count; i++) {
            const struct nvmeof_ns *ns = &t->ns[i];
            if (ns_sync(t, ns, 0, ns->len) != 0) {
                ernic_log(ERNIC_LOG_ERROR,
                          "nvmeof: nsid %u: writing back %s for a shutdown "
                          "failed: %s",
                          ns->cfg.nsid, ns->cfg.file, strerror(errno));
            }
        }
    }

    if (t->ctrl.cc & NVME_CC_ENABLE) {
        /*
         * A host shuts down by setting CC.SHN with CC.EN still set, and the
         * shutdown stays reported until a reset clears CC.EN. Enabling the
         * controller starts afresh: a shutdown reported while it was disabled
         * doesn't carry over.
         */
        t->ctrl.csts = NVME_CSTS_RDY |
                       (was_enabled ? t->ctrl.csts & NVME_CSTS_SHST_MASK : 0);
        if (shn != 0) {
            t->ctrl.csts |= NVME_CSTS_SHST_CMPLT;
        }
    } else {
        /* Shutdown or disable: report both not-ready and shutdown done. */
        t->ctrl.csts = shn != 0 ? NVME_CSTS_SHST_CMPLT : 0;
    }
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t exec_fabrics(struct nvmeof_queue *q, const uint8_t *capsule,
                             size_t capsule_len,
                             const struct nvmeof_dma_ops *dma, void *ctx,
                             uint8_t *rsp)
{
    uint8_t fctype = capsule[NVMEOF_SQE_FCTYPE_OFF];

    if (fctype != NVMEOF_FCTYPE_CONNECT && !q->connected) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
    }

    switch (fctype) {
    case NVMEOF_FCTYPE_CONNECT:
        return do_connect(q, capsule, capsule, capsule_len, dma, ctx, rsp);
    case NVMEOF_FCTYPE_PROPERTY_GET:
        return do_property_get(q, capsule, rsp);
    case NVMEOF_FCTYPE_PROPERTY_SET:
        return do_property_set(q, capsule);
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Admin commands                                                     */
/* ------------------------------------------------------------------ */

static uint16_t do_identify(const struct nvmeof_queue *q, const uint8_t *sqe,
                            const struct nvmeof_dma_ops *dma, void *ctx)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = ernic_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t nsid = ernic_get_le32(sqe + NVME_SQE_NSID_OFF);
    uint8_t cns = cdw10 & 0xff;
    uint8_t buf[NVME_IDENTIFY_SIZE];

    struct nvmeof_sgl sgl;
    if (!parse_sgl(sqe, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }

    switch (cns) {
    case NVME_ID_CNS_CTRL:
        build_id_ctrl(t, buf);
        break;
    case NVME_ID_CNS_NS: {
        const struct nvmeof_ns *ns = ns_by_id(t, nsid);
        if (ns == NULL || ns->cfg.kv) {
            /* Identify Namespace is NVM-command-set data.  A KV namespace
             * has none, and answering with block geometry would describe a
             * capacity and an LBA format that do not exist. */
            memset(buf, 0, sizeof(buf));
        } else {
            build_id_ns(ns, buf);
        }
        break;
    }
    case NVME_ID_CNS_NS_ACTIVE_LIST: {
        memset(buf, 0, sizeof(buf));
        if (t->ctrl.discovery) {
            break;
        }
        /*
         * Ascending, and strictly greater than the nsid asked about: the
         * host walks the list by passing back the last id it saw, so an
         * unsorted list would make it stop early or loop.
         */
        uint32_t ids[NVMEOF_MAX_NS];
        unsigned n = 0;
        for (unsigned i = 0; i < t->ns_count; i++) {
            uint32_t id = t->ns[i].cfg.nsid;
            if (id <= nsid) {
                continue;
            }
            unsigned j = n++;
            while (j > 0 && ids[j - 1] > id) {
                ids[j] = ids[j - 1];
                j--;
            }
            ids[j] = id;
        }
        for (unsigned i = 0; i < n; i++) {
            ernic_put_le32(buf + ((size_t)i * 4), ids[i]);
        }
        break;
    }
    case NVME_ID_CNS_NS_CS_INDEP: {
        const struct nvmeof_ns *ns = ns_by_id(t, nsid);
        if (ns == NULL) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NS, 1);
        }
        build_id_ns_cs_indep(ns, buf);
        break;
    }
    case NVME_ID_CNS_NS_DESC_LIST: {
        const struct nvmeof_ns *ns = ns_by_id(t, nsid);
        if (ns == NULL) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NS, 1);
        }
        build_ns_desc(t, ns, buf);
        break;
    }
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    return sgl_write(&sgl, dma, ctx, buf, sizeof(buf));
}

static uint16_t do_get_log_page(const struct nvmeof_queue *q,
                                const uint8_t *sqe,
                                const struct nvmeof_dma_ops *dma, void *ctx)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = ernic_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t cdw11 = ernic_get_le32(sqe + NVME_SQE_CDW11_OFF);
    uint8_t lid = cdw10 & 0xff;

    /* NUMD is a 0-based count of dwords split across cdw10 and cdw11. */
    uint64_t numd =
        ((cdw10 >> 16) & 0xffff) | ((uint64_t)(cdw11 & 0xffff) << 16);
    uint64_t want = (numd + 1) * 4;
    uint64_t off = ernic_get_le64(sqe + NVME_SQE_CDW12_OFF);

    struct nvmeof_sgl sgl;
    if (!parse_sgl(sqe, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }
    if (want > sgl.length) {
        want = sgl.length;
    }
    if (want > NVMEOF_LOG_PAGE_MAX) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    size_t total;
    switch (lid) {
    case NVME_LOG_DISCOVERY:
        total = NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE;
        break;
    case NVME_LOG_ERROR:
    case NVME_LOG_SMART:
    case NVME_LOG_FW_SLOT:
    case NVME_LOG_CHANGED_NS:
    case NVME_LOG_CMD_EFFECTS:
        total = 4096;
        break;
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    uint8_t *page = calloc(1, total);
    if (page == NULL) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL, 1);
    }
    if (lid == NVME_LOG_DISCOVERY) {
        build_discovery_log(t, page, total);
    }

    uint16_t st;
    if (off >= total) {
        /* Reading past the end yields zeros, not an error. */
        uint8_t zero[64] = {0};
        uint32_t n =
            want > sizeof(zero) ? (uint32_t)sizeof(zero) : (uint32_t)want;
        st = sgl_write(&sgl, dma, ctx, zero, n);
    } else {
        uint64_t avail = total - off;
        if (want > avail) {
            want = avail;
        }
        st = sgl_write(&sgl, dma, ctx, page + off, (uint32_t)want);
    }
    free(page);
    return st;
}

static uint16_t do_set_features(struct nvmeof_queue *q, const uint8_t *sqe,
                                uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = ernic_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t cdw11 = ernic_get_le32(sqe + NVME_SQE_CDW11_OFF);
    uint8_t fid = cdw10 & 0xff;

    switch (fid) {
    case NVME_FEAT_NUM_QUEUES: {
        /* Both counts are 0-based; grant no more than we can serve. */
        uint16_t want_sq = (uint16_t)(cdw11 & 0xffff);
        uint16_t want_cq = (uint16_t)(cdw11 >> 16);
        uint16_t max = (uint16_t)(t->cfg.max_queues - 1);
        if (want_sq > max) {
            want_sq = max;
        }
        if (want_cq > max) {
            want_cq = max;
        }
        t->ctrl.num_io_queues =
            (uint16_t)((want_sq < want_cq ? want_sq : want_cq) + 1);
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF,
                       (uint32_t)want_sq | ((uint32_t)want_cq << 16));
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    case NVME_FEAT_ASYNC_EVENT:
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, cdw11);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_KATO:
        t->ctrl.kato = cdw11;
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, cdw11);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }
}

static uint16_t do_get_features(struct nvmeof_queue *q, const uint8_t *sqe,
                                uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = ernic_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint8_t fid = cdw10 & 0xff;
    uint16_t n;

    switch (fid) {
    case NVME_FEAT_NUM_QUEUES:
        n = (uint16_t)(t->ctrl.num_io_queues > 0 ? t->ctrl.num_io_queues - 1
                                                 : 0);
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF,
                       (uint32_t)n | ((uint32_t)n << 16));
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_KATO:
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, t->ctrl.kato);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_ASYNC_EVENT:
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, 0);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }
}

static int exec_admin(struct nvmeof_queue *q, const uint8_t *capsule,
                      const struct nvmeof_dma_ops *dma, void *ctx, uint8_t *rsp,
                      uint16_t *status)
{
    q->target->stats.admin_cmds++;

    switch (capsule[NVME_SQE_OPCODE_OFF]) {
    case NVME_ADMIN_IDENTIFY:
        *status = do_identify(q, capsule, dma, ctx);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_GET_LOG_PAGE:
        *status = do_get_log_page(q, capsule, dma, ctx);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_SET_FEATURES:
        *status = do_set_features(q, capsule, rsp);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_GET_FEATURES:
        *status = do_get_features(q, capsule, rsp);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_KEEP_ALIVE:
        *status = nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_ABORT:
        /* Nothing is ever queued long enough to abort; report not-found. */
        ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, 1);
        *status = nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        return NVMEOF_EXEC_DONE;
    case NVME_ADMIN_ASYNC_EVENT:
        /*
         * Held deliberately: the host keeps one outstanding for the life
         * of the controller and completing it would spin it.
         */
        return NVMEOF_EXEC_HELD;
    default:
        *status = nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
        return NVMEOF_EXEC_DONE;
    }
}

/* ------------------------------------------------------------------ */
/* NVM commands                                                       */
/* ------------------------------------------------------------------ */

static uint16_t io_range(const struct nvmeof_ns *ns, const uint8_t *sqe,
                         uint64_t *off, uint64_t *len)
{
    uint64_t slba = ernic_get_le64(sqe + NVME_SQE_CDW10_OFF);
    uint32_t nlb = (ernic_get_le32(sqe + NVME_SQE_CDW12_OFF) & 0xffff) + 1U;

    uint64_t blocks = ns->cfg.size / ns->cfg.block_size;
    if (slba >= blocks || nlb > blocks - slba) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_LBA_RANGE, 1);
    }
    *off = slba * ns->cfg.block_size;
    *len = (uint64_t)nlb * ns->cfg.block_size;
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

/* A Read or, if @write, a Write: the data moves between the namespace and
 * the host buffers the capsule's SGL names. */
static uint16_t exec_rw(struct nvmeof_target *t, struct nvmeof_ns *ns,
                        const uint8_t *capsule, size_t capsule_len,
                        const struct nvmeof_dma_ops *dma, void *ctx, bool write,
                        bool fua)
{
    uint64_t off;
    uint64_t len;
    uint16_t st = io_range(ns, capsule, &off, &len);
    if (st != 0) {
        return st;
    }

    struct nvmeof_sgl sgl;
    if (!parse_sgl(capsule, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }
    if (len > UINT32_MAX) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    if (!write) {
        st = sgl_write(&sgl, dma, ctx, ns->data + off, (uint32_t)len);
        if (st == 0) {
            t->stats.read_bytes += len;
        }
    } else {
        st = sgl_read(&sgl, capsule, capsule_len, dma, ctx, ns->data + off,
                      (uint32_t)len);
        if (st == 0) {
            t->stats.write_bytes += len;
            if (fua) {
                st = ns_sync(t, ns, off, len);
            }
        }
    }
    return st;
}


/* ------------------------------------------------------------------ */
/* Key Value command set                                              */
/* ------------------------------------------------------------------ */

/*
 * Pull the key out of a command capsule.
 *
 * The specification splits it: key[0..7] in CDW2 and CDW3, key[8..15] in
 * CDW14 and CDW15, with the length in CDW11[07:00].  The bytes are an
 * opaque octet string, so they come out in the order they went in and no
 * byte swapping applies — only the dwords around them are little-endian.
 */
static bool kv_key_from_capsule(const uint8_t *capsule,
                                struct nvmeof_kv_key *key)
{
    uint32_t cdw11 = ernic_get_le32(capsule + NVME_SQE_CDW11_OFF);
    uint8_t len = (uint8_t)(cdw11 & 0xff);

    if (len == 0 || len > NVMEOF_KV_KEY_MAX) {
        return false;
    }

    /* The key fills CDW2, CDW3, CDW14 and CDW15, in that order */
    static const size_t key_dw_off[] = {NVME_SQE_CDW2_OFF, NVME_SQE_CDW3_OFF,
                                        NVME_SQE_CDW14_OFF, NVME_SQE_CDW15_OFF};
    _Static_assert(sizeof(key_dw_off) / sizeof(key_dw_off[0]) *
                           sizeof(uint32_t) ==
                       NVMEOF_KV_KEY_MAX,
                   "the key's dwords hold NVMEOF_KV_KEY_MAX bytes");
    uint8_t raw[NVMEOF_KV_KEY_MAX];
    for (size_t i = 0; i < sizeof(key_dw_off) / sizeof(key_dw_off[0]); i++) {
        memcpy(raw + (i * sizeof(uint32_t)), capsule + key_dw_off[i],
               sizeof(uint32_t));
    }

    memset(key->bytes, 0, sizeof(key->bytes));
    memcpy(key->bytes, raw, len);
    key->len = len;
    return true;
}

/* A Store: the value, CDW10 bytes long, comes from the host buffers the
 * capsule's SGL names, subject to CDW11's overwrite options. */
static uint16_t kv_exec_store(struct nvmeof_target *t, struct nvmeof_ns *ns,
                              const struct nvmeof_kv_key *key,
                              const uint8_t *capsule, size_t capsule_len,
                              const struct nvmeof_dma_ops *dma, void *ctx)
{
    uint32_t len = ernic_get_le32(capsule + NVME_SQE_CDW10_OFF);
    uint32_t cdw11 = ernic_get_le32(capsule + NVME_SQE_CDW11_OFF);
    uint8_t options = (uint8_t)((cdw11 >> 8) & 0xff);

    if (len > ns->cfg.kv_max_value) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_INVALID_VALUE_SIZE, 1);
    }

    bool exists = nvmeof_kv_exists(ns->kv, key);
    if ((options & NVME_KV_STORE_OPT_NO_OVERWRITE) != 0 && exists) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_KEY_EXISTS, 1);
    }
    if ((options & NVME_KV_STORE_OPT_MUST_EXIST) != 0 && !exists) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_KEY_NOT_EXIST, 1);
    }

    struct nvmeof_sgl sgl;
    if (!parse_sgl(capsule, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }

    /* The value arrives through the same keyed SGL a block Write uses,
     * so it comes out of guest memory by rkey -- including GPU memory,
     * when the guest registered it there. */
    uint8_t *staging = len != 0 ? malloc(len) : NULL;
    if (len != 0 && staging == NULL) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL, 1);
    }
    uint16_t st = sgl_read(&sgl, capsule, capsule_len, dma, ctx, staging, len);
    if (st != 0) {
        free(staging);
        return st;
    }
    if (!nvmeof_kv_put(ns->kv, key, staging, len)) {
        free(staging);
        /* Out of keys or out of capacity; the host cannot tell the two
         * apart from the status alone, which the spec accepts. */
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_CAP_EXCEEDED, 1);
    }
    free(staging);
    t->stats.write_bytes += len;
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

/* A Retrieve: as much of the value as fits in the host's CDW10-byte buffer
 * goes to it, and the value's full length to @p result. */
static uint16_t kv_exec_retrieve(struct nvmeof_target *t,
                                 const struct nvmeof_ns *ns,
                                 const struct nvmeof_kv_key *key,
                                 const uint8_t *capsule,
                                 const struct nvmeof_dma_ops *dma, void *ctx,
                                 uint32_t *result)
{
    uint32_t cdw10 = ernic_get_le32(capsule + NVME_SQE_CDW10_OFF);
    const uint8_t *value = NULL;
    uint32_t stored = 0;
    if (!nvmeof_kv_get(ns->kv, key, &value, &stored)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_KEY_NOT_EXIST, 1);
    }

    struct nvmeof_sgl sgl;
    if (!parse_sgl(capsule, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }

    /* CDW10 is the host buffer size, not a demand for that many bytes.
     * Write at most what fits and report the true length in the
     * completion, which is how the host learns it asked with too small
     * a buffer. */
    uint32_t want = cdw10 < stored ? cdw10 : stored;
    if (want != 0 && sgl.length < want) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
    }
    uint16_t st = 0;
    if (want != 0) {
        st = sgl_write(&sgl, dma, ctx, value, want);
    }
    if (st != 0) {
        return st;
    }
    *result = stored;
    t->stats.read_bytes += want;
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

/* A List: a list of {key length, key bytes} records, as many as fit, with
 * the number returned in @p result.  A nonzero @p key starts the list
 * after that key. */
static uint16_t kv_exec_list(const struct nvmeof_ns *ns,
                             const struct nvmeof_kv_key *key,
                             const uint8_t *capsule,
                             const struct nvmeof_dma_ops *dma, void *ctx,
                             uint32_t *result)
{
    struct nvmeof_sgl sgl;
    if (!parse_sgl(capsule, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }

    uint32_t cap = ernic_get_le32(capsule + NVME_SQE_CDW10_OFF);
    if (cap > NVME_IDENTIFY_SIZE) {
        cap = NVME_IDENTIFY_SIZE;
    }
    uint8_t buf[NVME_IDENTIFY_SIZE];
    memset(buf, 0, sizeof(buf));

    uint32_t off = 0;
    uint32_t listed = 0;
    uint32_t iter = 0;
    struct nvmeof_kv_key k;

    /* If the host passed a starting key, skip the iterator forward past
     * it so the response is a continuation page, not a restart. */
    if (key->len > 0) {
        struct nvmeof_kv_key scan_k;
        while (nvmeof_kv_next(ns->kv, &iter, &scan_k)) {
            if (scan_k.len == key->len &&
                memcmp(scan_k.bytes, key->bytes, key->len) == 0) {
                break;
            }
        }
    }

    while (nvmeof_kv_next(ns->kv, &iter, &k)) {
        if (off + 1U + k.len > cap) {
            break;
        }
        buf[off++] = k.len;
        memcpy(buf + off, k.bytes, k.len);
        off += k.len;
        listed++;
    }

    if (off != 0 && sgl.length < off) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
    }
    uint16_t st = off != 0 ? sgl_write(&sgl, dma, ctx, buf, off) : 0;
    if (st != 0) {
        return st;
    }
    *result = listed;
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t kv_exec(struct nvmeof_target *t, struct nvmeof_ns *ns,
                        const uint8_t *capsule, size_t capsule_len,
                        const struct nvmeof_dma_ops *dma, void *ctx,
                        uint32_t *result)
{
    uint8_t opcode = capsule[NVME_SQE_OPCODE_OFF];
    uint32_t cdw11 = ernic_get_le32(capsule + NVME_SQE_CDW11_OFF);

    /* List accepts a zero-length key meaning "start from the beginning";
     * a nonzero invalid length is rejected for all opcodes. */
    struct nvmeof_kv_key key = {0};
    if (!kv_key_from_capsule(capsule, &key) &&
        (opcode != NVME_KV_CMD_LIST || (cdw11 & 0xff) != 0)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_INVALID_KEY_SIZE, 1);
    }

    switch (opcode) {
    case NVME_KV_CMD_EXIST:
        if (!nvmeof_kv_exists(ns->kv, &key)) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_KEY_NOT_EXIST, 1);
        }
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_KV_CMD_DELETE:
        if (!nvmeof_kv_delete(ns->kv, &key)) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_KV_KEY_NOT_EXIST, 1);
        }
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_KV_CMD_STORE:
        return kv_exec_store(t, ns, &key, capsule, capsule_len, dma, ctx);

    case NVME_KV_CMD_RETRIEVE:
        return kv_exec_retrieve(t, ns, &key, capsule, dma, ctx, result);

    case NVME_KV_CMD_LIST:
        return kv_exec_list(ns, &key, capsule, dma, ctx, result);

    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
    }
}

static uint16_t exec_io(const struct nvmeof_queue *q, const uint8_t *capsule,
                        size_t capsule_len, const struct nvmeof_dma_ops *dma,
                        void *ctx, uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint8_t opcode = capsule[NVME_SQE_OPCODE_OFF];
    uint32_t nsid = ernic_get_le32(capsule + NVME_SQE_NSID_OFF);
    uint64_t off;
    uint64_t len;
    uint16_t st;

    /* FUA on a Read asks for the media's copy, which the mapping already
     * is, so only writes act on it. */
    bool fua =
        (ernic_get_le32(capsule + NVME_SQE_CDW12_OFF) & NVME_RW_FUA) != 0;

    t->stats.io_cmds++;

    struct nvmeof_ns *ns = ns_by_id(t, nsid);
    if (ns == NULL) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NS, 1);
    }

    /*
     * A KV namespace answers an entirely different command set, and the
     * opcodes overlap: dispatching on the namespace's own command set
     * before the switch below is what keeps a Store (01h) from reaching the
     * block Write path.  This is per namespace, not per controller, so one
     * controller can serve both -- which is the whole reason the host is
     * told each namespace's CSI separately.
     */
    if (ns->cfg.kv) {
        uint32_t result = 0;
        st = kv_exec(t, ns, capsule, capsule_len, dma, ctx, &result);
        if (result != 0) {
            ernic_put_le32(rsp + NVME_CQE_RESULT_OFF, result);
        }
        return st;
    }

    switch (opcode) {
    case NVME_CMD_FLUSH:
        return ns_sync(t, ns, 0, ns->len);

    case NVME_CMD_DSM:
        /* Deallocate is advisory; honouring it is optional. */
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_CMD_WRITE_ZEROES:
        st = io_range(ns, capsule, &off, &len);
        if (st != 0) {
            return st;
        }
        memset(ns->data + off, 0, (size_t)len);
        t->stats.write_bytes += len;
        return fua ? ns_sync(t, ns, off, len)
                   : nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return exec_rw(t, ns, capsule, capsule_len, dma, ctx,
                       opcode == NVME_CMD_WRITE, fua);

    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

int nvmeof_queue_exec(struct nvmeof_queue *q, const void *capsule, size_t len,
                      const struct nvmeof_dma_ops *dma, void *dma_ctx,
                      void *rsp)
{
    const uint8_t *sqe = capsule;
    uint8_t *out = rsp;

    if (q == NULL || sqe == NULL || out == NULL || dma == NULL) {
        return -EINVAL;
    }
    if (len < NVME_SQE_SIZE) {
        return -EINVAL;
    }

    struct nvmeof_target *t = q->target;
    uint8_t opcode = sqe[NVME_SQE_OPCODE_OFF];
    uint16_t cid = ernic_get_le16(sqe + NVME_SQE_CID_OFF);
    uint16_t status;
    int rc;

    memset(out, 0, NVME_CQE_SIZE);

    if (opcode == NVME_ADMIN_FABRICS) {
        status = exec_fabrics(q, sqe, len, dma, dma_ctx, out);
        rc = NVMEOF_EXEC_DONE;
    } else if (!q->connected) {
        status = nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
        rc = NVMEOF_EXEC_DONE;
    } else if (q->qid == 0) {
        rc = exec_admin(q, sqe, dma, dma_ctx, out, &status);
    } else {
        status = exec_io(q, sqe, len, dma, dma_ctx, out);
        rc = NVMEOF_EXEC_DONE;
    }

    if (rc == NVMEOF_EXEC_HELD) {
        return rc;
    }

    if (status != 0) {
        t->stats.errors++;
    }

    /*
     * SQ head is advanced per command and wraps at sqsize + 1; the host
     * uses it purely for flow control, so a queue that has not completed
     * Connect yet simply reports zero.
     */
    if (q->sqsize != 0) {
        q->sq_head = (uint16_t)((q->sq_head + 1U) % ((uint32_t)q->sqsize + 1U));
    }

    ernic_put_le16(out + NVME_CQE_SQHD_OFF, q->sq_head);
    ernic_put_le16(out + NVME_CQE_SQID_OFF, q->qid);
    ernic_put_le16(out + NVME_CQE_CID_OFF, cid);
    ernic_put_le16(out + NVME_CQE_STATUS_OFF, status);
    return NVMEOF_EXEC_DONE;
}
