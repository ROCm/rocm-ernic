# ernic_image_prep

Bakes the RDMA, ROCm and rocm-ernic prerequisites into a base Ubuntu image so
that per-VM overlays do not have to re-download them on every test cycle.

## Overview

Apply this role to a freshly booted Ubuntu image (a golden qcow2 booted as a
temporary VM, a cloud instance, a Packer build). It installs:

- a mainline kernel matching the pinned ionic sources (ionic mode only)
- RDMA userspace and tooling via `sbates130272.batesste.rdma_setup`
- the build and test packages `ernic_guest_setup` needs (cmake, ninja, dkms,
  `libcli11-dev`, iperf3, …)
- the ROCm stack via `sbates130272.batesste.rocm_setup`, when
  `ernic_gpu_passthrough` is true

and applies the static configuration that has to be present before first boot:
`KillUserProcesses=no`, `modules-load.d` entries for the guest RDMA modules
and `rocm-xio`, and a `pci.ids` entry so `lspci` names the emulated NIC.

Both of those follow `ernic_device_mode`, which defaults to `ionic`: the image
loads `ionic` / `ionic_rdma` and names `1022:8001`. With
`-e ernic_device_mode=legacy` it loads `rocm_ernic_eth` / `rocm_ernic_rdma`
and names `1022:8000` instead. Prepare the image for the mode the guests will
actually run.

### The mainline kernel

`ionic_rdma` is built from the upstream sources `IONIC_KERNEL_REF` pins, and
those sources track IB-core helpers that move between minor releases. No Ubuntu
stock kernel is usable: the in-tree ionic RDMA driver starts at 6.18, noble HWE
is 6.17 and resolute GA is 7.0, while the pin is well past all three. So in
ionic mode the role installs the matching kernel from the Ubuntu mainline PPA,
reading the four `amd64` packages out of that version's `CHECKSUMS` manifest
(their filenames carry a build timestamp and cannot be constructed) and
verifying each sha256.

This is why `ernic_vm_release` defaults to resolute (26.04). On noble the
mainline `linux-image` preinst hands `run-parts` two directories and noble's
`debianutils` takes one, so the install dies at `run-parts: missing operand`;
and 7.x headers want gcc-15, which noble cannot supply. Both problems are
absent on resolute, so the role carries no workaround for either.

The version comes from `IONIC_KERNEL_REF` by default — one pin for the C build,
the guest DKMS build and the image — and the distro kernel is left installed as
a fallback. No reboot is issued: the golden image is shut down after this role
and its overlays boot into the new kernel. Set `ernic_image_kernel_reboot` when
running the role against a live guest.

Background upgrades are masked (`ernic_image_disable_unattended`). A test image
wants a frozen userspace: unattended-upgrades is what silently replaced the
source-built rdma-core on the CI guests and drifted their kernel packages apart.

It does **not** install the driver or the rdma-core provider — those track the
source tree and belong in `ernic_guest_setup`, which runs per overlay.

`rdma_setup` is wrapped in a `rescue`: it finishes with `rdma-detect`, which
exits non-zero when there is no real HCA. The packages are installed by then,
so the failure is logged and the play continues.

## Requirements

- Ubuntu resolute (26.04) when installing a mainline kernel, noble (24.04) or
  resolute otherwise
- `become: true`
- `sbates130272.batesste` >= 1.3.0

## Role Variables

```yaml
ernic_image_rdma: true          # run rdma_setup
ernic_gpu_passthrough: true     # run rocm_setup
ernic_image_packages:           # see defaults/main.yml for the full list
  - build-essential
  - cmake
  - dkms
  # ...
ernic_image_logind_keep_processes: true
ernic_image_modules_load: true
ernic_image_pciids: true
ernic_image_pciids_vendor: "1022"
# Both default to the ionic values; see defaults/main.yml.
ernic_image_boot_modules: [ionic, ionic_rdma]
ernic_image_pciids_entry: "8001  ROCm Emulated ionic RDMA NIC"
ernic_image_clean_apt: true
ernic_image_apt_retries: 3
ernic_image_apt_delay: 10

# Mainline kernel.  Defaults on in ionic mode, off in legacy.
ernic_image_kernel_mainline: "{{ ernic_device_mode != 'legacy' }}"
ernic_image_kernel_version: ""   # empty = derive from IONIC_KERNEL_REF
ernic_image_kernel_mainline_url: "https://kernel.ubuntu.com/mainline"
ernic_image_kernel_reboot: false
ernic_image_kernel_reboot_timeout: 600

ernic_image_disable_unattended: true
```

## Example

```yaml
- hosts: golden_image
  become: true
  roles:
    - role: sbates130272.rocm_ernic.ernic_image_prep
      vars:
        ernic_gpu_passthrough: false
```

## License

MIT
