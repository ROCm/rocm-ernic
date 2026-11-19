The ionic Device
================

rocm-ernic presents a single PCIe device to the guest:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - VID:DID / SSVID:SDID
     - Guest driver
   * - ``1dd8:1002`` / ``1dd8:5400``
     - Upstream Linux ``ionic.ko`` + ``ionic_rdma.ko``,
       with the UC queue-pair patch in
       ``ansible/files/patches/`` applied, and the
       upstream ``providers/ionic`` in
       rdma-core

The server emulates the register and queue protocol of the
AMD Pensando ionic NIC, so the guest runs a driver that is
already in mainline Linux rather than one that only exists
here. The driver source is near-stock --- one small RDMA
patch --- but the guest itself is **not** a stock cloud
image. It needs a mainline kernel
matching ``IONIC_KERNEL_REF`` and the headers to build
against, which no Ubuntu release ships. CI builds such an
image, defined in ``ci/images/guest/`` and published to GHCR,
and ``scripts/fetch-guest-image.sh`` fetches it. See the
warning below.

The Pensando vendor ID ``0x1dd8`` is kept, and the emulated
PF now uses the upstream device ID ``0x1002``. The
subsystem ID ``0x5400`` distinguishes this function from
physical PFs with the same vendor/device pair. Once its
entry is published in ``pci.ids``, ``lspci`` will name it
``ROCm Emulated RDMA NIC`` after ``update-pciids``. The
underlying vendor/device pair remains intact for the upstream
driver.

Starting the Server
-------------------

.. code-block:: bash

   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback \
     --tap ernic0 \
     --log-level info

There is no flag to select the device. ``--tap`` (short
``-T``) attaches the emulated Ethernet LIF to a host TAP
interface.

All the RDMA backends (``loopback``, ``tcp``, ``none``) work
the same way: the backend choice governs the
RDMA data path, while ``--tap`` governs the Ethernet data
path.

BAR Layout
^^^^^^^^^^

The BAR layout matches what ``ionic_dev_setup()`` in the
upstream driver expects:

.. list-table::
   :header-rows: 1
   :widths: 10 20 70

   * - BAR
     - Size
     - Purpose
   * - BAR 0
     - 64 KB
     - 32 KB device register window (device info, device
       command, interrupt control) plus the MSI-X table and
       PBA above it
   * - BAR 2
     - 4 MB
     - Doorbell pages; the offset within a page encodes the
       queue type, the page index the process ID
   * - BAR 4
     - 8 MB
     - Controller Memory Buffer (CMB), only with ``--cmb``:
       without it there is no BAR 4, the driver finds no CMB,
       and every queue is in guest memory.  QEMU maps it into
       the guest, so the driver writes WQEs directly into it,
       then rings the queue's doorbell.  ``DISCOVER_CMB``
       (opcode 80) returns one region here, ``DEVMEM`` (type 0),
       128 × 64 KiB pages.  ``Q_IDENTIFY`` doesn't set
       ``IONIC_QIDENT_F_EXPDB``: the device offers no express
       doorbell, since it never sees the guest's stores into
       the CMB.

The MSI-X table has room for 128 vectors; the device
advertises the profile's ``queues.intr_vectors`` of them (32
under ``compat``).

Interrupts
^^^^^^^^^^

The device interrupts with MSI-X only. It has **no INTx**:
its Interrupt Pin reads 0, which PCI Express allows of an
endpoint, and ``ionic.ko`` asks for MSI-X alone. A driver,
or a vfio-user client of your own, has to enable MSI-X
before the device can interrupt it.

While MSI-X is off, QEMU gives the device no eventfds to
signal, so an interrupt the device raises then, on a vector
the driver has unmasked in the device's interrupt control,
is lost rather than held for later, as the specification
permits. libvfio-user logs ``no fd for interrupt`` for each.

NIC Profiles
------------

A NIC profile is the shape of the emulated NIC: what it
reports about itself (ASIC type and revision, firmware
version), its link (speed, transceiver, largest MTU) and the
queues and interrupt vectors it offers.
Without ``--nic-profile`` the server runs the built-in
``compat`` profile, which is the device described above,
byte for byte.

.. code-block:: bash

   # layer a file over compat; later layers win, key by key
   ./build/rocm-ernic --tap ernic0 --nic-profile ./fast.ini

   # show the merged profile and exit
   ./build/rocm-ernic --nic-profile ./fast.ini --print-profile

``--nic-profile`` can be given more than once. Each one
names either a built-in profile (a bare name: ``compat``,
``small-vm``, ``generic-100g``, ``generic-400g``,
``header-split``) or a file (a name containing a ``/`` or
ending in ``.ini``). The layers
apply in order on top of ``compat``. A profile is an ini
file:

.. code-block:: ini

   # fast.ini
   [identity]
   fw_version = 1.8.0-E-32

   [link]
   speed_mbps = 400000
   xcvr = fiber

Validation is strict. An unknown section or key, a duplicate
key, a value of the wrong type or a value out of range stops
the server with the file, line and key named, so a typo
can't quietly leave a default in place. A layer that fails
changes nothing.

``--print-profile`` writes the merged profile as an ini file
that loads back to the same values. Its header names the
layers and a 64-bit hash of the values; the startup log and
the ``nic_profile`` line in the stats file carry the same
chain and hash, so two runs can be checked for the same
device shape.

