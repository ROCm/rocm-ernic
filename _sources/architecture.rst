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
     - 32 MSI-X vectors, plus INTx

The upstream ``ionic.ko`` binds to that ID. The device has two
BARs:

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
              │ ionic_rm_*, ionic_mesh_*  │ attach
              ▼                           ▼
       rocm_ernic_compat.c          nvmeof_*.c, s3_*.c
       (bridge; only file that      in-process targets
        sees QEMU types)
              │
              ▼
       third-party/qemu/hw/rdma   resource manager (rdma_rm)
       src/rdma/                  backend vtable: none,
                                  loopback, tcp mesh
       src/qemu-compat/           the QEMU APIs the vendored
                                  code expects

The ionic Front End
^^^^^^^^^^^^^^^^^^^

The ``src/ionic_*.c`` files are the device the guest actually
talks to. They own the register file, decode doorbells, and
read and write every ring and buffer in guest memory. Each file
is described in a later section.

The Device Core
^^^^^^^^^^^^^^^

The device core is ``src/rocm_ernic_compat.c``,
``third-party/qemu/``, and ``src/rdma/``.

rocm-ernic began as an emulator of the VMware paravirtual RDMA
device (PVRDMA), using the device model from QEMU's
``hw/rdma/``. That guest-facing personality has been removed
and replaced by the ionic front end, but two pieces of the QEMU
code are still underneath it, which is why many names
(``pvrdma_handle``, ``PVRDMADev``, the ``pvrdma_*`` statistics)
still say "pvrdma":

- **rdma_rm**, the resource manager: handle tables for PDs,
  CQs, QPs, MRs, and SRQs, and each QP's current state (RESET,
  INIT, RTR, RTS, ...).
- **rdma_backend**, which forwards resource operations to a
  pluggable **backend** through the vtable in
  ``src/rdma/rdma_backend_ops.h``.

The ionic admin queue reaches these through the ``ionic_rm_*()``
wrappers in ``rocm_ernic_compat.c``, which is the only file that
includes the QEMU headers. Today the core's jobs in the ionic
world are:

