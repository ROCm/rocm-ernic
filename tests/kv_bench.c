/*
 * kv_bench.c -- Key Value Store/Retrieve round trip and timing, from
 * inside a guest, against a namespace served by the in-process target.
 *
 * WHY THIS EXISTS RATHER THAN A LOOP AROUND nvme-cli
 *
 * `nvme io-passthru` costs about 5 ms per invocation to fork, exec and
 * dynamically link, which on this emulator is two orders of magnitude
 * more than the command it issues.  A shell loop around it therefore
 * measures nvme-cli's startup and reports the same ~5 ms whatever the
 * value size -- a flat line that looks like a working measurement and
 * tracks nothing about the device.  This issues the command directly,
 * so the number is the capsule round trip and the RDMA transfer.
 *
 * It uses ioctl(NVME_IOCTL_IO_CMD) rather than io_uring passthrough:
 * one command at a time is the right shape for a latency figure, and
 * it needs no liburing in the guest.  A queue-depth sweep would need
 * io_uring; this is deliberately not that.
 *
 * Built in the guest from this single file, with no library beyond
 * libc, because that is what the lane can do over scp.
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/nvme_ioctl.h>

/* NVM Express Key Value command set, opcodes. */
#define KV_OPC_STORE    0x01
#define KV_OPC_RETRIEVE 0x02
#define KV_OPC_DELETE   0x10
/* Exist (0x14) is exercised by the play through nvme-cli, where the
 * round trip matters more than its cost; it is not benchmarked here. */

#define KV_KEY_LEN 16

/* Status codes the target returns, for the messages below. */
#define KV_SC_KEY_NOT_EXIST 0x87

