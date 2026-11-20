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
       ``v7.2.6`` with that commit backported
       (:project-file:`tests/devmem/kernel-patches/net-core-devmem-p2p`).
       On an unpatched ``v7.2.x`` the transfer works but the data is
       read from system memory.
   * - Receive into a dma-buf
     - Works against the emulator's ``header-split`` mode only, on
       ``v7.4-rc3`` and on the ``v7.2.6`` line the project's guests use,
       with ``devmem-rx-7.2.6``. The device interface is invented.
   * - GPU VRAM to GPU VRAM
     - Works on ``v7.2.6`` with that one backported commit: a stream
       that starts in one guest's GPU memory ends in the other guest's
       GPU memory, with neither kernel touching the payload. See
       `GPU memory, end to end`_.
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

The same eight patches, backported to the ``v7.2.6`` that
``IONIC_KERNEL_REF`` pins
(:project-file:`ansible/files/patches/devmem-rx-7.2.6`, whose
``README.md`` says what it is based on), needed no code change and ran
the same ground on that kernel line (``rocm102-rx.md``): header split
with ordinary pages, a 1 GiB receive into a dma-buf with nothing
through the linear area, devmem transmit into devmem receive, bind and
unbind raced against ring, channel, MTU and reset operations, about
320 binds with no page pool left behind, and a lockdep boot that
reported nothing. An independent pass re-ran the udmabuf receive case
and the "feature off" case with its own tools and confirmed both, and
checked the eight patches line by line against their mainline form
(``rocm102-verification-2.md``, sections 2.1 and 3.2).

Limits
------

A send in small pieces needs the eighth transmit patch, and the device's TSO
   ionic sends at most 16 buffers per packet: the head and 15
   scatter-gather elements. A devmem ``sendmsg()`` makes one frag per
   iovec entry per page it touches, and the driver cannot copy
   unreadable frags together. Before the eighth patch of
   :project-file:`ansible/files/patches/devmem-tx` it dropped such a
   packet and counted it in ``tx_unreadable_drop``, and because TCP
   retransmits the same packet the connection never recovered: the
   sender blocked, or its ``sendmsg()`` ended with ``ETIMEDOUT``. The
   pieces at risk were those smaller than about a fifteenth of the MSS
   (roughly 97 bytes at MTU 1500 and 600 at MTU 9000), and somewhat
   larger when pieces cross page boundaries, since a piece that
   crosses a page boundary counts twice and TCP can join sends issued
   back to back into one packet.

   **That stall is fixed.** The driver now sends an over-limit
   unreadable skb with the TSO opcode and an mss small enough that
   every descriptor fits, which is the same bytes in shorter segments,
   and counts the skb in ``tx_unreadable_reseg``. Every send that
   stalled without the patch completed with it, with the receiver's
   sha256 equal to the sender's, on the ``v7.2.6`` guests
   (``rocm102-frag-limit.md``). An independent run of a matched pair —
   the same settings on both sides, which the first table did not have
   — reproduced it: 15 pieces of 64 bytes gave ``ETIMEDOUT`` after
   13.1 s with 0 bytes at the receiver before, and 288000 bytes with
   equal hashes and 300 of 300 completions after
   (``rocm102-verification-2.md``, section 3.4).

   **What remains:** the fix needs the device's TSO for that IP
   version, because it is a TSO descriptor chain. With TSO off
   (``ethtool -K tso off``, or ``tx-tcp6-segmentation off`` for IPv6),
   on IPv4 with DF cleared, on anything but plain TCP, or where the
   layout needs more than half the transmit ring in descriptors, the
   over-limit packet is still dropped, still counted in
   ``tx_unreadable_drop``, and the connection still ends in
   ``ETIMEDOUT``. Only the sender's own connection is affected.
   Whether real firmware accepts an mss as small as 15 is unknown.

Ordinary TCP on a receiver stops while a devmem binding holds its only queue
   With one combined channel and ``tcp-data-split on``, every flow
   lands on the bound queue, whose payload buffers are net_iovs, so a
   socket that did not ask for ``MSG_SOCK_DEVMEM`` sees nothing: 4 MiB
   sent to an ordinary listener on such a receiver, with no devmem
   sender running at all, delivered **0 bytes**, and
   ``rx_unreadable_drop`` did not move. This bounds "devmem and RDMA
   on the same NIC": a ROCm RDMA dma-buf test run during a devmem
   transfer died in its out-of-band TCP exchange with a connection
   reset, while the same command alone, in the same configuration,
   passed; the devmem transfer itself was unaffected. ssh, a health
   check or a control connection on that interface would stop the same
   way. It follows from the design, and the answer is more than one
   channel with queue steering, not a change to the binding.
   (``rocm102-verification-2.md``, finding N1.)

