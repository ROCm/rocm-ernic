#!/usr/bin/env python3
"""Drive a real rocm-ernic process over its vfio-user socket.

This is the protocol layer: what a VMM does before any device-specific
code runs, and what the server should do with a client that gets it
wrong. Everything the guest controls here -- region indices, offsets and
counts, DMA addresses and lengths, interrupt ranges -- is checked for
being refused rather than truncated or trusted.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import errno
import os
import re
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic, pci, vfu
from ernic.check import Suite
from ernic.server import Server, stop_and_check

# pylint: enable=wrong-import-position

# The emulated device's PCI identity, which find_rdma_device() matches in
# full rather than on a shared vendor or device id.
PCI_VENDOR_ID_PENSANDO = 0x1DD8
PCI_DEVICE_ID_ROCM_ERNIC = 0x1002
PCI_SUBDEVICE_ID_ROCM_ERNIC = 0x5400

BAR0_SIZE = 64 << 10
# The ionic register window; the MSI-X table and PBA are above it.
BAR0_REGS_SIZE = 32 << 10
IONIC_DEV_SIG = 0x44455649  # "DEVI"

# The PCI base class of a network controller (the kernel's
# PCI_BASE_CLASS_NETWORK, which no uapi header has)
PCI_BASE_CLASS_NETWORK = 0x02
# The PCI Express link's width, in lanes; it runs at 64 GT/s
PCIE_LINK_WIDTH = 16

# struct ionic_lif_info: the status block at 256, link_status at +10.
LIF_INFO_LINK_STATUS_OFF = 256 + 10
PORT_OPER_STATUS_UP = 1

# What the server logs when a client goes, and when a LIF statistics
# write to guest memory fails.
CLIENT_DISCONNECTED = "Client disconnected after"
LIF_STATS_WRITE_FAILED = "LIF stats write to"
# What the server logs when it refuses a BAR0 access across the
# register/MSI-X split.
BAR0_STRADDLE_REFUSED = "ionic BAR0 access straddling the register/MSI-X split"
# The stats file's connection state after a function-level reset
FLR_CONNECTION = "connected (PCI FLR)"


def refused(suite, what, func, *expected):
    """Check that @func is refused, with one of @expected errnos."""
    try:
        func()
    except vfu.VfioUserError as exc:
        if not expected or exc.err in expected:
            return suite.check(
                True, "%s refused (%s)", what, errno.errorcode.get(exc.err, exc.err)
            )
        return suite.check(
            False,
            "%s refused with %s, expected %s",
            what,
            errno.errorcode.get(exc.err, exc.err),
            " or ".join(errno.errorcode.get(e, str(e)) for e in expected),
        )
    return suite.check(False, "%s was accepted", what)


def test_negotiation(suite, client):
    """The version exchange, and what the server says it can do."""
    caps = client.server_caps.get("capabilities", {})
    suite.check(
        caps.get("max_data_xfer_size", 0) >= 4096,
        f"server offers max_data_xfer_size={caps.get('max_data_xfer_size')}",
    )
    suite.check(
        caps.get("max_msg_fds", 0) >= 1,
        f"server offers max_msg_fds={caps.get('max_msg_fds')}",
    )


def test_device_info(suite, client):
    """What the device says it is, and the regions it has."""
    flags, num_regions, num_irqs = client.device_info()
    suite.check(flags & 2, "device reports itself as PCI")
    suite.check(flags & 1, "device reports that it can be reset")
    suite.check_eq(num_regions, vfu.VFU_PCI_DEV_NUM_REGIONS, "region count")
    suite.check(num_irqs >= 3, f"device has {num_irqs} interrupt types")

    bar0 = client.regions.get(vfu.VFU_PCI_DEV_BAR0_REGION_IDX)
    bar2 = client.regions.get(vfu.VFU_PCI_DEV_BAR2_REGION_IDX)
    cfg = client.regions.get(vfu.VFU_PCI_DEV_CFG_REGION_IDX)
    suite.check(bar0 is not None and bar0.size == BAR0_SIZE, "BAR0 is 64 KiB")
    suite.check(bar2 is not None and bar2.size >= 1 << 20, "BAR2 is at least 1 MiB")
    suite.check(cfg is not None and cfg.size >= 256, "config space is present")
    for region in (bar0, bar2, cfg):
        if region is not None:
            suite.check(
                region.flags & (vfu.VFIO_REGION_INFO_FLAG_READ)
                and region.flags & vfu.VFIO_REGION_INFO_FLAG_WRITE,
                f"region {region.index} is readable and writable",
            )
    suite.check(
        client.regions.get(1) is None,
        "BAR1 has no size, as the device sets up only two",
    )


def read_config16(client, offset):
    """The little-endian 16-bit config-space register at @offset."""
    return struct.unpack("<H", client.read_config(offset, 2))[0]


def read_config32(client, offset):
    """The little-endian 32-bit config-space register at @offset."""
    return struct.unpack("<I", client.read_config(offset, 4))[0]


def find_capability(client, regs, cap_id):
    """The config-space offset of capability @cap_id, or None.

    Walks the list from its head, as a guest does, so a capability the
    list doesn't reach isn't found. The walk is bounded, since the
    standard config space has room for at most 48 capabilities.
    """
    offset = client.read_config(regs.PCI_CAPABILITY_LIST, 1)[0]
    for _ in range(48):
        if offset == 0:
            return None
        if client.read_config(offset + regs.PCI_CAP_LIST_ID, 1)[0] == cap_id:
            return offset
        offset = client.read_config(offset + regs.PCI_CAP_LIST_NEXT, 1)[0]
    return None


def check_pcie_link(suite, client, regs, px):
    """The link in the PCI Express capability at @px: 64 GT/s, x16."""
    lnkcap = read_config32(client, px + regs.PCI_EXP_LNKCAP)
    suite.check_eq(
        pci.field(lnkcap, regs.PCI_EXP_LNKCAP_SLS),
        regs.PCI_EXP_LNKCAP_SLS_64_0GB,
        "the maximum link speed is 64 GT/s",
    )
    suite.check_eq(
        pci.field(lnkcap, regs.PCI_EXP_LNKCAP_MLW),
        PCIE_LINK_WIDTH,
        "the maximum link width is x16",
    )

    # The device has no physical link to train down
    lnksta = read_config16(client, px + regs.PCI_EXP_LNKSTA)
    suite.check_eq(
        pci.field(lnksta, regs.PCI_EXP_LNKSTA_CLS),
        regs.PCI_EXP_LNKSTA_CLS_64_0GB,
        "the current link speed is 64 GT/s",
    )
    suite.check_eq(
        pci.field(lnksta, regs.PCI_EXP_LNKSTA_NLW),
        PCIE_LINK_WIDTH,
        "the negotiated link width is x16",
    )

    lnkcap2 = read_config32(client, px + regs.PCI_EXP_LNKCAP2)
    every_speed = (
        regs.PCI_EXP_LNKCAP2_SLS_2_5GB
        | regs.PCI_EXP_LNKCAP2_SLS_5_0GB
        | regs.PCI_EXP_LNKCAP2_SLS_8_0GB
        | regs.PCI_EXP_LNKCAP2_SLS_16_0GB
        | regs.PCI_EXP_LNKCAP2_SLS_32_0GB
        | regs.PCI_EXP_LNKCAP2_SLS_64_0GB
    )
    suite.check_eq(
        lnkcap2 & regs.PCI_EXP_LNKCAP2_SLS,
        every_speed,
        "every speed from 2.5 to 64 GT/s is supported",
    )
    lnkctl2 = read_config16(client, px + regs.PCI_EXP_LNKCTL2)
    suite.check_eq(
        pci.field(lnkctl2, regs.PCI_EXP_LNKCTL2_TLS),
        regs.PCI_EXP_LNKCTL2_TLS_64_0GT,
        "the target link speed is 64 GT/s",
    )


def test_config_space(suite, client, regs):
    """The PCI identity a guest matches the driver against."""
    suite.check_eq(
        read_config16(client, regs.PCI_VENDOR_ID), PCI_VENDOR_ID_PENSANDO, "vendor id"
    )
    suite.check_eq(
        read_config16(client, regs.PCI_DEVICE_ID), PCI_DEVICE_ID_ROCM_ERNIC, "device id"
    )
    suite.check_eq(
        read_config16(client, regs.PCI_SUBSYSTEM_VENDOR_ID),
        PCI_VENDOR_ID_PENSANDO,
        "subsystem vendor id",
    )
    suite.check_eq(
        read_config16(client, regs.PCI_SUBSYSTEM_ID),
        PCI_SUBDEVICE_ID_ROCM_ERNIC,
        "subsystem device id",
    )
    suite.check_eq(
        read_config16(client, regs.PCI_CLASS_DEVICE) >> 8,
        PCI_BASE_CLASS_NETWORK,
        "base class is network",
    )
    suite.check_eq(
        client.read_config(regs.PCI_HEADER_TYPE, 1)[0] & regs.PCI_HEADER_TYPE_MASK,
        regs.PCI_HEADER_TYPE_NORMAL,
        "header type is a normal function",
    )

    suite.check(
        read_config16(client, regs.PCI_STATUS) & regs.PCI_STATUS_CAP_LIST,
        "status says there is a capability list",
    )
    px = find_capability(client, regs, regs.PCI_CAP_ID_EXP)
    suite.check(px is not None, "the PCI Express capability is on the list")
    suite.check(
        find_capability(client, regs, regs.PCI_CAP_ID_MSIX) is not None,
        "the MSI-X capability is on the list",
    )
    if px is None:
        return

    flags = read_config16(client, px + regs.PCI_EXP_FLAGS)
    suite.check_eq(
        pci.field(flags, regs.PCI_EXP_FLAGS_VERS), 2, "PCI Express capability version"
    )
    suite.check_eq(
        pci.field(flags, regs.PCI_EXP_FLAGS_TYPE),
        regs.PCI_EXP_TYPE_ENDPOINT,
        "PCI Express device type is Endpoint",
    )
    suite.check_eq(
        read_config32(client, px + regs.PCI_EXP_DEVCAP) & regs.PCI_EXP_DEVCAP_FLR,
        0,
        "the device doesn't advertise FLR",
    )
    check_pcie_link(suite, client, regs, px)


def test_bar_access(suite, client, server):
    """BAR0 reads, and the offsets and counts a guest must not get away with."""
    suite.check_eq(client.read_bar32(0, 0), IONIC_DEV_SIG, "BAR0 signature")

    refused(
        suite,
        "a BAR0 read running past the end of the region",
        lambda: client.read_region(0, BAR0_SIZE - 16, 64),
        errno.EINVAL,
    )
    refused(
        suite,
        "a BAR0 read across the register/MSI-X split",
        lambda: client.read_region(0, BAR0_REGS_SIZE - 4, 8),
        errno.EINVAL,
    )
    # libvfio-user passes this one on, so the refusal is the server's own.
    suite.check(
        server.wait_for_log(BAR0_STRADDLE_REFUSED),
        "the server refused the read across the split",
    )
    refused(
        suite,
        "a BAR0 read of more than the negotiated transfer size",
        lambda: client.read_region(0, 0, (1 << 20) + 1),
        errno.EINVAL,
    )
    refused(
        suite,
        "a read of a region the device does not have",
        lambda: client.read_region(3, 0, 4),
        errno.EINVAL,
    )
    refused(
        suite,
        "a read at an offset that wraps when the count is added",
        lambda: client.read_region(0, (1 << 64) - 8, 64),
        errno.EINVAL,
    )
    # A write whose header says more bytes than it carries. The device
    # must not read past what arrived.
    payload = struct.pack("<QII", 0, 0, 64) + b"\x01" * 8
    refused(
        suite,
        "a region write whose count exceeds its payload",
        lambda: client.request(vfu.VFIO_USER_REGION_WRITE, payload),
        errno.EINVAL,
    )
    suite.check_eq(
        client.read_bar32(0, 0), IONIC_DEV_SIG, "BAR0 still answers after the refusals"
    )


def test_dma(suite, client):
    """Mapping guest memory, and taking it back."""
    with vfu.GuestMemory(size=4 << 20) as mem:
        client.dma_map(mem)
        suite.check(True, "guest memory mapped")
        refused(
            suite,
            "unmapping a region that was never mapped",
            lambda: client.dma_unmap(0x9900_0000, 4096),
            errno.ENOENT,
            errno.EINVAL,
        )
        client.dma_unmap(mem.base, mem.size)
        suite.check(True, "guest memory unmapped")
        refused(
            suite,
            "unmapping the same region twice",
            lambda: client.dma_unmap(mem.base, mem.size),
            errno.ENOENT,
            errno.EINVAL,
        )

        # An IOVA range whose end wraps. libvfio-user currently accepts
        # this, so what is checked here is containment: the server must
        # survive it and must still serve a well-formed client.
        try:
            client.request(
                vfu.VFIO_USER_DMA_MAP,
                struct.pack("<IIQQQ", 32, 3, 0, (1 << 64) - 4096, 1 << 20),
                fds=[mem.fd],
            )
            suite.check(True, "a wrapping DMA range was accepted (libvfio-user)")
        except vfu.VfioUserError:
            suite.check(True, "a wrapping DMA range was refused")

        client.dma_map(mem)
        suite.check_eq(
            client.read_bar32(0, 0),
            IONIC_DEV_SIG,
            "the device still answers after a wrapping map",
        )
        client.dma_unmap(mem.base, mem.size)


def test_irqs(suite, client):
    """MSI-X, as a VMM sets it up: eventfds the device will signal."""
    flags, count = client.irq_info(vfu.VFU_DEV_MSIX_IRQ)
    suite.check(flags & 1, "MSI-X vectors are triggered by eventfd")
    suite.check(count >= 2, f"device has {count} MSI-X vectors")

    refused(
        suite,
        "asking about an interrupt type the device does not have",
        lambda: client.irq_info(vfu.VFU_DEV_REQ_IRQ + 1),
        errno.EINVAL,
    )

    fds = [os.eventfd(0) for _ in range(2)]
    try:
        client.set_irqs(vfu.VFU_DEV_MSIX_IRQ, 0, len(fds), fds)
        suite.check(True, "MSI-X vectors 0 and 1 registered")
        refused(
            suite,
            "registering a vector range that runs past the last vector",
            lambda: client.set_irqs(vfu.VFU_DEV_MSIX_IRQ, count - 1, 4, fds * 2),
            errno.EINVAL,
        )
        client.disable_irqs(vfu.VFU_DEV_MSIX_IRQ)
        suite.check(True, "MSI-X vectors taken back")
    finally:
        for fd in fds:
            os.close(fd)


def read_stats(path):
    """The stats file's connection state and write count.

    None if the file isn't there yet, or lacks either line.
    """
    try:
        with open(path, encoding="utf-8") as stats:
            text = stats.read()
    except OSError:
        return None
    state = re.search(r"^  connection *: (.*)$", text, re.M)
    count = re.search(r"^Write count : ([0-9]+)$", text, re.M)
    if not state or not count:
        return None
    return state.group(1), int(count.group(1))


def test_stats_file(suite, stats_path):
    """The server rewrites --stats-file about once a second while attached.

    Two rewrites after the first look take at least a second between them;
    a server that rewrote it on every pass of its loop would get there in
    milliseconds.
    """
    deadline = time.monotonic() + 15
    first = None
    last = None
    while time.monotonic() < deadline:
        looked = time.monotonic()
        stats = read_stats(stats_path)
        if stats is not None:
            last = stats
        if stats is not None and stats[0] == "connected":
            if first is None:
                first = (looked, stats[1])
            elif stats[1] >= first[1] + 2:
                took = time.monotonic() - first[0]
                suite.check(
                    took >= 0.9,
                    "the stats file was rewritten twice in %.2f s while "
                    "attached, and no faster than once a second",
                    took,
                )
                return
        time.sleep(0.05)
    suite.check(
        False,
        "the stats file wasn't rewritten twice within 15 s while attached "
        "(last read: %r)",
        last,
    )


def read_resets(path):
    """The stats file's connection state and reset count, or None."""
    with open(path, encoding="utf-8") as stats:
        text = stats.read()
    state = re.search(r"^  connection *: (.*)$", text, re.M)
    count = re.search(r"^  reset_count *: ([0-9]+)$", text, re.M)
    if not state or not count:
        return None
    return state.group(1), int(count.group(1))


