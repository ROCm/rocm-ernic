# rocm-ernic

[![MIT](https://img.shields.io/badge/License-MIT-blue.svg)][license]
[![Build](https://github.com/ROCm/rocm-ernic/actions/workflows/build-test.yml/badge.svg)][ci-build]
[![Docs](https://github.com/ROCm/rocm-ernic/actions/workflows/docs-check.yml/badge.svg)][ci-docs]
[![Lint](https://github.com/ROCm/rocm-ernic/actions/workflows/lint.yml/badge.svg)][ci-lint]
[![Spelling](https://github.com/ROCm/rocm-ernic/actions/workflows/spell-check.yml/badge.svg)][ci-spell]
[![Platform](https://img.shields.io/badge/platform-linux-lightgrey.svg)](INSTALL.md)

> [!CAUTION]
> This release is an *early-access* software technology preview. Running
> production workloads is *not* recommended.

Userspace emulated RDMA NIC for virtual machines, built on
[libvfio-user][libvfio]. Provides full RDMA functionality to guest VMs without
requiring physical RDMA hardware or an in-guest software stack such as
[Soft-RoCE][softroce]. Backends include loopback (for testing and CI), TCP/IP
(multi-node without hardware), and native verbs (real InfiniBand HCA
pass-through).

## Installing and Using rocm-ernic

See [INSTALL.md](INSTALL.md) for dependencies, supported platforms, and build
instructions.

## Documentation

Full documentation lives in the [`docs/`](docs/) directory and covers building,
architecture, usage, the kernel driver, the systemd service, testing, and the
API reference.

## License

[MIT](LICENSE.md). Some files carry different licenses per their SPDX headers;
see [LICENSE.md](LICENSE.md) for details.

<!-- References -->

[license]: https://github.com/ROCm/rocm-ernic/blob/main/LICENSE.md
[ci-build]: https://github.com/ROCm/rocm-ernic/actions/workflows/build-test.yml
[ci-docs]: https://github.com/ROCm/rocm-ernic/actions/workflows/docs-check.yml
[ci-lint]: https://github.com/ROCm/rocm-ernic/actions/workflows/lint.yml
[ci-spell]: https://github.com/ROCm/rocm-ernic/actions/workflows/spell-check.yml
[libvfio]: https://github.com/nutanix/libvfio-user
[softroce]: https://man7.org/linux/man-pages/man7/rxe.7.html
