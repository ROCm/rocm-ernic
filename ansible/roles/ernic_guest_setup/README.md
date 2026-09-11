# ernic_guest_setup

Turns a prepared Ubuntu guest into a working rocm-ernic RDMA node: the DKMS
kernel modules, rdma-core, udev naming rules, an address on the emulated NIC,
and optionally rocm-xio for GPU-initiated transfers.

## Device mode

`ernic_device_mode` decides what gets built. It defaults to `ionic` and must
match the mode the host's `rocm-ernic` server runs in.

| Component | `ionic` (default) | `legacy` (deprecated) |
|---|---|---|
| PCI ID | `1022:8001` | `1022:8000` |
| Kernel modules | `ionic`, `ionic_rdma` from patched upstream sources, DKMS package `ionic-ernic` | `rocm_ernic_eth`, `rocm_ernic_rdma` from `driver/` |
| rdma-core | stock, upstream `providers/ionic` | `rocm_ernic` provider injected |
| rocm-xio | `GDA_IONIC=ON`, `RDMA_CORE_BUILD=OFF` | `GDA_ERNIC=ON` |

The legacy path is deprecated and will be removed in a future release. udev
names the devices `rocm-ernic0` and `rocm-rdma-ernic0` in both modes, so
nothing downstream of this role has to care which one ran.

ionic mode needs a guest kernel of 6.18 or newer
(`ernic_ionic_min_kernel`) — that is where `drivers/infiniband/hw/ionic`
landed — and rdma-core 61 or newer, which is where `providers/ionic` did.

The floor is not enough on its own. The ionic sources track IB-core helpers
that move between minor releases, so the guest kernel's major.minor must also
match `IONIC_KERNEL_REF`: `v7.2.4` sources build on a 7.2.3 kernel but not on a
7.0 one. The role asserts this before the DKMS build rather than letting it
fail as a wall of implicit-declaration errors. `ernic_image_prep` installs a
matching mainline kernel when it builds the golden image.

## Overview

Phases, each behind a flag:

| Phase | Tasks | Flag |
|---|---|---|
| Guest agent | `qemu-guest-agent` for QMP `guest-get-load` | `ernic_guest_agent` |
| Stage sources | push `patches/` and the ionic helper scripts (legacy: `driver/`, `rdma-core/`) from the controller | always |
| rdma-core | download, patch or inject the provider, build, install, stamp | `ernic_build_rdma_core` |
| Driver | fetch and patch the ionic sources, DKMS build, udev rules, modprobe, `ibv_devices` checks | `ernic_install_driver` |
| NIC | hostname, `/etc/hosts`, address on `ernic_nic_name` | `ernic_configure_nic` |
| rocm-xio | build, `rocm-xio.ko`, `xio-tester` | `ernic_gpu_passthrough` |

What the controller supplies comes from
[`ernic_source`](../ernic_source/README.md), which this role includes. In ionic
mode that is only the patch series and two helper scripts: the guest fetches
the upstream kernel sources and rocm-xio itself. A tarball staged by
`ernic_host_setup` at `ernic_rocm_xio_tarball` is still used when it exists, so
air-gapped guests keep working.

The rdma-core build is the expensive part (about seven minutes per guest), so
it is skipped when `provider.stamp` shows it was built from the same mode,
version and source hashes. The stamp is written last, so an interrupted build
is not mistaken for a complete one.

Run `ernic_image_prep` first — this role assumes RDMA userspace, ROCm and the
build toolchain are already present. That includes `perftest`: the rocm-xio
fork is only built under `ernic_gpu_passthrough`, which is off in CI, so the
`ib_*_bw` binaries a CI perf sweep measures with are the distro package
(24.01.0, reporting `Version: 6.20`) rather than the fork.

## Requirements

- Ubuntu resolute (26.04) guest in ionic mode, noble (24.04) or resolute in
  legacy mode
- `become: true`
- `community.general` for `modprobe` / `make`
- A rocm-ernic checkout on the controller, or network access to clone one

## Role Variables

```yaml
# Device mode: ionic (default) or legacy (deprecated)
ernic_device_mode: ionic

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
ernic_rdma_core_version: "62.0"
ernic_rdma_core_prefix: /usr
ernic_rdma_core_hold: true

# NIC. vm_index / vm_ip host vars (set by vm-create.yml) are
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
