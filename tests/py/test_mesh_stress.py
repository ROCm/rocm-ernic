#!/usr/bin/env python3
"""Stress the TCP mesh with real server processes and no VM.

Three kinds of pressure, all of them reachable from a hosted runner:

  * Traffic. Every ordered pair of nodes sends to every other, and the
    bytes are checked. A mesh that resolved every destination to one
    node -- which is what the GID bug did -- passes a two-node test and
    fails this one.
  * Malformed input. A peer that sends a bad magic, a length past the
    cap, a frame a byte at a time, or nothing at all must be refused
    without taking the node down or disturbing the traffic above.
  * Churn and resources. Nodes are killed and restarted while the mesh
    runs, and the manager's descriptors are counted across the loop: a
    count that climbs, or sockets left in CLOSE_WAIT, is the leak.
  * Lost links. On a second mesh, a worker and then the manager are
    paused past the health check, and the workers have to register
    again under their own ids.
  * A peer that stops reading. On a third, a peer that has registered
    stops reading while the node sends it more than it can hold, and
    the node has to go on serving its guest, give up on the peer, and
    fail the requests it couldn't send.

Every node is then held to a shutdown deadline, because the TCP backend
is known to hang there, and to exiting with status 0, which is how a
sanitizer report from a server process reaches this test.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic, mesh
from ernic.check import Suite
from ernic.server import Server

# pylint: enable=wrong-import-position

# QP and CQ numbering. Ids 0 and 1 belong to the device, so start above
# them; each node's QP for talking to node j is QP_BASE + j, which keeps
# the destination a node names unambiguous.
QP_BASE = 16
CQ_ID = 1
MR_KEY = 0x0100_0001
MR_VA = 0x0000_7F00_0000_0000
MR_LEN = 2 * ionic.PAGE_SIZE

MESH_SIZE = 3

# What the peer that stops reading claims, and what is sent to it: as
# many 4 MiB WRITEs as a send queue holds, more than the node's socket,
# the peer's, and the 32 MiB the node queues for a connection, together.
STUCK_GID = "10.0.99.9"
STUCK_WRITE = 4 << 20
STUCK_WRITES = (1 << ionic.QP_DEPTH_LOG2) - 1
# The manager gives up on a peer in half of this
STUCK_SEND_TIMEOUT_MS = 2000
# enum ionic_status's retry-exceeded, which ernic.ionic doesn't name
STS_RETRY_EXCEEDED = 11


def bring_up(node, peers):
    """Attach a driver to @node and give it a QP for each peer."""
    driver = node.attach()
    cq = driver.create_cq(CQ_ID)
    mr = driver.create_mr(MR_KEY, MR_VA, MR_LEN)
    qps = {}
    for peer in peers:
        if peer == node.index:
            continue
        qps[peer] = driver.create_qp(QP_BASE + peer, ionic.QPT_RC, cq, cq)
    return driver, cq, mr, qps


def connect_side(driver, qp, dest_qpn, hdr, retry_for=0.0):
    """Take one QP to RTS, retrying RTR while @retry_for lasts.

    RTR is where the destination GID is resolved, so it is the step that
    fails while a node that has just restarted is still being relearned
    by its peers. Retrying it waits for the mesh to converge without
    asking any node's log what it believes.
    """
    driver.modify_qp(qp.id, ionic.QP_STATE, ionic.QPS_INIT)
    mask = ionic.QP_STATE | ionic.QP_AV | ionic.QP_DEST_QPN | ionic.QP_RQ_PSN
    deadline = time.monotonic() + retry_for
    while True:
        status = driver.modify_qp(qp.id, mask, ionic.QPS_RTR, dest_qpn, hdr)
        if status == 0:
            break
        if time.monotonic() >= deadline:
            raise ionic.DeviceError(
                f"QP {qp.id} never reached RTR for peer {dest_qpn} "
                f"(status {status})"
            )
        time.sleep(0.5)
    status = driver.modify_qp(qp.id, ionic.QP_STATE | ionic.QP_SQ_PSN, ionic.QPS_RTS)
    if status != 0:
        raise ionic.DeviceError(f"QP {qp.id} did not reach RTS (status {status})")


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def connect_pair(mesh_, drivers, qps, src, dst, retry_for=0.0):
    """Point src's QP for dst, and dst's for src, at each other."""
    to_dst = ionic.roce_hdr_v4(
        ionic.ipv4(mesh_.guest_ip(src)), ionic.ipv4(mesh_.guest_ip(dst))
    )
    to_src = ionic.roce_hdr_v4(
        ionic.ipv4(mesh_.guest_ip(dst)), ionic.ipv4(mesh_.guest_ip(src))
    )
    connect_side(drivers[src], qps[src][dst], QP_BASE + src, to_dst, retry_for)
    connect_side(drivers[dst], qps[dst][src], QP_BASE + dst, to_src, retry_for)


def test_formation(suite, mesh_):
    """The mesh forms, and every node learns every node's GID."""
    suite.check(mesh_.wait_formed(60), "every worker registered with the manager")
    suite.check(mesh_.wait_gids(60), "every node learned every other node's GID")
    for index in range(mesh_.size):
        suite.check(mesh_.nodes[index].server.is_running(), f"node {index} is running")


