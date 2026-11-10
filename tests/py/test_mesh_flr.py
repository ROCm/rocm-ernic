#!/usr/bin/env python3
"""A function-level reset on one node of a two-node mesh, and a new connection.

Two servers, a manager and a worker, each with a guest attached, connect
an RC QP each to the other and pass traffic both ways. Then one node's
guest starts an FLR, as Linux's pcie_flr() does, and:

  * The peer's QP from before the reset, still pointed at the reset node,
    is refused: an RDMA WRITE finds no region, a SEND no QP, and the
    WRITE doesn't land in the guest memory the reset gave back. This is
    checked before the reset node's driver loads again, since loading
    resets the device itself.
  * The peer destroys that QP and makes a new one, the reset node's
    driver loads and makes its CQ, MR and QP again under their old ids,
    and the two connect at once, with no wait for the mesh, and pass
    traffic both ways.
  * The reset node's server is still running and still in the mesh: the
    manager has registered nobody new, both nodes keep their ids, and the
    connection between them was never dropped, unlike a node that
    restarts, which registers under a new id and whose GID resolves to
    nobody until the mesh converges.

The worker is reset first, then the manager, which also runs the mesh.
Both nodes are then held to a shutdown deadline, and to exiting with
status 0, which is how a sanitizer report from a server process reaches
this test.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic, mesh, pci
from ernic.check import Suite

# Each node's CQ and MR are the stress suite's. A node makes its first QP
# as QP_BASE, and each QP that replaces one takes the next id.
from test_mesh_stress import CQ_ID, MR_KEY, MR_LEN, MR_VA, QP_BASE, post_write

# pylint: enable=wrong-import-position

# Node 0 is the manager and node 1 the worker: the worker is reset first
RESET_ORDER = (1, 0)
# How long a completion may take to cross the mesh
COMPLETION_TIMEOUT = 30


class Side:
    """One node's end of the connection: its driver, CQ, MR and QP."""

    def __init__(self, node):
        self.node = node
        self.driver = node.attach()
        self.cq = None
        self.mr = None
        self.qp = None
        self.next_qp_id = QP_BASE + 1
        self.make_resources(QP_BASE)

    def make_resources(self, qp_id):
        """Create the CQ, the MR and QP @qp_id, as a driver does once it loads.

        After a reset they come back under the ids they had, which the
        device refuses if the reset left the old ones behind.
        """
        self.cq = self.driver.create_cq(CQ_ID)
        self.mr = self.driver.create_mr(MR_KEY, MR_VA, MR_LEN)
        self.qp = self.driver.create_qp(qp_id, ionic.QPT_RC, self.cq, self.cq)

    def new_qp(self):
        """Create a QP on the CQ, under an id this node hasn't used."""
        qp = self.driver.create_qp(self.next_qp_id, ionic.QPT_RC, self.cq, self.cq)
        self.next_qp_id += 1
        return qp

    def guest_ip(self):
        """The guest address the mesh resolves to this node."""
        return self.node.guest_ip


def connect(a, b):
    """Point @a's QP at @b's and @b's at @a's.

    No retry: each node already knows the other's GID, so RTR resolving
    it is a check that the reset node's claim on it never lapsed.
    """
    for src, dst in ((a, b), (b, a)):
        hdr = ionic.roce_hdr_v4(ionic.ipv4(src.guest_ip()), ionic.ipv4(dst.guest_ip()))
        src.driver.connect_qp(src.qp, dst.qp.id, hdr)


def check_send(suite, src, dst, when):
    """A SEND from @src's QP to @dst's, checked byte for byte."""
    payload = f"SEND {src.node.index}->{dst.node.index} {when} ".encode() * 8
    src.driver.mr_write(src.mr, MR_VA, payload)
    dst.driver.mr_write(dst.mr, MR_VA, b"\0" * len(payload))
    dst.driver.post_recv(dst.qp, [ionic.Sge(MR_VA, MR_LEN, MR_KEY)])
    src.driver.post_send(
        src.qp,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
    )
    recv = dst.driver.wait_cq(dst.cq, timeout=COMPLETION_TIMEOUT)
    sent = src.driver.wait_cq(src.cq, timeout=COMPLETION_TIMEOUT)
    landed = dst.driver.mr_read(dst.mr, MR_VA, len(payload))
    suite.check(
        not recv.error and not sent.error and landed == payload,
        "a SEND from node %d to node %d %s",
        src.node.index,
        dst.node.index,
        when,
    )


