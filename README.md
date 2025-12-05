# ROCm ERNIC: Emulated RDMA NIC for Virtual Machines

This project contains a userspace implementation of an RDMA (Remote Direct
Memory Access) device using the [libvfio-user][ref-libvfio] framework.
It enables RDMA functionality for virtual machines without requiring
actual RDMA hardware or relying on an in-guest framework like
[soft RoCE][ref-softroce].

## Overview

This project implements a fully functional RDMA device that can be attached to
virtual machines (VMs) via the VFIO (Virtual Function I/O) user-space device
framework. The device provides RDMA (Remote Direct Memory Access) capabilities
to guest VMs via a number of different backends.

This project is intended to aid in RDMA-related software development without
needing actual RDMA hardware. However it can also be used for CI and, possibly,
in production via the RDMA backend.

## Key Features

  - Full PCIe device emulation in userspace
  - Three memory-mapped BARs
  - MSI-X interrupt support (command ring, async events, completion queue)
  - Compatible with the Linux kernel `rocm_ernic` driver which is available in
    the [driver](./driver) directory.

# Architecture

```
┌──────────────────────────────────────────────┐
│          Virtual Machine (Guest)             │
│  ┌────────────────────────────────────┐      │
│  │   Linux Kernel rocm_ernic Driver   │      │
│  └─────────────┬──────────────────────┘      │
│                │ PCI Interface               │
└────────────────┼─────────────────────────────┘
                 │ VFIO-User Protocol
┌────────────────┼─────────────────────────────┐
│                ▼                             │
│  ┌──────────────────────────────────────┐    │
│  │    rocm_ernic Server                 │    │
│  │  (This Project)                      │    │
│  │                                      │    │
│  │  ┌───────────────────────┐           │    │
│  │  │  RDMA Device Logic    │           │    │
│  │  │  (adapted from QEMU)  │           │    │
│  │  └──────────┬────────────┘           │    │
│  │             │                        │    │
│  │    ┌────────┴────────┐──────┐        │    │
│  │    │                 │      │        │    │
│  │    ▼                 ▼      ▼        │    │
│  │  ┌──────┐  ┌──────────┐  ┌──────┐    │    │
│  │  │Loop- │  │ TCP/IP   │  │RDMA/ │    │    │
│  │  │back  │  │ Backend  │  │Verbs │    │    │
│  │  │      │  │          │  │      │    │    │
│  │  │In-   │  │TCP Socket│  │      │    │    │
│  │  │Memory│  │Protocol  │  │      │    │    │
│  │  │Emul. │  │          │  │      │    │    │
│  │  └──────┘  └────┬─────┘  └──┬───┘    │    │
│  └─────────────────┼───────────┼────────┘    │
│                    │           │             │
│     Host (Userspace/Kernel)    │             │
│                    │           │             │
│         ┌──────────┘           │             │
│         │                      │             │
│         ▼                      ▼             │
│  ┌──────────────┐    ┌──────────────┐        │
│  │Another       │    │  libibverbs  │        │
│  │rocm_ernic    │    └──────┬───────┘        │
│  │Server        │           │                │
│  │(Remote VM)   │           ▼                │
│  └──────────────┘    ┌──────────────┐        │
│                      │ InfiniBand   │        │
│                      │ Hardware     │        │
│                      │              │        │
│                      └──────────────┘        │
└──────────────────────────────────────────────┘
```

# Building and Installing

## Dependencies

```bash
# Ubuntu/Debian
sudo apt install meson ninja-build libibverbs-dev librdmacm-dev libglib2.0-dev

# Build and install libvfio-user (if not already installed)
cd /path/to/libvfio-user
mkdir build && cd build
cmake ..
make && sudo make install
```

## Compilation

```bash
# From the project root directory
meson setup build
ninja -C build

# The executable will be at: build/rocm_ernic
```

## Installation

```bash
sudo ninja -C build install
# Installs to /usr/local/bin/rocm_ernic by default
```

# Usage

## Basic Usage

```bash
# Start the ROCm ERNIC device server with verbs backend
./build/rocm_ernic --socket /tmp/vfio-user-rocm-ernic.sock \
                   --backend verbs:device=mlx5_0,ethdev=eth0,port=1 \
                   --verbose

# Start with loopback backend (for testing)
./build/rocm_ernic --socket /tmp/vfio-user-rocm-ernic.sock \
                   --backend loopback \
                   --verbose

# Start with no backend (minimal stubs)
./build/rocm_ernic --socket /tmp/vfio-user-rocm-ernic.sock \
                   --backend none
```

# Acknowledgments

## Original QEMU PVRDMA Implementation

- Yuval Shaia <yuval.shaia@oracle.com> (Oracle)
- Marcel Apfelbaum <marcel@redhat.com> (Red Hat)

[ref-libvfio]: https://github.com/nutanix/libvfio-user
[ref-softroce]: https://man7.org/linux/man-pages/man7/rxe.7.html
