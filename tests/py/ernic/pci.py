"""PCI configuration-space values, from the header the server is built with.

The tests read the device's config space with the register offsets and
fields in ``linux/pci_regs.h``. Rather than keep a copy, they run the
``ernic-pci-regs`` program the build makes (``tests/pci_regs_dump.c``),
which prints the values from the header itself, and :func:`load` reads
them. The helpers below read and write config space through a
:class:`ernic.vfu.VfioUserClient` with those values, and
:class:`ResetRegs` reads the device's registers that a reset puts back.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

# Bandit flags the import itself. The one call below is marked with the
# reason it is safe.
import struct
import subprocess  # nosec B404
import types

# The PowerState values of the power states, in the Power Management
# capability's PMCSR, which linux/pci_regs.h doesn't name
PM_D0 = 0
PM_D1 = 1
PM_D2 = 2
PM_D3HOT = 3


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


def set_power_state(client, regs, pm, state):
    """Put the function in power @state, as Linux's pci_set_power_state() does.

    It writes PowerState in PMCSR, in the Power Management capability at
    @pm, keeping the register's other bits.
    """
    pmcsr = read16(client, pm + regs.PCI_PM_CTRL)
    write16(
        client,
        pm + regs.PCI_PM_CTRL,
        (pmcsr & ~regs.PCI_PM_CTRL_STATE_MASK) | state,
    )


# What ResetRegs.dirty() writes into the 8-bit header registers a reset
# puts back: a 64-byte cache line, a latency timer and an Interrupt Line.
# The device has no INTx, so the spec would let Interrupt Line be
# hardwired to 0, but libvfio-user keeps it read-write.
DIRTY_CACHE_LINE_SIZE = 16
DIRTY_LATENCY_TIMER = 0x40
DIRTY_INTERRUPT_LINE = 11


class ResetRegs:
    """The config-space registers a reset puts back, read through @client.

    @px, @msix and @pm are the offsets of the PCI Express, MSI-X and Power
    Management capabilities, and @regs the linux/pci_regs.h values.
    """

    def __init__(self, client, regs, px, msix, pm):
        self.client = client
        self.regs = regs
        self.px = px
        self.msix = msix
        self.pm = pm

    def vector_ctrl(self, vector):
        """The BAR and offset of MSI-X table entry @vector's Vector Control."""
        regs = self.regs
        table = read32(self.client, self.msix + regs.PCI_MSIX_TABLE)
        return (
            table & regs.PCI_MSIX_TABLE_BIR,
            (table & regs.PCI_MSIX_TABLE_OFFSET)
            + vector * regs.PCI_MSIX_ENTRY_SIZE
            + regs.PCI_MSIX_ENTRY_VECTOR_CTRL,
        )

    def unmasked(self):
        """The MSI-X vectors whose table entry's Mask bit is clear."""
        regs = self.regs
        flags = read16(self.client, self.msix + regs.PCI_MSIX_FLAGS)
        unmasked = []
        for vector in range(field(flags, regs.PCI_MSIX_FLAGS_QSIZE) + 1):
            ctrl = self.client.read_bar32(*self.vector_ctrl(vector))
            if not ctrl & regs.PCI_MSIX_ENTRY_CTRL_MASKBIT:
                unmasked.append(vector)
        return unmasked

    def read(self):
        """The registers, by name.

        Only the writable bits of MSI-X Message Control are read, and the
        first MSI-X vector stands for the table.
        """
        client, regs = self.client, self.regs
        msix_writable = regs.PCI_MSIX_FLAGS_ENABLE | regs.PCI_MSIX_FLAGS_MASKALL
        return {
            "Command": read16(client, regs.PCI_COMMAND),
            "Cache Line Size": client.read_config(regs.PCI_CACHE_LINE_SIZE, 1)[0],
            "Latency Timer": client.read_config(regs.PCI_LATENCY_TIMER, 1)[0],
            "Interrupt Line": client.read_config(regs.PCI_INTERRUPT_LINE, 1)[0],
            "Device Control": read16(client, self.px + regs.PCI_EXP_DEVCTL),
            "Device Control 2": read16(client, self.px + regs.PCI_EXP_DEVCTL2),
            "Link Control 2": read16(client, self.px + regs.PCI_EXP_LNKCTL2),
            "MSI-X Message Control": read16(client, self.msix + regs.PCI_MSIX_FLAGS)
            & msix_writable,
            "MSI-X vector 0 Vector Control": client.read_bar32(*self.vector_ctrl(0)),
            "PMCSR": read16(client, self.pm + regs.PCI_PM_CTRL),
        }

    def initial(self):
        """What read() returns before a driver writes, and after a reset.

        Device Control has the spec's defaults: Relaxed Ordering and No
        Snoop enabled, and a 512-byte Max_Read_Request_Size. Link Control
        2's Target Link Speed is the fastest the device supports. The
        function is in D0, with No_Soft_Reset set.
        """
        regs = self.regs
        return {
            "Command": 0,
            "Cache Line Size": 0,
            "Latency Timer": 0,
            "Interrupt Line": 0,
            "Device Control": regs.PCI_EXP_DEVCTL_RELAX_EN
            | regs.PCI_EXP_DEVCTL_NOSNOOP_EN
            | regs.PCI_EXP_DEVCTL_READRQ_512B,
            "Device Control 2": 0,
            "Link Control 2": regs.PCI_EXP_LNKCTL2_TLS_64_0GT,
            "MSI-X Message Control": 0,
            "MSI-X vector 0 Vector Control": regs.PCI_MSIX_ENTRY_CTRL_MASKBIT,
            "PMCSR": regs.PCI_PM_CTRL_NO_SOFT_RESET | PM_D0,
        }

    def dirty(self, suite):
        """Write what a driver might into every register.

        Device Control gets a 256-byte Max_Payload_Size, which an FLR
        keeps, with Relaxed Ordering and No Snoop off and a 4096-byte
        Max_Read_Request_Size. Link Control 2, which an FLR also keeps,
        gets a 2.5 GT/s Target Link Speed. The function goes to D3hot.
        Checks that every register took its new value, or a check that a
        reset put it back would pass without one.
        """
        client, regs = self.client, self.regs
        write16(
            client, regs.PCI_COMMAND, regs.PCI_COMMAND_MEMORY | regs.PCI_COMMAND_MASTER
        )
        client.write_config(regs.PCI_CACHE_LINE_SIZE, bytes((DIRTY_CACHE_LINE_SIZE,)))
        client.write_config(regs.PCI_LATENCY_TIMER, bytes((DIRTY_LATENCY_TIMER,)))
        client.write_config(regs.PCI_INTERRUPT_LINE, bytes((DIRTY_INTERRUPT_LINE,)))
        write16(
            client,
            self.px + regs.PCI_EXP_DEVCTL,
            regs.PCI_EXP_DEVCTL_PAYLOAD_256B | regs.PCI_EXP_DEVCTL_READRQ_4096B,
        )
        write16(
            client,
            self.px + regs.PCI_EXP_DEVCTL2,
            regs.PCI_EXP_DEVCTL2_COMP_TMOUT_DIS,
        )
        write16(client, self.px + regs.PCI_EXP_LNKCTL2, regs.PCI_EXP_LNKCTL2_TLS_2_5GT)
        flags = read16(client, self.msix + regs.PCI_MSIX_FLAGS)
        write16(
            client,
            self.msix + regs.PCI_MSIX_FLAGS,
            flags | regs.PCI_MSIX_FLAGS_ENABLE | regs.PCI_MSIX_FLAGS_MASKALL,
        )
        client.write_bar32(*self.vector_ctrl(0), 0)
        set_power_state(client, regs, self.pm, PM_D3HOT)

        initial = self.initial()
        for name, value in self.read().items():
            suite.check(
                value != initial[name],
                "%s moved off its initial value, to %#x",
                name,
                value,
            )

    def check(self, suite, expected, when):
        """Check read() returns @expected, @when (e.g. "after an FLR")."""
        for name, value in self.read().items():
            suite.check_eq(value, expected[name], "%s %s", name, when)
