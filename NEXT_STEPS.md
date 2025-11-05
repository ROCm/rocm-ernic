# Next Steps for vfu-pvrdma Integration

## Current Status
- ✅ Excellent architecture designed and implemented
- ✅ Comprehensive documentation (IMPLEMENTATION_PLAN, INTEGRATION_STATUS,
PHASE1_STATUS, STUB_HEADERS_STATUS)
- ✅ Compatibility bridge layer created
- ✅ Server code updated with proper structure
- ⏳ Build blocked on QEMU header dependencies (70% resolved)

## Immediate Next Steps (4-6 hours of work)

### 1. Implement Wrapper Function Approach
Create clean API boundary to isolate QEMU headers.

**File: src/vfu_compat_bridge.h** (add these declarations):
```c
/* PVRDMA device management - opaque handle */
typedef void* pvrdma_dev_handle_t;

/* Device lifecycle */
pvrdma_dev_handle_t pvrdma_device_create(vfu_pvrdma_dev_t *vfu_dev,
                                         const char *ib_dev,
                                         const char *eth_dev,
                                         uint8_t port);
void pvrdma_device_destroy(pvrdma_dev_handle_t handle);

/* Register access */
void pvrdma_reg_write(pvrdma_dev_handle_t handle, uint64_t offset,
                      uint32_t val);
uint32_t pvrdma_reg_read(pvrdma_dev_handle_t handle, uint64_t offset);

/* UAR access */
void pvrdma_uar_write(pvrdma_dev_handle_t handle, uint64_t offset,
                      uint32_t val);
uint32_t pvrdma_uar_read(pvrdma_dev_handle_t handle, uint64_t offset);

/* DSR management */
int pvrdma_dsr_load(pvrdma_dev_handle_t handle, dma_addr_t dma_addr);

/* Command execution */
int pvrdma_exec_cmd(pvrdma_dev_handle_t handle);
```

**File: src/vfu_compat_bridge.c** (implement wrappers):
```c
/* This file CAN include QEMU headers */
#define VFU_PVRDMA_INTERNAL_IMPL
#include "vfu_pvrdma_internal.h"
#include "from-qemu/hw/rdma/vmw/pvrdma.h"

pvrdma_dev_handle_t pvrdma_device_create(vfu_pvrdma_dev_t *vfu_dev,
                                          const char *ib_dev,
                                          const char *eth_dev,
                                          uint8_t port)
{
    PVRDMADev *dev = calloc(1, sizeof(PVRDMADev));
    /* Initialize QEMU structure */
    dev->backend_device_name = strdup(ib_dev);
    dev->backend_eth_device_name = strdup(eth_dev);
    dev->backend_port_num = port;
    /* ... rest of init */
    return (pvrdma_dev_handle_t)dev;
}

void pvrdma_reg_write(pvrdma_dev_handle_t handle, uint64_t offset, uint32_t val)
{
    PVRDMADev *dev = (PVRDMADev *)handle;
    pvrdma_regs_write(dev, offset, val, sizeof(val));
}

/* ... implement rest of wrappers */
```

**File: src/vfu_pvrdma_internal.h** (remove QEMU includes):
```c
/* Forward declarations only - no QEMU headers! */
typedef struct vfu_pvrdma_dev {
    vfu_ctx_t *vfu_ctx;
    PCIDevice pci_dev;  /* Forward declared, defined elsewhere */
    
    void *pvrdma_handle;  /* Opaque handle to QEMU PVRDMA */
    
    /* BAR memory */
    void *bar0_mem;
    void *bar1_mem;
    void *bar2_mem;
    
    /* Configuration */
    char *backend_device_name;
    char *backend_eth_device;
    uint8_t backend_port_num;
    
    bool device_initialized;
    bool device_active;
    bool verbose;
} vfu_pvrdma_dev_t;
```

**File: src/vfu_pvrdma.c** (use wrapper API):
```c
/* Initialize PVRDMA device (in pvrdma_device_init) */
dev->pvrdma_handle = pvrdma_device_create(dev, 
                                          dev->backend_device_name,
                                          dev->backend_eth_device,
                                          dev->backend_port_num);

/* In BAR1 handler */
if (is_write) {
    pvrdma_reg_write(dev->pvrdma_handle, offset, val);
} else {
    val = pvrdma_reg_read(dev->pvrdma_handle, offset);
}
```

### 2. Minimize QEMU Header Includes
Only `.c` files that implement wrappers should include QEMU headers.

