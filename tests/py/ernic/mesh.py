"""The TCP mesh: its wire protocol, and fixtures for standing a mesh up.

Two things live here. The first is the protocol a mesh node speaks to its
peers, so a test can stand in for one: register as a worker, advertise
GIDs, or send a frame no real node would send and check that the node on
the other end refuses it rather than trusting it.

The second is a fixture that starts a manager and some workers as real
processes, each with its own vfio-user socket and its own guest GIDs, so
a test can attach a driver to each and move traffic between them. Guest
GIDs are IPv4 here: a RoCE-over-IPv4 header resolves to the
::ffff:a.b.c.d form that an IPv4 address in ERNIC_TCP_GUEST_GIDS is
stored in, so the two compare equal and the node that owns a destination
is the node the test meant.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import re
import socket
import struct
import threading
import time

from .server import Server, ServerFailed

MAGIC = 0x52444D41  # "RDMA"
PROTOCOL_VERSION = 4
MAX_PAYLOAD_LEN = 16 << 20

# A manager keeps this many connections waiting to register at once; one
# past it is closed.
MAX_PENDING_CONNS = 64
# How long a peer has to finish its handshake, and to register.
HANDSHAKE_TIMEOUT_MS = 5000
REGISTER_TIMEOUT_MS = 5000

# The topology message carries at most this many nodes.
MAX_MESH_NODES = 64
# A node advertises at most this many of its own GIDs.
MAX_NODE_GIDS = 8

# TcpMsgType. The retired ordinals stay reserved; see the enum in
# src/rdma/rdma_backend_tcp_priv.h.
MSG_HANDSHAKE = 1
MSG_HANDSHAKE_RESP = 2
MSG_REGISTER_NODE = 12
MSG_REGISTER_RESP = 13
MSG_MESH_TOPOLOGY = 14
MSG_HEARTBEAT = 15
MSG_HEARTBEAT_RESP = 16
MSG_NODE_STATUS = 17
MSG_IONIC = 24
MSG_NODE_GIDS = 25

HEADER_FMT = "!IIIIIIII"
HEADER_SIZE = 32

# Sizes of a pre-v4 registration, which has no version field.
REGISTER_NODE_LEN_V3 = 262
REGISTER_RESP_LEN_V3 = 12

NODE_ID_AUTO = 0xFFFFFFFF


# The header has eight fields and a test sets whichever it is about;
# collapsing them into a struct would only hide the wire format.
# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def pack_header(
    msg_type, payload_len, seq=1, src_node=0, dst_node=0, src_qpn=0, dst_qpn=0
):
    """Build the fixed 32-byte mesh header."""
    return struct.pack(
        HEADER_FMT,
        MAGIC,
        msg_type,
        payload_len,
        seq,
        src_node,
        dst_node,
        src_qpn,
        dst_qpn,
    )


def unpack_header(data):
    """Split a mesh header into its fields."""
    return struct.unpack(HEADER_FMT, data[:HEADER_SIZE])


def register_payload(hostname=b"peer", port=9999, node_id=NODE_ID_AUTO, version=None):
    """A TCP_MSG_REGISTER_NODE payload.

    With @version None the payload stops where a pre-v4 one does, which
    is what a node running the old GID heuristic sends. A v4 manager must
    refuse it: such a node never advertises its GIDs and routes every
    peer to one default on its own side.
    """
    body = hostname.ljust(256, b"\0") + struct.pack("!H", port)
    body += struct.pack("!I", node_id)
    if version is not None:
        body += struct.pack("!I", version)
    return body


def parse_register_resp(payload):
    """A TCP_MSG_REGISTER_RESP payload: (node_id, num_nodes, result, version).

    ``result`` is signed on the wire: 0 is success, negative an error.
    ``version`` is None for a pre-v4 reply.
    """
    if len(payload) < REGISTER_RESP_LEN_V3:
        raise ValueError(f"short REGISTER_RESP: {len(payload)} bytes")
    node_id, num_nodes, result = struct.unpack("!III", payload[:12])
    if result >= 1 << 31:
        result -= 1 << 32
    version = None
    if len(payload) >= REGISTER_RESP_LEN_V3 + 4:
        version = struct.unpack("!I", payload[12:16])[0]
    return node_id, num_nodes, result, version


def gids_payload(node_id, gids):
    """A TCP_MSG_NODE_GIDS payload: be32 node id, be32 count, 8 x 16 bytes.

    Both counts are network byte order, as tcp_fill_node_gids() writes
    them with htonl() and the receive path reads them with ntohl(). The
    GIDs themselves are 16 opaque bytes and are not swapped.
    """
    if len(gids) > MAX_NODE_GIDS:
        raise ValueError(f"a node advertises at most {MAX_NODE_GIDS} GIDs")
    body = struct.pack("!II", node_id, len(gids))
    for gid in gids:
        if len(gid) != 16:
            raise ValueError("a GID is 16 bytes")
        body += gid
    return body + b"\0" * 16 * (MAX_NODE_GIDS - len(gids))


def gid_bytes(text):
    """The 16 bytes of an address, IPv4 in its ::ffff:a.b.c.d mapped form."""
    try:
        return socket.inet_pton(socket.AF_INET6, text)
    except OSError:
        return b"\0" * 10 + b"\xff\xff" + socket.inet_pton(socket.AF_INET, text)


def gid_log_form(text):
    """An address as a node logs it: eight colon-separated hex groups.

    ``TCP: node 1 owns GID 0000:...:ffff:0a00:0002``. A test greps for
    this to see what a node itself believes, rather than taking the
    manager's word for what it was told.
    """
    raw = gid_bytes(text)
    return ":".join(f"{raw[i]:02x}{raw[i + 1]:02x}" for i in range(0, 16, 2))


def free_port():
    """A port nothing is listening on, so concurrent runs don't collide."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class MeshPeer:
    """A connection to a mesh node, speaking the protocol by hand."""

    def __init__(self, host="127.0.0.1", port=None, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.node_id = None

    def connect(self):
        """Open the TCP connection."""
        self.sock = socket.create_connection(
            (self.host, self.port), timeout=self.timeout
        )
        return self

    def close(self):
        """Drop the connection, as a node that dies without cleaning up does."""
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *_exc):
        self.close()

    def send(self, msg_type, payload=b"", **kwargs):
        """Send one well-formed message."""
        self.sock.sendall(pack_header(msg_type, len(payload), **kwargs) + payload)

    def send_raw(self, data):
        """Send bytes with no framing of our own, for malformed-input tests."""
        self.sock.sendall(data)

    def send_dribbled(self, data, chunk=1, delay=0.0):
        """Send bytes a few at a time, so the peer sees a partial frame."""
        for off in range(0, len(data), chunk):
            self.sock.sendall(data[off : off + chunk])
            if delay:
                time.sleep(delay)

    def wait_closed(self, timeout=10.0):
        """Wait for the peer to close the connection. True if it did.

        What a node does with a frame it refuses is close the connection,
        so this is how a test sees the refusal from the outside rather
        than only in a log.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self.sock.settimeout(min(remaining, 0.5))
            try:
                if self.sock.recv(4096) == b"":
                    return True
            except socket.timeout:
                continue
            except OSError:
                return True

    def recv_exact(self, length):
        """Read exactly @length bytes, or None at end of stream.

        TCP delivers a stream, not messages, so a single recv() can
        return a prefix of a reply; reading short would flake.
        """
        buf = b""
        while len(buf) < length:
            chunk = self.sock.recv(length - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def recv_message(self):
        """Read one message. Returns (msg_type, payload) or None at EOF."""
        head = self.recv_exact(HEADER_SIZE)
        if head is None:
            return None
        magic, msg_type, length, _seq, _src, _dst, _sq, _dq = unpack_header(head)
        if magic != MAGIC:
            raise ValueError(f"bad magic {magic:#x} in a reply")
        if length > MAX_PAYLOAD_LEN:
            raise ValueError(f"reply payload of {length} bytes is over the cap")
        payload = self.recv_exact(length) if length else b""
        if payload is None:
            return None
        return msg_type, payload

    def register(self, hostname=b"peer", port=9999, version=PROTOCOL_VERSION):
        """Register with a manager and return the parsed response.

        Pass version=None to register the way a pre-v4 node does.
        """
        self.send(
            MSG_REGISTER_NODE,
            register_payload(hostname, port, version=version),
            src_node=NODE_ID_AUTO,
        )
        message = self.recv_message()
        if message is None:
            raise ConnectionError("the manager closed the connection")
        msg_type, payload = message
        if msg_type != MSG_REGISTER_RESP:
            raise ValueError(f"expected REGISTER_RESP, got {msg_type}")
        node_id, num_nodes, result, ver = parse_register_resp(payload)
        if result == 0:
            self.node_id = node_id
        return node_id, num_nodes, result, ver

    def advertise_gids(self, gids):
        """Send this peer's GIDs, as a node does once it has its id."""
        if self.node_id is None:
            raise ValueError("register before advertising GIDs")
        self.send(
            MSG_NODE_GIDS,
            gids_payload(self.node_id, [gid_bytes(g) for g in gids]),
            src_node=self.node_id,
        )


