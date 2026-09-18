/*
 *
 * Unit tests for the memory-region bounds checks in rdma_backend_tcp.c.
 *
 * Regression coverage for an integer-overflow bounds bypass. Four sites --
 * the RDMA_WRITE handler, the RDMA_READ_REQ handler, tcp_wr_map_sge(), and
 * the loopback path in tcp_post_send() -- all guarded a memory region with
 *
 *     if (addr < mr->start || addr + len > mr->start + mr->length) reject;
 *
 * Both addr and len come off the wire, so addr + len is unchecked 64-bit
 * arithmetic. Choosing addr just below mr->start *modulo 2^64* makes addr
 * compare greater than mr->start and makes addr + len wrap back down past
 * zero into the region, so the check passes. The host pointer is then
 * computed as mr->virt + (addr - mr->start), and that subtraction wraps the
 * same way, landing the copy a chosen number of bytes *below* mr->virt.
 *
 * Concretely, with mr->start = 16 and a target 80 bytes below the region:
 *
 *     addr = 16 - 80          = 0xFFFFFFFFFFFFFFC0   (>= mr->start)
 *     addr + 96               = 32                   (<= start + length)
 *     mr->virt + (addr - 16)  = mr->virt - 80        (outside the region)
 *
 * That is the primitive the rest of the disclosure's chain builds on: an
 * attacker-chosen out-of-region read or write at a controlled offset.
 *
 * The registered region is carved out of the middle of one larger heap
 * buffer, with poisoned guard bands on either side. A wrapped access
 * therefore stays inside a single allocation -- the test can inspect the
 * guard bytes and report a clean failure instead of dying in an ASan
 * redzone, and a leak of guard contents onto the wire is checkable rather
 * than merely undefined.
 *
 * The two wire-driven cases run the real per-connection receive thread over
 * a socketpair. Each sends the malicious message followed by a legitimate
 * one; because a single thread drains an ordered stream, the reply to the
 * legitimate message proves the malicious one was already processed to
 * completion, so the assertions never depend on a timeout.
 *
 * The static functions are reached by #including the translation unit; the
 * paths under test need only the MR lookup, so the rest of the backend's
 * externals are stubbed out.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

/* Pull in the code under test (including its static functions) */
#include "hw/rdma/rdma_backend_tcp.c"

/* ---- Test fixture geometry ---------------------------------------------
 *
 *   backing[0 .. GUARD_LO)              low guard band  (poisoned)
 *   backing[GUARD_LO .. GUARD_LO+MR_LEN) registered region -> mr->virt
 *   backing[GUARD_LO+MR_LEN .. END)     high guard band (poisoned)
 *
 * MR_START is deliberately small: the wrap requires mr->start to be less
 * than the offset being targeted below the region, since addr is computed
 * as mr->start - WRAP_BELOW and must underflow to a huge value.
 *
 * That pins this fixture to the addr + len wrap point. The other one,
 * mr->start + mr->length, needs a high start instead and so cannot share
 * the fixture; test_high_start_region() covers it with its own bounds.
 */
#define GUARD_LO    1024
#define MR_LEN      1024
#define GUARD_HI    1024
#define BACKING_LEN (GUARD_LO + MR_LEN + GUARD_HI)

#define MR_START   16u  /* guest address of the region's first byte */
#define MR_RKEY    0x42 /* the only handle the stubbed lookup answers for */
#define GUARD_BYTE 0xC7

/* Target WRAP_BELOW bytes under mr->virt, copying WRAP_LEN bytes. */
#define WRAP_BELOW 80
#define WRAP_LEN   96

/*
 * Offset within the region used as the *source* of the loopback write. Far
 * enough past the start that [SRC_OFFSET, SRC_OFFSET+WRAP_LEN) cannot overlap
 * the wrapped destination at -WRAP_BELOW.
 */
#define SRC_OFFSET 512

/*
 * Distinct from GUARD_BYTE so a failure message can say which way the
 * boundary was crossed: attacker payload landing in the guard band (write
 * side) versus guard contents reaching the wire (read side).
 */
#define ATTACK_BYTE 0x5A

/*
 * The wrapped guest address. Computed in uint64_t, where unsigned overflow
 * is defined, so this is the same arithmetic an attacker performs.
 */
static uint64_t wrapped_addr(void)
{
    return (uint64_t)MR_START - (uint64_t)WRAP_BELOW;
}

/* The fixture's single MR, published to the code under test via the
 * rdma_rm_get_mr() stub below. */
static RdmaRmMR test_mr;
static uint8_t *backing;

static void fixture_init(void)
{
    backing = malloc(BACKING_LEN);
    if (!backing) {
        abort();
    }
    memset(backing, GUARD_BYTE, BACKING_LEN);

    test_mr.virt = backing + GUARD_LO;
    test_mr.start = MR_START;
    test_mr.length = MR_LEN;
}

static void fixture_destroy(void)
{
    free(backing);
    backing = NULL;
}