def test_flr_refused(suite, client, regs, stats_path):
    """A write starting an FLR, which the device doesn't advertise.

    Linux's pcie_flr() starts one by setting Initiate FLR in Device
    Control, written as the one word it is. libvfio-user refuses it
    without calling the reset callback, which would rewrite the stats file
    before the write returned, counting the reset and saying it was an FLR.
    """
    px = find_capability(client, regs, regs.PCI_CAP_ID_EXP)
    if not suite.check(px is not None, "there is a PCI Express capability"):
        return
    before = read_resets(stats_path)
    if not suite.check(before is not None, "the stats file has a reset count"):
        return

    refused(
        suite,
        "a write starting an FLR",
        lambda: client.write_config(
            px + regs.PCI_EXP_DEVCTL, struct.pack("<H", regs.PCI_EXP_DEVCTL_BCR_FLR)
        ),
        errno.EINVAL,
    )
    after = read_resets(stats_path)
    if suite.check(after is not None, "the stats file still has a reset count"):
        suite.check(after[0] != FLR_CONNECTION, "the server didn't take an FLR")
        suite.check_eq(after[1], before[1], "the device wasn't reset")


def test_reset_and_reconnect(suite, client, socket_path):
    """A device reset, and a second client after the first detaches."""
    client.reset()
    suite.check_eq(
        client.read_bar32(0, 0), IONIC_DEV_SIG, "the device answers after a reset"
    )
    client.close()

    with vfu.VfioUserClient(socket_path).connect(timeout=30) as second:
        suite.check_eq(
            struct.unpack_from("<H", second.read_config(0, 2))[0],
            PCI_VENDOR_ID_PENSANDO,
            "a second client can attach after the first detaches",
        )


