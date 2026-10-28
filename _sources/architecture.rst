Architecture
============

This page explains how rocm-ernic makes a virtual machine
believe it has an RDMA network card. It covers the emulated
device and the code that implements it. Building, testing,
packaging, and deployment are covered in :doc:`building`,
:doc:`testing`, and :doc:`service`.

The audience is someone comfortable with operating systems,
PCI devices, and C, but not necessarily with RDMA. `RDMA in
brief`_ covers the vocabulary used in the rest of the page.

.. contents:: On this page
   :local:
   :depth: 1

The Short Version
-----------------

rocm-ernic is a userspace process (the *server*) that emulates
a PCIe RDMA NIC. QEMU connects to it over a UNIX socket using
the `vfio-user <https://github.com/nutanix/libvfio-user>`__
protocol, and plugs the emulated device into a guest VM as if
it were real hardware.

The device the server pretends to be is an **AMD Pensando
ionic** NIC. That choice means the guest runs the stock Linux
``ionic`` and ``ionic_rdma`` drivers and the stock rdma-core
``ionic`` provider; only one small driver patch is needed (see
:doc:`ionic`). Applications in the guest use ordinary
``libibverbs``.

There is no RDMA hardware anywhere. When the guest asks the
"NIC" to move data, the server reads the request out of guest
memory, copies the bytes itself, and writes a completion back
into guest memory. When both ends of a transfer are in the
same VM, that is a ``memcpy`` from one part of the guest's RAM
to another. When the other end is in a different VM (and so is
served by a different server process), the servers forward the
request to each other over ordinary TCP.

.. code-block:: text

    ┌──────────────────────── Guest VM ────────────────────────┐
    │  application  ──►  libibverbs + libionic (rdma-core)     │
    │                         │ control        │ data path     │
    │                         ▼                │ (kernel       │
    │                ionic_rdma.ko / ionic.ko  │  bypass)      │
    │                         │                │               │
    │         rings, buffers in guest RAM ◄────┘               │
    └─────────────────────────┬────────────────────────────────┘
                              │ PCI config, BAR reads/writes,
                              │ guest RAM shared by file descriptor,
                 QEMU         │ MSI-X interrupts
                              │ vfio-user protocol over a UNIX socket
    ┌─────────────────────────▼──────── rocm-ernic server ─────┐
    │  libvfio-user                                            │
    │  ionic device front end (registers, queues, data path)   │
    │  resource bookkeeping + backend (none/loopback/tcp/...)  │
    │  optional in-process NVMe-oF controller or S3 store      │
    │  optional P4 match-action engine                         │
    └─────────────────────────┬────────────────────────────────┘
                              │ TCP (tcp backend only)
                              ▼
                 other rocm-ernic servers, one per VM

RDMA in Brief
-------------

RDMA (remote direct memory access) lets a program move data
between its memory and another machine's memory without either
kernel copying the data, and often without either CPU touching
it. The NIC does the work. The programming interface is called
**verbs** (``libibverbs``).

The pieces that matter for this page:

Queue pair (QP)
   The endpoint of a connection, roughly the RDMA equivalent of
   a socket. A QP has a **send queue (SQ)** and a **receive
   queue (RQ)**. Each is a ring buffer in host memory that the
   application fills with **work queue entries (WQEs)**, one per
   request.

Doorbell
   After writing WQEs into a ring, software writes the new
   producer index to a device register so the NIC knows to look.
   That register write is the doorbell.

Completion queue (CQ)
   Another ring, written by the NIC. When the NIC finishes a
   work request it writes a **completion queue entry (CQE)**.
   Software polls the CQ to find out what finished and whether
   it succeeded.

Event queue (EQ) and interrupts
   Software can *arm* a CQ to ask for an interrupt when the next
   CQE arrives. The ionic device reports that through an event
   queue: it writes an **event queue entry (EQE)** naming the
   CQ, then raises the MSI-X interrupt assigned to that EQ.

Memory region (MR)
   Before the NIC may touch a buffer, the application
   *registers* it. Registration pins the pages and gives the NIC
   a translation from the application's virtual addresses to
   physical pages. It returns two keys: the **lkey**, used in
   the application's own WQEs to name local memory, and the
   **rkey**, given to the remote side so it can name this memory
   in RDMA READ and WRITE requests.

Scatter/gather element (SGE)
   A WQE names its buffers as a list of ``(address, length,
   lkey)`` triples.

Operations
   *SEND* delivers a message into the next buffer the remote
   side has posted on its RQ (two-sided: the receiver
   participates). *RDMA WRITE* and *RDMA READ* access remote
   memory directly, named by rkey and address (one-sided: the
   remote CPU is not involved). *Atomics* (compare-and-swap,
   fetch-and-add) operate on 8 bytes of remote memory. Variants
   *with immediate* also carry 32 bits of data that show up in
   the receiver's CQE.

Transport types
   *RC* (reliable connected) is a reliable, ordered, one-to-one
   connection. *UC* (unreliable connected) is one-to-one without
   delivery guarantees. *UD* (unreliable datagram) is
   connectionless: each send names its destination with an
   **address handle (AH)** and a remote QP number, and receivers
   check a **Q_Key**. Received UD messages are prefixed by a
   40-byte **global route header (GRH)**.

GSI / QP1
   A special UD QP used by the kernel for connection management
   (CM) messages, called **MADs** (management datagrams).
   ``rdma_cm``, and therefore things like
   ``nvme connect -t rdma``, set up RC connections by exchanging
   MADs on QP1.

RoCE and GIDs
   RDMA over Converged Ethernet runs RDMA over Ethernet and
   UDP/IP. Endpoints are named by a 128-bit **GID**, which for
   RoCE is an IPv6 address or an IPv4 address in
   ``::ffff:a.b.c.d`` form.

Protection domain (PD)
   A grouping that keeps one consumer's QPs from using another
   consumer's MRs.

Shared receive queue (SRQ)
   A receive ring several QPs draw from.

Fast registration
   A WQE (*REG_MR*) that binds pages and a fresh key to a
   pre-allocated MR without a system call, and *LOCAL_INV* to
   unbind it. The Linux ``nvme-rdma`` initiator uses this for
   every I/O.

Two properties of RDMA shape the whole emulator:

#. **The data path bypasses the kernel.** After setup, a user
   process writes WQEs into rings in its own memory and writes
   doorbells into a device page mapped into its address space.
   It polls CQs the same way. The guest kernel is not involved
   in moving data, so neither is anything that could be
   intercepted in it. The emulator has to work at the level of
   rings, doorbells, and memory.
#. **The device reads and writes host memory on its own.**
   Every ring and every buffer lives in guest RAM, and the
   device is expected to DMA into it. The emulator has to be
   able to reach guest memory directly.

How a PCI Device Is Emulated in Userspace
-----------------------------------------

`vfio-user
<https://github.com/nutanix/libvfio-user/blob/master/docs/vfio-user.rst>`__
is a protocol for implementing a PCI device in a separate
process. QEMU is the client and rocm-ernic is the server; the
server uses the libvfio-user library. What the protocol gives
the server:

- **PCI configuration space.** The server declares the vendor
  and device IDs, class code, and capabilities (MSI-X here).
  libvfio-user answers config space accesses.
- **BARs.** The server declares memory regions and a callback
  for each. When the guest reads or writes a BAR, QEMU traps
  the access and sends it over the socket, and libvfio-user
  calls the server's callback with the offset, size, and data.
  rocm-ernic doesn't let QEMU map any of its BARs directly, so
  **every register access and every doorbell is a message on
  the socket.**