/* True if every byte of both guard bands still holds GUARD_BYTE. */
static int guards_intact(const char *name)
{
    for (size_t i = 0; i < GUARD_LO; i++) {
        if (backing[i] != GUARD_BYTE) {
            printf("FAIL %-22s: low guard byte %zu is 0x%02x, expected 0x%02x "
                   "(write landed %zu bytes below the region)\n",
                   name, i, backing[i], GUARD_BYTE, GUARD_LO - i);
            return 0;
        }
    }
    for (size_t i = GUARD_LO + MR_LEN; i < BACKING_LEN; i++) {
        if (backing[i] != GUARD_BYTE) {
            printf("FAIL %-22s: high guard byte %zu is 0x%02x, "
                   "expected 0x%02x (write ran past the region)\n",
                   name, i, backing[i], GUARD_BYTE);
            return 0;
        }
    }
    return 1;
}

/* ---- Stubs for the TU's external symbols -------------------------------
 * Only the MR lookup carries behaviour; the rest exist to satisfy the
 * linker and to keep the paths under test from touching a real device.
 */
RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    (void)dev_res;
    return mr_handle == MR_RKEY ? &test_mr : NULL;
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
int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    (void)thread;
    (void)name;
    (void)start_routine;
    (void)arg;
    (void)mode;
    return 0;
}
void qemu_thread_exit(void *retval)
{
    (void)retval;
    abort();
}
void qemu_thread_join(QemuThread *thread)
{
    (void)thread;
}
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)dir;
    return NULL;
}
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;
}
int pci_dma_sync(PCIDevice *dev, uint64_t guest_addr, uint64_t len)
{
    (void)dev;
    (void)guest_addr;
    (void)len;
    return 0;
}
size_t dhcp_server_process(DhcpServer *server, const struct dhcp_packet *req,
                           size_t req_len, struct dhcp_packet *resp,
                           size_t max_resp_len)
{
    (void)server;
    (void)req;
    (void)req_len;
    (void)resp;
    (void)max_resp_len;
    return 0;
}
int eth_rx_inject_frame_mesh_blocking(PVRDMADev *dev, const void *frame_data,
                                      size_t len)
{
    (void)dev;
    (void)frame_data;
    (void)len;
    return 0;
}
/*
 * Recorded completions.
 *
 * A rejected access is only half-handled if it merely declines to copy: the
 * caller still has to be told it failed. The loopback path used to fall
 * through from its bounds-check failure to an unconditional IBV_WC_SUCCESS,
 * so the guest was told a write had landed that never happened. The guard
 * bands cannot see that -- memory is untouched either way, which is exactly
 * the bug -- so the completion is captured here and asserted on separately.
 */
static struct {
    unsigned count;
    enum ibv_wc_status status;
    uint32_t byte_len;
    uint32_t qp_num;
    enum ibv_wc_opcode opcode;
} last_completion;

static void completions_reset(void)
{
    memset(&last_completion, 0, sizeof(last_completion));
}

void rdma_backend_complete_work(enum ibv_wc_status status, uint32_t vendor_err,
                                uint32_t byte_len, uint32_t qp_num,
                                enum ibv_wc_opcode opcode, void *ctx)
{
    (void)vendor_err;
    (void)ctx;

    last_completion.count++;
    last_completion.status = status;
    last_completion.byte_len = byte_len;
    last_completion.qp_num = qp_num;
    last_completion.opcode = opcode;
}

/* ---- Direct table test of the bounds predicate ------------------------- */

struct range_case {
    const char *name;
    uint64_t addr;
    uint64_t len;
    bool expect_ok;
};

/* Run one table against one region. Shared by the low- and high-start
 * fixtures below, which differ only in the region they describe. */
static int run_range_cases(const RdmaRmMR *mr, const struct range_case *cases,
                           size_t n)
{
    int fail = 0;

    for (size_t i = 0; i < n; i++) {
        bool got = tcp_mr_range_ok(mr, cases[i].addr, cases[i].len);
        if (got != cases[i].expect_ok) {
            printf("FAIL range-predicate    : %s addr=0x%" PRIx64
                   " len=0x%" PRIx64 " against mr=[0x%" PRIx64 " len=%" PRIu64
                   "] -> %s, expected %s\n",
                   cases[i].name, cases[i].addr, cases[i].len, mr->start,
                   (uint64_t)mr->length, got ? "accept" : "reject",
                   cases[i].expect_ok ? "accept" : "reject");
            fail = 1;
        }
    }
    return fail;
}

