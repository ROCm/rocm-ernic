"""The emulated ionic Ethernet data queues, as the guest driver drives them.

Split out of :mod:`ernic.ionic` because it is a separate datapath: the
Ethernet Tx and Rx rings carry raw frames and have their own descriptor,
completion and scatter-gather layouts, none of which the RDMA queues share.
:class:`EthQueueMixin` is mixed into ``IonicDriver``, so the methods here
are part of that driver's interface and reach the device through it.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import struct
import time

from .errors import DeviceError

# BAR2 doorbell qtypes for the Ethernet data queues.
QTYPE_ETH_RXQ = 2
QTYPE_ETH_TXQ = 3

CMD_Q_INIT = 40
CMD_Q_CONTROL = 41
Q_DISABLE = 0
Q_ENABLE = 1

# Emulated Ethernet queue geometry (src/ionic/ionic_eth_emu_priv.h).
ETH_DESC_SIZE = 16
ETH_COMP_SIZE = 16
ETH_COMP_COLOR = 0x80  # bit set in the last byte of a new completion
ETH_SG_MAX_ELEMS = 8
ETH_SG_ELEM_SIZE = 16
ETH_SG_STRIDE = ETH_SG_MAX_ELEMS * ETH_SG_ELEM_SIZE  # per descriptor index
ETH_QCOUNT = 4  # IONIC_EMU_ETH_QCOUNT: queues per qtype
ETH_MAX_DEPTH_LOG2 = 15  # the emulator rejects ring_size >= 16
ETH_MTU_MAX = 9600  # IONIC_ETH_NET_MTU_MAX


# A record plus its ring bookkeeping; the attributes are the interface.
# pylint: disable-next=too-many-instance-attributes
class EthQueue:
    """An emulated Ethernet Tx or Rx queue and its completion ring.

    Deliberately not a Ring: the Ethernet rings have a fixed 16-byte stride
    for both descriptors and completions, and their completion colour starts
    at 1 -- the first completion written to a freshly initialised queue has
    bit 0x80 set -- so the consumer tracks its own expected colour rather
    than deriving it from a lap count.
    """

    # The three ring addresses are what Q_INIT was given, so there is no
    # smaller set to construct one from.
    # pylint: disable-next=too-many-arguments,too-many-positional-arguments
    def __init__(self, qtype, index, depth_log2, ring, cq_ring, sg_ring=0):
        self.qtype = qtype
        self.index = index
        self.depth_log2 = depth_log2
        self.ring = ring
        self.cq_ring = cq_ring
        self.sg_ring = sg_ring
        self.prod = 0  # descriptors posted, ever
        self.cons = 0  # completions consumed, ever
        self.cq_index = 0  # next completion slot to read
        self.cq_color = True  # colour that marks a completion as new
        # What is posted in each ring slot, as (address, bytes), or None.
        # Indexed by ring slot rather than by posting order, because a
        # completion names a slot: the two agree only until the ring wraps.
        self.posted = [None] * self.depth

    @property
    def depth(self):
        """Entries in the ring."""
        return 1 << self.depth_log2

    @property
    def outstanding(self):
        """Descriptors posted but not yet completed."""
        return self.prod - self.cons

    def claim(self, count=1):
        """Reserve @count ring slots, or raise.

        The device reads a descriptor when it gets to it, not when the
        doorbell rings, so posting over an uncompleted one does not fail --
        it replaces a frame that has not been sent yet, and the test sees a
        wrong frame or a missing completion somewhere else entirely.
        """
        if count > self.depth:
            raise ValueError(f"{count} descriptors will not fit a ring of {self.depth}")
        if self.outstanding + count > self.depth:
            raise DeviceError(
                f"ring is full: {self.outstanding} of {self.depth} "
                f"outstanding, {count} more requested; consume completions "
                f"first"
            )
        return self.prod % self.depth

    def release(self, index):
        """Take back what was posted in slot @index, or raise."""
        if not 0 <= index < self.depth:
            raise DeviceError(
                f"completion names slot {index}, outside a ring of " f"{self.depth}"
            )
        entry = self.posted[index]
        if entry is None:
            raise DeviceError(f"completion names unposted slot {index}")
        self.posted[index] = None
        return entry

    def desc_slot(self, index):
        """Guest physical address of descriptor @index."""
        return self.ring + (index % self.depth) * ETH_DESC_SIZE

    def comp_slot(self, index):
        """Guest physical address of completion @index."""
        return self.cq_ring + (index % self.depth) * ETH_COMP_SIZE

    def sg_slot(self, index):
        """Guest physical address of the SG block for descriptor @index."""
        return self.sg_ring + (index % self.depth) * ETH_SG_STRIDE

    def advance_cq(self):
        """Step the completion consumer, flipping colour on wrap."""
        self.cons += 1
        self.cq_index += 1
        if self.cq_index == self.depth:
            self.cq_index = 0
            self.cq_color = not self.cq_color


class EthQueueMixin:
    """Ethernet Tx and Rx queue support for :class:`~ernic.ionic.IonicDriver`.

    Mixed in rather than standalone: every method here reaches the device
    through the driver's own ``mem``, ``doorbell`` and ``devcmd_ok``.
    """

    def eth_q_init(self, qtype, index=0, depth_log2=3, intr=0, sg=False):
        """Q_INIT an Ethernet Tx or Rx queue. struct ionic_q_init_cmd.

        The emulator reports IONIC_RC_SUCCESS for a Q_INIT it rejected --
        a bad qtype, an index at or above ETH_QCOUNT, or a depth_log2 of 16
        or more all leave the queue uncreated with status 0 -- so a clean
        completion proves nothing. The arguments are checked here instead,
        because a queue that silently does not exist shows up much later as
        a poll that never completes.
        """
        if qtype not in (QTYPE_ETH_TXQ, QTYPE_ETH_RXQ):
            raise ValueError(f"not an Ethernet data queue: qtype {qtype}")
        if not 0 <= index < ETH_QCOUNT:
            raise ValueError(f"queue index {index} is outside the emulator")
        if not 0 <= depth_log2 <= ETH_MAX_DEPTH_LOG2:
            raise ValueError(f"depth_log2 {depth_log2} is out of range")

        depth = 1 << depth_log2
        ring = self.mem.alloc(depth * ETH_DESC_SIZE)
        cq_ring = self.mem.alloc(depth * ETH_COMP_SIZE)
        sg_ring = self.mem.alloc(depth * ETH_SG_STRIDE) if sg else 0

        cmd = bytearray(64)
        cmd[0] = CMD_Q_INIT
        cmd[4] = qtype
        struct.pack_into("<I", cmd, 8, index)
        struct.pack_into("<H", cmd, 14, intr)
        cmd[19] = depth_log2
        struct.pack_into("<Q", cmd, 20, ring)
        struct.pack_into("<Q", cmd, 28, cq_ring)
        struct.pack_into("<Q", cmd, 36, sg_ring)
        self.devcmd_ok(bytes(cmd))

        # ionic_qcq_enable() sends this once a queue is initialised, and
        # the emulator serves Rx only on enabled queues: without it
        # poll_rx() sees no active queue, returns before either of its
        # loops, and nothing is ever delivered -- including frames the P4
        # port injected, which never touch an Rx ring of their own.
        self.eth_q_control(qtype, index, Q_ENABLE)

        queue = EthQueue(qtype, index, depth_log2, ring, cq_ring, sg_ring)
        if qtype == QTYPE_ETH_TXQ:
            self.eth_tx = queue
        else:
            self.eth_rx = queue
        return queue

    def eth_q_control(self, qtype, index, oper):
        """Enable or disable a queue. struct ionic_q_control_cmd.

        opcode@0, type@1, lif_index le16 @2, index le32 @4, oper@8.
        """
        cmd = bytearray(64)
        cmd[0] = CMD_Q_CONTROL
        cmd[1] = qtype
        struct.pack_into("<I", cmd, 4, index)
        cmd[8] = oper
        self.devcmd_ok(bytes(cmd))

    def post_tx(self, frame, split=None):
        """Transmit @frame as the guest driver would.

        With @split, a list of trailing fragment lengths, the frame is sent
        through the scatter-gather ring: the head fragment carries what is
        left once those are subtracted. The queue must then have been
        created with sg=True.
        """
        queue = self.eth_tx
        if queue is None:
            raise DeviceError("no Tx queue; call eth_q_init first")
        if not 0 < len(frame) <= ETH_MTU_MAX:
            raise ValueError(f"frame of {len(frame)} bytes is out of range")

        split = list(split or [])
        if len(split) > ETH_SG_MAX_ELEMS:
            raise ValueError(f"at most {ETH_SG_MAX_ELEMS} SG elements")
        if split and not queue.sg_ring:
            raise DeviceError("Tx queue was not created with sg=True")
        tail = sum(split)
        if tail >= len(frame):
            raise ValueError("the SG fragments cover the whole frame")

        index = queue.claim()
        buf = self.mem.alloc(len(frame))
        self.mem.write(buf, frame)

        desc = bytearray(ETH_DESC_SIZE)
        # addr is stored shifted, with the SG element count beside it.
        struct.pack_into("<Q", desc, 0, (buf << 12) | (len(split) << 8))
        struct.pack_into("<H", desc, 8, len(frame))
        self.mem.write(queue.desc_slot(index), bytes(desc))

        if split:
            sg = bytearray(ETH_SG_STRIDE)
            off = len(frame) - tail
            for i, flen in enumerate(split):
                struct.pack_into("<Q", sg, i * ETH_SG_ELEM_SIZE, buf + off)
                struct.pack_into("<H", sg, i * ETH_SG_ELEM_SIZE + 8, flen)
                off += flen
            self.mem.write(queue.sg_slot(index), bytes(sg))

        queue.posted[index] = (buf, len(frame))
        queue.prod += 1
        self.doorbell(QTYPE_ETH_TXQ, queue.index, 0, queue.prod % queue.depth)
        # The ring slot, not the posting number: a completion names a slot,
        # so returning anything else gives a caller two numbers that agree
        # for the first lap of the ring and then quietly stop agreeing.
        return index

    def poll_tx(self):
        """Return the completed descriptor index, or None. ionic_txq_comp."""
        comp = self._poll_eth_comp(self.eth_tx)
        if comp is None:
            return None
        index, raw = comp
        self.eth_tx.release(index)
        return index, raw

    def wait_tx(self, timeout=None):
        """Wait for the next Tx completion, or raise DeviceError."""
        return self._wait_eth(self.eth_tx, self.poll_tx, "Tx", timeout)

    def post_rx_buffers(self, count=1, size=2048):
        """Post @count receive buffers of @size bytes. ionic_rxq_desc."""
        queue = self.eth_rx
        if queue is None:
            raise DeviceError("no Rx queue; call eth_q_init first")
        queue.claim(count)

        buffers = []
        for _ in range(count):
            buf = self.mem.alloc(size)
            buffers.append(buf)
            index = queue.prod % queue.depth
            desc = bytearray(ETH_DESC_SIZE)
            struct.pack_into("<H", desc, 6, size)
            struct.pack_into("<Q", desc, 8, buf)
            self.mem.write(queue.desc_slot(index), bytes(desc))
            queue.posted[index] = (buf, size)
            queue.prod += 1

        self.doorbell(QTYPE_ETH_RXQ, queue.index, 0, queue.prod % queue.depth)
        return buffers

    def poll_rx(self):
        """Return a received frame, or None. ionic_rxq_comp."""
        queue = self.eth_rx
        comp = self._poll_eth_comp(queue)
        if comp is None:
            return None
        index, raw = comp
        length = struct.unpack_from("<H", raw, 12)[0]
        # The slot the completion names, not the n-th buffer ever posted:
        # those are the same number only until the ring wraps, after which
        # the frame would be read out of the oldest buffer instead of the
        # one it was delivered into.
        buf, size = queue.release(index)
        if length > size:
            raise DeviceError(
                f"Rx completion claims {length} bytes in a {size}-byte buffer"
            )
        return self.mem.read(buf, length)

    def wait_rx(self, timeout=None):
        """Wait for the next received frame, or raise DeviceError."""
        return self._wait_eth(self.eth_rx, self.poll_rx, "Rx", timeout)

    def _poll_eth_comp(self, queue):
        """Read the completion at the consumer index if it is new."""
        if queue is None:
            raise DeviceError("queue not initialised")
        raw = self.mem.read(queue.comp_slot(queue.cq_index), ETH_COMP_SIZE)
        if bool(raw[ETH_COMP_SIZE - 1] & ETH_COMP_COLOR) != queue.cq_color:
            return None
        index = struct.unpack_from("<H", raw, 2)[0]
        queue.advance_cq()
        return index, raw

    def _wait_eth(self, queue, poll, what, timeout):
        """Poll until @poll yields, then raise rather than return nothing.

        A test wants a missing completion to be a failure: the emulator
        drops a frame larger than the posted buffer and delivers nothing to
        an Rx queue other than 0, both of which otherwise look like a hang.
        """
        if queue is None:
            raise DeviceError(f"no {what} queue; call eth_q_init first")
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            got = poll()
            if got is not None:
                return got
            if time.monotonic() > deadline:
                raise DeviceError(f"no {what} completion")
            time.sleep(0.001)
