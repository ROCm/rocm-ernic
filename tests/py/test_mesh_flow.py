#!/usr/bin/env python3
"""Flow control on the TCP mesh, with real server processes and no VM.

A node's receive threads queue what its peers send for the main thread to
apply, up to a limit (DP_RX_MAX_BYTES in src/ionic_datapath_priv.h). At
the limit, a receive thread stops reading its peer until there is room,
and TCP passes that back to the peer, so a node that falls behind slows
its peers down rather than holding whatever they send:

  * Two nodes write to each other flat out, round after round, each with
    its inbound queue plugged by the other's SENDs to a QP with no
    receive posted, so both hold the other back at once. Neither may
    stall for good or give up on its connection to the other.
  * A peer floods a node with SENDs to a QP that never posts a receive,
    which the node would otherwise keep. The peer is slowed, not cut
    off, the node holds no more than its limit, and another node's
    traffic gets through meanwhile.

Every node is then held to a shutdown deadline, and to exiting with
status 0, which is how a sanitizer report from a server process reaches
this test.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import collections
import os
import re
import select
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# pylint: disable=wrong-import-position
from ernic import ionic, mesh
from ernic.check import Suite

# QP and CQ numbering, as in test_mesh_stress.py: each node's QP for
# talking to node j is QP_BASE + j.
QP_BASE = 16
CQ_ID = 1
MESH_SIZE = 3
# The pairs of nodes whose QPs are connected to each other
PAIRS = ((0, 1), (1, 2))
# Each node's MR, which a SEND during the flood lands in
MR_KEY = 0x0100_0001
MR_VA = 0x0000_7F00_0000_0000
MR_LEN = 2 * ionic.PAGE_SIZE

# What a node's inbound queue holds (DP_RX_MAX_BYTES)
RX_MAX_BYTES = 64 << 20
# enum ionic_status's RNR-retry-exceeded, which ernic.ionic doesn't name
STS_RNR_RETRY_EXCEEDED = 12

# Each round of the two-way test has nodes 1 and 2 each post a send
# queue's worth of WRITEs to the other at once: 60 MiB each way, past the
# 32 MiB a node queues for a connection. Each node's MR is a half to send
# from and a half the other's WRITEs land in. The MR's id, the key's low
# 24 bits, mustn't be MR_KEY's.
TWO_WAY_KEY = 0x0100_0002
TWO_WAY_VA = 0x0000_7F10_0000_0000
TWO_WAY_WRITE = 4 << 20
TWO_WAY_DEPTH = (1 << ionic.QP_DEPTH_LOG2) - 1
TWO_WAY_ROUNDS = 8
# WRITEs alone needn't fill a node's inbound queue: its main thread can
# keep up. So first each node plugs the other's, with a send queue's worth
# of SENDs to a QP there with no receive posted, which wait there: 75 MiB,
# past what the queue holds. Node a's plug QP for node b is PLUG_QP + b,
# on CQs of its own.
PLUG_QP = QP_BASE + 8
PLUG_SEND = 5 << 20
PLUG_SEND_CQ = CQ_ID + 1
PLUG_RECV_CQ = CQ_ID + 2
# What a node logs when SENDs waiting for a receive have kept another
# message out of its queue for long enough that it gives up on them
GAVE_UP = "filled the mesh's inbound queue"

# The SENDs a peer floods node 0 with, to a QP that never posts a
# receive: twice what the node's inbound queue holds.
FLOOD_QP = QP_BASE + 32
FLOOD_SEND = 4 << 20
FLOOD_SENDS = 2 * RX_MAX_BYTES // FLOOD_SEND
# The flood sends a heartbeat after every this many SENDs, so the node
# doesn't take the peer for silent however slowly it reads
FLOOD_HEARTBEAT_EVERY = 4


def bring_up(mesh_):
    """Attach a driver to every node, and connect the QPs of PAIRS.

    Returns (drivers, cqs, mrs, qps), each by node index; qps[a][b] is
    node a's QP for talking to node b.
    """
    drivers, cqs, mrs, qps = {}, {}, {}, {}
    for node in mesh_.nodes:
        index = node.index
        drivers[index] = node.attach()
        cqs[index] = drivers[index].create_cq(CQ_ID)
        mrs[index] = drivers[index].create_mr(MR_KEY, MR_VA, MR_LEN)
        qps[index] = {}
    for pair in PAIRS:
        for src, dst in (pair, pair[::-1]):
            qps[src][dst] = drivers[src].create_qp(
                QP_BASE + dst, ionic.QPT_RC, cqs[src], cqs[src]
            )
    for pair in PAIRS:
        for src, dst in (pair, pair[::-1]):
            drivers[src].connect_qp(
                qps[src][dst], QP_BASE + src, roce_hdr(mesh_, src, dst)
            )
    return drivers, cqs, mrs, qps


def roce_hdr(mesh_, src, dst):
    """The RoCE header template for node @src's guest to reach node @dst's."""
    return ionic.roce_hdr_v4(
        ionic.ipv4(mesh_.guest_ip(src)), ionic.ipv4(mesh_.guest_ip(dst))
    )


def receive_threads_ended(mesh_, nodes):
    """How many receive threads each of @nodes has logged ending."""
    return [
        mesh_.nodes[n].server.log_text().count("Receive thread exiting") for n in nodes
    ]


def plug(mesh_, drivers, pair):
    """Have each node of @pair plug the other's inbound queue.

    Returns, by node, its plug QP and the CQ its SENDs complete on.
    """
    plugs = {}
    for src, dst in (pair, pair[::-1]):
        send_cq = drivers[src].create_cq(PLUG_SEND_CQ)
        recv_cq = drivers[src].create_cq(PLUG_RECV_CQ)
        qp = drivers[src].create_qp(PLUG_QP + dst, ionic.QPT_RC, send_cq, recv_cq)
        plugs[src] = (qp, send_cq)
    for src, dst in (pair, pair[::-1]):
        drivers[src].connect_qp(plugs[src][0], PLUG_QP + src, roce_hdr(mesh_, src, dst))
    for src in pair:
        for _ in range(TWO_WAY_DEPTH):
            drivers[src].post_send(
                plugs[src][0],
                ionic.OP_SEND,
                flags=ionic.WQE_SIGNALLED,
                sge=[ionic.Sge(TWO_WAY_VA, PLUG_SEND, TWO_WAY_KEY)],
            )
    return plugs


def unplug(drivers, plugs):
    """Post the receives the plug's SENDs were waiting for, and collect them.

    Returns the statuses the plug's SENDs completed with, by node. Those
    a node gave up on fail with RNR-retry-exceeded; the rest are
    delivered.
    """
    statuses = {}
    for node, (qp, _send_cq) in plugs.items():
        for _ in range(TWO_WAY_DEPTH):
            drivers[node].post_recv(qp, [ionic.Sge(TWO_WAY_VA, PLUG_SEND, TWO_WAY_KEY)])
    for node, (_qp, send_cq) in plugs.items():
        statuses[node] = collections.Counter(
            drivers[node].wait_cq(send_cq, timeout=60).status
            for _ in range(TWO_WAY_DEPTH)
        )
    return statuses


def write_rounds(drivers, cqs, qps, pair):
    """Have the nodes of @pair WRITE to each other, TWO_WAY_ROUNDS times.

    Each round, each node posts a send queue's worth at once, and waits
    for them all. Returns how many completed in error.
    """
    errors = 0
    for _round in range(TWO_WAY_ROUNDS):
        for src, dst in (pair, pair[::-1]):
            for _ in range(TWO_WAY_DEPTH):
                drivers[src].post_send(
                    qps[src][dst],
                    ionic.OP_RDMA_WRITE,
                    flags=ionic.WQE_SIGNALLED,
                    remote_va=TWO_WAY_VA + TWO_WAY_WRITE,
                    rkey=TWO_WAY_KEY,
                    sge=[ionic.Sge(TWO_WAY_VA, TWO_WAY_WRITE, TWO_WAY_KEY)],
                )
        errors += sum(
            int(drivers[node].wait_cq(cqs[node], timeout=60).error)
            for node in pair
            for _ in range(TWO_WAY_DEPTH)
        )
    return errors


def test_two_way_writes(suite, mesh_, state):
    """Two nodes that write to each other flat out, each holding the other back.

    Nodes 1 and 2 each plug the other's inbound queue, then post as many
    4 MiB WRITEs to the other as a send queue holds, at once, round after
    round. With its queue full, each node stops reading the other, so
    both directions are held back at once, until each gives up on the
    SENDs plugging it. Neither may stall for good, and neither may give
    up on its connection to the other.
    """
    drivers = state[0]
    pair = PAIRS[1]
    ended = receive_threads_ended(mesh_, pair)
    gave_up = [mesh_.nodes[n].server.log_text().count(GAVE_UP) for n in pair]
    mrs = {}
    for node in pair:
        mrs[node] = drivers[node].create_mr(TWO_WAY_KEY, TWO_WAY_VA, 2 * TWO_WAY_WRITE)
        drivers[node].mr_write(mrs[node], TWO_WAY_VA, bytes([node]) * TWO_WAY_WRITE)
    plugs = plug(mesh_, drivers, pair)

    suite.check_eq(
        write_rounds(drivers, state[1], state[3], pair),
        0,
        "every WRITE of %d MiB each way completed without error",
        TWO_WAY_ROUNDS * TWO_WAY_DEPTH * (TWO_WAY_WRITE >> 20),
    )
    # Before the plug's SENDs land, on the first MiB of the same half
    for src, dst in (pair, pair[::-1]):
        landed = drivers[dst].mr_read(mrs[dst], TWO_WAY_VA + TWO_WAY_WRITE, 4096)
        suite.check(
            landed == bytes([src]) * 4096,
            "node %d's WRITEs landed on node %d",
            src,
            dst,
        )
    for node, mark in zip(pair, gave_up):
        suite.check(
            mesh_.nodes[node].server.wait_for_count(GAVE_UP, mark + 1, 30),
            "node %d's queue filled, held the other node back, and gave up on "
            "the SENDs filling it",
            node,
        )
    statuses = unplug(drivers, plugs)
    suite.check(
        all(set(s) <= {0, STS_RNR_RETRY_EXCEEDED} for s in statuses.values()),
        f"every SEND of the plug was delivered or failed with "
        f"RNR-retry-exceeded, once receives were posted ({statuses})",
    )
    suite.check_eq(
        receive_threads_ended(mesh_, pair),
        ended,
        "no receive thread on either node ended: neither gave up on a connection",
    )


def flood(peer, progress, errors):
    """Send FLOOD_SENDS SENDs to node 0's FLOOD_QP, counting in @progress.

    Runs on a thread of its own, since a node that is holding the peer
    back keeps sendall() waiting. What goes wrong goes in @errors.
    """
    length = FLOOD_SEND - mesh.IONIC_WIRE_SIZE
    try:
        for index in range(FLOOD_SENDS):
            body = mesh.ionic_send_payload(FLOOD_QP, length, index + 1)
            peer.send(mesh.MSG_IONIC, body, seq=index + 1, src_node=peer.node_id)
            progress.append(index)
            if (index + 1) % FLOOD_HEARTBEAT_EVERY == 0:
                peer.send(mesh.MSG_HEARTBEAT, src_node=peer.node_id)
    except OSError as exc:
        errors.append(exc)


def read_flood_acks(peer, acks, wanted, timeout):
    """Count the ACKs the peer is sent, by status, in @acks.

    Reads until @wanted(acks) holds, for up to @timeout seconds, and
    returns whether it came to hold. Everything else the node sends the
    peer (heartbeats, topology, GIDs) is skipped. A heartbeat goes out
    every second meanwhile, as from a worker, or the node would take the
    peer for silent if this took three health-check intervals.
    """
    deadline = time.monotonic() + timeout
    beat = 0.0
    while not wanted(acks):
        now = time.monotonic()
        if now >= deadline:
            return False
        if now - beat >= 1.0:
            peer.send(mesh.MSG_HEARTBEAT, src_node=peer.node_id)
            beat = now
        # Wait for a message to start, then read it whole
        readable, _w, _x = select.select([peer.sock], [], [], 1.0)
        if not readable:
            continue
        peer.sock.settimeout(10.0)
        message = peer.recv_message()
        if message is None:
            return False
        msg_type, payload = message
        status = mesh.ionic_ack_status(payload)
        if msg_type == mesh.MSG_IONIC and status is not None:
            acks[status] += 1
    return True


def send_during_flood(suite, state):
    """Node 1 sends node 0 a SEND, which has to arrive."""
    drivers, cqs, mrs, qps = state
    payload = b"past the flood " * 8
    drivers[1].mr_write(mrs[1], MR_VA, payload)
    drivers[0].mr_write(mrs[0], MR_VA, b"\0" * len(payload))
    drivers[0].post_recv(qps[0][1], [ionic.Sge(MR_VA, MR_LEN, MR_KEY)])
    drivers[1].post_send(
        qps[1][0],
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
    )
    recv = drivers[0].wait_cq(cqs[0], timeout=30)
    send = drivers[1].wait_cq(cqs[1], timeout=30)
    suite.check(
        not recv.error
        and not send.error
        and drivers[0].mr_read(mrs[0], MR_VA, len(payload)) == payload,
        "another node's SEND arrived while the peer flooded the node",
    )


def run_flood(suite, peer, state):
    """Flood node 0 from @peer, with a SEND from node 1 part way through.

    Returns whether the whole flood was sent.
    """
    progress, errors = [], []
    sender = threading.Thread(target=flood, args=(peer, progress, errors))
    sender.start()
    try:
        # Once the flood is past what the queue holds
        deadline = time.monotonic() + 30
        while len(progress) <= RX_MAX_BYTES // FLOOD_SEND and sender.is_alive():
            if time.monotonic() >= deadline:
                break
            time.sleep(0.01)
        send_during_flood(suite, state)
    finally:
        sender.join(60)
    return suite.check(
        not errors and not sender.is_alive(),
        f"the node took the whole flood ({errors or 'no error'})",
    )


def test_flooding_peer(suite, mesh_, state):
    """A peer that sends faster than a node applies is slowed, not kept.

    The peer floods the manager with SENDs to a QP that never posts a
    receive. Each waits for one, and holds up those behind it, so a node
    that kept whatever came would hold the whole flood, and answer it 5 s
    at a time. The node's inbound queue holds half of it, and its receive
    thread stops reading the peer while the queue is full, which TCP
    passes back as far as the peer's sendall(). Half a second on, the
    SENDs filling the queue fail with RNR-retry-exceeded, and the rest
    come in. Those are as many as the queue holds, so nothing is kept out,
    and they wait for a receive as usual, until the QP is destroyed.
    """
    driver, cq = state[0][0], state[1][0]
    manager = mesh_.nodes[0].server
    flood_qp = driver.create_qp(FLOOD_QP, ionic.QPT_RC, cq, cq)
    gave_up = manager.log_text().count("filled the mesh's inbound queue")
    held = RX_MAX_BYTES // FLOOD_SEND
    acks = collections.Counter()

    peer = mesh.MeshPeer(port=mesh_.port).connect()
    try:
        result = peer.register(hostname=b"flood")[2]
        suite.check_eq(result, 0, "the flooding peer registered")
        if not run_flood(suite, peer, state):
            # Still sending, maybe: another writer would garble its stream
            return
        suite.check(
            read_flood_acks(
                peer, acks, lambda a: a[STS_RNR_RETRY_EXCEEDED] >= held, 30
            ),
            f"the SENDs filling the queue failed with RNR-retry-exceeded "
            f"within 30 s, letting the rest in ({dict(acks)})",
        )
        driver.destroy_qp(flood_qp)
        suite.check(
            read_flood_acks(peer, acks, lambda a: sum(a.values()) >= FLOOD_SENDS, 30),
            f"every SEND of the flood was answered ({dict(acks)})",
        )
        suite.check(
            set(acks) <= {STS_RNR_RETRY_EXCEEDED, ionic.STS_REMOTE_ACC_ERR},
            f"with RNR-retry-exceeded, or, once the QP was gone, a remote "
            f"access error ({dict(acks)})",
        )
    finally:
        peer.close()

    # What the node said it held each time it gave up
    queued = re.findall(
        r"filled the mesh's inbound queue \((\d+) messages, (\d+) bytes\)",
        manager.log_text(),
    )[gave_up:]
    suite.check(queued, "the node logged giving up on the SENDs that filled it")
    suite.check(
        all(int(size) <= RX_MAX_BYTES for _count, size in queued),
        f"the node held no more than its queue's limit ({queued})",
    )
    suite.check(manager.is_running(), "the node is still running")


def main():
    """Stand up a mesh, push it past its limits, and hold every node to its deadline."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    suite = Suite("rocm-ernic TCP mesh flow control tests")
    with tempfile.TemporaryDirectory(prefix="ernic-mesh-flow-") as run_dir:
        # Started inside the try, as in test_mesh_stress.py: a node that fails to
        # start leaves the others for the finally to stop.
        mesh_ = mesh.Mesh(args.server, run_dir, size=MESH_SIZE)
        try:
            mesh_.start(timeout=60)
            # Every node knowing every GID means every worker has its id
            suite.check(mesh_.wait_gids(60), "the mesh formed")
            state = bring_up(mesh_)
            suite.run(
                "two nodes write to each other flat out",
                test_two_way_writes,
                mesh_,
                state,
            )
            suite.run("a peer that floods a node", test_flooding_peer, mesh_, state)
        finally:
            problems = "; ".join(mesh_.stop())
            suite.check(
                not problems,
                f"every node exited cleanly and in time ({problems or 'they did'})",
            )
    suite.finish()


if __name__ == "__main__":
    main()
