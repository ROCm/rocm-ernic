# ernic_image_prep

Bakes the RDMA, ROCm and rocm-ernic prerequisites into a base Ubuntu image so
that per-VM overlays do not have to re-download them on every test cycle.

## Overview

Apply this role to a freshly booted Ubuntu image (a golden qcow2 booted as a
temporary VM, a cloud instance, a Packer build). It installs:

- RDMA userspace and tooling via `sbates130272.batesste.rdma_setup`
- the build and test packages `ernic_guest_setup` needs (cmake, ninja, dkms,
  `libcli11-dev`, iperf3, …)
- the ROCm stack via `sbates130272.batesste.rocm_setup`, when
  `ernic_gpu_passthrough` is true

and applies the static configuration that has to be present before first boot:
`KillUserProcesses=no`, `modules-load.d` entries for `rocm_ernic_eth` /
`rocm_ernic_rdma` / `rocm-xio`, and a `pci.ids` entry so `lspci` names the
emulated NIC.

It does **not** install the driver or the rdma-core provider — those track the
source tree and belong in `ernic_guest_setup`, which runs per overlay.

`rdma_setup` is wrapped in a `rescue`: it finishes with `rdma-detect`, which
exits non-zero when there is no real HCA. The packages are installed by then,
so the failure is logged and the play continues.

## Requirements

- Ubuntu noble (24.04) or resolute (26.04)
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
ernic_image_pciids_entry: "8000  ROCm Emulated RDMA NIC"
ernic_image_clean_apt: true
ernic_image_apt_retries: 3
ernic_image_apt_delay: 10
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