class FakeManager:
    """A manager that answers registrations however a test needs.

    Used to put a real worker opposite a manager it should refuse, or one
    that answers slowly or not at all.
    """

    # The answer it gives is configured field by field, which is the point.
    # pylint: disable=too-many-instance-attributes

    def __init__(self, port=None, version=PROTOCOL_VERSION, result=0, reply=True):
        self.port = port or free_port()
        self.version = version
        self.result = result
        self.reply = reply
        self.connections = 0
        self._sock = None
        self._thread = None
        self._stop = threading.Event()

    def start(self):
        """Listen, and answer each connection on a thread."""
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", self.port))
        self._sock.listen(16)
        self._sock.settimeout(0.5)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        """Stop listening and join the thread."""
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=10)
            self._thread = None
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *_exc):
        self.stop()

    def _serve(self):
        while not self._stop.is_set():
            try:
                conn, _addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections += 1
            with conn:
                try:
                    conn.settimeout(2.0)
                    conn.recv(8192)
                    if self.reply:
                        body = struct.pack("!III", 1, 2, self.result & 0xFFFFFFFF)
                        if self.version is not None:
                            body += struct.pack("!I", self.version)
                        conn.sendall(
                            pack_header(MSG_REGISTER_RESP, len(body), src_node=0) + body
                        )
                    # Hold the connection open until the test is done, so
                    # the worker sees a manager that is there but wrong
                    # rather than one that hung up.
                    while not self._stop.wait(0.1):
                        pass
                except OSError:
                    continue


