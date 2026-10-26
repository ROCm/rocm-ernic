/*
 * Guest-side end-to-end test of the P4 format conversion, and its
 * throughput.
 *
 * The sibling of p4_fp8_client.c: that one makes an RDMA write a
 * reduction, this one makes it a conversion. It opens /dev/ernic-p4,
 * pushes the compiled conversion program down in chunks, installs a
 * table entry naming a source and a destination format, and then posts
 * an ordinary RDMA WRITE whose gather list has a single entry. The
 * program's table turns that into an elementwise conversion, and the
 * result -- longer, shorter or the same length as the source, depending
 * on the pair -- lands in the region the work request named.
 *
 * Nothing here calls the engine's C API, because a guest cannot: every
 * control-plane step goes through the character device and the data-path
 * step is plain verbs. The expected values come from p4_fmt.c, compiled
 * in, so they are the engine's own arithmetic rather than a table
 * written to suit it.
 *
 * With --csv it also measures throughput and writes the rows
 * ci/report/gen-report.py reads out of <results>/perf-csv/.
 *
 * Exits 0 on success, 77 when there is nothing to run against (no
 * emulated device, or no /dev/ernic-p4 because the server was started
 * without --p4-guest-scope), and 1 on any failure.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#include "ernic_device.h"
#include "ernic_p4.h"
#include "p4_devcmd.h"
#include "p4_fmt.h"
#include "parse_int.h"

/* The device opcode an RDMA WRITE arrives as; see src/ionic_datapath.c. */
#define IONIC_V1_OP_RDMA_WRITE 4

#define P4_NODE "/dev/ernic-p4"

/*
 * One region each way, big enough for the largest measured size at the
 * widest ratio: 64 KiB of fp32 source is 64 KiB, and 64 KiB of fp4
 * source widens to eight times that.
 */
#define REGION_LEN (1024u * 1024u)

static int p4_fd = -1;
static unsigned g_pass, g_fail;

static void ok(const char *what)
{
    printf("ok   %s\n", what);
    g_pass++;
}

static void bad(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail++;
}

/* ---------------------------------------------------------------- */
/* control plane                                                     */
/* ---------------------------------------------------------------- */

static int rpc_call(uint32_t ep, uint32_t op, uint32_t scope, const void *req,
                    uint32_t req_len, void *resp, uint32_t resp_len)
{
    struct ernic_p4_rpc r;

    memset(&r, 0, sizeof(r));
    r.req = (uint64_t)(uintptr_t)req;
    r.req_len = req_len;
    r.resp = (uint64_t)(uintptr_t)resp;
    r.resp_len = resp_len;
    r.ep = ep;
    r.op = op;
    r.scope = scope;

    if (ioctl(p4_fd, ERNIC_P4_IOC_RPC, &r) != 0) {
        fprintf(stderr, "ioctl RPC ep=%u op=%u: %s\n", ep, op, strerror(errno));
        return -1;
    }
    if (r.status != P4_STS_OK) {
        fprintf(stderr, "RPC ep=%u op=%u refused by the device: status %u\n",
                ep, op, r.status);
        return -1;
    }
    return 0;
}

