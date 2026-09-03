# rocm-ernic Ansible Automation

Ansible playbooks that automate the full rocm-ernic
integration test workflow: build and install the service,
create golden VM images, launch VMs, install the kernel
driver and custom rdma-core provider, and run iperf3 /
perftest sanity tests.

The reusable parts — everything needed to turn an Ubuntu
image into a rocm-ernic node — are packaged as the
[`sbates130272.rocm_ernic`](COLLECTION.md) collection,
whose roles live in [roles/](roles/). This directory *is* the
collection: `galaxy.yml` sits here and `build_ignore` excludes
the repo-local parts (playbooks, inventory, group_vars) from
the published artifact. The setup playbooks are thin wrappers
around the roles; the test playbooks are repo-local.

## Prerequisites

- Ubuntu 24.04 or 26.04 host
- Ansible core 2.18+ — Ubuntu 24.04's `ansible` package ships 2.16,
  which is too old for `community.general` 13; use `pipx install
  ansible-core` or the Ansible PPA
- QEMU 10.1+ installed (default `/opt/qemu-v10.1.2/`)
- The [qemu-minimal](https://github.com/sbates130272/qemu-minimal)
  checkout at `~/Projects/qemu-minimal`
- The `sbates130272.batesste` Galaxy collection

Install dependencies:

```bash
ansible-galaxy collection install -r requirements.yml
```

`sbates130272.rocm_ernic` is *not* in `requirements.yml`:
`ansible.cfg` sets `roles_path = ./roles:...`, so
these playbooks reach its roles by short name and always run
against this checkout. That path is relative, so run
`ansible-playbook` from this directory.

To use the roles from another project, install the published
collection and address them by fully qualified name:

```bash
ansible-galaxy collection install sbates130272.rocm_ernic
```

```yaml
roles:
  - role: sbates130272.rocm_ernic.ernic_image_prep
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

`group_vars/all.yml` holds the site configuration for this
repo.
Per-role defaults (`ernic_bin`, `ernic_nic_name`,
`ernic_rdma_core_version`, the debug switches, …) live in
`roles/*/defaults/main.yml`; anything set in
`group_vars/all.yml` wins over them.

## Directory Layout

```text
ansible/                  # this directory is the collection
├── galaxy.yml            # Collection metadata + build_ignore
├── COLLECTION.md         # Galaxy landing page
├── meta/runtime.yml      # Minimum ansible-core
├── changelogs/           # antsibull-changelog fragments
├── roles/                # Shipped to Galaxy
│   ├── ernic_source/         # Resolve/clone the checkout
│   ├── ernic_image_prep/     # Bake a golden image
│   ├── ernic_guest_setup/    # Driver + rdma-core + NIC
│   └── ernic_host_setup/     # Build, service, vfio-pci
│                         # ── below: repo-local, build_ignore'd
├── ansible.cfg           # Ansible configuration
├── requirements.yml      # Galaxy collection deps
├── site.yml              # Master playbook
├── group_vars/
│   └── all.yml           # Site configuration
├── inventory/
│   └── hosts.yml         # Static inventory
├── playbooks/
│   ├── host-setup.yml         # -> ernic_host_setup
│   ├── vm-create.yml          # Golden image + VM launch
│   ├── guest-setup.yml        # -> ernic_guest_setup
│   ├── sanity-tests.yml       # iperf3 + perftest
│   ├── performance-tests.yml  # Full BW/lat sweeps
│   └── stress-tests.yml       # Multi-QP, soak, churn
└── templates/
    └── packages-ernic    # gen-vm package list
```

## How It Works

1. **host-setup** runs `ernic_host_setup`: builds the
   rocm-ernic server, installs the systemd service and
   ernicctl, templates the env file from Ansible variables,
   starts the service, binds GPUs to vfio-pci and stages a
   rocm-xio tarball for the guests.

2. **vm-create** checks for an existing golden backing qcow2.
   If absent, it runs `gen-vm` to create one via cloud-init,
   boots it once and applies `ernic_image_prep` to bake in
   RDMA userspace, ROCm and the build toolchain. It then calls
   `ernicctl vm-launch` for each instance and waits for SSH
   readiness.

3. **guest-setup** runs `ernic_guest_setup`: builds the custom
   rdma-core with the rocm_ernic provider, builds and loads
   the kernel driver via DKMS, applies the udev rules, and
   configures IP addresses on the emulated NICs.

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
