# RDMA Command Implementation Status

**Date**: November 7, 2025

## Purpose

This document tracks the no-backend support status for each RDMA command handler. Following the pattern established with `query_port` and `create_pd`, each command needs to check for `backend_dev->context` before calling backend functions.

---

## ✅ Fixed for No-Backend Mode

### `query_port` (Command 0)
**Status**: ✅ **Working**  
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Fix**: Returns default port attributes when `backend_dev->context` is NULL  
**Test**: Confirmed working in VM

### `query_pkey` (Command 1)
**Status**: ✅ **Working**  
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Fix**: Returns static PKEY value (no backend call needed)  
**Test**: Confirmed working in VM

### `create_pd` (Command 2)
**Status**: ✅ **Fixed** (needs testing)  
**File**: `src/from-qemu/hw/rdma/rdma_rm.c`  
**Fix**: 
```c
if (backend_dev && backend_dev->context) {
    ret = rdma_backend_create_pd(backend_dev, &pd->backend_pd);
    ...
} else {
    /* No backend - just allocate the PD handle for tracking */
    memset(&pd->backend_pd, 0, sizeof(pd->backend_pd));
}
```
**Test**: Pending (VM kernel module issues)

---

## ⚠️ Needs Review for No-Backend Mode

The following commands **likely need similar fixes** as they call backend functions:

### `destroy_pd` (Command 3)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_dealloc_pd()` → `rdma_backend_destroy_pd()`  
**Action needed**: Add NULL check before calling backend

### `create_mr` (Command 4)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_alloc_mr()` → likely calls `rdma_backend_create_mr()`  
**Action needed**: Check `rdma_rm_alloc_mr()` implementation

### `destroy_mr` (Command 5)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_dealloc_mr()` → likely calls backend  
**Action needed**: Check `rdma_rm_dealloc_mr()` implementation

### `create_cq` (Command 6)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_alloc_cq()` → likely calls `rdma_backend_create_cq()`  
**Action needed**: Similar pattern to `create_pd`

### `destroy_cq` (Command 8)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_dealloc_cq()` → likely calls backend  
**Action needed**: Check for backend cleanup

### `create_qp` (Command 9)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_alloc_qp()` → likely calls `rdma_backend_create_qp()`  
**Action needed**: Similar pattern to `create_pd`

### `modify_qp` (Command 10)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_modify_qp()` → likely calls backend  
**Action needed**: State transitions need handling

### `query_qp` (Command 11)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: Backend query  
**Action needed**: Return default QP attributes

### `destroy_qp` (Command 12)
**File**: `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`  
**Calls**: `rdma_rm_dealloc_qp()` → backend cleanup  
**Action needed**: Check for backend cleanup

---

## 📋 Testing Strategy

### Current Blocker
The VM kernel doesn't export the needed IB symbols (`ib_umem_release`, `ib_register_device`, etc.) even though `ib_core` is loaded. This prevents the driver from loading.

### Options to Proceed

#### Option 1: Fix VM Kernel (Recommended)
- Use a kernel with `CONFIG_INFINIBAND_USER_ACCESS` properly configured
- Ensure IB symbols are exported
- Boot into that kernel

#### Option 2: Unit Testing (Alternative)
- Create a simple test harness that calls command handlers directly
- Mock the DSR and command structures
- Test each handler in isolation without needing the full driver

#### Option 3: Incremental Fixes (Current)
- Fix each handler as we discover issues
- Use server logs to trace crashes/hangs
- Pattern: If it crashes/hangs without backend, add NULL check

---

## Pattern for No-Backend Fixes

**Standard Pattern**:
```c
int rdma_rm_alloc_XXX(..., RdmaBackendDev *backend_dev, ...) {
    /* ... allocate resources ... */
    
    /* Only call backend if available */
    if (backend_dev && backend_dev->context) {
        ret = rdma_backend_create_XXX(backend_dev, ...);
        if (ret) {
            goto cleanup;
        }
    } else {
        /* No backend - just track the handle */
        rdma_info_report("rdma_rm_alloc_XXX: No backend mode");
        memset(&resource->backend_XXX, 0, sizeof(...));
    }
    
    return 0;
}
```

**Key Points**:
1. Check `backend_dev && backend_dev->context` before backend calls
2. Zero out backend structures when no backend
3. Log the no-backend mode for debugging
4. Still allocate handles and track resources (for driver state management)

---

## Next Steps

1. **Immediate**: Commit the `create_pd` fix
2. **Short-term**: Apply same pattern to `destroy_pd`, `create_mr`, etc.
3. **Medium-term**: Fix VM kernel or create unit tests
4. **Long-term**: Full command set implementation with backend support

---

## References

- `query_port` fix: Commit with no-backend support
- `create_pd` fix: This commit
- PVRDMA spec: Device Shared Region (DSR) command protocol
- libvfio-user: Non-blocking attach pattern from nic-emu

