/**
 * Simple RDMA Loopback Test
 *
 * Tests the loopback backend by performing basic RDMA operations:
 * - Create PD, CQ, MR, QP
 * - Transition QP to RTS
 * - Post send and recv
 * - Poll for completions
 *
 * This will trigger the loopback backend's MD5 computation and
 * data pattern generation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>

#define TEST_MSG_SIZE 4096
#define CQ_SIZE 16

int main(int argc, char **argv)
{
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_qp_attr qp_attr;
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    char *send_buf, *recv_buf;
    int ret, num_devices;

    printf("\n=== RDMA Loopback Test ===\n\n");

    /* Get device list */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB devices list\n");
        return 1;
    }

    printf("Found %d RDMA device(s)\n", num_devices);
    if (num_devices == 0) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
    }

    /* Use first device */
    ib_dev = dev_list[0];
    printf("Using device: %s\n", ibv_get_device_name(ib_dev));

    /* Open device */
    context = ibv_open_device(ib_dev);
    if (!context) {
        fprintf(stderr, "Failed to open device\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    printf("✓ Opened device context\n");

    /* Allocate Protection Domain */
    pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        goto clean_device;
    }
    printf("✓ Allocated PD\n");

    /* Create Completion Queue */
    cq = ibv_create_cq(context, CQ_SIZE, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Failed to create CQ\n");
        goto clean_pd;
    }
    printf("✓ Created CQ (%d entries)\n", CQ_SIZE);

    /* Allocate and register memory */
    send_buf = malloc(TEST_MSG_SIZE);
    recv_buf = malloc(TEST_MSG_SIZE);
    if (!send_buf || !recv_buf) {
        fprintf(stderr, "Failed to allocate buffers\n");
        goto clean_cq;
    }

    /* Fill send buffer with a pattern */
    memset(send_buf, 0xAB, TEST_MSG_SIZE);
    memcpy(send_buf, "RDMA LOOPBACK TEST MESSAGE!", 27);

    printf("✓ Allocated buffers (%d bytes each)\n", TEST_MSG_SIZE);
    printf("  Send buffer: \"%.27s...\"\n", send_buf);

    /* Register memory regions */
    mr = ibv_reg_mr(pd, send_buf, TEST_MSG_SIZE * 2, 
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        fprintf(stderr, "Failed to register MR: %s\n", strerror(errno));
        goto clean_bufs;
    }
    printf("✓ Registered MR (lkey=0x%x, rkey=0x%x)\n", mr->lkey, mr->rkey);

    /* Create Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;  /* Reliable Connection */
    qp_init_attr.cap.max_send_wr = 10;
    qp_init_attr.cap.max_recv_wr = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        fprintf(stderr, "Failed to create QP: %s\n", strerror(errno));
        goto clean_mr;
    }
    printf("✓ Created QP (QPN=%d, type=RC)\n", qp->qp_num);

    /* Transition QP to INIT */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    ret = ibv_modify_qp(qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | 
                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to INIT: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ QP transitioned to INIT\n");

    /* Transition QP to RTR (Ready to Receive) */
    /* For RoCE, we need a GID. Use a dummy/default GID for loopback */
    union ibv_gid my_gid;
    memset(&my_gid, 0, sizeof(my_gid));
    /* Set a dummy GID: fe80::1 (link-local IPv6) */
    my_gid.raw[0] = 0xfe;
    my_gid.raw[1] = 0x80;
    my_gid.raw[15] = 0x01;
    
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.dest_qp_num = qp->qp_num;  /* Loopback to self */
    qp_attr.rq_psn = 0;
    qp_attr.max_dest_rd_atomic = 1;
    qp_attr.min_rnr_timer = 12;
    qp_attr.ah_attr.dlid = 0;
    qp_attr.ah_attr.sl = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = 1;
    /* For RoCE, GRH is required */
    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.grh.dgid = my_gid;
    qp_attr.ah_attr.grh.sgid_index = 0;
    qp_attr.ah_attr.grh.hop_limit = 1;
    qp_attr.ah_attr.grh.traffic_class = 0;

    ret = ibv_modify_qp(qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to RTR: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ QP transitioned to RTR\n");

    /* Transition QP to RTS (Ready to Send) */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    qp_attr.timeout = 14;
    qp_attr.retry_cnt = 7;
    qp_attr.rnr_retry = 7;
    qp_attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to RTS: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ QP transitioned to RTS\n\n");

    /* Post receive request first */
    printf("Posting receive...\n");
    memset(&recv_wr, 0, sizeof(recv_wr));
    memset(&sge, 0, sizeof(sge));
    
    sge.addr = (uintptr_t)recv_buf;
    sge.length = TEST_MSG_SIZE;
    sge.lkey = mr->lkey;

    recv_wr.wr_id = 2;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(qp, &recv_wr, &bad_recv_wr);
    if (ret) {
        fprintf(stderr, "Failed to post receive: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ Posted receive (WR ID=2)\n");

    /* Post send request */
    printf("\nPosting send...\n");
    memset(&send_wr, 0, sizeof(send_wr));
    memset(&sge, 0, sizeof(sge));
    
    sge.addr = (uintptr_t)send_buf;
    sge.length = TEST_MSG_SIZE;
    sge.lkey = mr->lkey;

    send_wr.wr_id = 1;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(qp, &send_wr, &bad_send_wr);
    if (ret) {
        fprintf(stderr, "Failed to post send: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ Posted send (WR ID=1, %d bytes)\n", TEST_MSG_SIZE);

    /* Poll for completions */
    printf("\nPolling for completions...\n");
    
    int num_completions = 0;
    int max_polls = 100;
    
    while (num_completions < 2 && max_polls-- > 0) {
        ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) {
            fprintf(stderr, "Failed to poll CQ: %s\n", strerror(errno));
            break;
        }
        
        if (ret > 0) {
            num_completions++;
            printf("✓ Completion %d: WR ID=%lu, status=%s, opcode=%d, byte_len=%d\n",
                   num_completions, wc.wr_id,
                   ibv_wc_status_str(wc.status),
                   wc.opcode, wc.byte_len);
            
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "  ✗ Completion failed: %s\n", 
                        ibv_wc_status_str(wc.status));
            }
        }
        
        usleep(10000);  /* 10ms */
    }

    if (num_completions == 2) {
        printf("\n🎉 SUCCESS! Both send and receive completed\n");
        printf("\nCheck server logs for:\n");
        printf("  - MD5 hash of transferred data\n");
        printf("  - Data pattern used\n");
        printf("  - Loopback send/recv matching\n");
        ret = 0;
    } else {
        printf("\n⚠️  Only %d/%d completions received\n", num_completions, 2);
        ret = 1;
    }

clean_qp:
    ibv_destroy_qp(qp);
clean_mr:
    ibv_dereg_mr(mr);
clean_bufs:
    free(send_buf);
    free(recv_buf);
clean_cq:
    ibv_destroy_cq(cq);
clean_pd:
    ibv_dealloc_pd(pd);
clean_device:
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    printf("\n=== Test Complete ===\n\n");
    return ret;
}

