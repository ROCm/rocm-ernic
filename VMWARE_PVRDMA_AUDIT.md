# VMware/PVRDMA Reference Audit

**Generated:** 2025-11-14  
**Updated:** 2025-11-14 (post file rename)  
**Purpose:** Comprehensive audit of all VMware, PVRDMA, and vfu_ references
in driver/ and src/ directories

**Note:** Driver source files have been renamed from `amd_emrdma_*` to
`rocm_ernic_*` since the initial audit. This document has been updated to
reflect the new filenames. The internal symbols (functions, structs, macros)
remain as `amd_emrdma_*` for kernel API compatibility.

---

## Executive Summary

This audit identifies all references to VMware, PVRDMA, and vfu_ prefixes in
the codebase. References fall into several categories:

1. **Copyright Notices** - Original VMware copyrights (must preserve)
2. **Compatibility References** - Intentional for hardware/protocol compat
3. **QEMU-Derived Code** - Files from QEMU's PVRDMA implementation
4. **Code Symbols** - Internal function/struct names that could be renamed
5. **Documentation** - Attribution and comparison references

---

## Driver Directory (`driver/`)

### 1. Copyright Notices (MUST PRESERVE)
All driver C files contain original VMware copyright:
```
Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
```

**Files:**
- `rocm_ernic_main.c`
- `rocm_ernic_cq.c`
- `rocm_ernic_qp.c`
- `rocm_ernic_mr.c`
- `rocm_ernic_cmd.c`
- `rocm_ernic_verbs.c`
- `rocm_ernic_verbs.h`
- `rocm_ernic_dev_api.h`
- `rocm_ernic_doorbell.c`
- `rocm_ernic_misc.c`
- `rocm_ernic_ring.h`
- `rocm_ernic.h`
- `rocm_ernic_srq.c` (Copyright 2016-2017)

**Action:** ✅ KEEP - Required for legal attribution

---

### 2. Compatibility Constants (KEEP for Protocol Compatibility)

#### `driver/rocm_ernic_main.c`
```c
Line 168: .driver_id = RDMA_DRIVER_VMW_PVRDMA, /* Use VMware ID for compat */
```
**Reason:** Uses upstream kernel constant for driver registration
**Action:** ✅ KEEP - Required for kernel RDMA subsystem compatibility

```c
Line 79: return sysfs_emit(buf, "VMW_AMD_EMRDMA-%s\n", DRV_VERSION);
```
**Reason:** Sysfs attribute string
**Action:** ⚠️ CONSIDER CHANGING to "AMD_EMRDMA-%s" for consistency

```c
Lines 1009-1017: VMware vmxnet3 device detection (optional feature)
```
**Reason:** Optional compatibility check for VMware environments
**Action:** ✅ KEEP - Harmless compatibility feature

```c
Lines 1247-1248: MODULE_AUTHOR/DESCRIPTION
```
**Action:** ⚠️ SHOULD UPDATE to reflect AMD/ROCm adaptation

---

#### `driver/rocm_ernic.h`
```c
Line 70: #define PCI_DEVICE_ID_VMWARE_AMD_EMRDMA 0x0820
```
**Reason:** This is confusing - it's named VMWARE but value is AMD's
**Action:** ⚠️ RENAME to `PCI_DEVICE_ID_AMD_EMRDMA` (value 0x1484 already
used correctly)

---

### 3. Documentation References

#### `driver/Kconfig`
```
Line 14: This driver is based on VMware's PVRDMA driver but has been
         adapted...
```
**Action:** ✅ KEEP - Proper attribution

#### `driver/README.md`
Multiple references to VMware PVRDMA for:
- Attribution (line 10, 195)
- Comparison table (lines 171-179)
- Key differences section (lines 13-17)
- Technical background (line 45, 184, 191)

**Action:** ✅ KEEP - All are appropriate attribution/documentation

---

## Source Directory (`src/`)

### 1. QEMU-Derived Code (`src/from-qemu/`)

These files are **direct imports from QEMU's PVRDMA implementation** and
contain extensive VMware/PVRDMA references:

#### Directory Structure with VMware/PVRDMA Names:
```
src/from-qemu/hw/rdma/vmw/
├── pvrdma.h
├── pvrdma_cmd.c
├── pvrdma_dev_ring.c
├── pvrdma_dev_ring.h
├── pvrdma_main.c
├── pvrdma_qp_ops.c
└── pvrdma_qp_ops.h

src/from-qemu/include/qemu-extra/standard-headers/
├── rdma/vmw_pvrdma-abi.h
└── drivers/infiniband/hw/vmw_pvrdma/
    ├── pvrdma.h
    ├── pvrdma_dev_api.h
    ├── pvrdma_ring.h
    └── pvrdma_verbs.h
```

**Total PVRDMA symbols found:** 1396+ occurrences across these files