class MeshNode:
    """One node of a mesh: a server process and the guest behind it."""

    # The node is a bundle of the pieces a test reaches for.
    # pylint: disable=too-many-instance-attributes
    def __init__(self, index, server, guest_ip, log_path):
        self.index = index
        self.server = server
        self.guest_ip = guest_ip
        self.log_path = log_path
        self.client = None
        self.driver = None
        self.mem = None

    def attach(self, mem_size=32 << 20, timeout=30.0):
        """Connect a vfio-user client, map guest memory, and bring the LIF up."""
        # Imported here: a mesh test that only speaks the wire protocol
        # has no use for the driver, and this keeps the import graph of
        # the two halves of this module separate.
        from . import ionic, vfu  # pylint: disable=import-outside-toplevel

        self.client = vfu.VfioUserClient(self.server.socket_path).connect(
            timeout=timeout
        )
        self.mem = vfu.GuestMemory(size=mem_size)
        self.client.dma_map(self.mem)
        self.driver = ionic.IonicDriver(self.client, self.mem)
        self.driver.bringup()
        return self.driver

    def detach(self):
        """Drop the client and the guest memory, as a VM shutting down does.

        The memory has to be closed explicitly: its memfd is a raw
        descriptor, which nothing collects, so a churn loop that attaches
        and detaches would leak one per round -- in the suite that exists
        to count the server's descriptors.
        """
        if self.client is not None:
            self.client.close()
            self.client = None
        if self.mem is not None:
            self.mem.close()
            self.mem = None
        self.driver = None