- **Guest memory.** QEMU passes the server file descriptors for
  the guest's RAM, which libvfio-user maps into the server's
  address space. To "DMA", the server asks libvfio-user to
  translate a guest physical address (``vfu_addr_to_sgl()``,
  ``vfu_sgl_get()``) and gets back an ordinary host pointer. A
  DMA read or write is then just ``memcpy``. The ionic front end
  also marks the pages it writes as dirty
  (``vfu_sgl_mark_dirty()``), which is what live migration
  would need.
- **Interrupts.** ``vfu_irq_trigger()`` signals an eventfd that
  QEMU turns into an MSI-X interrupt in the guest.
- **Resets.** libvfio-user tells the server when the guest
  resets the device or the client disconnects.

The libvfio-user context isn't thread-safe, so in rocm-ernic
**only the main thread may touch guest memory or the
context.** That rule shapes how the multi-VM code is built (see
`Threads`_).

The Device the Guest Sees
-------------------------

The server presents one PCIe function:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Field
     - Value
   * - Vendor : device
     - ``1dd8:1002`` (Pensando, upstream ionic PF)
   * - Subsystem
     - ``1dd8:5400`` (identifies the emulated function)
   * - Class
     - ``02:00:00``, Ethernet controller
   * - Interrupts
     - The NIC profile's MSI-X vectors (32 under ``compat``, at
       most 128), plus INTx

The upstream ``ionic.ko`` binds to that ID.

Some of what the device reports is set by its **NIC profile**:
its interrupt, Ethernet queue and RDMA event queue counts, link
speed and transceiver, largest MTU, firmware version, and how
often it rewrites its statistics. ``--nic-profile`` layers
built-in profiles and ini files over the default, ``compat``,
whose values this page gives. See :doc:`ionic` for the keys and
the built-in profiles.

The device has two BARs:

.. list-table::
   :header-rows: 1
   :widths: 10 15 75

   * - BAR
     - Size
     - Contents
   * - BAR 0
     - 64 KiB
     - 32 KiB ionic register window, then the MSI-X table and
       PBA
   * - BAR 2
     - 4 MiB
     - Doorbell pages, 4 KiB each

The BAR 0 register window is laid out exactly as the driver's
``ionic_dev_setup()`` expects:

.. code-block:: text

   0x0000  dev_info_regs   signature "DEVI", firmware status,
                           heartbeat, version
   0x0800  dev_cmd_regs    device-command doorbell, done,
                           64-byte command, 16-byte completion,
                           1912-byte data window
   0x1000  intr_status
   0x2000  intr_ctrl       32 bytes per vector: mask, credits,
                           mask-on-assert
   0x8000  MSI-X table     (above the window, so the driver
                           never sees it)
   0xa000  MSI-X PBA

Within a doorbell page, the byte offset selects a **queue
type** (eight bytes per type) and the written 64-bit value
carries the queue id, a producer index, and a "ring" field that
distinguishes "new work" from "arm this CQ":

.. code-block:: text

   bits 15:0   p_index   producer index (or CQ consumer index)
   bits 23:16  ring      0 = work, 1 = arm CQ,
                         2 = arm CQ for solicited events
   bits 47:24  qid

The kernel uses doorbell page 0; each user process gets its own
page so it can ring doorbells without a system call. The server
decodes only the offset within the page, so every page behaves
the same.

An ionic NIC is an Ethernet NIC first. The driver brings up an
Ethernet *logical interface* (LIF), and only then creates an
auxiliary device that ``ionic_rdma.ko`` binds to. So the
emulator implements both halves:

- **Ethernet** queue types 0 to 3: the Ethernet admin queue,
  notify queue, receive queues, and transmit queues.
- **RDMA** queue types 5 to 9: the RDMA admin queue (AQ), SQ,
  RQ, CQ, and EQ. Shared receive queues ring the RQ doorbell
  type with ids above the QP id range.

Inside the Server
-----------------

.. code-block:: text

                      rocm_ernic_server.c
            main(), BAR/IRQ/PCI setup, the main loop
                               │
       ┌───────────────────────┼─────────────────────────────┐
       │ ionic front end       │                             │
       │                       ▼                             │
       │  ionic_device.c ──── builds, wires, polls and tears │
       │    │                 down the four below            │
       │    ▼                                                │
       │  ionic_eth_emu.c ─── BAR 0 registers, device        │
       │    │    │            commands, Ethernet queues,     │
       │    │    │            interrupt controller, BAR 2    │
       │    │    │            doorbell decode                │
       │    │    └──► ionic_eth_net.c   host TAP interface   │
       │    ▼                                                │
       │  ionic_rdma_devcmd.c ─ RDMA bootstrap: EQs and the  │
       │    │                   RDMA admin queues            │
       │    ▼                                                │
       │  ionic_adminq.c ──── control path: create/modify/   │
       │    │                 destroy CQ, QP, MR, AH, SRQ    │
       │    ▼                                                │
       │  ionic_datapath.c ── data path: SQ/RQ/CQ rings, MR  │
       │                      table, data copies, CQEs, the  │
       │                      inter-server wire protocol     │
       └──────┬───────────────────────────┬──────────────────┘
              │ ernic_rm_*, ernic_mesh_*  │ attach
              ▼                           ▼
       rocm_ernic_compat.c          src/nvmeof/, src/s3/
       (bridge; only file that      in-process targets;
        sees QEMU types)            src/p4/, the optional
              │                     P4 engine
              ▼
       src/pvrdma/                resource manager (rdma_rm)
       src/rdma/                  backend vtable: none,
                                  loopback, tcp mesh
       src/qemu-compat/           the QEMU APIs the QEMU-derived
                                  code expects

The ionic Front End
^^^^^^^^^^^^^^^^^^^

The files in ``src/ionic/`` are the device the guest actually
talks to. They own the register file, decode doorbells, and
read and write every ring and buffer in guest memory. Each file
is described in a later section.

Two more pieces shape what the front end does without being
part of any one path:

- **The NIC profile** (``src/ionic/ionic_profile.c``; the
  built-in profiles are in ``src/ionic/ionic_profiles.c``,
  with copies in ``profiles/``) is read once at startup. The
  front end consults it wherever it describes itself to the
  driver: the MSI-X table size, IDENTIFY, LIF_IDENTIFY,
  PORT_INIT, and LIF_INIT.
- **Fault injection** (``--inject``,
  ``src/ionic/ionic_fault.c``) makes chosen events fail, so a
  test can check how the guest copes: a STATS_VALS admin
  command completes with an error status, a received frame is
  lost or dropped as if no buffer were posted, or a
  transmitted frame is completed but never sent. A fault fires
  on a pattern of events (every Nth, a run starting at the Nth,
  a burst out of every M, or an explicit sequence), never on a
  timer, so the same workload gives the same failures.

The Device Core
^^^^^^^^^^^^^^^

The device core is ``src/rocm_ernic_compat.c``,
``src/pvrdma/``, and ``src/rdma/``.

rocm-ernic began as an emulator of the VMware paravirtual RDMA
device (PVRDMA), using the device model from QEMU's
``hw/rdma/``. That guest-facing personality has been removed
and replaced by the ionic front end, but two pieces of the QEMU
code are still underneath it, which is why many names
(``pvrdma_handle``, ``PVRDMADev``)
still say "pvrdma":

- **rdma_rm**, the resource manager: handle tables for PDs,
  CQs, QPs, MRs, and SRQs, and each QP's current state (RESET,
  INIT, RTR, RTS, ...).
