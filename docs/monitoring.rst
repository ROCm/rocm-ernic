Prometheus Monitoring and Grafana Dashboard
============================================

rocm-ernic ships a Prometheus exporter
(``ernic-exporter``) that reads the same data sources
as ``ernicctl`` -- the ``instances.json`` manifest and
per-instance ``*.stats`` files under ``ERNIC_RUN_DIR``
-- and exposes them as Prometheus metrics on an HTTP
endpoint.  A pre-built Grafana dashboard is included
for visualising cluster state, network traffic, RDMA
vs TCP/IP ratios, FLR events, and VM lifecycle.

.. contents:: On this page
   :local:
   :depth: 2

Architecture
------------

.. code-block:: text

   ┌─────────────────── Host ───────────────────────┐
   │                                                 │
   │  rocm-ernic         *.stats (text, ~1 Hz)       │
   │  server instances ──────────────► ernic-exporter │
   │        │                           :9840/metrics │
   │        └──► instances.json ──────►     │         │
   │                                        │         │
   └────────────────────────────────────────┼─────────┘
                                            │
                                    Prometheus scrape
                                            │
                                       ┌────▼────┐
                                       │ Grafana │
                                       └─────────┘

The exporter runs as a ``systemd`` service alongside
``rocm-ernic``.  Prometheus scrapes ``/metrics`` at a
configurable interval (default 5 s); each scrape reads
the current stat files on-demand so no internal polling
loop is needed.

Quick Start
-----------

**Manual (single host):**

.. code-block:: bash

   # 1. Install the exporter's Python dependency
   python3 -m venv /opt/ernic-exporter-venv
   /opt/ernic-exporter-venv/bin/pip install \
       -r /usr/share/rocm-ernic/requirements-exporter.txt

   # 2. Start the exporter (uses system Python if
   #    prometheus_client is installed system-wide,
   #    otherwise point the unit at the venv -- see
   #    Ansible section below)
   sudo systemctl daemon-reload
   sudo systemctl enable --now ernic-exporter

   # 3. Verify
   curl -s http://localhost:9840/metrics | head -20

**Ansible (recommended):**

.. code-block:: bash

   cd ansible
   ansible-playbook playbooks/monitoring-setup.yml

The playbook creates the venv, installs the dependency,
configures a systemd override so the exporter runs
under the venv Python, writes a Prometheus
``file_sd`` scrape config, and copies the Grafana
dashboard JSON into the provisioning directory.

Configuration
-------------

All settings come from the same
``/etc/rocm-ernic/rocm-ernic.env`` file that the main
service uses, plus two exporter-specific variables:

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - Variable
     - Default
     - Description
   * - ``ERNIC_RUN_DIR``
     - ``/run/rocm-ernic``
     - Directory containing ``instances.json`` and
       ``*.stats`` files.
   * - ``ERNIC_EXPORTER_PORT``
     - ``9840``
     - HTTP port for the ``/metrics`` endpoint.

Override via environment or in the env file:

.. code-block:: bash

   echo "ERNIC_EXPORTER_PORT=9841" | \
       sudo tee -a /etc/rocm-ernic/rocm-ernic.env
   sudo systemctl restart ernic-exporter

Ansible variables (``group_vars/all.yml``):

.. list-table::
   :header-rows: 1
   :widths: 35 20 45

   * - Variable
     - Default
     - Description
   * - ``ernic_monitoring``
     - ``true``
     - Feature gate; set ``false`` to skip
       monitoring deployment entirely.
   * - ``ernic_exporter_port``
     - ``9840``
     - Maps to ``ERNIC_EXPORTER_PORT``.
   * - ``ernic_exporter_venv``
     - ``/opt/ernic-exporter-venv``
     - Path for the Python venv.
   * - ``ernic_exporter_scrape_interval``
     - ``5s``
     - Prometheus scrape interval.
   * - ``ernic_prometheus_config_dir``
     - ``/etc/prometheus/file_sd``
     - Directory for Prometheus ``file_sd`` configs.
   * - ``ernic_grafana_dashboard_dir``
     - ``/var/lib/grafana/dashboards``
     - Grafana provisioning dashboards directory.

Metrics Reference
-----------------

All metric names start with ``ernic_``.  Labels use
``instance`` for the numeric server instance ID (1, 2,
...) and ``qp`` for the QP handle.

Cluster and Instance Metrics
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_instances_total``
     - Gauge
     - Total number of rocm-ernic server instances.
   * - ``ernic_instance_up``
     - Gauge
     - Whether each instance process is alive
       (1 = running, 0 = dead).
       Labels: ``instance``, ``role``, ``mac``.
   * - ``ernic_instance_uptime_seconds``
     - Gauge
     - Process uptime in seconds.
       Label: ``instance``.

VM Lifecycle Metrics
^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_vms_total``
     - Gauge
     - Total number of attached VMs.
   * - ``ernic_vm_attached``
     - Gauge
     - Whether a VM is attached (1 = yes).
       Label: ``instance``.
   * - ``ernic_vm_uptime_seconds``
     - Gauge
     - VM uptime in seconds.
       Label: ``instance``.
   * - ``ernic_vm_gpu_passthrough``
     - Gauge
     - Whether GPU passthrough is enabled (1 = yes).
       Label: ``instance``.