#. Bookkeeping and limits for each object the guest creates.
#. Recording each QP's state and passing the INIT, RTR, and RTS
   transitions to the backend. The core doesn't check that a
   transition is legal: it records whatever state the guest
   asks for. The one check that matters is the TCP backend's at
   RTR, which fails the transition when no node owns a connected
   QP's destination GID (see `More Than One VM: the TCP
   Mesh`_).
#. Carrying the TCP mesh, through the ``ionic_mesh_*()``
   wrappers.
#. Statistics: counters, and the stats file written about once
   a second (``--stats-file``, consumed by ``ernic-exporter``;
   see :doc:`monitoring`).

The core does **not** move data for the ionic device. The
backends' ``post_send`` and ``post_recv`` entry points, and the
vendored completion code in ``pvrdma_qp_ops.c``, belonged to the
PVRDMA data path; nothing in the ionic path calls them. All data
movement happens in ``ionic_datapath.c``.

The Main Loop
^^^^^^^^^^^^^

``src/rocm_ernic_server.c`` builds the device, then waits for
QEMU to connect. Once QEMU has connected, it loops:

#. ``vfu_run_ctx()``: handle pending vfio-user messages. BAR
   accesses arrive here as callbacks, and **BAR 2 doorbells for
   send queues are executed inside the callback** (see
   `The Data Path`_).
#. Run any GLib idle callbacks and deliver deferred interrupts.
#. Move received Ethernet frames from the TAP, or from frames
   other threads queued, into the guest's receive ring.
#. Process new entries on the RDMA admin queues.
#. Apply messages that arrived from other servers over the mesh,
   and time out requests that never got an answer.
#. If none of that found work, sleep 100 µs.

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
     - Reports one port, one LIF, 32 interrupts, 32 EQs
   * - LIF_IDENTIFY
     - Reports Ethernet and RDMA capability, queue counts, the
       MAC, and the RDMA section: queue type numbers; QP, CQ,
       and SRQ counts; MR and AH limits; and the page table
       size, which sets the guest's maximum MR size (4 TiB)
   * - PORT_INIT
     - Writes a port-info block saying "100 Gb/s copper QSFP,
       link up", so ``ethtool`` reports a speed
   * - LIF_INIT
     - Writes "link up" into the LIF status block, so the netdev
       gets a carrier and the RDMA port goes active
   * - Q_INIT
     - Records where an Ethernet queue's descriptor and
       completion rings are in guest memory, and its interrupt
       vector
   * - Q_IDENTIFY
     - Reports descriptor sizes; advertises no offloads
   * - others
     - Succeed with a zeroed completion

The Ethernet Admin Queue
^^^^^^^^^^^^^^^^^^^^^^^^

Once its admin queue is set up, ``ionic.ko`` stops using BAR 0
and posts commands as 64-byte descriptors in a ring in guest
memory, rings a doorbell, and waits for an interrupt. The
emulator reads each descriptor by DMA, runs it, DMAs a 16-byte
completion with the expected *color* bit (see `Rings and the
Color Bit`_), and raises the queue's MSI-X vector.

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
     - Forget all EQs and admin queues
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
queue. ``src/ionic_adminq.c`` services those queues.

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
       and which server it lives on; for UD, record the Q_Key
   * - QUERY_QP
     - DMA the QP's state, PSNs, and access flags into the
       buffers the driver supplied
   * - CREATE_SRQ
     - Allocate an SRQ id (the device assigns it) and register
       its ring
   * - MODIFY_SRQ, QUERY_SRQ
     - Set or read the low-watermark limit
   * - DESTROY_*
     - Undo the above
   * - statistics, DCQCN, debug, QUERY_AH
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

``src/ionic_datapath.c`` is the heart of the emulator: it does
what a NIC's hardware data path would do.

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
object is registered. Runs of physically adjacent pages are
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
treating the WQE's own payload area as the source buffer.

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
must match. A datagram that crosses the mesh is not checked
(see `Trust Boundaries`_).

The received message starts with a 40-byte GRH carrying the
source and destination GIDs, as the verbs API requires, and the
CQE carries a source MAC address synthesized from the sending
QP.

More Than One VM: the TCP Mesh
------------------------------

Each server serves one VM. For QPs in different VMs to talk,
the ``tcp`` backend (``src/rdma/rdma_backend_tcp.c``) connects
the servers into a mesh, and the data path sends requests
across it.

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
  reconnects them.
- The protocol carries a magic number and a version, checked at
  registration and handshake.

Which Server Owns a GID?
^^^^^^^^^^^^^^^^^^^^^^^^

The guest kernel picks its own RoCE GIDs (normally the IPv6
link-local address derived from the MAC, or an IPv4 address on
the interface), and the emulator can't infer the node from a
GID. Instead, each server is **told** its guest's GIDs in
``ERNIC_TCP_GUEST_GIDS`` (the launcher derives them). Each node
advertises its GIDs, the manager fans the advertisements out,
and every node keeps an exact ``GID → node id`` table.

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
   memory, builds the wire header, and sends the message to the
   peer's server.
#. It records the request in a **pending table** and does not
   post a completion yet. The completion must wait for the
   peer, both so the reported latency includes the network and
   because READ and atomic results haven't arrived.
#. On the peer, a receive thread gets the message. It may not
   touch guest memory, so it only queues a copy. The peer's main
   loop applies it to its guest and sends the reply.
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
  behind it, so SENDs aren't reordered.
- **Lost peer.** A request whose reply never arrives completes
  with a retry-exceeded error after 30 seconds, so a peer that
  dies after accepting a request doesn't wedge the guest's send
  queue. This covers missing replies, not blocked sends.
  Requests and replies are sent synchronously from the main
  thread, and a send to a peer that is still connected but has
  stopped reading waits until the peer reads or the server
  shuts down. While it waits, the main thread does nothing
  else, timeouts included, so the whole device stalls.

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
  ``--tap`` (``src/ionic_eth_net.c``). Without a TAP, frames
  are completed and discarded.
- Receive: the main loop reads frames from the TAP and DMAs each
  into the next buffer the guest posted on receive queue 0, then
  raises the queue's interrupt.
- No checksum, TSO, or scatter-gather offloads are advertised,
  so the guest always sends simple linear frames.
- Before a transmitted frame goes to the TAP, an in-process
  endpoint can claim it (the S3 target uses this; see below).
  Threads that may not do DMA can queue frames for the main loop
  to deliver.

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
the initiator's keys. The same code runs in unit tests with a
byte array standing in for guest memory.

NVMe over Fabrics
^^^^^^^^^^^^^^^^^

``--backend nvmeof`` runs an NVMe-oF controller in the server.

- **Connection setup.** ``nvme connect -t rdma`` uses
  ``rdma_cm``, which sends IB CM MADs on the guest's GSI QP.
  When the GSI QP sends, the data path offers the MAD to
  ``src/nvmeof_cm.c``, which answers REQ with REP, accepts RTU,
  handles DREQ, and binds the guest's QP to a controller queue.
  The controller's side of each connection gets a QP number in
  a reserved range (``0xc0xxxx``) that can't collide with the
  guest's QPs. Replies go back into the guest's GSI receive
  queue with a GRH, as from a real peer.
- **I/O.** A SEND on a bound QP is an NVMe command capsule. The
  data path hands it to ``src/nvmeof_target.c``, which executes
  it synchronously against a RAM- or file-backed namespace. It
  moves data through the keyed SGL in the command (the guest's
  rkey, usually from a fast registration just before), and the
  response capsule is delivered into the guest's next posted
  receive.

The guest needs a static neighbor entry for the target address,
because nothing answers ARP for it. See :doc:`nvmeof`.

S3 over RDMA
^^^^^^^^^^^^

``--backend s3`` runs an object store whose control plane is
HTTP and whose data plane is RDMA, compatible with the protocol
hipObject and NVIDIA cuObject speak.

- **Control plane on the emulated wire.** ``src/s3_tcp.c`` is a
  minimal ARP, ICMP echo, and TCP implementation (handshake,
  in-order data, cumulative ACKs, one retransmit timer, FIN; no
  congestion control or reassembly, since the link is
  lossless). It claims the guest's transmitted frames addressed
  to the store's IP and injects its replies into the guest's
  receive ring. To the guest, the store is just a host on its
  segment; no TAP is needed. ``src/s3_http.c`` parses the HTTP
  requests.
- **Data plane by rkey.** A request carries an
  ``x-amz-rdma-token`` header (``src/s3_token.c``) naming a
  registered guest buffer: rkey, address, and length.
  ``src/s3_target.c`` moves the object directly into or out of
  that buffer through the DMA callbacks, and answers with
  ``x-amz-rdma-reply``. Requests without a token carry the
  payload in the HTTP body.

See :doc:`s3`.

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
   * - ``verbs``
     - Accepted on the command line and documented as
       forwarding to a real HCA, but **not implemented**: the
       registry entry is empty, so the server fails to start.

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
   * - health check
     - ``tcp`` manager
     - Heartbeats, dead-node detection, reconnects
   * - peer connect
     - ``tcp`` worker
     - Connects to peers as the topology changes, and closes dead
       connections

The rule is that **only the main thread touches guest memory**.
Every other thread hands work to it through a locked queue: the
data path's inbound message queue, or the Ethernet emulator's
frame inbox. The vendored QEMU code's own completion thread is
never started.

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
  refused when a peer sends it with a non-zero length.
- A page table is checked against the length of the region it
  describes, so it can't have more entries than that region
  needs. This is a geometry check, not a memory limit: the
  region's length also comes from the guest and nothing caps it,
  so a large enough CREATE_MR or fast registration still makes
  the server allocate a large page table. The ring
  geometry of SRQs and of the data path's CQ rings is bounded
  before it is used in shifts. Not every command checks its ring
  geometry yet, so this is not a guarantee for all queue
  creation.
- Datagram delivery within one VM checks that the destination
  is a UD or GSI QP, so a datagram can't land in an RC or UC
  QP, and that the Q_Key in the WQE matches the destination's.
  These checks catch a datagram sent to the wrong QP; they don't
  authenticate the sender. The Q_Key comes from the WQE, which
  the sending process writes, and the device doesn't restrict
  privileged Q_Keys. QP1's key is the well-known ``0x80010000``,
  so any process that writes it can deliver a MAD to QP1.
  Datagrams between servers aren't checked at all: the wire
  header carries neither the transport type nor the Q_Key, and
  an inbound SEND is delivered to whichever QP it names.
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
  a QP executes the WQEs posted to it in any state, and moving a
  QP to ERROR doesn't flush its outstanding work requests with
  flush-error completions.
- **One outstanding RDMA READ or atomic per QP** is what
  QUERY_QP reports.
- **Ethernet offloads** (checksum, TSO, scatter-gather) are not
  offered.
- **Statistics admin commands** succeed but return nothing.
- **The verbs backend** has no implementation (see
  `Backends`_).
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
   * - ``src/rocm_ernic_internal.h``
     - BAR layout constants and the top-level device struct
   * - ``src/ionic_eth_emu.c``
     - Registers, device commands, LIF, Ethernet queues,
       interrupt controller, doorbell decode
   * - ``src/ionic_eth_net.c``
     - Host TAP interface
   * - ``src/ionic_rdma_devcmd.c``
     - RDMA bootstrap (EQs, admin queues), EQE posting
   * - ``src/ionic_adminq.c``
     - RDMA control path
   * - ``src/ionic_datapath.c``
     - RDMA data path and inter-server wire protocol
   * - ``src/rocm_ernic_compat.c``, ``.h``
     - Bridge to the QEMU-derived core; guest-memory mapping for
       it; mesh wrappers; shared limits
   * - ``third-party/qemu/hw/rdma/``
     - Resource manager and backend dispatch, forked from QEMU
   * - ``src/rdma/rdma_backend_*.c``
     - Backend registry; none, loopback, and TCP mesh backends
   * - ``src/rdma/pvrdma_stats.c``
     - Statistics file
   * - ``src/qemu-compat/``
     - Stand-ins for the QEMU APIs the vendored code calls
   * - ``src/nvmeof_cm.c``, ``src/nvmeof_target.c``
     - In-process NVMe-oF CM responder and controller
   * - ``src/s3_tcp.c``, ``src/s3_http.c``, ``src/s3_token.c``,
       ``src/s3_target.c``
     - In-process S3 store