- **rdma_backend**, which forwards resource operations to a
  pluggable **backend** through the vtable in
  ``src/rdma/rdma_backend_ops.h``.

The ionic admin queue reaches these through the ``ernic_rm_*()``
wrappers in ``rocm_ernic_compat.c``, which is the only file that
includes the QEMU headers. The front end sees the core only
through three small headers, each included only where it's
used: ``rocm_ernic_compat.h`` creates and destroys it, and
``ernic_rm.h`` and ``ernic_mesh.h`` declare the wrappers for the
resource manager and the mesh. Today the core's jobs in the
ionic world are:

#. Bookkeeping and limits for each object the guest creates.
#. Recording each QP's state and passing the INIT, RTR, and RTS
   transitions to the backend. The core doesn't check that a
   transition is legal: it records whatever state the guest
   asks for. The one check that matters is on a connected QP's
   destination GID when the mesh is attached: if no node owns
   it, the admin queue refuses the MODIFY_QP before the core
   sees it, and the TCP backend would fail the RTR transition
   anyway (see `More Than One VM: the TCP Mesh`_).
#. Carrying the TCP mesh, through the ``ernic_mesh_*()``
   wrappers. They call the backend's optional mesh operations
   (``RdmaMeshOps``), which only the TCP backend has; on any
   other backend they answer that there is no mesh.

The core does **not** move data for the ionic device, or post
its completions. All data movement happens in
``ionic_datapath.c``. Nor does it keep the statistics: the
server does (``src/ernic_stats.c``), and hands them to the ionic
device, which counts into them. They are the device and per-QP
counters the guest reads as ``hw_counters`` (through the admin
queue's statistics commands, together with the data path's
error counters), and what the stats file shows, written about
once a second (``--stats-file``, consumed by
``ernic-exporter``; see :doc:`monitoring`). The ionic device
adds a section to the file for the Ethernet side's LIF and port
counters.

The Main Loop
^^^^^^^^^^^^^

``src/rocm_ernic_server.c`` builds the statistics, the RDMA core
and its backend, then the ionic device in front of them
(``ionic_device_create()`` in ``src/ionic/ionic_device.c``,
which creates the ionic objects and wires them together once),
then waits for QEMU to connect. Once QEMU has connected, it
loops:

#. Write the stats file, if there is one, about once a second.
#. ``vfu_run_ctx()``: handle pending vfio-user messages. BAR
   accesses arrive here as callbacks, and **BAR 2 doorbells for
   send queues are executed inside the callback** (see
   `The Data Path`_).
#. Run any GLib idle callbacks.
#. ``ionic_device_poll()``, the device's share, which is the
   next three steps.
#. Move received Ethernet frames from the TAP, and frames an
   in-process endpoint queued for the guest, into the guest's
   receive rings. Rewrite the Ethernet statistics in guest
   memory if the profile's interval has passed.
#. Process new entries on the RDMA admin queues.
#. Run the S3 store's TCP timers, apply messages that arrived
   from other servers over the mesh, and time out requests that
   never got an answer.
#. If no vfio-user message or idle callback ran, and nothing is
   waiting from the mesh or the S3 store, sleep 100 µs.

The server is a polling design. Latency is bounded by the loop
period, and an idle server still wakes up as often as 10,000
times a second.

Bringing the Device Up
----------------------

Getting from "PCI device present" to "``ibv_devices`` lists an
RDMA device" is a conversation between the driver and the
emulated firmware. Almost all of the emulator's work here is
returning exactly the bytes the driver expects in exactly the
places it looks.

Device Commands over BAR 0
^^^^^^^^^^^^^^^^^^^^^^^^^^

``ionic.ko`` reads the ``DEVI`` signature and the "firmware
running" bit from ``dev_info_regs``. From then on it issues
**device commands**: it writes a 64-byte command into
``dev_cmd_regs``, writes 1 to the command doorbell, and polls
the ``done`` register. ``ionic_eth_emu.c`` runs each command
synchronously inside the BAR write, fills in the completion and
the data window, and sets ``done``.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Command
     - What the emulator does
   * - IDENTIFY
     - Reports one port, one LIF, and the profile's interrupt
       and RDMA EQ counts (32 and 32 under ``compat``)
   * - LIF_IDENTIFY
     - Reports Ethernet and RDMA capability (and P4, when the
       guest has been given the engine's control surface; see
       :doc:`p4`), the profile's Ethernet queue count and
       maximum frame size, the MAC, and the RDMA section: queue
       type numbers; QP, CQ, and SRQ counts; MR and AH limits;
       and the page table size, which sets the guest's maximum
       MR size (4 TiB)
   * - PORT_INIT
     - Writes a port-info block giving the profile's link speed
       and transceiver ("100 Gb/s copper QSFP, link up" under
       ``compat``), so ``ethtool`` reports a speed. The device
       keeps the port statistics in that block from then on
   * - LIF_INIT
     - Writes "link up" and the speed into the LIF status block,
       so the netdev gets a carrier and the RDMA port goes
       active. The device keeps the LIF statistics in that block
       from then on (see :doc:`ionic`)
   * - LIF_RESET, PORT_RESET, RESET
     - Stop writing into the blocks, which the driver may free.
       RESET also zeroes every counter, and PORT_RESET the port
       counters
   * - Q_INIT
     - Records where an Ethernet queue's descriptor and
       completion rings are in guest memory, and its interrupt
       vector. Fails for an unknown queue type, a queue index
       past the LIF's queue count, or a ring deeper than 2^15
   * - Q_CONTROL
     - Enables or disables a queue; received frames are steered
       only to enabled receive queues
   * - Q_IDENTIFY
     - Reports descriptor sizes; advertises no offloads
   * - LIF_GETATTR, LIF_SETATTR
     - Read or set the MAC; other attributes succeed without
       effect
   * - 50 to 53
     - The RDMA bootstrap commands (see `RDMA Bootstrap`_)
   * - 70 to 72
     - The P4 engine's control surface; unknown opcodes when the
       guest hasn't been given it (see :doc:`p4`)
   * - INIT, GETATTR, SETATTR, PORT_IDENTIFY, PORT_GETATTR,
       PORT_SETATTR, NOP
     - Succeed with a zeroed completion
   * - others
     - Fail as unknown opcodes

The Ethernet Admin Queue
^^^^^^^^^^^^^^^^^^^^^^^^

Once its admin queue is set up, ``ionic.ko`` stops using BAR 0
and posts commands as 64-byte descriptors in a ring in guest
memory, rings a doorbell, and waits for an interrupt. The
emulator reads each descriptor by DMA, runs it, DMAs a 16-byte
completion with the expected *color* bit (see `Rings and the
Color Bit`_), and raises the queue's MSI-X vector.

The commands that matter here are Q_INIT, Q_CONTROL, the MAC
attribute of LIF_GETATTR and LIF_SETATTR, and the RDMA and P4
commands, which the emulator runs as it does from BAR 0.
Everything else the driver posts during bring-up (receive
modes, receive filters, ...) has no state in the emulator and
succeeds.

RDMA Bootstrap
^^^^^^^^^^^^^^

``ionic_rdma.ko`` then sends four RDMA commands, opcodes 50 to
53, over the Ethernet admin queue. ``ionic_rdma_devcmd.c``
handles them:

