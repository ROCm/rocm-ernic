/*
 * Standalone Loopback Backend Test
 * 
 * Tests the loopback backend by directly exercising RDMA operations
 * without requiring QEMU or a full VM. This is suitable for CI/CD.
 *
 * This test validates:
 * - Backend initialization
 * - Resource creation (PD, CQ, MR, QP)
 * - QP state transitions
 * - Send/Recv operations with loopback
 * - Completion handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>

#define TEST_BUF_SIZE 4096
#define NUM_ITERATIONS 10

static int test_count = 0;
static int test_passed = 0;

#define TEST_START(name) \
    do { \
        test_count++; \
        printf("[TEST %d] %s ... ", test_count, name); \
        fflush(stdout); \
    } while(0)

#define TEST_PASS() \
    do { \
        test_passed++; \
        printf("PASS\n"); \
    } while(0)

#define TEST_FAIL(msg, ...) \
    do { \
        printf("FAIL: "); \
        printf(msg, ##__VA_ARGS__); \
        printf("\n"); \
        return -1; \
    } while(0)

#define ASSERT(cond, msg, ...) \
    do { \
        if (!(cond)) { \
            TEST_FAIL(msg, ##__VA_ARGS__); \
        } \
    } while(0)

struct test_context {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    char *send_buf;
    char *recv_buf;
};

static int setup_context(struct test_context *test_ctx)
{
    TEST_START("Device discovery");
    test_ctx->dev_list = ibv_get_device_list(NULL);
    ASSERT(test_ctx->dev_list != NULL, "No RDMA devices found");
    ASSERT(test_ctx->dev_list[0] != NULL, "Empty device list");
    TEST_PASS();

    TEST_START("Open device");
    test_ctx->ctx = ibv_open_device(test_ctx->dev_list[0]);
    ASSERT(test_ctx->ctx != NULL, "Failed to open device");
    TEST_PASS();

    TEST_START("Allocate Protection Domain");
    test_ctx->pd = ibv_alloc_pd(test_ctx->ctx);
    ASSERT(test_ctx->pd != NULL, "Failed to allocate PD");
    TEST_PASS();

    TEST_START("Create Completion Queues");
    test_ctx->send_cq = ibv_create_cq(test_ctx->ctx, 16, NULL, NULL, 0);
    ASSERT(test_ctx->send_cq != NULL, "Failed to create send CQ");
    test_ctx->recv_cq = ibv_create_cq(test_ctx->ctx, 16, NULL, NULL, 0);
    ASSERT(test_ctx->recv_cq != NULL, "Failed to create recv CQ");
    TEST_PASS();

    TEST_START("Allocate and register memory");
    test_ctx->send_buf = malloc(TEST_BUF_SIZE);
    test_ctx->recv_buf = malloc(TEST_BUF_SIZE);
    ASSERT(test_ctx->send_buf && test_ctx->recv_buf, "Failed to allocate buffers");
    
    test_ctx->send_mr = ibv_reg_mr(test_ctx->pd, test_ctx->send_buf, 
                                   TEST_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    ASSERT(test_ctx->send_mr != NULL, "Failed to register send MR");
    
    test_ctx->recv_mr = ibv_reg_mr(test_ctx->pd, test_ctx->recv_buf,
                                   TEST_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    ASSERT(test_ctx->recv_mr != NULL, "Failed to register recv MR");
    TEST_PASS();

    TEST_START("Create Queue Pair");
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = test_ctx->send_cq,
        .recv_cq = test_ctx->recv_cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    test_ctx->qp = ibv_create_qp(test_ctx->pd, &qp_attr);
    ASSERT(test_ctx->qp != NULL, "Failed to create QP");
    printf("(QPN=%u) ", test_ctx->qp->qp_num);
    TEST_PASS();

    return 0;
}

static int transition_qp_to_rts(struct test_context *test_ctx)
{
    struct ibv_qp_attr attr = {};
    union ibv_gid gid;
    int ret;

    TEST_START("QP: RESET -> INIT");
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ret = ibv_modify_qp(test_ctx->qp, &attr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                       IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    ASSERT(ret == 0, "Failed to transition to INIT: %s", strerror(errno));
    TEST_PASS();

    TEST_START("QP: INIT -> RTR (self-loopback)");
    ret = ibv_query_gid(test_ctx->ctx, 1, 0, &gid);
    ASSERT(ret == 0, "Failed to query GID");
    
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = test_ctx->qp->qp_num;  /* Self-loopback */
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.port_num = 1;
    
    ret = ibv_modify_qp(test_ctx->qp, &attr,
                       IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    ASSERT(ret == 0, "Failed to transition to RTR: %s", strerror(errno));
    TEST_PASS();

    TEST_START("QP: RTR -> RTS");
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    ret = ibv_modify_qp(test_ctx->qp, &attr,
                       IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    ASSERT(ret == 0, "Failed to transition to RTS: %s", strerror(errno));
    TEST_PASS();

    return 0;
}

static int test_send_recv(struct test_context *test_ctx, size_t size)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = {}, *bad_wr;
    struct ibv_recv_wr recv_wr = {}, *bad_recv_wr;
    struct ibv_wc wc;
    int ret, send_done = 0, recv_done = 0;
    int poll_count = 0;

    /* Prepare buffers */
    memset(test_ctx->send_buf, 0xAB, size);
    memset(test_ctx->recv_buf, 0, size);

    /* Post receive */
    sge.addr = (uint64_t)test_ctx->recv_buf;
    sge.length = size;
    sge.lkey = test_ctx->recv_mr->lkey;
    recv_wr.wr_id = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    ret = ibv_post_recv(test_ctx->qp, &recv_wr, &bad_recv_wr);
    ASSERT(ret == 0, "Failed to post recv");

    /* Post send */
    sge.addr = (uint64_t)test_ctx->send_buf;
    sge.length = size;
    sge.lkey = test_ctx->send_mr->lkey;
    send_wr.wr_id = 2;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    ret = ibv_post_send(test_ctx->qp, &send_wr, &bad_wr);
    ASSERT(ret == 0, "Failed to post send");

    /* Poll for completions */
    while ((!send_done || !recv_done) && poll_count < 1000) {
        ret = ibv_poll_cq(test_ctx->send_cq, 1, &wc);
        if (ret > 0) {
            ASSERT(wc.status == IBV_WC_SUCCESS, 
                   "Send completion failed: %s", 
                   ibv_wc_status_str(wc.status));
            ASSERT(wc.wr_id == 2, "Wrong send WR ID");
            ASSERT(wc.byte_len == size, 
                   "Wrong send byte_len: %u != %zu", wc.byte_len, size);
            send_done = 1;
        }

        ret = ibv_poll_cq(test_ctx->recv_cq, 1, &wc);
        if (ret > 0) {
            ASSERT(wc.status == IBV_WC_SUCCESS,
                   "Recv completion failed: %s",
                   ibv_wc_status_str(wc.status));
            ASSERT(wc.wr_id == 1, "Wrong recv WR ID");
            ASSERT(wc.byte_len == size,
                   "Wrong recv byte_len: %u != %zu", wc.byte_len, size);
            recv_done = 1;
        }

        if (!send_done || !recv_done) {
            usleep(100);
            poll_count++;
        }
    }

    ASSERT(send_done && recv_done, 
           "Timeout waiting for completions (send=%d, recv=%d)",
           send_done, recv_done);

    return 0;
}