static int load_program(const char *path)
{
    struct p4_rpc_load_begin begin;
    uint8_t *chunk;
    FILE *f;
    long size;
    size_t off = 0;
    int rc = -1;

    f = fopen(path, "rbe"); /* e: O_CLOEXEC */
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot size %s\n", path);
        fclose(f);
        return -1;
    }

    /* The header plus the bytes travel in one buffer, so the device
     * reads both with one DMA and a chunk cannot name memory its own
     * header did not. */
    chunk = calloc(1, sizeof(struct p4_rpc_load_chunk) + 4096);
    if (!chunk) {
        fclose(f);
        return -1;
    }

    memset(&begin, 0, sizeof(begin));
    begin.total_len = (uint32_t)size;
    if (rpc_call(P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, P4_SCOPE_LOAD, &begin,
                 (uint32_t)sizeof(begin), NULL, 0) != 0)
        goto out;

    while (off < (size_t)size) {
        struct p4_rpc_load_chunk hdr;
        size_t n = (size_t)size - off;

        if (n > 4096)
            n = 4096;
        if (fread(chunk + sizeof(hdr), 1, n, f) != n) {
            fprintf(stderr, "short read of %s at %zu\n", path, off);
            goto out;
        }
        memset(&hdr, 0, sizeof(hdr));
        hdr.offset = off;
        hdr.len = (uint32_t)n;
        memcpy(chunk, &hdr, sizeof(hdr));

        if (rpc_call(P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, P4_SCOPE_LOAD, chunk,
                     (uint32_t)(sizeof(hdr) + n), NULL, 0) != 0)
            goto out;
        off += n;
    }

    if (rpc_call(P4_EP_PROGRAM, P4_OP_LOAD_COMMIT, P4_SCOPE_LOAD, NULL, 0, NULL,
                 0) != 0)
        goto out;

    printf("# loaded %s (%ld bytes)\n", path, size);
    rc = 0;
out:
    free(chunk);
    fclose(f);
    return rc;
}

/* Install or change the entry that says which conversion to apply. */
static int select_conversion(uint8_t type, uint8_t src_fmt, uint8_t dst_fmt)
{
    uint8_t body[sizeof(struct p4_rpc_entry_update) +
                 sizeof(struct p4_rpc_entry_match) +
                 2 * sizeof(struct p4_rpc_entry_param)];
    struct p4_rpc_entry_update u;
    struct p4_rpc_entry_match m;
    struct p4_rpc_entry_param a, b;
    size_t off;

    memset(body, 0, sizeof(body));
    memset(&u, 0, sizeof(u));
    memset(&m, 0, sizeof(m));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    u.type = type;
    u.nmatch = 1;
    u.nparams = 2;
    (void)snprintf(u.table, sizeof(u.table), "Ing.policy");
    (void)snprintf(u.action, sizeof(u.action), "Ing.convert");

    /* Values are right-aligned in the field, as everywhere else in the
     * engine: a one-byte key is the last byte. */
    m.len = 1;
    m.value[P4_DEVCMD_VALUE_MAX - 1] = IONIC_V1_OP_RDMA_WRITE;
    a.len = 1;
    a.value[P4_DEVCMD_VALUE_MAX - 1] = src_fmt;
    b.len = 1;
    b.value[P4_DEVCMD_VALUE_MAX - 1] = dst_fmt;

    off = 0;
    memcpy(body + off, &u, sizeof(u));
    off += sizeof(u);
    memcpy(body + off, &m, sizeof(m));
    off += sizeof(m);
    memcpy(body + off, &a, sizeof(a));
    off += sizeof(a);
    memcpy(body + off, &b, sizeof(b));
    off += sizeof(b);

    return rpc_call(P4_EP_TABLE, P4_OP_ENTRY_UPDATE, P4_SCOPE_WRITE, body,
                    (uint32_t)off, NULL, 0);
}

static int poll_one(struct ibv_cq *cq, struct ibv_wc *wc)
{
    for (int i = 0; i < 200000; i++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
        int n = ibv_poll_cq(cq, 1, wc);

        if (n < 0)
            return -1;
        if (n > 0)
            return 0;
        nanosleep(&ts, NULL);
    }
    return -1;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Deterministic source bytes, so a run is repeatable. */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1u;

    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)(x >> 7);
    }
}

/* ---------------------------------------------------------------- */

struct ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qpa, *qpb;
    uint8_t *src, *dst, *want;
    struct ibv_mr *mr_src, *mr_dst;
};

/*
 * Run one conversion and return the completion status. @in_len is the
 * source length in bytes.
 */