.. list-table::
   :header-rows: 1
   :widths: 10 30 60

   * - Opcode
     - Command
     - Emulator state
   * - 50
     - RDMA_RESET_LIF
     - Forget all EQs and admin queues, and free every QP, CQ,
       MR, SRQ and address handle the driver made
   * - 51
     - RDMA_CREATE_EQ
     - Record an event queue ring and its MSI-X vector (at
       least four are created)
   * - 52
     - RDMA_CREATE_CQ
     - Record the completion ring for the next admin queue
   * - 53
     - RDMA_CREATE_ADMINQ
     - Pair the admin queue with that CQ and start servicing it
       in ``ionic_adminq.c``

The driver then registers its IB device, and the guest can open
it with ``libibverbs``.

Interrupts
^^^^^^^^^^

The emulator keeps a shadow of the per-vector interrupt
controller in BAR 0: a mask, and a *mask-on-assert* flag that
makes the vector mask itself when it fires (so the driver's
NAPI poll runs without a second interrupt). An interrupt raised
while its vector is masked is latched and delivered when the
driver unmasks it, which it does by writing the mask register
or by returning "credits" with the unmask bit set.

The Control Path: the RDMA Admin Queue
--------------------------------------

Everything a verbs application does that is not on the fast
path (``ibv_create_cq``, ``ibv_create_qp``, ``ibv_reg_mr``,
``ibv_modify_qp``, ...) becomes a system call into
``ionic_rdma.ko``, which posts a command on an RDMA admin
queue. ``src/ionic/ionic_adminq.c`` services those queues.

The AQ doorbell only records the new producer index. The main
loop later reads each new WQE (64 bytes, or several 64-byte
strides for long commands), runs it, writes a 32-byte admin
CQE, and then posts an EQE and raises the EQ's interrupt. The
driver drains admin CQs only from its EQ handler, so without
the EQE the command would time out.

Most commands do two things: create the object in the `device
core <The Device Core_>`_, and tell the data path where the
guest put the object's memory.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Command
     - Effect
   * - CREATE_CQ
     - Allocate a CQ in ``rdma_rm``; register the CQ ring and
       its EQ with the data path
   * - CREATE_QP
     - Allocate a QP; register its SQ and RQ rings (or bind it
       to an SRQ) and its CQs with the data path
   * - CREATE_MR
     - Allocate an MR; register its address range and page
       table with the data path under its key
   * - CREATE_AH
     - Parse the address handle's packet header template for
       the destination GID; register it with the data path
   * - MODIFY_QP
     - Record the QP's new state. At RTR, record the peer QP
       and which server it lives on; for UD, record the Q_Key.
       Nothing is recorded unless the backend makes the
       transition, so a MODIFY_QP that fails changes nothing.
       GSI's transitions don't go to the backend; they are
       only recorded
   * - QUERY_QP
     - DMA the QP's state, PSNs, peer QP (by the id the driver
       gave it), and access flags into the buffers the driver
       supplied
   * - CREATE_SRQ
     - Allocate an SRQ id (the device assigns it) and register
       its ring
   * - MODIFY_SRQ, QUERY_SRQ
     - Set or read the low-watermark limit
   * - DESTROY_*
     - Undo the above
   * - STATS_HDRS, STATS_VALS, QP_STATS_HDRS, QP_STATS_VALS
     - DMA the names, or the values, of the device's or one
       QP's counters into the driver's buffer; this is what the
       guest reads as ``hw_counters`` (see :doc:`monitoring`)
   * - MODIFY_DCQCN, DEBUG, QUERY_AH
     - Succeed without doing anything

A few details worth knowing:

- **Two id spaces.** The driver names objects by its own ids,
  and ``rdma_rm`` hands out its own handles. The admin queue
  keeps tables mapping one to the other. The data path is
  indexed by the driver's ids.
- **One PD.** The ionic driver allocates protection domains
  itself and never tells the device, so the emulator creates a
  single PD and uses it for everything.
- **The destination is in a packet header.** The driver never
  gives the device a destination GID as a field. Instead,
  CREATE_AH and the RTR transition of MODIFY_QP point at a
  prebuilt Ethernet/IP/UDP header template (what real hardware
  would put on the wire). The emulator parses the IPv4 or IPv6
  addresses out of it. That is how it learns which GID the
  guest is talking to and, with the TCP mesh, which server owns
  that GID.
- **QP type numbering.** ionic numbers QP types differently
  from the IB core and folds GSI into UD; the GSI QP is
  recognized by its reserved id 1.

The Data Path
-------------

``src/ionic/ionic_datapath.c`` is the heart of the emulator:
it does what a NIC's hardware data path would do.

Guest Memory, Three Levels Down
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A WQE names memory as ``(lkey, virtual address, length)`` in
the address space of whatever process posted it. Turning that
into bytes the server can touch takes three translations:

.. code-block:: text

   (lkey, va, len)   from an SGE in a WQE
         │  MR table: lkey → MR (va range, page table)
         │  bounds check: [va, va+len) must lie inside the MR
         ▼
   guest physical page(s)   from the MR's page table
         │                  (read once, at registration)
         │  libvfio-user: vfu_addr_to_sgl() / vfu_sgl_get()
         ▼
   host virtual address in the server   → memcpy

The page table a driver provides for an MR, CQ, or QP ring is
either a single DMA address or the guest address of an array of
page addresses; the data path reads and caches it when the
object is registered, and the command creating the object fails
if it can't. Runs of physically adjacent pages are
merged so a large transfer isn't split at every 4 KiB. Key 0 is
the driver's reserved DMA key: the address is already a guest
bus address. The kernel uses it for its own buffers.

Because one server serves exactly one guest, the MR table holds
every key that guest has created, **lkeys and rkeys alike**. An
RDMA WRITE to another QP in the same VM resolves its rkey in the
same table as its own lkeys.

Rings and the Color Bit
^^^^^^^^^^^^^^^^^^^^^^^

All ionic rings are power-of-two sized and indexed modulo their
depth. The CQs, EQs, and Ethernet completion rings use a
*color* bit instead of a shared producer index: each entry has a
bit that the device sets to the current color, and the color
flips each time the producer wraps. The guest knows an entry is
new when its color matches what it expects. The emulator keeps
the color per ring. For the RDMA data path's CQs, it writes the
word that holds the color bit after the rest of the entry, so
the guest never sees a half-written CQE. Admin queue and
Ethernet completions are each written in a single copy, with no
such ordering.

Executing a Send Queue
^^^^^^^^^^^^^^^^^^^^^^

A send-queue doorbell is handled **synchronously in the BAR 2
write callback**. The emulator walks the SQ from its consumer
index up to the doorbell's producer index and executes each
WQE:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Operation
     - Local behavior
   * - SEND, SEND_IMM, SEND_INV
     - Take the next WQE posted on the destination QP's RQ (or
       its SRQ), copy the payload into the buffers that WQE
       names, and post a receive CQE on the destination's CQ
   * - RDMA_WRITE
     - Copy from the local SGEs to ``(rkey, remote va)``
   * - RDMA_WRITE_IMM
     - As RDMA_WRITE, then consume a receive on the destination
       QP and post a CQE carrying the immediate value
   * - RDMA_READ
     - Copy from ``(rkey, remote va)`` into the local SGEs
   * - ATOMIC_CS, ATOMIC_FA
     - Read 8 bytes at the rkey address, write the new value,
       return the old one to the local SGE
   * - REG_MR (fast registration)
     - Bind a new key and page table to an MR, dropping the
       previous key for that MR
   * - LOCAL_INV
     - Unbind a key

Inline sends (payload carried in the WQE itself) are handled by
treating the WQE's own payload area as the source buffer. One
claiming more than that area holds fails with a length error,
as does a send whose SGE lengths add up to more than 4 GiB.

