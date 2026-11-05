#!/usr/bin/env python3
"""Drive the emulated ionic device as the guest driver does, over the socket.

The in-process harness (tests/ionic_harness.c) already covers the device
logic. What this adds is the rest of the path a VM uses: the vfio-user
protocol, the server's own main loop, and real DMA through pages the
server maps itself. A WQE here is fetched by vfu_sgl_get() from memory
this process wrote, not from a fake.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic
from ernic.check import Suite
from ernic.fixture import BaseFixture
from ernic.server import run_vfu_suite

# pylint: enable=wrong-import-position

# QP ids 0 and 1 are the device's own: a QP created as id 1 comes up as
# GSI, whose sends are routed by the destination in the WQE rather than
# by the peer a connected QP learned. Start above them.
QP_A = 10
QP_B = 11
CQ_A = 1
CQ_B = 2

# MR keys as the driver's ionic_mrid() builds them: the MR's index, from
# 1 up, above a key byte
MR_A = (1 << 8) | 0x01
MR_B = (2 << 8) | 0x02
VA_A = 0x0000_7F00_0000_0100
VA_B = 0x0000_7F10_0000_0000
MR_LEN = 3 * ionic.PAGE_SIZE

# A UD receive starts with a GRH, which its byte count includes
UD_GRH_LEN = 40


def send_a_to_b(fix, length):
    """Post a signalled RC SEND of @length bytes from A's region.

    The three posting idioms below are shared with
    tests/py/test_ionic_hw_stats.py, which drives the same transfers to
    watch the counters move.
    """
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(VA_A, length, MR_A)],
    )


def rdma_a_to_b(fix, op, length):
    """Post a signalled RDMA @op of @length bytes between A's and B's regions.

    WRITE and READ differ only in the opcode: both name B's region by
    rkey and gather from, or scatter into, A's.
    """
    fix.driver.post_send(
        fix.qp_a,
        op,
        flags=ionic.WQE_SIGNALLED,
        remote_va=VA_B,
        rkey=MR_B,
        sge=[ionic.Sge(VA_A, length, MR_A)],
    )


def no_completion(driver, cq, settle=1.0):
    """Whether @cq stays empty, after giving the device time to fill it.

    A completion that should not exist cannot be waited for, so this
    waits out a settle period instead and reports what it found.
    """
    deadline = time.monotonic() + settle
    while time.monotonic() < deadline:
        if driver.poll_cq(cq) is not None:
            return False
        time.sleep(0.01)
    return True


# The pieces a test reaches for; bundling them is the point.
# pylint: disable-next=too-few-public-methods
class Fixture(BaseFixture):
    """Two connected RC queue pairs in one instance, over the loopback backend."""

    def __init__(self, server):
        super().__init__(server)
        self.cq_a = self.driver.create_cq(CQ_A)
        self.cq_b = self.driver.create_cq(CQ_B)
        self.qp_a = self.driver.create_qp(QP_A, ionic.QPT_RC, self.cq_a, self.cq_a)
        self.qp_b = self.driver.create_qp(QP_B, ionic.QPT_RC, self.cq_b, self.cq_b)
        hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
        self.driver.connect_qp(self.qp_a, QP_B, hdr)
        self.driver.connect_qp(self.qp_b, QP_A, hdr)
        # Reversed pages, so no two of A's pages are physically adjacent
        # and a gather that ignores the page table shows up as wrong data.
        self.mr_a = self.driver.create_mr(MR_A, VA_A, MR_LEN, reverse=True)
        self.mr_b = self.driver.create_mr(MR_B, VA_B, MR_LEN)


def test_bringup(suite, fix):
    """The device came up, and the admin path answers."""
    suite.check(fix.driver.eq is not None, "the RDMA event queue was created")
    status, _ = fix.driver.admin(ionic.ADMIN_NOOP)
    suite.check_eq(status, 0, "an admin NOOP completes")
    # struct ionic_admin_query_qp is 34 bytes, with the qp id in id_ver
    # at offset 28. Naming one nothing created must be refused.
    body = bytearray(34)
    body[28:32] = (0x00FF_0000 | 1 << 24).to_bytes(4, "little")
    status, _ = fix.driver.admin(ionic.ADMIN_QUERY_QP, bytes(body))
    suite.check(status != 0, "QUERY_QP of a QP that does not exist is refused")


def test_send(suite, fix):
    """An RC SEND, gathered from an MR whose pages are out of order."""
    payload = bytes(range(256)) * 4
    fix.driver.mr_write(fix.mr_a, VA_A, payload)
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    send_a_to_b(fix, len(payload))
    recv = fix.driver.wait_cq(fix.cq_b)
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check_eq(recv.type, ionic.CQE_TYPE_RECV, "B gets a receive completion")
    suite.check_eq(recv.qid, QP_B, "the receive completion names B")
    suite.check(not recv.error, "the receive completed without error")
    suite.check_eq(recv.status_len, len(payload), "the received byte count")
    suite.check_eq(send.type, ionic.CQE_TYPE_SEND_MSN, "A gets a send completion")
    suite.check(not send.error, "the send completed without error")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)), payload, "the received bytes"
    )


def test_send_multi_sge(suite, fix):
    """A SEND gathered from several SGEs, and scattered into several."""
    payload = bytes((i * 7) & 0xFF for i in range(600))
    fix.driver.mr_write(fix.mr_a, VA_A, payload)
    parts = [200, 200, 200]
    offset = 0
    recv_sges = []
    for part in parts:
        recv_sges.append(ionic.Sge(VA_B + offset, part, MR_B))
        offset += part
    fix.driver.post_recv(fix.qp_b, recv_sges)

    send_sges = []
    offset = 0
    for part in parts:
        send_sges.append(ionic.Sge(VA_A + offset, part, MR_A))
        offset += part
    fix.driver.post_send(
        fix.qp_a, ionic.OP_SEND, flags=ionic.WQE_SIGNALLED, sge=send_sges
    )
    recv = fix.driver.wait_cq(fix.cq_b)
    fix.driver.wait_cq(fix.cq_a)
    suite.check_eq(recv.status_len, len(payload), "all three SGEs were delivered")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
        payload,
        "the scattered bytes, reassembled",
    )


def test_send_inline(suite, fix):
    """A SEND carrying its payload in the WQE itself."""
    payload = b"inline payload, no SGE in sight"
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED | ionic.WQE_INLINE,
        inline_data=payload,
    )
    recv = fix.driver.wait_cq(fix.cq_b)
    fix.driver.wait_cq(fix.cq_a)
    suite.check_eq(recv.status_len, len(payload), "the inline byte count")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)), payload, "the inline bytes"
    )


def test_send_imm(suite, fix):
    """SEND_IMM, whose immediate data reaches the receive completion."""
    payload = b"with immediate"
    imm = b"\xde\xad\xbe\xef"
    fix.driver.mr_write(fix.mr_a, VA_A, payload)
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_SEND_IMM,
        flags=ionic.WQE_SIGNALLED,
        imm_be=imm,
        sge=[ionic.Sge(VA_A, len(payload), MR_A)],
    )
    recv = fix.driver.wait_cq(fix.cq_b)
    fix.driver.wait_cq(fix.cq_a)
    suite.check_eq(recv.recv_op, ionic.RECV_OP_SEND_IMM, "the receive opcode")
    suite.check_eq(recv.imm_be, imm, "the immediate data")


def test_rdma_write(suite, fix):
    """An RDMA WRITE into the peer's memory, with no receive posted."""
    payload = b"written straight in, no receive" * 8
    fix.driver.mr_write(fix.mr_a, VA_A, payload)
    fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * len(payload))
    rdma_a_to_b(fix, ionic.OP_RDMA_WRITE, len(payload))
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(not send.error, "the write completed without error")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
        payload,
        "the bytes landed at the remote address",
    )