static int run_convert(struct ctx *c, uint32_t in_len,
                       enum ibv_wc_status *status)
{
    struct ibv_sge sge;
    struct ibv_send_wr swr, *bad_wr = NULL;
    struct ibv_wc wc;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uint64_t)(uintptr_t)c->src;
    sge.length = in_len;
    sge.lkey = c->mr_src->lkey;

    memset(&swr, 0, sizeof(swr));
    swr.wr_id = 0x43565400;
    swr.opcode = IBV_WR_RDMA_WRITE;
    swr.send_flags = IBV_SEND_SIGNALED;
    swr.sg_list = &sge;
    swr.num_sge = 1;
    swr.wr.rdma.remote_addr = (uint64_t)(uintptr_t)c->dst;
    swr.wr.rdma.rkey = c->mr_dst->rkey;

    if (ibv_post_send(c->qpa, &swr, &bad_wr))
        return -1;
    if (poll_one(c->cq, &wc) != 0)
        return -2;
    *status = wc.status;
    return 0;
}

/* One correctness case: convert @n elements and compare against p4_fmt. */
static void case_convert(struct ctx *c, enum p4_fmt src, enum p4_fmt dst,
                         size_t n)
{
    size_t in_len = p4_fmt_bytes(src, n);
    size_t out_len = p4_fmt_bytes(dst, n);
    enum ibv_wc_status st;
    char what[160];

    (void)snprintf(what, sizeof(what),
                   "%s -> %s: %zu elements, %zu bytes in, %zu out",
                   p4_fmt_name(src), p4_fmt_name(dst), n, in_len, out_len);

    if (in_len == 0 || out_len == 0 || in_len > REGION_LEN ||
        out_len > REGION_LEN) {
        bad("%s: the fixture does not fit", what);
        return;
    }

    fill_pattern(c->src, in_len, (uint32_t)(src * 31u + dst * 7u + n));
    if (!p4_fmt_convert(src, dst, c->src, n, c->want)) {
        bad("%s: the oracle refused it", what);
        return;
    }
    /* Poison past the result too, so a device that wrote the source
     * length rather than the destination length is caught. */
    memset(c->dst, 0xA5, REGION_LEN);

    if (select_conversion(P4_WIRE_UPDATE_MODIFY, (uint8_t)src, (uint8_t)dst) !=
        0) {
        bad("%s: the entry was not modified", what);
        return;
    }
    if (run_convert(c, (uint32_t)in_len, &st) != 0) {
        bad("%s: post or poll failed", what);
        return;
    }
    if (st != IBV_WC_SUCCESS) {
        bad("%s: completion %s", what, ibv_wc_status_str(st));
        return;
    }
    if (memcmp(c->dst, c->want, out_len) != 0) {
        size_t i;

        for (i = 0; i < out_len; i++)
            if (c->dst[i] != c->want[i])
                break;
        bad("%s: wrong at byte %zu (got %#x want %#x)", what, i, c->dst[i],
            c->want[i]);
        return;
    }
    for (size_t i = out_len; i < REGION_LEN; i++) {
        if (c->dst[i] != 0xA5u) {
            bad("%s: wrote past the end of the result at byte %zu", what, i);
            return;
        }
    }
    ok(what);
}

/*
 * Throughput, written in the shape ci/report/gen-report.py reads.
 *
 * The rate is reported over the SOURCE bytes, which is the side a
 * caller sizes its buffer by, and the size column is the source length
 * so the series lines up with the other bandwidth sweeps.
 */