def check_write(suite, src, dst, when):
    """An RDMA WRITE from @src's MR into @dst's, checked byte for byte."""
    payload = f"WRITE {src.node.index}->{dst.node.index} {when} ".encode() * 8
    src.driver.mr_write(src.mr, MR_VA, payload)
    dst.driver.mr_write(dst.mr, MR_VA, b"\0" * len(payload))
    post_write(src.driver, src.qp, len(payload))
    sent = src.driver.wait_cq(src.cq, timeout=COMPLETION_TIMEOUT)
    landed = dst.driver.mr_read(dst.mr, MR_VA, len(payload))
    suite.check(
        not sent.error and landed == payload,
        "an RDMA WRITE from node %d to node %d %s",
        src.node.index,
        dst.node.index,
        when,
    )


def check_traffic(suite, sides, when):
    """A SEND and an RDMA WRITE each way between the two nodes."""
    for src, dst in ((sides[0], sides[1]), (sides[1], sides[0])):
        check_send(suite, src, dst, when)
        check_write(suite, src, dst, when)


def check_stale_qp(suite, survivor, reset, old_mr):
    """The survivor's QP from before the reset is refused.

    The reset node has Bus Master Enable back but its driver hasn't
    loaded again, so the WRITE names a region and the SEND a QP that
    only the FLR can have taken away. Each must fail at the reset node,
    and the WRITE must leave the pages behind @old_mr, the reset node's
    MR from before the reset, as they were.
    """
    payload = b"\x5a" * 256
    before = reset.driver.mr_read(old_mr, MR_VA, len(payload))
    survivor.driver.mr_write(survivor.mr, MR_VA, payload)

    post_write(survivor.driver, survivor.qp, len(payload))
    sent = survivor.driver.wait_cq(survivor.cq, timeout=COMPLETION_TIMEOUT)
    suite.check(sent.error, "a WRITE from the old QP to the reset node fails")
    suite.check_eq(
        sent.status,
        ionic.STS_REMOTE_ACC_ERR,
        "the WRITE's status, blaming the region the reset took away",
    )
    suite.check_eq(
        reset.driver.mr_read(old_mr, MR_VA, len(payload)),
        before,
        "the guest memory behind the reset node's old MR",
    )

    survivor.driver.post_send(
        survivor.qp,
        ionic.OP_SEND,
        flags=ionic.WQE_SIGNALLED,
        sge=[ionic.Sge(MR_VA, len(payload), MR_KEY)],
    )
    sent = survivor.driver.wait_cq(survivor.cq, timeout=COMPLETION_TIMEOUT)
    suite.check(sent.error, "a SEND from the old QP to the reset node fails")
    suite.check_eq(
        sent.status,
        ionic.STS_REMOTE_ACC_ERR,
        "the SEND's status, blaming the QP the reset took away",
    )


def mesh_state(mesh_):
    """What a reset must leave alone in the mesh.

    Every registration, as each side logs it: the manager's of a new
    node and of a node registering again under its id, which
    registrations() leaves out, and the worker's, whose "Registered
    with manager" and "Registered with the manager again" share a
    prefix. Then every node's id, and the connections the manager has
    dropped.
    """
    manager = mesh_.nodes[0].server.log_text()
    return {
        "registrations": mesh_.registrations(),
        "registrations again": manager.count("registered again"),
        "worker registrations": mesh_.nodes[1]
        .server.log_text()
        .count("TCP: Registered with "),
        "node ids": [mesh_.node_id(i) for i in range(mesh_.size)],
        "disconnects": manager.count("disconnected"),
    }