.. list-table::
   :header-rows: 1
   :widths: 25 35 15 25

   * - Key
     - Values
     - ``compat``
     - Guest effect
   * - ``identity.asic_type``
     - ``none``, ``capri``, ``elba``, ``giglio``, ``salina``
     - ``salina``
     - ``dev_info`` ASIC type. For ``none`` and ``elba``, the
       driver works around missed doorbells
       (``ionic_doorbell_wa()``): it polls its admin and
       Ethernet queues each second and re-rings idle ones'
       doorbells, which the emulated device doesn't need
   * - ``identity.asic_rev``
     - 0 to 255
     - 0
     - ``dev_info`` ASIC revision
   * - ``identity.fw_version``
     - 1 to 31 printable ASCII characters; no ``\``, no
       leading or trailing space
     - ``rocm-ernic-1.0``
     - ``ethtool -i`` ``firmware-version``
   * - ``link.speed_mbps``
     - 1000, 10000, 25000, 40000, 50000, 100000, 200000,
       400000
     - 100000
     - port and LIF link speed: ``ethtool`` ``Speed``, the
       RDMA port ``rate``
   * - ``link.xcvr``
     - ``copper``, ``fiber``, ``none``
     - ``copper``
     - transceiver: ``ethtool`` ``Port`` is ``Direct Attach
       Copper``, ``FIBRE`` or ``None``
   * - ``link.mtu_max``
     - 1500 to 9582
     - 9198
     - the largest MTU the guest may set. The LIF reports a
       maximum frame of ``mtu_max`` + 18, and the port's
       configured MTU is the smaller of ``mtu_max`` and 9000
   * - ``stats.interval_ms``
     - 10 to 60000
     - 1000
     - how often the LIF counters are rewritten in guest
       memory, which bounds how stale ``ip -s link`` can be.
       ``--stats-interval-ms`` overrides every layer
   * - ``queues.eth``
     - 1 to 64
     - 4
     - Ethernet queue pairs the LIF offers: the most
       ``ethtool -l`` can set ``combined`` to, and the
       ``rx_N_*``/``tx_N_*`` rows in ``ethtool -S``.
       ``--eth-queues`` overrides every layer
   * - ``queues.rdma_eqs``
     - 4 to 32
     - 32
     - RDMA event queues; the RDMA driver needs at least 4
   * - ``queues.intr_vectors``
     - ``auto``, or 2 to 128
     - 32
     - MSI-X vectors the device advertises. ``auto`` is
       1 + ``queues.eth`` + ``queues.rdma_eqs``, enough for
       every queue and EQ to have its own
   * - ``rx.header_split``
     - ``off``, ``on``
     - ``off``
     - ``on`` offers the prototype receive header split to a
       driver that asks for it, and reports receive checksums
       to every driver, so ``ethtool -k`` shows
       ``rx-checksumming`` on; see `Receive header split`_

The built-in profiles:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Name
     - Shape
   * - ``compat``
     - the device described above: 4 queues, 32 EQs, 32
       vectors, 100 Gb/s copper
   * - ``small-vm``
     - ``compat`` cut down for a guest with a few vCPUs: 2
       queues, 4 EQs, ``auto`` (7) vectors
   * - ``generic-100g``
     - 16 queues, 32 EQs, ``auto`` (49) vectors, 100 Gb/s
       fiber
   * - ``generic-400g``
     - 64 queues, 32 EQs, ``auto`` (97) vectors, 400 Gb/s
       fiber
   * - ``header-split``
     - ``rx.header_split = on`` and nothing else, so it
       layers on any of the others

The guest driver (``ionic_lif_size()``) wants one vector for
the admin and notify queues, one per queue pair and one per
EQ, each count capped at the online vCPUs. If that is more
than the device advertises it halves its notify queues, then
its EQs, then its queue pairs until it fits. A profile whose
``queues.intr_vectors`` is below 1 + ``queues.eth`` +
``queues.rdma_eqs`` therefore works, but a guest with enough
vCPUs gets fewer EQs or queues than the profile names; the
server says so at startup. ``compat`` is such a profile (1 +
4 + 32 = 37 > 32), kept that way so its guest sees exactly
what it saw before profiles existed.

Ethernet and TCP/IP
-------------------

The emulated LIF is a working Ethernet NIC. A
Linux TAP interface is used as the host-side backend, which
is the cheapest way to give the guest a real, routable
Ethernet segment: the host end is an ordinary netdev, so ARP,
ICMP, DHCP, and TCP all work against the host network stack
with no protocol emulation inside the server.

Create a persistent TAP owned by the user that will run the
server, then address it:

.. code-block:: bash

   sudo ip tuntap add dev ernic0 mode tap user "$USER"
   sudo ip addr add 192.168.77.1/24 dev ernic0
   sudo ip link set ernic0 up

Start the server with ``--tap ernic0``, boot the guest, and
configure the guest end of the segment:

.. code-block:: bash

   # in the guest
   sudo ip addr add 192.168.77.2/24 dev enp0s4
   sudo ip link set enp0s4 up
   ping 192.168.77.1

Two Guests on One Bridge
^^^^^^^^^^^^^^^^^^^^^^^^

A single TAP with an address on it is enough for host-to-guest
traffic, but every two-VM test in ``ansible/playbooks/`` needs
guest-to-guest IP on ``192.168.200.x``: ``sanity-tests.yml``
pings it, ``tcp-performance-tests.yml`` runs iperf3 over it,
and ``performance-tests.yml`` uses it for the out-of-band
exchange in perftest. That traffic leaves through the TAP
rather than through the rocm-ernic TCP mesh, so each instance
needs its own TAP and all of them need to share one host
bridge:

.. code-block:: bash

   sudo ip link add ernicbr0 type bridge
   sudo ip link set ernicbr0 up
   for n in 1 2; do
     sudo ip tuntap add dev "ernic-tap${n}" mode tap user "$USER"
     sudo ip link set "ernic-tap${n}" master ernicbr0 up
   done