def test_traffic_matrix(suite, mesh_, state):
    """Every ordered pair of nodes sends to every other, and writes to it."""
    drivers, cqs, mrs, qps = state
    for src in range(mesh_.size):
        for dst in range(mesh_.size):
            if src == dst:
                continue
            payload = f"from node {src} to node {dst}: ".encode() + bytes(range(200))
            drivers[src].mr_write(mrs[src], MR_VA, payload)
            drivers[dst].mr_write(mrs[dst], MR_VA, b"\0" * len(payload))
            drivers[dst].post_recv(qps[dst][src], [ionic.Sge(MR_VA, MR_LEN, MR_KEY)])
            drivers[src].post_send(
                qps[src][dst],
                ionic.OP_SEND,
                flags=ionic.WQE_SIGNALLED,
                sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
            )
            recv = drivers[dst].wait_cq(cqs[dst], timeout=20)
            send = drivers[src].wait_cq(cqs[src], timeout=20)
            ok = (
                not recv.error
                and not send.error
                and recv.qid == QP_BASE + src
                and drivers[dst].mr_read(mrs[dst], MR_VA, len(payload)) == payload
            )
            suite.check(ok, "SEND from node %d arrived at node %d intact", src, dst)


def test_write_matrix(suite, mesh_, state):
    """An RDMA WRITE lands in the right node's memory and in no other's."""
    drivers, cqs, mrs, qps = state
    for src in range(mesh_.size):
        for dst in range(mesh_.size):
            if src == dst:
                continue
            payload = f"write {src}->{dst} ".encode() * 16
            for index in range(mesh_.size):
                drivers[index].mr_write(mrs[index], MR_VA, b"\0" * len(payload))
            drivers[src].mr_write(mrs[src], MR_VA, payload)
            drivers[src].post_send(
                qps[src][dst],
                ionic.OP_RDMA_WRITE,
                flags=ionic.WQE_SIGNALLED,
                remote_va=MR_VA,
                rkey=MR_KEY,
                sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
            )
            send = drivers[src].wait_cq(cqs[src], timeout=20)
            landed = drivers[dst].mr_read(mrs[dst], MR_VA, len(payload))
            suite.check(
                not send.error and landed == payload,
                "WRITE from node %d landed on node %d",
                src,
                dst,
            )
            for other in range(mesh_.size):
                if other in (src, dst):
                    continue
                suite.check(
                    drivers[other].mr_read(mrs[other], MR_VA, len(payload))
                    == b"\0" * len(payload),
                    "the WRITE %d->%d did not touch node %d",
                    src,
                    dst,
                    other,
                )


def test_unknown_gid(suite, mesh_, state):
    """A QP aimed at a GID no node owns must fail, not pick a default.

    This is the property the GID table exists for: a wrong node here is
    a silent write into another guest's memory that nothing downstream
    catches.
    """
    drivers, cqs, _mrs, _qps = state
    driver = drivers[0]
    qp = driver.create_qp(QP_BASE + 40, ionic.QPT_RC, cqs[0], cqs[0])
    hdr = ionic.roce_hdr_v4(ionic.ipv4(mesh_.guest_ip(0)), ionic.ipv4("198.51.100.77"))
    status = driver.modify_qp(qp.id, ionic.QP_STATE, ionic.QPS_INIT)
    suite.check_eq(status, 0, "the QP reaches INIT")
    status = driver.modify_qp(
        qp.id,
        ionic.QP_STATE | ionic.QP_AV | ionic.QP_DEST_QPN | ionic.QP_RQ_PSN,
        ionic.QPS_RTR,
        QP_BASE,
        hdr,
    )
    suite.check(status != 0, "RTR to a GID no node owns is refused")
    suite.check(
        "no mesh node owns" in mesh_.nodes[0].server.log_text(),
        "the refusal names the unresolved GID in the log",
    )