The driver creates a queue that takes three to eight SGEs with
*speculative* SGE lists. A WQE on one with more than two SGEs
starts with a table of their lengths, 32-bit or 16-bit as its
flags say, in the first two SGE slots, and the SGEs follow it.
The emulator skips the table and reads the SGEs. A WQE whose
flags name a list format the driver doesn't define is refused
rather than guessed at: a send completes with a QP operation
error, and a receive takes no data.

Copies between two guest buffers go through a 4 KiB bounce
buffer on the server's stack, one contiguous run at a time.

Each WQE then gets a send completion on the SQ's CQ. RC and UC
requests complete with a running *message sequence number*
(MSN); everything else completes by SQ index. The emulator
posts a send completion for **every** WQE, signaled or not,
because the driver will not advance past an entry that has no
completion; whether the application sees a work completion is
up to the driver.

Failures are reported the way hardware would: a receive buffer
that is too small gives a length error, an SGE outside the MR
its key names gives a protection error, a bad rkey gives a
remote access error, and so on, with the error bit set in the
CQE.

Example: an RDMA WRITE Between Two QPs in One VM
""""""""""""""""""""""""""""""""""""""""""""""""

#. The application calls ``ibv_post_send()``. The rdma-core
   provider writes the WQE into the SQ ring (in the
   application's memory) and writes the new producer index to
   its doorbell page in BAR 2. The kernel is not involved.
#. QEMU traps the write and sends it to the server.
   libvfio-user calls ``bar2_access()``, which hands it to
   ``ionic_eth_emu_bar2_access()``. That decodes queue type 6
   (SQ) and calls ``ionic_datapath_doorbell()``.
#. ``process_sq_wqe()`` reads the WQE from guest memory, parses
   the SGEs, resolves the local lkeys and the remote rkey in the
   MR table, and copies the bytes page by page from the source
   buffer to the target buffer.
#. It writes a send CQE into the SQ's CQ ring.
#. If the application armed the CQ, the emulator writes an EQE
   naming the CQ into the CQ's event queue and raises that EQ's
   MSI-X vector.
#. The application's ``ibv_poll_cq()`` finds the CQE by its
   color bit.

Receive Queues and CQ Doorbells
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

RQ doorbells only advance the ring's producer index: posting a
receive does nothing until a SEND arrives for it. CQ doorbells
either report the guest's consumer index (ring 0) or arm the CQ
(ring 1 or 2). Arming behaves like a level, not an edge: if
completions are already waiting when the CQ is armed, the event
fires at once.

Datagrams (UD and GSI)
^^^^^^^^^^^^^^^^^^^^^^

A UD send names its destination per WQE: an AH id and a
destination QP number. The emulator delivers by QP number, and
uses the AH only to decide which server the destination lives
on and to fill in the GRH. An AH whose destination GID no server
owns is treated as local, so sends through it are delivered by
QP number within the same VM.

When the destination is in the same VM, the emulator enforces
the datagram rules before delivering: the destination must
itself be a UD or GSI QP, and if it has a Q_Key, the sender's
must match, a controlled key in the WQE standing for the
sending QP's own. A datagram that crosses the mesh is not
checked (see `Trust Boundaries`_).

The received message starts with a 40-byte GRH carrying the
source and destination GIDs, as the verbs API requires, and the
CQE carries a source MAC address synthesized from the sending
QP.

More Than One VM: the TCP Mesh
------------------------------

Each server serves one VM. For QPs in different VMs to talk,
the ``tcp`` backend (``src/rdma/rdma_backend_tcp.c`` and the
``tcp_*.c`` files beside it) connects the servers into a mesh,
and the data path sends requests across it.

Topology
^^^^^^^^

.. code-block:: text

            ┌────────────────────┐
            │ manager (node 0)   │   tcp:manager:listen:5000
            └──┬──────────────┬──┘
               │              │      workers register with the
      ┌────────┴──┐       ┌───┴───────┐  manager, which assigns
      │ worker 1  │───────│ worker 2  │  node ids and broadcasts
      └───────────┘       └───────────┘  the topology
          tcp:worker:<manager>:5000

- The manager is node 0. Workers connect to it, register, and
  get node ids 1, 2, and so on, in the order they arrive.
- The manager broadcasts the topology; workers connect directly
  to each other (the lower id connects to the higher), and
  connect again if the connection drops. A message for a node
  with no direct connection is relayed by the manager.
- The manager sends heartbeats, notices dead nodes, and
  closes their connections. It never connects to a worker:
  a worker whose connection to the manager ends, or carries
  nothing for three health-check intervals, connects again
  and registers under the id it has. A worker that has heard
  nothing for an interval sends the manager a heartbeat of
  its own, so the two needn't share an interval.
- A dead node stays in the manager's table. A node that went
  quiet and comes back keeps its id; a restarted one registers
  again and gets a new id. The table holds at most 64 nodes:
  when a new node registers in a full table, the manager evicts
  the node that has been dead longest, or refuses the new node
  if none can be evicted. An id is never given out twice.
- A send to a node that stays connected but stops reading
  gives up once nothing has gone out for the send timeout,
  ``ERNIC_TCP_SEND_TIMEOUT_MS`` (10000 by default), and shuts
  the connection down, which has it closed as if the node had
  hung up. The manager gives up in half the time, so that when
  it relays to a node that has stopped reading, it gives up on
  that node before the worker whose message it is relaying
  gives up on the manager. A heartbeat, or the answer to one,
  never waits for another sender: when the connection is busy
  it goes on the connection's send queue, ahead of the data, so
  steady traffic to a node doesn't keep its heartbeats out. The
  queue holds one heartbeat and one answer at most.
- The protocol carries a magic number and a version, checked at
  registration and handshake.
- The mesh runs over IPv4 or IPv6. Every node listens on all
  addresses of both families (on IPv4 alone if the host has no
  IPv6), and a host name is tried at each address it resolves
  to. An IPv6 address in a config goes in brackets:
  ``tcp:worker:[fe80::1%eth0]:5000``.

Which Server Owns a GID?
^^^^^^^^^^^^^^^^^^^^^^^^

The guest kernel picks its own RoCE GIDs (normally the IPv6
link-local address derived from the MAC, or an IPv4 address on
the interface), and the emulator can't infer the node from a
GID. Instead, each server is **told** its guest's GIDs in
``ERNIC_TCP_GUEST_GIDS`` (the launcher derives them). Each node
advertises its GIDs, the manager fans the advertisements out,
and every node keeps an exact ``GID → node id`` table.

The manager sends these updates, and the topology, from a
thread of its own. A change to one node's GIDs, or to whether
it is alive, sends that node's set to every worker; only a
worker that has just registered is sent the whole table.
Changes made while the thread is sending go out together in
its next round.

The admin queue consults that table when a QP reaches RTR and
when an AH is created. The two differ when no node owns the
destination GID:

- For a connected (RC or UC) QP, MODIFY_QP fails. Picking a
  default node instead would deliver writes into the wrong VM's
  memory.
- CREATE_AH succeeds, and the AH is treated as local (see
  `Datagrams (UD and GSI)`_). Creating an AH does not reject an
  unknown destination.

The ionic Wire Protocol
^^^^^^^^^^^^^^^^^^^^^^^

The mesh transports messages of type ``TCP_MSG_IONIC`` without
looking inside them. Their contents belong to
``ionic_datapath.c``: a fixed header (``struct
ionic_wire_hdr``: operation, source and destination QP, request
id, rkey, remote address, length, immediate, status, atomic
operands) followed by a payload of up to 16 MiB.

.. list-table::
   :header-rows: 1
   :widths: 25 20 55

   * - Request
     - Reply
     - Responder does
   * - SEND, SEND_IMM, SEND_INV
     - ACK
     - Deliver into the destination QP's next posted receive
   * - WRITE, WRITE_IMM
     - ACK
     - Copy the payload to ``(rkey, va)`` in its guest; for
       _IMM, also consume a receive
   * - READ_REQ
     - READ_RESP
     - Read ``(rkey, va)`` from its guest and send it back
   * - ATOMIC_REQ
     - ATOMIC_RESP
     - Perform the atomic on its guest's memory and return the
       old value

Note that **rkeys are resolved by the responder**, against its
own MR table. An rkey only means something to the server that
issued it, just as with real hardware.

A Remote Request, Step by Step
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the data path executes a WQE on a connected (RC or UC) QP
whose peer is on another node:

#. It gathers the payload (for SEND and WRITE) out of guest
   memory, builds the wire header, and queues the message for
   the peer's server. The main thread never waits for a peer:
   each connection has a send thread that writes what is
   queued. A connection takes up to 32 MiB of requests; a WQE
   that doesn't fit stays on the send queue, which stops there,
   and is tried again on a later pass, as a NIC out of transmit
   credits holds its queue. Replies have twice the room, and a
   WQE also waits if its reply might not fit the peer's (see
   `When a Node Falls Behind`_). When a
   QP is flushed, or a request times out (see **Lost peer**,
   below), the requests still queued are taken back, so none
   reaches the peer after its work request has completed; one
   already being written still arrives. A destroyed QP's
   requests still go out: nothing has completed them, and a
   consumer may destroy a QP straight after an unsignaled WRITE,
   counting on it to arrive.