static int test_range_predicate(void)
{
    const uint64_t start = MR_START;
    const uint64_t len = MR_LEN;
    const struct range_case cases[] = {
        /* Ordinary accepts */
        {"whole-region", start, len, true},
        {"zero-length-at-start", start, 0, true},
        {"zero-length-at-end", start + len, 0, true},
        {"last-byte", start + len - 1, 1, true},
        {"interior", start + 100, 200, true},

        /* Ordinary rejects */
        {"one-below", start - 1, 1, false},
        {"one-past-end", start + len, 1, false},
        {"one-too-long", start, len + 1, false},
        {"straddles-end", start + len - 1, 2, false},

        /* The overflow cases: each of these passed the old check */
        {"wrap-below-region", (uint64_t)MR_START - WRAP_BELOW, WRAP_LEN, false},
        {"addr-max", UINT64_MAX, 1, false},
        {"addr-max-len-wraps", UINT64_MAX, 2, false},
        {"len-max", start, UINT64_MAX, false},
        {"both-max", UINT64_MAX, UINT64_MAX, false},
        /* addr + len sums to exactly mr->start: wraps all the way to 0 */
        {"wrap-to-start", (uint64_t)0 - 1, (uint64_t)MR_START + 1, false},
    };
    int fail =
        run_range_cases(&test_mr, cases, sizeof(cases) / sizeof(cases[0]));

    /* A NULL region is never in bounds -- callers rely on this to avoid a
     * separate NULL check before every use. */
    if (tcp_mr_range_ok(NULL, start, 1)) {
        printf("FAIL range-predicate    : NULL mr accepted\n");
        fail = 1;
    }

    /*
     * Nor is a region with no host mapping, however well-formed its bounds.
     * rdma_rm_alloc_mr() stores virt = NULL when the host mapping fails and
     * create_mr() carries on regardless, so start and length here are purely
     * guest-chosen; accepting them would have the callers add the offset to
     * NULL and write to an absolute guest-chosen address. The bounds below
     * are the whole registered region -- unimpeachable except for virt.
     */
    {
        RdmaRmMR unmapped = test_mr;
        unmapped.virt = NULL;
        if (tcp_mr_range_ok(&unmapped, start, len)) {
            printf("FAIL range-predicate    : MR with virt=NULL accepted for "
                   "its own full range (addr=0x%" PRIx64 " len=0x%" PRIx64
                   ")\n",
                   start, len);
            fail = 1;
        }
    }

    if (!fail) {
        printf("PASS %-22s: %zu range cases\n", "range-predicate",
               sizeof(cases) / sizeof(cases[0]));
    }
    return fail;
}

/*
 * The second wrap point: mr->start + mr->length, rather than addr + len.
 *
 * Everything above pins mr->start at 16, which exercises only the addr side.
 * But mr->start is no more trustworthy than addr: rdma_rm_alloc_mr() stores
 * cmd->start verbatim, and create_mr() validates neither it nor cmd->length
 * before registering (see "Known issues" in the CHANGELOG). A guest can
 * therefore register a region whose own end wraps, and the naive
 *
 *     addr < mr->start || addr + len > mr->start + mr->length
 *
 * then computes an "end" below its start, which is wrong in both directions
 * depending on the region's size:
 *
 *   - small region: the vacuous upper bound accepts an access whose offset
 *     lands past the mapping -- the same out-of-region primitive the addr
 *     wrap gives, reached from the other operand;
 *   - large region: the wrapped end is a tiny number, so a legitimate
 *     in-bounds access is *rejected*.
 *
 * Both directions are asserted, so an over-strict "fix" that simply refuses
 * every high-start region fails here rather than silently breaking a valid
 * one. The offsets are checked against the containment truth, not against
 * what the old code happened to do.
 *
 * Note what "in bounds" means once the end wraps: the region's addressable
 * part stops at UINT64_MAX, since a guest address cannot go higher. A
 * 1024-byte region based 99 below UINT64_MAX has only 100 reachable bytes,
 * and an address computed as start + length - 1 wraps to a small number
 * that names none of them. The cases below pin both halves of that.
 *
 * virt is the same backing as the low-start fixture: these cases never
 * dereference the pointer, and a distinct mapping would prove nothing about
 * a predicate that only does arithmetic.
 */
static int test_high_start_region(void)
{
    /*
     * 99 below UINT64_MAX, so start + length wraps for any region longer
     * than 100 bytes, and an in-region offset can reach at most 99.
     */
    const uint64_t hs = UINT64_MAX - 99;

    /* Small enough that offsets inside the representable range still escape
     * the mapping: offset 50 of a 16-byte region is genuinely out. */
    const uint64_t small_len = 16;
    const struct range_case small_cases[] = {
        /* Accepts: wholly inside the 16 bytes that really exist. */
        {"high-start-base", hs, small_len, true},
        {"high-start-last-byte", hs + small_len - 1, 1, true},

        /* Rejects. The old check accepted both: start + length wrapped, so
         * the upper half of the test was vacuous and any offset passed. */
        {"high-start-past-region", hs + 50, 50, false},
        {"high-start-at-max", UINT64_MAX, 1, false},
        {"high-start-one-past", hs + small_len, 1, false},
    };

    /*
     * Longer than the 100 bytes of headroom, so start + length wraps. These
     * accesses are genuinely in bounds and must be accepted -- the old
     * check rejected them, comparing against an "end" that had wrapped to a
     * small number.
     */
    const uint64_t big_len = 1024;
    const struct range_case big_cases[] = {
        {"wrapping-end-base", hs, 50, true},
        {"wrapping-end-interior", hs + 50, 50, true},
        {"wrapping-end-whole", hs, big_len, true},
        /*
         * The last byte a guest can actually name. Only 100 of this
         * region's 1024 bytes have addresses below 2^64; the rest are
         * unreachable however the check is written, because a guest address
         * cannot exceed UINT64_MAX. Accepting this one and rejecting
         * everything past it is what "in bounds" means for such a region.
         */
        {"wrapping-end-at-max", UINT64_MAX, 1, true},

        /* Still rejected, wrapped end or not. */
        {"wrapping-end-too-long", hs, big_len + 1, false},
        {"wrapping-end-below", hs - 1, 1, false},
        {"wrapping-end-len-max", hs, UINT64_MAX, false},
        /*
         * hs + big_len - 1 would be the last byte if the region's end did
         * not wrap; it lands at 0x39b instead, far below hs, so it names no
         * part of the region and must be rejected. Computing a bound this
         * way is exactly the mistake the predicate avoids.
         */
        {"wrapping-end-past-max", hs + big_len - 1, 1, false},
    };

    RdmaRmMR high = test_mr; /* same virt; only the bounds differ */
    int fail;

    high.start = hs;
    high.length = small_len;
    fail = run_range_cases(&high, small_cases,
                           sizeof(small_cases) / sizeof(small_cases[0]));

    high.length = big_len;
    fail |= run_range_cases(&high, big_cases,
                            sizeof(big_cases) / sizeof(big_cases[0]));

    if (!fail) {
        printf("PASS %-22s: %zu cases, region end wrapping 64 bits\n",
               "high-start-region",
               sizeof(small_cases) / sizeof(small_cases[0]) +
                   sizeof(big_cases) / sizeof(big_cases[0]));
    }
    return fail;
}

