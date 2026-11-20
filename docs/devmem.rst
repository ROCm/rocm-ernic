.. SPDX-License-Identifier: MIT
.. Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

Device Memory TCP (devmem)
==========================

Device memory TCP lets a Linux application send a TCP stream from, or
receive one into, a dma-buf: memory the kernel never reads or copies,
such as a GPU's. The NIC's driver hands the device the buffer's DMA
addresses, and the payload goes between that memory and the wire.

This page describes a **proof of concept**: devmem TCP on the ionic
NIC rocm-ernic emulates, with patches to the upstream Linux ionic
driver that are not upstream. It was done to find out what the driver
and the device need, with the emulator standing in for hardware.

.. warning::

   Nothing here has run on ionic hardware. The driver series are RFCs
   that have not been posted. The receive side needs a device
   interface that was invented for the purpose and that no ionic
   firmware has. Treat every result as true of two guests of this
   emulator, on the kernels named, and of nothing else.

Each result below was observed in a test, and names the note that
reports it. The few statements taken from the source and not run say
so. The notes are the working record of the project, in
:project-file:`docs/devmem/notes`; its
:project-file:`README <docs/devmem/notes/README.md>` says how to read
them.

What works
----------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - What
     - Status
   * - Transmit from a dma-buf, ``v7.2.x`` guest kernel
     - Works, with the backport series ``devmem-tx``. Tested on
       ``v7.2.15`` and on the stock ``v7.2.6`` of the ROCm 10.2 guest
       image.
   * - Transmit from a dma-buf, ``v7.4-rc3``
     - Works, with the mainline form of the series, under KASAN and
       lockdep.
   * - Transmit from GPU VRAM
     - Works where the networking core attaches to the dma-buf
       peer-to-peer (commit ``69ab87acc614``, first in ``v7.4-rc1``),
       and only with the pieces listed under
       `Sending from GPU memory`_. Shown on ``v7.4-rc3``, and on
       ``v7.2.6`` with a hand backport of that commit which is not in
       this repository. On an unpatched ``v7.2.x`` the transfer works
       but the data is read from system memory.
   * - Receive into a dma-buf
     - Works against the emulator's ``header-split`` mode only, on
       ``v7.4-rc3``. The device interface is invented.
   * - ionic hardware
     - Not tested. A runbook exists and has not been run.

Transmit
^^^^^^^^

A guest binds a dma-buf to its ionic netdev (``NETDEV_CMD_BIND_TX``)
and sends from it with ``sendmsg(MSG_ZEROCOPY)`` and
``SCM_DEVMEM_DMABUF``. The driver needs six patches, none of them to
the core (the backport has a seventh, an upstream fix its base
lacks): ionic already maps frags the right way, and the series makes
the unmap paths safe for memory that has no ``struct page``, counts
what it has to drop, and sets ``netdev->netmem_tx``.

The device needs nothing new, but it must offer transmit
scatter-gather and checksum offload: the driver sets ``netmem_tx``
only when it was granted both, and without it the bind fails with
``EOPNOTSUPP``. That is from the source, not from a run: every
``EOPNOTSUPP`` in the notes is from a driver without the series, and
the series was never loaded against a server without the offloads.
The emulator offers both; see "Transmit offloads" in :doc:`ionic`.

Two guests of the emulator ran the same test matrix on two kernels
(``test-tx-7.2.15.md``, ``test-tx-7.4-rc3.md``):

- On ``v7.2.15`` with the backport, a 360 s run moved 64.4 GiB, and
  on ``v7.4-rc3`` with the mainline series 62.0 GiB, with every byte
  checked at the receiver and every zerocopy completion returned,
  none reporting a copy.
- Unaligned offsets and lengths, MTU 1500 and 9000, TSO on and off,
  and teardown with sends in flight passed on both, and IPv6 on
  ``v7.4-rc3``. A probe showed the data skbs reaching the driver
  unreadable, with ``net_iov`` frags: nothing fell back to a copy.
- With transmit checksum or scatter-gather turned off on the sender,
  nothing corrupt reached the receiver.
