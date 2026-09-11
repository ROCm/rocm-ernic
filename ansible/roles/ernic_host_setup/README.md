# ernic_host_setup

Builds, installs, configures and starts the rocm-ernic service on a host, and
prepares the GPUs and rocm-xio that the guest VMs need from it.

## Overview

Phases, each behind a flag:

| Phase | Tasks | Flag |
|---|---|---|
| Build | cmake configure / build / install (server, `ernicctl`, systemd units) | `ernic_build` |
| TAP | create `ernic_tap_bridge` and one TAP per instance, enslave, verify | `ernic_tap_setup` (ionic only) |
| Service | render `/etc/rocm-ernic/rocm-ernic.env`, reload systemd, tear down a previous run, `ernicctl start`, wait for sockets | `ernic_install_service` |
| vfio | IOMMU check, unbind from `amdgpu`, bind to `vfio-pci`, verify | `ernic_gpu_passthrough` |
| rocm-xio | clone and tar rocm-xio for the guests | `ernic_gpu_passthrough` |

The source tree comes from [`ernic_source`](../ernic_source/README.md), which
this role includes; `ernic_build_dir` defaults to `<source>/build`.

rocm-xio is cloned here and staged at `ernic_rocm_xio_tarball`;
`ernic_guest_setup` unpacks it when it is there and clones for itself when it
is not.

## Device mode and TAP networking

`ernic_device_mode` defaults to `ionic`, so the server presents `1022:8001`
and each instance attaches to a host TAP. The deprecated legacy PVRDMA path
is still reachable with `-e ernic_device_mode=legacy`.

In ionic mode guest Ethernet leaves through the TAP rather than the rocm-ernic
TCP mesh, so every TAP is enslaved to a shared bridge — otherwise the guests
cannot reach each other on `ernic_nic_subnet` and every two-VM test fails.
The TAP phase creates `ernic_tap_bridge` and `ernic_tap_prefix<n>` for
`n` in `1..ernic_instances`, owned by `ernic_tap_owner` so an unprivileged
launcher can open them. It runs before the service starts, because the
launcher only attaches an instance to a TAP that already exists.

Set `ernic_tap_setup: false` when the interfaces are provisioned some other
way (the CI runner creates them once at install time), and
`ernic_tap_bridge_ip` to give the host an address on the segment for
debugging.

## Requirements

- Ubuntu noble (24.04) or resolute (26.04)
- A build toolchain (cmake, ninja, a compiler) — the rocm-ernic build deps
- `become` for the install, systemd and vfio steps
- For passthrough: IOMMU enabled on the kernel command line
  (`amd_iommu=on` / `intel_iommu=on`)

## Role Variables

```yaml
ernic_device_mode: ionic        # ionic | legacy (deprecated)

ernic_tap_setup: true           # ionic only
ernic_tap_prefix: ernic-tap
ernic_tap_bridge: ernicbr0
ernic_tap_owner: "{{ ansible_user_id }}"
ernic_tap_bridge_ip: ""         # e.g. 192.168.200.1/24

ernic_build: true
ernic_install_service: true
ernic_start_service: true
ernic_restart_existing: true    # stop a previous mesh before starting

ernic_build_type: Release
ernic_install_prefix: /usr/local

ernic_instances: 2
ernic_tcp_port: 6320
ernic_manager_ip: 127.0.0.1
ernic_log_level: warn           # none|error|warn|info|debug
ernic_verbose: "false"          # legacy shorthand for debug
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
