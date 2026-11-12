#!/usr/bin/env python3
"""Drive the Controller Memory Buffer (CMB), BAR4, over vfio-user.

The device has a CMB only when the server is started with --cmb. With
one, DISCOVER_CMB describes it, Q_IDENTIFY offers the express doorbell,
a QP's rings can be placed in it, and nothing written there outlives a
reset or the client that wrote it. Without one, the device offers no CMB
anywhere a driver might look.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic, vfu
from ernic.check import Suite
from ernic.server import Server, stop_and_check

# pylint: enable=wrong-import-position

# BAR4, the CMB
CMB_SIZE = 8 << 20


# pylint: disable-next=too-many-locals  # 4 regions × 3 fields each
def test_cmb(suite, server):
    """CMB/EXPDB: DISCOVER_CMB, Q_IDENTIFY EXPDB feature, and a CMB QP."""
    client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
    mem = vfu.GuestMemory(size=1 << 20)
    try:
        client.dma_map(mem)
        drv = ionic.IonicDriver(client, mem)
        drv.bringup()

        # DISCOVER_CMB (opcode 80): two CMB regions on BAR4.
        cmd = bytearray(64)
        cmd[0] = ionic.CMD_DISCOVER_CMB
        data = drv.devcmd_ok(bytes(cmd), 40)
        num_regions = struct.unpack_from("<I", data, 0)[0]
        flags = struct.unpack_from("<I", data, 4)[0]
        suite.check(
            num_regions == 2,
            f"DISCOVER_CMB reports 2 regions, got {num_regions}",
        )
        suite.check(
            bool(flags & ionic.IONIC_CMB_FLAG_EXPDB),
            f"DISCOVER_CMB sets EXPDB flag (flags={flags:#x})",
        )
        # region[0]: DEVMEM on BAR4 at response offset 8
        r0_bar = data[8]
        r0_type = data[9]
        r0_len = struct.unpack_from("<I", data, 8 + 12)[0]
        suite.check(r0_bar == 4, f"region[0] is on BAR4 (got {r0_bar})")
        suite.check(
            r0_type == ionic.IONIC_CMB_TYPE_DEVMEM,
            f"region[0] is DEVMEM (got {r0_type})",
        )
        suite.check(r0_len == 128, f"region[0] is 128 pages (got {r0_len})")
        # region[1]: EXPDB512 on BAR4, at response offset 24
        r1_bar = data[24]
        r1_type = data[25]
        r1_len = struct.unpack_from("<I", data, 24 + 12)[0]
        suite.check(r1_bar == 4, f"region[1] is on BAR4 (got {r1_bar})")
        suite.check(
            r1_type == ionic.IONIC_CMB_TYPE_EXPDB512,
            f"region[1] is EXPDB512 (got {r1_type})",
        )
        suite.check(r1_len == 128, f"region[1] is 128 pages (got {r1_len})")

        # Q_IDENTIFY (opcode 39) for TXQ (qtype 3): must advertise EXPDB.
        cmd = bytearray(64)
        cmd[0] = ionic.CMD_Q_IDENTIFY
        cmd[4] = ionic.IONIC_QTYPE_TXQ  # ionic_q_identify_cmd.type
        data = drv.devcmd_ok(bytes(cmd), 8 + 8)
        features = struct.unpack_from("<Q", data, 8)[0]
        suite.check(
            bool(features & ionic.IONIC_QIDENT_F_EXPDB),
            f"Q_IDENTIFY TXQ reports EXPDB (features={features:#x})",
        )

        # CREATE_QP with IONIC_QPF_SQ_CMB: must succeed.
        cq = drv.create_cq(cq_id=1)
        drv.create_qp(qp_id=1, qp_type=ionic.QPT_RC, scq=cq, rcq=cq, sq_cmb=0)
        suite.check(True, "CREATE_QP with SQ_CMB flag succeeded")
    finally:
        client.close()
        mem.close()


def test_cmb_cleared(suite, server):
    """Nothing written to the CMB outlives a device reset or its client.

    A guest's work requests, and their inline data, are written to BAR4.
    The server serves one client after another on the same device, a guest
    reboots through a device reset, and the driver resets the device with
    IONIC_CMD_RESET as it loads and unloads, so what one left there must
    not be readable by the next. Each pattern is read back before the
    reset, so zeros afterwards mean it was cleared, not that it never
    landed.
    """
    bar4 = vfu.VFU_PCI_DEV_BAR4_REGION_IDX
    # The first WQE slot, and the last 8 bytes of the CMB
    spots = ((0, bytes(range(1, 65))), (CMB_SIZE - 8, b"\xa5" * 8))
    zeros = {offset: bytes(len(data)) for offset, data in spots}

    def fill(client, before):
        for offset, data in spots:
            client.write_region(bar4, offset, data)
            suite.check_eq(
                client.read_region(bar4, offset, len(data)),
                data,
                "BAR4 at %#x holds what was written before %s",
                offset,
                before,
            )

    def cleared(client, after):
        for offset, data in zeros.items():
            suite.check_eq(
                client.read_region(bar4, offset, len(data)),
                data,
                "BAR4 at %#x is zero after %s",
                offset,
                after,
            )

    with (
        vfu.VfioUserClient(server.socket_path).connect(timeout=30) as client,
        vfu.GuestMemory(size=1 << 20) as mem,
    ):
        fill(client, "a device reset")
        client.reset()
        cleared(client, "a device reset")

        fill(client, "IONIC_CMD_RESET")
        cmd = bytearray(64)
        cmd[0] = ionic.CMD_RESET
        ionic.IonicDriver(client, mem).devcmd_ok(bytes(cmd))
        cleared(client, "IONIC_CMD_RESET")

        fill(client, "the client goes")

    with vfu.VfioUserClient(server.socket_path).connect(timeout=30) as client:
        cleared(client, "the first client went")


def q_identify_features(drv, qtype):
    """The features Q_IDENTIFY reports for queue type @qtype."""
    cmd = bytearray(64)
    cmd[0] = ionic.CMD_Q_IDENTIFY
    cmd[4] = qtype  # ionic_q_identify_cmd.type
    return struct.unpack_from("<Q", drv.devcmd_ok(bytes(cmd), 8 + 8), 8)[0]


def test_no_cmb(suite, binary, run_dir):
    """The device has no CMB unless the server is started with --cmb.

    Without BAR4 the guest driver finds no CMB, and puts every ring in
    guest memory whatever the application asked for. Nor does the device
    offer one anywhere else a driver might look: Q_IDENTIFY advertises no
    express doorbell, DISCOVER_CMB is refused, and so is a QP whose send
    ring is in the CMB.
    """
    server = Server(
        binary,
        os.path.join(run_dir, "no-cmb.sock"),
        log_path=os.path.join(run_dir, "no-cmb.log"),
    )
    server.start(timeout=60)
    try:
        with (
            vfu.VfioUserClient(server.socket_path).connect(timeout=30) as client,
            vfu.GuestMemory(size=1 << 20) as mem,
        ):
            suite.check(
                client.regions.get(vfu.VFU_PCI_DEV_BAR4_REGION_IDX) is None,
                "BAR4 has no size",
            )
            client.dma_map(mem)
            drv = ionic.IonicDriver(client, mem)
            drv.bringup()

            cmd = bytearray(64)
            cmd[0] = ionic.CMD_DISCOVER_CMB
            status, _ = drv.devcmd(bytes(cmd), 40)
            suite.check_eq(status, ionic.RC_ENOSUPP, "DISCOVER_CMB status")

            for qtype, name in (
                (ionic.IONIC_QTYPE_RXQ, "RXQ"),
                (ionic.IONIC_QTYPE_TXQ, "TXQ"),
            ):
                suite.check_eq(
                    q_identify_features(drv, qtype) & ionic.IONIC_QIDENT_F_EXPDB,
                    0,
                    "Q_IDENTIFY %s's EXPDB feature",
                    name,
                )

            cq = drv.create_cq(cq_id=1)
            try:
                drv.create_qp(1, ionic.QPT_RC, cq, cq, sq_cmb=0)
                why = "it was accepted"
            except ionic.DeviceError as exc:
                why = str(exc)
            suite.check(
                why.endswith(f"status {ionic.RC_ENOSUPP}"),
                "CREATE_QP with its send ring in the CMB refused as "
                "unsupported (%s)",
                why,
            )
    finally:
        stop_and_check(suite, server)


def main():
    """Start a server with a CMB, run the CMB tests against it, and stop it."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    suite = Suite("rocm-ernic CMB tests")
    with tempfile.TemporaryDirectory(prefix="ernic-cmb-") as run_dir:
        server = Server(
            args.server,
            os.path.join(run_dir, "cmb.sock"),
            log_path=os.path.join(run_dir, "cmb.log"),
            extra_args=("--cmb",),
        )
        server.start(timeout=60)
        try:
            suite.run("CMB/EXPDB discovery and queue setup", test_cmb, server)
            suite.run(
                "CMB cleared by a reset and between clients", test_cmb_cleared, server
            )
        finally:
            stop_and_check(suite, server)
        suite.run("no CMB without --cmb", test_no_cmb, args.server, run_dir)
    suite.finish()


if __name__ == "__main__":
    main()
