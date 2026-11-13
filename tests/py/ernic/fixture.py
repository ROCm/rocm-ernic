# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

"""The part of a test fixture that is the same for every driver test.

Connecting over the vfio-user socket, mapping guest memory and bringing
the driver up is identical whatever a test goes on to do with the device,
so it lives here rather than in each test. What a test adds on top --
queue pairs, memory regions, an Ethernet queue -- is its own business and
belongs in its own subclass.
"""

from . import ionic, vfu

# How much guest memory a fixture maps. Large enough for the rings and
# the memory regions every test registers, and small enough that mapping
# it is not what the test spends its time on.
GUEST_MEM_SIZE = 32 << 20

# Long enough that a server still starting under a sanitizer is not
# mistaken for one that will never answer.
CONNECT_TIMEOUT = 30


# The pieces a test reaches for; bundling them is the point.
# pylint: disable-next=too-few-public-methods
class BaseFixture:
    """A connected client and a brought-up driver.

    With a CMB (--cmb), the driver maps it, as QEMU maps BAR4 into a
    guest, so WQEs reach it as a guest's stores do.
    """

    def __init__(self, server):
        self.client = vfu.VfioUserClient(server.socket_path).connect(
            timeout=CONNECT_TIMEOUT
        )
        self.mem = vfu.GuestMemory(size=GUEST_MEM_SIZE)
        self.client.dma_map(self.mem)
        self.driver = ionic.IonicDriver(self.client, self.mem)
        if vfu.VFU_PCI_DEV_BAR4_REGION_IDX in self.client.regions:
            self.driver.map_cmb()
        self.driver.bringup()

    def close(self):
        """Drop the client, as a guest shutting down does."""
        self.driver.close()
        self.mem.close()
        self.client.close()