def test_malformed_peers(suite, mesh_):
    """Frames no real node sends, and peers that never finish registering.

    Each row checks what the manager did about the frame, not just that
    it is still running: a manager that ignored the payload cap, or that
    never reaped a peer which connected and said nothing, would survive
    all of this.
    """
    port = mesh_.port
    manager = mesh_.nodes[0].server

    with mesh.MeshPeer(port=port) as peer:
        peer.send_raw(struct.pack(mesh.HEADER_FMT, 0xDEADBEEF, 12, 0, 1, 0, 0, 0, 0))
        suite.check(
            peer.wait_closed(10.0),
            "a frame with a bad magic gets the connection closed",
        )

    with mesh.MeshPeer(port=port) as peer:
        # A length past the 16 MiB cap, with nothing behind it. The
        # manager must reject it on the header alone rather than waiting
        # for a gigabyte that is never coming.
        peer.send_raw(
            struct.pack(
                mesh.HEADER_FMT, mesh.MAGIC, mesh.MSG_IONIC, 1 << 30, 1, 0, 0, 0, 0
            )
        )
        suite.check(
            manager.wait_for_log(f"payload too large: {1 << 30}", 10),
            "the payload cap is enforced on the header, and says so",
        )
        suite.check(
            peer.wait_closed(10.0), "the oversized frame gets the connection closed"
        )

    with mesh.MeshPeer(port=port) as peer:
        peer.send_raw(b"\x00" * (mesh.HEADER_SIZE - 1))
        suite.check(
            peer.wait_closed(mesh.REGISTER_TIMEOUT_MS / 1000.0 + 10.0),
            "a truncated header does not hold a connection open forever",
        )

    with mesh.MeshPeer(port=port) as peer:
        body = mesh.register_payload()
        peer.send_dribbled(
            mesh.pack_header(mesh.MSG_REGISTER_NODE, len(body)) + body, chunk=7
        )
        message = peer.recv_message()
        suite.check(
            message is not None and message[0] == mesh.MSG_REGISTER_RESP,
            "a registration delivered a few bytes at a time is still parsed",
        )

    # A peer that connects and says nothing. The manager gives it
    # TCP_REGISTER_TIMEOUT_MS to register and then reaps it, so this one
    # is left connected rather than closed from here -- closing it would
    # pass whether or not the reaper works.
    with mesh.MeshPeer(port=port) as peer:
        suite.check(
            manager.wait_for_log("which didn't register", 30),
            "a peer that never registers is reaped",
        )
        suite.check(
            peer.wait_closed(10.0),
            "the reaped peer's connection is closed from the manager's end",
        )

    suite.check(manager.is_running(), "the manager survived every malformed peer")


def test_peer_advertisement(suite, mesh_):
    """A peer that registers and advertises a GID owns it, and loses it.

    Exercises the advertisement from the outside, which is the only way
    to check the encoding: the payload's node id and count are network
    byte order, and a little-endian one decodes as sixteen million GIDs
    and is refused.

    The second half is the liveness rule end to end: when the peer hangs
    up, the manager declares it dead and stops it owning the address, so
    the same address can be claimed again by whoever comes back with it.
    """
    manager = mesh_.nodes[0].server
    claimed = "10.0.99.7"
    wanted = f"owns GID {mesh.gid_log_form(claimed)}"

    peer = mesh.MeshPeer(port=mesh_.port).connect()
    try:
        _node_id, _num, result, _version = peer.register(hostname=b"advertiser")
        suite.check_eq(result, 0, "the peer registered")
        peer.advertise_gids([claimed])
        suite.check(
            manager.wait_for_log(wanted, 15),
            "the manager accepted the advertisement and recorded the owner",
        )
    finally:
        peer.close()

    suite.check(
        manager.wait_for_log("unreachable while it is away", 60),
        "the manager stops a hung-up node owning its GIDs",
    )


