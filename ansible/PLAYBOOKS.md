# rocm-ernic Ansible Automation

Ansible playbooks that automate the rocm-ernic integration
test workflow: build and install the service, provision the
guests with the kernel driver and the rdma-core provider, and
run iperf3 / perftest / NVMe-oF tests against them.

The guest disk is **not** built here. It is the published
`ionic` flavour from
[batesste-ci-images](https://github.com/sbates130272/batesste-ci-images),
pulled by `scripts/fetch-guest-image.sh` or `ci/jobs/vm-up.sh`.
That image pins mainline kernel 7.2.3 on Ubuntu 26.04 and bakes
the RDMA userspace, the DKMS toolchain and `perftest`.

The reusable parts — everything needed to turn that image into a
rocm-ernic node — are packaged as the
[`sbates130272.rocm_ernic`](README.md) collection,
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

Install dependencies:

```bash
ansible-galaxy collection install -r requirements.yml
```

That is one collection, `community.general`. Through 0.1.0 it was
six and ~29M, because `sbates130272.batesste` came with
`amazon.aws` and `community.libvirt`; the role that needed it
built a golden image, which the published artifact replaced.

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
  - role: sbates130272.rocm_ernic.ernic_guest_setup
```

## Quick Start

Run the full end-to-end workflow (build, provision, test):

```bash
cd ansible
ansible-playbook site.yml
```

## Running Individual Plays

```bash
# Host setup only (build + install + configure service)
ansible-playbook playbooks/host-setup.yml

# Register the running VMs into the ernic_vms group.  Reads the
# launcher's instances.json; every play below needs it first.
ansible-playbook playbooks/vm-register.yml

# Guest provisioning (driver, rdma-core, NIC config)
ansible-playbook playbooks/guest-setup.yml

# Sanity tests (iperf3 + perftest)
ansible-playbook playbooks/sanity-tests.yml

# NVMe-oF against the in-process controller.  Needs only one
# guest, and the instance must have been started on the
# nvmeof backend (--backend nvmeof:size=256M,bs=4096).
ansible-playbook playbooks/nvmeof-tests.yml

# Full performance sweep (BW + latency + reliability)
ansible-playbook playbooks/performance-tests.yml
```

## Variable Overrides

Override any default in `group_vars/all.yml` via `-e`:

```bash
# Use 4 instances instead of 2
ansible-playbook site.yml -e ernic_instances=4

# Skip the build step (use existing install)
ansible-playbook site.yml -e ernic_build=false

# Skip sanity tests
ansible-playbook site.yml -e ernic_tests=false

# Run performance sweep with custom parameters
ansible-playbook playbooks/performance-tests.yml \
  -e ernic_perf_bw_iters=200 \
  -e ernic_perf_reliability_runs=10

# Pin a different published image
ansible-playbook site.yml \
  -e ernic_vm_artifact_tag=<tag>

# Specify a backing image for overlays yourself
ansible-playbook site.yml \
  -e ernic_vm_backing=/path/to/backing.qcow2
```

Keep `ernic_vm_artifact_tag` equal to `GUEST_ARTIFACT_TAG`
in `.github/workflows/system-tests.yml` and to
`CI_GUEST_ARTIFACT_TAG` in `ci/lib/common.sh`, so the VMs
launched here are the VMs CI tests. Nothing compares those
three to each other.

`scripts/fetch-guest-image.sh` does check the image against
this checkout before it caches it. The image's own
`vm-info.json` has to report a kernel at or above 6.18 --
below that there is no `drivers/infiniband/hw/ionic` to build
against -- sharing a `major.minor` with `IONIC_KERNEL_REF` in
`cmake/ErnicKernelModule.cmake`, and matching the hardcoded
`CI guest kernel` badge on line 9 of `README.md`, so a tag
bump cannot leave the front page advertising a kernel nothing
ships. Callers can add `--expect-user`, `--expect-disk`,
`--expect-release` and `--expect-flavour`; `ci/lib/common.sh`
passes all four. These assertions used to live in
`playbooks/vm-fetch.yml`, which 0.2.0 removed.

The hosted jobs do not run that script -- they pull through
`.github/actions/fetch-guest-vm` and carry their own inline
copy of the badge comparison in the `Read VM info` step of
`.github/workflows/system-tests.yml`. Deleting either copy
drops coverage of a path the other does not reach.

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
├── README.md             # Galaxy landing page
├── meta/runtime.yml      # Minimum ansible-core
├── changelogs/           # antsibull-changelog fragments
├── roles/                # Shipped to Galaxy
│   ├── ernic_source/         # Resolve/clone the checkout
│   ├── ernic_guest_setup/    # Driver + rdma-core + NIC
│   └── ernic_host_setup/     # Build, service, vfio-pci
│                         # ── below: repo-local, build_ignore'd
├── ansible.cfg           # Ansible configuration
├── PLAYBOOKS.md          # this file
├── requirements.yml      # Galaxy collection deps
├── site.yml              # Master playbook
├── ci-site.yml           # Self-hosted CI entry point
├── group_vars/
│   └── all.yml           # Site configuration
├── inventory/
│   └── hosts.yml         # Static inventory
└── playbooks/
    ├── host-setup.yml         # -> ernic_host_setup
    ├── vm-register.yml        # instances.json -> ernic_vms
    ├── guest-setup.yml        # -> ernic_guest_setup
    ├── sanity-tests.yml       # iperf3 + perftest
    ├── nvmeof-tests.yml       # NVMe-oF, one guest
    ├── tcp-performance-tests.yml  # iperf3 sweeps
    └── performance-tests.yml  # Full BW/lat sweeps
```

## How It Works

1. **host-setup** runs `ernic_host_setup`: builds the
   rocm-ernic server, installs the systemd service and
   ernicctl, templates the env file from Ansible variables,
   starts the service, binds GPUs to vfio-pci and stages a
   rocm-xio tarball for the guests.

2. **vm-register** reads the launcher's `instances.json` and
   adds each running VM to the `ernic_vms` group with its ssh
   port, user and NIC address. It creates no VMs — `ernicctl
   vm-launch` or `ci/jobs/vm-up.sh` does that — so it is the
   one registration path for both the developer and CI entry
   points.

3. **guest-setup** runs `ernic_guest_setup`: installs
   rdma-core with the ionic provider (or skips it when the
   image already carries a matching one, which resolute does),
   builds and loads the kernel driver via DKMS, applies the
   udev rules, and configures IP addresses on the emulated
   NICs.

4. **sanity-tests** runs iperf3 between two VMs over the
   emulated Ethernet NICs for TCP/IP validation, then runs
   perftest tools (`ib_send_bw`, `ibv_rc_pingpong`) for
   RDMA verification.

   **nvmeof-tests** is the odd one out, and is numbered with
   no phase of its own because it is not part of the
   sequence: it needs a single guest, not a pair. The
   rocm-ernic instance it is attached to must have been
   started with `--backend nvmeof`, which runs an NVMe over
   Fabrics target inside the server, so one guest and one
   server are a complete fabric. The play loads
   `nvme-rdma`, seeds the neighbour entry the controller has
   no way to answer ARP for, discovers and connects, checks a
   digest round trip through `O_DIRECT`, runs fio, then
   disconnects and asserts the namespace went away. It is not
   part of the `functional`/`test` tag sets, because against
   the usual two-VM mesh there is no controller to connect to
   -- ask for it by name with `--tags nvmeof`. Only
   `ci-site.yml` imports it, since that is always invoked
   with `--tags`; `site.yml` is run bare, where an import
   would run it unconditionally. Developers run the play
   directly.

5. **performance-tests** runs the full bandwidth and latency
   sweeps matching the test report format: `ib_send_bw`,
   `ib_write_bw`, `ib_read_bw` across 12 message sizes
   (4 KB to 8 MB), the same for `ib_send_lat`,
   `ib_write_lat`, `ib_read_lat`, plus multi-run
   reliability at 64 KB and `ibv_rc_pingpong` rounds.
   Timestamped CSV files are written to
   `docs/perf-results/` for easy before/after comparison.