class Mesh:
    """A manager and some workers, as real processes.

    Node 0 is the manager. Each node gets the guest IP 10.0.0.<index+1>,
    advertised in ERNIC_TCP_GUEST_GIDS, so a RoCE header addressed to
    that IP resolves to that node and to no other.
    """

    # pylint: disable-next=too-many-arguments,too-many-positional-arguments
    def __init__(
        self,
        binary,
        run_dir,
        size=3,
        port=None,
        log_level="info",
        health_interval=None,
        send_timeout_ms=None,
    ):
        self.binary = binary
        self.run_dir = run_dir
        self.size = size
        self.port = port or free_port()
        self.log_level = log_level
        # The backend's settings every node gets: the health-check
        # interval in seconds, and the send timeout in milliseconds, each
        # left at the server's default when None
        self.tcp_env = {}
        if health_interval is not None:
            self.tcp_env["ERNIC_TCP_HEALTH_INTERVAL"] = str(health_interval)
        if send_timeout_ms is not None:
            self.tcp_env["ERNIC_TCP_SEND_TIMEOUT_MS"] = str(send_timeout_ms)
        self.nodes = []
        os.makedirs(run_dir, exist_ok=True)

    @staticmethod
    def guest_ip(index):
        """The guest address of node @index."""
        return f"10.0.0.{index + 1}"

    def node_backend(self, index):
        """The --backend argument for node @index."""
        if index == 0:
            return f"tcp:manager:listen:{self.port}"
        return f"tcp:worker:127.0.0.1:{self.port}"

    def start(self, timeout=60.0):
        """Start every node, the manager first.

        A worker is not started until the manager is listening on its
        mesh port. The vfio-user socket is not that barrier: the server
        binds it in vfu_create_ctx() and only starts the backend, and so
        the listen, in ionic_device_init() afterwards. A worker that
        connects in between fails with ECONNREFUSED, and its first
        connection to the manager is not retried.
        """
        manager = self.start_node(0, timeout)
        if not manager.server.wait_for_log("TCP: Listening on port", timeout):
            raise ServerFailed(
                "the manager never listened on its mesh port: "
                f"{manager.server.tail_log()}"
            )
        for index in range(1, self.size):
            self.start_node(index, timeout)
        return self

    def start_node(self, index, timeout=60.0):
        """Start one node and wait for its vfio-user socket."""
        guest_ip = self.guest_ip(index)
        log_path = os.path.join(self.run_dir, f"node{index}.log")
        env = {"ERNIC_TCP_GUEST_GIDS": guest_ip, **self.tcp_env}
        node_server = Server(
            self.binary,
            os.path.join(self.run_dir, f"node{index}.sock"),
            backend=self.node_backend(index),
            log_path=log_path,
            log_level=self.log_level,
            env=env,
            extra_args=["--mac", f"72:6f:63:6d:00:{index:02x}"],
        )
        node_server.start(timeout=timeout)
        node = MeshNode(index, node_server, guest_ip, log_path)
        while len(self.nodes) <= index:
            self.nodes.append(None)
        self.nodes[index] = node
        return node

    def node_id(self, index):
        """The mesh node id node @index was given, or None.

        Ids go in the order workers register, which isn't the order they
        were started in: a server opens its vfio-user socket before its
        backend registers.
        """
        if index == 0:
            return 0
        found = re.search(
            r"Registered with manager, assigned node_id=(\d+)",
            self.nodes[index].server.log_text(),
        )
        return int(found.group(1)) if found else None

    def registrations(self):
        """How many workers the manager has registered since it started.

        Cumulative, because the manager's log is: a node that dies and
        restarts registers again and the count goes up rather than being
        replaced. That is why wait_formed() takes a mark. A worker that
        kept running and registers again under its own id isn't a new
        node, and isn't counted.
        """
        return self.nodes[0].server.log_text().count("Registered node")

    def wait_registrations(self, wanted, timeout=60.0):
        """Wait until the manager has registered @wanted workers in total.

        A total rather than an increment, because the manager's log is
        cumulative and the caller knows what it is waiting for: the whole
        mesh forming is ``size - 1``, and one node rejoining is whatever
        registrations() said before it was killed, plus one. Waiting for
        a fixed ``size - 1`` after the mesh has already formed asserts
        nothing -- the threshold is satisfied before the wait starts.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.registrations() >= wanted:
                return True
            time.sleep(0.1)
        return self.registrations() >= wanted

    def wait_formed(self, timeout=60.0):
        """Wait for every worker of a freshly started mesh to register."""
        return self.wait_registrations(self.size - 1, timeout)

    def wait_gids(self, timeout=60.0):
        """Wait until every node has learned every other node's GID.

        Each node logs what it learns, so this is the node's own view,
        not the manager's claim about it.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if all(self.node_knows_all_gids(n) for n in range(self.size)):
                return True
            time.sleep(0.1)
        return False

    def node_knows_all_gids(self, index):
        """Whether node @index has a GID entry for every other node."""
        text = self.nodes[index].server.log_text()
        for other in range(self.size):
            if other == index:
                continue
            if f"owns GID {gid_log_form(self.guest_ip(other))}" not in text:
                return False
        return True

    def stop(self, timeout=None, check=True):
        """Stop every node, and report the ones that misbehaved.

        A node that outlives the deadline or exits non-zero is collected
        rather than raised on at once, so one bad node doesn't hide the
        rest, and so every node is stopped either way.
        """
        problems = []
        for node in self.nodes:
            if node is None:
                continue
            node.detach()
            try:
                if timeout is None:
                    node.server.stop(check=check)
                else:
                    node.server.stop(timeout=timeout, check=check)
            except Exception as exc:  # pylint: disable=broad-except
                problems.append(f"node {node.index}: {exc}")
        return problems

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.stop(check=False)