- On ``v7.4-rc3`` no KASAN, lockdep or DMA-API unmap report came from
  the driver.

A second pass re-ran most of the matrix with a sender, a receiver and
a probe written independently of the first, from a random buffer, and
confirmed every result it re-ran (``test-tx-verification.md``). It
did not re-run the 360 s runs, IPv6, MTU 9198, the XDP row or the
100 and 110-byte threshold, and on ``v7.2.15`` it re-ran a subset.

The matrix also ran on a third stack, two guests of the ROCm 10.2
image that qemu-minimal publishes, on that image's stock ``v7.2.6``
kernel (the one `Trying it`_ is about), with the backport built on
top of the image's other ionic series: 35 cases, no mismatching byte
at the receiver, and a 360 s run that moved 74.1 GiB with 79571 of
79571 completions (``rocm102-devmem.md``). The MTU, IPv6, XDP,
module-unload and reconfiguration rows were not run there. An
independent pass over that stack re-ran a subset with its own tools
and refuted no pass (``rocm102-verification.md``).

Receive
^^^^^^^

To receive into memory the kernel cannot read, the device has to put
each segment's headers in one buffer and its payload in another. No
ionic firmware does. The emulator does, in its ``header-split`` mode
(``--nic-profile header-split``; "Receive header split" in
:doc:`ionic`), through an interface invented for this work and
written down in ``rx-interface.md``.

With that mode and the eight-patch receive series, on ``v7.4-rc3``
with KASAN and lockdep (``test-rx.md``):

- A guest bound a dma-buf to a receive queue (``NETDEV_CMD_BIND_RX``)
  and received into it, from an ordinary sender and from the other
  guest's devmem transmit, with every byte validated and no payload
  arriving through the linear area. Transfers up to 1.5 GB were
  validated.
- The binding survived interface down and up, ring, MTU and channel
  changes, and a PCI function reset in mid-stream.
- With the mode off the device and the driver behave as before.

Limits
------

A send in small pieces can stall
   ionic sends at most 16 buffers per packet: the head and 15
   scatter-gather elements. A devmem ``sendmsg()`` makes one frag per
   iovec entry per page it touches, and the driver cannot copy
   unreadable frags together, so it drops a packet with more than 15
   and counts it in ``tx_unreadable_drop``. TCP retransmits the same
   packet, so the connection does not recover: the sender blocks, or
   its ``sendmsg()`` ends with ``ETIMEDOUT``. At MTU 1500, sends of
   110-byte pieces passed and 100-byte pieces stalled; at MTU 9000,
   pieces of 128 and of 512 bytes stalled and 1024-byte pieces
   passed. A piece that crosses a page boundary counts twice, and TCP
   can join sends issued back to back into one packet. The pieces at
   risk are those smaller than about a fifteenth of the MSS (roughly
   97 bytes at MTU 1500 and 600 at MTU 9000), and somewhat larger
   when pieces cross page boundaries. Only the sender's own
   connection is affected. The series' commit messages and cover
   letter say a sixteenth; the tests are later.
   (``test-tx-verification.md``, findings V1 and V2;
   ``rocm102-verification.md``, finding F1.)

