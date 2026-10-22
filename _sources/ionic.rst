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
against, which no Ubuntu release ships. Nothing in this repo
bakes such an image either: the guest is the published
``ionic`` flavour of `batesste-ci-images
<https://github.com/sbates130272/batesste-ci-images>`_,
fetched by ``scripts/fetch-guest-image.sh``. See the warning
below.

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

All the RDMA backends (``loopback``, ``tcp``, ``verbs``,
``none``) work the same way: the backend choice governs the
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

The MSI-X table has room for 128 vectors; the device
advertises the profile's ``queues.intr_vectors`` of them (32
under ``compat``).

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
``small-vm``, ``generic-100g``, ``generic-400g``) or a file
(a name containing a ``/`` or ending in ``.ini``). The layers
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
     - ``none``
     - ``dev_info`` ASIC type. The driver runs its periodic
       doorbell re-check (``ionic_doorbell_wa()``) only for
       ``none`` and ``elba``
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
comes up with no Ethernet, which is why ``ci/doctor.sh`` and
``ci/jobs/vm-up.sh`` check for the interfaces up front.

The bridge carries no IP of its own by default; the guests
address each other directly across it. Set
``ernic_tap_bridge_ip`` if the host needs to join the segment
for debugging.

The transmit path gathers the head fragment plus any
scatter-gather elements out of guest memory and writes the
frame to the TAP file descriptor. The receive path drains the
TAP from the server's main loop and DMAs each frame into a
descriptor the guest has posted on one of its receive queues.
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
not fall back to another queue. A frame from the TCP mesh is
held until the queue it steers to has a buffer, and the mesh
frames behind it wait too.

.. note::

   The emulated LIF advertises no checksum, TSO, or
   scatter-gather offloads, so the guest stack always hands
   down linear skbs. This keeps the descriptor handling
   simple at the cost of transmit throughput.

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
   * - receive with no buffer posted, or the mesh inbox full
     - ``rx_*_drop_*`` and ``rx_queue_empty``
       (``rx_dropped`` and ``rx_over_errors``)
   * - frame larger than the posted buffer
     - ``rx_*_drop_*`` and ``rx_desc_data_error``
   * - DMA to or from guest memory failed
     - ``{tx,rx}_dma_error``
   * - the host backend refused a transmit
     - ``tx_*_drop_*``
   * - descriptor unreadable, or nonsensical
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
may have freed the block.

The stats file (``--stats-file``) carries the same counters
in a ``LIF Statistics`` section, one line per named field of
``struct ionic_lif_stats`` whether or not it is zero, so a
test can check what the guest reports against what the
emulator counted. They agree to within one interval.

Not counted yet: ``rx_queue_disabled`` (the receive path
skips queues that are not initialized) and the RDMA error
counters at qwords 96 to 125, which stay zero.

Guest Driver: Patched Upstream ionic
------------------------------------

The guest driver is *not* built from this repository. The
build fetches a pinned upstream Linux tree, applies the
patches in ``ansible/files/patches/``, and hands the
result to DKMS.

.. note::

   The patch series and the two scripts that apply it
   live in the Ansible collection so that a consumer
   installing ``sbates130272.rocm_ernic`` gets them at
   the same ref as the roles that apply them; see
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
``IONIC_KERNEL_REF`` (default ``v7.2.4``) and applies
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
   ``ib_respond_udata`` --- so the default ``v7.2.4`` needs a
   7.2.x guest. A point-release gap (``v7.2.4`` sources on
   7.2.3) is fine. No Ubuntu stock kernel qualifies today:
   noble HWE is 6.17 and resolute GA is 7.0, so the guest
   image bakes a mainline kernel from the Ubuntu mainline
   PPA --- the ``ionic`` flavour pins 7.2.3 --- and
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
v61, and the guest role installs v62.0. The
``ernic_guest_setup`` role therefore skips the provider
injection and registration steps entirely and only verifies
that ``libionic*.so`` landed in ``libibverbs/``.

The one exception is GPU work. With
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
TAP when one is available, comes up with no extra flags, and
writes the full counter set to the stats file.

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

Not yet working:

- ``rdma_cm`` connection establishment between two guests,
  and therefore ``rping``: only the link-local GID is
  populated, and GSI traffic is not forwarded between
  endpoints. The ``nvmeof`` backend sidesteps this by
  answering the CM exchange inside the server rather than
  routing it to a peer, so ``nvme connect -t rdma`` has a
  responder even though guest-to-guest ``rdma_cm`` does not
  (see :doc:`nvmeof`)
- Shared receive queues, which the upstream RDMA driver does
  not implement in its ``ib_device_ops``
- The PCI Express capability is not advertised, so the guest
  reports an unknown link speed and width; this is cosmetic