def test_rdma_read(suite, fix):
    """An RDMA READ out of the peer's memory."""
    payload = b"read me back" * 16
    fix.driver.mr_write(fix.mr_b, VA_B, payload)
    fix.driver.mr_write(fix.mr_a, VA_A, b"\0" * len(payload))
    rdma_a_to_b(fix, ionic.OP_RDMA_READ, len(payload))
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(not send.error, "the read completed without error")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_a, VA_A, len(payload)),
        payload,
        "the bytes arrived from the remote address",
    )


def test_bad_rkey(suite, fix):
    """A WRITE to a key nothing registered must fail, not land somewhere."""
    before = fix.driver.mr_read(fix.mr_b, VA_B, 64)
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_RDMA_WRITE,
        flags=ionic.WQE_SIGNALLED,
        remote_va=VA_B,
        rkey=0xDEAD_BEEF,
        sge=[ionic.Sge(VA_A, 64, MR_A)],
    )
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(send.error, "a write with an unregistered rkey fails")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, 64),
        before,
        "the peer's memory is unchanged",
    )


def test_write_past_mr(suite, fix):
    """A WRITE whose length runs past the end of the region it names.

    The local side supplies all 4096 bytes, so the only thing wrong with
    this work request is that the destination runs off the end of the
    peer's region. With a short local SGE the transfer would come up
    short anyway and the test would pass whether or not the remote bound
    was enforced at all.
    """
    local_len = 4096
    remote_va = VA_B + MR_LEN - 64
    tail = fix.driver.mr_read(fix.mr_b, remote_va, 64)
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_RDMA_WRITE,
        flags=ionic.WQE_SIGNALLED,
        remote_va=remote_va,
        rkey=MR_B,
        length=local_len,
        sge=[ionic.Sge(VA_A, local_len, MR_A)],
    )
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(send.error, "a write running past the end of the region fails")
    suite.check_eq(
        send.status,
        ionic.STS_REMOTE_ACC_ERR,
        "the end that was out of bounds is the one blamed",
    )
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, remote_va, 64),
        tail,
        "the bytes that would have fitted were not written either",
    )