The ``ernic_host_setup`` role does this from
``tasks/tap.yml`` using ``ernic_tap_prefix``,
``ernic_tap_bridge`` and ``ernic_tap_owner``; the launcher
then passes ``--tap ${ERNIC_TAP_PREFIX}${id}`` to each
instance. A missing TAP is a warning rather than an error
there, so a loopback-only host still starts --- but the guest
comes up with no Ethernet, and every guest-to-guest test then
fails without saying why.

The bridge carries no IP of its own by default; the guests
address each other directly across it. Set
``ernic_tap_bridge_ip`` if the host needs to join the segment
for debugging.

The transmit path gathers each frame out of guest memory, the
descriptor's own buffer and then the elements of its
scatter-gather list, applies the offload the descriptor asks
for (see `Transmit offloads`_), and writes the frame to the TAP
file descriptor. The receive path drains the
TAP from the server's main loop and DMAs each frame into a
descriptor the guest has posted on one of its receive queues:
into the descriptor's buffer, and, when that is too short, on
into the buffers of its scatter-gather list in order, filling
each before starting the next. The completion says how many
of the list's buffers the frame used. A frame longer than all
of them together is dropped, and none of it is written.
Both directions run on the thread that owns the vfio-user
context, because that is the only thread permitted to DMA
into guest memory; no extra thread is created.

