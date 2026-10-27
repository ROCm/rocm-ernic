/*
 * Guest-side end-to-end test of the P4 fp8 reduction.
 *
 * This is the program the whole P4 stack exists to make work, run from
 * where it has to work: inside a guest, against the emulated NIC, with
 * nothing in the process that is not a guest's to use.
 *
 * It opens /dev/ernic-p4, pushes a compiled P4 program down in chunks,
 * installs one table entry, and then posts an ordinary RDMA WRITE whose
 * gather list names two operand regions. The program's table turns that
 * into an elementwise fp8 add, and the result lands in a third region
 * the work request named as its destination. Nothing here calls the
 * engine's C API, because a guest cannot: every control-plane step goes
 * through the character device, and the data-path step is plain verbs.
 *
 * What it proves that the in-process tests cannot: that the convention
 * the device enforces is one a real driver can satisfy. The work
 * request's length field is whatever ionic_prep_common() puts there --
 * the sum of the gather list -- because libibverbs gives no way to set
 * it, which is the point.
 *
 * Exits 0 on success, 77 when there is nothing to run against (no
 * emulated device, or no /dev/ernic-p4 because the server was started
 * without --p4-guest-scope), and 1 on any failure. Set $RDMA_DEVICE to
 * pin the device; a named-but-absent device is then a failure rather
 * than a skip, as in the other guest tests.
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
#include "p4/p4_devcmd.h"
#include "p4/p4_fp8.h"

/* The device opcode an RDMA WRITE arrives as (src/ionic/ionic_datapath.c). */
#define IONIC_V1_OP_RDMA_WRITE 4

#define P4_NODE "/dev/ernic-p4"

/*
 * One page per region, and page-aligned. The emulated MR mapping
 * rejects a region whose chunk count does not divide the page-rounded
 * length, which is why the other guest tests do the same.
 */
#define REGION_LEN 4096u

/* fp8 E4M3 encodings, and what the reduction should make of them. */
#define FP8_1_0 0x38u
#define FP8_2_0 0x40u
#define FP8_3_0 0x44u
#define FP8_4_0 0x48u
#define FP8_6_0 0x4Cu

/* The element given a different value, so a result that ignored
 * position could not pass. */
#define ODD_ELEMENT 1234u

static int p4_fd = -1;

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

/* Push a compiled bmv2 program down in chunks, as a control plane would. */
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

    printf("loaded %s (%ld bytes)\n", path, size);
    rc = 0;
out:
    free(chunk);
    fclose(f);
    return rc;
}

/* Install the one entry that makes an RDMA write a reduction. */
static int install_entry(uint8_t fp8_op)
{
    uint8_t body[sizeof(struct p4_rpc_entry_update) +
                 sizeof(struct p4_rpc_entry_match) +
                 sizeof(struct p4_rpc_entry_param)];
    struct p4_rpc_entry_update u;
    struct p4_rpc_entry_match m;
    struct p4_rpc_entry_param a;

    memset(body, 0, sizeof(body));
    memset(&u, 0, sizeof(u));
    memset(&m, 0, sizeof(m));
    memset(&a, 0, sizeof(a));

    u.type = P4_WIRE_UPDATE_INSERT;
    u.nmatch = 1;
    u.nparams = 1;
    (void)snprintf(u.table, sizeof(u.table), "Ing.policy");
    (void)snprintf(u.action, sizeof(u.action), "Ing.reduce_with");

    /* Values are right-aligned in the field, as everywhere else in the
     * engine: a one-byte key is the last byte. */
    m.len = 1;
    m.value[P4_DEVCMD_VALUE_MAX - 1] = IONIC_V1_OP_RDMA_WRITE;
    a.len = 1;
    a.value[P4_DEVCMD_VALUE_MAX - 1] = fp8_op;

    memcpy(body, &u, sizeof(u));
    memcpy(body + sizeof(u), &m, sizeof(m));
    memcpy(body + sizeof(u) + sizeof(m), &a, sizeof(a));

    return rpc_call(P4_EP_TABLE, P4_OP_ENTRY_UPDATE, P4_SCOPE_WRITE, body,
                    (uint32_t)sizeof(body), NULL, 0);
}

