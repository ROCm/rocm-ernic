/*
 * ernic-rdma-errors: make the RDMA errors rocm-ernic counts happen on a real
 * ionic_rdma device in a guest, and check its hw_counters moved by exactly
 * what the emulator's unit tests (test_ionic_dp_errors) say they should.
 *
 * Each row connects two fresh RC QPs on the one device to each other over a
 * RoCE v2 GID, does one thing wrong (or nothing, for the negative row), waits
 * for the completion that shows it, and compares every req_*, resp_* and
 * CNP/ECN counter before and after.  Both QPs are on the one device, so its
 * counters carry both the requester's and the responder's side of an error.
 * A counter that moves when it should not fails the row just as one that
 * stays still does.
 *
 * Prints "ok <category>: <description>" or "FAIL <category>: <description>;
 * expected: ...; got: ..." per row and exits 1 if any row failed.
 *
 * usage: ernic-rdma-errors [-d DEVICE] [-g GID_INDEX]
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include "ernic_attributes.h"
#include "ernic_counter_snapshot.h"
#include "parse_int.h"

#define PORT_NUM        1
#define BUF_SIZE        4096u
#define MAX_WANT        4u
#define POLL_TIMEOUT_MS 10000u
/* ionic_rdma's hw_counters are cached for `lifespan` ms (10); wait past it. */
#define SETTLE_MS 100u
/* Long enough that the emulator's RNR pass finds no receive, well inside its
 * RNR_RETRY_MS (5000) so the SEND is still delivered. */
#define NO_RECV_MS 500u

struct pair {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *req;  /* the requester */
    struct ibv_qp *resp; /* the responder */
    struct ibv_mr *req_mr;
    struct ibv_mr *resp_mr;
    uint8_t *req_buf;
    uint8_t *resp_buf;
};

struct env {
    struct ibv_context *ctx;
    const char *device;
    int gid_index;
    union ibv_gid gid;
    bool atomics;
};

struct want {
    const char *name;
    uint64_t delta;
    /* delta is a floor: the counter moves once per retry, and how many
     * retries fit in a wait depends on the host. */
    bool at_least;
};

struct rdma_case {
    const char *category;
    const char *description;
    const char *expected;
    /* Returns 0 when the completions are the expected ones; otherwise
     * writes what it saw to got.  1 means the row does not apply here. */
    int (*run)(const struct env *env, struct pair *p, char *got,
               size_t got_len);
    struct want want[MAX_WANT];
};

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = {.tv_sec = ms / 1000u,
                          .tv_nsec = (long)(ms % 1000u) * 1000000L};

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* The counters an RDMA error can move: everything but commands, lifespan
 * and the byte totals, which successful traffic moves too. */
static bool checked_counter(const char *name)
{
    return strncmp(name, "req_", 4) == 0 || strncmp(name, "resp_", 5) == 0 ||
           strcmp(name, "rx_cnp_pkts") == 0 ||
           strcmp(name, "rx_ecn_pkts") == 0 || strcmp(name, "tx_cnp_pkts") == 0;
}

static int read_counters(const char *device, struct counters *c)
{
    char dir_path[256];
    DIR *dir;
    const struct dirent *entry;

    memset(c, 0, sizeof(*c));
    if (snprintf(dir_path, sizeof(dir_path),
                 "/sys/class/infiniband/%s/ports/%d/hw_counters", device,
                 PORT_NUM) >= (int)sizeof(dir_path))
        return -1;
    dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "%s: %s\n", dir_path, strerror(errno));
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[512];
        char text[32];
        FILE *f;
        uint64_t v;
        bool complete;

        if (!checked_counter(entry->d_name))
            continue;
        if (c->n == MAX_COUNTERS || strlen(entry->d_name) >= NAME_LEN) {
            fprintf(stderr, "hw_counters: too many or too long names\n");
            closedir(dir);
            return -1;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >=
            (int)sizeof(path))
            continue;
        f = fopen(path, "re");
        if (!f || !fgets(text, sizeof(text), f)) {
            fprintf(stderr, "%s: unreadable\n", path);
            if (f)
                fclose(f);
            closedir(dir);
            return -1;
        }
        complete = fgetc(f) == EOF && !ferror(f);
        fclose(f);
        if (!complete || !parse_counter_value(text, &v)) {
            fprintf(stderr, "%s: not a number: %s", path, text);
            closedir(dir);
            return -1;
        }
        memcpy(c->name[c->n], entry->d_name, strlen(entry->d_name) + 1);
        c->value[c->n] = v;
        c->n++;
    }
    closedir(dir);
    return c->n ? 0 : -1;
}

