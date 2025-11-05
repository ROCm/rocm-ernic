# vfu-rdma Project TODO

## Project Overview

This project is porting QEMU's PVRDMA (ParaVirtualized RDMA) device emulation to use **libvfio-user** for userspace device emulation. The goal is to run RDMA device emulation outside of QEMU as a standalone process.

**Current Status:** ✅ Trace infrastructure removed, code compiles cleanly

---

## Completed Tasks ✅

- [x] Remove QEMU trace infrastructure (~48 trace calls removed)
- [x] Clean up orphaned code from trace removal
- [x] Fix meson.build configuration
- [x] Add optional debug macro (`rdma_debug_report()`)
- [x] Verify all source files compile without errors (10/10)
- [x] Document all changes

---

## High Priority Tasks 🔴

### 1. Add Missing Dependencies

**Problem:** Linker fails due to missing library dependencies

**Actions:**
- [ ] Add libibverbs to meson dependencies
  ```python
  ibverbs_dep = dependency('libibverbs')
  dependencies: [glibc_dep, ibverbs_dep]
  ```
- [ ] Add other required QEMU libraries or create stub implementations
- [ ] Consider linking against QEMU object files if using QEMU infrastructure

**Estimated Effort:** 2-4 hours

---

### 2. Create Main Entry Point

**Problem:** No `main()` function exists - executable cannot run

**Actions:**
- [ ] Design the application architecture (standalone daemon vs library)
- [ ] Create `main.c` with entry point
- [ ] Add command-line argument parsing (device path, config, etc.)
- [ ] Initialize logging and error handling
- [ ] Set up signal handlers for graceful shutdown

**Key Decisions:**
- Should this be a daemon that runs in background?
- How will it be configured? (config file, command-line args, env vars?)
- What privileges does it need? (root for RDMA hardware access?)

**Estimated Effort:** 4-8 hours

---

### 3. Integrate libvfio-user Framework

**Problem:** Core integration with libvfio-user is missing

**Location:** libvfio-user is installed at:
- Headers: `/usr/local/include/vfio-user/`
- Library: `/usr/local/lib/x86_64-linux-gnu/libvfio-user.so`

**Actions:**
- [ ] Add libvfio-user to meson dependencies
  ```python
  vfio_user_dep = dependency('vfio-user', required: true)
  # or use direct library if pkg-config unavailable:
  # vfio_user_dep = declare_dependency(
  #     include_directories: include_directories('/usr/local/include/vfio-user'),
  #     link_args: ['-lvfio-user']
  # )
  ```
- [ ] Create vfio-user context and device initialization
- [ ] Implement required callbacks:
  - [ ] Region access (BAR0, BAR1, BAR2)
  - [ ] DMA map/unmap handlers
  - [ ] Interrupt setup (MSI-X)
  - [ ] Device reset handler
- [ ] Map PVRDMA's PCI BARs to vfio-user regions
- [ ] Replace QEMU's DMA functions with vfio-user DMA operations
- [ ] Implement the vfio-user event loop

**Key Files to Create:**
- `vfu_pvrdma_server.c` - Main vfio-user integration
- `vfu_pvrdma_callbacks.c` - vfio-user callback implementations
- `vfu_pvrdma.h` - Integration header

**References:**
- libvfio-user docs: https://github.com/nutanix/libvfio-user
- QEMU vfio-user implementation: `qemu/hw/vfio/user.c`

**Estimated Effort:** 16-32 hours (complex integration)

---

## Medium Priority Tasks 🟡

### 4. Replace QEMU Infrastructure

**Problem:** Code uses QEMU-specific functions not available in standalone build

**Options:**
1. **Link against QEMU libraries** (easiest but adds heavy dependencies)
2. **Stub out unused functions** (for functions never called)
3. **Implement minimal replacements** (for actively used functions)

**Actions:**
- [ ] Audit which QEMU functions are actually used:
  - `qemu_mutex_*` - Threading primitives (NEEDED)
  - `type_register_static` - QEMU type system (MAY NOT NEED)
  - `address_space_map/unmap` - DMA operations (REPLACE WITH VFIO-USER)
  - `pci_register_bar` - PCI config (REPLACE WITH VFIO-USER)
  - `memory_region_*` - Memory regions (REPLACE WITH VFIO-USER)
  - `qemu_chr_fe_*` - Character device (for RDMACM mux - may need rework)

- [ ] Create `compat/qemu-compat.c` with minimal implementations
- [ ] Replace PCI/memory abstractions with vfio-user equivalents
- [ ] Replace QEMU threading with POSIX pthread equivalents

**Estimated Effort:** 8-16 hours

---

### 5. Simplify Build System

