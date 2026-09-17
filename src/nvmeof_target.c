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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ionic_datapath.h" /* IONIC_MAX_MR_QUEUES -- the queue-count ceiling */
#include "nvmeof_target.h"

#define NVMEOF_DEFAULT_NQN        "nvmet-test"
#define NVMEOF_DEFAULT_SIZE       (64ull << 20)
#define NVMEOF_DEFAULT_BLOCK_SIZE 512u
#define NVMEOF_DEFAULT_PORT       4420
#define NVMEOF_DEFAULT_TRADDR     0xc0a8c801u /* 192.168.200.1 */
#define NVMEOF_DEFAULT_QUEUES     8
#define NVMEOF_DEFAULT_DEPTH      128

#define NVMEOF_CNTLID 1

/* Maximum Data Transfer Size, as 2^mdts pages of 4 KiB. */
#define NVMEOF_MDTS 5

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

struct nvmeof_target {
    struct nvmeof_target_cfg cfg;
    struct nvmeof_target_stats stats;
    struct nvmeof_ctrl ctrl;

    uint8_t *ns_data;
    size_t ns_len;
    int ns_fd;

    struct nvmeof_queue *queues;
    unsigned queue_cap;
};

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

void nvmeof_target_cfg_defaults(struct nvmeof_target_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->subnqn, sizeof(cfg->subnqn), "%s", NVMEOF_DEFAULT_NQN);
    snprintf(cfg->serial, sizeof(cfg->serial), "ERNIC0000000000001");
    snprintf(cfg->model, sizeof(cfg->model), "rocm-ernic NVMe-oF target");
    cfg->size = NVMEOF_DEFAULT_SIZE;
    cfg->block_size = NVMEOF_DEFAULT_BLOCK_SIZE;
    cfg->nsid = 1;
    cfg->traddr = NVMEOF_DEFAULT_TRADDR;
    cfg->trsvcid = NVMEOF_DEFAULT_PORT;
    cfg->max_queues = NVMEOF_DEFAULT_QUEUES;
    cfg->queue_depth = NVMEOF_DEFAULT_DEPTH;
}

static bool parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (*s == '\0' || *s == '-') {
        return false;
    }
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s) {
        return false;
    }

    uint64_t mult = 1;
    switch (*end) {
    case 'k':
    case 'K':
        mult = 1ull << 10;
        end++;
        break;
    case 'm':
    case 'M':
        mult = 1ull << 20;
        end++;
        break;
    case 'g':
    case 'G':
        mult = 1ull << 30;
        end++;
        break;
    case 't':
    case 'T':
        mult = 1ull << 40;
        end++;
        break;
    default:
        break;
    }
    /* Accept the "MiB"/"MB" spellings people reach for out of habit. */
    if (mult != 1) {
        if (*end == 'i' || *end == 'I') {
            end++;
        }
        if (*end == 'b' || *end == 'B') {
            end++;
        }
    }
    if (*end != '\0') {
        return false;
    }
    if (v != 0 && v > UINT64_MAX / mult) {
        return false;
    }
    *out = (uint64_t)v * mult;
    return true;
}

static bool parse_ipv4(const char *s, uint32_t *out)
{
    unsigned o[4];
    char extra;

    if (sscanf(s, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &extra) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255) {
            return false;
        }
    }
    *out = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