**Files that include QEMU headers**:
- `src/vfu_compat_bridge.c` - Wrapper implementations
- `src/from-qemu/hw/rdma/*.c` - QEMU source files

**Files that DON'T include QEMU headers**:
- `src/vfu_pvrdma.c` - Our main server
- `src/vfu_pvrdma_internal.h` - Device structure
- `src/vfu_compat_bridge.h` - Public API (opaque handles only)

### 3. Fix Remaining Stub Issues

**qemu/osdep.h conflict**:
The QEMU `.c` files include `qemu/osdep.h` which pulls in everything. Options:

A. **Remove osdep.h includes** (simplest):
```bash
# In all src/from-qemu/hw/rdma/*.c files:
# Comment out: #include "qemu/osdep.h"
# Add specific includes instead: #include <stdint.h>, <stdbool.h>, etc.
```

B. **Create minimal osdep.h stub**:
```c
/* src/from-qemu/include/qemu-extra/qemu/osdep.h */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "qemu/compiler.h"  /* Also stub this */
```

### 4. Test and Iterate
```bash
meson setup build
ninja -C build
# Fix any remaining issues
# Should compile cleanly within 2-3 iterations
```

## Medium Term (After Build Works)

1. **Initialize RDMA Backend**
   - Call `rdma_backend_init()`
   - Connect to libibverbs
   - Get device attributes

2. **Test DSR Protocol**
   - Start server
   - Attach with qemu (or write simple test client)
   - Verify DSR address written to register
   - Verify capabilities populated

3. **Implement Command Channel**
   - CREATE_PD, CREATE_QP, CREATE_CQ
   - REG_MR, DEREG_MR
   - Test with rdma-core tools

4. **Add UAR/Doorbell Handling**
   - Process QP send/recv doorbells
   - CQ arm/poll operations
   - Trigger MSI-X interrupts

## Testing Strategy

### Phase 1: Basic Enumeration
```bash
# Start server
./build/vfu_pvrdma -s /tmp/pvrdma.sock -d mlx5_0 -e eth0

# Test with qemu (in another terminal)
qemu-system-x86_64 \
  -device vfio-user-pci,socket=/tmp/pvrdma.sock \
  ...
  
# Check dmesg in guest for pvrdma driver loading
```

### Phase 2: RDMA Operations
```bash
# In guest VM
ibv_devinfo    # Should see pvrdma device
ibv_rc_pingpong -d pvrdma0 -g 0  # Simple RDMA test
```

### Phase 3: Real Workloads
```bash
# perftest suite
ib_send_bw -d pvrdma0
ib_read_lat -d pvrdma0

# rping (RDMA CM test)
rping -s -v  # In one VM
rping -c -a <ip> -v  # In another VM
```

## Files Summary

| File | Status | Purpose |
|------|--------|---------|
| IMPLEMENTATION_PLAN.md | ✅ Done | 5-phase roadmap with timeline |
| INTEGRATION_STATUS.md | ✅ Done | Progress tracking |
| PHASE1_STATUS.md | ✅ Done | Detailed Phase 1 status |
| STUB_HEADERS_STATUS.md | ✅ Done | Header dependency analysis |
| NEXT_STEPS.md | ✅ Done | This file |
| src/vfu_compat_bridge.h | ⏳ Needs wrapper API | Currently too coupled |
| src/vfu_compat_bridge.c | ⏳ Needs wrapper impl | Currently too coupled |
| src/vfu_pvrdma_internal.h | ⏳ Needs cleanup | Remove QEMU headers |
| src/vfu_pvrdma.c | ⏳ Needs update | Use wrapper API |
| src/from-qemu/include/qemu-extra/* | ⏳ Partial | Stub headers (70% done) |
| src/from-qemu/hw/rdma/*.c | ⏳ Minor edits | Remove osdep.h includes |

## Time Estimates

- Wrapper function implementation: 2-3 hours
- Header cleanup and osdep.h handling: 1-2 hours
- Build fixes and iteration: 1-2 hours
- **Total**: 4-7 hours

After this, Phase 1 will be complete with a working build, and we can move to
Phase 2 (DSR testing).

## Questions or Blockers?

If you encounter issues:
1. Check error messages carefully - usually missing types or includes
2. Add stubs incrementally for any missing types
3. Use `void*` liberally for opaque types
4. When in doubt, add another wrapper function

The architecture is solid. This is just wrapping up the implementation details.