static void bench(struct ctx *c, FILE *csv, enum p4_fmt src, enum p4_fmt dst,
                  size_t in_len, uint32_t iters, uint32_t runs)
{
    size_t n = p4_fmt_elems(src, in_len);

    /* Every way out of here without a measurement goes through bad(): a
     * size the sweep asked for and did not measure is a partial sweep,
     * and main() must not publish one as a good run. */
    if (n == 0 || p4_fmt_bytes(dst, n) > REGION_LEN) {
        bad("bench: %zu bytes of %s does not fit", in_len, p4_fmt_name(src));
        return;
    }
    if (select_conversion(P4_WIRE_UPDATE_MODIFY, (uint8_t)src, (uint8_t)dst) !=
        0) {
        bad("bench: the %s->%s entry was not modified", p4_fmt_name(src),
            p4_fmt_name(dst));
        return;
    }
    fill_pattern(c->src, in_len, 1);

    /* One pass to take the first-touch costs out of the measurement. */
    for (unsigned i = 0; i < 8; i++) {
        enum ibv_wc_status st;

        if (run_convert(c, (uint32_t)in_len, &st) != 0 ||
            st != IBV_WC_SUCCESS) {
            bad("bench: warm-up failed at %zu bytes", in_len);
            return;
        }
    }

    for (uint32_t r = 0; r < runs; r++) {
        double t0 = now_s(), dt, gbs, mpps;

        for (uint32_t i = 0; i < iters; i++) {
            enum ibv_wc_status st;

            if (run_convert(c, (uint32_t)in_len, &st) != 0 ||
                st != IBV_WC_SUCCESS) {
                bad("bench: a conversion failed at %zu bytes", in_len);
                return;
            }
        }
        dt = now_s() - t0;
        if (dt <= 0.0)
            continue;
        gbs = ((double)in_len * iters) / dt / 1e9;
        mpps = (double)iters / dt / 1e6;
        printf("# bench %s->%s %zu B: %.3f GB/s, %.4f Mops/s\n",
               p4_fmt_name(src), p4_fmt_name(dst), in_len, gbs, mpps);
        if (csv)
            fprintf(csv, "p4cvt,%zu,%.6f,%.6f,%.6f\n", in_len, gbs, gbs, mpps);
    }
}

