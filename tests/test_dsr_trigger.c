/*
 * test_dsr_trigger.c - Test DSR (Device Shared Region) initialization
 *
 * This test mimics what QEMU does to trigger load_dsr() in the server,
 * which involves writing guest physical addresses to DSR registers.
 * This should reproduce the DMA mapping crash we see with QEMU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

/* PVRDMA register offsets (BAR1) */
#define PVRDMA_REG_VERSION  0x00 /* Device version */
#define PVRDMA_REG_DSR_LO   0x04 /* DSR guest PA low 32 bits */
#define PVRDMA_REG_DSR_HI   0x08 /* DSR guest PA high 32 bits */
#define PVRDMA_REG_CTL      0x0c /* Control register */
#define PVRDMA_REG_REQUEST  0x10 /* Command request */
#define PVRDMA_REG_RESPONSE 0x14 /* Command response */
#define PVRDMA_REG_ERR      0x28 /* Error register */

#define PVRDMA_HW_VERSION 17

/* Size of DSR structure from QEMU */
#define DSR_SIZE 4096

/* Simple vfio-user protocol structures */
#define VFIO_USER_VERSION 1

enum vfio_user_command {
    VFIO_USER_VERSION_CMD = 0,
    VFIO_USER_DMA_MAP = 1,
    VFIO_USER_DMA_UNMAP = 2,
    VFIO_USER_DEVICE_GET_INFO = 3,
    VFIO_USER_DEVICE_GET_REGION_INFO = 4,
    VFIO_USER_REGION_READ = 6,
    VFIO_USER_REGION_WRITE = 7,
};

struct vfio_user_header {
    uint16_t msg_id;
    uint16_t cmd;
    uint32_t msg_size;
    uint32_t flags;
    uint32_t error;
} __attribute__((packed));

struct vfio_user_region_access {
    struct vfio_user_header hdr;
    uint64_t offset;
    uint32_t region;
    uint32_t count;
    uint8_t data[];
} __attribute__((packed));

/* Connect to vfio-user server */
static int connect_to_server(const char *socket_path)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

/* Read a 32-bit register from BAR1 */
static int read_bar1_reg(int fd, uint64_t offset, uint32_t *value)
{
    struct vfio_user_region_access msg;
    struct vfio_user_region_access reply;
    ssize_t ret;
    static uint16_t msg_id = 1;

    /* Build read request */
    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_id = msg_id++;
    msg.hdr.cmd = VFIO_USER_REGION_READ;
    msg.hdr.msg_size = sizeof(msg);
    msg.hdr.flags = 0; /* Command */
    msg.region = 1;    /* BAR1 */
    msg.offset = offset;
    msg.count = 4; /* 32-bit */

    ret = write(fd, &msg, sizeof(msg));
    if (ret < 0) {
        perror("write");
        return -1;
    }

    /* Read reply */
    ret = read(fd, &reply, sizeof(reply) + 4);
    if (ret < 0) {
        perror("read");
        return -1;
    }

    if (reply.hdr.error != 0) {
        fprintf(stderr, "Server returned error: %d\n", reply.hdr.error);
        return -1;
    }

    memcpy(value, reply.data, 4);
    return 0;
}

