# ernic_host_setup

Builds, installs, configures and starts the rocm-ernic service on a host, and
prepares the GPUs and rocm-xio that the guest VMs need from it.

## Overview

Phases, each behind a flag:

| Phase | Tasks | Flag |
|---|---|---|
| Build | cmake configure / build / install (server, `ernicctl`, systemd units) | `ernic_build` |
| Service | render `/etc/rocm-ernic/rocm-ernic.env`, reload systemd, tear down a previous run, `ernicctl start`, wait for sockets | `ernic_install_service` |
| vfio | IOMMU check, unbind from `amdgpu`, bind to `vfio-pci`, verify | `ernic_gpu_passthrough` |
| rocm-xio | clone and tar rocm-xio for the guests | `ernic_gpu_passthrough` |

The source tree comes from [`ernic_source`](../ernic_source/README.md), which
this role includes; `ernic_build_dir` defaults to `<source>/build`.

Guests have no route to GitHub, so rocm-xio is cloned here and staged at
`ernic_rocm_xio_tarball` for `ernic_guest_setup` to unpack.

## Requirements

- Ubuntu noble (24.04) or resolute (26.04)
- A build toolchain (cmake, ninja, a compiler) — the rocm-ernic build deps
- `become` for the install, systemd and vfio steps
- For passthrough: IOMMU enabled on the kernel command line
  (`amd_iommu=on` / `intel_iommu=on`)

## Role Variables

```yaml
ernic_build: true
ernic_install_service: true
ernic_start_service: true
ernic_restart_existing: true    # stop a previous mesh before starting

ernic_build_type: Release
ernic_install_prefix: /usr/local

ernic_instances: 2
ernic_tcp_port: 6320
ernic_manager_ip: 127.0.0.1
ernic_verbose: "false"
ernic_debug_mesh: true
ernic_debug_dma_map: false

ernic_gpu_passthrough: true
ernic_pci_mmio_bridge: true
ernic_gpu_pci_devices: {}       # {1: "0000:4a:00.0", 2: "0000:0f:00.0"}

ernic_rocm_xio_repo: "https://github.com/ROCm/rocm-xio.git"
ernic_rocm_xio_branch: "main"
ernic_rocm_xio_commit: ""       # pin for reproducibility
```

`ernic_gpu_pci_devices` empty means nothing is bound to vfio-pci — the role
says so and moves on. See `defaults/main.yml` for the QEMU/VM values written
into the env file.

## Example

```yaml
- hosts: localhost
  roles:
    - role: sbates130272.rocm_ernic.ernic_host_setup
      vars:
        ernic_source_dir: /home/me/Projects/rocm-ernic
        ernic_instances: 4
        ernic_gpu_passthrough: false
```

## License

MIT