**Critical PVRDMA Symbols (examples):**
- `PVRDMADev` struct - Core device structure
- `pvrdma_device_shared_region` - Shared memory region
- `pvrdma_sq_wqe_hdr`, `pvrdma_rq_wqe_hdr` - Work queue entries
- `pvrdma_cqe` - Completion queue entry
- `pvrdma_sge` - Scatter-gather entry
- `PVRDMA_REG_*` - Hardware register offsets
- `PVRDMA_HW_VERSION` - Protocol version
- Functions: `pvrdma_regs_write_impl`, `pvrdma_uar_write_impl`, etc.

**File Headers:**
```c
Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
```

**PCI Constants:**
```c
#define PCI_VENDOR_ID_VMWARE        0x15ad
#define PCI_DEVICE_ID_VMWARE_PVRDMA 0x0820
```

**Action for QEMU-Derived Code:**
🔴 **DO NOT RENAME** - These files are:
1. Direct QEMU imports with VMware copyright
2. Intentionally match QEMU source structure
3. Use hardware-defined protocol/layout
4. May need periodic resyncing with upstream QEMU
5. Renaming would break maintainability

**Rationale:** The device **IS** implementing the VMware PVRDMA protocol.
The fact that we're AMD ROCm ERNIC is a layer above this - we're using the
PVRDMA device protocol as our virtual hardware interface.

---

### 2. Compatibility Bridge (`src/rocm_ernic_compat.c/h`)

These files **wrap** the QEMU PVRDMA code and thus necessarily use PVRDMA
symbols:

#### `src/rocm_ernic_compat.h`
```c
Line 32: typedef void *pvrdma_handle_t;
Lines 47-79: pvrdma_device_create/destroy/realize functions
Lines 79-106: pvrdma_regs_write/read, pvrdma_uar_write/read functions
```

#### `src/rocm_ernic_compat.c`
```c
Lines 27-33: #include "from-qemu/hw/rdma/vmw/pvrdma*.h"
Line 61: PVRDMADev *pvrdma;
Line 70: pvrdma = calloc(1, sizeof(PVRDMADev));
Lines 56-708: Heavy use of pvrdma_* functions and PVRDMADev struct
```

**Action:** 🟡 **COULD RENAME** pvrdma_handle_t and API functions, but:
- Current names clearly indicate they wrap PVRDMA protocol
- Renaming adds little value
- Creates maintenance burden
- **Recommendation:** KEEP as-is for clarity

---

### 3. Server Main Code (`src/rocm_ernic_server.c`)

#### PVRDMA Symbol Usage:
```c
Line 138: if (!dev->pvrdma_handle) {
Line 164: pvrdma_regs_write(dev->pvrdma_handle, ...)
Line 170: val = pvrdma_regs_read(dev->pvrdma_handle, ...)
Line 203: pvrdma_uar_write(dev->pvrdma_handle, ...)
Line 209: val = pvrdma_uar_read(dev->pvrdma_handle, ...)
Lines 272-303: pvrdma_device_init(), pvrdma_device_create/realize()
Lines 593,688,692: "vfu_pvrdma:" log prefixes
Line 755: pvrdma_device_destroy()
```

**Action:** 🟡 **CONSIDER UPDATING**
- `pvrdma_handle_t` could become `rdma_device_handle_t` or `device_handle_t`
- API function calls could be renamed to `rdma_device_*` or `device_*`
- Log prefixes should be "rocm_ernic:" (already noted in grep)
- BUT: Changes must cascade through compat.h and internal.h

**Benefit vs Cost:**
- **Benefit:** More generic naming
- **Cost:** Additional churn, reduced clarity about underlying protocol
- **Recommendation:** LOW PRIORITY - Current names are accurate

---

### 4. VFU_ Prefix Usage

The `vfu_` prefix is from **libvfio-user library** and is standard API:

#### libvfio-user API Calls (147 occurrences):
```c
vfu_ctx_t, vfu_create_ctx(), vfu_setup_log(), vfu_pci_init(),
vfu_setup_region(), vfu_setup_device_nr_irqs(), vfu_realize_ctx(),
vfu_attach_ctx(), vfu_run_ctx(), vfu_destroy_ctx(),
vfu_addr_to_sgl(), vfu_sgl_get(), vfu_sgl_put(), vfu_irq_trigger()
```

**Action:** ✅ **KEEP** - These are external library APIs, cannot be renamed

---

## Categorized Recommendations

### Category A: MUST PRESERVE
✅ All VMware copyright notices (legal requirement)
✅ QEMU-derived code in `src/from-qemu/` (maintainability)
✅ libvfio-user API (`vfu_*`) (external library)
✅ Kernel constant `RDMA_DRIVER_VMW_PVRDMA` (kernel API)
✅ Attribution in documentation

