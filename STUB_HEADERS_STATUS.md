# QEMU Stub Headers Status

## Overview

This document describes the QEMU stub header creation attempt and the remaining
challenges.

## What We Created

We created stub headers in `src/from-qemu/include/qemu-extra/` for:

### Core Device Model
- `hw/pci/pci_device.h` - PCIDevice structure with our vfu_dev/vfu_ctx fields
- `hw/pci/msix.h` - MSI-X functions (no-ops, libvfio-user handles interrupts)
- `hw/qdev-core.h` - DeviceState and DeviceClass (minimal stubs)
- `hw/hw.h` - VMStateDescription (state save/load, not used)

### Memory & Threading
- `exec/memory.h` - MemoryRegion, AddressSpace, DMA types
- `qemu/thread.h` - QemuMutex/Cond wrappers around pthread

### Peripheral Devices
- `hw/net/vmxnet3_defs.h` - VMXNET3State (PVRDMA's optional func0 companion)
- `qemu/notify.h` - Notifier system (not used)
- `chardev/char-fe.h` - CharBackend for MAD interface (not used)

## The Problem

While these stubs handle the direct includes from PVRDMA headers, QEMU headers
have deep transitive dependencies:

### Dependency Chain Example
```
pvrdma.h
  ↓ includes hw/pci/pci_device.h (our stub)
  ↓ includes hw/net/vmxnet3_defs.h (our stub)
  ↓ includes rdma_backend_defs.h (QEMU original)
    ↓ includes qemu/thread.h (our stub)
    ↓ includes contrib/rdmacm-mux/rdmacm-mux.h (QEMU original)
      ↓ includes net/net.h (QEMU original - full networking stack!)
        ↓ includes build/qapi/qapi-types-net.h (generated QAPI types)
          ↓ includes qapi-builtin-types.h (QAPI type system)
            ↓ Uses G_DEFINE_AUTOPTR_CLEANUP_FUNC with qapi_free_* functions
              ↓ Conflicts with system headers (in6_addr, sockaddr_in6)
```

The issue compounds:
1. QEMU's networking headers conflict with system networking headers
2. QAPI (QEMU's type system) uses GLib macros that expect QEMU's build
environment
3. Generated headers reference functions that don't exist outside QEMU
4. Thread-safety annotations (`TSA_*`) need proper definitions

## Why It's Hard

1. **QAPI Code Generation**: QEMU generates headers during build with
definitions for networking, block devices, etc. We don't have this
infrastructure.

2. **Header Conflicts**: QEMU's `linux/in6.h` conflicts with system
`netinet/in.h`. These are both included through different paths.

3. **GLib Integration**: QEMU uses GLib heavily with custom macros
(`G_GNUC_PRINTF`, `G_DEFINE_AUTOPTR_CLEANUP_FUNC`) that expect specific
contexts.

4. **Deep Type System**: QEMU's QOM (QEMU Object Model) permeates everything
with macros like `OBJECT_DECLARE_TYPE`, `DECLARE_INSTANCE_CHECKER`, etc.

## Solutions (in order of preference)

### Option 1: Minimize QEMU Header Exposure (Recommended)
**Approach**: Don't include QEMU headers in our interface files at all.

**Implementation**:
1. Move `PVRDMADev` and QEMU types completely out of shared headers
2. Create a pure C interface in `vfu_compat_bridge.h`:
   ```c
   void* pvrdma_device_create(vfu_pvrdma_dev_t *vfu_dev);
   void pvrdma_regs_write_wrapper(void *pvrdma, uint64_t offset, uint32_t val);
   uint32_t pvrdma_regs_read_wrapper(void *pvrdma, uint64_t offset);
   ```
3. Implement these wrappers in a `.c` file that *does* include QEMU headers
4. Only QEMU `.c` files see QEMU headers; our code sees opaque pointers

**Effort**: 1 day
**Benefits**: Clean separation, minimal stub requirements
**Drawbacks**: Extra wrapper layer (minor performance impact)

### Option 2: Remove Problematic QEMU Includes
**Approach**: Modify QEMU source files to remove unnecessary includes.

**Implementation**:
1. `rdma_backend_defs.h` doesn't need `rdmacm-mux.h` (MAD handling not used)
2. Comment out `#include "contrib/rdmacm-mux/rdmacm-mux.h"` 
3. Stub out any MAD-related functions in `rdma_backend.c`
4. This breaks the dependency chain to networking headers

**Effort**: 1 day  
**Benefits**: Keeps most QEMU code unchanged
**Drawbacks**: Modifies QEMU source (acceptable since it's a port)

### Option 3: Comprehensive Stub Library
**Approach**: Create complete stubs for all transitively-included headers.

**Files needed**:
- `net/net.h`, `qapi/*.h` (dozens of files)
- All networking types and functions
- QAPI type system infrastructure
- Header conflict resolution (in6_addr, etc.)

**Effort**: 3-5 days
**Benefits**: Most "complete" solution
**Drawbacks**: Huge effort, maintenance burden

### Option 4: Link Against QEMU Libraries
**Approach**: Build QEMU as libraries, link against them.

**Implementation**:
1. Build QEMU with `--enable-modules`
2. Link against `libqemu-common.a`, `libqemuutil.a`
3. Pull in full QEMU runtime

**Effort**: 1 day
**Benefits**: Everything "just works"
**Drawbacks**: 
- Huge binary size
- Complex dependencies
- Overkill for our needs

## Recommendation

**Use Option 1** (Minimize Header Exposure) combined with **Option 2** (Remove
Problematic Includes).

### Concrete Steps:
1. Create wrapper functions in `vfu_compat_bridge.c`:
   - `void* vfu_pvrdma_backend_create(const char *dev_name, const char *eth_dev,
   uint8_t port)`
   - `int vfu_pvrdma_dsr_load(void *pvrdma, dma_addr_t addr)`
   - `int vfu_pvrdma_cmd_exec(void *pvrdma)`
   
2. Remove `#include "contrib/rdmacm-mux/rdmacm-mux.h"` from
`rdma_backend_defs.h`

3. Keep QEMU headers confined to QEMU `.c` files only

4. Use opaque pointers (`void*`) in our interface

This gives us:
- ✅ Clean build in 1-2 days
- ✅ No header pollution
- ✅ Minimal QEMU modifications
- ✅ Maintainable long-term

## Current Build Errors (for reference)

Last build attempt showed:
- `error: field 'qdev' has incomplete type` - DeviceState not fully defined
- `error: redefinition of 'struct in6_addr'` - Header conflicts
- `error: expected declaration specifiers before 'G_GNUC_PRINTF'` - GLib macro
issues
- `error: expected '...' before 'qapi_free_strList'` - Missing QAPI
infrastructure

These are all solvable with Option 1+2 approach.

## Files to Modify (Option 1+2)

1. `src/vfu_compat_bridge.h` - Add wrapper function declarations
2. `src/vfu_compat_bridge.c` - Implement wrappers (includes QEMU headers)
3. `src/vfu_pvrdma_internal.h` - Remove QEMU header includes, use opaque
pointers
4. `src/vfu_pvrdma.c` - Use wrapper functions instead of direct QEMU calls
5. `src/from-qemu/hw/rdma/rdma_backend_defs.h` - Comment out rdmacm-mux include

**Estimated time**: 4-6 hours of focused work.

## Conclusion

We have a clear path forward. The architecture is excellent. The design is
sound. We just need to create a proper abstraction layer to hide QEMU's header
complexity from our code.

The stub headers we created are useful as a starting point and document what
types QEMU code needs. With the wrapper approach, we'll need fewer stubs and
have a cleaner result.