/* Write a 32-bit register to BAR1 */
static int write_bar1_reg(int fd, uint64_t offset, uint32_t value)
{
    uint8_t buf[sizeof(struct vfio_user_region_access) + 4];
    struct vfio_user_region_access *msg = (struct vfio_user_region_access *)buf;
    struct vfio_user_header reply;
    ssize_t ret;
    static uint16_t msg_id = 100;

    /* Build write request */
    memset(buf, 0, sizeof(buf));
    msg->hdr.msg_id = msg_id++;
    msg->hdr.cmd = VFIO_USER_REGION_WRITE;
    msg->hdr.msg_size = sizeof(buf);
    msg->hdr.flags = 0; /* Command */
    msg->region = 1;    /* BAR1 */
    msg->offset = offset;
    msg->count = 4; /* 32-bit */
    memcpy(msg->data, &value, 4);

    ret = write(fd, buf, sizeof(buf));
    if (ret < 0) {
        perror("write");
        return -1;
    }

    /* Read reply */
    ret = read(fd, &reply, sizeof(reply));
    if (ret < 0) {
        perror("read");
        return -1;
    }

    if (reply.error != 0) {
        fprintf(stderr, "Server returned error: %d\n", reply.error);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *socket_path = "/tmp/vfio-user-pvrdma.sock";
    int fd;
    uint32_t version, dsr_lo, dsr_hi;
    uint64_t fake_dsr_addr = 0x100000000ULL; /* Fake guest physical address */

    if (argc > 1) {
        socket_path = argv[1];
    }

    printf("=================================================\n");
    printf("  PVRDMA DSR Trigger Test\n");
    printf("=================================================\n");
    printf("\n");
    printf("This test mimics QEMU's DSR initialization to\n");
    printf("reproduce the DMA mapping crash.\n");
    printf("\n");
    printf("Socket: %s\n", socket_path);
    printf("\n");

    /* Connect to server */
    printf("[1] Connecting to server...\n");
    fd = connect_to_server(socket_path);
    if (fd < 0) {
        fprintf(stderr, "✗ Failed to connect\n");
        return 1;
    }
    printf("    ✓ Connected (fd=%d)\n", fd);

    /* Read version register */
    printf("\n[2] Reading version register (BAR1 offset 0x%02x)...\n",
           PVRDMA_REG_VERSION);
    if (read_bar1_reg(fd, PVRDMA_REG_VERSION, &version) < 0) {
        fprintf(stderr, "✗ Failed to read version\n");
        close(fd);
        return 1;
    }
    printf("    Version: %u (expected %u)\n", version, PVRDMA_HW_VERSION);
    if (version == PVRDMA_HW_VERSION) {
        printf("    ✓ Version correct\n");
    } else {
        printf("    ✗ Version mismatch!\n");
        close(fd);
        return 1;
    }

    /* Write DSR address - this triggers load_dsr() which does DMA mapping */
    printf("\n[3] Writing DSR address registers...\n");
    printf("    Writing fake DSR guest PA: 0x%lx\n", fake_dsr_addr);

    dsr_lo = (uint32_t)(fake_dsr_addr & 0xFFFFFFFF);
    dsr_hi = (uint32_t)(fake_dsr_addr >> 32);

    printf("    Writing DSR_LO (0x%02x) = 0x%08x\n", PVRDMA_REG_DSR_LO, dsr_lo);
    if (write_bar1_reg(fd, PVRDMA_REG_DSR_LO, dsr_lo) < 0) {
        fprintf(stderr, "✗ Failed to write DSR_LO\n");
        close(fd);
        return 1;
    }
    printf("    ✓ DSR_LO written\n");

    printf("    Writing DSR_HI (0x%02x) = 0x%08x\n", PVRDMA_REG_DSR_HI, dsr_hi);
    if (write_bar1_reg(fd, PVRDMA_REG_DSR_HI, dsr_hi) < 0) {
        fprintf(stderr, "✗ Failed to write DSR_HI\n");
        close(fd);
        return 1;
    }
    printf("    ✓ DSR_HI written\n");

    /* Read back to verify */
    printf("\n[4] Reading back DSR registers...\n");
    if (read_bar1_reg(fd, PVRDMA_REG_DSR_LO, &dsr_lo) < 0) {
        fprintf(stderr, "✗ Failed to read DSR_LO\n");
        close(fd);
        return 1;
    }
    printf("    DSR_LO = 0x%08x\n", dsr_lo);

    if (read_bar1_reg(fd, PVRDMA_REG_DSR_HI, &dsr_hi) < 0) {
        fprintf(stderr, "✗ Failed to read DSR_HI\n");
        close(fd);
        return 1;
    }
    printf("    DSR_HI = 0x%08x\n", dsr_hi);

    uint64_t read_back = ((uint64_t)dsr_hi << 32) | dsr_lo;
    printf("    Combined address: 0x%lx\n", read_back);

    if (read_back == fake_dsr_addr) {
        printf("    ✓ DSR address registers correct\n");
    } else {
        printf("    ✗ Address mismatch!\n");
    }

    /* Small delay to let any async operations complete */
    printf("\n[5] Waiting 2 seconds for server processing...\n");
    sleep(2);

    printf("\n[6] Test complete, closing connection...\n");
    close(fd);

    printf("\n========================================\n");
    printf("✓ Test COMPLETED\n");
    printf("========================================\n");
    printf("\n");
    printf("Check server logs for:\n");
    printf("  - DMA mapping attempts\n");
    printf("  - Any crash or segfault\n");
    printf("  - load_dsr() execution\n");
    printf("\n");

    return 0;
}