int main(int argc, char **argv)
{
    const char *prog = "fp8_convert.json";
    const char *csv_path = NULL;
    uint32_t iters = 200, runs = 5;
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    enum ernic_device_result lookup;
    struct ctx c;
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr attr;
    union ibv_gid gid;
    struct p4_devcmd_ident devid;
    struct ernic_p4_ident req;
    FILE *csv = NULL;
    int ndev = 0, ret = 1;

    memset(&c, 0, sizeof(c));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
            csv_path = argv[++i];
        else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            if (!ernic_parse_u32(argv[++i], &iters) || iters == 0) {
                fprintf(stderr, "--iters wants a positive count, not '%s'\n",
                        argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            if (!ernic_parse_u32(argv[++i], &runs) || runs == 0) {
                fprintf(stderr, "--runs wants a positive count, not '%s'\n",
                        argv[i]);
                return 1;
            }
        } else
            prog = argv[i];
    }

    dev_list = ibv_get_device_list(&ndev);
    if (!dev_list || ndev < 1) {
        lookup = ernic_requested_device() ? ERNIC_DEVICE_MISMATCH
                                          : ERNIC_DEVICE_ABSENT;
        ernic_report_no_device(lookup);
        if (dev_list)
            ibv_free_device_list(dev_list);
        return lookup == ERNIC_DEVICE_MISMATCH ? 1 : 77;
    }
    ibdev = ernic_find_device(dev_list, ndev, &lookup);
    if (!ibdev) {
        ernic_report_no_device(lookup);
        ibv_free_device_list(dev_list);
        return lookup == ERNIC_DEVICE_MISMATCH ? 1 : 77;
    }

    p4_fd = open(P4_NODE, O_RDWR | O_CLOEXEC);
    if (p4_fd < 0) {
        int err = errno;

        fprintf(stderr, "%s: %s\n", P4_NODE, strerror(err));
        if (err == ENOENT)
            fprintf(stderr,
                    "  the server was started without --p4-guest-scope, "
                    "or ernic_p4 is not loaded\n");
        else if (err == EACCES || err == EPERM)
            fprintf(stderr, "  the node is owned by root and mode 0600; "
                            "run this as root\n");
        ibv_free_device_list(dev_list);
        return err == ENOENT ? 77 : 1;
    }

    memset(&devid, 0, sizeof(devid));
    memset(&req, 0, sizeof(req));
    req.buf = (uint64_t)(uintptr_t)&devid;
    req.len = (uint32_t)sizeof(devid);
    if (ioctl(p4_fd, ERNIC_P4_IOC_IDENT, &req) != 0 || req.status != 0) {
        fprintf(stderr, "IDENT failed (status %u): %s\n", req.status,
                strerror(errno));
        goto out;
    }
    printf("# abi=%u scope=%u max_program=%u\n", devid.abi_version, devid.scope,
           devid.max_program);
    if (devid.scope < P4_SCOPE_LOAD) {
        fprintf(stderr,
                "the operator granted scope %u; this test needs %u "
                "(--p4-guest-scope load)\n",
                devid.scope, (unsigned)P4_SCOPE_LOAD);
        goto out;
    }

    if (load_program(prog) != 0)
        goto out;
    /* One entry, modified per case; inserting it once is what makes the
     * rest of the run a control-plane update rather than a reload. */
    if (select_conversion(P4_WIRE_UPDATE_INSERT, P4_FMT_FP16,
                          P4_FMT_FP8_E4M3) != 0) {
        fprintf(stderr, "could not install the conversion entry\n");
        goto out;
    }

    c.ctx = ibv_open_device(ibdev);
    if (!c.ctx) {
        perror("ibv_open_device");
        goto out;
    }
    printf("# device=%s\n", ibv_get_device_name(ibdev));

    c.pd = ibv_alloc_pd(c.ctx);
    c.cq = ibv_create_cq(c.ctx, 64, NULL, NULL, 0);
    if (!c.pd || !c.cq) {
        fprintf(stderr, "pd/cq allocation failed\n");
        goto out;
    }
    if (posix_memalign((void **)&c.src, 4096, REGION_LEN) ||
        posix_memalign((void **)&c.dst, 4096, REGION_LEN)) {
        fprintf(stderr, "posix_memalign failed\n");
        goto out;
    }
    c.want = malloc(REGION_LEN);
    if (!c.want) {
        fprintf(stderr, "malloc failed\n");
        goto out;
    }
    memset(c.src, 0, REGION_LEN);
    memset(c.dst, 0, REGION_LEN);

    c.mr_src = ibv_reg_mr(c.pd, c.src, REGION_LEN, IBV_ACCESS_LOCAL_WRITE);
    c.mr_dst = ibv_reg_mr(c.pd, c.dst, REGION_LEN,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!c.mr_src || !c.mr_dst) {
        fprintf(stderr, "ibv_reg_mr failed\n");
        goto out;
    }

    memset(&init, 0, sizeof(init));
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 16;
    init.cap.max_recv_wr = 16;
    init.cap.max_send_sge = 2;
    init.cap.max_recv_sge = 2;
    init.send_cq = c.cq;
    init.recv_cq = c.cq;
    c.qpa = ibv_create_qp(c.pd, &init);
    c.qpb = ibv_create_qp(c.pd, &init);
    if (!c.qpa || !c.qpb) {
        perror("ibv_create_qp");
        goto out;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(c.qpa, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) ||
        ibv_modify_qp(c.qpb, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "INIT transition failed\n");
        goto out;
    }
    if (ibv_query_gid(c.ctx, 1, 0, &gid) != 0) {
        fprintf(stderr, "ibv_query_gid failed\n");
        goto out;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.rq_psn = 1;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.port_num = 1;
    {
        int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                   IBV_QP_MIN_RNR_TIMER;

        attr.dest_qp_num = c.qpb->qp_num;
        if (ibv_modify_qp(c.qpa, &attr, mask)) {
            fprintf(stderr, "RTR qpa failed\n");
            goto out;
        }
        attr.dest_qp_num = c.qpa->qp_num;
        if (ibv_modify_qp(c.qpb, &attr, mask)) {
            fprintf(stderr, "RTR qpb failed\n");
            goto out;
        }
    }
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 1;
    attr.max_rd_atomic = 1;
    {
        int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                   IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

        if (ibv_modify_qp(c.qpa, &attr, mask) ||
            ibv_modify_qp(c.qpb, &attr, mask)) {
            fprintf(stderr, "RTS transition failed\n");
            goto out;
        }
    }

    /*
     * Correctness first, and in both directions. Narrowing is what a
     * collective does on the way out; widening is what it does on the
     * way back, and the earlier interface could not express it at all.
     */
    case_convert(&c, P4_FMT_FP16, P4_FMT_FP8_E4M3, 2048); /* narrow */
    case_convert(&c, P4_FMT_BF16, P4_FMT_FP8_E4M3, 2048); /* narrow */
    case_convert(&c, P4_FMT_FP32, P4_FMT_FP8_E5M2, 2048); /* narrow, 4:1 */
    case_convert(&c, P4_FMT_FP16, P4_FMT_FP4_E2M1, 2048); /* narrow to 4 bit */
    case_convert(&c, P4_FMT_FP32, P4_FMT_FP4_E2M1, 2048); /* narrow, 8:1 */
    case_convert(&c, P4_FMT_FP4_E2M1, P4_FMT_FP16, 2048); /* widen from 4 bit */
    case_convert(&c, P4_FMT_FP4_E2M1, P4_FMT_FP32, 2048); /* widen, 1:8 */
    case_convert(&c, P4_FMT_FP8_E4M3, P4_FMT_BF16, 2048); /* widen */
    case_convert(&c, P4_FMT_FP8_E4M3, P4_FMT_FP8_E5M2, 2048); /* same width */
    /* An odd number of 4-bit elements is half a byte at one end. */
    case_convert(&c, P4_FMT_FP16, P4_FMT_FP8_E4M3, 1);

    if (g_fail == 0 && csv_path) {
        /* open() rather than fopen(), which would create the file with
         * mode 0666 less the umask.  With _FORTIFY_SOURCE, glibc 2.40 and
         * later give Clang an open(path, flags, mode) overload, and
         * android-cloexec-open looks for O_CLOEXEC in its mode */
        /* NOLINTBEGIN(android-cloexec-open) */
        int fd = open(csv_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        /* NOLINTEND(android-cloexec-open) */

        int err = errno;

        if (fd >= 0) {
            csv = fdopen(fd, "w");
            err = errno;
            if (!csv)
                (void)close(fd);
        }
        if (!csv) {
            fprintf(stderr, "open %s: %s\n", csv_path, strerror(err));
            goto out;
        }
        fprintf(csv, "verb,size,bw_peak_GBs,bw_avg_GBs,msg_rate_mpps\n");
        /*
         * 4 KiB and 64 KiB of the tracked sizes. 1 MiB is not measured:
         * a conversion is capped at 256 Ki elements, which 1 MiB of a
         * 16-bit source is twice over.
         */
        bench(&c, csv, P4_FMT_FP16, P4_FMT_FP8_E4M3, 4096, iters, runs);
        bench(&c, csv, P4_FMT_FP16, P4_FMT_FP8_E4M3, 65536, iters, runs);
        fclose(csv);
        csv = NULL;
        if (g_fail) {
            /* A half-measured sweep must not be published as a good run,
             * so the file goes with the failure that produced it. */
            (void)remove(csv_path);
            printf("# removed %s: the sweep did not complete\n", csv_path);
        } else {
            printf("# wrote %s\n", csv_path);
        }
    }

    printf("# %u passed, %u failed\n", g_pass, g_fail);
    ret = g_fail ? 1 : 0;

out:
    if (csv)
        fclose(csv);
    if (c.qpa)
        ibv_destroy_qp(c.qpa);
    if (c.qpb)
        ibv_destroy_qp(c.qpb);
    if (c.mr_src)
        ibv_dereg_mr(c.mr_src);
    if (c.mr_dst)
        ibv_dereg_mr(c.mr_dst);
    free(c.src);
    free(c.dst);
    free(c.want);
    if (c.cq)
        ibv_destroy_cq(c.cq);
    if (c.pd)
        ibv_dealloc_pd(c.pd);
    if (c.ctx)
        ibv_close_device(c.ctx);
    if (p4_fd >= 0)
        close(p4_fd);
    ibv_free_device_list(dev_list);
    return ret;
}
