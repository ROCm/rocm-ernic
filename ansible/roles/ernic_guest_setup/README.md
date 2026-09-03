# ernic_guest_setup

Turns a prepared Ubuntu guest into a working rocm-ernic RDMA node: the DKMS
kernel driver, a custom rdma-core carrying the `rocm_ernic` provider, udev
naming rules, an address on the emulated NIC, and optionally rocm-xio for
GPU-initiated transfers.

## Overview

Phases, each behind a flag:

| Phase | Tasks | Flag |
|---|---|---|
| Guest agent | `qemu-guest-agent` for QMP `guest-get-load` | `ernic_guest_agent` |
| Stage sources | push `driver/`, `rdma-core/` from the controller | always |
| rdma-core | download, inject the provider, build, install, stamp | `ernic_build_rdma_core` |
| Driver | DKMS build, udev rules, modprobe, `ibv_devices` checks | `ernic_install_driver` |
| NIC | hostname, `/etc/hosts`, address on `ernic_nic_name` | `ernic_configure_nic` |
| rocm-xio | build, `rocm-xio.ko`, `xio-tester` | `ernic_gpu_passthrough` |

Sources come from the controller via
[`ernic_source`](../ernic_source/README.md), which this role includes: guests in
a rocm-ernic mesh usually have no route to GitHub.

The rdma-core build is the expensive part (about seven minutes per guest), so
it is skipped when `provider.stamp` shows the installed provider was built from
the same source hash and rdma-core version. The stamp is written last, so an
interrupted build is not mistaken for a complete one.

Run `ernic_image_prep` first — this role assumes RDMA userspace, ROCm and the
build toolchain are already present.

## Requirements

- Ubuntu noble (24.04) or resolute (26.04) guest
- `become: true`
- `community.general` for `modprobe` / `make`
- A rocm-ernic checkout on the controller, or network access to clone one

## Role Variables

```yaml
# Phase gates
ernic_guest_agent: true
ernic_build_rdma_core: true
ernic_install_driver: true
ernic_configure_nic: true
ernic_set_hostname: true
ernic_gpu_passthrough: true
ernic_pci_mmio_bridge: true

# rdma-core
ernic_rdma_core_version: "62.0"
ernic_rdma_core_prefix: /usr

# NIC. vm_index / vm_ip host vars (set by vm-create.yml) are
# picked up automatically; set these directly for a static
# inventory.
ernic_nic_name: rocm-ernic0
ernic_nic_prefix: 24
ernic_guest_vm_index: "{{ vm_index | default(1) }}"
ernic_guest_vm_ip: "{{ vm_ip | default('') }}"
ernic_vm_name_base: rocm-ernic-vm

# rocm-xio: tarball staged on the guest by ernic_host_setup
ernic_rocm_xio_tarball: /tmp/rocm-xio.tar.gz
```

See `defaults/main.yml` for the rest (staging paths, DKMS version, module
list, timeouts).

## Example

```yaml
- hosts: ernic_vms
  become: true
  roles:
    - role: sbates130272.rocm_ernic.ernic_guest_setup
      vars:
        ernic_source_dir: /home/me/Projects/rocm-ernic
        ernic_gpu_passthrough: false
        ernic_guest_vm_ip: 192.168.200.10
```

## License

MIT
