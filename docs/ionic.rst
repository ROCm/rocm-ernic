The ionic Device
================

rocm-ernic presents a single PCIe device to the guest:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - VID:DID
     - Guest driver
   * - ``1dd8:100a``
     - Upstream Linux ``ionic.ko`` + ``ionic_rdma.ko``,
       with the patches in ``patches/`` applied, and the
       upstream ``providers/ionic`` in rdma-core

The server emulates the register and queue protocol of the
AMD Pensando ionic NIC, so the guest runs a driver that is
already in mainline Linux rather than one that only exists
here. The driver source is near-stock --- two small patches,
one of them a device-ID addition --- but the guest itself is
not a stock cloud image: it needs a mainline kernel matching
``IONIC_KERNEL_REF`` and the headers to build against, which
is what the ``ernic_image_prep`` role provides. See the
warning below.

The Pensando vendor ID ``0x1dd8`` is kept, because that is
what the upstream driver claims, but device ID ``0x100a``
sits outside the range real hardware uses (``0x1002`` and
``0x1003``) so an emulated device can never be confused with
a physical DSC on the same host.

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

32 MSI-X vectors are advertised.

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
descriptor the guest has posted on the receive queue. Both
directions run on the thread that owns the vfio-user context,
because that is the only thread permitted to DMA into guest
memory; no extra thread is created.

.. note::

   The emulated LIF advertises no checksum, TSO, or
   scatter-gather offloads, so the guest stack always hands
   down linear skbs. This keeps the descriptor handling
   simple at the cost of transmit throughput.

Guest Driver: Patched Upstream ionic
------------------------------------

The guest driver is *not* built from this repository. The
build fetches a pinned upstream Linux tree, applies the
patches in ``patches/``, and hands the result to DKMS.

Patches currently carried:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Patch
     - Purpose
   * - ``0001-ionic-add-AMD-emulated-ionic-device-id.patch``
     - Adds ``1dd8:100a`` to the ionic PCI ID table so the
       upstream driver binds to the emulated device.
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
``patches/*.patch`` in filename order with ``git am``. The
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
   noble HWE is 6.17 and resolute GA is 7.0, so
   ``ernic_image_prep`` installs a matching kernel from the
   Ubuntu mainline PPA when it builds the golden image, and
   ``ernic_guest_setup`` asserts the match before it starts
   the DKMS build.

Loading and Verifying
^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   sudo modprobe ionic
   sudo modprobe ionic_rdma

   lspci -nn | grep 1dd8:100a
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
rebase it and refresh the file in ``patches/``. The
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
starts on each backend, announces ``1dd8:100a``, reports the
expected BAR and MSI-X geometry, shuts down cleanly on
``SIGTERM``, attaches to a TAP when one is available, comes
up with no extra flags, and writes the full counter set to
the stats file.

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
