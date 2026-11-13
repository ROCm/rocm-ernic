#!/usr/bin/env python3
"""Drive the Controller Memory Buffer (CMB), BAR4, over vfio-user.

The device has a CMB only when the server is started with --cmb. With
one, DISCOVER_CMB describes it, a QP's rings can be placed in it, and
the server offers the file behind it for the client to map, as QEMU maps
it into a guest. Nobody can change the file's size, what is stored
through the mapping is what is read through the socket, and nothing
written there outlives a reset or the client that wrote it, whose
mapping no longer reaches the next client's CMB. The device offers no
express doorbell: it never sees a store through the mapping. Without a
CMB, the device offers none anywhere a driver might look.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import fcntl
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


def test_cmb(suite, server):
    """DISCOVER_CMB, Q_IDENTIFY's features, and a QP with a ring in the CMB."""
    client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
    mem = vfu.GuestMemory(size=1 << 20)
    try:
        client.dma_map(mem)
        drv = ionic.IonicDriver(client, mem)
        drv.bringup()

        # DISCOVER_CMB (opcode 80): one DEVMEM region, the whole of BAR4,
        # and no express doorbell
        cmd = bytearray(64)
        cmd[0] = ionic.CMD_DISCOVER_CMB
        data = drv.devcmd_ok(bytes(cmd), 40)
        num_regions = struct.unpack_from("<I", data, 0)[0]
        flags = struct.unpack_from("<I", data, 4)[0]
        suite.check_eq(num_regions, 1, "DISCOVER_CMB's region count")
        suite.check(
            not flags & ionic.IONIC_CMB_FLAG_EXPDB,
            f"DISCOVER_CMB offers no express doorbell (flags={flags:#x})",
        )
        # region[0] at response offset 8
        suite.check_eq(data[8], 4, "region[0]'s BAR")
        suite.check_eq(data[9], ionic.IONIC_CMB_TYPE_DEVMEM, "region[0]'s type")
        suite.check_eq(
            struct.unpack_from("<I", data, 8 + 12)[0], 128, "region[0]'s pages"
        )
        suite.check_eq(data[24:40], bytes(16), "region[1], which isn't there")

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

        # CREATE_QP with IONIC_QPF_SQ_CMB: must succeed.
        cq = drv.create_cq(cq_id=1)
        drv.create_qp(qp_id=1, qp_type=ionic.QPT_RC, scq=cq, rcq=cq, sq_cmb=0)
        suite.check(True, "CREATE_QP with SQ_CMB flag succeeded")
    finally:
        client.close()
        mem.close()


def test_cmb_mapped(suite, server):
    """The CMB, mapped from the file the server offers, as QEMU maps it.

    The mapping is the memory the device reads, so what is stored
    through it is what is read through the socket, and the other way
    round, and a reset clears it. The file's size is sealed: a client
    that cut it short would kill the server on its next access. A client
    that goes keeps its mapping, so the next client gets other memory,
    which the old mapping doesn't reach.
    """
    bar4 = vfu.VFU_PCI_DEV_BAR4_REGION_IDX
    stored = bytes(range(1, 65))
    written = b"\x5a" * 8
    seals = fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL

    first = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
    try:
        region = first.regions.get(bar4)
        suite.check(
            region is not None and region.flags & vfu.VFIO_REGION_INFO_FLAG_MMAP,
            "BAR4 is offered to map",
        )
        old = first.map_region(bar4)
        if not suite.check(old is not None, "BAR4 came with a file to map"):
            return
        suite.check_eq(old.areas, [(0, CMB_SIZE)], "the areas that may be mapped")
        suite.check_eq(
            fcntl.fcntl(old.fd, fcntl.F_GET_SEALS) & seals,
            seals,
            "the seals on the CMB's file",
        )
        try:
            os.ftruncate(old.fd, CMB_SIZE // 2)
            shrunk = True
        except PermissionError:
            shrunk = False
        suite.check(not shrunk, "the CMB's file can't be cut short")

        old.map[0:64] = stored
        suite.check_eq(
            first.read_region(bar4, 0, 64),
            stored,
            "what was stored through the mapping, read through the socket",
        )
        first.write_region(bar4, 4096, written)
        suite.check_eq(
            old.map[4096:4104],
            written,
            "what was written through the socket, seen through the mapping",
        )
        first.reset()
        suite.check_eq(old.map[0:64], bytes(64), "the mapping after a reset")

        # The first client goes, keeping its mapping
        old.map[0:64] = stored
        first.close()
        with vfu.VfioUserClient(server.socket_path).connect(timeout=30) as second:
            suite.check_eq(
                second.read_region(bar4, 0, 64),
                bytes(64),
                "the next client's CMB, before the old mapping is stored to again",
            )
            old.map[0:64] = stored
            suite.check_eq(
                second.read_region(bar4, 0, 64),
                bytes(64),
                "the next client's CMB, after the old mapping is stored to again",
            )
            with second.map_region(bar4) as new:
                suite.check(
                    os.fstat(new.fd).st_ino != os.fstat(old.fd).st_ino,
                    "the next client is sent another file",
                )
                new.map[0:64] = stored
                suite.check_eq(
                    second.read_region(bar4, 0, 64),
                    stored,
                    "the next client's own mapping is its CMB",
                )
        old.close()
    finally:
        first.close()


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
    offer one anywhere else a driver might look: DISCOVER_CMB is refused,
    and so is a QP whose send ring is in the CMB.
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
            suite.run("CMB discovery and queue setup", test_cmb, server)
            suite.run("CMB mapped by the client", test_cmb_mapped, server)
            suite.run(
                "CMB cleared by a reset and between clients", test_cmb_cleared, server
            )
        finally:
            stop_and_check(suite, server)
        suite.run("no CMB without --cmb", test_no_cmb, args.server, run_dir)
    suite.finish()


if __name__ == "__main__":
    main()
