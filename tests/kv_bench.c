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
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/nvme_ioctl.h>

#include "parse_int.h"

/* NVM Express Key Value command set, opcodes. */
#define KV_OPC_STORE    0x01
#define KV_OPC_RETRIEVE 0x02
#define KV_OPC_DELETE   0x10
/* Exist (0x14) is exercised by the play through nvme-cli, where the
 * round trip matters more than its cost; it is not benchmarked here. */

/* Upper bound on a value passed to sweep_one; large enough to cover any
 * realistic bench sweep, small enough to reject a runaway argv argument. */
#define KV_BENCH_MAX_VALUE_SIZE (64U * 1024U * 1024U)

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
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

static void fill(uint8_t *p, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++) {
        p[i] = (uint8_t)((i * 7U) + seed);
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
    if (len > KV_BENCH_MAX_VALUE_SIZE) {
        fprintf(stderr, "kv_bench: size %u exceeds the bench maximum (%u)\n",
                len, KV_BENCH_MAX_VALUE_SIZE);
        return 1;
    }

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
                st < 0 ? 0U : (unsigned)st);
        goto out;
    }

    st = kv_cmd(fd, nsid, KV_OPC_RETRIEVE, key, dst, len);
    if (st != 0) {
        fprintf(stderr,
                "kv_bench: Retrieve of %u bytes failed: %s (status 0x%x)\n",
                len, st < 0 ? strerror(errno) : "device",
                st < 0 ? 0U : (unsigned)st);
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
    (void)fflush(stdout);
    rc = 0;

out:
    free(src);
    free(dst);
    return rc;
}

/*
 * Check that arg names a namespace's generic char device,
 * /dev/ng<ctrl>n<ns>, and write that name to buf.  This program sends
 * passthrough commands, Stores and Deletes among them, to whatever it
 * opens, so it refuses anything else: another path, a block device such
 * as /dev/nvme0n1, or the right name spelled differently, such as
 * /dev/ng00n2.  The name in buf is rebuilt from the two numbers rather
 * than copied, and must match arg exactly.  A block namespace has a
 * generic char device too, which this can't tell from a Key Value
 * namespace's; the caller has to name the right one.
 */
static bool kv_dev_path(const char *arg, char *buf, size_t size)
{
    static const char prefix[] = "/dev/ng";
    const size_t prefix_len = sizeof(prefix) - 1;

    if (strncmp(arg, prefix, prefix_len) != 0) {
        return false;
    }
    const char *ctrl_str = arg + prefix_len;
    const char *sep = strchr(ctrl_str, 'n');
    if (sep == NULL) {
        return false;
    }

    /* Linux numbers controllers and namespace heads with an int. */
    uint64_t ctrl;
    uint64_t ns;
    if (!ernic_parse_u64_span(ctrl_str, (size_t)(sep - ctrl_str), &ctrl) ||
        ctrl > INT_MAX || !ernic_parse_u64(sep + 1, &ns) || ns > INT_MAX) {
        return false;
    }

    int len = snprintf(buf, size, "/dev/ng%" PRIu64 "n%" PRIu64, ctrl, ns);
    if (len < 0 || (size_t)len >= size) {
        return false;
    }
    return strcmp(buf, arg) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <device> <nsid> <iters> <size> [size ...]\n"
                "  device  the namespace's generic char device,\n"
                "          /dev/ng<ctrl>n<ns>, e.g. /dev/ng0n2\n"
                "prints: size store_GBs retrieve_GBs store_us retrieve_us\n",
                argv[0]);
        return 2;
    }

    /* Room for two of any uint64_t, the type kv_dev_path() prints, not
     * just the INT_MAX it accepts: GCC can lose that bound and then
     * reports the snprintf() as able to truncate. */
    char dev[sizeof("/dev/ng18446744073709551615n18446744073709551615")];
    if (!kv_dev_path(argv[1], dev, sizeof(dev))) {
        fprintf(stderr,
                "kv_bench: device must be a generic char device, "
                "/dev/ng<ctrl>n<ns>, not '%s'\n",
                argv[1]);
        return 2;
    }

    uint32_t nsid;
    if (!ernic_parse_u32(argv[2], &nsid) || nsid == 0) {
        fprintf(stderr,
                "kv_bench: nsid must be a positive integer <= 4294967295\n");
        return 2;
    }

    int iters;
    if (!ernic_parse_int(argv[3], &iters) || iters <= 0) {
        fprintf(stderr, "kv_bench: iters must be a positive integer\n");
        return 2;
    }

    /* The name alone doesn't make it a namespace: refuse a symlink, or
     * anything else that isn't a char device, put in its place. */
    int fd = open(dev, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "kv_bench: cannot open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        fprintf(stderr, "kv_bench: cannot stat %s: %s\n", dev, strerror(errno));
        close(fd);
        return 1;
    }
    if (!S_ISCHR(sb.st_mode)) {
        fprintf(stderr, "kv_bench: %s is not a char device\n", dev);
        close(fd);
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
        uint32_t len;
        if (!ernic_parse_u32(argv[i], &len) || len == 0) {
            fprintf(stderr, "kv_bench: bad size '%s'\n", argv[i]);
            rc = 2;
            break;
        }
        if (sweep_one(fd, nsid, key, len, iters) != 0) {
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
