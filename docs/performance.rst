Performance
===========

This page describes how rocm-ernic performance is measured and
what the current numbers mean. The tracked numbers are published
by CI rather than written here: see :doc:`perf-trends`, which the
nightly full-tier run regenerates from
``docs/perf-history/history.jsonl``. `Reference Measurements`_
below records one full sweep in prose, because the trend page
tracks three message sizes and says nothing about the shape of
the curve between them.

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
MTU                             1024 bytes active (4096 max)
GID index                       1 (IPv4-mapped, 192.168.200.x/24)
Connection type                 RC (Reliable Connected)
==============================  ==========================================

The active MTU follows the Ethernet MTU of the emulated LIF,
which nothing raises from the 1500-byte default, so it settles
at the largest IB value that fits. The device advertises 4096.

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

A third series comes from the NVMe-oF lane rather than from
perftest: :file:`.github/workflows/nvmeof-nightly.yml` sweeps
fio across the same three block sizes against an in-process
NVMe-oF controller and publishes it as the ``nvmeof`` series.
It is kept separate because it is measured on a GitHub-hosted
runner rather than the self-hosted node, so it is comparable
with itself over time but not with the numbers on this page.
See the *Performance* section of :doc:`nvmeof`.

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

Reference Measurements
----------------------

One full ``ci/jobs/perf.sh`` run on the reference node above,
against ``rocm-ernic`` at 59d5b73 and a resolute guest running
7.2.3-070203-generic with the ionic sources pinned at v7.2.4.
The guests were freshly launched from the published image
immediately before the sweep. These are point measurements from
a single run; see `Repeatability`_ for the spread.

Bandwidth, GB/s (``ib_send_bw``, ``ib_write_bw``, ``ib_read_bw``,
``-n 1000``, averaged column):

============  ========  =========  ========
Message size  send      write      read
============  ========  =========  ========
4 KiB             0.19       0.28      0.32
8 KiB             0.31       0.54      0.59
16 KiB            0.56       0.96      1.12
32 KiB            1.08       1.71      2.01
64 KiB            1.61       2.50      2.74
128 KiB           2.30       3.49      4.14
256 KiB           3.41       4.63      4.57
512 KiB           5.02       5.34      4.65
1 MiB             6.07       5.13      4.83
2 MiB             6.08       5.40      4.74
4 MiB             5.60       5.56      4.52
8 MiB             3.41       3.24      2.81
============  ========  =========  ========

Throughput climbs to roughly 6 GB/s at 1-4 MiB and falls back at
8 MiB, where a single message no longer fits comfortably in the
server's staging path and the transfer is split.

Latency, microseconds (``ib_send_lat``, ``ib_write_lat``,
``ib_read_lat``, ``-n 50``, typical column):

============  ========  =========  ========
Message size  send      write      read
============  ========  =========  ========
4 KiB           245.65     243.75    325.96
8 KiB           243.65     242.76    326.40
16 KiB          246.96     245.23    479.19
32 KiB          253.33     254.43    327.44
64 KiB          257.33     330.99    338.30
128 KiB         338.92     265.25    491.46
256 KiB         357.91     354.33    526.78
512 KiB         467.02     484.83    528.99
1 MiB           673.38     688.01    732.60
2 MiB          1053.88    1087.62   1136.33
4 MiB          2022.79    1986.61   2094.55
8 MiB          4455.15    5450.70   5099.79
============  ========  =========  ========

Below about 128 KiB the curve is flat at roughly 245 us for send
and write and 330 us for read: that is the emulation round trip,
not the message. Above it the numbers scale with size, which is
where the transfer itself starts to dominate. Read costs one
extra round trip at every size.

TCP/IP over the emulated Ethernet LIF, for comparison:
``iperf3`` sustains 0.34 GB/s (2.7 Gbit/s) guest to guest,
roughly an order of magnitude below RDMA at the same sizes. The
LIF advertises no offloads, so the guest stack hands down linear
skbs and every byte is copied.

Repeatability
~~~~~~~~~~~~~

A five-run reliability sweep at 64 KiB, averaged column:

==========  =============  ==============
Verb        Range, GB/s    Mean, GB/s
==========  =============  ==============
send        0.73 - 1.00              0.82
write       0.62 - 0.85              0.73
read        0.78 - 0.90              0.85
==========  =============  ==============

The reliability sweep runs with a much shorter iteration count
than the bandwidth sweep, which is why its absolute numbers sit
below the 64 KiB row above; it is measuring run-to-run variation,
not throughput. Treat differences below about 20% between runs as
noise. ``ernic_perf_reliability_runs`` defaults to 1 in CI and
was set to 5 for this measurement.

Known Limitations
-----------------

- **Locked memory:** every verbs resource is pinned, and the
  distro default ``RLIMIT_MEMLOCK`` of 8 MiB is too small for a
  4 MiB perftest run, which registers twice the message size.
  The failure surfaces on whatever allocation comes next rather
  than the one that crossed the line, so it reads as
  ``Couldn't create CQ``. ``ernic_guest_setup`` installs the
  ``limits.d`` drop-in and the systemd ``DefaultLimitMEMLOCK``
  that the distro ``rdma-core`` package would have provided; a
  guest provisioned by hand needs both.

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