/* The row's expectation for counter name: exactly 0 when it names none. */
static struct want want_for(const struct rdma_case *rc, const char *name)
{
    struct want none = {name, 0, false};

    for (size_t i = 0; i < MAX_WANT && rc->want[i].name; i++)
        if (strcmp(rc->want[i].name, name) == 0)
            return rc->want[i];
    return none;
}

/*
 * Appends to got, after ", " unless it is empty, cutting what does not fit.
 * *used is strlen(got) on entry and stays so.
 */
ERNIC_PRINTF_FORMAT(4, 5)
static void got_add(char *got, size_t got_len, size_t *used, const char *fmt,
                    ...)
{
    va_list ap;
    int n;

    if (*used > 0) {
        if (got_len - *used <= sizeof(", "))
            return;
        memcpy(got + *used, ", ", sizeof(", "));
        *used += sizeof(", ") - 1;
    }
    if (got_len - *used < 2)
        return;
    va_start(ap, fmt);
    n = vsnprintf(got + *used, got_len - *used, fmt, ap);
    va_end(ap);
    if (n > 0)
        *used += (size_t)n < got_len - *used ? (size_t)n : got_len - *used - 1;
}

/* Appends "name +delta" for every counter whose delta differs from the row,
 * or that the row names; returns the number that differ. */
static int compare_counters(const struct rdma_case *rc,
                            const struct counters *before,
                            const struct counters *after, char *got,
                            size_t got_len)
{
    int mismatches = 0;
    size_t used = strlen(got);

    if (!counter_sets_match(before, after)) {
        got_add(got, got_len, &used, "counter names changed or invalid");
        return 1;
    }
    for (size_t i = 0; i < after->n; i++) {
        uint64_t old = 0;
        uint64_t delta;
        struct want want = want_for(rc, after->name[i]);

        for (size_t j = 0; j < before->n; j++)
            if (strcmp(before->name[j], after->name[i]) == 0)
                old = before->value[j];
        if (!counter_delta(old, after->value[i], &delta)) {
            mismatches++;
            got_add(got, got_len, &used, "%s decreased", after->name[i]);
            continue;
        }
        if (delta == 0 && want.delta == 0)
            continue;
        if (want.at_least ? delta < want.delta : delta != want.delta)
            mismatches++;
        got_add(got, got_len, &used, "%s +%" PRIu64, after->name[i], delta);
    }
    for (size_t i = 0; i < MAX_WANT && rc->want[i].name; i++) {
        bool present = false;

        for (size_t j = 0; j < after->n; j++)
            present = present || strcmp(after->name[j], rc->want[i].name) == 0;
        if (!present) {
            mismatches++;
            got_add(got, got_len, &used, "%s missing", rc->want[i].name);
        }
    }
    return mismatches;
}

static int pick_gid(struct env *env)
{
    struct ibv_port_attr port;

    if (ibv_query_port(env->ctx, PORT_NUM, &port) != 0)
        return -1;
    for (int i = 0; i < port.gid_tbl_len; i++) {
        struct ibv_gid_entry entry;
        static const uint8_t v4_mapped[12] = {0, 0, 0, 0, 0,    0,
                                              0, 0, 0, 0, 0xff, 0xff};

        if (env->gid_index >= 0 && i != env->gid_index)
            continue;
        if (ibv_query_gid_ex(env->ctx, PORT_NUM, (uint32_t)i, &entry, 0) != 0)
            continue;
        if (env->gid_index < 0 &&
            (entry.gid_type != IBV_GID_TYPE_ROCE_V2 ||
             memcmp(entry.gid.raw, v4_mapped, sizeof(v4_mapped)) != 0))
            continue;
        env->gid_index = i;
        env->gid = entry.gid;
        return 0;
    }
    return -1;
}

static struct ibv_qp *create_qp(struct pair *p)
{
    struct ibv_qp_init_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.send_cq = p->cq;
    attr.recv_cq = p->cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = 4;
    attr.cap.max_recv_wr = 4;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    return ibv_create_qp(p->pd, &attr);
}

static int connect_qp(const struct env *env, struct ibv_qp *qp,
                      uint32_t dest_qp_num)
{
    struct ibv_qp_attr attr;
    const unsigned int access =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = PORT_NUM;
    attr.qp_access_flags = access;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0)
        return -1;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = dest_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = env->gid;
    attr.ah_attr.grh.sgid_index = (uint8_t)env->gid_index;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.port_num = PORT_NUM;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) !=
        0)
        return -1;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                             IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                             IBV_QP_MAX_QP_RD_ATOMIC);
}