/* ---- Wire-driven cases ------------------------------------------------- */

#define TEST_PEER_NODE  7
#define TEST_LOCAL_NODE 1

/* Read exactly len bytes, or return -1. */
static int read_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t ret = recv(fd, (char *)buf + got, len - got, 0);
        if (ret <= 0) {
            return -1;
        }
        got += (size_t)ret;
    }
    return 0;
}

/*
 * Sequence numbers identifying the two requests each wire case sends. The
 * handlers echo hdr.seq back on the reply, so the first reply says which
 * request produced it -- the assertion the msg_type alone cannot make, since
 * a served wrapped op replies with the same message type as a legitimate one.
 */
#define SEQ_ATTACK 0xA11
#define SEQ_LEGIT  0x1E6

/* Push one message onto the socket in the wire format tcp_recv_message()
 * expects: byte-swapped header, then a raw (host-order) payload. */
static int send_raw(int fd, TcpMsgType type, uint32_t seq, const void *payload,
                    size_t len)
{
    TcpMsgHeader hdr;

    hdr.magic = htonl(TCP_PROTOCOL_MAGIC);
    hdr.msg_type = htonl(type);
    hdr.msg_len = htonl((uint32_t)len);
    hdr.seq = htonl(seq);
    hdr.src_node_id = htonl(TEST_PEER_NODE);
    hdr.dst_node_id = htonl(TEST_LOCAL_NODE);
    hdr.src_qpn = htonl(1);
    hdr.dst_qpn = htonl(1);

    if (send(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        return -1;
    }
    if (len && send(fd, payload, len, 0) != (ssize_t)len) {
        return -1;
    }
    return 0;
}

/* An RDMA op header followed by `data_len` bytes of payload. */
static int send_rdma_op(int fd, TcpMsgType type, uint32_t seq, uint64_t raddr,
                        uint32_t data_len, const void *data)
{
    uint8_t buf[sizeof(TcpRdmaOpHeader) + WRAP_LEN];
    TcpRdmaOpHeader oh;
    size_t total = sizeof(oh) + (data ? data_len : 0);

    if (total > sizeof(buf)) {
        return -1;
    }

    /* Payload fields are consumed in host order -- tcp_recv_message() only
     * byte-swaps the header. */
    oh.remote_addr = raddr;
    oh.rkey = MR_RKEY;
    oh.data_len = data_len;

    memcpy(buf, &oh, sizeof(oh));
    if (data) {
        memcpy(buf + sizeof(oh), data, data_len);
    }
    return send_raw(fd, type, seq, buf, total);
}

/*
 * Fixture for the two receive-thread cases: a socketpair with the real
 * per-connection receive thread on one end and the test on the other.
 */
struct recv_fixture {
    TcpBackendPrivate priv;
    TcpConnection conn;
    RdmaBackendDev backend_dev;
    RdmaDeviceResources dev_res;
    pthread_t tid;
    int sv[2];
};

static int recv_fixture_start(struct recv_fixture *f)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, f->sv) != 0) {
        return -1;
    }

    memset(&f->priv, 0, sizeof(f->priv));
    memset(&f->backend_dev, 0, sizeof(f->backend_dev));
    memset(&f->dev_res, 0, sizeof(f->dev_res));

    f->backend_dev.rdma_dev_res = &f->dev_res;
    f->backend_dev.backend_private = &f->priv;
    f->priv.backend_dev = &f->backend_dev;
    f->priv.local_node_id = TEST_LOCAL_NODE;
    f->priv.connections = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_mutex_init(&f->priv.conn_table_lock);
    qemu_mutex_init(&f->priv.lock);
    tcp_bufpool_init(&f->priv.recv_pool);

    memset(&f->conn, 0, sizeof(f->conn));
    f->conn.node_id = TEST_PEER_NODE;
    f->conn.sockfd = f->sv[0];
    f->conn.is_connected = true;
    f->conn.recv_thread_running = true;
    f->conn.priv = &f->priv;
    qemu_mutex_init(&f->conn.lock);

    /* Registered under its own node id so replies addressed back to the
     * sender return on the same socketpair. */
    g_hash_table_insert(f->priv.connections, GUINT_TO_POINTER(TEST_PEER_NODE),
                        &f->conn);

    if (pthread_create(&f->tid, NULL, tcp_recv_thread_per_conn, &f->conn) !=
        0) {
        close(f->sv[0]);
        close(f->sv[1]);
        return -1;
    }
    return 0;
}

