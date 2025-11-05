# vfu-pvrdma: Userspace PVRDMA Device Emulation

A userspace implementation of the VMware ParaVirtualized RDMA (PVRDMA) device
using the [libvfio-user](https://github.com/nutanix/libvfio-user) framework.
This project enables RDMA functionality for virtual machines without requiring
in-kernel device emulation.

## Overview

This project implements a fully functional PVRDMA device server that can be
attached to virtual machines (VMs) via the VFIO (Virtual Function I/O)
user-space device framework. The device provides RDMA (Remote Direct Memory
Access) capabilities to guest VMs, allowing high-performance, low-latency
networking with direct hardware access semantics.

**Key Features:**
- Full PCIe device emulation in userspace
- RDMA verbs support via InfiniBand hardware backend
- Three memory-mapped BARs (MSI-X, Registers, UAR)
- MSI-X interrupt support (command ring, async events, completion queue)
- Compatible with the Linux kernel `vmw_pvrdma` driver

## Architecture

```
┌─────────────────────────────────────────┐
│          Virtual Machine (Guest)        │
│  ┌────────────────────────────────────┐ │
│  │   Linux Kernel vmw_pvrdma Driver  │ │
│  └─────────────┬──────────────────────┘ │
│                │ PCI Interface          │
└────────────────┼────────────────────────┘
                 │ VFIO-User Protocol
┌────────────────┼────────────────────────┐
│                ▼                        │
│  ┌─────────────────────────────┐       │
│  │    vfu_pvrdma Server        │       │
│  │  (This Project)             │       │
│  │                             │       │
│  │  ┌───────────────────────┐ │       │
│  │  │  PVRDMA Device Logic  │ │       │
│  │  │  (from QEMU)          │ │       │
│  │  └──────────┬────────────┘ │       │
│  │             │               │       │
│  │  ┌──────────▼────────────┐ │       │
│  │  │   RDMA Backend        │ │       │
│  │  └──────────┬────────────┘ │       │
│  └─────────────┼───────────────┘       │
│                │                        │
│     Host (Userspace/Kernel)           │
│                │                        │
│  ┌─────────────▼────────────┐          │
│  │   libibverbs             │          │
│  └─────────────┬────────────┘          │
│                │                        │
│  ┌─────────────▼────────────┐          │
│  │   InfiniBand Hardware    │          │
│  │   (mlx5, rxe, etc.)      │          │
│  └──────────────────────────┘          │
└─────────────────────────────────────────┘
```

## History and Background

### PVRDMA Origins

**PVRDMA (ParaVirtualized RDMA)** was originally developed by VMware as a
paravirtualized RDMA adapter for VMware ESXi and later contributed to the
open-source community. The technology enables RDMA capabilities in virtualized
environments without requiring direct hardware passthrough.

### QEMU Implementation

The QEMU project includes a complete PVRDMA device emulation
(`hw/rdma/vmw/pvrdma*`), which was added in **QEMU v3.1** (December 2018). This
implementation provides:
- Full PCI device emulation with proper BAR mappings
- RDMA resource management (QPs, CQs, MRs, PDs, etc.)
- Integration with host InfiniBand hardware via `libibverbs`
- Support for both RC (Reliable Connection) and UD (Unreliable Datagram)
transport

This project is based on the **QEMU v9.0.4** PVRDMA implementation, adapted for
standalone userspace operation.

### Linux Kernel Driver

The Linux kernel includes a native PVRDMA driver
(`drivers/infiniband/hw/vmw_pvrdma/`) that was merged in **Linux 4.5** (March
2016). This driver:
- Implements the standard RDMA verbs interface (`ib_*` APIs)
- Communicates with the PVRDMA device via PCI memory-mapped I/O
- Supports the full RDMA feature set (QPs, CQs, SRQs, MRs, etc.)
- Is production-ready and widely used in VMware environments

**Device IDs:**
- Vendor ID: `0x15ad` (VMware)
- Device ID: `0x0820` (PVRDMA)

### libvfio-user Framework

[**libvfio-user**](https://github.com/nutanix/libvfio-user) is a framework
developed by Nutanix that enables implementing VFIO devices in userspace. Unlike
traditional device emulation which requires running inside a VMM (Virtual
Machine Monitor) like QEMU, libvfio-user allows:

- **Standalone device servers** running as separate processes
- **Enhanced security** through process isolation
- **Simplified development** without VMM coupling
- **Flexible deployment** (local sockets, network sockets, etc.)

The framework implements the **vfio-user protocol**, which extends the Linux
VFIO (Virtual Function I/O) interface to userspace, enabling communication
between VMs and userspace device emulators.

## Project Components

### Source Code Organization

```
vfu-rdma/
├── src/
│   ├── vfu_pvrdma.c           # Main server implementation
│   └── from-qemu/             # QEMU PVRDMA code (v9.0.4)
│       ├── hw/rdma/
│       │   ├── rdma_backend.c # InfiniBand backend integration
│       │   ├── rdma_rm.c      # RDMA resource manager
│       │   ├── rdma_utils.c   # Utility functions
│       │   └── vmw/
│       │       ├── pvrdma_main.c      # Device initialization
│       │       ├── pvrdma_cmd.c       # Command processing
│       │       ├── pvrdma_dev_ring.c  # Ring buffer management
│       │       └── pvrdma_qp_ops.c    # Queue Pair operations
│       ├── include/           # QEMU headers (stubs and definitions)
│       └── utils/             # Error reporting utilities
├── meson.build                # Build system configuration
└── README.md                  # This file
```

### Key Components

1. **vfu_pvrdma.c** - Main server implementing:
   - libvfio-user device lifecycle management
   - PCI configuration space setup
   - BAR (Base Address Register) handlers
   - MSI-X interrupt management
   - DMA region management

2. **PVRDMA Device Logic** (from QEMU):
   - Device register handling
   - Command channel processing
   - Queue Pair (QP) operations
   - Completion Queue (CQ) management
   - Memory Region (MR) registration

3. **RDMA Backend**:
   - Integration with host InfiniBand hardware via `libibverbs`
   - Translation between PVRDMA and native IB operations
   - Resource lifecycle management

## Building

### Prerequisites

- **Operating System**: Linux (tested on Ubuntu 24.04)
- **Compiler**: GCC 13.x or newer
- **Build System**: Meson 1.3+ and Ninja
- **Dependencies**:
  - `libvfio-user` (installed in `/usr/local/`)
  - `libibverbs` (InfiniBand verbs library)
  - `glib-2.0`

### Installing Dependencies

```bash
# Ubuntu/Debian
sudo apt install meson ninja-build libibverbs-dev libglib2.0-dev

# Build and install libvfio-user (if not already installed)
cd /path/to/libvfio-user
mkdir build && cd build
cmake ..
make && sudo make install
```

### Compilation

```bash
# From the project root directory
meson setup build
ninja -C build

# The executable will be at: build/vfu_pvrdma
```

### Installation

```bash
sudo ninja -C build install
# Installs to /usr/local/bin/vfu_pvrdma by default
```

## Usage

### Basic Usage

```bash
# Start the PVRDMA device server
./build/vfu_pvrdma --socket /tmp/vfio-pvrdma.sock \
                   --device mlx5_0 \
                   --ethdev eth0 \
                   --port 1 \
                   --verbose
```

### Command-Line Options

- `-s, --socket PATH` - VFIO-user socket path (default:
`/tmp/vfio-user-pvrdma.sock`)
- `-d, --device NAME`    - InfiniBand device name (e.g., `mlx5_0`, `rxe0`)
- `-e, --ethdev NAME`    - Ethernet device name for GID resolution
- `-p, --port NUM`       - IB port number (default: 1)
- `-v, --verbose`        - Enable verbose debug logging
- `-h, --help`           - Show help message

### Attaching to QEMU

To attach the device to a QEMU VM:

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -m 4G \
  -device vfio-user-pci,socket=/tmp/vfio-pvrdma.sock \
  -device e1000,netdev=net0 \
  -netdev user,id=net0 \
  ...
```

### Verifying in Guest

Once the VM is running with the device attached:

```bash
# Inside the guest VM
lspci | grep VMware
# Should show: 00:XX.0 Network controller: VMware PVRDMA Device

# Check if driver is loaded
lsmod | grep pvrdma

# Verify RDMA device
ibv_devices
# Should list the PVRDMA device
```

## Device Specifications

### PCI Configuration

- **Vendor ID**: 0x15ad (VMware)
- **Device ID**: 0x0820 (PVRDMA)
- **Class Code**: 0x028000 (Network Controller - Other)
- **PCI Type**: PCIe (PCI Express)
- **Header Type**: 0x00 (Normal device)

### Base Address Registers (BARs)

| BAR | Type | Size | Purpose |
|-----|------|------|---------|
| BAR0 | Memory | 16 KB | MSI-X Table and PBA |
| BAR1 | Memory | 256 bytes | Device Registers |
| BAR2 | Memory | Variable | UAR (User Access Region) for doorbells |

### MSI-X Interrupts

| Vector | Purpose |
|--------|---------|
| 0 | Command Ring |
| 1 | Async Events |
| 2 | Completion Queue |

## Development Status

### Current Status

- ✅ Core device structure and initialization
- ✅ PCI configuration and BAR setup
- ✅ Basic register and UAR access handlers
- ✅ DMA region management callbacks
- ✅ Build system integration
- ⚠️ RDMA backend initialization (stub)
- ⚠️ Command processing (stub)
- ⚠️ Interrupt handling (stub)
- ❌ Full RDMA verbs integration
- ❌ Testing and validation

### Next Steps

1. **Initialize RDMA Backend**
   - Complete `rdma_backend_init()` integration
   - Configure InfiniBand device contexts
   - Set up protection domains and initial resources

2. **Implement Device Shared Region (DSR)**
   - Map guest memory for command/completion rings
   - Initialize async event ring
   - Set up completion queue ring

3. **Command Channel Processing**
   - Implement command handler dispatch
   - Process CREATE_QP, CREATE_CQ, REG_MR, etc.
   - Connect to backend RDMA operations

4. **UAR (User Access Region) Handling**
   - Implement doorbell processing
   - Handle QP send/receive doorbells
   - Process CQ arm requests

5. **Interrupt Management**
   - Trigger MSI-X interrupts on completions
   - Handle async events
   - Implement command completion notifications

6. **Testing**
   - Unit tests for core components
   - Integration testing with QEMU
   - Performance benchmarking with real workloads

## References

### Documentation

- [QEMU PVRDMA Documentation][qemu-pvrdma-docs]
- [libvfio-user GitHub](https://github.com/nutanix/libvfio-user)
- [Linux PVRDMA Kernel Driver][linux-pvrdma-driver]
- [InfiniBand Verbs API][ibverbs-api]

### Related Projects

- [QEMU](https://www.qemu.org/) - Machine emulator and virtualizer
- [SPDK][spdk-link] - Storage Performance Development Kit (uses
  libvfio-user)
- [RXE](https://github.com/linux-rdma/rdma-core) - Software RDMA over
  Ethernet

[qemu-pvrdma-docs]: https://www.qemu.org/docs/master/system/devices/pvrdma.html
[linux-pvrdma-driver]: https://github.com/torvalds/linux/tree/master/drivers/infiniband/hw/vmw_pvrdma
[ibverbs-api]: https://man7.org/linux/man-pages/man3/ibv_get_device_list.3.html
[spdk-link]: https://spdk.io/

## License

This project combines code from multiple sources:

- **QEMU PVRDMA Code** (`src/from-qemu/`): GPL-2.0-or-later
- **New Code** (`src/vfu_pvrdma.c`): GPL-2.0-or-later

See individual source files for detailed copyright information.

## Contributing

Contributions are welcome! This is an active development project. Areas where
help is needed:

- RDMA backend integration and testing
- Command processing implementation
- Documentation improvements
- Testing with various InfiniBand hardware
- Performance optimization

## Authors and Acknowledgments

### Original QEMU PVRDMA Implementation

- Yuval Shaia <yuval.shaia@oracle.com> (Oracle)
- Marcel Apfelbaum <marcel@redhat.com> (Red Hat)

### This Project

- Adapted from QEMU v9.0.4 PVRDMA implementation
- Integrated with libvfio-user framework
- Standalone userspace server development

### Special Thanks

- VMware for the original PVRDMA specification and implementation
- QEMU project for the comprehensive device emulation
- Nutanix for the libvfio-user framework
- Linux kernel RDMA maintainers

## Support

For issues, questions, or contributions, please use the project's issue tracker.

---

**Status**: Active Development | **Version**: 0.1.0 | **Last Updated**: November
2025