bool nvmeof_target_cfg_parse(struct nvmeof_target_cfg *cfg, const char *opts,
                             char *err, size_t errlen)
{
    if (opts == NULL || *opts == '\0') {
        return true;
    }

    char *dup = strdup(opts);
    if (dup == NULL) {
        set_err(err, errlen, "out of memory");
        return false;
    }

    bool ok = true;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok != NULL && ok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            set_err(err, errlen, "option '%s' is not key=value", tok);
            ok = false;
            break;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        if (strcmp(key, "nqn") == 0) {
            if (*val == '\0' || strlen(val) >= NVMEOF_NQN_MAX) {
                set_err(err, errlen, "nqn must be 1..%d characters",
                        NVMEOF_NQN_MAX - 1);
                ok = false;
            } else {
                snprintf(cfg->subnqn, sizeof(cfg->subnqn), "%s", val);
            }
        } else if (strcmp(key, "size") == 0) {
            if (!parse_size(val, &cfg->size) || cfg->size == 0) {
                set_err(err, errlen, "bad size '%s'", val);
                ok = false;
            }
        } else if (strcmp(key, "file") == 0) {
            if (*val == '\0') {
                set_err(err, errlen, "file needs a path");
                ok = false;
            } else {
                free(cfg->file);
                cfg->file = strdup(val);
                if (cfg->file == NULL) {
                    set_err(err, errlen, "out of memory");
                    ok = false;
                }
            }
        } else if (strcmp(key, "bs") == 0) {
            uint32_t bs;
            if (!parse_u32(val, &bs) ||
                (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096)) {
                set_err(err, errlen,
                        "bs must be 512, 1024, 2048 or 4096 (got '%s')", val);
                ok = false;
            } else {
                cfg->block_size = bs;
            }
        } else if (strcmp(key, "nsid") == 0) {
            uint32_t v;
            if (!parse_u32(val, &v) || v == 0 || v == 0xffffffffu) {
                set_err(err, errlen, "bad nsid '%s'", val);
                ok = false;
            } else {
                cfg->nsid = v;
            }
        } else if (strcmp(key, "ip") == 0) {
            if (!parse_ipv4(val, &cfg->traddr)) {
                set_err(err, errlen, "bad IPv4 address '%s'", val);
                ok = false;
            }
        } else if (strcmp(key, "port") == 0) {
            uint32_t v;
            if (!parse_u32(val, &v) || v == 0 || v > 65535) {
                set_err(err, errlen, "bad port '%s'", val);
                ok = false;
            } else {
                cfg->trsvcid = (uint16_t)v;
            }
        } else if (strcmp(key, "queues") == 0) {
            uint32_t v;
            if (!parse_u32(val, &v) || v == 0 || v > IONIC_MAX_MR_QUEUES) {
                /* The ceiling is the memory-region budget, not the NVMe spec:
                 * the initiator pre-allocates IONIC_MR_PER_QUEUE regions per
                 * queue at connect time.  Refusing here turns what was a bare
                 * "invalid arguments/configuration" from nvme connect, long
                 * after startup, into a message that names the real limit. */
                set_err(err, errlen,
                        "queues must be 1..%d, one per %d memory regions in a "
                        "table of %d (got '%s')",
                        IONIC_MAX_MR_QUEUES, IONIC_MR_PER_QUEUE, IONIC_MAX_MR,
                        val);
                ok = false;
            } else {
                cfg->max_queues = (uint16_t)v;
            }
        } else if (strcmp(key, "model") == 0) {
            snprintf(cfg->model, sizeof(cfg->model), "%s", val);
        } else if (strcmp(key, "serial") == 0) {
            snprintf(cfg->serial, sizeof(cfg->serial), "%s", val);
        } else {
            set_err(err, errlen, "unknown nvmeof option '%s'", key);
            ok = false;
        }
    }
    free(dup);

    if (ok && cfg->size % cfg->block_size != 0) {
        set_err(err, errlen, "size %llu is not a multiple of bs %u",
                (unsigned long long)cfg->size, cfg->block_size);
        ok = false;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen)
{
    struct nvmeof_target *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    t->cfg = *cfg;
    t->cfg.file = NULL;
    t->ns_fd = -1;
    t->ns_len = (size_t)cfg->size;

    if (cfg->file != NULL) {
        t->cfg.file = strdup(cfg->file);
        if (t->cfg.file == NULL) {
            set_err(err, errlen, "out of memory");
            goto fail;
        }
        t->ns_fd = open(t->cfg.file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (t->ns_fd < 0) {
            set_err(err, errlen, "cannot open %s: %s", t->cfg.file,
                    strerror(errno));
            goto fail;
        }
        if (ftruncate(t->ns_fd, (off_t)t->ns_len) != 0) {
            set_err(err, errlen, "cannot size %s to %llu bytes: %s",
                    t->cfg.file, (unsigned long long)t->ns_len,
                    strerror(errno));
            goto fail;
        }
        t->ns_data = mmap(NULL, t->ns_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                          t->ns_fd, 0);
    } else {
        t->ns_data = mmap(NULL, t->ns_len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    if (t->ns_data == MAP_FAILED) {
        t->ns_data = NULL;
        set_err(err, errlen, "cannot map %llu bytes of namespace: %s",
                (unsigned long long)t->ns_len, strerror(errno));
        goto fail;
    }

    t->queue_cap = (unsigned)cfg->max_queues + 1;
    t->queues = calloc(t->queue_cap, sizeof(*t->queues));
    if (t->queues == NULL) {
        set_err(err, errlen, "out of memory");
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
    if (t->ns_data != NULL) {
        munmap(t->ns_data, t->ns_len);
    }
    if (t->ns_fd >= 0) {
        close(t->ns_fd);
    }
    free(t->queues);
    free(t->cfg.file);
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
    uint8_t type = d[15] >> 4;
    uint8_t subtype = d[15] & 0xf;

    memset(sgl, 0, sizeof(*sgl));
    sgl->addr = nvme_get_le64(d);

    switch (type) {
    case NVME_SGL_TYPE_KEYED_DATA:
        sgl->keyed = true;
        sgl->length =
            (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16);
        sgl->key = nvme_get_le32(d + 11);
        return true;
    case NVME_SGL_TYPE_DATA_BLOCK:
    case NVME_SGL_TYPE_TRANSPORT_DATA:
        sgl->length = nvme_get_le32(d + 8);
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
 */
static uint16_t sgl_write(const struct nvmeof_sgl *sgl,
                          const struct nvmeof_dma_ops *dma, void *ctx,
                          const void *src, uint32_t len)
{
    if (len == 0) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    if (sgl->in_capsule) {
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
        /* addr is an offset past the 64-byte SQE. */
        uint64_t off = (uint64_t)NVME_SQE_SIZE + sgl->addr;
        if (off > capsule_len || capsule_len - off < len) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_DATA, 1);
        }
        memcpy(dst, capsule + off, len);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
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
    size_t n = strlen(src);
    if (n > len) {
        n = len;
    }
    memcpy(dst, src, n);
    memset(dst + n, ' ', len - n);
}

static uint8_t lba_shift(uint32_t block_size)
{
    uint8_t s = 0;
    while ((1u << s) < block_size) {
        s++;
    }
    return s;
}

static void build_id_ctrl(const struct nvmeof_target *t, uint8_t *buf)
{
    const struct nvmeof_target_cfg *c = &t->cfg;

    memset(buf, 0, NVME_IDENTIFY_SIZE);
    nvme_put_le16(buf + 0, 0x1dd8); /* vid: AMD Pensando */
    nvme_put_le16(buf + 2, 0x1dd8); /* ssvid */
    ascii_field(buf + 4, 20, c->serial);
    ascii_field(buf + 24, 40, c->model);
    ascii_field(buf + 64, 8, "1.0");
    buf[72] = 6; /* rab */
    buf[77] = NVMEOF_MDTS;
    nvme_put_le16(buf + 78, NVMEOF_CNTLID);
    nvme_put_le32(buf + 80, 0x00010400); /* ver: 1.4.0 */

    buf[111] = t->ctrl.discovery ? 2 : 1; /* cntrltype */

    nvme_put_le16(buf + 256, 0); /* oacs */
    buf[259] = 3;                /* aerl */
    buf[261] = 0x02;             /* lpa: no extended data */
    buf[262] = 0;                /* elpe */

    /* Keep Alive is mandatory over fabrics.  KAS is in 100ms units, so this
     * is a 1s granularity, matching what nvmet advertises; the host's default
     * KATO is 5s. */
    nvme_put_le16(buf + 320, 10); /* kas, in 100ms units */

    buf[512] = 0x66;                                     /* sqes: 64B fixed */
    buf[513] = 0x44;                                     /* cqes: 16B fixed */
    nvme_put_le16(buf + 514, t->cfg.queue_depth);        /* maxcmd */
    nvme_put_le32(buf + 516, t->ctrl.discovery ? 0 : 1); /* nn */
    nvme_put_le16(buf + 520, 0x0008); /* oncs: Write Zeroes */
    buf[524] = 0;                     /* fna */
    buf[525] = 0;                     /* vwc: no volatile write cache */

    /*
     * sgls: supported (bit 0), keyed SGLs (bit 2, which nvme-rdma treats
     * as mandatory), and SGL address-as-offset (bit 20) for in-capsule.
     */
    nvme_put_le32(buf + 536, (1u << 0) | (1u << 2) | (1u << 20));

    /*
     * A discovery association has to name the discovery NQN here.  Linux
     * builds subsys->subnqn from this field and then rejects the controller
     * outright -- "Subsystem %s is not a discovery controller", EINVAL -- if
     * it says CNTRLTYPE is discovery while the NQN is an ordinary subsystem's.
     */
    snprintf((char *)buf + 768, 256, "%s",
             t->ctrl.discovery ? NVMEOF_DISCOVERY_NQN : c->subnqn);

    nvme_put_le32(buf + 1792, 4); /* ioccsz: 64 bytes, no in-capsule data */
    nvme_put_le32(buf + 1796, 1); /* iorcsz: 16 bytes */
    nvme_put_le16(buf + 1800, 0); /* icdoff */
    buf[1802] = 0;                /* ctrattr */
    buf[1803] = 1;                /* msdbd */
}

static void build_id_ns(const struct nvmeof_target *t, uint8_t *buf)
{
    uint64_t blocks = t->cfg.size / t->cfg.block_size;

    memset(buf, 0, NVME_IDENTIFY_SIZE);
    nvme_put_le64(buf + 0, blocks);  /* nsze */
    nvme_put_le64(buf + 8, blocks);  /* ncap */
    nvme_put_le64(buf + 16, blocks); /* nuse */
    buf[24] = 0;                     /* nsfeat */
    buf[25] = 0;                     /* nlbaf: one format */
    buf[26] = 0;                     /* flbas: format 0 */
    buf[27] = 0;                     /* mc */
    buf[28] = 0;                     /* dpc */
    buf[29] = 0;                     /* dps */

    /* LBA format 0: ms = 0, lbads = log2(block size), rp = best. */
    nvme_put_le16(buf + 128, 0);
    buf[130] = lba_shift(t->cfg.block_size);
    buf[131] = 0;
}

/*
 * Namespace Identification Descriptor list.  A UUID derived from the
 * subsystem NQN and the namespace ID keeps udev's by-id links stable
 * across restarts of the same server configuration.
 */
static void build_ns_desc(const struct nvmeof_target *t, uint8_t *buf)
{
    memset(buf, 0, NVME_IDENTIFY_SIZE);
    buf[0] = 3;  /* nidt: UUID */
    buf[1] = 16; /* nidl */

    uint8_t *uuid = buf + 4;
    uint64_t h = 0xcbf29ce484222325ull;
    const char *p = t->cfg.subnqn;
    while (*p != '\0') {
        h = (h ^ (uint8_t)*p++) * 0x100000001b3ull;
    }
    h ^= t->cfg.nsid;
    for (int i = 0; i < 16; i++) {
        uuid[i] = (uint8_t)(h >> ((i % 8) * 8)) ^ (uint8_t)(i * 0x11);
    }
    uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40); /* version 4 */
    uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80); /* variant 1 */
}

static void build_discovery_log(const struct nvmeof_target *t, uint8_t *buf,
                                size_t len)
{
    memset(buf, 0, len);
    if (len < NVMEOF_DISC_HDR_SIZE) {
        return;
    }
    nvme_put_le64(buf + NVMEOF_DISC_GENCTR_OFF, 1);
    nvme_put_le64(buf + NVMEOF_DISC_NUMREC_OFF, 1);
    nvme_put_le16(buf + NVMEOF_DISC_RECFMT_OFF, 0);

    if (len < NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE) {
        return;
    }
    uint8_t *e = buf + NVMEOF_DISC_HDR_SIZE;
    e[NVMEOF_DISC_E_TRTYPE_OFF] = NVMEOF_TRTYPE_RDMA;
    e[NVMEOF_DISC_E_ADRFAM_OFF] = NVMEOF_ADRFAM_IPV4;
    e[NVMEOF_DISC_E_SUBTYPE_OFF] = NVMEOF_SUBTYPE_NVME;
    e[NVMEOF_DISC_E_TREQ_OFF] = 0;
    nvme_put_le16(e + NVMEOF_DISC_E_PORTID_OFF, 1);
    nvme_put_le16(e + NVMEOF_DISC_E_CNTLID_OFF, 0xffff); /* dynamic */
    nvme_put_le16(e + NVMEOF_DISC_E_ASQSZ_OFF, t->cfg.queue_depth);
    snprintf((char *)e + NVMEOF_DISC_E_TRSVCID_OFF, 32, "%u", t->cfg.trsvcid);
    snprintf((char *)e + NVMEOF_DISC_E_SUBNQN_OFF, 256, "%s", t->cfg.subnqn);
    snprintf((char *)e + NVMEOF_DISC_E_TRADDR_OFF, 256, "%u.%u.%u.%u",
             (t->cfg.traddr >> 24) & 0xff, (t->cfg.traddr >> 16) & 0xff,
             (t->cfg.traddr >> 8) & 0xff, t->cfg.traddr & 0xff);

    uint8_t *tsas = e + NVMEOF_DISC_E_TSAS_OFF;
    tsas[0] = NVMEOF_RDMA_QPTYPE_CONNECTED;
    tsas[1] = NVMEOF_RDMA_PRTYPE_NONE;
    tsas[2] = NVMEOF_RDMA_CMS_RDMA_CM;
    nvme_put_le16(tsas + 8, 0xffff); /* pkey */
}

/* ------------------------------------------------------------------ */
/* Fabrics commands                                                   */
/* ------------------------------------------------------------------ */

static uint64_t prop_cap(const struct nvmeof_target *t)
{
    uint64_t mqes = t->cfg.queue_depth > 0 ? t->cfg.queue_depth - 1u : 0;
    return mqes |          /* MQES */
           (1ull << 16) |  /* CQR: contiguous queues required */
           (15ull << 24) | /* TO: 7.5 seconds */
           (1ull << 37);   /* CSS: NVM command set */
}

static uint16_t do_connect(struct nvmeof_queue *q, const uint8_t *sqe,
                           const uint8_t *capsule, size_t capsule_len,
                           const struct nvmeof_dma_ops *dma, void *ctx,
                           uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint16_t recfmt = nvme_get_le16(sqe + NVMEOF_CONNECT_RECFMT_OFF);
    uint16_t qid = nvme_get_le16(sqe + NVMEOF_CONNECT_QID_OFF);
    uint16_t sqsize = nvme_get_le16(sqe + NVMEOF_CONNECT_SQSIZE_OFF);
    uint32_t kato = nvme_get_le32(sqe + NVMEOF_CONNECT_KATO_OFF);

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
        snprintf(t->ctrl.hostnqn, sizeof(t->ctrl.hostnqn), "%s", hostnqn);
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
    nvme_put_le16(rsp + NVME_CQE_RESULT_OFF, NVMEOF_CNTLID);
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t do_property_get(struct nvmeof_queue *q, const uint8_t *sqe,
                                uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint8_t attrib = sqe[NVMEOF_PROP_ATTRIB_OFF];
    uint32_t off = nvme_get_le32(sqe + NVMEOF_PROP_OFFSET_OFF);
    uint64_t v;

    switch (off) {
    case NVMEOF_PROP_CAP:
        if (attrib != 1) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        }
        v = prop_cap(t);
        break;
    case NVMEOF_PROP_VS:
        v = 0x00010400;
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

    nvme_put_le64(rsp + NVME_CQE_RESULT_OFF, v);
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t do_property_set(struct nvmeof_queue *q, const uint8_t *sqe)
{
    struct nvmeof_target *t = q->target;
    uint32_t off = nvme_get_le32(sqe + NVMEOF_PROP_OFFSET_OFF);
    uint64_t val = nvme_get_le64(sqe + NVMEOF_PROP_VALUE_OFF);

    if (off != NVMEOF_PROP_CC) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    t->ctrl.cc = (uint32_t)val;
    if (t->ctrl.cc & NVME_CC_ENABLE) {
        t->ctrl.csts = NVME_CSTS_RDY;
    } else {
        /* Shutdown or disable: report both not-ready and shutdown done. */
        uint32_t shn = (t->ctrl.cc >> 14) & 0x3;
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

static uint16_t do_identify(struct nvmeof_queue *q, const uint8_t *sqe,
                            const struct nvmeof_dma_ops *dma, void *ctx)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = nvme_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t nsid = nvme_get_le32(sqe + NVME_SQE_NSID_OFF);
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
    case NVME_ID_CNS_NS:
        if (nsid != t->cfg.nsid) {
            memset(buf, 0, sizeof(buf));
        } else {
            build_id_ns(t, buf);
        }
        break;
    case NVME_ID_CNS_NS_ACTIVE_LIST:
        memset(buf, 0, sizeof(buf));
        if (!t->ctrl.discovery && nsid < t->cfg.nsid) {
            nvme_put_le32(buf, t->cfg.nsid);
        }
        break;
    case NVME_ID_CNS_NS_DESC_LIST:
        if (nsid != t->cfg.nsid) {
            return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NS, 1);
        }
        build_ns_desc(t, buf);
        break;
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    return sgl_write(&sgl, dma, ctx, buf, sizeof(buf));
}

static uint16_t do_get_log_page(struct nvmeof_queue *q, const uint8_t *sqe,
                                const struct nvmeof_dma_ops *dma, void *ctx)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = nvme_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t cdw11 = nvme_get_le32(sqe + NVME_SQE_CDW11_OFF);
    uint8_t lid = cdw10 & 0xff;

    /* NUMD is a 0-based count of dwords split across cdw10 and cdw11. */
    uint64_t numd =
        ((cdw10 >> 16) & 0xffff) | ((uint64_t)(cdw11 & 0xffff) << 16);
    uint64_t want = (numd + 1) * 4;
    uint64_t off = nvme_get_le64(sqe + NVME_SQE_CDW12_OFF);

    struct nvmeof_sgl sgl;
    if (!parse_sgl(sqe, &sgl)) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SGL_INVALID_TYPE, 1);
    }
    if (want > sgl.length) {
        want = sgl.length;
    }
    if (want > (1u << 20)) {
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
    uint32_t cdw10 = nvme_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint32_t cdw11 = nvme_get_le32(sqe + NVME_SQE_CDW11_OFF);
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
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF,
                      (uint32_t)want_sq | ((uint32_t)want_cq << 16));
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }
    case NVME_FEAT_ASYNC_EVENT:
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF, cdw11);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_KATO:
        t->ctrl.kato = cdw11;
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF, cdw11);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    default:
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }
}