static void pair_close(struct pair *p)
{
    if (p->req)
        ibv_destroy_qp(p->req);
    if (p->resp)
        ibv_destroy_qp(p->resp);
    if (p->req_mr)
        ibv_dereg_mr(p->req_mr);
    if (p->resp_mr)
        ibv_dereg_mr(p->resp_mr);
    if (p->cq)
        ibv_destroy_cq(p->cq);
    if (p->pd)
        ibv_dealloc_pd(p->pd);
    free(p->req_buf);
    free(p->resp_buf);
    memset(p, 0, sizeof(*p));
}

static int pair_open(const struct env *env, struct pair *p)
{
    const unsigned int access =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;

    memset(p, 0, sizeof(*p));
    p->ctx = env->ctx;
    p->req_buf = calloc(1, BUF_SIZE);
    p->resp_buf = calloc(1, BUF_SIZE);
    p->pd = ibv_alloc_pd(env->ctx);
    if (!p->req_buf || !p->resp_buf || !p->pd)
        goto fail;
    p->cq = ibv_create_cq(env->ctx, 16, NULL, NULL, 0);
    p->req_mr = ibv_reg_mr(p->pd, p->req_buf, BUF_SIZE, access);
    p->resp_mr = ibv_reg_mr(p->pd, p->resp_buf, BUF_SIZE, access);
    if (!p->cq || !p->req_mr || !p->resp_mr)
        goto fail;
    p->req = create_qp(p);
    p->resp = create_qp(p);
    if (!p->req || !p->resp)
        goto fail;
    if (connect_qp(env, p->req, p->resp->qp_num) != 0 ||
        connect_qp(env, p->resp, p->req->qp_num) != 0)
        goto fail;
    return 0;
fail:
    fprintf(stderr, "QP pair setup failed: %s\n", strerror(errno));
    pair_close(p);
    return -1;
}

/* Polls until a completion for qp arrives; other QPs' completions are
 * dropped.  Returns 0 and fills wc, or -1 on timeout. */
static int wait_wc(struct pair *p, const struct ibv_qp *qp, struct ibv_wc *wc)
{
    uint64_t deadline = now_ms() + POLL_TIMEOUT_MS;

    while (now_ms() < deadline) {
        int n;

        memset(wc, 0, sizeof(*wc));
        n = ibv_poll_cq(p->cq, 1, wc);
        if (n < 0)
            return -1;
        if (n == 1 && wc->qp_num == qp->qp_num)
            return 0;
        if (n == 0)
            sleep_ms(1);
    }
    return -1;
}

static int post_recv(struct pair *p, uint32_t length)
{
    struct ibv_sge sge = {.addr = (uintptr_t)p->resp_buf,
                          .length = length,
                          .lkey = p->resp_mr->lkey};
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 2;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    return ibv_post_recv(p->resp, &wr, &bad);
}

static int post_send(struct pair *p, enum ibv_wr_opcode opcode, uint32_t length,
                     uint32_t lkey, uint32_t rkey)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)p->req_buf, .length = length, .lkey = lkey};
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (opcode == IBV_WR_ATOMIC_FETCH_AND_ADD) {
        wr.wr.atomic.remote_addr = (uintptr_t)p->resp_buf;
        wr.wr.atomic.rkey = rkey;
        wr.wr.atomic.compare_add = 1;
    } else {
        wr.wr.rdma.remote_addr = (uintptr_t)p->resp_buf;
        wr.wr.rdma.rkey = rkey;
    }
    return ibv_post_send(p->req, &wr, &bad);
}

/* Posts one work request and checks the completion that qp sees. */
static int expect_status(struct pair *p, const struct ibv_qp *qp,
                         enum ibv_wc_status want, char *got, size_t got_len)
{
    struct ibv_wc wc;

