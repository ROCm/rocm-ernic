# Next Steps: Resolving PVRDMA + vfio-user Async Write Issue

## Problem Statement

The `vmw_pvrdma` kernel driver fails to probe with error:
```
vmw_pvrdma 0000:00:04.0: driver needs RoCE v1 or v2 support
vmw_pvrdma: probe of 0000:00:04.0 failed with error -14
```

Even though our server correctly writes DSR capabilities (mode=0, gid_types=0x1)
and flushes them using the proper `vfu_sgl_put()` pattern.

## Root Cause Analysis

### The Fundamental Issue

**QEMU's vfio-user client uses asynchronous MMIO BAR writes by default.**

From `qemu/hw/vfio/pci.c:1925`:
```c
/* IO regions are sync, memory can be async */
bar->region.post_wr = (bar->ioport == 0);
```

This means:
- **MMIO BARs**: `post_wr = true` → Uses `vfio_user_send_async()` (no wait)
- **IO Port BARs**: `post_wr = false` → Uses `vfio_user_send_wait()` (waits)

### The Sequence of Events

1. Guest driver writes to PVRDMA BAR1 `DSRHIGH` register
2. QEMU intercepts the write and sends `VFIO_USER_REGION_WRITE` message over
   the Unix socket to our server
3. **QEMU returns immediately to guest WITHOUT waiting** (async send)
4. Guest executes `mb()` memory barrier
5. Guest immediately reads DSR from shared memory
6. **Guest sees stale data** (zeros) because our server hasn't finished yet
7. Our server processes the BAR write in the background and updates DSR
8. Too late - guest already checked and failed probe

### Why We Didn't See This Before

- Our logging shows DSR writes and `vfu_sgl_put()` working correctly
- `memory-backend-memfd` and DMA mapping infrastructure works perfectly
- The `libvfio-user` sample programs (`gpio-pci-idio-16.c`, `server.c`) work
  because they don't have timing-critical initialization sequences
- PVRDMA is unique in requiring **synchronous completion** of its DSR setup
  before the driver reads capabilities

### What We've Verified

✅ `memory-backend-memfd` is **required** (without it `vfu_sgl_get()` fails)
✅ `vfu_sgl_put()` correctly marks pages dirty and flushes to guest
✅ Our DMA infrastructure follows the correct `server.c` pattern
✅ QEMU **does support synchronous writes** via `vfio_user_send_wait()`
❌ QEMU **hardcodes async writes for MMIO BARs** with no server override

## Solution Options

### Option A: Patch QEMU (Recommended)

**Approach:** Add a device property or hardcode `post_wr = false` for vfio-user
PVRDMA devices.

**Implementation:**
```c
// In qemu/hw/vfio/pci.c, around line 1925:
if (vdev->vbasedev.proxy && vdev->vendor_id == PCI_VENDOR_ID_VMWARE &&
    vdev->device_id == PCI_DEVICE_ID_VMWARE_PVRDMA) {
    /* PVRDMA requires synchronous BAR writes for DSR initialization */
    bar->region.post_wr = false;
} else {
    /* IO regions are sync, memory can be async */
    bar->region.post_wr = (bar->ioport == 0);
}
```

**Pros:**
- Clean, minimal change (~5 lines)
- Fixes root cause in QEMU where the decision is made
- No kernel changes required
- Works for all PVRDMA devices (VM and bare-metal)