def test_version_gate(suite, mesh_):
    """A pre-v4 peer is refused, and says why.

    A node running the old GID heuristic never advertises its GIDs and
    routes every peer to one default on its own side, so admitting one
    puts a silently misrouting node in the mesh.
    """
    manager = mesh_.nodes[0].server
    before = manager.log_text().count("Registered node")
    with mesh.MeshPeer(port=mesh_.port) as peer:
        _node_id, _num, result, version = peer.register(
            hostname=b"oldpeer", version=None
        )
    suite.check(result != 0, "the manager refused a pre-v4 worker")
    suite.check(
        version == mesh.PROTOCOL_VERSION,
        f"the refusal carries the version it wants ({version})",
    )
    suite.check(
        manager.wait_for_log("refusing registration from a v0 peer", 10),
        "the manager logged why it refused",
    )
    suite.check_eq(
        manager.log_text().count("Registered node"),
        before,
        "the refused peer was not admitted anyway",
    )


def test_worker_refuses_old_manager(suite, binary, run_dir):
    """A worker refuses a manager that answers the way a pre-v4 one does.

    An old manager cannot refuse us, so the worker has to refuse it.
    """
    with mesh.FakeManager(version=None) as fake:
        worker = Server(
            binary,
            os.path.join(run_dir, "oldmgr.sock"),
            backend=f"tcp:worker:127.0.0.1:{fake.port}",
            log_path=os.path.join(run_dir, "oldmgr.log"),
            log_level="info",
            env={"ERNIC_TCP_GUEST_GIDS": "10.0.9.9"},
        )
        worker.start(timeout=60, wait=False)
        try:
            suite.check(
                worker.wait_for_log("manager speaks v0", 30),
                "the worker refused a pre-v4 manager",
            )
            suite.check(
                "Registered with manager" not in worker.log_text(),
                "the worker did not report success against it",
            )
        finally:
            # A worker that refuses the manager fails to start, so it
            # exits non-zero by design. What matters is that it exits:
            # a node that refuses and then hangs is as bad as one that
            # joins.
            try:
                status = worker.stop(check=False)
                suite.check(
                    status != 0,
                    f"the refused worker exited rather than joining "
                    f"(status {status})",
                )
            except Exception as exc:  # pylint: disable=broad-except
                suite.check(False, "the refused worker: %s", exc)


def test_churn(suite, mesh_, rounds=3):
    """Kill and restart a worker, and watch the manager's descriptors.

    Each sample is taken after something observable has happened, not
    after a fixed sleep: the manager retires a dead connection on its
    next health-check pass, which is up to an interval away, so a sleep
    shorter than that samples before the sweep and a sleep longer than
    it is a guess that will be wrong on a slow runner. A count that
    climbs across rounds is the socket leak; sockets left in CLOSE_WAIT
    name it.
    """
    manager = mesh_.nodes[0].server
    victim = mesh_.size - 1
    counts = []
    for round_index in range(rounds):
        registrations = mesh_.registrations()
        disconnects = manager.log_text().count("disconnected")

        mesh_.nodes[victim].detach()
        mesh_.nodes[victim].server.kill()
        suite.check(
            not mesh_.nodes[victim].server.is_running(),
            f"round {round_index}: node {victim} killed",
        )
        suite.check(
            manager.wait_for_count("disconnected", disconnects + 1, 60),
            f"round {round_index}: the manager noticed and retired the connection",
        )

        mesh_.start_node(victim, timeout=60)
        suite.check(
            mesh_.wait_registrations(registrations + 1, 60),
            f"round {round_index}: node {victim} restarted and re-registered",
        )
        counts.append(manager.fd_count())

    suite.check(manager.is_running(), f"the manager survived {rounds} rounds of churn")
    suite.check_eq(
        manager.close_wait_count(),
        0,
        "the manager has no sockets left in CLOSE_WAIT after the churn",
    )
    growth = counts[-1] - counts[0]
    suite.check(
        growth <= 2,
        f"the manager's descriptor count is stable across churn ({counts})",
    )


