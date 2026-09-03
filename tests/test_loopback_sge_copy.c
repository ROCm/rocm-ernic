/*
 * Unit tests for the loopback SGE copy helpers in rdma_backend_loopback.c.
 *
 * These functions walk source/destination scatter-gather lists, mapping
 * one SGE at a time and advancing through partial buffers. They previously
 * carried inner "if (idx >= num_sge) break;" guards that the enclosing
 * while-condition already made unreachable; this test exercises the
 * boundary-walking logic (single/multi/partial/mismatched SGEs) to show
 * the copy still transfers the right bytes with those dead guards removed.
 *
 * The static helpers are reached by #including the translation unit; the
 * DMA layer is stubbed with an identity mapping (an SGE addr is just a host
 * pointer), so copies run in-process under AddressSanitizer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the code under test (including its static functions). */
#include "hw/rdma/rdma_backend_loopback.c"

/* ---- Stubs for the TU's external symbols -------------------------------
 * rdma_pci_dma_map is an identity mapping: the SGE "guest address" is a
 * real host pointer, so map returns it unchanged and unmap/sync are no-ops.
 */
void *rdma_pci_dma_map(void *dev, uint64_t addr, uint64_t len)
{
    (void)dev;
    (void)len;
    return (void *)(uintptr_t)addr;
}
void rdma_pci_dma_unmap(void *dev, void *buffer, uint64_t len)
{
    (void)dev;
    (void)buffer;
    (void)len;
}
int pci_dma_sync(PCIDevice *dev, uint64_t guest_addr, uint64_t len)
{
    (void)dev;
    (void)guest_addr;
    (void)len;
    return 0;
}
void *PVRDMA_DEV(void *obj)
{
    return obj;
}
PVRDMAQPStats *pvrdma_get_qp_stats(PVRDMADev *dev, uint32_t qp_handle)
{
    (void)dev;
    (void)qp_handle;
    return NULL;
}
void rdma_backend_complete_work(enum ibv_wc_status status, uint32_t vendor_err,
                                uint32_t byte_len, uint32_t qp_num,
                                enum ibv_wc_opcode opcode, void *ctx)
{
    (void)status;
    (void)vendor_err;
    (void)byte_len;
    (void)qp_num;
    (void)opcode;
    (void)ctx;
}
void error_report(const char *fmt, ...)
{
    (void)fmt;
}
void warn_report(const char *fmt, ...)
{
    (void)fmt;
}
void info_report(const char *fmt, ...)
{
    (void)fmt;
}
/* Receive-completion helpers live in pvrdma_qp_ops.c, which this test does
 * not link; the SGE-copy paths under test never depend on their effect. */
void pvrdma_queue_recv_work_completion(PVRDMADev *dev, uint32_t recv_cq_handle,
                                       uint64_t recv_guest_wr_id,
                                       uint32_t byte_len, uint32_t src_qp_num)
{
    (void)dev;
    (void)recv_cq_handle;
    (void)recv_guest_wr_id;
    (void)byte_len;
    (void)src_qp_num;
}
void pvrdma_queue_recv_imm_work_completion(
    PVRDMADev *dev, uint32_t recv_cq_handle, uint32_t recv_qp_handle,
    uint64_t recv_guest_wr_id, uint32_t byte_len, uint32_t src_qp_num,
    uint32_t imm_data)
{
    (void)dev;
    (void)recv_cq_handle;
    (void)recv_qp_handle;
    (void)recv_guest_wr_id;
    (void)byte_len;
    (void)src_qp_num;
    (void)imm_data;
}

/* ---- Test helpers ------------------------------------------------------ */

static PCIDevice *dummy_pci(void)
{
    static uint8_t dummy;
    return (PCIDevice *)&dummy;
}

/* Fill a buffer with a recognisable, position-dependent pattern. */
static void fill_seq(uint8_t *buf, size_t len, uint8_t base)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(base + i);
    }
}

/*
 * Build an SGE list over freshly-allocated exact-size buffers and seed the
 * source bytes into a linear reference stream. Returns total bytes.
 */
static uint32_t build_sges(struct ibv_sge *sges, const uint32_t *sizes,
                           uint32_t n, uint8_t *ref, int seed_source)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *buf = malloc(sizes[i] ? sizes[i] : 1);
        sges[i].addr = (uint64_t)(uintptr_t)buf;
        sges[i].length = sizes[i];
        sges[i].lkey = 0;
        if (seed_source) {
            fill_seq(buf, sizes[i], (uint8_t)(0x10 * (i + 1)));
            memcpy(ref + total, buf, sizes[i]);
        } else {
            memset(buf, 0, sizes[i]);
        }
        total += sizes[i];
    }
    return total;
}

