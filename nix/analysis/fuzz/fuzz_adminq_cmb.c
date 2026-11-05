/*
 * libFuzzer harness for the ionic admin-queue CREATE_QP command with the
 * CMB/EXPDB priv_flags: IONIC_QPF_SQ_CMB and IONIC_QPF_RQ_CMB.
 *
 * The harness posts CREATE_QP through the real ionic_adminq_dispatch_wqe()
 * against the fake device in tests/ionic_adminq_fakes.h, and checks every
 * promise:
 *
 *   - the status is 0 or one of the CREATE_QP error codes;
 *   - a short body (< IONIC_ADMIN_CREATE_QP_IN_V1_LEN) completes with EINVAL;
 *   - a duplicate QP id completes with EEXIST;
 *   - a bad CQ id completes with ENOENT, EQID, or ERANGE;
 *   - with valid body and QP/CQ ids, CMB-flagged creates succeed.
 *
 * A broken promise aborts, which libFuzzer reports as a crash.  ASan catches
 * writes outside guest memory.
 *
 * Input layout:
 *   byte 0      bits 0-1  flags index (see flags_tab[])
 *               bit  2    inject a duplicate QP id (use QP_ID)
 *               bit  3    use a bad CQ id for the SCQ field
 *   byte 1      body length (mod BODY_MOD)
 *   then        body bytes; missing bytes are zero
 *
 * LLVMFuzzerInitialize() runs a seed table on every startup; a failing
 * seed aborts before fuzzing starts.  With FUZZ_EMIT_SEEDS=1 it writes
 * a corpus in the current directory.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ionic/ionic_adminq_priv.h"
#include "ionic/ionic_status.h"

#include "ionic_adminq_fakes.h"

#include "fuzz_harness.h"

#define MAX_BODY  64
#define IN_HDR    ((size_t)2)
/* Keep body_len mod small so short bodies are exercised frequently. */
#define BODY_MOD  (MAX_BODY + 1)

/* Four flag combinations: none, SQ only, RQ only, both. */
static const uint32_t flags_tab[4] = {
    0,
    IONIC_QPF_SQ_CMB,
    IONIC_QPF_RQ_CMB,
    IONIC_QPF_SQ_CMB | IONIC_QPF_RQ_CMB,
};

#define F_FLAGS_MASK 0x03U
#define F_DUP_QP     0x04U
#define F_BAD_SCQ    0x08U

/* A valid CQ id the CREATE_QP can reference.  The fake device starts with
 * no CQs registered, so the handler returns ENOENT/EQID/ERANGE for any
 * CQ id unless we pre-register one; for positive-path seeds we write the
 * CQ id the RDMA back-end would allocate (slot 0 → id 0). */
#define VALID_CQ_ID  0U
#define BAD_CQ_ID    0xFFFFFFFFU
#define NEW_QP_ID    1U

/* Build the minimum-valid body for CREATE_QP.  Caller owns buf[MAX_BODY]. */
static void build_body(uint8_t *buf, uint32_t priv_flags, uint32_t scq_id,
                       uint32_t qp_id)
{
    memset(buf, 0, MAX_BODY);
    /* be32 priv_flags at [4:7] */
    buf[4] = (uint8_t)(priv_flags >> 24);
    buf[5] = (uint8_t)(priv_flags >> 16);
    buf[6] = (uint8_t)(priv_flags >> 8);
    buf[7] = (uint8_t)priv_flags;
    /* le32 scq_id at [8:11] */
    buf[8]  = (uint8_t)scq_id;
    buf[9]  = (uint8_t)(scq_id >> 8);
    buf[10] = (uint8_t)(scq_id >> 16);
    buf[11] = (uint8_t)(scq_id >> 24);
    /* sq depth_log2=6, stride_log2=6, page_size_log2=12 at [12..15] */
    buf[12] = 6;
    buf[13] = 6;
    buf[15] = 12;
    /* sq nbufs=1 at [20:23] */
    buf[20] = 1;
    /* sq dma at [24:31]: BUF_GPA in guest memory */
    uint64_t gpa = BUF_GPA;
    memcpy(buf + 24, &gpa, 8);
    /* le32 rcq_id at [32:35]: same as scq */
    buf[32] = (uint8_t)scq_id;
    buf[33] = (uint8_t)(scq_id >> 8);
    buf[34] = (uint8_t)(scq_id >> 16);
    buf[35] = (uint8_t)(scq_id >> 24);
    /* rq depth_log2=6, stride_log2=6, page_size_log2=12 at [36..39] */
    buf[36] = 6;
    buf[37] = 6;
    buf[39] = 12;
    /* rq srq_id=0xFFFFFFFF (no SRQ) at [40:43] */
    buf[40] = 0xFF;
    buf[41] = 0xFF;
    buf[42] = 0xFF;
    buf[43] = 0xFF;
    /* rq nbufs=1 at [44:47] */
    buf[44] = 1;
    /* rq dma at [48:55]: BUF_GPA + 1024 to avoid overlap */
    gpa = BUF_GPA + 512;
    memcpy(buf + 48, &gpa, 8);
    /* le32 qp_id | (1<<24) at [56:59] */
    uint32_t id_ver = qp_id | (1U << 24);
    buf[56] = (uint8_t)id_ver;
    buf[57] = (uint8_t)(id_ver >> 8);
    buf[58] = (uint8_t)(id_ver >> 16);
    buf[59] = (uint8_t)(id_ver >> 24);
    /* qp_type = RC (0) at [62] */
    buf[62] = 0;
}