static uint32_t le32_of(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*
 * The key is carried in four command dwords rather than in the data
 * buffer: key[0..7] in CDW2 and CDW3, key[8..15] in CDW14 and CDW15,
 * and the length in CDW11[07:00].  CDW10 is the value size.
 */
static void kv_set_key(struct nvme_passthru_cmd *cmd, const uint8_t *key,
                       uint8_t len)
{
    uint8_t raw[KV_KEY_LEN] = {0};

    memcpy(raw, key, len);
    cmd->cdw2 = le32_of(raw);
    cmd->cdw3 = le32_of(raw + 4);
    cmd->cdw14 = le32_of(raw + 8);
    cmd->cdw15 = le32_of(raw + 12);
    cmd->cdw11 = len;
}

/* Returns the NVMe status, 0 on success, or -1 when the ioctl itself
 * failed.  A non-zero status is a device answer, not a transport
 * error, and the two have to be told apart by the caller. */
static int kv_cmd(int fd, uint32_t nsid, uint8_t opcode, const uint8_t *key,
                  void *buf, uint32_t len)
{
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = nsid;
    kv_set_key(&cmd, key, KV_KEY_LEN);
    cmd.cdw10 = len;
    cmd.addr = (uint64_t)(uintptr_t)buf;
    cmd.data_len = len;
    cmd.timeout_ms = 30000;

    int rc = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
    if (rc < 0) {
        return -1;
    }
    return rc;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fill(uint8_t *p, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++) {
        p[i] = (uint8_t)((i * 7u) + seed);
    }
}

/*
 * One size: Store the value, Retrieve it into a cleared buffer and
 * compare, then time each direction.  The compare comes first and is
 * not optional -- a Retrieve that returns the wrong bytes quickly is
 * not a better number, and timing it without checking it is how a
 * broken data path gets reported as a fast one.
 */
static int sweep_one(int fd, uint32_t nsid, const uint8_t *key, uint32_t len,
                     int iters)
{
    /*
     * Every local is declared before the first goto: the project builds
     * with -Wjump-misses-init, where a jump over an initialiser is a
     * warning even when the variable is not read after it.
     */
    uint8_t *src = malloc(len);
    uint8_t *dst = malloc(len);
    double t0 = 0.0;
    double store_s = 0.0;
    double retr_s = 0.0;
    double bytes = 0.0;
    int st = 0;
    int rc = 1;

    if (src == NULL || dst == NULL) {
        fprintf(stderr, "kv_bench: out of memory for %u bytes\n", len);
        goto out;
    }
    fill(src, len, len);
    memset(dst, 0, len);

    st = kv_cmd(fd, nsid, KV_OPC_STORE, key, src, len);
    if (st != 0) {
        fprintf(stderr,
                "kv_bench: Store of %u bytes failed: %s (status 0x%x)\n", len,
                st < 0 ? strerror(errno) : "device",
                st < 0 ? 0u : (unsigned)st);
        goto out;
    }

    st = kv_cmd(fd, nsid, KV_OPC_RETRIEVE, key, dst, len);
    if (st != 0) {
        fprintf(stderr,
                "kv_bench: Retrieve of %u bytes failed: %s (status 0x%x)\n",
                len, st < 0 ? strerror(errno) : "device",
                st < 0 ? 0u : (unsigned)st);
        goto out;
    }

    if (memcmp(src, dst, len) != 0) {
        fprintf(stderr, "kv_bench: Retrieve returned different bytes at %u\n",
                len);
        goto out;
    }

    t0 = now_s();
    for (int i = 0; i < iters; i++) {
        if (kv_cmd(fd, nsid, KV_OPC_STORE, key, src, len) != 0) {
            fprintf(stderr, "kv_bench: Store failed mid-sweep at %u\n", len);
            goto out;
        }
    }
    store_s = now_s() - t0;

    t0 = now_s();
    for (int i = 0; i < iters; i++) {
        if (kv_cmd(fd, nsid, KV_OPC_RETRIEVE, key, dst, len) != 0) {
            fprintf(stderr, "kv_bench: Retrieve failed mid-sweep at %u\n", len);
            goto out;
        }
    }
    retr_s = now_s() - t0;

    bytes = (double)len * (double)iters;
    /* Bytes over seconds is GB/s decimal, which is what the CSV and
     * every other lane in this repo report. */
    printf("%u %.6f %.6f %.3f %.3f\n", len, bytes / store_s / 1e9,
           bytes / retr_s / 1e9, store_s / iters * 1e6, retr_s / iters * 1e6);
    fflush(stdout);
    rc = 0;

out:
    free(src);
    free(dst);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <device> <nsid> <iters> <size> [size ...]\n"
                "  device  a generic char device, e.g. /dev/ng0n2\n"
                "prints: size store_GBs retrieve_GBs store_us retrieve_us\n",
                argv[0]);
        return 2;
    }

    const char *path = argv[1];

    char *end;
    unsigned long long nsid_ll = strtoull(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || nsid_ll == 0 ||
        nsid_ll > UINT32_MAX) {
        fprintf(stderr,
                "kv_bench: nsid must be a positive integer <= 4294967295\n");
        return 2;
    }
    uint32_t nsid = (uint32_t)nsid_ll;

    long iters_l = strtol(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || iters_l <= 0) {
        fprintf(stderr, "kv_bench: iters must be a positive integer\n");
        return 2;
    }
    int iters = (int)iters_l;

    int fd = open(path, O_RDONLY); // codeql[cpp/path-injection] benchmark tool: caller supplies the device path by design
    if (fd < 0) {
        fprintf(stderr, "kv_bench: cannot open %s: %s\n", path,
                strerror(errno));
        return 1;
    }

    /* A fixed key, so a failed run leaves one entry rather than one per
     * size, and so the Delete below removes exactly what was added. */
    uint8_t key[KV_KEY_LEN];
    for (uint8_t i = 0; i < KV_KEY_LEN; i++) {
        key[i] = (uint8_t)(i + 1);
    }

    int rc = 0;
    for (int i = 4; i < argc; i++) {
        unsigned long long len_ll = strtoull(argv[i], &end, 10);
        if (end == argv[i] || *end != '\0' || len_ll == 0 ||
            len_ll > UINT32_MAX) {
            fprintf(stderr, "kv_bench: bad size '%s'\n", argv[i]);
            rc = 2;
            break;
        }
        unsigned long len = (unsigned long)len_ll;
        if (sweep_one(fd, nsid, key, (uint32_t)len, iters) != 0) {
            rc = 1;
            break;
        }
    }

    /* Leave the store as it was found.  The capacity is bounded and a
     * lane that ran repeatedly without this would eventually fill it,
     * which surfaces much later as an unrelated Store failure. */
    int st = kv_cmd(fd, nsid, KV_OPC_DELETE, key, NULL, 0);
    if (st != 0 && st != KV_SC_KEY_NOT_EXIST) {
        fprintf(stderr, "kv_bench: warning: Delete left the key behind\n");
    }

    close(fd);
    return rc;
}
