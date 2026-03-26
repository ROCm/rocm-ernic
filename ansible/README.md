# rocm-ernic Ansible Automation

Ansible playbooks that automate the full rocm-ernic
integration test workflow: build and install the service,
create golden VM images, launch VMs, install the kernel
driver and custom rdma-core provider, and run iperf3 /
perftest sanity tests.

## Prerequisites

- Ubuntu 24.04 or 26.04 host
- Ansible 2.16+ (`sudo apt install ansible`)
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
```

## Variable Overrides

Override any default in `group_vars/all.yml` via `-e`:

```bash
# Use 4 instances instead of 2
ansible-playbook site.yml -e ernic_instances=4

# Skip the build step (use existing install)
ansible-playbook site.yml -e ernic_skip_build=true

# Skip golden image creation (images exist)
ansible-playbook site.yml -e ernic_skip_golden_image=true

# Skip sanity tests
ansible-playbook site.yml -e ernic_skip_tests=true

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
│   ├── host-setup.yml    # Build, install, configure
│   ├── vm-create.yml     # Golden image + VM launch
│   ├── guest-setup.yml   # Driver + rdma-core
│   └── sanity-tests.yml  # iperf3 + perftest
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
