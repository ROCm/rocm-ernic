"""PCI configuration-space values, from the header the server is built with.

The tests read the device's config space with the register offsets and
fields in ``linux/pci_regs.h``. Rather than keep a copy, they run the
``ernic-pci-regs`` program the build makes (``tests/pci_regs_dump.c``),
which prints the values from the header itself, and :func:`load` reads
them.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

# Bandit flags the import itself. The one call below is marked with the
# reason it is safe.
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


def field(value, mask):
    """The field @mask selects in register @value, shifted down to bit 0."""
    return (value & mask) >> ((mask & -mask).bit_length() - 1)