#. It records the request in a **pending table** and does not
   post a completion yet. The completion must wait for the
   peer, both so the reported latency includes the network and
   because READ and atomic results haven't arrived. When the QP
   is destroyed, or another is created with its id, its entries
   are dropped without completions, so a reply that comes back
   later is ignored: a READ's or atomic's result doesn't land in
   memory the consumer has had back, and a QP that took the id
   isn't sent the old one's completions. The mesh names each
   request by the QP it came from, numbered afresh at every
   creation, so a later QP with the same id takes back only its
   own requests.
#. On the peer, a receive thread gets the message. It may not
   touch guest memory, so it only queues the message, in the
   buffer it was received into, if the queue has room (see
   `When a Node Falls Behind`_). The peer's main loop applies it
   to its guest and sends the reply.
#. Back on the requester, the reply is likewise queued and
   applied by the main loop. For READ and atomics, the returned
   data is scattered into the local SGEs. Then the send
   completion is posted.

Datagrams are different. A UD or GSI send to another node is
sent the same way, with its GRH already attached, but its send
completion is posted at once, as for a local datagram, and the
peer's ACK only clears the pending entry. A UD send completion
therefore doesn't mean the peer received the message.

Reliability and ordering come from TCP, so the emulator has no
packet sequence numbers, retransmission, or ACK/NAK protocol of
its own. Two parts of RoCE's error handling are imitated:

- **Receiver not ready.** If a SEND arrives before the guest has
  posted a receive, the message stays queued and is retried on
  later passes for up to 5 seconds, then fails with an
  RNR-retry-exceeded status. Later messages for the same QP wait
  behind it, so SENDs aren't reordered. If the SENDs waiting,
  and what waits behind them, fill the queue and keep another
  message out, they get half a second, not 5 (see
  `When a Node Falls Behind`_).
- **Lost peer.** A request whose reply never arrives completes
  with a retry-exceeded error after 30 seconds, so a peer that
  dies after accepting a request doesn't wedge the guest's send
  queue. A request that can't be sent at all, because there is
  no connection to the peer, completes with the same error at
  once. A peer that is still connected but has stopped reading
  holds up only its own connection's send thread, until the
  send timeout shuts the connection down; the requests still
  queued for it are lost, and complete after the 30 seconds.
  The WQEs waiting for room in the queue then go to the
  manager to relay, on a worker, or fail at once, if there is
  no route left.

When a Node Falls Behind
^^^^^^^^^^^^^^^^^^^^^^^^

A node's receive threads can read faster than its main loop
applies what they read, especially with several peers sending
to it at once, and a peer that doesn't run this code needn't
wait for answers at all. So the queue they fill is bounded, at
64 MiB or 4096 messages, whichever comes first (an empty queue
takes any one message). A receive thread with a message the
queue has no room for keeps it, and reads nothing more from that
peer until the main loop has made room. TCP passes that back:
the connection's socket buffers fill, then the sending node's
send queue for the connection, and then that node's guest's send
queues stop, as when a NIC runs out of transmit credits. While
the main loop runs, nothing is dropped, and a node's memory for
its peers' messages is bounded by the queue, plus the one
message each receive thread holds.

The node being held back has its replies to queue too. Its send
queue for a connection takes up to 64 MiB, its own requests
included, and drops a reply it has no room for, since it can't
wait for the requester to read on. A READ's reply is as large as
the READ, which the request doesn't show, so a node asks a peer
for no more than 32 MiB of replies at once: the room the peer's
queue is sure to have beside the most it queues of its own
requests, 32 MiB. A WQE that would ask for more waits on its
send queue, as one with no room to be sent does, until earlier
replies come back. A peer can still drop a reply to a request
the requester has given up on, after a flush or a timeout, or
one it sends by way of the manager: a worker with no connection
to the requester sends its replies on its one connection to the
manager, which every node it reaches that way shares.

This can't deadlock. Neither node's main loop ever waits for the
other (it only queues what it sends), and every pass empties the
queue, so a receive thread waits only for the main loop to apply
what is ahead of it, unless:

- **SENDs are waiting for a receive.** They stay queued, and the
  messages behind them for the same QP with them (see **Receiver
  not ready**, above). If they leave no room for a message a
  receive thread is waiting to queue for half a second, the main
  loop stops waiting for the guest's receives: those still
  without one fail with RNR-retry-exceeded at once, as they would
  have after 5 seconds, and the queue empties. Otherwise a guest
  that falls behind on its receives would keep the connection
  unread past its peer's send timeout, costing it the
  connection, and the heartbeats and other QPs' traffic on it.
- **The main loop isn't running**, as between one QEMU
  disconnecting and the next connecting. Once 2 seconds have
  passed since its last pass ended, with no other started, a full
  queue drops what comes instead of holding up the receive
  threads, and the next pass logs how many it dropped. The
  requests among them complete in error after their 30-second
  timeout. A pass that takes longer is still applying what is
  queued, so the receive threads go on waiting for it.

The Ethernet Side
-----------------

The Ethernet half of the device exists because the ionic driver
requires it, and because RDMA consumers need IP. ``rdma_cm``
resolves IP addresses to GIDs over the netdev, and many tools
exchange connection details over TCP before starting RDMA.
**RDMA traffic itself never goes over the emulated Ethernet**;
it goes through the data path described above.

- Transmit: on a TX doorbell, the emulator gathers each frame
  (head descriptor plus scatter/gather elements) out of guest
  memory and writes it to the host **TAP** interface named by
  ``--tap`` (``src/ionic/ionic_eth_net.c``). Without a TAP,
  frames are completed and discarded.