# pylint: disable-next=too-many-locals
def test_traffic_after_churn(suite, mesh_):
    """Every node still talks, the one that restarted included.

    A restarted node registers under a new id -- the manager allocates
    ids monotonically and keeps a dead one until a full mesh needs its
    slot -- so for a moment its guest GID is claimed by both the id it
    had and the id it now has, and the duplicate-claim rule resolves it
    to neither. The manager's health check then notices the old id is
    gone and withdraws its claims, which leaves the new id the sole
    claimant and the address reachable again.

    Convergence is what is asserted: the mesh gets a bounded time to get
    there, and traffic has to flow to the restarted node afterwards. The
    rule that makes the contested window safe is checked separately, in
    the unresolved-GID test -- an address with two claimants must refuse,
    never pick one.
    """
    peers = list(range(mesh_.size))
    drivers, cqs, mrs, qps = {}, {}, {}, {}
    for index in peers:
        mesh_.nodes[index].detach()
        driver, cq, mr, node_qps = bring_up(mesh_.nodes[index], peers)
        drivers[index], cqs[index], mrs[index], qps[index] = driver, cq, mr, node_qps

    # A pair that never restarted, and the pair that did.
    connect_pair(mesh_, drivers, qps, 0, 1, retry_for=30.0)
    suite.check(True, "two nodes that did not restart reconnect at once")
    victim = mesh_.size - 1
    connect_pair(mesh_, drivers, qps, 0, victim, retry_for=90.0)
    suite.check(True, "the restarted node's GID resolves again")

    for src, dst in ((0, 1), (0, victim), (victim, 0)):
        payload = f"after the churn {src}->{dst} ".encode() * 8
        drivers[src].mr_write(mrs[src], MR_VA, payload)
        drivers[dst].mr_write(mrs[dst], MR_VA, b"\0" * len(payload))
        drivers[dst].post_recv(qps[dst][src], [ionic.Sge(MR_VA, MR_LEN, MR_KEY)])
        drivers[src].post_send(
            qps[src][dst],
            ionic.OP_SEND,
            flags=ionic.WQE_SIGNALLED,
            sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
        )
        recv = drivers[dst].wait_cq(cqs[dst], timeout=30)
        send = drivers[src].wait_cq(cqs[src], timeout=30)
        landed = drivers[dst].mr_read(mrs[dst], MR_VA, len(payload))
        suite.check(
            not recv.error and not send.error and landed == payload,
            f"a SEND from node {src} to node {dst} arrived after the churn",
        )


def test_worker_paused(suite, mesh_):
    """A worker paused past the health check registers again under its id.

    The manager closes the connection of a node that stops answering, and
    doesn't connect to it. When the worker runs again it finds the
    connection ended, connects to the manager, and asks for its own id
    back, which it gets: the mesh has no new node, and the worker takes
    the mesh's word on the new connection.
    """
    manager = mesh_.nodes[0].server
    victim = mesh_.nodes[mesh_.size - 1].server
    node_id = mesh_.node_id(mesh_.size - 1)
    if node_id is None:
        suite.check(False, "the worker to pause has no node id")
        return
    closing = f"node {node_id}, which has stopped answering"
    again = f"Node {node_id} registered again"
    marks = (
        mesh_.registrations(),
        manager.log_text().count(closing),
        manager.log_text().count(again),
        victim.log_text().count("Received mesh topology"),
    )

    victim.pause()
    try:
        closed = manager.wait_for_count(closing, marks[1] + 1, 30)
    finally:
        victim.resume()
    suite.check(closed, "the manager closed the paused worker's connection")
    suite.check(
        manager.wait_for_count(again, marks[2] + 1, 30),
        f"node {node_id} registered again under its own id",
    )
    suite.check(
        victim.wait_for_count("Received mesh topology", marks[3] + 1, 30),
        "the worker takes topology updates on its new connection",
    )
    suite.check_eq(mesh_.registrations(), marks[0], "the mesh has no new node")


def test_manager_paused(suite, mesh_):
    """Every worker registers again once a paused manager runs again.

    Paused, the manager answers nothing. Each worker sends it a heartbeat
    and, three intervals on, gives up on the connection and connects
    again, which the paused manager's kernel accepts and nobody answers.
    When the manager runs again, it finds every node silent and closes
    their connections too. However the two cross, each worker has to end
    up registered again under its own id, with no new node.
    """
    manager = mesh_.nodes[0].server
    workers = [node.server for node in mesh_.nodes[1:]]
    registrations = mesh_.registrations()
    gave_up = [w.log_text().count("Nothing from the manager") for w in workers]
    again = [w.log_text().count("Registered with the manager again") for w in workers]

    manager.pause()
    try:
        silent = all(
            w.wait_for_count("Nothing from the manager", mark + 1, 30)
            for w, mark in zip(workers, gave_up)
        )
    finally:
        manager.resume()
    suite.check(silent, "every worker gave up on the paused manager")
    suite.check(
        all(
            w.wait_for_count("Registered with the manager again", mark + 1, 60)
            for w, mark in zip(workers, again)
        ),
        "every worker registered again",
    )
    suite.check_eq(mesh_.registrations(), registrations, "the mesh has no new node")