static void recv_fixture_stop(struct recv_fixture *f)
{
    f->conn.recv_thread_running = false;
    pthread_join(f->tid, NULL);

    close(f->sv[0]);
    close(f->sv[1]);
    qemu_mutex_destroy(&f->conn.lock);
    qemu_mutex_destroy(&f->priv.lock);
    qemu_mutex_destroy(&f->priv.conn_table_lock);
    tcp_bufpool_destroy(&f->priv.recv_pool);
    g_hash_table_destroy(f->priv.connections);
}

/*
 * RDMA_WRITE with a wrapping remote address must be rejected outright.
 *
 * A legitimate write follows it; its completion reply is the barrier that
 * proves the malicious message has already been handled.
 */
static int test_write_wrap(void)
{
    const char *name = "rdma-write-wrap";
    struct recv_fixture f;
    uint8_t attack[WRAP_LEN];
    uint8_t legit[WRAP_LEN];
    TcpMsgHeader reply;
    int fail = 0;

    if (recv_fixture_start(&f) != 0) {
        printf("FAIL %-22s: fixture setup failed\n", name);
        return 1;
    }

    memset(attack, ATTACK_BYTE, sizeof(attack));
    memset(legit, 0x11, sizeof(legit));

    if (send_rdma_op(f.sv[1], TCP_MSG_RDMA_WRITE, SEQ_ATTACK, wrapped_addr(),
                     WRAP_LEN, attack) != 0 ||
        send_rdma_op(f.sv[1], TCP_MSG_RDMA_WRITE, SEQ_LEGIT, MR_START, WRAP_LEN,
                     legit) != 0) {
        printf("FAIL %-22s: send failed\n", name);
        recv_fixture_stop(&f);
        return 1;
    }

    /*
     * Exactly one completion, and it must be the legitimate write's. A served
     * wrapped write also answers with TCP_MSG_COMPLETION, so the message type
     * proves nothing here -- the echoed seq is what distinguishes them.
     */
    if (read_exact(f.sv[1], &reply, sizeof(reply)) != 0) {
        printf("FAIL %-22s: no completion for the legitimate write\n", name);
        fail = 1;
    } else if (ntohl(reply.msg_type) != TCP_MSG_COMPLETION) {
        printf("FAIL %-22s: first reply was msg_type %u, expected "
               "COMPLETION (%u)\n",
               name, ntohl(reply.msg_type), TCP_MSG_COMPLETION);
        fail = 1;
    } else if (ntohl(reply.seq) != SEQ_LEGIT) {
        printf("FAIL %-22s: first completion echoed seq 0x%x, expected the "
               "legitimate write's 0x%x -- the wrapped write (seq 0x%x) was "
               "acknowledged rather than rejected\n",
               name, ntohl(reply.seq), SEQ_LEGIT, SEQ_ATTACK);
        fail = 1;
    }

    if (!guards_intact(name)) {
        fail = 1;
    }

    /* The legitimate write must still have landed, or the fix over-rejects. */
    if (memcmp((uint8_t *)test_mr.virt, legit, WRAP_LEN) != 0) {
        printf("FAIL %-22s: legitimate in-bounds write did not land\n", name);
        fail = 1;
    }

    recv_fixture_stop(&f);
    if (!fail) {
        printf("PASS %-22s: wrapped write rejected, guards intact\n", name);
    }
    return fail;
}

/*
 * RDMA_READ_REQ with a wrapping remote address must not put out-of-region
 * bytes on the wire. A legitimate read follows; the first response back must
 * be that one, carrying region contents rather than guard contents.
 */