# pylint: disable-next=too-many-arguments,too-many-positional-arguments
def test_flr(suite, mesh_, regs, sides, reset_index):
    """FLR node @reset_index, and connect it to the other node again."""
    reset = sides[reset_index]
    survivor = sides[1 - reset_index]
    client = reset.node.client
    server = reset.node.server
    before = mesh_state(mesh_)

    px = pci.find_capability(client, regs, regs.PCI_CAP_ID_EXP)
    if not suite.check(px is not None, "there is a PCI Express capability"):
        return
    # The driver set Bus Master Enable, and the FLR's reset of config
    # space clears it before libvfio-user answers the write starting it,
    # so a clear bit afterwards says the FLR reached the device.
    suite.check(
        pci.read16(client, regs.PCI_COMMAND) & regs.PCI_COMMAND_MASTER,
        "Bus Master Enable is set before the FLR",
    )
    pci.start_flr(client, regs, px)
    suite.check(
        not pci.read16(client, regs.PCI_COMMAND) & regs.PCI_COMMAND_MASTER,
        "the FLR cleared Bus Master Enable",
    )

    # Linux's pci_reset_function() restores the config space it saved,
    # Bus Master Enable included, before the driver's reset_done()
    # loads it again, so the device answers peers in between.
    client.set_bus_master(True)
    check_stale_qp(suite, survivor, reset, reset.mr)
    reset.driver.bringup()

    suite.check_eq(
        survivor.driver.destroy_qp(survivor.qp), 0, "the old QP is destroyed"
    )
    survivor.qp = survivor.new_qp()
    old_qp_id = reset.qp.id
    reset.make_resources(old_qp_id)
    suite.check(True, "the reset node made its CQ, MR and QP %d again", old_qp_id)
    connect(sides[0], sides[1])
    suite.check(True, "the new QPs connected without waiting for the mesh")
    check_traffic(suite, sides, f"after node {reset_index}'s FLR")

    suite.check(server.is_running(), "the reset node's server is still running")
    suite.check(
        survivor.node.server.is_running(), "the other node's server is still running"
    )
    after = mesh_state(mesh_)
    for key, value in before.items():
        suite.check_eq(after[key], value, "the mesh's %s", key)


def test_connect(suite, mesh_, sides):
    """Attach a guest to each node, connect them, and pass traffic.

    Fills in @sides, which the resets go on to use.
    """
    suite.check(mesh_.wait_gids(60), "each node learned the other's GID")
    sides.extend(Side(mesh_.nodes[i]) for i in range(mesh_.size))
    connect(sides[0], sides[1])
    check_traffic(suite, sides, "before any reset")


def run_tests(suite, mesh_, regs):
    """Connect the two nodes, then reset each in turn."""
    sides = []
    suite.run("connect the nodes and pass traffic", test_connect, mesh_, sides)
    if not suite.check(len(sides) == mesh_.size, "both nodes have a guest"):
        return
    for index in RESET_ORDER:
        role = "manager" if index == 0 else "worker"
        suite.run(f"an FLR on the {role}", test_flr, mesh_, regs, sides, index)


def main():
    """Stand up a two-node mesh, reset each node, and stop both."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args, regs = pci.parse_args(parser)

    suite = Suite("rocm-ernic function-level reset on the TCP mesh")
    with tempfile.TemporaryDirectory(prefix="ernic-mesh-flr-") as run_dir:
        # Inside the try: a worker that fails to start leaves the manager
        # running, which the finally has to stop.
        mesh_ = mesh.Mesh(args.server, run_dir, size=2)
        try:
            mesh_.start(timeout=60)
            run_tests(suite, mesh_, regs)
        finally:
            problems = mesh_.stop()
            suite.check(
                not problems,
                "both nodes exited cleanly and within their deadline"
                + ("" if not problems else f": {'; '.join(problems)}"),
            )
    suite.finish()


if __name__ == "__main__":
    main()