static void cleanup_context(struct test_context *test_ctx)
{
    if (test_ctx->qp) ibv_destroy_qp(test_ctx->qp);
    if (test_ctx->send_mr) ibv_dereg_mr(test_ctx->send_mr);
    if (test_ctx->recv_mr) ibv_dereg_mr(test_ctx->recv_mr);
    if (test_ctx->send_cq) ibv_destroy_cq(test_ctx->send_cq);
    if (test_ctx->recv_cq) ibv_destroy_cq(test_ctx->recv_cq);
    if (test_ctx->pd) ibv_dealloc_pd(test_ctx->pd);
    if (test_ctx->ctx) ibv_close_device(test_ctx->ctx);
    if (test_ctx->dev_list) ibv_free_device_list(test_ctx->dev_list);
    free(test_ctx->send_buf);
    free(test_ctx->recv_buf);
}

int main(void)
{
    struct test_context test_ctx = {};
    int ret;

    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Loopback Backend Standalone Test (CI/CD)         ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("\n");

    /* Setup */
    ret = setup_context(&test_ctx);
    if (ret < 0) goto cleanup;

    /* QP state transitions */
    ret = transition_qp_to_rts(&test_ctx);
    if (ret < 0) goto cleanup;

    /* Data transfer tests */
    TEST_START("Send/Recv: 64 bytes");
    ret = test_send_recv(&test_ctx, 64);
    if (ret < 0) goto cleanup;
    TEST_PASS();

    TEST_START("Send/Recv: 256 bytes");
    ret = test_send_recv(&test_ctx, 256);
    if (ret < 0) goto cleanup;
    TEST_PASS();

    TEST_START("Send/Recv: 1024 bytes");
    ret = test_send_recv(&test_ctx, 1024);
    if (ret < 0) goto cleanup;
    TEST_PASS();

    TEST_START("Send/Recv: 4096 bytes");
    ret = test_send_recv(&test_ctx, 4096);
    if (ret < 0) goto cleanup;
    TEST_PASS();

    /* Multiple iterations */
    TEST_START("Multiple iterations (10x 512 bytes)");
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        ret = test_send_recv(&test_ctx, 512);
        if (ret < 0) goto cleanup;
    }
    TEST_PASS();

    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Test Summary                                      ║\n");
    printf("╠════════════════════════════════════════════════════╣\n");
    printf("║  Total Tests: %-5d                                 ║\n", test_count);
    printf("║  Passed:      %-5d                                 ║\n", test_passed);
    printf("║  Failed:      %-5d                                 ║\n", test_count - test_passed);
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("\n");

cleanup:
    cleanup_context(&test_ctx);
    
    if (test_passed == test_count) {
        printf("✅ ALL TESTS PASSED\n\n");
        return 0;
    } else {
        printf("❌ SOME TESTS FAILED\n\n");
        return 1;
    }
}

