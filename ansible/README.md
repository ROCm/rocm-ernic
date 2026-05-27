# rocm-ernic Ansible Automation

Ansible playbooks that automate the full rocm-ernic
integration test workflow: build and install the service,
create golden VM images, launch VMs, install the kernel
driver and custom rdma-core provider, and run iperf3 /
perftest sanity tests.

## Prerequisites

- Ubuntu 24.04 or 26.04 host
- Ansible 2.16+ (`sudo apt install ansible`)
- `libfabric-dev` when using the default `ofi-tcp` mesh backend
  (installed automatically by `host-setup.yml`)
- QEMU 10.1+ installed (default `/opt/qemu-v10.1.2/`)
- The [qemu-minimal](https://github.com/sbates130272/qemu-minimal)
  checkout at `~/Projects/qemu-minimal`
- The `sbates130272.batesste` Galaxy collection

Install dependencies:

```bash
ansible-galaxy collection install -r requirements.yml
```

## Quick Start

Run the full end-to-end workflow (build, VMs, tests):

```bash
cd ansible
ansible-playbook site.yml
```

## Running Individual Plays

```bash
# Host setup only (build + install + configure service)
ansible-playbook playbooks/host-setup.yml

# Create golden image + launch VMs
ansible-playbook playbooks/vm-create.yml

# Guest provisioning (driver, rdma-core, NIC config)
ansible-playbook playbooks/guest-setup.yml

# Sanity tests (iperf3 + perftest)
ansible-playbook playbooks/sanity-tests.yml

# Full performance sweep (BW + latency + reliability)
ansible-playbook playbooks/performance-tests.yml

# Stress tests (multi-QP, bidir, soak, churn, etc.)
ansible-playbook playbooks/stress-tests.yml
```

## Variable Overrides

Override any default in `group_vars/all.yml` via `-e`:

```bash
# BSD socket mesh instead of libfabric ofi-tcp
ansible-playbook site.yml -e ernic_mesh_backend=tcp

# Use 4 instances instead of 2
ansible-playbook site.yml -e ernic_instances=4

# Skip the build step (use existing install)
ansible-playbook site.yml -e ernic_build=false

# Skip golden image creation (images exist)
ansible-playbook site.yml -e ernic_golden_image=false

# Skip sanity tests
ansible-playbook site.yml -e ernic_tests=false

# Run performance sweep with custom parameters
ansible-playbook playbooks/performance-tests.yml \
  -e ernic_perf_bw_iters=200 \
  -e ernic_perf_reliability_runs=10

# Run stress tests with a 1-hour soak
ansible-playbook playbooks/stress-tests.yml \
  -e ernic_stress_soak_duration=3600

# Run stress tests with 50k iterations
ansible-playbook playbooks/stress-tests.yml \
  -e ernic_stress_high_iters=50000

# Specify a golden backing image for overlays
ansible-playbook site.yml \
  -e ernic_vm_backing=/path/to/backing.qcow2
```

See `group_vars/all.yml` for the full variable reference.

## Directory Layout

```
ansible/
├── ansible.cfg           # Ansible configuration
├── requirements.yml      # Galaxy collection deps
├── site.yml              # Master playbook
├── group_vars/
│   └── all.yml           # Default variables
├── inventory/
│   └── hosts.yml         # Static inventory
├── playbooks/
│   ├── host-setup.yml         # Build, install, configure
│   ├── vm-create.yml          # Golden image + VM launch
│   ├── guest-setup.yml        # Driver + rdma-core
│   ├── sanity-tests.yml       # iperf3 + perftest
│   ├── performance-tests.yml  # Full BW/lat sweeps
│   └── stress-tests.yml       # Multi-QP, soak, churn
└── templates/
    └── rocm-ernic.env.j2 # Env file template
```

## How It Works

1. **host-setup** builds the rocm-ernic server, installs the
   systemd service and ernicctl, templates the env file from
   Ansible variables, and starts the service.

2. **vm-create** checks for an existing golden backing qcow2.
   If absent, it runs `gen-vm` to create one via cloud-init.
   It then calls `ernicctl vm-launch` for each instance and
   waits for SSH readiness.

3. **guest-setup** uses the `sbates130272.batesste.rdma_setup`
   role to install RDMA packages (including perftest), then
   builds the custom rdma-core with the rocm_ernic provider,
   builds and loads the kernel driver, and configures IP
   addresses on the emulated NICs.

4. **sanity-tests** runs iperf3 between two VMs over the
   emulated Ethernet NICs for TCP/IP validation, then runs
   perftest tools (`ib_send_bw`, `ibv_rc_pingpong`) for
   RDMA verification.

5. **performance-tests** runs the full bandwidth and latency
   sweeps matching the test report format: `ib_send_bw`,
   `ib_write_bw`, `ib_read_bw` across 12 message sizes
   (4 KB to 8 MB), the same for `ib_send_lat`,
   `ib_write_lat`, `ib_read_lat`, plus multi-run
   reliability at 64 KB and `ibv_rc_pingpong` rounds.
   Timestamped CSV files are written to
   `docs/perf-results/` for easy before/after comparison.

6. **stress-tests** exercises the emulated RDMA device
   under conditions the performance sweep never touches.
   Eight test sections run sequentially:

   - **Multi-QP** -- `ib_send_bw` / `ib_write_bw` with
     `-q 2`, `-q 4`, `-q 8` queue pairs at 64 KB.
   - **Bidirectional** -- `--bidirectional` flag at 64 KB,
     256 KB, and 1 MB to test full-duplex traffic.
   - **Duration soak** -- `--duration N` sustained load
     (default 300 s) with periodic `ernicctl stats`
     polling to a time-series CSV.
   - **Concurrent verbs** -- `ib_send_bw` and
     `ib_write_bw` running simultaneously on different
     perftest ports for `ernic_stress_concurrent_duration`
     seconds.
   - **High iterations** -- `-n 10000` (overridable) at
     64 KB and 1 MB to detect slow resource leaks.
   - **QP churn** -- rapid create/destroy cycles via
     `ibv_rc_pingpong` (default 50 cycles).
   - **Resource limits** -- sequential QP creation up to
     64 attempts to find the maximum.
   - **iperf3 TCP baseline** -- TCP throughput over the
     emulated Ethernet NICs for comparison with RDMA
     numbers.

   Override any knob via `-e`:

   ```
   ernic_stress_multi_qp_counts  [2, 4, 8]
   ernic_stress_bidir_sizes      [65536, 262144, 1048576]
   ernic_stress_soak_duration    300  (seconds)
   ernic_stress_high_iters       10000
   ernic_stress_qp_churn_cycles  50
   ernic_stress_concurrent_duration  60  (seconds)
   ```