def test_bad_lkey(suite, fix):
    """A SEND gathering from a key nothing registered.

    It is a local protection error: the sender's own memory refused the
    gather, so the sender's send completion carries the failure, nothing
    is delivered, and the peer's receive stays posted.
    """
    marker = b"\xa5" * 64
    fix.driver.mr_write(fix.mr_b, VA_B, marker)
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(VA_A, 64, 0xBAD0_0000)],
    )
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(send.error, "the sender's completion carries the error")
    suite.check_eq(
        send.status, ionic.STS_LOCAL_PROT_ERR, "the status is a local protection error"
    )
    suite.check(
        no_completion(fix.driver, fix.cq_b),
        "nothing was delivered to the peer",
    )
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, 64),
        marker,
        "the receive buffer is untouched",
    )


def test_bad_lkey_length(suite, fix):
    """A SEND whose SGE runs past the end of the region it names."""
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        fix.qp_a,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(VA_A + MR_LEN - 64, 4096, MR_A)],
    )
    send = fix.driver.wait_cq(fix.cq_a)
    suite.check(send.error, "the sender's completion carries the error")
    suite.check_eq(
        send.status, ionic.STS_LOCAL_PROT_ERR, "the status is a local protection error"
    )
    suite.check(
        no_completion(fix.driver, fix.cq_b), "nothing was delivered to the peer"
    )


def test_srq(suite, fix):
    """A QP whose receives come from a shared receive queue."""
    srq = fix.driver.create_srq()
    cq_c = fix.driver.create_cq(3)
    qp_c = fix.driver.create_qp(20, ionic.QPT_RC, cq_c, cq_c, srq=srq)
    qp_d = fix.driver.create_qp(21, ionic.QPT_RC, cq_c, cq_c)
    hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
    fix.driver.connect_qp(qp_c, qp_d.id, hdr)
    fix.driver.connect_qp(qp_d, qp_c.id, hdr)
    suite.check(srq.id >= ionic.IONIC_SRQ_QID_BASE, "the SRQ id is above the QP ids")

    payload = b"through a shared receive queue"
    fix.driver.mr_write(fix.mr_a, VA_A, payload)
    fix.driver.post_srq_recv(srq, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        qp_d,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(VA_A, len(payload), MR_A)],
    )
    first = fix.driver.wait_cq(cq_c)
    second = fix.driver.wait_cq(cq_c)
    recv = first if first.type == ionic.CQE_TYPE_RECV else second
    suite.check_eq(recv.type, ionic.CQE_TYPE_RECV, "the SRQ-bound QP got a receive")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
        payload,
        "the bytes delivered through the SRQ",
    )
    suite.check_eq(fix.driver.destroy_srq(srq), 0, "DESTROY_SRQ")


