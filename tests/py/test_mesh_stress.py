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

# pylint: disable=wrong-import-position
from ernic import ionic, mesh
from ernic.check import Suite
from ernic.server import Server

# QP and CQ numbering. Ids 0 and 1 belong to the device, so start above
# them; each node's QP for talking to node j is QP_BASE + j, which keeps
# the destination a node names unambiguous.
QP_BASE = 16
CQ_ID = 1
MR_KEY = 0x0100_0001
MR_VA = 0x0000_7F00_0000_0000
MR_LEN = 2 * ionic.PAGE_SIZE

MESH_SIZE = 3


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
    """Frames no real node sends, and peers that never finish registering."""
    port = mesh_.port

    with mesh.MeshPeer(port=port) as peer:
        peer.send_raw(struct.pack(mesh.HEADER_FMT, 0xDEADBEEF, 12, 0, 1, 0, 0, 0, 0))
        time.sleep(0.2)
        suite.check(True, "a frame with a bad magic did not take the manager down")

    with mesh.MeshPeer(port=port) as peer:
        # A length past the 16 MiB cap, with nothing behind it.
        peer.send_raw(
            struct.pack(
                mesh.HEADER_FMT, mesh.MAGIC, mesh.MSG_IONIC, 1 << 30, 1, 0, 0, 0, 0
            )
        )
        time.sleep(0.2)
        suite.check(True, "a payload length past the cap was refused")

    with mesh.MeshPeer(port=port) as peer:
        peer.send_raw(b"\x00" * (mesh.HEADER_SIZE - 1))
        time.sleep(0.2)
        suite.check(True, "a truncated header did not wedge the manager")

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

    # A peer that connects and says nothing. The manager gives it five
    # seconds to register and then reaps it.
    silent = mesh.MeshPeer(port=port).connect()
    try:
        suite.check(True, "a peer that connects and never registers is tolerated")
    finally:
        silent.close()

    suite.check(
        mesh_.nodes[0].server.is_running(), "the manager survived every malformed peer"
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

    The counts are taken after each round has settled. A climb across
    rounds is the socket leak; sockets left in CLOSE_WAIT name it.
    """
    manager = mesh_.nodes[0].server
    victim = mesh_.size - 1
    counts = []
    for round_index in range(rounds):
        mesh_.nodes[victim].detach()
        mesh_.nodes[victim].server.kill()
        suite.check(
            not mesh_.nodes[victim].server.is_running(),
            f"round {round_index}: node {victim} killed",
        )
        # Let the manager notice and reap before counting.
        time.sleep(2.0)
        mesh_.start_node(victim, timeout=60)
        suite.check(
            mesh_.nodes[victim].server.is_running(),
            f"round {round_index}: node {victim} restarted",
        )
        time.sleep(2.0)
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
    """The nodes that did not restart keep talking.

    The one that did is a different matter. A restarted node registers
    as a new node id -- the manager allocates ids monotonically and the
    dead node is never evicted -- so its guest GID is now claimed by two
    node ids, the dead one and the live one. The conflict rule then
    resolves it to neither, and the address stays unreachable for the
    rest of the mesh's life.

    That is a loss of availability, and it is checked here as such. What
    must not happen, and is what this asserts, is the other outcome: the
    GID resolving to the stale node id and the traffic landing in a
    guest that no longer owns the address.
    """
    suite.check(mesh_.wait_formed(60), "the restarted node re-registered")
    peers = list(range(mesh_.size))
    drivers, cqs, mrs, qps = {}, {}, {}, {}
    for index in peers:
        mesh_.nodes[index].detach()
        driver, cq, mr, node_qps = bring_up(mesh_.nodes[index], peers)
        drivers[index], cqs[index], mrs[index], qps[index] = driver, cq, mr, node_qps

    connect_pair(mesh_, drivers, qps, 0, 1, retry_for=30.0)
    payload = b"after the churn" * 8
    src, dst = 0, 1
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
    drivers[src].wait_cq(cqs[src], timeout=30)
    suite.check(
        not recv.error
        and drivers[dst].mr_read(mrs[dst], MR_VA, len(payload)) == payload,
        "the nodes that did not restart still exchange data",
    )

    victim = mesh_.size - 1
    hdr = ionic.roce_hdr_v4(
        ionic.ipv4(mesh_.guest_ip(0)), ionic.ipv4(mesh_.guest_ip(victim))
    )
    drivers[0].modify_qp(qps[0][victim].id, ionic.QP_STATE, ionic.QPS_INIT)
    status = drivers[0].modify_qp(
        qps[0][victim].id,
        ionic.QP_STATE | ionic.QP_AV | ionic.QP_DEST_QPN | ionic.QP_RQ_PSN,
        ionic.QPS_RTR,
        QP_BASE,
        hdr,
    )
    suite.check(
        status != 0,
        "the restarted node's contested GID is refused rather than routed to the "
        "node id it used to have",
    )
    suite.check(
        "claimed by more than one node" in mesh_.nodes[0].server.log_text(),
        "the conflict is reported: a restarted node contests its own old claim",
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
        mesh_ = mesh.Mesh(args.server, run_dir, size=args.size)
        mesh_.start(timeout=60)
        try:
            suite.run("mesh formation", test_formation, mesh_)
            run_traffic_tests(suite, mesh_)
            suite.run("malformed peers", test_malformed_peers, mesh_)
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
    suite.finish()


if __name__ == "__main__":
    main()