def test_hang_up_mid_command(suite, server):
    """A client that hangs up while the device answers a command.

    The command is LIF_INIT, which writes the link status into guest
    memory and has the device publish the LIF statistics on that pass of
    its loop. The client shuts its receiving side first, so the reply
    fails; libvfio-user then resets the context, unmapping guest memory,
    and vfu_run_ctx() returns in the same pass that ran the command. The
    server must not go on to publish into memory that is no longer
    mapped: with a libvfio-user that keeps a stale region pointer, that
    write lands in freed, unmapped memory and kills the server.
    """
    client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
    # Counted only now: the server logs the last client's going before it
    # attaches the next, but that can be after the last test returned.
    disconnects = server.log_text().count(CLIENT_DISCONNECTED)
    mem = vfu.GuestMemory(size=1 << 20)
    try:
        client.dma_map(mem)
        driver = ionic.IonicDriver(client, mem)
        for opcode in (
            ionic.CMD_IDENTIFY,
            ionic.CMD_RESET,
            ionic.CMD_PORT_IDENTIFY,
            ionic.CMD_LIF_IDENTIFY,
        ):
            cmd = bytearray(64)
            cmd[0] = opcode
            driver.devcmd_ok(bytes(cmd))

        # ionic_dev_cmd_go(), as devcmd() does it, but the doorbell that
        # runs the command goes out after the client stops receiving, so
        # the server's reply to it fails.
        cmd = bytearray(64)
        cmd[0] = ionic.CMD_LIF_INIT
        struct.pack_into("<Q", cmd, 8, driver.lif_info)
        driver.bar0_write(ionic.BAR0_DEVCMD_CMD, bytes(cmd))
        driver.bar0_write32(ionic.BAR0_DEVCMD_DONE, 0)
        client.sock.shutdown(socket.SHUT_RD)
        client.send_command(
            vfu.VFIO_USER_REGION_WRITE,
            struct.pack("<QII", ionic.BAR0_DEVCMD, vfu.VFU_PCI_DEV_BAR0_REGION_IDX, 4)
            + struct.pack("<I", 1),
        )

        suite.check(
            server.wait_for_count(CLIENT_DISCONNECTED, disconnects + 1, timeout=30),
            "the server saw the client go",
        )
        # The command ran before the reset: the link status is in memory
        # the client still holds.
        link = mem.read(driver.lif_info + LIF_INFO_LINK_STATUS_OFF, 2)
        suite.check_eq(
            struct.unpack("<H", link)[0],
            PORT_OPER_STATUS_UP,
            "LIF_INIT ran before the hang-up took effect",
        )
        suite.check(
            LIF_STATS_WRITE_FAILED not in server.log_text(),
            "the server published no statistics after the client went",
        )
        suite.check(server.is_running(), "the server is still running")
    finally:
        client.close()
        mem.close()