static uint16_t do_get_features(struct nvmeof_queue *q, const uint8_t *sqe,
                                uint8_t *rsp)
{
    struct nvmeof_target *t = q->target;
    uint32_t cdw10 = nvme_get_le32(sqe + NVME_SQE_CDW10_OFF);
    uint8_t fid = cdw10 & 0xff;
    uint16_t n;

    switch (fid) {
    case NVME_FEAT_NUM_QUEUES:
        n = (uint16_t)(t->ctrl.num_io_queues > 0 ? t->ctrl.num_io_queues - 1
                                                 : 0);
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF,
                      (uint32_t)n | ((uint32_t)n << 16));
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_KATO:
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF, t->ctrl.kato);
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    case NVME_FEAT_ASYNC_EVENT:
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF, 0);
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
        nvme_put_le32(rsp + NVME_CQE_RESULT_OFF, 1);
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

static uint16_t io_range(struct nvmeof_target *t, const uint8_t *sqe,
                         uint64_t *off, uint64_t *len)
{
    uint64_t slba = nvme_get_le64(sqe + NVME_SQE_CDW10_OFF);
    uint32_t nlb = (nvme_get_le32(sqe + NVME_SQE_CDW12_OFF) & 0xffff) + 1u;

    uint64_t blocks = t->cfg.size / t->cfg.block_size;
    if (slba >= blocks || nlb > blocks - slba) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_LBA_RANGE, 1);
    }
    *off = slba * t->cfg.block_size;
    *len = (uint64_t)nlb * t->cfg.block_size;
    return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
}