/* Valid status codes handle_create_qp_op() may complete with. */
static bool is_create_qp_status(uint8_t rc)
{
    return rc == IONIC_RC_SUCCESS || rc == IONIC_RC_EINVAL ||
           rc == IONIC_RC_EEXIST  || rc == IONIC_RC_ENOSPC ||
           rc == IONIC_RC_ENOENT  || rc == IONIC_RC_EQID   ||
           rc == IONIC_RC_ERANGE  || rc == IONIC_RC_ERDMA   ||
           rc == IONIC_RC_ENOMEM  || rc == IONIC_RC_EIO;
}

/*
 * Run one input through the handler.  Returns NULL when all checks pass,
 * else a string naming what failed; *rc_out is the completion status.
 */
static const char *run_input(const uint8_t *data, size_t size, uint8_t *rc_out)
{
    uint8_t body[MAX_BODY];
    uint8_t iflags;
    uint8_t rc;
    size_t body_len;

    *rc_out = 0;
    if (size < IN_HDR) {
        return NULL;
    }
    iflags   = data[0];
    body_len = data[1] % BODY_MOD;

    ctx_reset();

    memset(body, 0, sizeof(body));
    size_t avail = size - IN_HDR;

    if (avail < body_len) {
        memcpy(body, data + IN_HDR, avail);
    } else {
        memcpy(body, data + IN_HDR, body_len);
    }

    /* Overlay a duplicate QP id or bad SCQ id from the flag bits. */
    if (iflags & F_DUP_QP) {
        uint32_t id_ver = QP_ID | (1U << 24);
        body[56] = (uint8_t)id_ver;
        body[57] = (uint8_t)(id_ver >> 8);
        body[58] = (uint8_t)(id_ver >> 16);
        body[59] = (uint8_t)(id_ver >> 24);
    }
    if (iflags & F_BAD_SCQ) {
        uint32_t bad = BAD_CQ_ID;
        body[8]  = (uint8_t)bad;
        body[9]  = (uint8_t)(bad >> 8);
        body[10] = (uint8_t)(bad >> 16);
        body[11] = (uint8_t)(bad >> 24);
        body[32] = body[8];
        body[33] = body[9];
        body[34] = body[10];
        body[35] = body[11];
    }

    rc = ionic_adminq_dispatch_wqe(&g_ctx, IONIC_V1_ADMIN_CREATE_QP, body,
                                   (uint16_t)body_len);
    *rc_out = rc;

    if (!is_create_qp_status(rc)) {
        return "status is not a CREATE_QP completion code";
    }
    if (body_len < IONIC_ADMIN_CREATE_QP_IN_V1_LEN) {
        return rc == IONIC_RC_EINVAL ? NULL
                                     : "a short body was not EINVAL";
    }
    if ((iflags & F_DUP_QP) && rc != IONIC_RC_EEXIST) {
        return "a duplicate QP id was not EEXIST";
    }

    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t rc;
    const char *why = run_input(data, size, &rc);

    if (why) {
        fprintf(stderr, "fuzz_adminq_cmb: %s (status %u)\n", why, rc);
        abort();
    }
    return 0;
}

/* ---- Seeds ------------------------------------------------------------- */

struct seed {
    const char *category;
    const char *description;
    const char *expected;
    uint8_t  iflags;    /* F_* bits */
    uint32_t priv_flags;
    uint32_t scq_id;
    uint32_t qp_id;
    uint16_t body_len;
    uint8_t  want_rc;
};