Received frames are spread over the receive queues the guest
has enabled (``Q_CONTROL``; ``ethtool -L`` changes the set)
by a fixed hash of the flow, so one flow always lands on one
queue and each queue's interrupt fires on its own vector. The
hash is 32-bit FNV-1a over the IPv4 or IPv6 source and
destination address, the protocol, and for TCP, UDP and SCTP
the two ports, after up to two VLAN tags; fragments leave the
ports out so every fragment of a datagram stays together.
Other frames hash on the two MAC addresses. The queue is the
hash modulo the number of enabled queues, in index order. The
guest's RSS key and indirection table are accepted and
ignored. A frame from the TAP whose queue has no buffer
posted is dropped and counted in ``rx_queue_empty``; it does
not fall back to another queue. A frame an in-process
endpoint (the S3 store's control plane, or the P4 engine)
queues for the guest is held until the queue it steers to
has a buffer, and the queued frames behind it wait too.

Transmit offloads
^^^^^^^^^^^^^^^^^

The driver asks for offloads with ``LIF_SETATTR`` and enables
those the completion repeats. The emulated LIF grants transmit
scatter-gather, the transmit checksum, and TCP segmentation
over IPv4 and IPv6 with ECN, when the driver asks for them, and
nothing else (but the receive checksum, with
``rx.header_split = on``), so the guest's ``ethtool -k`` shows
``scatter-gather``, ``tx-checksum-ip-generic``,
``tx-tcp-segmentation``, ``tx-tcp6-segmentation`` and
``tx-tcp-ecn-segmentation`` on. ``ethtool -K`` turns each off
and on again.

Scatter-gather
   A frame is the descriptor's buffer, whose length is the
   descriptor's ``len``, followed by the elements of its
   scatter-gather list. ``Q_IDENTIFY`` offers version 1 of the
   transmit queue, whose lists hold 15 elements in 256 bytes,
   to a driver that knows it, as Linux does; a driver that
   asks for version 0 gets 8 elements in 128 bytes. No buffer
   may be empty, and a frame is at most 9600 bytes.

Checksum
   For a ``CSUM_PARTIAL`` descriptor the device sums the frame
   from ``csum_start`` to its end and stores the Internet
   checksum ``csum_offset`` bytes further on, a computed zero
   as ``0xffff``. This is what the stack's software fallback
   does, so it serves TCP and UDP over IPv4 and IPv6 alike,
   with or without a VLAN tag in the frame.

TCP segmentation
   A ``TSO`` packet is a chain of descriptors, one per
   segment, from the one flagged ``TSO_SOT`` to the one
   flagged ``TSO_EOT``. The first brings ``hdr_len`` bytes of
   headers and up to ``mss`` bytes of payload, the others
   payload alone. Each goes out behind a copy of the headers
   with its own IPv4 total length, identification (counting
   up from the packet's) and header checksum or IPv6 payload
   length, TCP sequence number and TCP checksum; FIN and PSH
   stay only on the last segment and CWR only on the first.
   The checksums are computed from nothing, whatever the
   driver left in the fields. The headers may carry one
   802.1Q or 802.1ad tag, IPv4 and TCP options, and IPv6
   hop-by-hop, destination-options and routing headers (a
   routing header only with no segments left); they are at
   most 256 bytes, and a chain carries at most 65535 bytes of
   payload. The chain gets one completion, naming its last
   descriptor, once every segment has been read; a chain
   whose end the guest has not posted yet waits for it, each
   doorbell until then reading the descriptors posted so far
   and none of their buffers.

A descriptor the device cannot make sense of is not sent cut
to fit or as something else: an empty buffer, more
scatter-gather elements than the queue's lists hold or any on
a queue initialized without ``IONIC_QINIT_F_SG`` or with an
``sg_ring_base`` of 0, a frame over 9600 bytes, a checksum
field outside the frame, a TSO chain with a malformed
descriptor or headers that are not TCP over IPv4 or IPv6, and
anything that needs an offload the LIF did not grant
(``CSUM_HW``, a VLAN tag to insert, tunnel headers to
rewrite). It is counted in ``tx_desc_data_error`` and still
completed, since the driver reads no status from a transmit
completion: the guest is not told, and the counter is the
only sign. A refused TSO chain counts once.

A TSO chain is checked whole before any of it is sent, and
one that fails the check sends nothing. The buffers of the
descriptors after the first are read only as each segment
goes out, and the descriptors are read again then. So a chain
with a buffer that cannot be read (``tx_dma_error``), or one
the guest rewrites meanwhile into something the check would
have refused, stops part-way: the segments before that one
have been sent, the rest are not, and the chain counts once.

.. note::

   The Linux driver can hand the device TSO sends it refuses.
   The guest's stack takes segmentation away from a send with
   IPv6 extension headers only for a device with
   ``NETIF_F_IPV6_CSUM``, and ionic has ``NETIF_F_HW_CSUM``.
   So a TCP flow over IPv6 with a routing header that has
   segments left (segment routing with the SRH in the packet),
   an extension header other than hop-by-hop,
   destination-options and routing, or more than 256 bytes of
   headers has every TSO send dropped, without an error, and
   advances only by TCP's retransmissions of single segments,
   which are not TSO. ``tx_desc_data_error`` climbing with
   such a flow is the sign; ``ethtool -K <dev>
   tx-tcp6-segmentation off`` in the guest has its stack
   segment instead.

.. note::

   Receive hash, VLAN insertion, stripping and filtering,
   and segmentation through tunnels are not offered, so the
   guest's stack does those itself, and the receive checksum
   too unless ``rx.header_split`` is ``on`` (see `Receive
   header split`_). Receive scatter-gather is not an offload:
   the driver posts an SG list whenever a frame may not fit
   in one buffer.

Receive header split
^^^^^^^^^^^^^^^^^^^^

.. warning::

   This is a prototype, off by default. No ionic firmware
   has receive header split, and the queue feature bit, the
   completion layout and the flags below were made up so
   that a driver under development has a device to run
   against. They are not an ionic device interface, may
   change, and say nothing about what real hardware will do.

With ``rx.header_split = on`` in the NIC profile
(``--nic-profile header-split``), the device can put a TCP
segment's headers in one guest buffer and its payload in
others, which is what a driver needs in order to receive
into memory it cannot read (devmem TCP): the stack reads the
headers and never the payload. The mode does three things,
and is fixed for the life of the server. With it off, the
device does none of them and is byte for byte the device
described above.

Receive checksum
   The LIF grants ``IONIC_ETH_HW_RX_CSUM``, and every receive
   completion, on every queue, reports the frame's checksum
   status whether or not the driver asked for the feature.
   ``csum_flags`` has ``CALC`` for any frame of 14 bytes or
   more, and ``csum`` is then the ones-complement sum, not
   inverted, of the frame from byte 14 to its end, VLAN tags
   and padding included, stored high byte first. An IPv4
   header behind at most two VLAN tags adds ``IP_OK`` or
   ``IP_BAD``. TCP or UDP directly behind an unfragmented
   IPv4 header with a good checksum, or behind an IPv6
   header with no extension header, adds ``TCP_OK``,
   ``TCP_BAD``, ``UDP_OK`` or ``UDP_BAD`` if the IP header's
   length puts all of it inside the frame; the checksum is
   taken over that length, so padding does not count. A UDP
   checksum of zero adds neither flag, and nor does a UDP
   length that disagrees with the IP header's. A good TCP
   segment over IPv4 has ``csum_flags`` ``0x91``, over IPv6
   ``0x81``; a good UDP datagram ``0x94`` and ``0x84``.
   ``VLAN``, ``vlan_tci``, ``rss_hash`` and the packet type
   stay zero.

Negotiation
   ``Q_IDENTIFY`` of the receive queue reports ``features``
   ``0x20020``: ``IONIC_Q_F_2X_CQ_DESC`` (bit 5) and
   ``IONIC_RXQ_F_HDS`` (bit 17, which ``ionic_if.h`` does not
   have). ``comp_sz`` stays 16. A driver asks for header
   split on a queue with exactly those two bits in
   ``Q_INIT``'s ``features`` (the le64 at byte 56), together
   with ``IONIC_QINIT_F_SG`` and an ``sg_ring_base`` other
   than 0. ``features`` of 0 gives a queue without header
   split. Any other bit is refused with ``IONIC_RC_ENOSUPP``,
   and one of the two bits without the other, or both without
   an SG ring, with ``IONIC_RC_EINVAL``; a refused ``Q_INIT``
   leaves the queue as it was. The choice is the queue's
   until its next ``Q_INIT``, and queues of one LIF may
   differ. With the mode off ``Q_INIT`` does not look at
   ``features``, as before.

Completion
   A header-split queue's completion ring has 32-byte
   entries, as many as the descriptor ring. The second 16
   bytes are the ``struct ionic_rxq_comp`` every receive
   queue has, so the colour is bit 7 of byte 31. The first
   16 are:

   .. code-block:: text

      [0:1]   le16 hds_hdr_len  frame bytes in the descriptor's own buffer
      [2]     hds_flags         0x01 SPLIT, 0x02 HBO
      [3:7]   reserved, zero
      [8:15]  zero (where a hardware timestamp would be)

   ``hds_hdr_len`` is set on every completion. The frame is
   always the first ``hds_hdr_len`` bytes of the descriptor's
   buffer followed by ``num_sg_elems`` SG elements, each full
   but the last, so a driver builds the packet from those two
   numbers and never parses it to find the split.

What is split
   A frame is split if it is TCP over IPv4 (any header
   length, not a fragment, a good header checksum) or over
   IPv6 with no extension header, behind at most two VLAN
   tags; the IP header's length ends exactly where the frame
   does; the TCP checksum is good; and the TCP header length
   is at least 20 bytes and inside the segment. The descriptor
   must be an ``SG`` descriptor whose own buffer holds the
   headers, Ethernet through the TCP options, and whose SG
   elements hold the payload. Then the headers alone go to the
   descriptor's buffer, the payload to the elements, and
   ``hds_flags`` is ``SPLIT``. A segment with no payload (a
   SYN, an ACK that was not padded) is ``SPLIT`` with no
   element used. ``SPLIT`` always comes with ``TCP_OK``, so
   the driver can tell the stack the checksum is verified
   without reading the payload.

   Every other frame fills the descriptor's buffer and then
   the elements, exactly as on a queue without header split,
   with ``SPLIT`` clear: UDP and every other protocol,
   fragments, IPv6 with any extension header, three or more
   VLAN tags, frames padded past the IP length (a short ACK
   padded to 60 bytes) or cut short of it, and frames with a
   wrong IPv4 or TCP checksum. So does a frame that would be
   split but whose payload does not fit the elements while
   the whole frame fits the buffers as they are, and one whose
   headers are longer than the descriptor's buffer; the
   latter has ``HBO`` set. A driver that posted unreadable
   payload buffers must drop a completion with ``SPLIT`` clear
   and ``num_sg_elems`` above 0: the device does not avoid
   producing it. A frame the buffers cannot hold is dropped
   and counted in ``rx_desc_data_error``, as on any queue.

   One frame is dropped here that a queue without header
   split would deliver. A frame that is to be split and has a
   payload has its SG list read even if all of it would fit
   the descriptor's buffer, because the payload belongs in
   the elements. If the list's address is not readable guest
   memory, the frame is dropped and counted in
   ``rx_desc_fetch_error``; a queue without header split
   reads the list only for a frame longer than the
   descriptor's buffer. A segment with no payload reads no
   list on either.

A driver that does not know the feature never sets the bits,
and sees only the receive checksum: its queues, buffers and
the rest of each completion are as with the mode off. Which
queue a frame lands on is not changed by the mode.

Statistics
^^^^^^^^^^

The ionic driver never asks the device for Ethernet
counters. At ``LIF_INIT`` it hands the device a ``struct
ionic_lif_info`` in its own memory, and from then on reads
``struct ionic_lif_stats`` (byte 320 of that block) whenever
``ip -s link``, ``/proc/net/dev`` or ``ethtool -S`` asks. The
emulator counts every frame the LIF moves and rewrites that
block from the main loop: once straight after ``LIF_INIT``,
then every ``stats.interval_ms`` (1000 by default). A real
NIC's firmware does the same.

.. code-block:: bash

   # rewrite the counters every 100 ms
   ./build/rocm-ernic --tap ernic0 --stats-interval-ms 100

Frames are classed by destination MAC: the group bit makes a
frame multicast, all-ones makes it broadcast, anything else
is unicast. What moves:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Event
     - Counters
   * - frame sent or received
     - ``{tx,rx}_{ucast,mcast,bcast}_{bytes,packets}``; the
       driver sums them into ``ip -s link`` packets and bytes
   * - receive with no buffer posted, or the queue of frames
       for the guest full
     - ``rx_*_drop_*`` and ``rx_queue_empty``
       (``rx_dropped`` and ``rx_over_errors``)
   * - frame larger than the posted buffer
     - ``rx_*_drop_*`` and ``rx_desc_data_error``
   * - DMA to or from guest memory failed
     - ``{tx,rx}_dma_error``
   * - the host backend refused a transmit
     - ``tx_*_drop_*``
   * - descriptor or its scatter-gather list unreadable, or
       nonsensical (see `Transmit offloads`_)
     - ``tx_desc_fetch_error``, ``tx_desc_data_error``
   * - doorbell on a queue that was never initialized
     - ``tx_queue_disabled``
   * - RDMA payload sent or landed
     - ``{tx,rx}_rdma_ucast_{bytes,packets}``, one packet per
       message

Frames are counted whether or not the server has a TAP:
without one, transmitted frames are counted and discarded.
The counters restart at zero on a device reset. After
``LIF_RESET`` or a device reset nothing more is written into
guest memory until the next ``LIF_INIT``, because the driver
may have freed the block. Nothing is written while the guest
has Bus Master Enable clear, or the function in D3hot, either;
the block is rewritten on the first interval after that ends.

The stats file (``--stats-file``) carries the same counters
in a ``LIF Statistics`` section, one line per named field of
``struct ionic_lif_stats`` whether or not it is zero, so a
test can check what the guest reports against what the
emulator counted. They agree to within one interval.

Not counted yet: ``rx_queue_disabled`` (the receive path
skips queues that are not initialized).

The RDMA error counters at qwords 96 to 125, and
``{tx,rx}_rdma_cnp_packets`` and ``rx_rdma_ecn_packets``,
belong to the data path, which serves the same counters
to the guest's ``hw_counters``; they are copied into this
block whenever it is written or read, so the two agree.
Those with no emulator event behind them yet (packet
sequence, duplicates, flushes, CNP and ECN) read zero. A
device reset zeroes them in the data path, so
``hw_counters`` restarts from zero along with this block.

``ernic-exporter`` reads that section and publishes every
counter as ``ernic_lif_counter_total``, labelled with the
``ionic_if.h`` name; the Grafana dashboard graphs them.
The guest's separate RDMA ``hw_counters`` view, which
the device serves from the admin queue rather than from
this block, and how the two counter sets compare with
the silicon, are in :doc:`monitoring`.

Port (MAC) statistics
~~~~~~~~~~~~~~~~~~~~~

At ``PORT_INIT`` the driver hands the device a ``struct
ionic_port_info``. Its ``struct ionic_port_stats`` (byte 576,
89 counters) is what ``ethtool -S`` lists under the port
names such as ``frames_rx_ok`` and ``frames_rx_64b``. The
emulator counts these at the wire, the way a MAC sees it, and
publishes them on the same tick as the LIF block.

A frame's wire length is its length padded to 60 bytes, as a
sending MAC pads it, plus the 4-byte FCS. A 42-byte ARP from
the TAP is therefore a 64-byte frame. What moves:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Event
     - Counters
   * - frame sent
     - ``frames_tx_all``, ``_ok``, ``octets_tx_total``,
       ``_ok``, the size bucket, the cast class
   * - frame received
     - ``frames_rx_all``, ``_ok``, ``octets_rx_all``, ``_ok``,
       the size bucket, the cast class
   * - VLAN tag (0x8100, or a 0x88a8 outer tag)
     - ``frames_rx_vlan_good`` or ``frames_tx_vlan``, and
       ``frames_{rx,tx}_pri_N`` from the tag's PCP; untagged
       frames count as ``_pri_0``
   * - received frame over the port's maximum
       (``link.mtu_max`` + 18)
     - ``frames_rx_too_long``, ``_oversized``, ``_bad_all``;
       the MAC drops it and the LIF never sees it
   * - received with no buffer posted, or the queue of frames
       for the guest full
     - ``frames_rx_dropped``, on top of the counters for a
       received frame: the MAC took it in, the LIF dropped it

The size buckets run 64, 65–127, 128–255, 256–511,
512–1023, 1024–1518, 1519–2047, 2048–4095, 4096–8191,
8192–9215 and 9216 and up (``_other``), so ``ping -s 1472`` lands in
``frames_rx_1024b_1518b``. Because every frame is padded,
``frames_rx_undersized`` and ``_less_than_64b`` stay zero, as
do the counters only a real wire can move: bad FCS, jabber,
fragments and pause frames.

``PORT_RESET`` stops the port writes but not the LIF ones,
and ``LIF_RESET`` does the reverse. ``PORT_RESET`` also zeroes
the port counters, so the next ``PORT_INIT`` starts from 0;
the LIF counters are kept. The stats file has a
``Port Statistics`` section, one line per counter.

Fault Injection
---------------

``--inject`` makes chosen events fail, so a test can check
how the guest copes. Give it once for each fault; a site
takes one fault at most:

.. code-block:: bash

   ./build/rocm-ernic --tap ernic0 \
     --inject 'stats_vals: after 2 count 3 effect fail status eagain' \
     --inject 'eth_rx: every 10 effect drop'

A fault reads ``SITE: PATTERN effect EFFECT [status S]``,
in the grammar of tc netem's loss models. The pattern says
which of the site's events the fault fires on, counting them
from 1:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Pattern
     - Fires on
   * - ``every N``
     - events N, 2N, 3N, ...
   * - ``after N [count M]``
     - events N to N+M-1; M is 1 if not given
   * - ``burst N every M``
     - the first N of every M events; N may not exceed M
   * - ``sequence "1x3,0x10"``
     - a list of ``1`` (fire) and ``0`` (pass) items, each
       repeated K times if written ``1xK`` or ``0xK``, run
       over and over; at most 65536 events long

Every number is at least 1. A pattern counts only its site's
events, never time, so the same guest workload gives the
same failures and a test can assert exact counts. A
malformed fault stops the server at startup with a message
naming the bad token.

The sites, what counts as an event at each, and the effects
each takes:

.. list-table::
   :header-rows: 1
   :widths: 14 28 58

   * - Site
     - Event
     - Effects
   * - ``stats_vals``
     - a ``STATS_VALS`` admin command: the guest reading the
       device's ``hw_counters``
     - ``fail [status S]``: the command completes with ionic
       status S and writes nothing. S is ``eio`` if not
       given; it is any ``IONIC_RC_*`` name in lower case
       without the prefix, such as ``eagain``, ``einval`` or
       ``bad_addr``
   * - ``eth_rx``
     - a frame arriving for the guest
     - ``drop``: the frame is lost before the NIC sees it.
       Nothing is delivered and no counter moves.

       ``nobuf``: the NIC drops it as if no receive buffer
       were posted. The port counts a received frame and
       ``frames_rx_dropped``; the LIF counts ``rx_*_drop_*``
       and ``rx_queue_empty`` (``rx_dropped`` and
       ``rx_over_errors`` in the guest). A frame over the
       port's maximum is counted as too long instead, as any
       other is, and the LIF never sees it
   * - ``eth_tx``
     - a frame gathered from a transmit descriptor
     - ``drop``: the frame is never sent, but the descriptor
       still completes, so the guest sees a successful
       transmit. Only the LIF's ``tx_*_drop_*`` count it
       (``tx_dropped`` in the guest); no port counter moves

The guest's kernel reads every ``hw_counters`` value once
while it registers the RDMA device, so that read is
``stats_vals`` event 1. A fault that fires on it fails
``ib_register_device()``, and the guest has no RDMA device.
``every N`` and ``after N`` with N of 2 or more leave it
alone; ``burst`` always fires on event 1, as does a
``sequence`` that starts with ``1``.

An ``eth_rx`` event is a frame read from the TAP, or one
that the S3 target or the P4 engine has queued for the
guest. A queued frame the guest has no buffer for yet waits
in the queue, and is one event however long it waits. With
the P4 engine on the wire, a TAP frame is an event before
the engine sees it, and the frame the program sends on to
the guest is another. An ``eth_tx`` event is counted whether
or not the server has a TAP, and before the S3 target or the
P4 engine sees the frame, so a dropped frame reaches neither.

At startup the server prints each fault in its canonical
form (``Fault: eth_rx: every 10 effect drop``). At a clean
shutdown it prints how many events each site saw and how
many the fault fired on, so a test can check the guest's
counters against them:

.. code-block:: text

   Fault eth_rx: eligible 1200, fired 120

Both lines are printed unless ``--log-level`` is ``error``
or ``none``. With ``--log-level info``, the server also logs
each ``stats_vals`` failure it injects, with the event's
number.

Guest Driver: Patched Upstream ionic
------------------------------------

The guest driver is *not* built from this repository. The
build fetches a pinned upstream Linux tree, applies the
patches in ``ansible/files/patches/``, and hands the
result to DKMS.

.. note::

   The patch series and the two scripts that apply it
   live in the Ansible collection so that a consumer
   installing ``rocm.ernic`` gets them at the same ref
   as the roles that apply them; see
   ``ansible/files/README.md``. The installed layout
   under ``share/rocm-ernic`` is unchanged.

Patches currently carried:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Patch
     - Purpose
   * - ``0002-ionic-allocate-an-address-handle-for-UC-queue-pairs.patch``
     - Allocates an address handle for UC queue pairs, which
       the upstream RDMA driver otherwise omits.

Building and Installing
^^^^^^^^^^^^^^^^^^^^^^^

Configure with ``-DERNIC_BUILD_KMOD=ON`` and use the
generated targets. These run inside the *guest*, where the
modules are needed:

.. code-block:: bash

   cmake -B build -G Ninja -DERNIC_BUILD_KMOD=ON
   cmake --build build --target fetch-ionic-sources
   cmake --build build --target build-ionic-dkms
   sudo cmake --build build --target install-ionic-dkms

   # and to back it out again
   sudo cmake --build build --target remove-ionic-dkms

``fetch-ionic-sources`` sparse-clones only the two ionic
subtrees --- ``drivers/net/ethernet/pensando/ionic`` and
``drivers/infiniband/hw/ionic`` --- from
``IONIC_KERNEL_REF`` (default ``v7.2.6``) and applies
``ansible/files/patches/*.patch`` in filename order with
``git am``. The
DKMS package is registered as ``ionic-ernic``.

.. warning::

   ``IONIC_KERNEL_REF`` must be at least ``v6.18``:
   ``drivers/infiniband/hw/ionic`` was merged for 6.18, so
   older refs cannot supply the RDMA half of the stack.

   The guest kernel's *major.minor* must also match the ref.
   The ionic sources track IB-core helpers that move between
   minor releases --- ``v7.2.4`` sources on a 7.0 kernel fail
   on ``ib_umem_get_va``, ``ib_copy_validate_udata_in`` and
   ``ib_respond_udata`` --- so the default ``v7.2.6`` needs a
   7.2.x guest. A point-release gap (``v7.2.4`` sources on
   7.2.6) is fine. No Ubuntu stock kernel qualifies today:
   noble HWE is 6.17 and resolute GA is 7.0, so the guest
   image bakes a mainline kernel from the Ubuntu mainline
   PPA --- the ``ionic`` flavour pins 7.2.6 --- and
   ``ernic_guest_setup`` asserts the match before it starts
   the DKMS build.

Loading and Verifying
^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   sudo modprobe ionic
   sudo modprobe ionic_rdma

   sudo update-pciids
   lspci -nnv -d 1dd8:1002 | grep '\[1dd8:5400\]'
   ip link                  # the LIF appears as a normal netdev
   ibv_devices              # the RDMA device appears here

Userspace: the Upstream Provider
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

No patched rdma-core is needed:
``providers/ionic`` landed upstream in rdma-core
v61, which Ubuntu 26.04 packages and the CI guest image
carries. The ``ernic_guest_setup`` role installs
``ernic_rdma_core_version`` (61.0 by default), building it
only when the guest's provider doesn't already match, and
then only verifies that ``libionic*.so`` landed in
``libibverbs/``.

There are two exceptions. No rdma-core release has shared
receive queues in ``providers/ionic`` yet, so a guest that
needs them builds the provider from the series in
``ansible/files/patches/rdma-core/`` (the
``ernic_srq_provider`` role). The other is GPU work. With
``ernic_gpu_passthrough`` on, the role applies
``rdma-core/ionic-gda/*.patch`` from the guest's rocm-xio
checkout before configuring, so the installed provider
exposes the direct-verbs symbols the ``GDA_IONIC`` backend
in rocm-xio links against. rocm-xio is then configured with
``-DGDA_IONIC=ON -DRDMA_CORE_BUILD=OFF`` so it uses that
system rdma-core rather than building a second copy.

Bumping the Upstream Baseline
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Change ``IONIC_KERNEL_REF`` in
``cmake/ErnicKernelModule.cmake`` and re-run
``fetch-ionic-sources``. If a patch no longer applies,
rebase it and refresh the file in
``ansible/files/patches/``. The
``ionic-patches`` CI job reads the pinned ref straight out
of the cmake file and fails the pull request if any patch
stops applying, so the two cannot drift apart.

The pin is also what the driver pack records for guests: the
install rules write it to
``share/rocm-ernic/ionic-kernel-ref``, and
``rocm-ernic-driver-pack`` copies that file into the tarball
so the guest builds from the same baseline the patches
beside it were generated against. Because it is captured at
install time, a bump needs ``cmake --install`` re-run on the
host before the next ``driver-pack``; neither script carries
a fallback, so a pack assembled without that file fails
immediately rather than guessing a ref.

Testing
-------

``tests/test_ionic_ci.sh`` is registered with CTest as
``ionic-ci`` and needs no VM. It checks that the server
starts on each backend, announces ``1dd8:1002`` with
subsystem ``1dd8:5400``, reports the expected BAR and MSI-X
geometry, shuts down cleanly on ``SIGTERM``, attaches to a
TAP when one is available, comes up with no extra flags,
reports receive header split under ``--nic-profile
header-split`` and only then, and writes the full counter set
to the stats file.

The TAP attach check is skipped unless ``ERNIC_TEST_TAP``
names an existing interface owned by the current user,
because creating one needs ``CAP_NET_ADMIN``:

.. code-block:: bash

   sudo ip tuntap add dev ernic-ci0 mode tap user "$USER"
   ERNIC_TEST_TAP=ernic-ci0 ctest --test-dir build -R ionic-ci

Current Status
--------------

Working:

- PCI enumeration, device identify, LIF init, and the
  admin queue
- Ethernet transmit and receive over TAP, including ARP,
  ICMP, and bulk TCP in both directions
- RC, UC, and UD queue pairs (``ib_*_pingpong`` and the
  in-tree ``rdma_verify`` payload checks)
- Fast registration: ``IB_WR_REG_MR`` and
  ``IB_WR_LOCAL_INV`` as local work requests, which is what
  a stock ``nvme-rdma`` initiator uses to produce the keys
  in its keyed SGLs
- guest-to-guest ``rdma_cm``, and the transfers that follow
  it: ``rping`` between two guests on a mesh connects,
  exchanges its payloads -- SEND, RDMA READ and RDMA WRITE,
  verified at both ends -- and exits 0
- flushing a QP that enters the Error state, so that work
  requests still outstanding are completed with
  ``IBV_WC_WR_FLUSH_ERR`` rather than left to a consumer that
  waits for them forever. One completion per queue, which is
  what the device owes: it names no work request, and the
  driver completes what it has posted from its own records.
  A QP bound to an SRQ flushes its send queue only, because
  the receives in an SRQ belong to the SRQ and other QPs are
  still drawing from them
- restarting a mesh worker. It comes back as a new node --
  it asks for an id of ``0xFFFFFFFF``, as any worker does
  when it starts, and its listening port is ephemeral, so
  nothing in the registration says which node it used to be
  -- and the GIDs it claimed before are withdrawn so that
  they follow it to its new id rather than being claimed by
  two nodes at once and resolving to neither. The manager
  withdraws what an unreachable node claimed both when its
  health check notices one has died and when any worker
  registers, the second being what covers a worker that
  comes back inside the health-check interval. The
  withdrawal is reversible: the claims are kept and only
  stop counting, so a node that was merely silent and then
  answers a heartbeat, or that registers again under its own
  id, owns its addresses again without advertising them
  afresh
- losing a mesh worker's connection to the manager. The
  worker connects again and registers under the id it has
  when the connection ends, or when nothing has come on it
  for three health-check intervals, and the manager gives
  the id back if the worker asks from the address it
  registered and can't still be answering on the connection
  it had. A worker whose id the manager no longer has (it
  evicted the node, or it has restarted) stops asking, and
  has to be restarted to rejoin. The manager closes the
  connection of a node that stops answering, rather than
  connecting to it. While a worker has no connection to the
  manager it hears nothing of the mesh, and reaches only the
  nodes it has a connection of its own to

Not yet working:

- ``rping`` prints ``poll error -5`` once as it tears down,
  from a bug in the rdma-core ionic provider rather than in
  the device. The provider keeps receive metadata in slots
  taken off a free list and records which slot a queue
  position used in ``rq.meta_idx``;
  ``ionic_poll_recv()`` reads it back through that index, but
  ``ionic_flush_recv()`` uses the raw ``wqe_id`` as the slot
  number. The two agree only until the first completion
  permutes the free list, after which the flush walk reads a
  slot that has already been returned, calls it "wqe not
  posted" and gives ``ibv_poll_cq()`` ``-EIO``. Present in
  rdma-core v61.0 and still in master
  (``providers/ionic/ionic_verbs.c``). The exchange itself is
  unaffected and ``rping`` exits 0.

  Establishment was broken until recently, and both reasons
  this page used to give were wrong: GSI traffic *is*
  forwarded between endpoints, which ``ernic-test-gsi-mesh``
  demonstrates, and both the link-local and the v4-mapped
  GID are populated. What actually stopped it was the shape
  of the 40-byte header in front of a datagram -- the device
  claimed RoCEv2 over IPv4 in the completion while writing an
  IPv6 GRH, so ``ib_cm`` read the middle of the destination
  GID as the sender's address, ARPed for ``0.0.255.255``,
  and dropped every CM REQ with ``-ETIMEDOUT`` and no
  reject. Fixed, pinned in both directions by
  ``ionic-harness-ud``, ``ionic-harness-nvmeof`` and
  ``gsi-mesh``, and confirmed on a two-guest pair: with the
  previous build the client printed nothing and the server's
  neighbour table held ``0.0.255.255 FAILED``; with the fix
  that entry is gone and the connection is accepted.

  The ``nvmeof`` backend does not depend on any of this: it
  answers the CM exchange inside the server rather than
  routing it to a peer, so ``nvme connect -t rdma`` has
  always had a responder (see :doc:`nvmeof`)

- Shared receive queues, which the upstream RDMA driver does
  not implement in its ``ib_device_ops``
- The link speed and width, which the guest sees only when the
  device sits behind a ``pcie-root-port``: on the root bus,
  where the examples here put it, QEMU makes it a Root Complex
  Integrated Endpoint, which has no link, so the guest reports
  them as unknown instead of 64 GT/s x16; this is cosmetic
