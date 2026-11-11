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
# Link Capabilities' ASPM Optionality Compliance bit, which
# linux/pci_regs.h doesn't name
PCIE_LNKCAP_ASPM_OPT_COMP = 0x00400000
# The Power Management capability's version, which linux/pci_regs.h
# doesn't name
PCI_PM_VERSION = 3

# struct ionic_lif_info: the status block at 256, link_status at +10.
LIF_INFO_LINK_STATUS_OFF = 256 + 10
PORT_OPER_STATUS_UP = 1
# struct ionic_port_info: the status block at 256, its status byte at +8.
PORT_INFO_STATUS_OFF = 256 + 8
# What the guest fills an info block with, to see whether the device wrote it
INFO_POISON = b"\xa5"
PAGE_SIZE = vfu.PAGE_SIZE

# What the server logs when a client goes, and when a LIF statistics
# write to guest memory fails.
CLIENT_DISCONNECTED = "Client disconnected after"
LIF_STATS_WRITE_FAILED = "LIF stats write to"
# What the server logs when it refuses a BAR0 access across the
# register/MSI-X split.
BAR0_STRADDLE_REFUSED = "ionic BAR0 access straddling the register/MSI-X split"
# The stats file's connection state after a function-level reset
FLR_CONNECTION = "connected (PCI FLR)"
# A MAC the guest gives the LIF, which a device reset takes away
GUEST_MAC = bytes((0x02, 0x66, 0x77, 0x88, 0x99, 0xAA))
# LIF_GETATTR and LIF_SETATTR (enum ionic_cmd_opcode), and the MAC
# attribute (enum ionic_lif_attr), which is at byte 4 of both the command
# and the completion
CMD_LIF_GETATTR = 23
CMD_LIF_SETATTR = 24
LIF_ATTR_MAC = 3
LIF_ATTR_OFF = 4
MAC_LEN = 6


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


def check_pcie_link(suite, client, regs, px):
    """The link in the PCI Express capability at @px: 64 GT/s, x16, no ASPM."""
    lnkcap = pci.read32(client, px + regs.PCI_EXP_LNKCAP)
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
    suite.check_eq(
        pci.field(lnkcap, regs.PCI_EXP_LNKCAP_ASPMS), 0, "no ASPM is supported"
    )
    suite.check(
        lnkcap & PCIE_LNKCAP_ASPM_OPT_COMP,
        "ASPM Optionality Compliance is set, as every function must",
    )

    # The device has no physical link to train down
    lnksta = pci.read16(client, px + regs.PCI_EXP_LNKSTA)
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

    lnkcap2 = pci.read32(client, px + regs.PCI_EXP_LNKCAP2)
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
    lnkctl2 = pci.read16(client, px + regs.PCI_EXP_LNKCTL2)
    suite.check_eq(
        pci.field(lnkctl2, regs.PCI_EXP_LNKCTL2_TLS),
        regs.PCI_EXP_LNKCTL2_TLS_64_0GT,
        "the target link speed is 64 GT/s",
    )