def test_send_cmb(suite, fix):
    """SENDs between QPs whose rings are in the CMB.

    The guest driver puts a QP's rings there once the device offers a
    CMB, and names each by its offset in BAR4. C's send ring is created
    first, and D's below it: a ring is where its own offset says, not
    relative to whichever ring came first.
    """
    cq_e = fix.driver.create_cq(4)
    qp_c = fix.driver.create_qp(30, ionic.QPT_RC, cq_e, cq_e, sq_cmb=ionic.PAGE_SIZE)
    qp_d = fix.driver.create_qp(
        31, ionic.QPT_RC, cq_e, cq_e, sq_cmb=0, rq_cmb=2 * ionic.PAGE_SIZE
    )
    hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
    fix.driver.connect_qp(qp_c, qp_d.id, hdr)
    fix.driver.connect_qp(qp_d, qp_c.id, hdr)

    for sender, receiver in ((qp_c, qp_d), (qp_d, qp_c)):
        name = f"QP {sender.id} to QP {receiver.id}"
        payload = f"from a CMB send ring, {name}".encode()
        fix.driver.mr_write(fix.mr_a, VA_A, payload)
        fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * len(payload))
        fix.driver.post_recv(receiver, [ionic.Sge(VA_B, MR_LEN, MR_B)])
        fix.driver.post_send(
            sender,
            ionic.OP_SEND,
            flags=ionic.WQE_SIGNALLED,
            sge=[ionic.Sge(VA_A, len(payload), MR_A)],
        )
        cqes = [fix.driver.wait_cq(cq_e), fix.driver.wait_cq(cq_e)]
        send = [c for c in cqes if c.type == ionic.CQE_TYPE_SEND_MSN]
        recv = [c for c in cqes if c.type == ionic.CQE_TYPE_RECV]
        suite.check(
            len(send) == 1 and not send[0].error,
            f"{name}: the send completed without error",
        )
        suite.check(
            len(recv) == 1 and recv[0].qid == receiver.id and not recv[0].error,
            f"{name}: the receiver got a receive completion",
        )
        suite.check_eq(
            fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
            payload,
            f"{name}: the received bytes",
        )

    suite.check_eq(fix.driver.destroy_qp(qp_c), 0, "DESTROY_QP C")
    suite.check_eq(fix.driver.destroy_qp(qp_d), 0, "DESTROY_QP D")
    suite.check_eq(fix.driver.destroy_cq(cq_e), 0, "DESTROY_CQ E")