### Category B: SHOULD UPDATE (High Value)
⚠️ `driver/rocm_ernic_main.c` line 79: sysfs string "VMW_AMD_EMRDMA"
   → Change to "AMD_EMRDMA"

⚠️ `driver/rocm_ernic_main.c` lines 1247-1248: MODULE_AUTHOR/DESCRIPTION
   → Update to reflect AMD/ROCm stewardship

⚠️ `driver/rocm_ernic.h` line 70: `PCI_DEVICE_ID_VMWARE_AMD_EMRDMA`
   → Rename to `PCI_DEVICE_ID_AMD_EMRDMA_COMPAT` or similar

⚠️ `src/rocm_ernic_server.c` log prefixes: "vfu_pvrdma:"
   → Change to "rocm_ernic:"

### Category C: COULD UPDATE (Low Priority)
🟡 `pvrdma_handle_t` and related API in compat layer
   - Could become `rdma_device_handle_t` or similar
   - Requires coordinated changes across multiple files
   - Low value - current names accurately reflect protocol layer

### Category D: INTENTIONAL COMPATIBILITY (Keep)
✅ PVRDMA protocol structures/constants in QEMU code
✅ VMware PCI vendor/device IDs in QEMU headers
✅ vmxnet3 compatibility check in driver

---

## Summary Statistics

| Category | Count | Action |
|----------|-------|--------|
| VMware copyright notices | 15 files | KEEP |
| PVRDMA symbols in QEMU code | 1396+ | KEEP |
| PVRDMA symbols in compat layer | ~150 | KEEP (or low-pri rename) |
| PVRDMA symbols in server main | ~20 | CONSIDER |
| vfu_ libvfio-user API calls | 147 | KEEP |
| VMware/PVRDMA in docs | 20+ | KEEP (attribution) |

**Total References Found:** ~1600+

**High-Priority Changes:** 4 items (sysfs string, MODULE_* macros, log
prefixes)

**Estimated Effort:**
- High-priority changes: 30 minutes
- Complete compat layer rename: 3-4 hours (low value)

---

## Architectural Note

The ROCm ERNIC project has a **layered architecture**:

```
┌─────────────────────────────────────────────┐
│  ROCm ERNIC (AMD EMRDMA Driver + Server)   │ ← Our brand/project
├─────────────────────────────────────────────┤
│  PVRDMA Protocol Implementation             │ ← VMware-designed protocol
│  (from QEMU, in src/from-qemu/)             │   (hardware interface)
├─────────────────────────────────────────────┤
│  libvfio-user (vfu_* APIs)                  │ ← Transport layer
└─────────────────────────────────────────────┘
```

The PVRDMA references in `src/from-qemu/` are **not remnants to clean up** -
they are the **intentional hardware protocol layer** we're implementing.

Think of it like this:
- We make an "AMD EMRDMA device"
- That speaks the "PVRDMA protocol" (VMware-designed, now standardized in
  QEMU/Linux)
- Over the "vfio-user transport"

Just as a WiFi driver might reference "802.11" standards without being
802.11-branded, we reference PVRDMA protocol without being VMware-branded.

---

## Recommendation

**Focus on high-priority changes only:**

1. Update sysfs string to remove VMW prefix
2. Update MODULE_AUTHOR/DESCRIPTION
3. Fix confusing PCI_DEVICE_ID_VMWARE_AMD_EMRDMA name
4. Change log prefixes from "vfu_pvrdma:" to "rocm_ernic:"

**Leave everything else as-is because:**
- Copyright notices are legally required
- QEMU-derived code should match upstream for maintainability
- PVRDMA symbols accurately describe the protocol layer
- Renaming effort >> benefit for internal symbols
- Current naming aids understanding of architecture

---

## Files Requiring Changes

### High Priority Changes:

1. **driver/rocm_ernic_main.c**
   - Line 79: `"VMW_AMD_EMRDMA-%s"` → `"AMD_EMRDMA-%s"`
   - Line 1247: `MODULE_AUTHOR("VMware, Inc")` → `MODULE_AUTHOR("Advanced
     Micro Devices, Inc")`
   - Line 1248: `MODULE_DESCRIPTION("VMware Paravirtual RDMA driver")` →
     `MODULE_DESCRIPTION("AMD Emulated RDMA NIC driver")`

2. **driver/rocm_ernic.h**
   - Line 70: `PCI_DEVICE_ID_VMWARE_AMD_EMRDMA` →
     `PCI_DEVICE_ID_AMD_EMRDMA_PVRDMA_COMPAT`
   - Line 68: Update comment "VMware AMD_EMRDMA PCI device id" → "AMD EMRDMA
     PCI device id (PVRDMA protocol compatible)"

