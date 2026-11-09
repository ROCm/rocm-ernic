"""PCI configuration-space values, from the header the server is built with.

The tests read the device's config space with the register offsets and
fields in ``linux/pci_regs.h``. Rather than keep a copy, they run the
``ernic-pci-regs`` program the build makes (``tests/pci_regs_dump.c``),
which prints the values from the header itself, and :func:`load` reads
them. The helpers below read and write config space through a
:class:`ernic.vfu.VfioUserClient` with those values.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

# Bandit flags the import itself. The one call below is marked with the
# reason it is safe.
import struct
import subprocess  # nosec B404
import types


class PciRegsError(Exception):
    """The program printing the header's values failed or printed nonsense."""


def load(program):
    """Run @program, ernic-pci-regs, and return the values it prints.

    The result has one attribute per value, named as in the header, e.g.
    ``regs.PCI_EXP_DEVCTL``.
    """
    # No shell. The program is the one the build made, which the test's
    # caller names.
    result = subprocess.run(  # nosec B603
        [program], capture_output=True, text=True, timeout=30, check=False
    )
    if result.returncode != 0:
        raise PciRegsError(
            f"{program} exited with status {result.returncode}: {result.stderr}"
        )
    values = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or not fields[1].isdigit():
            raise PciRegsError(f"{program} printed {line!r}")
        values[fields[0]] = int(fields[1])
    return types.SimpleNamespace(**values)


def parse_args(parser):
    """Parse the command line with @parser, given a --pci-regs option.

    The option names the ernic-pci-regs program. Returns the parsed
    arguments, and the values :func:`load` reads from that program.
    """
    parser.add_argument(
        "--pci-regs", required=True, help="path to the ernic-pci-regs program"
    )
    args = parser.parse_args()
    return args, load(args.pci_regs)


def field(value, mask):
    """The field @mask selects in register @value, shifted down to bit 0."""
    return (value & mask) >> ((mask & -mask).bit_length() - 1)


def read16(client, offset):
    """The little-endian 16-bit config-space register at @offset."""
    return struct.unpack("<H", client.read_config(offset, 2))[0]


def read32(client, offset):
    """The little-endian 32-bit config-space register at @offset."""
    return struct.unpack("<I", client.read_config(offset, 4))[0]


def write16(client, offset, value):
    """Write @value to the 16-bit config-space register at @offset.

    One 2-byte write, as a guest makes it: libvfio-user refuses any other
    size in the PCI Express capability.
    """
    client.write_config(offset, struct.pack("<H", value))


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


def start_flr(client, regs, px):
    """Reset the function, as Linux's pcie_flr() does.

    It sets Initiate FLR in Device Control, in the PCI Express capability
    at @px, keeping the register's other bits. libvfio-user resets the
    device before it answers the write, so the reset is over when this
    returns.
    """
    devctl = read16(client, px + regs.PCI_EXP_DEVCTL)
    write16(client, px + regs.PCI_EXP_DEVCTL, devctl | regs.PCI_EXP_DEVCTL_BCR_FLR)
