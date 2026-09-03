# sbates130272.rocm_ernic

An Ansible collection for [rocm-ernic][ref-rocm-ernic]: roles that prepare
Ubuntu hosts and VM images to run the emulated RDMA NIC, its kernel driver and
the matching rdma-core provider.

## Introduction

rocm-ernic emulates an RDMA NIC for a mesh of QEMU guests. Getting a guest to
the point where `ibv_devices` shows a device takes a fair amount of work: a
DKMS kernel driver, a custom rdma-core build carrying the `rocm_ernic`
provider, udev naming rules, and addressing on the emulated NIC. This
collection packages that work so it can be applied to any image build, not just
the test mesh in the rocm-ernic repo.

It supports Ubuntu 24.04 LTS (noble) and 26.04 LTS (resolute).

## Roles

| Role | Runs on | Purpose |
|---|---|---|
| [`ernic_image_prep`](roles/ernic_image_prep/README.md) | golden image | RDMA userspace, ROCm, build tools, `modules-load.d`, `pci.ids` — everything worth baking in once |
| [`ernic_guest_setup`](roles/ernic_guest_setup/README.md) | guest VM | DKMS driver, rdma-core with the `rocm_ernic` provider, udev rules, NIC addressing, rocm-xio |
| [`ernic_host_setup`](roles/ernic_host_setup/README.md) | host | build/install/run the rocm-ernic service, bind GPUs to vfio-pci, stage rocm-xio |
| [`ernic_source`](roles/ernic_source/README.md) | controller | resolve or clone the rocm-ernic checkout the others copy from (included automatically) |

## Installing

```bash
ansible-galaxy collection install sbates130272.rocm_ernic
```

Or in a `requirements.yml`:

```yaml
collections:
  - name: sbates130272.rocm_ernic
    version: ">=0.1.0"
  - name: sbates130272.batesste
    version: ">=1.3.0"
```

## Building a VM image

The usual sequence is: bake the expensive, version-stable parts into a golden
image, then apply the per-VM parts to each overlay.

```yaml
- name: Bake rocm-ernic prerequisites into the image
  hosts: golden_image
  become: true
  roles:
    - role: sbates130272.rocm_ernic.ernic_image_prep

- name: Make each guest a rocm-ernic RDMA node
  hosts: ernic_vms
  become: true
  roles:
    - role: sbates130272.rocm_ernic.ernic_guest_setup
      vars:
        ernic_guest_vm_ip: "192.168.200.{{ 10 * (vm_index | int) }}"
```

By default the roles clone `ROCm/rocm-ernic` on the controller to get the
driver and provider sources. Point them at a checkout you already have with
`ernic_source_dir`, and pin `ernic_source_repo_version` to a tag when the image
needs to be reproducible.

## Requirements

- `ansible-core` >= 2.16
- `sbates130272.batesste` >= 1.3.0 (`rdma_setup`, `rocm_setup`)
- `ansible.posix` >= 1.6.2, `community.general` >= 9.0.0
- A rocm-ernic checkout on the controller, or network access to clone one

## License

MIT

[ref-rocm-ernic]: https://github.com/ROCm/rocm-ernic