**Cons:**
- Requires QEMU rebuild and patch maintenance
- Slightly slower BAR writes (but PVRDMA doesn't do frequent BAR I/O)
- Need to upstream or maintain out-of-tree patch

**Effort:** Low (1-2 hours to develop and test)

---

### Option B: Use ioeventfd for Critical Registers

**Approach:** Configure specific BAR1 offsets (DSRHIGH, DSRLOW) to use
ioeventfd, which bypasses the message protocol.

**Implementation:**
- Call `vfu_setup_region_ioeventfds()` in our server for offsets 0x4 and 0x8
- QEMU's KVM will deliver these writes via eventfd (synchronous from guest POV)
- Our server processes the eventfd notification immediately

**Pros:**
- No QEMU patches required
- Very fast notification path (KVM → eventfd → server)
- Can be selective (only critical registers)

**Cons:**
- ioeventfd has limited data passing (usually just notification, not values)
- Would need to restructure DSR initialization logic
- More complex server-side implementation
- Still doesn't solve the fundamental async issue for other registers

**Effort:** Medium (4-6 hours to implement and test)

---

### Option C: Patch Kernel Driver

**Approach:** Modify `vmw_pvrdma` driver to poll a status register instead of
assuming synchronous DSR initialization.

**Implementation:**
```c
// In drivers/infiniband/hw/vmw_pvrdma/pvrdma_main.c
// After writing DSRHIGH, poll for completion:
for (i = 0; i < 1000; i++) {
    mb();
    if (dev->dsr->caps.gid_types != 0)
        break;
    usleep_range(100, 200);
}
if (dev->dsr->caps.gid_types == 0) {
    dev_err(&pdev->dev, "DSR initialization timeout\n");
    return -ETIMEDOUT;
}
```

**Pros:**
- No QEMU changes required
- Makes driver more robust for all device implementations
- Could be upstreamed to mainline kernel

**Cons:**
- Requires kernel rebuild in guest
- Changes device/driver contract (assumes async is possible)
- VMware might reject upstream (PVRDMA was designed for VMware ESXi)
- Doesn't help with existing/stock kernels

**Effort:** Medium (3-4 hours to develop, test, and validate)

---

### Option D: Document as Known Limitation

**Approach:** Document that PVRDMA + vfio-user is incompatible due to
QEMU's async MMIO behavior, and recommend alternatives.

**Implementation:**
- Update README.md with "Known Limitations" section
- Suggest using kernel vfio-pci passthrough instead
- Document workarounds (Options A, B, or C)
- Reference this analysis for future implementers

**Pros:**
- No code changes required
- Preserves current working state for local testing
- Educational value for vfio-user community

**Cons:**
- Doesn't solve the problem
- Project remains incomplete for production use
- Wastes previous development effort

**Effort:** Low (30 minutes to document)

---

## Recommendation

**Proceed with Option A (Patch QEMU)** for the following reasons:

1. **Minimal effort** - Small, localized change
2. **Root cause fix** - Solves the problem where it originates
3. **Clean design** - No hack or workaround
4. **Precedent exists** - Other devices (e.g., NVMe) have special cases
5. **Upstreamable** - Could be submitted to QEMU with proper rationale

### Implementation Plan

1. Create a patch for `qemu/hw/vfio/pci.c` (~10 lines)
2. Rebuild QEMU with the patch
3. Test with our `vfu_pvrdma` server
4. Verify guest driver probes successfully
5. Document the patch in `docs/QEMU_PATCH.md`
6. Consider upstreaming to QEMU mailing list

### Timeline

- **Patch development:** 1 hour
- **Testing:** 30 minutes
- **Documentation:** 30 minutes
- **Total:** ~2 hours

## References

- QEMU vfio-user implementation: `qemu/hw/vfio-user/device.c`
- vfio-user protocol spec: `qemu/docs/interop/vfio-user.rst`
- PVRDMA kernel driver: `kernel-tools/src/drivers/infiniband/hw/vmw_pvrdma/`
- libvfio-user documentation: `libvfio-user/docs/`

## Current Status

- ✅ Server compiles and runs stably
- ✅ PCI device enumeration works
- ✅ DSR DMA mapping works correctly
- ✅ DSR capability writes confirmed via logging
- ✅ `vfu_sgl_put()` correctly flushes to guest
- ❌ Guest driver fails due to async write timing issue
- 🔍 Root cause identified: QEMU's `post_wr = true` for MMIO

---

**Next Action:** Implement Option A (QEMU patch) and test.