**Actions:**
- [ ] Remove hardcoded paths from `meson.build`:
  ```python
  # Current (BAD):
  '/home/stebates/Projects/qemu/build/'
  
  # Should be (GOOD):
  - Use pkg-config to find QEMU if needed
  - Use dependency() for libraries
  - Make paths configurable via meson options
  ```
- [ ] Add proper dependency checking with fallbacks
- [ ] Create installation target (install to `/usr/local/bin` etc.)
- [ ] Add build options for debug/release builds
- [ ] Consider splitting into library + executable

**Estimated Effort:** 2-4 hours

---

## Low Priority / Future Enhancements 🟢

### 6. Testing Infrastructure

- [ ] Create unit tests for core functionality
- [ ] Add integration tests with real RDMA hardware
- [ ] Create mock RDMA client for testing
- [ ] Add CI/CD pipeline (GitHub Actions?)

**Estimated Effort:** 8-16 hours

---

### 7. Documentation

- [ ] Write comprehensive README.md
  - Project purpose and architecture
  - Build instructions
  - Usage examples
  - Configuration options
  - Troubleshooting guide
- [ ] Add inline code documentation (Doxygen?)
- [ ] Create architecture diagrams
- [ ] Write developer guide for contributors

**Estimated Effort:** 4-8 hours

---

### 8. Performance and Features

- [ ] Profile and optimize hot paths
- [ ] Add statistics/monitoring interface
- [ ] Implement logging levels (debug, info, warn, error)
- [ ] Add configuration file support (YAML/JSON/TOML)
- [ ] Implement graceful restart/reload
- [ ] Add systemd service file for daemon mode

**Estimated Effort:** Variable

---

## Technical Debt & Code Quality 🔧

### 9. Code Cleanup

- [ ] Remove unused QEMU type system calls if not needed
- [ ] Simplify error handling patterns
- [ ] Add consistent logging throughout
- [ ] Fix compiler warnings (TARGET_PAGE_SIZE poisoning)
- [ ] Add const correctness
- [ ] Static analysis (clang-tidy, cppcheck)

**Estimated Effort:** 4-8 hours

---

## Questions to Resolve ❓

1. **Architecture Decision:** Should this be:
   - A standalone daemon exposing vfio-user socket?
   - A library that can be embedded in other applications?
   - Both (library with example daemon)?

2. **RDMACM MUX:** The code uses `qemu_chr_fe_*` for RDMA connection manager multiplexing
   - Do we need this functionality?
   - If yes, how should it be implemented without QEMU's character device framework?
   - Can it be replaced with direct socket communication?

3. **VMXNET3 Dependency:** Code requires vmxnet3 device in PCI slot 0
   - Is this a hard requirement for ROCm use case?
   - Can it be made optional or removed?
   - What is the actual network backend in vfio-user context?

4. **Target Use Case:** What is the end goal?
   - Running RDMA device for ROCm in containers?
   - Testing RDMA applications without hardware?
   - Something else?

---

## Build Information

**Current Build Status:**
- ✅ **Compilation:** Success (10/10 files)
- ⚠️ **Linking:** Fails (expected - missing dependencies)

**Dependencies Detected:**
- glib-2.0 ✅ (found)
- libibverbs ⚠️ (needed, not linked)
- libvfio-user ⚠️ (installed, not configured)
- QEMU libraries ⚠️ (optional, not configured)

**Build Commands:**
```bash
cd src/fake-qemu
meson setup build
ninja -C build
```

---

## Resources

**Installed Libraries:**
- **libvfio-user:** `/usr/local/lib/x86_64-linux-gnu/libvfio-user.so.0.0.1`
- **Headers:** `/usr/local/include/vfio-user/`

**Useful References:**
- libvfio-user: https://github.com/nutanix/libvfio-user
- QEMU PVRDMA: `qemu/hw/rdma/vmw/`
- InfiniBand Verbs: https://man7.org/linux/man-pages/man3/ibv_post_send.3.html
- VFIO User Protocol: `vfio-user.h` in libvfio-user

**Original Authors:**
- Yuval Shaia <yuval.shaia@oracle.com>
- Marcel Apfelbaum <marcel@redhat.com>

---

## Getting Started

**Immediate next steps (recommended order):**

1. ✅ **Review and commit trace removal** (run `./commit-trace-removal.sh`)
2. 🔴 **Clarify use case and architecture** (answer questions above)
3. 🔴 **Add libibverbs dependency** (quick win, fixes many linker errors)
4. 🔴 **Create minimal main() function** (establishes entry point)
5. 🔴 **Begin libvfio-user integration** (core functionality)

---

**Last Updated:** 2025-11-05  
**Project:** vfu-rdma (ROCm libvfio-user PVRDMA)  
**Repository:** git@github.com:ROCm/vfu-rdma.git