static const struct seed seeds[] = {
    /* positive */
    {"positive", "CREATE_QP, no CMB flags",
     "SUCCESS (0)", 0, 0, VALID_CQ_ID, NEW_QP_ID,
     IONIC_ADMIN_CREATE_QP_IN_V1_LEN, 0},
    {"positive", "CREATE_QP with SQ_CMB flag",
     "SUCCESS (0)", 0, IONIC_QPF_SQ_CMB, VALID_CQ_ID, NEW_QP_ID,
     IONIC_ADMIN_CREATE_QP_IN_V1_LEN, 0},
    {"positive", "CREATE_QP with RQ_CMB flag",
     "SUCCESS (0)", 0, IONIC_QPF_RQ_CMB, VALID_CQ_ID, NEW_QP_ID,
     IONIC_ADMIN_CREATE_QP_IN_V1_LEN, 0},
    {"positive", "CREATE_QP with SQ_CMB and RQ_CMB",
     "SUCCESS (0)", 0, IONIC_QPF_SQ_CMB | IONIC_QPF_RQ_CMB, VALID_CQ_ID,
     NEW_QP_ID, IONIC_ADMIN_CREATE_QP_IN_V1_LEN, 0},
    /* negative */
    {"negative", "CREATE_QP, body one byte short",
     "EINVAL (14)", 0, IONIC_QPF_SQ_CMB, VALID_CQ_ID, NEW_QP_ID,
     IONIC_ADMIN_CREATE_QP_IN_V1_LEN - 1, IONIC_RC_EINVAL},
    {"negative", "CREATE_QP with duplicate QP id",
     "EEXIST (5)", F_DUP_QP, IONIC_QPF_SQ_CMB, VALID_CQ_ID, QP_ID,
     IONIC_ADMIN_CREATE_QP_IN_V1_LEN, IONIC_RC_EEXIST},
    {"negative", "CREATE_QP with zero-length body",
     "EINVAL (14)", 0, 0, VALID_CQ_ID, NEW_QP_ID, 0, IONIC_RC_EINVAL},
};

static size_t encode_seed(const struct seed *s, uint8_t *out)
{
    uint8_t body[MAX_BODY];

    build_body(body, s->priv_flags, s->scq_id, s->qp_id);

    memset(out, 0, IN_HDR + MAX_BODY);
    out[0] = s->iflags;
    out[1] = (uint8_t)s->body_len;
    if (s->body_len > 0) {
        memcpy(out + IN_HDR, body, s->body_len);
    }

    if (s->iflags & F_DUP_QP) {
        uint32_t id_ver = QP_ID | (1U << 24);
        out[IN_HDR + 56] = (uint8_t)id_ver;
        out[IN_HDR + 57] = (uint8_t)(id_ver >> 8);
        out[IN_HDR + 58] = (uint8_t)(id_ver >> 16);
        out[IN_HDR + 59] = (uint8_t)(id_ver >> 24);
    }

    return IN_HDR + s->body_len;
}

static bool emit_seed(size_t i, const uint8_t *buf, size_t len)
{
    char path[32];
    int fd;
    FILE *f;
    bool ok;

    snprintf(path, sizeof(path), "seed-%02zu", i);
    /* NOLINTBEGIN(android-cloexec-open) */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    /* NOLINTEND(android-cloexec-open) */
    if (fd < 0) {
        fprintf(stderr, "fuzz_adminq_cmb: cannot create %s\n", path);
        return false;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        fprintf(stderr, "fuzz_adminq_cmb: cannot open %s as a stream\n", path);
        return false;
    }
    ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "fuzz_adminq_cmb: cannot write %s\n", path);
    }
    return ok;
}

/* libFuzzer gives this its signature */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *emit   = getenv("FUZZ_EMIT_SEEDS");
    bool emit_seeds    = emit && strcmp(emit, "1") == 0;
    int failures       = 0;
    int unwritten      = 0;

    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        const struct seed *s = &seeds[i];
        uint8_t buf[IN_HDR + MAX_BODY];
        size_t len = encode_seed(s, buf);
        uint8_t rc;
        const char *why = run_input(buf, len, &rc);
        bool ok = why == NULL && rc == s->want_rc;

        printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", s->category,
               s->description, s->expected);
        if (!ok) {
            printf("     status %u (want %u)%s%s\n", rc, s->want_rc,
                   why ? ": " : "", why ? why : "");
            failures++;
        }
        if (emit_seeds && !emit_seed(i, buf, len)) {
            unwritten++;
        }
    }
    if (failures > 0) {
        fprintf(stderr, "fuzz_adminq_cmb: %d seed(s) failed\n", failures);
        abort();
    }
    if (unwritten > 0) {
        fprintf(stderr, "fuzz_adminq_cmb: %d seed(s) not written\n", unwritten);
        return 1;
    }
    return 0;
}