def test_inline_cmb(suite, fix):
    """Inline payloads from a send ring in the CMB.

    The payload is in the WQE, which is in BAR4 rather than any guest
    page, so it has to come from the copy the device read. Taken from
    the guest address of a ring in guest memory, it came from guest
    physical address 32, and the peer received the guest's low memory.
    A RC SEND, a RC WRITE and a UD SEND each take the payload a
    different way.
    """
    hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
    cq_g = fix.driver.create_cq(6)
    rc_c = fix.driver.create_qp(50, ionic.QPT_RC, cq_g, cq_g, sq_cmb=0)
    rc_d = fix.driver.create_qp(51, ionic.QPT_RC, cq_g, cq_g)
    fix.driver.connect_qp(rc_c, rc_d.id, hdr)
    fix.driver.connect_qp(rc_d, rc_c.id, hdr)

    payload = b"inline, from a send ring in the CMB"
    fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * MR_LEN)
    fix.driver.post_recv(rc_d, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        rc_c,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED | ionic.WQE_INLINE,
        inline_data=payload,
    )
    cqes = [fix.driver.wait_cq(cq_g), fix.driver.wait_cq(cq_g)]
    suite.check(
        all(not c.error for c in cqes), "RC SEND: both ends completed without error"
    )
    recv = [c for c in cqes if c.type == ionic.CQE_TYPE_RECV]
    suite.check(
        len(recv) == 1 and recv[0].status_len == len(payload),
        "RC SEND: the received byte count",
    )
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
        payload,
        "RC SEND: the received bytes",
    )

    payload = b"written inline from the CMB"
    fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * MR_LEN)
    fix.driver.post_send(
        rc_c,
        ionic.OP_RDMA_WRITE,
        flags=ionic.WQE_SIGNALLED | ionic.WQE_INLINE,
        remote_va=VA_B,
        rkey=MR_B,
        inline_data=payload,
    )
    send = fix.driver.wait_cq(cq_g)
    suite.check(not send.error, "RC WRITE: completed without error")
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
        payload,
        "RC WRITE: the bytes at the remote address",
    )

    # A READ has no payload to send, and would scatter its response into
    # the copy of the WQE, so one marked inline is refused
    before = fix.driver.mr_read(fix.mr_b, VA_B, MR_LEN)
    fix.driver.post_send(
        rc_c,
        ionic.OP_RDMA_READ,
        flags=ionic.WQE_SIGNALLED | ionic.WQE_INLINE,
        remote_va=VA_B,
        rkey=MR_B,
        inline_data=b"\0" * 16,
    )
    send = fix.driver.wait_cq(cq_g)
    suite.check(
        send.error and send.status == ionic.STS_LOCAL_QP_OPER_ERR,
        "RC READ marked inline: refused as a local QP operation error",
    )
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B, MR_LEN),
        before,
        "RC READ marked inline: the remote region is unchanged",
    )

    ud_c = fix.driver.create_qp(52, ionic.QPT_UD, cq_g, cq_g, sq_cmb=ionic.PAGE_SIZE)
    ud_d = fix.driver.create_qp(53, ionic.QPT_UD, cq_g, cq_g)
    for qp in (ud_c, ud_d):
        for state in (ionic.QPS_INIT, ionic.QPS_RTR, ionic.QPS_RTS):
            suite.check_eq(
                fix.driver.modify_qp(qp.id, ionic.QP_STATE, state),
                0,
                f"UD QP {qp.id} to state {state}",
            )

    payload = b"a datagram, inline from the CMB"
    fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * MR_LEN)
    fix.driver.post_recv(ud_d, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    fix.driver.post_send(
        ud_c,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED | ionic.WQE_INLINE,
        dest_qpn=ud_d.id,
        inline_data=payload,
    )
    cqes = [fix.driver.wait_cq(cq_g), fix.driver.wait_cq(cq_g)]
    recv = [c for c in cqes if c.type == ionic.CQE_TYPE_RECV]
    suite.check(
        len(recv) == 1
        and not recv[0].error
        and recv[0].status_len == UD_GRH_LEN + len(payload),
        "UD SEND: a receive of the GRH and the payload",
    )
    suite.check_eq(
        fix.driver.mr_read(fix.mr_b, VA_B + UD_GRH_LEN, len(payload)),
        payload,
        "UD SEND: the received bytes, after the GRH",
    )

    for qp in (rc_c, rc_d, ud_c, ud_d):
        suite.check_eq(fix.driver.destroy_qp(qp), 0, f"DESTROY_QP {qp.id}")
    suite.check_eq(fix.driver.destroy_cq(cq_g), 0, "DESTROY_CQ G")


def test_cmb_id_reused(suite, fix):
    """A QP in guest memory, under the id a QP in the CMB had.

    Its sends must come from its own ring. The old QP's ring in BAR4
    still holds the SEND it posted, and fetched from there the new QP
    would send that instead, at the old length.
    """
    cq_f = fix.driver.create_cq(5)
    peer = fix.driver.create_qp(41, ionic.QPT_RC, cq_f, cq_f)
    hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
    fix.driver.connect_qp(peer, 40, hdr)

    for sq_cmb, payload in (
        (3 * ionic.PAGE_SIZE, b"from the QP whose send ring is in the CMB"),
        (None, b"from its successor, in guest memory"),
    ):
        where = "in the CMB" if sq_cmb is not None else "in guest memory"
        qp = fix.driver.create_qp(40, ionic.QPT_RC, cq_f, cq_f, sq_cmb=sq_cmb)
        fix.driver.connect_qp(qp, peer.id, hdr)
        fix.driver.mr_write(fix.mr_a, VA_A, payload)
        fix.driver.mr_write(fix.mr_b, VA_B, b"\0" * MR_LEN)
        fix.driver.post_recv(peer, [ionic.Sge(VA_B, MR_LEN, MR_B)])
        fix.driver.post_send(
            qp,
            ionic.OP_SEND,
            flags=ionic.WQE_SIGNALLED,
            sge=[ionic.Sge(VA_A, len(payload), MR_A)],
        )
        cqes = [fix.driver.wait_cq(cq_f), fix.driver.wait_cq(cq_f)]
        recv = [c for c in cqes if c.type == ionic.CQE_TYPE_RECV]
        suite.check(
            len(recv) == 1 and not recv[0].error,
            f"send ring {where}: the peer got a receive completion",
        )
        suite.check(
            len(recv) == 1 and recv[0].status_len == len(payload),
            f"send ring {where}: the received byte count",
        )
        suite.check_eq(
            fix.driver.mr_read(fix.mr_b, VA_B, len(payload)),
            payload,
            f"send ring {where}: the received bytes",
        )
        suite.check_eq(fix.driver.destroy_qp(qp), 0, f"DESTROY_QP {where}")

    suite.check_eq(fix.driver.destroy_qp(peer), 0, "DESTROY_QP peer")
    suite.check_eq(fix.driver.destroy_cq(cq_f), 0, "DESTROY_CQ F")


def test_teardown(suite, fix):
    """Every resource comes back."""
    suite.check_eq(fix.driver.destroy_qp(fix.qp_a), 0, "DESTROY_QP A")
    suite.check_eq(fix.driver.destroy_qp(fix.qp_b), 0, "DESTROY_QP B")
    suite.check_eq(fix.driver.destroy_mr(fix.mr_a), 0, "DESTROY_MR A")
    suite.check_eq(fix.driver.destroy_mr(fix.mr_b), 0, "DESTROY_MR B")
    suite.check_eq(fix.driver.destroy_cq(fix.cq_a), 0, "DESTROY_CQ A")
    suite.check_eq(fix.driver.destroy_cq(fix.cq_b), 0, "DESTROY_CQ B")


TESTS = (
    ("device bring-up", test_bringup),
    ("RC SEND", test_send),
    ("SEND from several SGEs", test_send_multi_sge),
    ("SEND with an inline payload", test_send_inline),
    ("SEND with immediate data", test_send_imm),
    ("RDMA WRITE", test_rdma_write),
    ("RDMA READ", test_rdma_read),
    ("WRITE with an unregistered rkey", test_bad_rkey),
    ("WRITE past the end of a region", test_write_past_mr),
    ("SEND from an unregistered lkey", test_bad_lkey),
    ("SEND from an SGE past the end of its region", test_bad_lkey_length),
    ("shared receive queue", test_srq),
    ("SEND between QPs with rings in the CMB", test_send_cmb),
    ("inline payloads from a send ring in the CMB", test_inline_cmb),
    ("a QP id reused outside the CMB", test_cmb_id_reused),
    ("teardown", test_teardown),
)


def main():
    """Start a loopback server and run the driver tests against it."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    def make_fixture(server, _run_dir):
        return Fixture(server)

    suite = Suite("rocm-ernic ionic driver tests over vfio-user")
    run_vfu_suite(suite, args.server, TESTS, make_fixture)


if __name__ == "__main__":
    main()
