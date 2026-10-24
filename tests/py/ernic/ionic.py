"""The ionic driver, as a guest kernel would run it, over vfio-user.

This is the Python counterpart of tests/ionic_harness.c. That harness
drives the device code in-process over a fake libvfio-user; this one
drives a real server process over its socket, so the vfio-user protocol,
the DMA path, and the server's own main loop are all in the picture.

The layouts below are written out from the driver's side (ionic_if.h and
ionic_fw.h), as the C harness does, rather than shared with the device
code, so a wrong offset in one doesn't cancel a wrong offset in the
other.

The one structural difference from the C harness: there, ih_poll() runs
a pass of the server's loop, so a completion is ready the moment the
doorbell returns. Here the server polls on its own thread of control, so
everything that waits for the device waits with a deadline.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import struct
import time

from .errors import DeviceError
from .eth import CMD_Q_INIT, EthQueueMixin
from .vfu import VFU_PCI_DEV_BAR0_REGION_IDX, VFU_PCI_DEV_BAR2_REGION_IDX

# BAR0 register map (union ionic_dev_regs)
BAR0_DEVCMD = 0x0800
BAR0_DEVCMD_DONE = BAR0_DEVCMD + 0x04
BAR0_DEVCMD_CMD = BAR0_DEVCMD + 0x08
BAR0_DEVCMD_COMP = BAR0_DEVCMD + 0x48
BAR0_DEVCMD_DATA = BAR0_DEVCMD + 0x88
BAR0_INTR_CTRL = 0x2000
BAR0_INTR_STRIDE = 32
BAR0_INTR_MASK = 4

IONIC_DEV_SIG = 0x44455649  # "DEVI"
DEVCMD_DATA_BYTES = 1912
# union ionic_lif_identity: le32 eth.max_frame_size
LIFID_MAX_FRAME_SIZE_OFF = 0x01A
# BAR2 doorbell qtypes: the Ethernet logical queues, then the RDMA
# hardware qtypes the LIF identity advertises.
QTYPE_ETH_ADMINQ = 0
QTYPE_AQ = 5
QTYPE_SQ = 6
QTYPE_RQ = 7
QTYPE_CQ = 8

# Device commands (enum ionic_cmd_opcode)
CMD_NOP = 0
CMD_IDENTIFY = 1
CMD_RESET = 3
CMD_PORT_IDENTIFY = 10
CMD_PORT_INIT = 11
CMD_LIF_IDENTIFY = 20
CMD_LIF_INIT = 21
CMD_RDMA_RESET = 50
CMD_RDMA_EQ = 51
CMD_RDMA_CQ = 52
CMD_RDMA_ADMINQ = 53

# RDMA admin opcodes (enum ionic_v1_admin_op)
ADMIN_NOOP = 0
ADMIN_CREATE_CQ = 1
ADMIN_CREATE_QP = 2
ADMIN_CREATE_MR = 3
ADMIN_STATS_HDRS = 4
ADMIN_STATS_VALS = 5
ADMIN_DESTROY_MR = 6
ADMIN_DESTROY_CQ = 8
ADMIN_MODIFY_QP = 9
ADMIN_QUERY_QP = 10
ADMIN_DESTROY_QP = 11
ADMIN_CREATE_AH = 13
ADMIN_DESTROY_AH = 16
ADMIN_QP_STATS_HDRS = 17
ADMIN_QP_STATS_VALS = 18
ADMIN_CREATE_SRQ = 26
ADMIN_DESTROY_SRQ = 29

# enum ionic_qp_type
QPT_RC = 0
QPT_UC = 1
QPT_UD = 3

# IB QP states and the MODIFY_QP attribute mask bits
QPS_RESET = 0
QPS_INIT = 1
QPS_RTR = 2
QPS_RTS = 3

QP_STATE = 1 << 0
QP_QKEY = 1 << 6
QP_AV = 1 << 7
QP_RQ_PSN = 1 << 12
QP_SQ_PSN = 1 << 16
QP_DEST_QPN = 1 << 20

# IB access flags, the low bits of CREATE_MR's flags
ACCESS_LOCAL_WRITE = 1 << 0
ACCESS_REMOTE_WRITE = 1 << 1
ACCESS_REMOTE_READ = 1 << 2
ACCESS_REMOTE_ATOMIC = 1 << 3
ACCESS_ALL = (
    ACCESS_LOCAL_WRITE | ACCESS_REMOTE_WRITE | ACCESS_REMOTE_READ | ACCESS_REMOTE_ATOMIC
)

# enum ionic_v1_op
OP_SEND = 0
OP_SEND_INV = 1
OP_SEND_IMM = 2
OP_RDMA_READ = 3
OP_RDMA_WRITE = 4
OP_RDMA_WRITE_IMM = 5
OP_ATOMIC_CS = 6
OP_ATOMIC_FA = 7
OP_REG_MR = 8
OP_LOCAL_INV = 9

# enum ionic_v1_flag
WQE_INLINE = 0x0004
WQE_SIGNALLED = 0x0008

# ionic_v1_cqe types
CQE_TYPE_ADMIN = 0
CQE_TYPE_RECV = 1
CQE_TYPE_SEND_MSN = 2
CQE_TYPE_SEND_NPG = 3

# enum ionic_status, as a completion's status_length carries it
STS_OK = 0
STS_LOCAL_LEN_ERR = 1
STS_LOCAL_QP_OPER_ERR = 2
STS_LOCAL_PROT_ERR = 3
STS_LOCAL_ACC_ERR = 7
STS_REMOTE_ACC_ERR = 9

RECV_OP_SEND = 0
RECV_OP_SEND_INV = 1
RECV_OP_SEND_IMM = 2
RECV_OP_RDMA_IMM = 3

# Ring geometry, matching the C harness
ETH_AQ_DEPTH_LOG2 = 4
EQ_DEPTH_LOG2 = 8
AQ_DEPTH_LOG2 = 6
ACQ_DEPTH_LOG2 = 6
QP_DEPTH_LOG2 = 4
WQE_STRIDE_LOG2 = 7  # 128 bytes: six SGEs, or 96 bytes inline
CQE_STRIDE_LOG2 = 5

ADMIN_STRIDE = 64
ADMIN_HDR_LEN = 4
WQE_PLD_OFF = 32

PAGE_SIZE = 4096
GRH_SIZE = 40

# SRQ ids start above the QP id space, and have to: a qtype-7 doorbell
# carries either a QP id or an SRQ id and the device tells them apart by
# the number alone (IONIC_SRQ_QID_BASE in src/rocm_ernic_compat.h).
IONIC_EMU_QP_COUNT_SHARED = 1 << 15
IONIC_SRQ_QID_BASE = IONIC_EMU_QP_COUNT_SHARED

ROCE_UDP_PORT = 4791

# MSI-X vectors the driver gives its queues, the EQ it reports on, and
# the admin CQ's id.
ETH_ADMINQ_INTR = 0
EQ_INTR = 1
EQ_ID = 0
ACQ_ID = 0

# How long the device gets to answer, by default. Generous: a sanitizer
# or valgrind build is slow, and a mesh hop crosses two processes.
DEFAULT_TIMEOUT = 10.0


class Ring:
    """A ring in guest memory, and where the driver is in it."""

    # A record with two derived values; the attributes are the interface.
    # pylint: disable=too-few-public-methods
    def __init__(self, gpa, depth_log2, stride_log2):
        self.gpa = gpa
        self.depth_log2 = depth_log2
        self.stride_log2 = stride_log2
        self.prod = 0
        self.cons = 0

    @property
    def depth(self):
        """Entries in the ring."""
        return 1 << self.depth_log2

    def slot(self, index):
        """The guest physical address of entry @index."""
        return self.gpa + ((index % self.depth) << self.stride_log2)

    def color(self):
        """The colour an entry at the consumer index needs to be new."""
        return (self.cons // self.depth) % 2 == 0


class Cq:
    """A completion queue."""

    # pylint: disable=too-few-public-methods
    def __init__(self, cq_id, ring):
        self.id = cq_id
        self.ring = ring


class Srq:
    """A shared receive queue."""

    # pylint: disable=too-few-public-methods
    def __init__(self, srq_id, ring):
        self.id = srq_id
        self.ring = ring
        self.next_wqe_id = 1


class Qp:
    """A queue pair and its rings."""

    # pylint: disable=too-few-public-methods
    def __init__(self, qp_id, qp_type, sq, rq):
        self.id = qp_id
        self.type = qp_type
        self.sq = sq
        self.rq = rq
        self.next_wqe_id = 1


class Mr:
    """A memory region: a virtual range backed by guest pages."""

    # pylint: disable=too-few-public-methods
    def __init__(self, lkey, va, length, pages, pt_gpa):
        self.lkey = lkey
        self.va = va
        self.len = length
        self.pages = pages
        self.pt_gpa = pt_gpa

    def dma_addr(self):
        """What the driver puts in a dma_addr field.

        The page table, or for a region that fits one page, the page
        itself with the offset folded in, as ionic_pgtbl_dma() does.
        """
        if len(self.pages) == 1:
            return self.pages[0] + self.va % PAGE_SIZE
        return self.pt_gpa


class Sge:
    """One scatter/gather entry."""

    # pylint: disable=too-few-public-methods
    def __init__(self, va, length, lkey):
        self.va = va
        self.len = length
        self.lkey = lkey


class Cqe:
    """A completion, decoded."""

    # A decoded wire structure; every field is part of the interface.
    # pylint: disable=too-few-public-methods,too-many-instance-attributes
    def __init__(self, raw):
        self.raw = raw
        qtf = struct.unpack_from(">I", raw, 28)[0]
        self.type = (qtf >> 5) & 7
        self.error = (qtf & 2) != 0
        self.qid = qtf >> 8
        self.status_len = struct.unpack_from(">I", raw, 24)[0]
        # One field doing two jobs: a byte count, or the status when the
        # error bit is set. The whole word carries the status here, unlike
        # an admin completion, where it sits in bits[31:24]; the two paths
        # differ and tests/test_ionic_harness.c encodes the same reading.
        self.status = self.status_len if self.error else 0
        self.wqe_id = 0
        self.msn = 0
        self.recv_op = 0
        self.src_qpn = 0
        self.imm_be = b"\0\0\0\0"
        self.has_smac = False
        self.smac = b"\0" * 6
        if self.type == CQE_TYPE_RECV:
            qpn_op = struct.unpack_from(">I", raw, 8)[0]
            self.wqe_id = struct.unpack_from("<Q", raw, 0)[0]
            self.recv_op = (qpn_op >> 24) & 0x7F
            self.src_qpn = qpn_op & 0xFFFFFF
            self.has_smac = (qpn_op & 0x80000000) != 0
            self.smac = raw[12:18]
            self.imm_be = raw[20:24]
        elif self.type == CQE_TYPE_SEND_MSN:
            self.msn = struct.unpack_from(">I", raw, 4)[0]
        elif self.type == CQE_TYPE_SEND_NPG:
            self.wqe_id = struct.unpack_from("<Q", raw, 16)[0]

    def __repr__(self):
        length = 0 if self.error else self.status_len
        return (
            f"Cqe(type={self.type}, qid={self.qid}, error={self.error}, "
            f"status={self.status}, len={length})"
        )


def roce_hdr_v4(sip, dip, smac=None, dmac=None):
    """The header template ib_ud_header_pack() leaves for an IPv4 peer.

    Ethernet, IPv4, UDP. The device reads the peer's address out of it;
    adminq_parse_roce_hdr() turns the addresses into ::ffff:a.b.c.d GIDs,
    which is the form a GID in ERNIC_TCP_GUEST_GIDS is stored in.
    """
    dmac = dmac or b"\x02\x00\x00\xaa\xbb\xcc"
    smac = smac or b"\x02\x00\x00\x11\x22\x33"
    hdr = bytearray(14 + 20 + 8)
    hdr[0:6] = dmac
    hdr[6:12] = smac
    struct.pack_into(">H", hdr, 12, 0x0800)
    ip = 14
    hdr[ip] = 0x45
    struct.pack_into(">H", hdr, ip + 2, 28)
    hdr[ip + 8] = 64
    hdr[ip + 9] = 17  # UDP
    struct.pack_into(">I", hdr, ip + 12, sip)
    struct.pack_into(">I", hdr, ip + 16, dip)
    struct.pack_into(">H", hdr, 34 + 2, ROCE_UDP_PORT)
    struct.pack_into(">H", hdr, 34 + 4, 8)
    return bytes(hdr)


def ipv4(text):
    """An IPv4 address as the 32-bit number a RoCE header carries."""
    parts = [int(p) for p in text.split(".")]
    if len(parts) != 4 or any(p < 0 or p > 255 for p in parts):
        raise ValueError(f"not an IPv4 address: {text}")
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]


def gid_v4(text):
    """The ::ffff:a.b.c.d GID an IPv4 RoCE header resolves to."""
    return f"::ffff:{text}"


class IonicDriver(EthQueueMixin):
    """The guest driver for one device.

    Construct with a connected VfioUserClient and the GuestMemory that
    has been DMA-mapped to it, then call bringup().
    """

    # The driver owns every queue the device has; splitting it up would
    # only move the state somewhere less obvious.
    # pylint: disable=too-many-instance-attributes,too-many-public-methods

    def __init__(self, client, mem, timeout=DEFAULT_TIMEOUT):
        self.client = client
        self.mem = mem
        self.timeout = timeout
        self.eth_aq = None
        self.eth_acq = None
        self.eth_tx = None
        self.eth_rx = None
        self.eq = None
        self.aq = None
        self.acq = None
        self.port_info = mem.alloc(PAGE_SIZE)
        self.lif_info = mem.alloc(PAGE_SIZE)
        self.scratch = mem.alloc(PAGE_SIZE)

    # ---- registers and doorbells ---------------------------------------

    def bar0_read(self, offset, length):
        """Read from BAR0."""
        return self.client.read_region(VFU_PCI_DEV_BAR0_REGION_IDX, offset, length)

    def bar0_write(self, offset, data):
        """Write to BAR0."""
        self.client.write_region(VFU_PCI_DEV_BAR0_REGION_IDX, offset, data)

    def bar0_read32(self, offset):
        """Read one 32-bit register from BAR0."""
        return struct.unpack("<I", self.bar0_read(offset, 4))[0]

    def bar0_write32(self, offset, value):
        """Write one 32-bit register to BAR0."""
        self.bar0_write(offset, struct.pack("<I", value))

    def intr_mask(self, vector, masked):
        """Mask or unmask one MSI-X vector through its intr_ctrl registers."""
        self.bar0_write32(
            BAR0_INTR_CTRL + vector * BAR0_INTR_STRIDE + BAR0_INTR_MASK,
            1 if masked else 0,
        )

    def doorbell(self, qtype, qid, ring, p_index):
        """Ring a doorbell.

        struct ionic_doorbell: le16 p_index, u8 ring, u8 qid_lo, le16
        qid_hi. The kernel's doorbell page is page 0, one slot per qtype.
        """
        value = (
            (p_index & 0xFFFF)
            | (ring & 0xFF) << 16
            | (qid & 0xFF) << 24
            | ((qid >> 8) & 0xFFFF) << 32
        )
        self.client.write_region(
            VFU_PCI_DEV_BAR2_REGION_IDX, qtype * 8, struct.pack("<Q", value)
        )

    # ---- guest memory ---------------------------------------------------

    def alloc_ring(self, depth_log2, stride_log2):
        """Allocate a ring in guest memory."""
        gpa = self.mem.alloc(1 << (depth_log2 + stride_log2))
        return Ring(gpa, depth_log2, stride_log2)

    # ---- commands -------------------------------------------------------

    def devcmd(self, cmd, data_len=0):
        """Run one devcmd through the BAR0 window.

        Returns (status, data). Raises DeviceError if the device never
        sets done.
        """
        if len(cmd) != 64:
            raise ValueError("a devcmd is 64 bytes")
        if data_len > DEVCMD_DATA_BYTES:
            raise ValueError(f"the devcmd data window is {DEVCMD_DATA_BYTES} bytes")
        # ionic_dev_cmd_go(): command, clear done, ring.
        self.bar0_write(BAR0_DEVCMD_CMD, cmd)
        self.bar0_write32(BAR0_DEVCMD_DONE, 0)
        self.bar0_write32(BAR0_DEVCMD, 1)
        if not self.bar0_read32(BAR0_DEVCMD_DONE) & 1:
            raise DeviceError(f"devcmd {cmd[0]} never set done")
        comp = self.bar0_read(BAR0_DEVCMD_COMP, 16)
        data = b""
        for off in range(0, data_len, 64):
            data += self.bar0_read(BAR0_DEVCMD_DATA + off, min(64, data_len - off))
        return comp[0], data

    def devcmd_ok(self, cmd, data_len=0):
        """devcmd(), raising DeviceError on a non-zero status."""
        status, data = self.devcmd(cmd, data_len)
        if status != 0:
            raise DeviceError(f"devcmd {cmd[0]} failed with status {status}")
        return data

    def eth_admin(self, cmd, timeout=None):
        """Run one command through the Ethernet admin queue.

        Returns its status byte.
        """
        queue, cqring = self.eth_aq, self.eth_acq
        slot = queue.prod % queue.depth
        self.mem.write(queue.slot(queue.prod), cmd)
        queue.prod += 1
        self.doorbell(QTYPE_ETH_ADMINQ, 0, 0, queue.prod % queue.depth)

        # struct ionic_admin_comp, with the colour in the top bit of the
        # last byte.
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            comp = self.mem.read(cqring.slot(cqring.cons), 16)
            if ((comp[15] & 0x80) != 0) == cqring.color():
                break
            if time.monotonic() > deadline:
                raise DeviceError(f"eth adminq command {cmd[0]} drew no completion")
            time.sleep(0.001)
        cqring.cons += 1
        got = struct.unpack_from("<H", comp, 2)[0]
        if got != slot:
            raise DeviceError(f"eth admin completion for slot {got}, expected {slot}")
        return comp[0]

    def eth_admin_ok(self, cmd, timeout=None):
        """eth_admin(), raising DeviceError on a non-zero status."""
        status = self.eth_admin(cmd, timeout)
        if status != 0:
            raise DeviceError(f"eth admin command {cmd[0]} failed with status {status}")

    def eq_next(self):
        """The next event on the EQ, as a CQ id, or None if there is none."""
        raw = self.mem.read(self.eq.slot(self.eq.cons), 4)
        event = struct.unpack(">I", raw)[0]
        if ((event & 1) != 0) != self.eq.color():
            return None
        self.eq.cons += 1
        return event >> 8

    def admin_post(self, op, body=b""):
        """Write one RDMA admin WQE and ring the doorbell.

        Returns the ring slot its completion will name.
        """
        queue = self.aq
        strides = (ADMIN_HDR_LEN + len(body) + ADMIN_STRIDE - 1) // ADMIN_STRIDE
        if strides > 4:
            raise ValueError(f"admin body of {len(body)} bytes is too long")
        slot = queue.prod % queue.depth
        wqe = bytearray(strides * ADMIN_STRIDE)
        wqe[0] = op
        struct.pack_into("<H", wqe, 2, len(body))
        wqe[ADMIN_HDR_LEN : ADMIN_HDR_LEN + len(body)] = body
        # ionic_admin_poll_locked() produces each stride as its own ring
        # entry, so a WQE that reaches the end of the ring continues at its
        # start.
        for i in range(strides):
            off = i * ADMIN_STRIDE
            self.mem.write(
                queue.slot(queue.prod + i), bytes(wqe[off : off + ADMIN_STRIDE])
            )
        queue.prod += strides
        self.doorbell(QTYPE_AQ, 0, 0, queue.prod % queue.depth)
        return slot

    def admin_complete(self, op, slot, timeout=None):
        """Take an admin completion off the admin CQ.

        Returns (status, resp_len). A non-zero status is the device's
        rejection, which some tests assert on, so this does not raise for
        one; it raises only when no completion arrives.
        """
        cqring = self.acq
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            cqe = self.mem.read(cqring.slot(cqring.cons), 32)
            qtf = struct.unpack_from(">I", cqe, 28)[0]
            if ((qtf & 1) != 0) == cqring.color():
                break
            if time.monotonic() > deadline:
                raise DeviceError(f"admin op {op} drew no completion")
            time.sleep(0.001)
        cqring.cons += 1

        cmd_idx = struct.unpack_from(">H", cqe, 0)[0]
        if cmd_idx != slot or cqe[2] != op or (qtf >> 8) != 0:
            raise DeviceError(
                f"admin completion for op {cqe[2]} slot {cmd_idx}, "
                f"expected op {op} slot {slot}"
            )
        if ((qtf >> 5) & 7) != CQE_TYPE_ADMIN:
            raise DeviceError(f"admin completion has type {(qtf >> 5) & 7}")

        # The driver only looks at the admin CQ when the EQ says to. The
        # device writes the completion first and the event after, so the
        # event has its own wait: finding the completion says nothing
        # about whether the event has landed yet, and under valgrind the
        # gap between the two is wide enough to lose that race.
        event_deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            event = self.eq_next()
            if event == ACQ_ID:
                break
            if event is not None:
                raise DeviceError(f"admin op {op} drew an event for CQ {event}")
            if time.monotonic() > event_deadline:
                raise DeviceError(f"admin op {op} completed with no event on the EQ")
            time.sleep(0.001)

        status_len = struct.unpack_from(">I", cqe, 24)[0]
        if qtf & 2:
            # The status code is the whole field, as the driver reads it.
            status = status_len
            if status == 0 or status > 0xFF:
                raise DeviceError(
                    f"admin op {op} has the error bit with status {status}"
                )
            return status, 0
        return 0, status_len

    def admin(self, op, body=b"", timeout=None):
        """Run one RDMA admin command. Returns (status, resp_len)."""
        slot = self.admin_post(op, body)
        return self.admin_complete(op, slot, timeout)

    def admin_ok(self, op, body=b"", timeout=None):
        """admin(), raising DeviceError on a non-zero status."""
        status, resp_len = self.admin(op, body, timeout)
        if status != 0:
            raise DeviceError(f"admin op {op} failed with status {status}")
        return resp_len

    def max_frame_size(self):
        """The largest frame LIF_IDENTIFY says the port takes, FCS excluded.

        The driver lets the guest set an MTU of up to this less
        VLAN_ETH_HLEN, so a tagged frame at that MTU is exactly this long.
        """
        cmd = bytearray(64)
        cmd[0] = CMD_LIF_IDENTIFY
        data = self.devcmd_ok(bytes(cmd), LIFID_MAX_FRAME_SIZE_OFF + 4)
        return struct.unpack_from("<I", data, LIFID_MAX_FRAME_SIZE_OFF)[0]

    # ---- bring-up -------------------------------------------------------

    def bringup(self):
        """Everything the driver does before the first RDMA admin command.

        The devcmds from IDENTIFY to LIF_INIT, the Ethernet admin queue,
        interrupt unmasking, and the four RDMA devcmds that create the
        EQ, the admin CQ, and the admin queue.
        """
        if self.bar0_read32(0) != IONIC_DEV_SIG:
            raise DeviceError("no DEVI signature at BAR0 offset 0")

        for opcode in (CMD_IDENTIFY, CMD_RESET, CMD_PORT_IDENTIFY, CMD_LIF_IDENTIFY):
            cmd = bytearray(64)
            cmd[0] = opcode
            self.devcmd_ok(bytes(cmd))

        for opcode, info in (
            (CMD_PORT_INIT, self.port_info),
            (CMD_LIF_INIT, self.lif_info),
        ):
            cmd = bytearray(64)
            cmd[0] = opcode
            struct.pack_into("<Q", cmd, 8, info)
            self.devcmd_ok(bytes(cmd))

        # The Ethernet admin queue: struct ionic_q_init_cmd
        self.eth_aq = self.alloc_ring(ETH_AQ_DEPTH_LOG2, 6)
        self.eth_acq = self.alloc_ring(ETH_AQ_DEPTH_LOG2, 4)
        cmd = bytearray(64)
        cmd[0] = CMD_Q_INIT
        cmd[4] = QTYPE_ETH_ADMINQ
        struct.pack_into("<I", cmd, 8, 0)
        struct.pack_into("<H", cmd, 14, ETH_ADMINQ_INTR)
        cmd[19] = ETH_AQ_DEPTH_LOG2
        struct.pack_into("<Q", cmd, 20, self.eth_aq.gpa)
        struct.pack_into("<Q", cmd, 28, self.eth_acq.gpa)
        self.devcmd_ok(bytes(cmd))

        self.intr_mask(ETH_ADMINQ_INTR, False)
        self.intr_mask(EQ_INTR, False)

        cmd = bytearray(64)
        cmd[0] = CMD_RDMA_RESET
        self.eth_admin_ok(bytes(cmd))

        self.eq = self.alloc_ring(EQ_DEPTH_LOG2, 2)
        self.acq = self.alloc_ring(ACQ_DEPTH_LOG2, CQE_STRIDE_LOG2)
        self.aq = self.alloc_ring(AQ_DEPTH_LOG2, 6)
        queues = (
            (CMD_RDMA_EQ, self.eq, EQ_ID, EQ_INTR),
            (CMD_RDMA_CQ, self.acq, ACQ_ID, EQ_ID),
            (CMD_RDMA_ADMINQ, self.aq, 0, ACQ_ID),
        )
        for opcode, ring, qid, cid in queues:
            cmd = bytearray(64)
            cmd[0] = opcode
            struct.pack_into("<I", cmd, 4, qid | 1 << 24)
            struct.pack_into("<I", cmd, 8, cid)
            cmd[14] = ring.depth_log2
            cmd[15] = ring.stride_log2
            struct.pack_into("<Q", cmd, 16, ring.gpa)
            self.eth_admin_ok(bytes(cmd))
        return self

    # ---- resources ------------------------------------------------------

    def create_cq(self, cq_id, depth_log2=ACQ_DEPTH_LOG2):
        """CREATE_CQ. struct ionic_admin_create_cq."""
        ring = self.alloc_ring(depth_log2, CQE_STRIDE_LOG2)
        body = bytearray(34)
        struct.pack_into("<I", body, 0, EQ_ID)
        body[4] = depth_log2
        body[5] = CQE_STRIDE_LOG2
        body[7] = 12
        struct.pack_into("<I", body, 12, cq_id | 1 << 24)
        struct.pack_into("<I", body, 20, 1)
        struct.pack_into("<Q", body, 24, ring.gpa)
        self.admin_ok(ADMIN_CREATE_CQ, bytes(body))
        return Cq(cq_id, ring)

    def destroy_cq(self, cq):
        """DESTROY_CQ. Returns the completion status."""
        return self.admin(ADMIN_DESTROY_CQ, struct.pack("<I", cq.id))[0]

    def create_qp(self, qp_id, qp_type, scq, rcq, srq=None):
        """CREATE_QP. struct ionic_admin_create_qp."""
        send_ring = self.alloc_ring(QP_DEPTH_LOG2, WQE_STRIDE_LOG2)
        recv_ring = None if srq else self.alloc_ring(QP_DEPTH_LOG2, WQE_STRIDE_LOG2)
        body = bytearray(64)
        struct.pack_into("<I", body, 8, scq.id)
        body[12] = QP_DEPTH_LOG2
        body[13] = WQE_STRIDE_LOG2
        body[15] = 12
        struct.pack_into("<I", body, 20, 1)
        struct.pack_into("<Q", body, 24, send_ring.gpa)
        struct.pack_into("<I", body, 32, rcq.id)
        if srq:
            # An SRQ-bound QP leaves its own receive ring empty.
            struct.pack_into("<I", body, 40, srq.id)
        else:
            body[36] = QP_DEPTH_LOG2
            body[37] = WQE_STRIDE_LOG2
            body[39] = 12
            struct.pack_into("<I", body, 40, 0xFFFFFFFF)
            struct.pack_into("<I", body, 44, 1)
            struct.pack_into("<Q", body, 48, recv_ring.gpa)
        struct.pack_into("<I", body, 56, qp_id | 1 << 24)
        body[62] = qp_type
        self.admin_ok(ADMIN_CREATE_QP, bytes(body))
        return Qp(qp_id, qp_type, send_ring, recv_ring)

    def destroy_qp(self, qp):
        """DESTROY_QP. Returns the completion status."""
        return self.admin(ADMIN_DESTROY_QP, struct.pack("<I", qp.id))[0]

    def modify_qp(self, qp_id, attr_mask, state, qkey_dest_qpn=0, hdr=None):
        """MODIFY_QP. struct ionic_admin_mod_qp. Returns the status.

        @hdr is a RoCE header template the device reads the peer's
        address out of; it goes in the scratch page, which stands in for
        the driver's DMA-mapped header buffer.
        """
        body = bytearray(60)
        if hdr:
            if len(hdr) > 255:
                raise ValueError("RoCE header template is too long")
            self.mem.write(self.scratch, hdr)
            struct.pack_into("<I", body, 32, len(hdr) << 24)
            struct.pack_into("<Q", body, 48, self.scratch)
        struct.pack_into(">I", body, 0, attr_mask)
        struct.pack_into("<I", body, 8, 0x100)  # rq_psn
        struct.pack_into("<I", body, 12, 0x200)  # sq_psn
        struct.pack_into("<I", body, 16, qkey_dest_qpn)
        body[39] = state
        struct.pack_into("<I", body, 56, qp_id | 1 << 24)
        return self.admin(ADMIN_MODIFY_QP, bytes(body))[0]

    def connect_qp(self, qp, dest_qpn, hdr):
        """Take one QP through INIT, RTR and RTS, pointed at @dest_qpn."""
        steps = (
            (QP_STATE, QPS_INIT, 0, None),
            (
                QP_STATE | QP_AV | QP_DEST_QPN | QP_RQ_PSN,
                QPS_RTR,
                dest_qpn,
                hdr,
            ),
            (QP_STATE | QP_SQ_PSN, QPS_RTS, 0, None),
        )
        for mask, state, qkey_dest, template in steps:
            status = self.modify_qp(qp.id, mask, state, qkey_dest, template)
            if status != 0:
                raise DeviceError(
                    f"connecting QP {qp.id} to {dest_qpn}: state {state} "
                    f"failed with status {status}"
                )

    def create_mr(self, lkey, va, length, access=ACCESS_ALL, reverse=False):
        """CREATE_MR, with fresh guest pages behind it.

        With @reverse the pages go into guest memory in reverse order, so
        no two neighbours are physically adjacent.
        """
        mr = self.alloc_mr(lkey, va, length, reverse)
        body = bytearray(45)
        struct.pack_into("<Q", body, 0, va)
        struct.pack_into("<Q", body, 8, length)
        struct.pack_into("<I", body, 20, lkey)
        struct.pack_into("<I", body, 28, len(mr.pages))
        struct.pack_into("<Q", body, 32, mr.dma_addr())
        struct.pack_into("<H", body, 40, access)
        body[44] = 12
        self.admin_ok(ADMIN_CREATE_MR, bytes(body))
        return mr

    def alloc_mr(self, lkey, va, length, reverse=False):
        """Allocate the backing pages and page table, without CREATE_MR."""
        first_off = va % PAGE_SIZE
        npages = (first_off + length + PAGE_SIZE - 1) // PAGE_SIZE
        pages = [0] * npages
        order = range(npages - 1, -1, -1) if reverse else range(npages)
        for index in order:
            pages[index] = self.mem.alloc(PAGE_SIZE)
        pt_gpa = self.mem.alloc(8 * npages, align=8)
        for i, page in enumerate(pages):
            self.mem.write_u64(pt_gpa + 8 * i, page)
        return Mr(lkey, va, length, pages, pt_gpa)

    def destroy_mr(self, mr):
        """DESTROY_MR. Returns the completion status."""
        return self.admin(ADMIN_DESTROY_MR, struct.pack("<I", mr.lkey))[0]

    def mr_write(self, mr, va, data):
        """Write through an MR's page table, as the application sees it."""
        self._mr_copy(mr, va, len(data), data)

    def mr_read(self, mr, va, length):
        """Read through an MR's page table."""
        return self._mr_copy(mr, va, length, None)

    def _mr_copy(self, mr, va, length, data):
        if va < mr.va or length > mr.len or va - mr.va > mr.len - length:
            raise ValueError(f"[{va:#x}, +{length}) is outside MR {mr.lkey:#x}")
        off = va - mr.va + mr.va % PAGE_SIZE
        done = 0
        out = bytearray()
        while done < length:
            gpa = mr.pages[off // PAGE_SIZE] + off % PAGE_SIZE
            count = min(PAGE_SIZE - off % PAGE_SIZE, length - done)
            if data is None:
                out += self.mem.read(gpa, count)
            else:
                self.mem.write(gpa, data[done : done + count])
            done += count
            off += count
        return bytes(out)

    def create_srq(self, depth_log2=QP_DEPTH_LOG2):
        """CREATE_SRQ. The id comes back by DMA."""
        ring = self.alloc_ring(depth_log2, WQE_STRIDE_LOG2)
        resp = self.scratch + 512
        self.mem.write(resp, b"\xff" * 5)
        body = bytearray(42)
        struct.pack_into("<Q", body, 0, ring.gpa)
        struct.pack_into("<I", body, 8, 1)
        body[26] = depth_log2
        body[27] = WQE_STRIDE_LOG2
        body[28] = 12
        struct.pack_into("<I", body, 30, 5)
        struct.pack_into("<Q", body, 34, resp)
        resp_len = self.admin_ok(ADMIN_CREATE_SRQ, bytes(body))
        if resp_len != 5:
            raise DeviceError(f"CREATE_SRQ reported a {resp_len}-byte response")
        return Srq(self.mem.read_u32(resp), ring)

    def destroy_srq(self, srq):
        """DESTROY_SRQ. Returns the completion status."""
        return self.admin(ADMIN_DESTROY_SRQ, struct.pack("<I", srq.id))[0]

    def create_ah(self, ah_id, sip, dip):
        """CREATE_AH from an IPv4 RoCE header template."""
        hdr = roce_hdr_v4(sip, dip)
        self.mem.write(self.scratch, hdr)
        body = bytearray(24)
        struct.pack_into("<Q", body, 0, self.scratch)
        struct.pack_into("<I", body, 8, len(hdr))
        struct.pack_into("<I", body, 16, ah_id | 1 << 24)
        return self.admin(ADMIN_CREATE_AH, bytes(body))[0]

    # ---- work requests and completions ----------------------------------

    def post_send(self, qp, op, **kwargs):
        """Post one send WQE and ring the SQ doorbell.

        Keyword arguments, all optional: flags, imm_be, key, ah_id,
        dest_qpn, qkey, remote_va, rkey, length, swap_add, compare, sge,
        inline_data, mr.
        """
        wqe = self._build_send_wqe(qp, op, kwargs)
        self.mem.write(qp.sq.slot(qp.sq.prod), wqe)
        qp.sq.prod += 1
        self.doorbell(QTYPE_SQ, qp.id, 0, qp.sq.prod % qp.sq.depth)
        return struct.unpack_from("<Q", wqe, 0)[0]

    def _build_send_wqe(self, qp, op, args):
        # One WQE layout with a branch per opcode class; splitting it up
        # would scatter struct ionic_v1_wqe across several functions.
        # pylint: disable=too-many-branches,too-many-statements
        stride = 1 << WQE_STRIDE_LOG2
        sges = args.get("sge") or []
        inline_data = args.get("inline_data")
        flags = args.get("flags", 0)
        if len(sges) > (stride - WQE_PLD_OFF) // 16:
            raise ValueError(f"send WQE does not fit a {stride}-byte stride")
        if inline_data and len(inline_data) > stride - WQE_PLD_OFF:
            raise ValueError(f"inline payload does not fit a {stride}-byte stride")

        wqe_id = qp.next_wqe_id
        qp.next_wqe_id += 1
        total = sum(s.len for s in sges)

        wqe = bytearray(stride)
        struct.pack_into("<Q", wqe, 0, wqe_id)
        wqe[8] = op
        wqe[9] = len(sges)
        struct.pack_into(">H", wqe, 10, flags)
        if op in (OP_SEND_INV, OP_LOCAL_INV, OP_REG_MR):
            struct.pack_into(">I", wqe, 12, args.get("key", 0))
        else:
            wqe[12:16] = args.get("imm_be", b"\0\0\0\0")

        if op in (OP_RDMA_READ, OP_RDMA_WRITE, OP_RDMA_WRITE_IMM):
            _put_be32_pair(wqe, 16, args.get("remote_va", 0))
            struct.pack_into(">I", wqe, 24, args.get("rkey", 0))
            struct.pack_into(">I", wqe, 28, args.get("length") or total)
        elif op in (OP_ATOMIC_CS, OP_ATOMIC_FA):
            # struct ionic_v1_atomic_bdy: one fixed 8-byte result SGE
            _put_be32_pair(wqe, 16, args.get("remote_va", 0))
            struct.pack_into(">I", wqe, 24, args.get("rkey", 0))
            _put_be32_pair(wqe, 28, args.get("swap_add", 0))
            _put_be32_pair(wqe, 36, args.get("compare", 0))
            if sges:
                struct.pack_into(">Q", wqe, 48, sges[0].va)
                struct.pack_into(">I", wqe, 56, 8)
                struct.pack_into(">I", wqe, 60, sges[0].lkey)
        elif op == OP_REG_MR:
            mr = args.get("mr")
            if mr:
                struct.pack_into(">Q", wqe, 16, mr.va)
                struct.pack_into(">Q", wqe, 24, mr.len)
                struct.pack_into(">Q", wqe, 40, mr.dma_addr())
                struct.pack_into(">I", wqe, 48, len(mr.pages))
                wqe[55] = 12
        elif op == OP_LOCAL_INV:
            pass
        else:
            struct.pack_into(">I", wqe, 16, args.get("ah_id", 0))
            struct.pack_into(">I", wqe, 20, args.get("dest_qpn", 0))
            struct.pack_into(">I", wqe, 24, args.get("qkey", 0))
            struct.pack_into(">I", wqe, 28, total)

        if op not in (OP_ATOMIC_CS, OP_ATOMIC_FA, OP_REG_MR):
            if flags & WQE_INLINE:
                wqe[WQE_PLD_OFF : WQE_PLD_OFF + len(inline_data)] = inline_data
                struct.pack_into(">I", wqe, 28, len(inline_data))
            else:
                _put_sges(wqe, sges)
        return bytes(wqe)

    def post_recv(self, qp, sges):
        """Post one receive on a QP's own receive ring."""
        return self._post_recv(qp.rq, qp.id, qp, sges)

    def post_srq_recv(self, srq, sges):
        """Post one receive on a shared receive queue."""
        return self._post_recv(srq.ring, srq.id, srq, sges)

    def _post_recv(self, ring, qid, owner, sges):
        stride = 1 << WQE_STRIDE_LOG2
        if len(sges) > (stride - WQE_PLD_OFF) // 16:
            raise ValueError("receive WQE has too many SGEs")
        wqe_id = owner.next_wqe_id
        owner.next_wqe_id += 1
        wqe = bytearray(stride)
        struct.pack_into("<Q", wqe, 0, wqe_id)
        wqe[9] = len(sges)
        _put_sges(wqe, sges)
        self.mem.write(ring.slot(ring.prod), bytes(wqe))
        ring.prod += 1
        self.doorbell(QTYPE_RQ, qid, 0, ring.prod % ring.depth)
        return wqe_id

    def poll_cq(self, cq):
        """Take the next completion off @cq, or None if there is none."""
        ring = cq.ring
        raw = self.mem.read(ring.slot(ring.cons), 32)
        qtf = struct.unpack_from(">I", raw, 28)[0]
        if ((qtf & 1) != 0) != ring.color():
            return None
        ring.cons += 1
        self.doorbell(QTYPE_CQ, cq.id, 0, ring.cons % ring.depth)
        return Cqe(raw)

    def wait_cq(self, cq, timeout=None):
        """Wait for the next completion on @cq.

        Raises DeviceError if none arrives, which is what a test wants:
        a missing completion is a failure, not an empty result.
        """
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            cqe = self.poll_cq(cq)
            if cqe is not None:
                return cqe
            if time.monotonic() > deadline:
                raise DeviceError(f"no completion on CQ {cq.id}")
            time.sleep(0.001)

    def arm_cq(self, cq):
        """Arm @cq for its next completion."""
        self.doorbell(QTYPE_CQ, cq.id, 1, cq.ring.cons % cq.ring.depth)


def _put_sges(wqe, sges):
    for i, sge in enumerate(sges):
        off = WQE_PLD_OFF + 16 * i
        struct.pack_into(">Q", wqe, off, sge.va)
        struct.pack_into(">I", wqe, off + 8, sge.len)
        struct.pack_into(">I", wqe, off + 12, sge.lkey)


def _put_be32_pair(buf, off, value):
    """The wire splits 64-bit RDMA addresses and operands into be32 halves."""
    struct.pack_into(">I", buf, off, (value >> 32) & 0xFFFFFFFF)
    struct.pack_into(">I", buf, off + 4, value & 0xFFFFFFFF)