Unloading ionic with a live transmit binding crashes the kernel
   On a kernel without net-next commit ``ede1618064a9`` ("net:
   devmem: detach TX bindings on NETDEV_UNREGISTER"), ``rmmod ionic``
   while a dma-buf is bound for transmit leaves the binding pointing
   at a freed netdev. The unload succeeds; the use-after-free and an
   oops follow when the process that made the binding closes its
   netlink socket. It is a bug of the networking core, not of ionic,
   and was hit on both ``v7.2.15`` and ``v7.4-rc3``. Close the
   binding before unloading the module. A backport of the fix to
   ``v7.2.15`` is in
   :project-file:`tests/devmem/kernel-patches/net-core-v7.2.15`; it
   builds, and **has not been booted**.

The receive series needs the XDP frag offset fix beneath it
   The series as first carried here kept, in the multi-buffer XDP
   path, the frag offset bug that the base series fixes: an
   ``XDP_TX`` or ``XDP_REDIRECT`` of a frame with frags could put
   another frame's bytes, or stale kernel memory, on the wire. The
   series was regenerated on top of the fix, and its fourth patch no
   longer applies without it. With the regenerated series every one
   of 10,000 echoed 8000-byte datagrams at MTU 9000 was intact, with
   ``XDP_TX`` and with ``XDP_REDIRECT``; with the series as it was,
   about half were wrong in their last 426 bytes
   (``rx-xdp-frag-fix.md``). The fix is not upstream. Only the XDP
   echo, header split with ordinary pages and two devmem receive
   cases were re-run on the regenerated series; see its
   :project-file:`README <ansible/files/patches/devmem-rx/README.md>`.

Binding HIP memory on an unpatched ``v7.2.x`` disturbs the GPU process
   On the stock ``v7.2.6`` image, binding a ``hipMalloc`` buffer for
   transmit moves it out of VRAM, which evicts and restores the
   process in the GPU driver. What the NIC sent verified in five of
   five runs, but in four of them HIP's own next read of the buffer
   was wrong (it returned success and copied nothing, or differed)
   or hung. Forcing the eviction with no NIC involved failed the same
   way, so the fault is in the GPU stack or its emulation, not in
   devmem. With the peer-to-peer attach the buffer does not move, and
   HIP read it back correctly in four of four runs.
   (``rocm102-gpu.md``, finding F1.)

Receive works only against this emulator
   The ``header-split`` mode's feature bit is the next free one in
   the driver's header, not one a firmware defines. A device that
   uses that bit for something else would be taken for one that
   splits headers. The receive series must not go upstream as it is.

A firmware that really stops was not tested
   The driver's firmware-reset handling was driven through test-only
   knobs, with the emulated firmware alive throughout.

The cover letter of the mainline transmit series is out of date
   It says the series was only built. It has since been run, as
   above; the letter was not rewritten.

Sending from GPU memory
-----------------------

A dma-buf exported by amdgpu from the emulated GPU can be the source.
Whether the NIC then reads the GPU's VRAM or a copy in system memory
depends on one commit of the networking core, ``69ab87acc614`` ("net:
devmem: allow peer-to-peer dma-buf attach for bindings"), first in
``v7.4-rc1``:

- Without it, as on an unpatched ``v7.2.x``, the core attaches to the
  dma-buf statically, amdgpu moves the buffer to system memory when
  it is pinned, and the NIC reads that. The transfer verifies; no
  VRAM is read (``test-gpu-dmabuf.md`` and ``rocm102-gpu.md``, both
  on a guest image's stock ``v7.2.6``).
- With it the core attaches dynamically and allows peer-to-peer. The
  buffer stays in VRAM, the driver puts addresses inside the GPU's
  BAR in its descriptors, and the emulator reads them through QEMU.
  This was shown twice:

  - On ``v7.4-rc3``: 5.6 GB in 30 s, every byte verified, with the
    emulator's ``tx_dma_error`` at 0 (``test-gpu-vram.md``).
  - On ``v7.2.6`` with a hand backport of the commit, in one guest of
    the ROCm 10.2 stack: GEM and ``hipMalloc`` buffers stayed in
    VRAM, every frag address was inside the GPU's BAR, up to 4.4 GB
    per run verified, and the sending NIC server's own mapping of
    the VRAM held the pattern (``rocm102-gpu.md``, section 5.2).
    That kernel was built from a reduced configuration, the rest of
    the test matrix was not run on it, the independent pass over
    that stack did not re-run these cases, and **the backport is not
    in this repository**.

The second case needs all of:

- a guest kernel with that commit (``v7.4-rc1`` or later, or a
  backport) and ``CONFIG_PCI_P2PDMA``;
- the fork of QEMU that passes a vfio-user region's file descriptor
  when it maps guest memory for a device (see "GPU VRAM buffers" in
  :doc:`performance`), so that the NIC's server can map the GPU's
  BAR;
- an **AMD Zen host**, or the two devices behind one root port. They
  sit behind different root ports in the test stack, and the kernel
  allows peer-to-peer across the host bridge only for bridges on its
  list, which QEMU's is not, or for AMD CPUs of family 17h and later.
  The guest sees the host's CPU, so on an Intel host the same stack
  would fall back to system memory. Every run was on one AMD host
  with that layout: that one root port would do, and what an Intel
  host would do, are from the kernel's source and were not run;
- an amdgpu that probes the emulated GPU on that kernel. The in-tree
  driver needed two patches
  (:project-file:`tests/devmem/kernel-patches/amdgpu-intree-v7.4`);
  ``amdgpu-dkms`` 31.70 needed a port to ``v7.4``
  (:project-file:`tests/devmem/kernel-patches/amdgpu-dkms-31.70-v7.4`).

A control run with the buffer in system memory, the same program with
one flag changed, put the addresses in system RAM instead, which
shows the probe tells the two apart.

Trying it
---------

Transmit, on the kernel the project's guests use:

#. Build and run the server as usual (:doc:`building`,
   :doc:`usage`). The default NIC profile offers the offloads the
   bind needs.
#. Build the guest's ionic with the ``devmem-tx`` series, after the
   others. Its
   :project-file:`README <ansible/files/patches/devmem-tx/README.md>`
   has the ``fetch-ionic-sources.sh`` command and the
   ``ernic_guest_setup`` variable.
#. Give the guest a kernel with ``CONFIG_NET_DEVMEM`` and something
   to make a dma-buf from, such as ``CONFIG_UDMABUF``.
#. Build ``devmem_tx`` from
   :project-file:`tests/devmem/src <tests/devmem/src>` as
   :project-file:`tests/devmem/README.md` says, and copy it to both
   guests. Start the receiver on one and the sender on the other
   with the same plan:

   .. code-block:: bash

      devmem_tx -r -p 5401 -S 4m -l 65536 -n 64 -a 4096
      sudo devmem_tx -f IFACE -s RECEIVER -p 5401 \
        -S 4m -l 65536 -n 64 -a 4096

   The receiver's ``RESULT rx PASS`` line, with no mismatches, is the
   result. The sender's line says what the kernel accepted, not what
   arrived.
   These two commands, with a ``devmem_tx`` built from this tree,
   passed between two ``v7.2.15`` guests
   (:project-file:`tests/devmem/README.md`).

Receive, and transmit on ``v7.4``, need a ``v7.4-rc3`` guest kernel
and the series in ``devmem-tx-mainline`` and ``devmem-rx``, which no
role or script applies; their READMEs say what they are based on.
The receive series also needs the XDP frag offset fix applied first,
:project-file:`tests/devmem/kernel-patches/ionic-mainline-fixes`.
Receive also needs the server started with ``--nic-profile
header-split``.

The tests ran on stacks of two guests that qemu-minimal's
``qemu-tool quick-start`` brings up: QEMU, one rocm-ernic server and
one emulated GPU for each guest. The harness that drove them is in
:project-file:`tests/devmem/scripts`, written for the host it ran on;
the notes ``testbed.md`` and ``test-rx.md`` are its runbooks.

Where the detail is
-------------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Where
     - What
   * - :project-file:`ansible/files/patches/devmem-tx`
     - The transmit series for the guests' ``v7.2.6`` ionic sources
   * - :project-file:`ansible/files/patches/devmem-tx-mainline`
     - The same series against ``v7.4-rc3``, as it would be posted
   * - :project-file:`ansible/files/patches/devmem-rx`
     - The receive series, against ``v7.4-rc3``
   * - :doc:`ionic`
     - The device as emulated: "Transmit offloads" and "Receive
       header split"
   * - :project-file:`tests/devmem`
     - The test programs, probes, harness and kernel patches. Test
       aids: nothing there is built or run by CMake
   * - :project-file:`docs/devmem/notes`
     - The working notes: studies, design, and the report of every
       run