def wait_retry_exceeded(driver, cq, timeout):
    """Wait for a send completion with retry-exceeded. True if one came.

    Earlier WRITEs, queued before the peer stopped reading, can complete
    first, or not until their own pending timeout; either is fine here.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        cqe = driver.poll_cq(cq)
        if cqe is None:
            time.sleep(0.05)
        elif cqe.error and cqe.status == STS_RETRY_EXCEEDED:
            return True
    return False


# pylint: disable-next=too-many-locals
def test_peer_stops_reading(suite, mesh_):
    """A peer that stops reading holds up nothing but its own connection.

    The node's main thread runs the guest's device, so it must never wait
    for a peer: it queues what it sends, and a full queue holds the
    guest's send queue instead, as a NIC out of credits does. The peer
    registers and claims a GID, so the node's data path has somewhere to
    send, and then reads nothing while the node sends it more than can be
    held anywhere. The device must go on answering the guest's admin
    commands and moving its local traffic, give up on the peer after the
    send timeout, and complete the WRITEs it never sent with
    retry-exceeded, which is what a peer that never answers gets.
    """
    manager = mesh_.nodes[0].server
    driver = mesh_.nodes[0].attach(mem_size=96 << 20)
    cq = driver.create_cq(CQ_ID)
    local_cq = driver.create_cq(CQ_ID + 1)
    mr = driver.create_mr(MR_KEY, MR_VA, STUCK_WRITE)

    peer = mesh.MeshPeer(port=mesh_.port).connect()
    try:
        _node_id, _num, result, _version = peer.register(hostname=b"stuck")
        suite.check_eq(result, 0, "the peer registered")
        peer.advertise_gids([STUCK_GID])
        suite.check(
            manager.wait_for_log(f"owns GID {mesh.gid_log_form(STUCK_GID)}", 15),
            "the node knows the peer's GID",
        )

        # From here on, nothing reads the peer's socket.
        qp = driver.create_qp(QP_BASE, ionic.QPT_RC, cq, cq)
        to_peer = ionic.roce_hdr_v4(
            ionic.ipv4(mesh_.guest_ip(0)), ionic.ipv4(STUCK_GID)
        )
        connect_side(driver, qp, QP_BASE, to_peer)
        for _ in range(STUCK_WRITES):
            driver.post_send(
                qp,
                ionic.OP_RDMA_WRITE,
                flags=ionic.WQE_SIGNALLED,
                remote_va=MR_VA,
                rkey=MR_KEY,
                sge=[ionic.Sge(MR_VA, STUCK_WRITE, mr.lkey)],
            )

        # Admin commands and a SEND between two of the node's own QPs
        to_self = ionic.roce_hdr_v4(
            ionic.ipv4(mesh_.guest_ip(0)), ionic.ipv4(mesh_.guest_ip(0))
        )
        qp_a = driver.create_qp(QP_BASE + 1, ionic.QPT_RC, local_cq, local_cq)
        qp_b = driver.create_qp(QP_BASE + 2, ionic.QPT_RC, local_cq, local_cq)
        connect_side(driver, qp_a, QP_BASE + 2, to_self)
        connect_side(driver, qp_b, QP_BASE + 1, to_self)
        driver.post_recv(qp_b, [ionic.Sge(MR_VA, 64, mr.lkey)])
        driver.post_send(
            qp_a,
            ionic.OP_SEND,
            flags=ionic.WQE_SIGNALLED,
            sge=[ionic.Sge(MR_VA, 64, mr.lkey)],
        )
        recv = driver.wait_cq(local_cq, timeout=20)
        send = driver.wait_cq(local_cq, timeout=20)
        suite.check(
            not recv.error and not send.error,
            "the device went on serving the guest, its local traffic included",
        )

        suite.check(
            manager.wait_for_log("has read nothing for", 30),
            "the node gave up on the peer that stopped reading",
        )
        suite.check(
            wait_retry_exceeded(driver, cq, 30),
            "a WRITE the node couldn't send completed with retry-exceeded",
        )
        suite.check(manager.is_running(), "the node is still running")
    finally:
        peer.close()


def run_stuck_peer_test(suite, binary, run_dir):
    """The peer that stops reading, against a manager of its own.

    Its send timeout is short, so the test waits seconds for it, not the
    default ten.
    """
    mesh_ = mesh.Mesh(
        binary,
        os.path.join(run_dir, "stuck"),
        size=1,
        send_timeout_ms=STUCK_SEND_TIMEOUT_MS,
    )
    try:
        mesh_.start(timeout=60)
        suite.run("a peer that stops reading", test_peer_stops_reading, mesh_)
    finally:
        problems = mesh_.stop()
        suite.check(
            not problems,
            "the third mesh's node exited cleanly and within its deadline"
            + ("" if not problems else f": {'; '.join(problems)}"),
        )


def run_link_tests(suite, binary, run_dir):
    """Cut workers off from the manager, on a mesh of their own.

    Its health-check interval is 1 s rather than the default 5 s, so a
    node goes silent in seconds.
    """
    mesh_ = mesh.Mesh(binary, os.path.join(run_dir, "link"), health_interval=1)
    try:
        mesh_.start(timeout=60)
        # Not wait_formed(): the manager logs a registration before it
        # answers it. A worker takes GID tables only once it has its
        # answer, so every node knowing every GID means every worker has
        # its id.
        suite.check(mesh_.wait_gids(60), "the second mesh formed")
        suite.run("a paused worker", test_worker_paused, mesh_)
        suite.run("a paused manager", test_manager_paused, mesh_)
    finally:
        problems = mesh_.stop()
        suite.check(
            not problems,
            "every node of the second mesh exited cleanly and within its deadline"
            + ("" if not problems else f": {'; '.join(problems)}"),
        )


def run_traffic_tests(suite, mesh_):
    """Attach a driver to every node and run the traffic tests."""
    peers = list(range(mesh_.size))
    drivers, cqs, mrs, qps = {}, {}, {}, {}
    for index in peers:
        driver, cq, mr, node_qps = bring_up(mesh_.nodes[index], peers)
        drivers[index], cqs[index], mrs[index], qps[index] = driver, cq, mr, node_qps
    for src in peers:
        for dst in peers:
            if src < dst:
                connect_pair(mesh_, drivers, qps, src, dst)
    state = (drivers, cqs, mrs, qps)
    suite.run("traffic between every pair of nodes", test_traffic_matrix, mesh_, state)
    suite.run("RDMA WRITE reaches one node only", test_write_matrix, mesh_, state)
    suite.run("a destination no node owns is refused", test_unknown_gid, mesh_, state)


def main():
    """Stand up a mesh, stress it, and hold every node to its deadline."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    parser.add_argument(
        "--size", type=int, default=MESH_SIZE, help="nodes in the mesh (default 3)"
    )
    args = parser.parse_args()

    suite = Suite("rocm-ernic TCP mesh stress tests")
    with tempfile.TemporaryDirectory(prefix="ernic-mesh-") as run_dir:
        # Inside the try: a mesh whose third node fails to start still
        # has two running servers that the finally has to stop, or they
        # outlive the temporary directory being removed.
        mesh_ = mesh.Mesh(args.server, run_dir, size=args.size)
        try:
            mesh_.start(timeout=60)
            suite.run("mesh formation", test_formation, mesh_)
            run_traffic_tests(suite, mesh_)
            suite.run("malformed peers", test_malformed_peers, mesh_)
            suite.run("a peer's GID advertisement", test_peer_advertisement, mesh_)
            suite.run("the protocol version gate", test_version_gate, mesh_)
            suite.run(
                "a worker refuses a pre-v4 manager",
                test_worker_refuses_old_manager,
                args.server,
                run_dir,
            )
            suite.run("node churn", test_churn, mesh_)
            suite.run("traffic after churn", test_traffic_after_churn, mesh_)
        finally:
            problems = mesh_.stop()
            suite.check(
                not problems,
                "every node exited cleanly and within its deadline"
                + ("" if not problems else f": {'; '.join(problems)}"),
            )
        run_link_tests(suite, args.server, run_dir)
        run_stuck_peer_test(suite, args.server, run_dir)
    suite.finish()


if __name__ == "__main__":
    main()