Network Traffic
^^^^^^^^^^^^^^^

All per-instance, labelled ``{instance="<id>"}``:

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_ip_bytes_tx_total``
     - Gauge
     - Total IP/Ethernet bytes transmitted.
   * - ``ernic_ip_bytes_rx_total``
     - Gauge
     - Total IP/Ethernet bytes received.
   * - ``ernic_rdma_bytes_sent_total``
     - Gauge
     - Total bytes sent via RDMA SEND operations.
   * - ``ernic_rdma_bytes_received_total``
     - Gauge
     - Total bytes received via RDMA RECV operations.
   * - ``ernic_rdma_bytes_read_total``
     - Gauge
     - Total bytes via RDMA Read.
   * - ``ernic_rdma_bytes_write_total``
     - Gauge
     - Total bytes via RDMA Write.
   * - ``ernic_rdma_bytes_total``
     - Gauge
     - Aggregate: send + recv + read + write.
   * - ``ernic_ip_bytes_total``
     - Gauge
     - Aggregate: IP TX + RX.

Device Health and Events
^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_flr_reset_total``
     - Gauge
     - Cumulative FLR / device reset count.
       Label: ``instance``.
   * - ``ernic_commands_total``
     - Gauge
     - Total PVRDMA commands processed.
       Label: ``instance``.
   * - ``ernic_interrupts_total``
     - Gauge
     - Total interrupts delivered.
       Label: ``instance``.
   * - ``ernic_connection_up``
     - Gauge
     - Connection state (1 = connected).
       Labels: ``instance``, ``state``.
   * - ``ernic_mmio_reads_total``
     - Gauge
     - Total MMIO read operations.
       Label: ``instance``.
   * - ``ernic_mmio_writes_total``
     - Gauge
     - Total MMIO write operations.
       Label: ``instance``.
   * - ``ernic_stats_writes_total``
     - Gauge
     - How many times the server has flushed stats.
       Label: ``instance``.

Per-QP Metrics
^^^^^^^^^^^^^^

All labelled ``{instance="<id>", qp="<handle>"}``:

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_qp_count``
     - Gauge
     - Number of active Queue Pairs.
       Label: ``instance``.
   * - ``ernic_qp_bytes_sent_total``
     - Gauge
     - Bytes sent via SEND on this QP.
   * - ``ernic_qp_bytes_received_total``
     - Gauge
     - Bytes received via RECV on this QP.
   * - ``ernic_qp_bytes_rdma_read_total``
     - Gauge
     - RDMA Read bytes on this QP.
   * - ``ernic_qp_bytes_rdma_write_total``
     - Gauge
     - RDMA Write bytes on this QP.
   * - ``ernic_qp_wqes_processed_total``
     - Gauge
     - Total WQEs processed on this QP.
   * - ``ernic_qp_cqes_posted_total``
     - Gauge
     - Total CQEs posted on this QP.
   * - ``ernic_qp_doorbell_send_total``
     - Gauge
     - Send doorbell rings on this QP.
   * - ``ernic_qp_doorbell_recv_total``
     - Gauge
     - Receive doorbell rings on this QP.

Grafana Dashboard
-----------------

The dashboard JSON is at
``prometheus/grafana/ernic-dashboard.json`` in the
source tree and
is installed to the Grafana provisioning directory by
the Ansible playbook.  It can also be imported manually
via the Grafana UI (Dashboards > Import > Upload JSON).

Dashboard rows:

1. **Cluster Overview** -- stat panels for instance
   count, running instances, attached VMs, FLR resets,
   total QPs, and GPU-passthrough VMs; tables showing
   instance and VM details.

2. **Network Traffic** -- time-series panels for IP and
   RDMA traffic rates; a pie chart comparing RDMA vs
   IP total bytes; a per-instance traffic totals table.

3. **RDMA Detail** -- separate Send/Recv and Read/Write
   rate panels; a per-QP traffic table.

4. **Device Health** -- FLR reset count over time,
   commands/s, interrupts/s, MMIO read/write rates,
   and stats write count.

5. **VM Lifecycle** -- VM count over time, per-instance
   attached gauge, VM uptime, and connection state
   timeline.

The dashboard uses a template variable ``$instance``
that allows filtering to a specific server instance or
viewing all instances at once.  Default refresh is 10 s
with a 1 h time window.

Useful PromQL Examples
----------------------

Aggregate RDMA throughput across all instances:

.. code-block:: promql

   sum(rate(ernic_rdma_bytes_total[5m]))

Per-instance IP vs RDMA ratio:

.. code-block:: promql

     rate(ernic_rdma_bytes_total[5m])
   / (rate(ernic_rdma_bytes_total[5m])
      + rate(ernic_ip_bytes_total[5m]))

Alert when any instance is down:

.. code-block:: promql

   ernic_instance_up == 0

Alert on FLR reset:

.. code-block:: promql

   increase(ernic_flr_reset_total[5m]) > 0