static uint16_t exec_io(struct nvmeof_queue *q, const uint8_t *capsule,
                        size_t capsule_len, const struct nvmeof_dma_ops *dma,
                        void *ctx)
{
    struct nvmeof_target *t = q->target;
    uint8_t opcode = capsule[NVME_SQE_OPCODE_OFF];
    uint32_t nsid = nvme_get_le32(capsule + NVME_SQE_NSID_OFF);
    uint64_t off, len;
    uint16_t st;

    t->stats.io_cmds++;

    if (nsid != t->cfg.nsid) {
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NS, 1);
    }

    switch (opcode) {
    case NVME_CMD_FLUSH:
        if (t->ns_fd >= 0) {
            msync(t->ns_data, t->ns_len, MS_ASYNC);
        }
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_CMD_DSM:
        /* Deallocate is advisory; honouring it is optional. */
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_CMD_WRITE_ZEROES:
        st = io_range(t, capsule, &off, &len);
        if (st != 0) {
            return st;
        }
        memset(t->ns_data + off, 0, (size_t)len);
        t->stats.write_bytes += len;
        return nvme_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_CMD_READ:
    case NVME_CMD_WRITE: {
        st = io_range(t, capsule, &off, &len);
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

        if (opcode == NVME_CMD_READ) {
            st = sgl_write(&sgl, dma, ctx, t->ns_data + off, (uint32_t)len);
            if (st == 0) {
                t->stats.read_bytes += len;
            }
        } else {
            st = sgl_read(&sgl, capsule, capsule_len, dma, ctx,
                          t->ns_data + off, (uint32_t)len);
            if (st == 0) {
                t->stats.write_bytes += len;
            }
        }
        return st;
    }

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
    uint16_t cid = nvme_get_le16(sqe + NVME_SQE_CID_OFF);
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
        status = exec_io(q, sqe, len, dma, dma_ctx);
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
        q->sq_head = (uint16_t)((q->sq_head + 1) % ((uint32_t)q->sqsize + 1));
    }

    nvme_put_le16(out + NVME_CQE_SQHD_OFF, q->sq_head);
    nvme_put_le16(out + NVME_CQE_SQID_OFF, q->qid);
    nvme_put_le16(out + NVME_CQE_CID_OFF, cid);
    nvme_put_le16(out + NVME_CQE_STATUS_OFF, status);
    return NVMEOF_EXEC_DONE;
}
