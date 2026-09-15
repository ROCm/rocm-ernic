Performance
===========

This page describes how rocm-ernic performance is measured and
what the current numbers mean. The numbers themselves are
published by CI rather than written here: see
:doc:`perf-trends`, which the nightly full-tier run regenerates
from ``docs/perf-history/history.jsonl``.

.. note::

   The milestone-by-milestone results that used to live on this
   page were measured against the removed PVRDMA-derived device
   and its out-of-tree guest driver. They no longer describe
   anything this repository builds, so they were dropped when
   that device was removed; they remain in the git history.

.. contents:: Sections
   :local:
   :depth: 2

Test Environment
----------------

The reference environment is the self-hosted CI node,
**hpe-rack-15.adc.amd.com** (AMD EPYC 7513, 128 threads), with
the TCP mesh backend: a manager and a worker server instance on
one host, each attached to a guest VM over localhost loopback.

==============================  ==========================================
Component                       Value
==============================  ==========================================
Emulated device                 ``1dd8:100a`` (ionic)
Guest driver                    upstream ``ionic`` + ``ionic_rdma``
                                (DKMS, ``IONIC_KERNEL_REF``)
rdma-core                       v62.0, upstream ``providers/ionic``
MTU                             4096 bytes
GID index                       1 (IPv4-mapped, 192.168.200.x/24)
Connection type                 RC (Reliable Connected)
==============================  ==========================================

Guest-to-guest IP runs over one host TAP per instance on a
shared bridge, not over the RDMA mesh; see :doc:`ionic`.

Running the Benchmarks
----------------------

The Ansible playbooks in ``ansible/playbooks/`` drive the same
benchmarks CI runs:

.. code-block:: bash

   # RDMA: ib_send_bw / ib_write_bw / ib_read_bw and the
   # latency variants, plus ibv_rc_pingpong
   ansible-playbook playbooks/performance-tests.yml

   # Ethernet: iperf3 between the two guests over the TAP bridge
   ansible-playbook playbooks/tcp-performance-tests.yml

``ci/jobs/perf.sh`` wraps the first of these for the nightly
run and hands the results to ``ci/report/publish-perf.py``,
which appends one record per run to
``docs/perf-history/history.jsonl`` and regenerates the charts,
the trend tables, and the shields.io badges.

Interpreting the Results
------------------------

Bandwidth and latency are reported per message size and never
plotted on a shared axis: 4 KiB and 1 MiB bandwidth differ by
more than an order of magnitude, so one scale would hide the
small sizes entirely.

Latency is dominated by the emulation path, not by the wire.
Every doorbell is a vfio-user round trip into the server
process, so the floor is set by scheduling and by the poll
intervals in the server and in QEMU rather than by the message
size, and the small-message latency numbers should be read as a
property of the emulator.

Known Limitations
-----------------

- **Small-message bandwidth runs:** at 4 KB and 8 KB the
  perftest server can exit before the client connects. Sizes of
  16 KB and above are unaffected.

- **iperf3 rate cap:** the Ansible stress default
  ``ernic_iperf_bandwidth`` is passed to ``iperf3 -b`` verbatim
  and caps the reported TCP row well below what the link does
  unthrottled. Very low values on TCP (for example ``10K``) are
  not useful at all: intervals round to zero and the connection
  can stall. Use a Mbit/s-scale cap or ``-u`` for a clean one.

- **No Ethernet offloads:** the emulated LIF advertises no
  checksum, TSO, or scatter-gather offload, so the guest stack
  always hands down linear skbs. This costs transmit
  throughput.

- **Host sysctl required:** large-message bidirectional traffic
  over the TCP mesh needs ``net.core.wmem_max`` and
  ``net.core.rmem_max`` raised to at least 16 MB on the host.

- **GPU CQ/SQ in system memory:** GPU VRAM buffers would need a
  vfio-user DMA proxy for GPU BAR regions, which does not
  exist. System memory buffers registered with
  ``hipHostRegister`` work.

- **~1 ms GPU latency floor:** GPU-initiated work goes through
  the QEMU pci-mmio-bridge, whose default poll interval is
  1 ms. Lowering ``poll-interval-ns`` trades CPU for latency.