A receive dma-buf too small for the ring starves the page pool
   A 1 MiB receive dma-buf is 256 pages, against a 1024-entry receive
   ring: the provider's allocation failed 9995 times in 100 s, the NIC
   had nothing to post, TCP fell into retransmission backoff, and the
   receiver ended on its idle timeout after 26406984 of 33554432
   bytes. The failure is clean — 0 mismatches, every token returned,
   no leak and no corruption; the stream merely stops, and the other
   of the two runs at that size did finish, slowly. 4 MiB, which
   matches the ring, was the smallest size that worked in both
   repetitions, and
   even there the pool momentarily ran dry. The same mechanism appears
   under genuine pressure (450 allocation failures on a guest sending
   and receiving at once) and that transfer still completed and
   verified. (``rocm102-gpu-to-gpu.md``, finding F2, confirmed in
   ``rocm102-verification-2.md``, section 4.)

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
  - On ``v7.2.6`` with the commit backported
    (:project-file:`tests/devmem/kernel-patches/net-core-devmem-p2p`),
    in both guests of the ROCm 10.2 stack: GEM and ``hipMalloc``
    buffers stayed in VRAM, every frag address was inside the GPU's
    BAR, buffers up to 1020 MiB (GEM) and 960 MiB (``hipMalloc``)
    worked, 4130 bind and unbind cycles left nothing behind, and a
    GPU kernel rewriting the buffer between sends was seen by the
    peer, with no stale copy anywhere. Two 330 s runs from GPU
    buffers verified 67.3 GiB and 66.2 GiB with no mismatching byte
    (``rocm102-gpu.md`` section 5.2, ``rocm102-p2p-kernel.md``). That
    kernel is stock ``v7.2.6`` plus that one commit and nothing else,
    built from the guest image's own configuration; the upstream
    quick-start tests and the 35-case transmit matrix give it the
    stock kernel's verdict in every case. An independent pass
    confirmed the kernel's identity by diffing the whole tree it was
    built from against pristine ``v7.2.6`` — one file differs — and
    the patch against upstream's commit
    (``rocm102-verification-2.md``, section 1.1).

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

GPU memory, end to end
----------------------

With that one commit on both guests, and the emulator's
``header-split`` mode on the receiver, a stream that starts in one
guest's GPU VRAM arrives in the other guest's GPU VRAM, and neither
kernel reads the payload. The one commit covers both directions: no
receive-specific core change was needed.

A second engineer re-ran the headline case with their own random seed,
their own expected-byte generator, their own placement probe and their
own host-side hash, and confirmed four links in a single run
(``rocm102-verification-2.md`` section 3.1, ``rocm102-gpu-to-gpu.md``):

- **the sender's frag addresses are in the GPU's BAR**: 17533 of them,
  with no address outside it;
- **the receiver's posted buffers are in the GPU's BAR**: 38059
  net_iov addresses, again with no other bucket;
- **both buffers are in VRAM while bound**: ``VRAM VISIBLE pin
  count 1`` on both guests, with a dynamic, peer-to-peer dma-buf
  attach on each;
- **the bytes are in the receiving GPU's memory**: three fragments,
  hashed on the host from the *receiving* NIC server's own mapping of
  that GPU's BAR, at the address the driver had given the device, all
  equal to what the host computed for the sent stream.

The receiver checked every byte in the dma-buf's own memory rather
than from the socket, with nothing through the linear area
(``linear_cmsgs=0``), every token returned, and the emulator's error
counters at 0 on both servers. Of the receive payload descriptors, a
fixed 64 per run are in system RAM — the same 64 in the independent
run, from the fill before the binding — and the rest are in the BAR.

Two further checks make the result credible:

- **a negative control**: one fragment's expected bytes hashed against
  a different fragment's address differ, as they must;
- **a poison pre-fill**: the receive buffer was filled with ``0xa5``
  before the transfer, and its last 64 KiB still hashes as 64 KiB of
  ``0xa5`` afterwards, so the NIC wrote the plan's bytes where the
  plan said and nothing else. The result cannot be a wholesale fill.

What the host-side hash does **not** show is an independent arrival:
the server's mapping and the receiver's are the same backing store. It
proves placement — the address the driver handed the device maps to
exactly where the correct bytes are — and not a second observation of
the data.

Around that run: killing the receiver mid-transfer with a live VRAM
binding left no dma-buf, no pinned buffer object and no page pool
behind, and a fresh binding worked straight after; and both guests
sending from their own VRAM and receiving into another buffer at the
same time passed in both directions
(``rocm102-verification-2.md``, findings N2 and N3).

The trap: without the backport it passes anyway
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This is how a reader would fool themselves. On a stock ``v7.2.6``, the
control run bound the same VRAM-created buffer, the transfer completed
and **every byte verified** — while the buffer was not in VRAM at all.
Without ``69ab87acc614`` the core's plain ``dma_buf_attach()`` leaves
``attach->peer2peer`` false, amdgpu strips VRAM from the pin domains
of a non-peer-to-peer attachment, the buffer moves to system memory,
and every net_iov (19103) and every payload descriptor (23330) is in
system RAM with none in the BAR. A test that checks only the payload
therefore passes while the data goes nowhere near the GPU. Check the
placement, not the bytes (``rocm102-gpu-to-gpu.md``, section 7).