def main():
    """Start a server, run the protocol tests against it, and stop it."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    parser.add_argument(
        "--pci-regs", required=True, help="path to the ernic-pci-regs program"
    )
    args = parser.parse_args()
    regs = pci.load(args.pci_regs)

    suite = Suite("rocm-ernic vfio-user protocol tests")
    with tempfile.TemporaryDirectory(prefix="ernic-vfu-") as run_dir:
        stats_path = os.path.join(run_dir, "stats")
        server = Server(
            args.server,
            os.path.join(run_dir, "vfu.sock"),
            log_path=os.path.join(run_dir, "server.log"),
            extra_args=("--stats-file", stats_path),
        )
        server.start(timeout=60)
        try:
            client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
            suite.run("version negotiation", test_negotiation, client)
            suite.run("device and region info", test_device_info, client)
            suite.run("PCI configuration space", test_config_space, client, regs)
            suite.run("BAR access and its bounds", test_bar_access, client, server)
            suite.run("DMA mapping", test_dma, client)
            suite.run("interrupts", test_irqs, client)
            suite.run("stats file", test_stats_file, stats_path)
            suite.run("FLR refused", test_flr_refused, client, regs, stats_path)
            suite.run(
                "reset and reconnect",
                test_reset_and_reconnect,
                client,
                server.socket_path,
            )
            suite.run(
                "a client that hangs up mid-command",
                test_hang_up_mid_command,
                server,
            )
        finally:
            stop_and_check(suite, server)
    suite.finish()


if __name__ == "__main__":
    main()
