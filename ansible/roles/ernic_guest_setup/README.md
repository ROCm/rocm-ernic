# ernic_guest_setup

Turns a prepared Ubuntu guest into a working rocm-ernic RDMA node: the DKMS
kernel modules, rdma-core, udev naming rules, an address on the emulated NIC,
and optionally rocm-xio for GPU-initiated transfers.

## Device

The guest drives PCI device `1dd8:100a` with the upstream `ionic` and
`ionic_rdma` modules, built by DKMS from patched upstream kernel sources as
the `ionic-ernic` package, against stock rdma-core with its upstream
`providers/ionic`. rocm-xio is built with `GDA_IONIC=ON` and
`RDMA_CORE_BUILD=OFF`. udev names the devices `rocm-ernic0` and
`rocm-rdma-ernic0`.

This needs a guest kernel of 6.18 or newer
(`ernic_ionic_min_kernel`) — that is where `drivers/infiniband/hw/ionic`
landed — and rdma-core 61 or newer, which is where `providers/ionic` did.

The floor is not enough on its own. The ionic sources track IB-core helpers
that move between minor releases, so the guest kernel's major.minor must also
match `IONIC_KERNEL_REF`: `v7.2.4` sources build on a 7.2.3 kernel but not on a
7.0 one. The role asserts this before the DKMS build rather than letting it
fail as a wall of implicit-declaration errors. Supplying a matching kernel is
the base image's job — in this repo, the `ionic` flavour of
[batesste-ci-images](https://github.com/sbates130272/batesste-ci-images), which
pins mainline 7.2.3.

## Overview

Phases, each behind a flag:

| Phase | Tasks | Flag |
|---|---|---|
| Guest agent | `qemu-guest-agent` for QMP `guest-get-load` | `ernic_guest_agent` |
| Stage sources | push `patches/` and the ionic helper scripts from the controller | always |
| rdma-core | download, patch or inject the provider, build, install, stamp | `ernic_build_rdma_core` |
| Driver | fetch and patch the ionic sources, DKMS build, udev rules, modprobe, `ibv_devices` checks | `ernic_install_driver` |
| NIC | hostname, `/etc/hosts`, address on `ernic_nic_name` | `ernic_configure_nic` |
| rocm-xio | build, `rocm-xio.ko`, `xio-tester` | `ernic_gpu_passthrough` |

What the controller supplies comes from
[`ernic_source`](../ernic_source/README.md), which this role includes. That is
only the patch series and two helper scripts: the guest fetches
the upstream kernel sources and rocm-xio itself. A tarball staged by
`ernic_host_setup` at `ernic_rocm_xio_tarball` is still used when it exists, so
air-gapped guests keep working.

The rdma-core build is the expensive part (about seven minutes per guest), so
it is skipped when `provider.stamp` shows it was built from the same version
and source hashes. The stamp is written last, so an interrupted build
is not mistaken for a complete one.

This role assumes RDMA userspace, ROCm and the build toolchain are already
present in the base image; `ernic_guest_build_deps` installs them when they
are not. That includes `perftest`: the rocm-xio
fork is only built under `ernic_gpu_passthrough`, which is off in CI, so the
`ib_*_bw` binaries a CI perf sweep measures with are the distro package
(24.01.0, reporting `Version: 6.20`) rather than the fork.

## Requirements

- Ubuntu resolute (26.04) guest
- `become: true`
- `community.general` for `modprobe` / `make`
- A rocm-ernic checkout on the controller, or network access to clone one

## Role Variables

```yaml
# ionic kernel modules. The ref itself is ernic_ionic_kernel_ref,
# owned by the ernic_source role: empty means "use the
# IONIC_KERNEL_REF pinned in cmake/ErnicKernelModule.cmake".
ernic_ionic_source_dir: /var/tmp/ionic-src
ernic_ionic_min_kernel: "6.18"

# Phase gates
ernic_guest_agent: true
ernic_build_rdma_core: true
ernic_install_driver: true
ernic_configure_nic: true
ernic_set_hostname: true
ernic_gpu_passthrough: true
ernic_pci_mmio_bridge: true

# rdma-core.  The build installs over the distro's rdma-core,
# so the packages it overwrites are held: only this build has
# an ionic provider, and an apt upgrade that restored the
# packaged libraries would take the RDMA device away.
ernic_rdma_core_version: "61.0"
ernic_rdma_core_prefix: /usr
ernic_rdma_core_hold: true

# NIC. vm_index / vm_ip host vars (set by vm-register.yml) are
# picked up automatically; set these directly for a static
# inventory.
ernic_nic_name: rocm-ernic0
ernic_nic_prefix: 24
ernic_guest_vm_index: "{{ vm_index | default(1) }}"
ernic_guest_vm_ip: "{{ vm_ip | default('') }}"
ernic_vm_name_base: rocm-ernic-vm

# rocm-xio. The guest clones this unless a tarball staged by
# ernic_host_setup is found on the controller.
ernic_rocm_xio_guest_repo: "https://github.com/ROCm/rocm-xio.git"
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