static int test_read_wrap(void)
{
    const char *name = "rdma-read-wrap";
    struct recv_fixture f;
    TcpMsgHeader reply;
    uint8_t body[WRAP_LEN];
    uint8_t expect[WRAP_LEN];
    int fail = 0;

    if (recv_fixture_start(&f) != 0) {
        printf("FAIL %-22s: fixture setup failed\n", name);
        return 1;
    }

    /* Recognisable region contents, distinct from the guard poison. */
    memset(test_mr.virt, 0x33, MR_LEN);
    memset(expect, 0x33, sizeof(expect));

    if (send_rdma_op(f.sv[1], TCP_MSG_RDMA_READ_REQ, SEQ_ATTACK, wrapped_addr(),
                     WRAP_LEN, NULL) != 0 ||
        send_rdma_op(f.sv[1], TCP_MSG_RDMA_READ_REQ, SEQ_LEGIT, MR_START,
                     WRAP_LEN, NULL) != 0) {
        printf("FAIL %-22s: send failed\n", name);
        recv_fixture_stop(&f);
        return 1;
    }

    /*
     * As in the write case, a served wrapped read answers with the same
     * message type as a legitimate one, so the echoed seq carries the
     * assertion. The body check below is the independent one: it catches a
     * served read even if the seq were ever to match.
     */
    if (read_exact(f.sv[1], &reply, sizeof(reply)) != 0 ||
        read_exact(f.sv[1], body, WRAP_LEN) != 0) {
        printf("FAIL %-22s: no response for the legitimate read\n", name);
        fail = 1;
    } else if (ntohl(reply.msg_type) != TCP_MSG_RDMA_READ_RESP) {
        printf("FAIL %-22s: first reply was msg_type %u, expected "
               "READ_RESP (%u)\n",
               name, ntohl(reply.msg_type), TCP_MSG_RDMA_READ_RESP);
        fail = 1;
    } else if (ntohl(reply.seq) != SEQ_LEGIT) {
        printf("FAIL %-22s: first response echoed seq 0x%x, expected the "
               "legitimate read's 0x%x -- the wrapped read (seq 0x%x) was "
               "served\n",
               name, ntohl(reply.seq), SEQ_LEGIT, SEQ_ATTACK);
        fail = 1;
    } else if (memcmp(body, expect, WRAP_LEN) != 0) {
        /* The first response carries guard bytes: the wrapped read was
         * served, and out-of-region memory reached the peer. */
        printf("FAIL %-22s: first response body is not region contents -- "
               "byte 0 is 0x%02x (guard poison is 0x%02x); the wrapped "
               "read leaked out-of-region memory\n",
               name, body[0], GUARD_BYTE);
        fail = 1;
    }

    if (!guards_intact(name)) {
        fail = 1;
    }

    recv_fixture_stop(&f);
    if (!fail) {
        printf("PASS %-22s: wrapped read rejected, no leak on the wire\n",
               name);
    }
    return fail;
}

/* ---- tcp_wr_map_sge() -------------------------------------------------- */

/*
 * A work request containing an unmappable scatter-gather entry must be
 * refused whole, not mapped down to the entries that happen to be valid.
 *
 * Zeroing the bad entry's length and carrying on -- what this did before --
 * looks safe, since nothing copies through a NULL host pointer. But every
 * copy loop downstream walks the entries in order and advances its cursor
 * only for entries it actually copies, so a skipped entry does not leave a
 * hole: it slides all the later data down by that entry's length. The
 * request then writes the right number of bytes to the wrong offsets and
 * reports success. Refusing it is the only answer that keeps the surviving
 * entries' placement meaningful.
 *
 * The valid-entry case is asserted alongside so a fix that simply refuses
 * everything fails here.
 */
static int test_sge_wrap(void)
{
    const char *name = "sge-map-wrap";
    RdmaBackendDev backend_dev;
    RdmaDeviceResources dev_res;
    TcpQP tqp;
    TcpWR wr;
    struct ibv_sge sge[2];
    int fail = 0;

    memset(&backend_dev, 0, sizeof(backend_dev));
    memset(&dev_res, 0, sizeof(dev_res));
    memset(&tqp, 0, sizeof(tqp));
    memset(&wr, 0, sizeof(wr));

    backend_dev.rdma_dev_res = &dev_res;
    tqp.backend_dev = &backend_dev;

    /* [0] wraps below the region; [1] is a legitimate in-bounds entry. The
     * valid entry must not rescue the request. */
    memset(sge, 0, sizeof(sge));
    sge[0].addr = wrapped_addr();
    sge[0].length = WRAP_LEN;
    sge[0].lkey = MR_RKEY;
    sge[1].addr = MR_START;
    sge[1].length = WRAP_LEN;
    sge[1].lkey = MR_RKEY;

    if (tcp_wr_map_sge(&tqp, &wr, sge, 2)) {
        printf("FAIL %-22s: request with a wrapped SGE was accepted; entry 0 "
               "mapped to %p len %u, entry 1 to %p len %u\n",
               name, wr.sge[0].host_addr, wr.sge[0].length, wr.sge[1].host_addr,
               wr.sge[1].length);
        fail = 1;
    }
    if (wr.sge[0].host_addr != NULL) {
        printf("FAIL %-22s: wrapped SGE still mapped to %p (region base "
               "is %p)\n",
               name, wr.sge[0].host_addr, test_mr.virt);
        fail = 1;
    }

    /* An all-valid request is still mapped, entry for entry. */
    memset(&wr, 0, sizeof(wr));
    sge[0].addr = MR_START;
    sge[0].length = WRAP_LEN;
    if (!tcp_wr_map_sge(&tqp, &wr, sge, 2)) {
        printf("FAIL %-22s: request with two in-bounds SGEs was refused\n",
               name);
        fail = 1;
    } else if (wr.sge[0].host_addr != test_mr.virt ||
               wr.sge[1].host_addr != test_mr.virt ||
               wr.sge[0].length != WRAP_LEN || wr.sge[1].length != WRAP_LEN) {
        printf("FAIL %-22s: in-bounds SGEs mapped to %p/%p len %u/%u, "
               "expected %p len %u for both\n",
               name, wr.sge[0].host_addr, wr.sge[1].host_addr, wr.sge[0].length,
               wr.sge[1].length, test_mr.virt, WRAP_LEN);
        fail = 1;
    }

    /*
     * More entries than the fixed array holds. max_sge is 32 and the guest's
     * count is checked upstream, but wr->num_sge travels to the peer inside
     * TcpWR and bounds receive-side loops, so the backend refuses rather than
     * trusting that check to stay in place.
     */
    memset(&wr, 0, sizeof(wr));
    if (tcp_wr_map_sge(&tqp, &wr, sge, 33)) {
        printf("FAIL %-22s: num_sge=33 accepted into a 32-entry array "
               "(wr.num_sge left at %u)\n",
               name, wr.num_sge);
        fail = 1;
    }

    if (!guards_intact(name)) {
        fail = 1;
    }
    if (!fail) {
        printf("PASS %-22s: partial mapping refused, valid request mapped\n",
               name);
    }
    return fail;
}