Performance
-----------

.. warning::

   **These are emulation numbers, and they measure the emulator, not
   ionic hardware.** Every one was taken on a shared, loaded
   development host — load average between about 7 and 16 during these
   runs, two of the CPUs being the stack's own servers, which spin at
   about 99 % of a CPU from start-up — and identical runs varied by
   more than 2x. The rule the independent pass applied, and this page
   follows: **never read a difference from a single sample.** Nothing
   below is a hardware number or a regression.

Devmem transmit and receive, 64 MiB per run, three samples each, as
sender-side MiB/s (``rocm102-gpu-to-gpu.md``, section 8):

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Path
     - Transmit MiB/s, 3 samples
   * - Copy TCP
     - 191.5, 226.4, 206.2
   * - ``MSG_ZEROCOPY``
     - 240.0, 215.9, 177.1
   * - devmem transmit (udmabuf) to an ordinary receive
     - 184.1, 218.3, 195.7
   * - devmem transmit to devmem receive, udmabuf both ends
     - 145.4, 94.0, 103.4
   * - VRAM to VRAM, GPU buffers both ends
     - 4.3, 4.4, 4.4

**The VRAM row is not a datapath measurement.** Reading an emulated
GPU's VRAM back through a CPU mapping costs about 3 MiB/s on this
stack, and both ends do it: the sender fills its buffer through one
mapping and the verifier reads through the other. The row measures the
test's readback, and quoting it as a devmem rate would mislead.

Sustained runs, which say more than a rate sample because every byte
and every completion is accounted for:

- 360 s of devmem transmit from a udmabuf into an ordinary receiver:
  **74.1 GiB** verified, 0 mismatches, 79571 of 79571 zerocopy
  completions, none reporting a copy (``rocm102-devmem.md``).
- 330 s from a 64 MiB GEM VRAM buffer: **67.3 GiB** verified, 0
  mismatches, 72242 of 72242 completions, every one of 18793507 frag
  addresses in the GPU's BAR; and 330 s from a 16 MiB ``hipMalloc``
  buffer: **66.2 GiB** verified, 71070 of 71070 completions, all
  18487738 frag addresses in the BAR (``rocm102-p2p-kernel.md``).

For scale, the same two guests on the same host run the upstream
quick-start tests at (``rocm102-baseline.md``):

- ``ib_send_bw``: 1134 to 1950 MiB/s across eight runs — the band to
  treat as normal, not any single value in it;
- ROCm RDMA, host allocation 1158 MiB/s and dma-buf 1305 MiB/s, each
  one sample, each validated;
- hipFile fio, 212.4 and 214.4 MB/s on the two guests;
- the P4 fp8 reduction, 170.5 MB/s.

**Never measured:** ionic hardware; any configuration with an IOMMU,
where a dma-buf map returns IOVAs rather than the guest physical
addresses used throughout here; and any host but this one.

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

Receive on the pinned ``v7.2.6`` line needs
:project-file:`ansible/files/patches/devmem-rx-7.2.6` after ``srq``,
``p4`` and ``devmem-tx`` ``0001-0007``, a guest that rebuilds
``ionic_rdma.ko`` and ``ernic_p4.ko`` against the same ionic, and the
server started with ``--nic-profile header-split``. A guest that wants
the eighth transmit patch as well applies
``devmem-tx/on-devmem-rx/0009`` after the receive series instead; the
two directories' READMEs say so.

Transmit on ``v7.4``, and the reference form of receive, need a
``v7.4-rc3`` guest kernel and the series in ``devmem-tx-mainline`` and
``devmem-rx``, which no role or script applies; their READMEs say what
they are based on. The receive series also needs the XDP frag offset
fix applied first,
:project-file:`tests/devmem/kernel-patches/ionic-mainline-fixes`.

Sending from, or receiving into, GPU memory rather than a copy in
system memory needs the guest kernel to carry ``69ab87acc614``:
:project-file:`tests/devmem/kernel-patches/net-core-devmem-p2p` is the
backport, which this repository builds into no kernel — its
``README.md`` says what an image bake would have to do.

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
   * - :project-file:`ansible/files/patches/devmem-rx-7.2.6`
     - The same series against the ``v7.2.6`` the guests use
   * - :project-file:`tests/devmem/kernel-patches/net-core-devmem-p2p`
     - The one core commit that keeps a bound dma-buf in GPU VRAM
   * - :doc:`ionic`
     - The device as emulated: "Transmit offloads" and "Receive
       header split"
   * - :project-file:`tests/devmem`
     - The test programs, probes, harness and kernel patches. Test
       aids: nothing there is built or run by CMake
   * - :project-file:`docs/devmem/notes`
     - The working notes: studies, design, and the report of every
       run