static int poll_one(struct ibv_cq *cq, struct ibv_wc *wc)
{
    for (int i = 0; i < 100000; i++) {
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

int main(int argc, char **argv)
{
    const char *prog = argc > 1 ? argv[1] : "fp8_policy.json";
    struct ibv_device **dev_list = NULL;
    struct ibv_device *ibdev;
    enum ernic_device_result lookup;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qpa = NULL, *qpb = NULL;
    struct ibv_mr *mr_a = NULL, *mr_b = NULL, *mr_c = NULL;
    uint8_t *buf_a = NULL, *buf_b = NULL, *buf_c = NULL;
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr attr;
    struct ibv_sge sge[2];
    struct ibv_send_wr swr, *bad = NULL;
    struct ibv_wc wc;
    union ibv_gid gid;
    struct ernic_p4_ident id;
    unsigned wrong = 0, first = 0;
    int ndev = 0;
    int ret = 1;

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
        /* A named-but-absent device is a real failure, not a skip. */
        return lookup == ERNIC_DEVICE_MISMATCH ? 1 : 77;
    }

    p4_fd = open(P4_NODE, O_RDWR | O_CLOEXEC);
    if (p4_fd < 0) {
        int err = errno;

        /*
         * A node that is not there is not a failure on its own: it
         * exists only when the server was started with
         * --p4-guest-scope, and a guest talking to one without it is a
         * correct configuration. A lane that started the server itself
         * treats 77 as a failure, as it does for the other guest tests.
         *
         * Anything else is. The node is mode 0600 and owned by root
         * because the engine decides where every frame goes, so a
         * caller that is not root gets EACCES -- which is a caller
         * problem, not an absent device, and saying "the server was
         * started without --p4-guest-scope" about it sends the reader
         * to the wrong end of the system entirely.
         */
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

    memset(&id, 0, sizeof(id));
    id.buf = (uint64_t)(uintptr_t)&id;
    /* Asking about the surface before using it, which is also what
     * tells us whether the operator granted enough to load a program. */
    {
        struct p4_devcmd_ident devid;
        struct ernic_p4_ident req;

        memset(&devid, 0, sizeof(devid));
        memset(&req, 0, sizeof(req));
        req.buf = (uint64_t)(uintptr_t)&devid;
        req.len = (uint32_t)sizeof(devid);
        if (ioctl(p4_fd, ERNIC_P4_IOC_IDENT, &req) != 0 || req.status != 0) {
            fprintf(stderr, "IDENT failed (status %u): %s\n", req.status,
                    strerror(errno));
            goto out;
        }
        printf("abi=%u scope=%u max_program=%u\n", devid.abi_version,
               devid.scope, devid.max_program);
        if (devid.scope < P4_SCOPE_LOAD) {
            fprintf(stderr,
                    "the operator granted scope %u; this test needs %u "
                    "(--p4-guest-scope load)\n",
                    devid.scope, (unsigned)P4_SCOPE_LOAD);
            goto out;
        }
    }

    if (load_program(prog) != 0 || install_entry(P4_FP8_OP_ADD) != 0)
        goto out;

    ctx = ibv_open_device(ibdev);
    if (!ctx) {
        perror("ibv_open_device");
        goto out;
    }
    printf("device=%s\n", ibv_get_device_name(ibdev));

    pd = ibv_alloc_pd(ctx);
    cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!pd || !cq) {
        fprintf(stderr, "pd/cq allocation failed\n");
        goto out;
    }

    if (posix_memalign((void **)&buf_a, REGION_LEN, REGION_LEN) ||
        posix_memalign((void **)&buf_b, REGION_LEN, REGION_LEN) ||
        posix_memalign((void **)&buf_c, REGION_LEN, REGION_LEN)) {
        fprintf(stderr, "posix_memalign failed\n");
        goto out;
    }

    memset(buf_a, FP8_1_0, REGION_LEN);
    memset(buf_b, FP8_2_0, REGION_LEN);
    memset(buf_c, 0, REGION_LEN);
    buf_a[ODD_ELEMENT] = FP8_4_0;

    mr_a = ibv_reg_mr(pd, buf_a, REGION_LEN, IBV_ACCESS_LOCAL_WRITE);
    mr_b = ibv_reg_mr(pd, buf_b, REGION_LEN, IBV_ACCESS_LOCAL_WRITE);
    mr_c = ibv_reg_mr(pd, buf_c, REGION_LEN,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr_a || !mr_b || !mr_c) {
        fprintf(stderr, "ibv_reg_mr failed\n");
        goto out;
    }

    memset(&init, 0, sizeof(init));
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 8;
    init.cap.max_recv_wr = 8;
    /* Two, because the operand convention is two gather entries. */
    init.cap.max_send_sge = 2;
    init.cap.max_recv_sge = 2;
    init.send_cq = cq;
    init.recv_cq = cq;
    qpa = ibv_create_qp(pd, &init);
    qpb = ibv_create_qp(pd, &init);
    if (!qpa || !qpb) {
        perror("ibv_create_qp");
        goto out;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(qpa, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) ||
        ibv_modify_qp(qpb, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "INIT transition failed\n");
        goto out;
    }

    /* The port reports LID 0 (RoCE-style); route via GID 0. */
    if (ibv_query_gid(ctx, 1, 0, &gid) != 0) {
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

        attr.dest_qp_num = qpb->qp_num;
        if (ibv_modify_qp(qpa, &attr, mask)) {
            fprintf(stderr, "RTR qpa failed\n");
            goto out;
        }
        attr.dest_qp_num = qpa->qp_num;
        if (ibv_modify_qp(qpb, &attr, mask)) {
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

        if (ibv_modify_qp(qpa, &attr, mask) ||
            ibv_modify_qp(qpb, &attr, mask)) {
            fprintf(stderr, "RTS transition failed\n");
            goto out;
        }
    }

    /*
     * An ordinary RDMA WRITE. Nothing about it says "reduce": the two
     * gather entries are the operands only because the table entry
     * installed above says an RDMA write is one. Note what is NOT set
     * anywhere here -- the work request's length field. libibverbs sums
     * the gather list into it, which is the whole reason the device has
     * to read it as a sum.
     */
    memset(sge, 0, sizeof(sge));
    sge[0].addr = (uint64_t)(uintptr_t)buf_a;
    sge[0].length = REGION_LEN;
    sge[0].lkey = mr_a->lkey;
    sge[1].addr = (uint64_t)(uintptr_t)buf_b;
    sge[1].length = REGION_LEN;
    sge[1].lkey = mr_b->lkey;

    memset(&swr, 0, sizeof(swr));
    swr.wr_id = UINT64_C(0x46503852454455);
    swr.opcode = IBV_WR_RDMA_WRITE;
    swr.send_flags = IBV_SEND_SIGNALED;
    swr.sg_list = sge;
    swr.num_sge = 2;
    swr.wr.rdma.remote_addr = (uint64_t)(uintptr_t)buf_c;
    swr.wr.rdma.rkey = mr_c->rkey;

    if (ibv_post_send(qpa, &swr, &bad)) {
        perror("ibv_post_send");
        goto out;
    }
    if (poll_one(cq, &wc) != 0) {
        fprintf(stderr, "no completion for the write\n");
        goto out;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "write completed with status %d (%s)\n", wc.status,
                ibv_wc_status_str(wc.status));
        goto out;
    }

    for (unsigned i = 0; i < REGION_LEN; i++) {
        uint8_t want = (i == ODD_ELEMENT) ? FP8_6_0 : FP8_3_0;

        if (buf_c[i] != want) {
            if (!wrong)
                first = i;
            wrong++;
        }
    }
    if (wrong) {
        fprintf(stderr,
                "%u of %u elements wrong, first at %u (got %#x, want %#x)\n",
                wrong, REGION_LEN, first, buf_c[first],
                first == ODD_ELEMENT ? FP8_6_0 : FP8_3_0);
        goto out;
    }

    printf("✓ %u fp8 elements reduced by a guest-installed P4 entry\n",
           REGION_LEN);
    ret = 0;

out:
    if (qpa)
        ibv_destroy_qp(qpa);
    if (qpb)
        ibv_destroy_qp(qpb);
    if (mr_a)
        ibv_dereg_mr(mr_a);
    if (mr_b)
        ibv_dereg_mr(mr_b);
    if (mr_c)
        ibv_dereg_mr(mr_c);
    free(buf_a);
    free(buf_b);
    free(buf_c);
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (p4_fd >= 0)
        close(p4_fd);
    ibv_free_device_list(dev_list);
    return ret;
}