/* ---- tcp_post_send() loopback ------------------------------------------ */

/*
 * Must match the PvrdmaCompHandlerCtx layout that tcp_post_send() casts its
 * ctx to (it redeclares the same layout locally).
 */
typedef struct {
    void *dev;
    uint32_t cq_handle;
    uint32_t qp_handle;
    struct pvrdma_cqe cqe;
    uint32_t opcode;
    uint64_t remote_addr;
    uint32_t rkey;
} TestCompCtx;

/*
 * Drive one loopback RDMA_WRITE through tcp_post_send() to `raddr` and report
 * what came back: the completion recorded by the stub and the device's
 * RDMA-write byte counter.
 *
 * The destination node equals the local node, so the request takes the
 * loopback shortcut that copies straight into the target region instead of
 * going to the wire.
 */
static void run_loopback_write(uint64_t raddr, uint64_t *rdma_write_bytes)
{
    RdmaBackendDev backend_dev;
    RdmaDeviceResources dev_res;
    RdmaBackendQP qp;
    TcpBackendPrivate priv;
    TcpQP tqp;
    TestCompCtx ctx;
    struct ibv_sge sge;
    const uint32_t qpn = 1;

    /*
     * A real device struct, because the byte counters live on it: the stats
     * helper casts backend_dev.dev to PVRDMADev and would return early on the
     * NULL this test used to pass, hiding the very update being asserted.
     * Heap-allocated as it is far too large for a stack frame.
     */
    PVRDMADev *dev = g_new0(PVRDMADev, 1);

    memset(&backend_dev, 0, sizeof(backend_dev));
    memset(&dev_res, 0, sizeof(dev_res));
    memset(&priv, 0, sizeof(priv));
    memset(&tqp, 0, sizeof(tqp));
    memset(&qp, 0, sizeof(qp));
    memset(&ctx, 0, sizeof(ctx));

    backend_dev.rdma_dev_res = &dev_res;
    backend_dev.backend_private = &priv;
    backend_dev.dev = (PCIDevice *)dev;
    priv.backend_dev = &backend_dev;
    priv.local_node_id = TEST_LOCAL_NODE;
    priv.qps = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_mutex_init(&priv.lock);

    tqp.qpn = qpn;
    tqp.backend_dev = &backend_dev;
    tqp.send_queue = g_queue_new();
    tqp.recv_queue = g_queue_new();
    /* Same node as priv.local_node_id: takes the loopback branch. */
    tqp.remote_node_id = TEST_LOCAL_NODE;
    g_hash_table_insert(priv.qps, GUINT_TO_POINTER(qpn), &tqp);

    qp.ibqp = (struct ibv_qp *)(uintptr_t)qpn;

    memset(&sge, 0, sizeof(sge));
    sge.addr = MR_START + SRC_OFFSET;
    sge.length = WRAP_LEN;
    sge.lkey = MR_RKEY;

    ctx.opcode = PVRDMA_WR_RDMA_WRITE;
    ctx.cqe.opcode = IBV_WC_RDMA_WRITE;
    ctx.remote_addr = raddr;
    ctx.rkey = MR_RKEY;

    completions_reset();
    tcp_post_send(&backend_dev, &qp, IBV_QPT_RC, &sge, 1, 0, NULL, NULL, 0, 0,
                  &ctx);

    *rdma_write_bytes = dev->stats.total_bytes_rdma_write;

    g_queue_free_full(tqp.send_queue, g_free);
    g_queue_free_full(tqp.recv_queue, g_free);
    g_hash_table_destroy(priv.qps);
    qemu_mutex_destroy(&priv.lock);
    g_free(dev);
}

