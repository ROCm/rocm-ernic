# sbates130272.rocm_ernic

An Ansible collection for [rocm-ernic][ref-rocm-ernic]: roles that turn a
prepared Ubuntu guest into an RDMA node on the emulated NIC, and prepare the
host that serves it.

## Introduction

rocm-ernic emulates an RDMA NIC for a mesh of QEMU guests. Getting a guest to
the point where `ibv_devices` shows a device takes a DKMS kernel driver, an
rdma-core with the ionic provider, and addressing on the emulated NIC. This
collection packages that work so it can be applied to any guest, not just the
test mesh in the rocm-ernic repo.

The guest runs the upstream `ionic` and `ionic_rdma` drivers built by DKMS
from a patched upstream kernel tree, with the upstream `providers/ionic`, and
each instance gets a host TAP on a shared bridge so the guests can reach each
other over IP.

### What this collection does not do

It does not build a guest image. The base image is expected to carry a kernel
new enough for the driver and the toolchain to build it — in this repo that is
the published `ionic` flavour from
[batesste-ci-images][ref-ci-images], which pins mainline 7.2.3 on Ubuntu 26.04
(resolute) and bakes the RDMA userspace, DKMS toolchain and `perftest`.

The kernel floor is not negotiable in either direction:
`drivers/infiniband/hw/ionic` merged in 6.18, and `ionic-ernic` calls
`ib_umem_get_va`, which lands after 7.0 — so a 7.0 guest compiles the floor
check and then fails the DKMS build. `ernic_ionic_min_kernel` asserts the
lower bound against the running guest.

## Roles

| Role | Runs on | Purpose |
|---|---|---|
| [`ernic_guest_setup`](roles/ernic_guest_setup/README.md) | guest VM | DKMS ionic driver, rdma-core, udev rules, NIC addressing, rocm-xio |
| [`ernic_host_setup`](roles/ernic_host_setup/README.md) | host | build/install/run the rocm-ernic service, TAP/bridge networking, bind GPUs to vfio-pci, stage rocm-xio |
| [`ernic_source`](roles/ernic_source/README.md) | controller | resolve or clone the rocm-ernic checkout the others copy from (included automatically) |

## Installing

```bash
ansible-galaxy collection install sbates130272.rocm_ernic
```

Or in a `requirements.yml`:

```yaml
collections:
  - name: sbates130272.rocm_ernic
    version: ">=0.2.0"
```

## Provisioning a guest

```yaml
- name: Make each guest a rocm-ernic RDMA node
  hosts: ernic_vms
  become: true
  roles:
    - role: sbates130272.rocm_ernic.ernic_guest_setup
      vars:
        ernic_guest_vm_ip: "192.168.200.{{ 10 * (vm_index | int) }}"
```

Set `ernic_guest_build_deps: true` if the base image does not already carry
`dkms`, `cmake`, `ninja-build` and the rdma-core build dependencies.

By default the roles clone `ROCm/rocm-ernic` on the controller to get the
driver and provider sources. Point them at a checkout you already have with
`ernic_source_dir`, and pin `ernic_source_repo_version` to a tag when the run
needs to be reproducible.

`ernic_rdma_core_version` defaults to `61.0`, which Ubuntu 26.04 packages. The
role writes a provider stamp and skips the source build when the installed
provider already matches, so on resolute nothing is built and nothing
`dpkg`-owned is overwritten — and because nothing is overwritten, the packages
in `ernic_rdma_core_hold_packages` are left unheld and keep taking archive
updates. Raise it only for a provider genuinely newer than the archive's; the
source build that follows records itself in the guest, and the holds come back
with it on that run and on every later one.

## Requirements

- `ansible-core` >= 2.18, which is what `community.general` >= 13.3.0 needs
- `community.general` >= 13.3.0 (`modprobe`, `make`)
- A rocm-ernic checkout on the controller, or network access to clone one
- A guest running a kernel >= 6.18, and >= 7.2 to build `ionic-ernic`

## License

MIT

[ref-rocm-ernic]: https://github.com/ROCm/rocm-ernic
[ref-ci-images]: https://github.com/sbates130272/batesste-ci-images
