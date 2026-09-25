# rocm-ernic

[![License](https://img.shields.io/badge/License-MIT%20%7C%20GPL--2.0--or--later-blue.svg)][license]
[![Build](https://github.com/ROCm/rocm-ernic/actions/workflows/build-and-test.yml/badge.svg)][ci-build]
[![Docs](https://github.com/ROCm/rocm-ernic/actions/workflows/docs-check.yml/badge.svg)][ci-docs]
[![clang-format](https://github.com/ROCm/rocm-ernic/actions/workflows/clang-format.yml/badge.svg)][ci-clang-format]
[![ShellCheck](https://github.com/ROCm/rocm-ernic/actions/workflows/shellcheck.yml/badge.svg)][ci-shellcheck]
[![pylint](https://github.com/ROCm/rocm-ernic/actions/workflows/pylint.yml/badge.svg)][ci-pylint]
[![cmakelint](https://github.com/ROCm/rocm-ernic/actions/workflows/cmakelint.yml/badge.svg)][ci-cmakelint]
[![System Tests](https://github.com/ROCm/rocm-ernic/actions/workflows/system-tests.yml/badge.svg)][ci-system-tests]
[![Platform](https://img.shields.io/badge/platform-linux-lightgrey.svg)](INSTALL.md)
[![CI guest kernel](https://img.shields.io/badge/CI%20guest%20kernel-7.2.3-blue.svg)][guest-kernel]
[![RDMA bandwidth](https://img.shields.io/endpoint?url=https%3A%2F%2Frocm.github.io%2Frocm-ernic%2Fperf%2Fbadge-rdma.json)][perf-trends]
[![TCP/IP bandwidth](https://img.shields.io/endpoint?url=https%3A%2F%2Frocm.github.io%2Frocm-ernic%2Fperf%2Fbadge-tcp.json)][perf-trends]
[![NVMe-oF 4K read](https://img.shields.io/endpoint?url=https%3A%2F%2Frocm.github.io%2Frocm-ernic%2Fperf%2Fbadge-nvmeof.json)][perf-trends]
[![S3 1M GET](https://img.shields.io/endpoint?url=https%3A%2F%2Frocm.github.io%2Frocm-ernic%2Fperf%2Fbadge-s3.json)][perf-trends]

> [!CAUTION]
> This release is an *early-access* software technology preview. Running
> production workloads is *not* recommended.

Userspace emulated RDMA NIC for virtual machines, built on
[libvfio-user][libvfio]. Provides full RDMA functionality to guest VMs without
requiring physical RDMA hardware or an in-guest software stack such as
[Soft-RoCE][softroce]. Backends include loopback (for testing and CI), TCP/IP
(multi-node without hardware), native verbs (real InfiniBand HCA
pass-through), and nvmeof (an in-process NVMe over Fabrics target, so a single
VM and a single server instance are a complete fabric — see
[`docs/nvmeof.rst`](docs/nvmeof.rst)), and s3 (an in-process S3-over-RDMA
object store with its own in-band HTTP endpoint, so the same single VM is a
complete object fabric — see [`docs/s3.rst`](docs/s3.rst)).

The server emulates an AMD Pensando ionic NIC (`1dd8:100a`), so the guest runs
the upstream Linux `ionic` and `ionic_rdma` drivers with only the small
device-ID and UC address-handle patches in [`patches/`](patches/) applied, and
the upstream `providers/ionic` in rdma-core. `--tap IFNAME` attaches the
emulated Ethernet interface to a host TAP, giving the guest a real routable
segment with working ARP, ICMP, and TCP/IP. See
[`docs/ionic.rst`](docs/ionic.rst) for details.

## Installing and Using rocm-ernic

See [INSTALL.md](INSTALL.md) for dependencies, supported platforms, and build
instructions.

## Releases

Project releases are published from `vX.Y.Z` tags and attach GitHub release
assets for the `rocm-ernic` server and `ernicctl`; see the
[Releases page](https://github.com/ROCm/rocm-ernic/releases).

The Ansible collection is published separately from `ansible-vX.Y.Z` tags and
uses the existing Galaxy release flow under [`ansible/`](ansible/).

## Documentation

Full documentation lives in the [`docs/`](docs/) directory and covers building,
architecture, usage, the kernel driver, the systemd service, testing, and the
API reference.

## License

[MIT](LICENSE.md) for the build system, documentation, and the deployment and automation code;
everything under `src/` and `tests/` is `GPL-2.0-or-later` unless its per-file
SPDX declaration says otherwise. The per-file SPDX header is authoritative; see
[LICENSE.md](LICENSE.md) for details.

<!-- References -->

[license]: https://github.com/ROCm/rocm-ernic/blob/develop/LICENSE.md
[ci-build]: https://github.com/ROCm/rocm-ernic/actions/workflows/build-and-test.yml
[ci-docs]: https://github.com/ROCm/rocm-ernic/actions/workflows/docs-check.yml
[ci-clang-format]: https://github.com/ROCm/rocm-ernic/actions/workflows/clang-format.yml
[ci-shellcheck]: https://github.com/ROCm/rocm-ernic/actions/workflows/shellcheck.yml
[ci-pylint]: https://github.com/ROCm/rocm-ernic/actions/workflows/pylint.yml
[ci-cmakelint]: https://github.com/ROCm/rocm-ernic/actions/workflows/cmakelint.yml
[ci-codeql]: https://github.com/ROCm/rocm-ernic/actions/workflows/codeql.yml
[ci-system-tests]: https://github.com/ROCm/rocm-ernic/actions/workflows/system-tests.yml
[guest-kernel]: https://github.com/ROCm/rocm-ernic/blob/develop/docs/performance.rst
[perf-trends]: https://rocm.github.io/rocm-ernic/perf-trends.html
[libvfio]: https://github.com/nutanix/libvfio-user
[softroce]: https://man7.org/linux/man-pages/man7/rxe.7.html