- Receive: the main loop reads frames from the TAP and DMAs each
  into the next buffer the guest posted on one of its enabled
  receive queues, then raises that queue's interrupt. A fixed
  hash of the flow picks the queue, so one flow always lands on
  one queue (``src/ionic/ionic_eth_steer.c``). A TAP frame
  whose queue has no buffer posted is dropped, as on a real
  NIC.
- No checksum, TSO, or scatter-gather offloads are advertised,
  so the guest always sends simple linear frames.
- Before a transmitted frame goes to the TAP, an in-process
  endpoint can claim it, and one can likewise claim a frame
  received from the TAP (the S3 target and the P4 engine use
  these; see below and :doc:`p4`). An endpoint sends frames to
  the guest by queueing them for the main loop, which delivers
  them after the TAP's. Unlike a TAP frame, a queued frame with
  no buffer to go to waits for one, and so do the frames behind
  it.
- The emulator counts every frame and keeps the LIF and port
  statistics blocks the driver reads (``ip -s link``,
  ``ethtool -S``) up to date in guest memory
  (``src/ionic/ionic_emu_stats.c``; see :doc:`ionic`).

To give several guests a shared segment, each server gets its
own TAP and the TAPs are joined in a host bridge (see
:doc:`ionic`). The mesh does not carry Ethernet.

In-Process Targets: NVMe-oF and S3
----------------------------------

Two backends put the *other end* of the RDMA conversation
inside the server, so one VM and one server make a complete
fabric. Both are written against a pair of DMA callbacks
(``from_host``, ``to_host``) that take an rkey and an address,
and neither touches guest memory or the network directly. In
the server, the data path supplies callbacks that resolve the
rkey in the guest's MR table, which is the emulator's
equivalent of the target posting RDMA READ and WRITE against
the initiator's keys. They move nothing for key 0, which would
otherwise reach all of guest memory (see `Trust Boundaries`_).
The same code runs in unit tests with a byte array standing in
for guest memory.

NVMe over Fabrics
^^^^^^^^^^^^^^^^^

``--backend nvmeof`` runs an NVMe-oF controller in the server.

- **Connection setup.** ``nvme connect -t rdma`` uses
  ``rdma_cm``, which sends IB CM MADs on the guest's GSI QP.
  When the GSI QP sends, the data path offers the MAD to
  ``src/nvmeof/nvmeof_cm.c``, which answers REQ with REP,
  accepts RTU, handles DREQ, and binds the guest's QP to a
  controller queue. The controller's side of each connection
  gets a QP number in a reserved range (``0xc0xxxx``) that
  can't collide with the guest's QPs. Replies go back into the
  guest's GSI receive queue with a GRH, as from a real peer.
- **I/O.** A SEND on a bound QP is an NVMe command capsule. The
  data path hands it to ``src/nvmeof/nvmeof_target.c``, which
  executes it synchronously against a RAM- or file-backed
  namespace. It moves data through the keyed SGL in the
  command (the guest's rkey, usually from a fast registration
  just before), and the response capsule is delivered into the
  guest's next posted receive.

The guest needs a static neighbor entry for the target address,
because nothing answers ARP for it. See :doc:`nvmeof`.

S3 over RDMA
^^^^^^^^^^^^

``--backend s3`` runs an object store whose control plane is
HTTP and whose data plane is RDMA, compatible with the protocol
hipObject and NVIDIA cuObject speak.

- **Control plane on the emulated wire.** ``src/s3/s3_tcp.c``
  is a minimal ARP, ICMP echo, and TCP implementation
  (handshake, in-order data, cumulative ACKs, one retransmit
  timer, FIN; no congestion control or reassembly, since the
  link is lossless). It claims the guest's transmitted frames
  addressed to the store's IP and injects its replies into the
  guest's receive ring. To the guest, the store is just a host
  on its segment; no TAP is needed. ``src/s3/s3_http.c``
  parses the HTTP requests.
- **Data plane by rkey.** A request carries an
  ``x-amz-rdma-token`` header (``src/s3/s3_token.c``) naming a
  registered guest buffer: rkey, address, and length.
  ``src/s3/s3_target.c`` moves the object directly into or out
  of that buffer through the DMA callbacks, and answers with
  ``x-amz-rdma-reply``. Requests without a token carry the
  payload in the HTTP body.

See :doc:`s3`.

The P4 Engine
-------------

The silicon ionic models is P4-programmable, and
``src/p4/`` is the emulator's equivalent: an engine that runs a
compiled P4 program (bmv2 JSON) loaded with ``--p4-program``.
It is built only with the ``ERNIC_BUILD_P4`` CMake option, and
runs only when ``--p4``, ``--p4-program`` or
``--p4-guest-scope`` (even ``none``) is given. It sits on two
paths:

- **Ethernet.** It takes the transmit and receive frame
  filters described in `The Ethernet Side`_, so every frame
  between the guest and the TAP goes through the program, which
  decides where it goes and what it looks like. A frame the S3
  store claims never reaches it.
- **RDMA.** For a program written for it, the data path runs
  the pipeline on every send WQE, after the WQE's own memory
  has been checked. The program can drop the operation, or
  turn an RDMA WRITE into fp8 arithmetic or a format conversion
  through an extern. Every region involved is local and named
  by the WQE, never by the program; an rkey belonging to
  another node is refused.

A guest can reach the engine through device commands 70 to 72,
but only as far as ``--p4-guest-scope`` allows; by default it
can't. Everything runs on the main thread. See :doc:`p4`.

Backends
--------

``--backend`` picks one entry in the backend registry
(``src/rdma/rdma_backend_core.c``). With the ionic front end
doing all data movement, the choice mostly decides whether the
mesh exists and whether a target is attached.

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Backend
     - What it means for the guest
   * - ``loopback`` (default)
     - Single VM. Every peer QP is in the same guest. Resources
       are tracked in memory.
   * - ``tcp``
     - Multi-VM. The mesh described in `More Than One VM: the
       TCP Mesh`_.
   * - ``nvmeof``
     - Single VM with an NVMe-oF controller in the server.
       Resource handling is the loopback backend's.
   * - ``s3``
     - Single VM with an S3 store in the server. Resource
       handling is the loopback backend's.
   * - ``none``
     - Stub resource operations; meant for PCI enumeration and
       driver bring-up.

Threads
-------

.. list-table::
   :header-rows: 1
   :widths: 25 20 55

   * - Thread
     - Exists when
     - Does
   * - main
     - always
     - vfio-user, every register access, every guest-memory
       access, every interrupt
   * - accept
     - ``tcp``
     - Accepts incoming mesh connections
   * - receive (one per connection)
     - ``tcp``
     - Reads mesh messages and queues them for the main thread
   * - send (one per connection)
     - ``tcp``, once the data path sends on it, or a heartbeat
       finds it busy
     - Writes the messages the main thread queues for the
       connection's peer, so the main thread never waits for one,
       and heartbeats that couldn't be written at once
   * - health check
     - ``tcp`` manager
     - Heartbeats, dead-node detection, closing dead nodes'
       connections, and those of nodes that never registered
   * - broadcast
     - ``tcp`` manager
     - Sends the topology and GID ownership to the workers (see
       `Which Server Owns a GID?`_)
   * - peer connect
     - ``tcp`` worker
     - Connects to peers as the topology changes, and closes dead
       connections; connects to the manager again when its
       connection is lost

The rule is that **only the main thread touches guest memory**.
The mesh's receive threads hand what they read to it through a
locked queue, the data path's inbound message queue. The
in-process targets and the P4 engine run on the main thread
itself, so they start no threads. The completion thread the
QEMU code had upstream is gone.