/*
 * The loopback shortcut in tcp_post_send() copies straight into the target
 * region when the destination node is this node, bypassing the wire. It
 * needs the same overflow-safe check as the receive handlers -- and, having
 * refused the copy, must say so.
 *
 * Rejecting the copy is only half the job. This path used to log the bounds
 * failure and then fall through to an unconditional statistics update and an
 * IBV_WC_SUCCESS completion, so a guest whose write was refused was told it
 * had succeeded and went on reading stale bytes as if they were its own.
 * Silent data loss is a poor trade for a blocked overflow, and the remote
 * path in the same function has always failed this request properly, so the
 * completion and the counter are asserted here alongside the guard bands.
 */
static int test_loopback_wrap(void)
{
    const char *name = "loopback-write-wrap";
    uint8_t source[WRAP_LEN];
    uint64_t bytes = 0;
    int fail = 0;

    /*
     * Source data is read from a valid in-bounds SGE; only the *remote*
     * address wraps. It is placed deep inside the region rather than at the
     * start so it cannot overlap the wrapped destination -- an overlapping
     * memcpy would abort under ASan with a param-overlap report, masking the
     * guard-band check that is the actual assertion here.
     */
    memset(source, ATTACK_BYTE, sizeof(source));
    memcpy((uint8_t *)test_mr.virt + SRC_OFFSET, source, sizeof(source));

    run_loopback_write(wrapped_addr(), &bytes);

    if (!guards_intact(name)) {
        fail = 1;
    }

    /* The caller must be told. This is the assertion the guard bands cannot
     * make: on a rejection memory is untouched either way. */
    if (last_completion.count != 1) {
        printf("FAIL %-22s: %u completions posted, expected exactly 1\n", name,
               last_completion.count);
        fail = 1;
    } else if (last_completion.status == IBV_WC_SUCCESS) {
        printf("FAIL %-22s: rejected write completed IBV_WC_SUCCESS -- the "
               "guest is told a write that copied nothing succeeded\n",
               name);
        fail = 1;
    } else if (last_completion.byte_len != 0) {
        printf("FAIL %-22s: rejected write reported %u bytes, expected 0\n",
               name, last_completion.byte_len);
        fail = 1;
    }

    /* And not billed for bytes that were never written. */
    if (bytes != 0) {
        printf("FAIL %-22s: rejected write added %" PRIu64 " bytes to the "
               "RDMA-write counter, expected 0\n",
               name, bytes);
        fail = 1;
    }

    if (!fail) {
        printf("PASS %-22s: wrapped loopback write rejected with an error "
               "completion, guards intact, stats unchanged\n",
               name);
    }
    return fail;
}

/*
 * The mirror of the case above: a legitimate loopback write must still copy,
 * still complete successfully, and still count. Without this, a "fix" that
 * failed every loopback write would pass the rejection test.
 */
static int test_loopback_ok(void)
{
    const char *name = "loopback-write-ok";
    uint8_t source[WRAP_LEN];
    uint64_t bytes = 0;
    int fail = 0;

    memset(source, ATTACK_BYTE, sizeof(source));
    memcpy((uint8_t *)test_mr.virt + SRC_OFFSET, source, sizeof(source));

    /* Destination at the region's base: in bounds, and clear of the source
     * at SRC_OFFSET so the copy does not overlap. */
    run_loopback_write(MR_START, &bytes);

    if (!guards_intact(name)) {
        fail = 1;
    }

    if (memcmp(test_mr.virt, source, WRAP_LEN) != 0) {
        printf("FAIL %-22s: in-bounds loopback write did not land\n", name);
        fail = 1;
    }

    if (last_completion.count != 1) {
        printf("FAIL %-22s: %u completions posted, expected exactly 1\n", name,
               last_completion.count);
        fail = 1;
    } else if (last_completion.status != IBV_WC_SUCCESS) {
        printf("FAIL %-22s: in-bounds write completed with status %d, "
               "expected IBV_WC_SUCCESS (%d)\n",
               name, last_completion.status, IBV_WC_SUCCESS);
        fail = 1;
    }

    if (bytes != WRAP_LEN) {
        printf("FAIL %-22s: in-bounds write added %" PRIu64 " bytes to the "
               "RDMA-write counter, expected %u\n",
               name, bytes, WRAP_LEN);
        fail = 1;
    }

    if (!fail) {
        printf("PASS %-22s: in-bounds loopback write landed, completed "
               "successfully, counted\n",
               name);
    }
    return fail;
}

int main(void)
{
    int failures = 0;

    fixture_init();

    failures += test_range_predicate();
    failures += test_high_start_region();
    failures += test_write_wrap();

    /* Each wire case rewrites the region; re-poison so the next case starts
     * from a known board. */
    memset(backing, GUARD_BYTE, BACKING_LEN);
    failures += test_read_wrap();

    memset(backing, GUARD_BYTE, BACKING_LEN);
    failures += test_sge_wrap();

    memset(backing, GUARD_BYTE, BACKING_LEN);
    failures += test_loopback_wrap();

    memset(backing, GUARD_BYTE, BACKING_LEN);
    failures += test_loopback_ok();

    fixture_destroy();

    if (failures) {
        printf("\n%d test case(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll test cases passed\n");
    return 0;
}
