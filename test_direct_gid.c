/**
 * Direct GID Test - Bypasses GID cache for standalone RD MA testing
 * 
 * This test works with the loopback backend by using hardcoded GIDs
 * instead of querying from the system GID cache.
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
    union ibv_gid hardcoded_gid;

    printf("\n=== Direct GID RDMA Test ===\n\n");

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

    /* Allocate buffers */
    send_buf = malloc(TEST_MSG_SIZE);
    recv_buf = malloc(TEST_MSG_SIZE);
    if (!send_buf || !recv_buf) {
        fprintf(stderr, "Failed to allocate buffers\n");
        goto clean_cq;
    }
    
    memset(send_buf, 'A', TEST_MSG_SIZE);
    memset(recv_buf, 0, TEST_MSG_SIZE);
    printf("✓ Allocated buffers (%d bytes each)\n", TEST_MSG_SIZE);

    /* Register memory region */
    mr = ibv_reg_mr(pd, send_buf, TEST_MSG_SIZE * 2,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        fprintf(stderr, "Failed to register MR: %s\n", strerror(errno));
        goto clean_buf;
    }
    printf("✓ Registered MR (lkey=0x%x)\n", mr->lkey);

    /* Create Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
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
    /* Use hardcoded GID that matches what driver populated */
    memset(&hardcoded_gid, 0, sizeof(hardcoded_gid));
    hardcoded_gid.raw[0] = 0xfe;
    hardcoded_gid.raw[1] = 0x80;
    /* Add node_guid bytes: 0002:c900:0000:0400 */
    hardcoded_gid.raw[8] = 0x00;
    hardcoded_gid.raw[9] = 0x02;
    hardcoded_gid.raw[10] = 0xc9;
    hardcoded_gid.raw[11] = 0x00;
    hardcoded_gid.raw[12] = 0x00;
    hardcoded_gid.raw[13] = 0x00;
    hardcoded_gid.raw[14] = 0x04;
    hardcoded_gid.raw[15] = 0x00;
    
    printf("Using hardcoded GID: %02x%02x::%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
           hardcoded_gid.raw[0], hardcoded_gid.raw[1],
           hardcoded_gid.raw[8], hardcoded_gid.raw[9],
           hardcoded_gid.raw[10], hardcoded_gid.raw[11],
           hardcoded_gid.raw[12], hardcoded_gid.raw[13],
           hardcoded_gid.raw[14], hardcoded_gid.raw[15]);
    
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
    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.grh.dgid = hardcoded_gid;
    qp_attr.ah_attr.grh.sgid_index = 0;
    qp_attr.ah_attr.grh.hop_limit = 1;

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
                        IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to RTS: %s\n", strerror(errno));
        goto clean_qp;
    }
    printf("✓ QP transitioned to RTS\n");

    printf("\n=== Test Complete - QP Ready for Data Transfer ===\n\n");
    ret = 0;

clean_qp:
    ibv_destroy_qp(qp);
clean_mr:
    ibv_dereg_mr(mr);
clean_buf:
    free(send_buf);
    free(recv_buf);
clean_cq:
    ibv_destroy_cq(cq);
clean_pd:
    ibv_dealloc_pd(pd);
clean_device:
    ibv_close_device(context);
    ibv_free_device_list(dev_list);
    
    return ret;
}