static void free_sges(struct ibv_sge *sges, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        free((void *)(uintptr_t)sges[i].addr);
    }
}

/* Concatenate destination SGE contents into a linear buffer. */
static void gather_dst(const struct ibv_sge *sges, uint32_t n, uint8_t *out)
{
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(out + off, (void *)(uintptr_t)sges[i].addr, sges[i].length);
        off += sges[i].length;
    }
}

static int check_sge_copy(const char *name, const uint32_t *src_sizes,
                          uint32_t nsrc, const uint32_t *dst_sizes,
                          uint32_t ndst)
{
    struct ibv_sge src[8], dst[8];
    uint8_t src_ref[4096] = {0};
    uint8_t dst_ref[4096] = {0};

    uint32_t src_total = build_sges(src, src_sizes, nsrc, src_ref, 1);
    uint32_t dst_total = build_sges(dst, dst_sizes, ndst, dst_ref, 0);

    int copied = loopback_copy_sge_data(dummy_pci(), src, nsrc, dst, ndst,
                                        LOOPBACK_DATA_PATTERN_PRESERVE);

    uint32_t expect = src_total < dst_total ? src_total : dst_total;
    int fail = 0;
    if (copied != (int)expect) {
        printf("FAIL %-22s: copied=%d expected=%u\n", name, copied, expect);
        fail = 1;
    } else {
        gather_dst(dst, ndst, dst_ref);
        if (memcmp(src_ref, dst_ref, expect) != 0) {
            printf("FAIL %-22s: copied bytes differ from source\n", name);
            fail = 1;
        }
    }

    free_sges(src, nsrc);
    free_sges(dst, ndst);
    return fail;
}

static int check_remote_roundtrip(const char *name, const uint32_t *sizes,
                                  uint32_t n)
{
    struct ibv_sge src[8], dst[8];
    uint8_t src_ref[4096] = {0};
    uint8_t dst_ref[4096] = {0};

    uint32_t total = build_sges(src, sizes, n, src_ref, 1);
    (void)build_sges(dst, sizes, n, dst_ref, 0);

    /* remote address is an identity-mapped scratch buffer of size total */
    uint8_t *remote = calloc(total ? total : 1, 1);
    uint64_t remote_addr = (uint64_t)(uintptr_t)remote;

    int w = loopback_copy_to_remote_addr(dummy_pci(), src, n, remote_addr,
                                         total, LOOPBACK_DATA_PATTERN_PRESERVE);
    int r = loopback_copy_from_remote_addr(dummy_pci(), remote_addr, total, dst,
                                           n, LOOPBACK_DATA_PATTERN_PRESERVE);

    int fail = 0;
    if (w != (int)total || r != (int)total) {
        printf("FAIL %-22s: write=%d read=%d expected=%u\n", name, w, r, total);
        fail = 1;
    } else {
        gather_dst(dst, n, dst_ref);
        if (memcmp(src_ref, dst_ref, total) != 0) {
            printf("FAIL %-22s: round-trip bytes differ\n", name);
            fail = 1;
        }
    }

    free(remote);
    free_sges(src, n);
    free_sges(dst, n);
    return fail;
}

int main(void)
{
    int failures = 0;

    /* loopback_copy_sge_data: single, multi, partial, mismatched */
    failures += check_sge_copy("single->single", (uint32_t[]){64}, 1,
                               (uint32_t[]){64}, 1);
    failures +=
        check_sge_copy("multi-src->single-dst", (uint32_t[]){10, 20, 30}, 3,
                       (uint32_t[]){60}, 1);
    failures += check_sge_copy("single-src->multi-dst", (uint32_t[]){60}, 1,
                               (uint32_t[]){10, 20, 30}, 3);
    failures += check_sge_copy("mismatched-boundaries", (uint32_t[]){10, 20}, 2,
                               (uint32_t[]){5, 5, 20}, 3);
    failures += check_sge_copy("more-src-than-dst", (uint32_t[]){40, 40}, 2,
                               (uint32_t[]){40}, 1);
    failures += check_sge_copy("more-dst-than-src", (uint32_t[]){40}, 1,
                               (uint32_t[]){40, 40}, 2);

    /* remote copy helpers (RDMA write/read paths) */
    failures += check_remote_roundtrip("remote-single", (uint32_t[]){100}, 1);
    failures +=
        check_remote_roundtrip("remote-multi", (uint32_t[]){16, 48, 80}, 3);

    if (failures) {
        printf("loopback_sge_copy: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("loopback_sge_copy: all checks passed\n");
    return 0;
}