Shared state between threads uses mutexes or C11 atomics. The
thread-safety section of ``AGENTS.md``, in the repository root,
has the rules.

Trust Boundaries
----------------

The server is the boundary between a guest and the host, so
everything that comes from a guest (register writes, doorbells,
admin commands, ring contents, keys, addresses, lengths) or from
another server is untrusted. The main defenses in the device
model:

- A memory access through a non-zero key is bounds-checked
  against the MR it names. Key 0 is the driver's reserved DMA
  key: its address is used as a guest physical address with no
  MR lookup, and the device doesn't check which QP, or which
  guest process, used it. The only limit on a key-0 access is
  vfio-user's: it can reach only guest RAM that QEMU shared with
  the server.
- A peer server may name memory only by rkey. Key 0, which
  locally means "the address is already a bus address", is
  refused when a peer sends it with a non-zero length. So is a
  key 0 in a request to the NVMe-oF controller or the S3 store,
  or as an operand of the P4 engine's externs, since any guest
  process can make those requests.
- Guest addresses and lengths are checked for wrap-around before
  they are used: a memory region or a keyed range that runs past
  the end of the address space is refused, and a send whose SGE
  lengths add up to more than 4 GiB fails instead of being
  truncated.
- A page table is checked against the length of the region it
  describes, so it can't have more entries than that region
  needs. This is a geometry check, not a memory limit: the
  region's length also comes from the guest and nothing caps it,
  so a large enough CREATE_MR or fast registration still makes
  the server allocate a large page table.
- Every ring's depth and stride is bounded before it is used in
  a shift. A command creating a queue (an RDMA admin queue,
  EQ, CQ, QP, or SRQ, or a Q_INIT of an Ethernet queue) that
  exceeds the bounds is refused.
- Datagram delivery within one VM checks that the destination
  is a UD or GSI QP, so a datagram can't land in an RC or UC
  QP, and that the sender's Q_Key matches the destination's. A
  Q_Key in the WQE with the high-order bit set is a *controlled*
  key: as the IB spec requires, the device sends the sending
  QP's own Q_Key instead, and refuses the datagram if that QP
  has none. Only a privileged consumer can give its QP a
  controlled key (the guest kernel refuses one from user
  space), so a process can't reach QP1 by writing its
  well-known key, ``0x80010000``, into a WQE. Datagrams between
  servers aren't checked at all: the wire header carries
  neither the transport type nor the Q_Key, and an inbound SEND
  is delivered to whichever QP it names.
- A connected QP whose destination resolves to no known node
  fails MODIFY_QP instead of going to a default node. An AH with
  such a destination is still created, and treated as local.

What Is Not Emulated
--------------------

- **No RoCE packets.** Nothing builds or parses RoCE frames, and
  there is no packet sequence number, ACK/NAK, retransmission,
  or congestion control. The header templates the driver
  supplies are read only for their addresses.
- **No asynchronous events.** The only path to the driver is a
  CQ completion through an EQ, so events such as "SRQ limit
  reached" are never raised. The limit can be set, but nobody is
  told when it is reached.
- **QP state isn't enforced.** The data path doesn't consult it:
  a QP executes the WQEs posted to it in any state, including
  ones posted after it has moved to ERROR. Moving to ERROR is the
  one transition it acts on: the work requests outstanding then
  are flushed, with one flush-error completion for each queue (a
  QP on a shared receive queue flushes only its send queue), and
  the requests it still has queued for other nodes are taken
  back (see `A Remote Request, Step by Step`_).
- **One outstanding RDMA READ or atomic per QP** is what
  QUERY_QP reports.
- **Ethernet offloads** (checksum, TSO, scatter-gather) are not
  offered, and the guest's RSS key and indirection table are
  accepted but ignored.
- **Counters with no emulator event behind them**, such as
  packet sequence errors, duplicates, CNPs, and ECN marks, read
  zero (see :doc:`ionic`).
- **Performance** is that of a software device: every doorbell
  is a socket message, every byte is copied by the server's CPU,
  and the main loop polls.

Where to Look in the Source
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Role
   * - ``src/rocm_ernic_server.c``
     - Startup, PCI identity, BARs, MSI-X, main loop
   * - ``src/ernic_socket.c``
     - The vfio-user socket's permissions
   * - ``src/rocm_ernic_internal.h``
     - The top-level device struct
   * - ``src/ionic/ionic_bar.h``
     - BAR layout constants
   * - ``src/ionic/ionic_device.c``
     - The ionic device as one object: builds, wires, polls and
       tears down the files below
   * - ``src/ionic/ionic_eth_emu.c``
     - Registers, device commands, LIF, Ethernet queues,
       interrupt controller, doorbell decode
   * - ``src/ionic/ionic_eth_net.c``
     - Host TAP interface
   * - ``src/ionic/ionic_eth_steer.c``
     - Choosing the receive queue for a frame
   * - ``src/ionic/ionic_emu_stats.c``
     - Ethernet LIF and port counters, and the statistics blocks
       written into guest memory
   * - ``src/ionic/ionic_profile.c``,
       ``src/ionic/ionic_profiles.c``
     - NIC profiles: loading and layering them, and the built-in
       ones
   * - ``src/ionic/ionic_fault.c``
     - Fault injection (``--inject``)
   * - ``src/ionic/ionic_rdma_devcmd.c``
     - RDMA bootstrap (EQs, admin queues), EQE posting
   * - ``src/ionic/ionic_adminq.c``
     - RDMA control path
   * - ``src/ionic/ionic_datapath.c``
     - RDMA data path and inter-server wire protocol
   * - ``src/ionic/ionic_datapath_nvmeof.c``,
       ``src/ionic/ionic_datapath_s3.c``,
       ``src/ionic/ionic_datapath_p4.c``
     - The data path's side of the NVMe-oF and S3 targets and
       the P4 pipeline
   * - ``src/ionic/ionic_eth_p4.c``
     - The P4 pipeline on the Ethernet frame filters
   * - ``src/ernic_dma.c``
     - Copying to and from guest memory, for all of the above
   * - ``src/ernic_stats.c``, ``.h``
     - The device and per-QP counters, and the stats file
   * - ``src/ernic_siphash.c``, ``.h``
     - SipHash-2-4, for values a guest must not predict
   * - ``src/rocm_ernic_compat.c``, ``.h``,
       ``src/ernic_rm.h``, ``src/ernic_mesh.h``
     - Bridge to the QEMU-derived core: the device's lifecycle,
       and wrappers for the resource manager and the mesh
   * - ``src/pvrdma/``
     - Resource manager and backend dispatch, forked from QEMU
   * - ``src/rdma/rdma_backend_*.c``
     - Backend registry; none, loopback, and TCP mesh backends
   * - ``src/rdma/tcp_*.c``
     - The TCP mesh: connections, the wire format, GID
       ownership, and the manager's and workers' halves
   * - ``src/qemu-compat/``
     - Stand-ins for the QEMU APIs the QEMU-derived code calls
   * - ``src/nvmeof/nvmeof_cm.c``,
       ``src/nvmeof/nvmeof_target.c``
     - In-process NVMe-oF CM responder and controller
   * - ``src/s3/s3_tcp.c``, ``src/s3/s3_http.c``,
       ``src/s3/s3_token.c``, ``src/s3/s3_target.c``
     - In-process S3 store
   * - ``src/p4/``
     - P4 engine: program loader, tables, the pipeline, its
       device commands, and the fp8 and format conversion
       externs