    if (wait_wc(p, qp, &wc) != 0) {
        snprintf(got, got_len, "no completion on QP %u", qp->qp_num);
        return -1;
    }
    if (wc.status != want) {
        snprintf(got, got_len, "completion %s", ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

/* A key that no MR in this PD has: the real one with its low byte flipped. */
static uint32_t bad_key(uint32_t key)
{
    return key ^ 0xffu;
}

static int run_succeed(const struct env *env, struct pair *p, char *got,
                       size_t got_len)
{
    (void)env;
    if (post_send(p, IBV_WR_RDMA_WRITE, 64, p->req_mr->lkey,
                  p->resp_mr->rkey) != 0 ||
        expect_status(p, p->req, IBV_WC_SUCCESS, got, got_len) != 0)
        return -1;
    if (post_send(p, IBV_WR_RDMA_READ, 64, p->req_mr->lkey, p->resp_mr->rkey) !=
            0 ||
        expect_status(p, p->req, IBV_WC_SUCCESS, got, got_len) != 0)
        return -1;
    if (post_recv(p, BUF_SIZE) != 0 ||
        post_send(p, IBV_WR_SEND, 64, p->req_mr->lkey, 0) != 0 ||
        expect_status(p, p->resp, IBV_WC_SUCCESS, got, got_len) != 0)
        return -1;
    return 0;
}

static int run_write_bad_rkey(const struct env *env, struct pair *p, char *got,
                              size_t got_len)
{
    (void)env;
    if (post_send(p, IBV_WR_RDMA_WRITE, 64, p->req_mr->lkey,
                  bad_key(p->resp_mr->rkey)) != 0)
        return -1;
    return expect_status(p, p->req, IBV_WC_REM_ACCESS_ERR, got, got_len);
}

static int run_write_bad_lkey(const struct env *env, struct pair *p, char *got,
                              size_t got_len)
{
    (void)env;
    if (post_send(p, IBV_WR_RDMA_WRITE, 64, bad_key(p->req_mr->lkey),
                  p->resp_mr->rkey) != 0)
        return -1;
    return expect_status(p, p->req, IBV_WC_LOC_PROT_ERR, got, got_len);
}

static int run_recv_too_small(const struct env *env, struct pair *p, char *got,
                              size_t got_len)
{
    (void)env;
    if (post_recv(p, 16) != 0 ||
        post_send(p, IBV_WR_SEND, 64, p->req_mr->lkey, 0) != 0)
        return -1;
    return expect_status(p, p->resp, IBV_WC_LOC_LEN_ERR, got, got_len);
}

static int run_atomic_bad_rkey(const struct env *env, struct pair *p, char *got,
                               size_t got_len)
{
    if (!env->atomics) {
        snprintf(got, got_len, "device has no atomics");
        return 1;
    }
    if (post_send(p, IBV_WR_ATOMIC_FETCH_AND_ADD, 8, p->req_mr->lkey,
                  bad_key(p->resp_mr->rkey)) != 0)
        return -1;
    return expect_status(p, p->req, IBV_WC_REM_ACCESS_ERR, got, got_len);
}

static int run_send_no_recv(const struct env *env, struct pair *p, char *got,
                            size_t got_len)
{
    (void)env;
    if (post_send(p, IBV_WR_SEND, 64, p->req_mr->lkey, 0) != 0)
        return -1;
    sleep_ms(NO_RECV_MS);
    if (post_recv(p, BUF_SIZE) != 0)
        return -1;
    return expect_status(p, p->resp, IBV_WC_SUCCESS, got, got_len);
}

static int run_send_bad_source(const struct env *env, struct pair *p, char *got,
                               size_t got_len)
{
    (void)env;
    if (post_recv(p, BUF_SIZE) != 0 ||
        post_send(p, IBV_WR_SEND, 64, bad_key(p->req_mr->lkey), 0) != 0)
        return -1;
    return expect_status(p, p->req, IBV_WC_LOC_PROT_ERR, got, got_len);
}

static const struct rdma_case cases[] = {
    {"negative",
     "RDMA WRITE, RDMA READ and SEND that all succeed",
     "no error counter moves",
     run_succeed,
     {{NULL, 0, false}}},
    {"positive",
     "RDMA WRITE with an unregistered rkey",
     "REM_ACCESS_ERR; req_rx_remote_access_err, req_rx_cqe_err and "
     "resp_tx_remote_access_err +1",
     run_write_bad_rkey,
     {{"req_rx_remote_access_err", 1, false},
      {"req_rx_cqe_err", 1, false},
      {"resp_tx_remote_access_err", 1, false}}},
    {"positive",
     "RDMA WRITE whose local SGE names an unregistered lkey",
     "LOC_PROT_ERR; req_tx_memory_mgmt_err and req_rx_cqe_err +1",
     run_write_bad_lkey,
     {{"req_tx_memory_mgmt_err", 1, false}, {"req_rx_cqe_err", 1, false}}},
    {"positive",
     "SEND of 64 bytes into a 16-byte receive",
     "receive LOC_LEN_ERR; resp_rx_local_len_err and resp_rx_cqe_err +1",
     run_recv_too_small,
     {{"resp_rx_local_len_err", 1, false}, {"resp_rx_cqe_err", 1, false}}},
    {"positive",
     "fetch-and-add with an unregistered rkey",
     "REM_ACCESS_ERR; a remote access error on both ends, as for RDMA WRITE",
     run_atomic_bad_rkey,
     {{"req_rx_remote_access_err", 1, false},
      {"req_rx_cqe_err", 1, false},
      {"resp_tx_remote_access_err", 1, false}}},
    {"boundary",
     "SEND with no receive posted for 500 ms, then one",
     "delivered; resp_rx_out_of_buffer +1, resp_tx_rnr_retry_err +1 or more "
     "(one per retry), and no error completion",
     run_send_no_recv,
     {{"resp_rx_out_of_buffer", 1, false}, {"resp_tx_rnr_retry_err", 1, true}}},
    {"corner",
     "SEND whose source SGE names an unregistered lkey",
     "the requester completes LOC_PROT_ERR and req_tx_memory_mgmt_err and "
     "req_rx_cqe_err +1; the send is refused before delivery, so the "
     "responder counts nothing",
     run_send_bad_source,
     {{"req_tx_memory_mgmt_err", 1, false}, {"req_rx_cqe_err", 1, false}}},
};

static int run_case(const struct env *env, const struct rdma_case *rc)
{
    struct counters before;
    struct counters after;
    struct pair p;
    char got[1024] = "";
    int rc_run;

    if (pair_open(env, &p) != 0) {
        printf("FAIL %s: %s; expected: %s; got: QP pair setup failed\n",
               rc->category, rc->description, rc->expected);
        return 1;
    }
    sleep_ms(SETTLE_MS);
    if (read_counters(env->device, &before) != 0) {
        pair_close(&p);
        return 1;
    }
    rc_run = rc->run(env, &p, got, sizeof(got));
    pair_close(&p);
    if (rc_run == 1) {
        printf("skip %s: %s; %s\n", rc->category, rc->description, got);
        return 0;
    }
    sleep_ms(SETTLE_MS);
    if (read_counters(env->device, &after) != 0)
        return 1;
    if (rc_run != 0) {
        printf("FAIL %s: %s; expected: %s; got: %s\n", rc->category,
               rc->description, rc->expected, got[0] ? got : "post failed");
        return 1;
    }
    if (compare_counters(rc, &before, &after, got, sizeof(got)) != 0) {
        printf("FAIL %s: %s; expected: %s; got: %s\n", rc->category,
               rc->description, rc->expected, got);
        return 1;
    }
    printf("ok %s: %s%s%s\n", rc->category, rc->description, got[0] ? "; " : "",
           got);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-d DEVICE] [-g GID_INDEX]\n", argv0);
}

int main(int argc, char **argv)
{
    struct env env = {.gid_index = -1};
    struct ibv_device **list;
    struct ibv_device_attr attr;
    const char *want_device = NULL;
    int num = 0;
    int failures = 0;
    int opt;

    /* getopt() keeps its state in globals, such as optind, so it isn't
     * thread safe, but this program has only one thread */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) */
    while ((opt = getopt(argc, argv, "d:g:h")) != -1) {
        uint32_t v;

        switch (opt) {
        case 'd':
            want_device = optarg;
            break;
        case 'g':
            /* A GID table index is a u8 in the verbs ABI. */
            if (!ernic_parse_u32(optarg, &v) || v > UINT8_MAX) {
                usage(argv[0]);
                return 2;
            }
            env.gid_index = (int)v;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return 2;
    }

    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "no RDMA devices\n");
        return 1;
    }
    for (int i = 0; i < num && !env.ctx; i++) {
        if (want_device &&
            strcmp(ibv_get_device_name(list[i]), want_device) != 0)
            continue;
        env.ctx = ibv_open_device(list[i]);
        env.device = ibv_get_device_name(list[i]);
    }
    if (!env.ctx) {
        fprintf(stderr, "device %s not found\n",
                want_device ? want_device : "(any)");
        ibv_free_device_list(list);
        return 1;
    }
    if (pick_gid(&env) != 0) {
        fprintf(stderr, "%s: no RoCE v2 IPv4-mapped GID%s\n", env.device,
                env.gid_index >= 0 ? " at that index" : "");
        ibv_close_device(env.ctx);
        ibv_free_device_list(list);
        return 1;
    }
    memset(&attr, 0, sizeof(attr));
    env.atomics = ibv_query_device(env.ctx, &attr) == 0 &&
                  attr.atomic_cap != IBV_ATOMIC_NONE;
    printf("device %s, GID index %d\n", env.device, env.gid_index);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failures += run_case(&env, &cases[i]);

    ibv_close_device(env.ctx);
    ibv_free_device_list(list);
    return failures ? 1 : 0;
}