3. **src/rocm_ernic_server.c**
   - Lines 593, 685, 688, 692, 768: `"vfu_pvrdma:"` → `"rocm_ernic:"`
   - Line 593: `"Starting PVRDMA device server"` → `"Starting ROCm ERNIC
     device server"`
   - Line 298: `"PVRDMA device initialized"` → `"ROCm ERNIC device
     initialized"`
   - Line 260: `"PVRDMA device realized"` → `"ROCm ERNIC device realized"`
   - Line 169, 200: Update comments

All other references should remain unchanged for the architectural reasons
described above.

---

## Changes Completed Since Audit

Since the initial audit, the following changes have been completed:

### ✅ File Renames (Completed)
All 14 driver source files renamed from `amd_emrdma_*` to `rocm_ernic_*`:
- Headers: 5 files renamed
- Source files: 9 files renamed
- Makefiles updated to reference new filenames
- All `#include` statements updated (19 changes)

**Impact:**
- Source code organization now matches project naming
- Module name remains `amd_emrdma.ko` (compatibility)
- All internal symbols remain `amd_emrdma_*` (kernel API compatibility)

### ✅ Format Warning Fix (Completed)
Fixed format specifier warning in `rocm_ernic_misc.c`:
- Line 83: Changed `%llu` to `%u` for `pdir->ntables` (int type)

### ✅ HIGH-PRIORITY CHANGES (ALL COMPLETED)

#### 1. driver/rocm_ernic_main.c - sysfs string (✅ DONE)
- Line 79: Changed `"VMW_AMD_EMRDMA-%s\n"` → `"AMD_EMRDMA-%s\n"`

#### 2. driver/rocm_ernic_main.c - MODULE macros (✅ DONE)
- Line ~1247: Changed `MODULE_AUTHOR("VMware, Inc")` → 
  `MODULE_AUTHOR("Advanced Micro Devices, Inc")`
- Line ~1248: Changed `MODULE_DESCRIPTION("VMware Paravirtual RDMA driver")` → 
  `MODULE_DESCRIPTION("AMD ROCm ERNIC - Emulated RDMA NIC driver")`

#### 3. driver/rocm_ernic.h - PCI constant (✅ DONE)
- Lines 67-70: Removed unused `PCI_DEVICE_ID_VMWARE_AMD_EMRDMA` definition
- This constant was never actually used in the code

#### 4. src/rocm_ernic_server.c - log prefixes (✅ DONE)
- Lines 685, 688, 692: Changed `"vfu_pvrdma:"` → `"rocm_ernic:"`
- Updated all server log messages to use correct project name

### ✅ Additional Cleanup (Completed)

#### Build System Simplification
- Removed `driver/Makefile.in-tree` (unused for vfio-user out-of-tree driver)
- Updated `driver/Makefile` comment to remove in-tree reference
- Updated `driver/README.md` to remove "In-Tree Build" section
- Updated rebase planning docs to reflect simplified build

#### Tests Directory Cleanup
Removed 8 obsolete test files with hardcoded paths:
- `test-cq-creation.sh` - manual VM test, not in CI
- `test-driver.sh` - manual VM test, not in CI
- `test-full-flow.sh` - manual VM test, not in CI
- `test-nic-emu-style.sh` - manual VM test, not in CI
- `test_backend_patterns.sh` - complex VM setup, not in CI
- `verify-version-init.sh` - simple check, not in CI
- `test_loopback_standalone.c` - orphaned, not built by meson
- `test_dt` - orphaned binary

**Retained 7 essential test files:**
- test_pci_client.c (meson-built)
- test_bar1_read.c (meson-built)
- test_dsr_trigger.c (meson-built)
- test_data_transfer.c (meson-built)
- run-test.sh (meson-registered test harness)
- meson.build (build config)
- README.md (documentation)

**Rationale:** Local testing now uses `scripts/local-vm-test.sh`, and CI uses
`integration-test.yml` workflow. The removed scripts had hardcoded paths to
`/home/stebates/Projects/qemu-minimal/qemu` and were not portable.

### ✅ Documentation Updates (Completed)
- `README.md` - Updated 3 references from "vfu-rdma" to "rocm-ernic"
- `driver/README.md` - Updated 2 references from "vfu-rdma" to "rocm-ernic"
- `tests/README.md` - Updated title from "vfu-rdma Tests" to "rocm-ernic Tests"

---

## Final Status

**ALL HIGH-PRIORITY ITEMS COMPLETE** ✅

The codebase is now ready for the proposed 4-commit rebase with proper:
- AMD branding and attribution
- Clean file organization
- Minimal test suite
- Correct log prefixes
- Simplified build system

**Remaining VMware/PVRDMA references are intentional:**
- Copyright notices (legal requirement)
- QEMU-derived protocol code (maintainability)
- Documentation attribution (proper credit)
- Kernel API constants (compatibility)

---

**End of Audit**