def test_config_space(suite, client, regs):
    """The PCI identity, the capabilities, and the registers' initial values.

    The identity is what a guest matches the driver against; the registers
    are the ones a driver writes, read before any has.
    """
    suite.check_eq(
        pci.read16(client, regs.PCI_VENDOR_ID), PCI_VENDOR_ID_PENSANDO, "vendor id"
    )
    suite.check_eq(
        pci.read16(client, regs.PCI_DEVICE_ID), PCI_DEVICE_ID_ROCM_ERNIC, "device id"
    )
    suite.check_eq(
        pci.read16(client, regs.PCI_SUBSYSTEM_VENDOR_ID),
        PCI_VENDOR_ID_PENSANDO,
        "subsystem vendor id",
    )
    suite.check_eq(
        pci.read16(client, regs.PCI_SUBSYSTEM_ID),
        PCI_SUBDEVICE_ID_ROCM_ERNIC,
        "subsystem device id",
    )
    suite.check_eq(
        pci.read16(client, regs.PCI_CLASS_DEVICE) >> 8,
        PCI_BASE_CLASS_NETWORK,
        "base class is network",
    )
    suite.check_eq(
        client.read_config(regs.PCI_HEADER_TYPE, 1)[0] & regs.PCI_HEADER_TYPE_MASK,
        regs.PCI_HEADER_TYPE_NORMAL,
        "header type is a normal function",
    )

    suite.check(
        pci.read16(client, regs.PCI_STATUS) & regs.PCI_STATUS_CAP_LIST,
        "status says there is a capability list",
    )
    px = pci.find_capability(client, regs, regs.PCI_CAP_ID_EXP)
    suite.check(px is not None, "the PCI Express capability is on the list")
    msix = pci.find_capability(client, regs, regs.PCI_CAP_ID_MSIX)
    suite.check(msix is not None, "the MSI-X capability is on the list")
    pm = pci.find_capability(client, regs, regs.PCI_CAP_ID_PM)
    suite.check(pm is not None, "the Power Management capability is on the list")
    if px is None or msix is None or pm is None:
        return

    flags = pci.read16(client, px + regs.PCI_EXP_FLAGS)
    suite.check_eq(
        pci.field(flags, regs.PCI_EXP_FLAGS_VERS), 2, "PCI Express capability version"
    )
    suite.check_eq(
        pci.field(flags, regs.PCI_EXP_FLAGS_TYPE),
        regs.PCI_EXP_TYPE_ENDPOINT,
        "PCI Express device type is Endpoint",
    )
    devcap = pci.read32(client, px + regs.PCI_EXP_DEVCAP)
    suite.check(devcap & regs.PCI_EXP_DEVCAP_FLR, "the device advertises FLR")
    suite.check(
        devcap & regs.PCI_EXP_DEVCAP_RBER,
        "Role-Based Error Reporting is set, as every function must",
    )
    # Required of an endpoint issuing its own non-posted requests, as the
    # device does in reading guest memory
    suite.check(
        pci.read32(client, px + regs.PCI_EXP_DEVCAP2)
        & regs.PCI_EXP_DEVCAP2_COMP_TMOUT_DIS,
        "the device supports Completion Timeout Disable",
    )
    check_pcie_link(suite, client, regs, px)

    pmc = pci.read16(client, pm + regs.PCI_PM_PMC)
    suite.check_eq(
        pci.field(pmc, regs.PCI_PM_CAP_VER_MASK),
        PCI_PM_VERSION,
        "Power Management capability version",
    )
    suite.check_eq(
        pmc & ~regs.PCI_PM_CAP_VER_MASK,
        0,
        "no D1 or D2, no PME, no auxiliary power, and no special initialization",
    )
    suite.check_eq(
        client.read_config(pm + regs.PCI_PM_DATA_REGISTER, 1)[0],
        0,
        "no Power Management Data register",
    )

    config = pci.ResetRegs(client, regs, px, msix, pm)
    config.check(suite, config.initial(), "before any driver")
    suite.check_eq(config.unmasked(), [], "every MSI-X vector is masked at startup")


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


def find_reset_regs(suite, client, regs):
    """The ResetRegs for @client, or None without the capabilities."""
    px = pci.find_capability(client, regs, regs.PCI_CAP_ID_EXP)
    msix = pci.find_capability(client, regs, regs.PCI_CAP_ID_MSIX)
    pm = pci.find_capability(client, regs, regs.PCI_CAP_ID_PM)
    if not suite.check(
        px is not None and msix is not None and pm is not None,
        "there are PCI Express, MSI-X and Power Management capabilities",
    ):
        return None
    return pci.ResetRegs(client, regs, px, msix, pm)


