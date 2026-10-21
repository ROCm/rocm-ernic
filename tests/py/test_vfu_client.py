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
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# pylint: disable=wrong-import-position
from ernic import vfu
from ernic.check import Suite
from ernic.server import Server, stop_and_check

# The emulated device's PCI identity, which find_rdma_device() matches in
# full rather than on a shared vendor or device id.
PCI_VENDOR_ID_PENSANDO = 0x1DD8
PCI_DEVICE_ID_ROCM_ERNIC = 0x1002
PCI_SUBDEVICE_ID_ROCM_ERNIC = 0x5400

BAR0_SIZE = 64 << 10
IONIC_DEV_SIG = 0x44455649  # "DEVI"


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


def test_config_space(suite, client):
    """The PCI identity a guest matches the driver against."""
    cfg = client.read_config(0, 64)
    vendor, device = struct.unpack_from("<HH", cfg, 0)
    subvendor, subdevice = struct.unpack_from("<HH", cfg, 0x2C)
    suite.check_eq(vendor, PCI_VENDOR_ID_PENSANDO, "vendor id")
    suite.check_eq(device, PCI_DEVICE_ID_ROCM_ERNIC, "device id")
    suite.check_eq(subvendor, PCI_VENDOR_ID_PENSANDO, "subsystem vendor id")
    suite.check_eq(subdevice, PCI_SUBDEVICE_ID_ROCM_ERNIC, "subsystem device id")
    suite.check_eq(cfg[0x0B], 0x02, "base class is network")
    suite.check_eq(cfg[0x0E] & 0x7F, 0, "header type is a normal function")


def test_bar_access(suite, client):
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
    mem = vfu.GuestMemory(size=4 << 20)
    try:
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
    finally:
        mem.close()


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


def main():
    """Start a server, run the protocol tests against it, and stop it."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    suite = Suite("rocm-ernic vfio-user protocol tests")
    with tempfile.TemporaryDirectory(prefix="ernic-vfu-") as run_dir:
        server = Server(
            args.server,
            os.path.join(run_dir, "vfu.sock"),
            log_path=os.path.join(run_dir, "server.log"),
        )
        server.start(timeout=60)
        try:
            client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
            suite.run("version negotiation", test_negotiation, client)
            suite.run("device and region info", test_device_info, client)
            suite.run("PCI configuration space", test_config_space, client)
            suite.run("BAR access and its bounds", test_bar_access, client)
            suite.run("DMA mapping", test_dma, client)
            suite.run("interrupts", test_irqs, client)
            suite.run(
                "reset and reconnect",
                test_reset_and_reconnect,
                client,
                server.socket_path,
            )
        finally:
            stop_and_check(suite, server)
    suite.finish()


if __name__ == "__main__":
    main()