def wait_for_write_count(stats_path, count, timeout=15.0):
    """Wait until the stats file's write count reaches @count.

    The server's main loop rewrites the file, about once a second, at the
    top of a pass that then polls the device, so a count two past one read
    after a command shows a whole pass ran after it. Returns whether it got
    there.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        stats = read_stats(stats_path)
        if stats is not None and stats[1] >= count:
            return True
        time.sleep(0.05)
    return False


def info_devcmd(driver, opcode, info=0):
    """Run device command @opcode, with the info block address @info."""
    cmd = bytearray(64)
    cmd[0] = opcode
    struct.pack_into("<Q", cmd, 8, info)
    driver.devcmd_ok(bytes(cmd))


def poison_info(driver):
    """Fill the port and LIF info blocks with INFO_POISON."""
    for gpa in (driver.port_info, driver.lif_info):
        driver.mem.write(gpa, INFO_POISON * PAGE_SIZE)


def check_info_untouched(suite, driver, stats_path, passes, what):
    """Check the info blocks stay poisoned for @passes of the server's loop.

    The server is shown to have made the passes by the stats file's write
    count, which it raises at the top of each pass that polls the device.
    """
    poisoned = INFO_POISON * PAGE_SIZE
    stats = read_stats(stats_path)
    if suite.check(stats is not None, "the stats file has a write count"):
        suite.check(
            wait_for_write_count(stats_path, stats[1] + passes),
            f"the server made passes {what}",
        )
    suite.check(
        driver.mem.read(driver.port_info, PAGE_SIZE) == poisoned
        and driver.mem.read(driver.lif_info, PAGE_SIZE) == poisoned,
        f"the device left the info blocks alone {what}",
    )


def check_info_written(suite, driver, when):
    """Wait for, and check, what PORT_INIT and LIF_INIT publish, @when."""
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and (
        driver.mem.read(driver.lif_info + LIF_INFO_LINK_STATUS_OFF, 1)[0]
        != PORT_OPER_STATUS_UP
    ):
        time.sleep(0.05)
    suite.check_eq(
        driver.mem.read(driver.port_info + PORT_INFO_STATUS_OFF, 1)[0],
        PORT_OPER_STATUS_UP,
        f"the port status written {when}",
    )
    suite.check_eq(
        driver.mem.read(driver.lif_info + LIF_INFO_LINK_STATUS_OFF, 1)[0],
        PORT_OPER_STATUS_UP,
        f"the LIF link status written {when}",
    )


def check_writes_held(suite, client, regs, stats_path, states):
    """Check the device writes nothing into guest memory while stopped.

    @states is ((stop, stopped), (go, going)): functions that stop and
    restart the device, and words for the states they leave it in, for the
    messages. Stopped, the device must not write the port info and LIF
    status PORT_INIT and LIF_INIT publish, nor the statistics it rewrites
    into them every second. Restarted, it writes what it held, and stopped
    again, it stops again: the statistics aren't rewritten over three
    passes' worth of seconds. The Command register is put back as it was.
    """
    (stop_func, stopped), (go_func, going) = states
    command = pci.read16(client, regs.PCI_COMMAND)
    with vfu.GuestMemory(size=1 << 20) as mem:
        client.dma_map(mem)
        driver = ionic.IonicDriver(client, mem)
        try:
            stop_func()
            poison_info(driver)
            info_devcmd(driver, ionic.CMD_RESET)
            info_devcmd(driver, ionic.CMD_PORT_INIT, driver.port_info)
            info_devcmd(driver, ionic.CMD_LIF_INIT, driver.lif_info)
            check_info_untouched(
                suite, driver, stats_path, 2, f"after LIF_INIT {stopped}"
            )

            go_func()
            check_info_written(suite, driver, going)

            stop_func()
            poison_info(driver)
            check_info_untouched(suite, driver, stats_path, 3, f"{stopped} again")
        finally:
            go_func()
            # Nothing of this client's for the device to write to later
            info_devcmd(driver, ionic.CMD_RESET)
            pci.write16(client, regs.PCI_COMMAND, command)
            client.dma_unmap(mem.base, mem.size)


def test_bus_master(suite, client, regs, stats_path):
    """Bus Master Enable, which the guest clears to stop the device's DMA.

    Linux clears it when a driver unbinds and before kexec starts the next
    kernel, and nothing else stops a device whose driver has no shutdown
    hook.
    """
    check_writes_held(
        suite,
        client,
        regs,
        stats_path,
        (
            (lambda: client.set_bus_master(False), "with BME clear"),
            (lambda: client.set_bus_master(True), "once BME is set"),
        ),
    )


def test_power_state(suite, client, regs):
    """PowerState, the one field of the Power Management capability a guest
    writes.

    The device supports D0 and D3hot, which every function must, and no
    other state: a write of D1 or D2 completes, but is discarded. Every
    other bit is read-only, so the writes Linux makes, which put back
    what they read, and the write of 0 its pci_power_up() makes, leave
    No_Soft_Reset set.
    """
    pm = pci.find_capability(client, regs, regs.PCI_CAP_ID_PM)
    if not suite.check(pm is not None, "there is a Power Management capability"):
        return
    pmcsr_off = pm + regs.PCI_PM_CTRL
    in_d0 = regs.PCI_PM_CTRL_NO_SOFT_RESET | pci.PM_D0
    in_d3hot = regs.PCI_PM_CTRL_NO_SOFT_RESET | pci.PM_D3HOT
    # What is written to PMCSR, in turn, and what it then reads
    writes = (
        (in_d0 | pci.PM_D1, in_d0, "D1 from D0"),
        (in_d0 | pci.PM_D2, in_d0, "D2 from D0"),
        (0xFFFF, in_d3hot, "every bit, which sets D3hot"),
        (in_d0 | pci.PM_D1, in_d3hot, "D1 from D3hot"),
        (in_d0 | pci.PM_D2, in_d3hot, "D2 from D3hot"),
        (0, in_d0, "0"),
    )

    try:
        for value, expected, what in writes:
            pci.write16(client, pmcsr_off, value)
            suite.check_eq(
                pci.read16(client, pmcsr_off), expected, "PMCSR after %s", what
            )

        # The read-only registers: the capabilities, and PMCSR's bridge
        # extensions and the Data register written with it in one dword
        pmc = pci.read16(client, pm + regs.PCI_PM_PMC)
        pci.write16(client, pm + regs.PCI_PM_PMC, 0xFFFF)
        suite.check_eq(
            pci.read16(client, pm + regs.PCI_PM_PMC), pmc, "PMC after a write"
        )
        client.write_config(pmcsr_off, struct.pack("<I", 0xFFFFFFFF))
        suite.check_eq(
            pci.read32(client, pmcsr_off),
            in_d3hot,
            "PMCSR and the Data register after a 32-bit write of every bit",
        )
    finally:
        pci.set_power_state(client, regs, pm, pci.PM_D0)


def test_d3hot(suite, client, regs, stats_path):
    """D3hot, where the function may do no DMA, Bus Master Enable or not.

    The guest's PCI core puts a function in D3hot before suspending, and
    for runtime power management. The device holds its work there as with
    BME clear. No_Soft_Reset is set, so going back to D0 keeps the
    device's state, and it then writes what it held. A VMM unmaps the
    BARs in D3hot, so a guest can't send the device commands sent here,
    but they show that the device would DMA nothing for them.
    """
    pm = pci.find_capability(client, regs, regs.PCI_CAP_ID_PM)
    if not suite.check(pm is not None, "there is a Power Management capability"):
        return

    def stop():
        client.set_bus_master(True)
        pci.set_power_state(client, regs, pm, pci.PM_D3HOT)

    check_writes_held(
        suite,
        client,
        regs,
        stats_path,
        (
            (stop, "in D3hot with BME set"),
            (lambda: pci.set_power_state(client, regs, pm, pci.PM_D0), "once in D0"),
        ),
    )


def test_flr(suite, client, regs, stats_path):
    """A function-level reset, as Linux's pcie_flr() starts it.

    It sets Initiate FLR in Device Control along with the register's
    other bits, in one 16-bit write. libvfio-user calls the reset callback
    before it answers, so the stats file already counts the reset, and
    every register a driver wrote is back at its initial value, but for
    Max_Payload_Size and Link Control 2, the link's state, which an FLR
    keeps. The device's own state is reset
    too: the MAC the guest gave the LIF goes back to the station MAC, and
    the last device command no longer reads as done.
    """
    config = find_reset_regs(suite, client, regs)
    before = read_resets(stats_path)
    if config is None or not suite.check(
        before is not None, "the stats file has a reset count"
    ):
        return

    # The driver model only allocates from it, for devcmds that DMA nothing
    with vfu.GuestMemory(size=1 << 20) as mem:
        driver = ionic.IonicDriver(client, mem)
        station = lif_mac(driver)
        set_lif_mac(driver, GUEST_MAC)
        config.dirty(suite)

        pci.start_flr(client, regs, config.px)
        after = read_resets(stats_path)
        if suite.check(after is not None, "the stats file still has a reset count"):
            suite.check_eq(after[0], FLR_CONNECTION, "the connection state")
            suite.check_eq(after[1], before[1] + 1, "the reset count")

        # What dirty() set: the 256-byte Max_Payload_Size, and the 2.5 GT/s
        # Target Link Speed
        expected = config.initial()
        expected["Device Control"] = (
            expected["Device Control"] & ~regs.PCI_EXP_DEVCTL_PAYLOAD
        ) | regs.PCI_EXP_DEVCTL_PAYLOAD_256B
        expected["Link Control 2"] = regs.PCI_EXP_LNKCTL2_TLS_2_5GT
        config.check(suite, expected, "after an FLR")
        suite.check_eq(
            driver.bar0_read32(ionic.BAR0_DEVCMD_DONE),
            0,
            "the last device command no longer reads as done after an FLR",
        )
        suite.check_eq(lif_mac(driver), station, "the MAC after an FLR")


def lif_mac(driver):
    """The MAC LIF_GETATTR reports."""
    cmd = bytearray(64)
    cmd[0] = CMD_LIF_GETATTR
    cmd[1] = LIF_ATTR_MAC
    driver.devcmd_ok(bytes(cmd))
    return driver.bar0_read(ionic.BAR0_DEVCMD_COMP + LIF_ATTR_OFF, MAC_LEN)


def set_lif_mac(driver, mac):
    """Give the LIF @mac, with LIF_SETATTR."""
    cmd = bytearray(64)
    cmd[0] = CMD_LIF_SETATTR
    cmd[1] = LIF_ATTR_MAC
    cmd[LIF_ATTR_OFF : LIF_ATTR_OFF + MAC_LEN] = mac
    driver.devcmd_ok(bytes(cmd))


def test_reset_and_reconnect(suite, client, regs, socket_path):
    """A device reset, and a second client after the first detaches.

    Each returns the device to its state before any client attached: the
    MAC the guest gave the LIF goes back to the station MAC, the last
    device command no longer reads as done, and every config-space
    register a driver wrote is back at its initial value, Max_Payload_Size
    and Link Control 2 included. The first client leaves its MAC and registers behind without
    resetting the device, as a VM that dies does.
    """
    config = find_reset_regs(suite, client, regs)
    if config is None:
        return

    # The driver model only allocates from it, for devcmds that DMA nothing.
    # Leaving the block detaches the first client.
    with client, vfu.GuestMemory(size=1 << 20) as mem:
        driver = ionic.IonicDriver(client, mem)
        station = lif_mac(driver)
        set_lif_mac(driver, GUEST_MAC)
        config.dirty(suite)
        client.reset()
        suite.check_eq(
            client.read_bar32(0, 0), IONIC_DEV_SIG, "the device answers after a reset"
        )
        suite.check_eq(
            driver.bar0_read32(ionic.BAR0_DEVCMD_DONE),
            0,
            "the last device command no longer reads as done after a reset",
        )
        suite.check_eq(lif_mac(driver), station, "the MAC after a reset")
        config.check(suite, config.initial(), "after a reset")
        set_lif_mac(driver, GUEST_MAC)
        config.dirty(suite)

    with (
        vfu.VfioUserClient(socket_path).connect(timeout=30) as second,
        vfu.GuestMemory(size=1 << 20) as mem,
    ):
        suite.check_eq(
            struct.unpack_from("<H", second.read_config(0, 2))[0],
            PCI_VENDOR_ID_PENSANDO,
            "a second client can attach after the first detaches",
        )
        driver = ionic.IonicDriver(second, mem)
        suite.check_eq(
            lif_mac(driver),
            station,
            "the MAC a second client finds after the first changed it",
        )
        config = pci.ResetRegs(second, regs, config.px, config.msix, config.pm)
        config.check(
            suite, config.initial(), "for a second client after the first changed it"
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
        # Without it, the device would hold the writes this is about
        client.set_bus_master(True)
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
    args, regs = pci.parse_args(parser)

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
            suite.run("Bus Master Enable", test_bus_master, client, regs, stats_path)
            suite.run("power state", test_power_state, client, regs)
            suite.run("D3hot", test_d3hot, client, regs, stats_path)
            suite.run("function-level reset", test_flr, client, regs, stats_path)
            suite.run(
                "reset and reconnect",
                test_reset_and_reconnect,
                client,
                regs,
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
