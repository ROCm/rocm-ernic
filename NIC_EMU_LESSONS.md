# Lessons Learned from nic-emu

## Problem Statement
The `vfu_pvrdma` server was experiencing intermittent socket binding issues and race conditions during startup, particularly:
- `vfu_create_ctx() failed: Address already in use`
- `vfu_attach_ctx() failed: Interrupted system call (EINTR)`
- Timing-dependent failures when starting/restarting quickly

## Root Cause
The original implementation used **blocking attach mode**, which caused:
1. **Race conditions** between socket cleanup and context creation
2. **Signal handling issues** during the blocking attach call
3. **Poor error recovery** for transient errors like EAGAIN/EINTR

## Solution: Learning from nic-emu
By studying the **nic-emu** project (a working Rust-based E1000 vfio-user device), we identified the proper pattern:

### Key Changes

#### 1. Non-Blocking Attach Flag
**Before:**
```c
vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path, 0, dev, VFU_DEV_TYPE_PCI);
```

**After:**
```c
vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path, 
                        LIBVFIO_USER_FLAG_ATTACH_NB, dev, VFU_DEV_TYPE_PCI);
```

#### 2. Proper Attach Loop
**Before:**
```c
ret = vfu_attach_ctx(vfu_ctx);
if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        /* Retry with logging spam */
        usleep(100000);
        continue;
    }
    err(EXIT_FAILURE, "vfu_attach_ctx() failed");
}
```

**After:**
```c
/* Attach to client (non-blocking) */
ret = vfu_attach_ctx(vfu_ctx);
if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* No client yet, sleep and retry */
        usleep(100000);
        continue;
    } else if (errno == EINTR) {
        /* Interrupted by signal, check shutdown flag */
        continue;
    }
    vfu_log(vfu_ctx, LOG_ERR, "vfu_attach_ctx() failed with errno=%d: %s", 
            errno, strerror(errno));
    err(EXIT_FAILURE, "vfu_attach_ctx() failed");
}
```

### Benefits

#### Before (Blocking Mode)
- ❌ Socket binding race conditions
- ❌ EINTR errors during attach
- ❌ Poor startup reliability
- ❌ Hard to diagnose timing issues

#### After (Non-Blocking Mode)
- ✅ Clean, reliable startup
- ✅ Proper EAGAIN/EINTR handling
- ✅ No socket binding races
- ✅ Server waits gracefully for client connection
- ✅ Better signal handling (Ctrl+C works properly)

## Test Results

### Startup Test
```bash
$ sudo ./build/vfu_pvrdma -s /tmp/vfio-user-pvrdma.sock -v
vfu_pvrdma: Starting PVRDMA device server (Phase 1 integration)
  Socket: /tmp/vfio-user-pvrdma.sock
  ...
INFO: rdma: PVRDMA device realized successfully
```
✅ **Server starts and waits cleanly for client**

### QEMU Connection Test
```bash
$ sudo ./run-vm-vfio-user
=== Starting VM with vfio-user-pci device ===
...
```

Server log shows:
```
vfu_pvrdma: DEBUG: region7: read 0x2800000 from (0x8:4)
vfu_pvrdma: DEBUG: BAR0 addr 0xfe850000
vfu_pvrdma: DEBUG: region7: wrote 0xfe850000 to (0x10:4)
vfu_pvrdma: DEBUG: BAR1 addr 0xfe857000
...
```
✅ **QEMU connects successfully, PCI BAR access working**

## nic-emu Architecture (Rust Reference)

The `nic-emu` project demonstrates the proper pattern:

### 1. Device Configuration (`e1000.rs`)
```rust
DeviceConfigurator::default()
    .socket_path(path)
    .overwrite_socket(true)           // Auto-cleanup stale sockets
    .non_blocking(true)                // Non-blocking attach
    .pci_config(...)
    .add_device_region(...)
    .using_interrupt_requests(...)
    .build()
```

### 2. Attach Loop (`main.rs`)
```rust
// Wait for client to attach
loop {
    events.clear();
    poller.wait(&mut events, None).unwrap();

    match ctx.attach().unwrap() {
        Some(_) => break,  // Client connected
        None => {
            // Renew fd interest
            poller.modify(&ctx, Event::all(EVENT_KEY_ATTACH)).unwrap();
        }
    }
}
```

### 3. Run Loop
```rust
// Process client requests
loop {
    events.clear();
    poller.wait(&mut events, None).unwrap();
    
    for event in events.iter() {
        match event.key {
            EVENT_KEY_RUN => ctx.run().unwrap(),
            // ... other events ...
        }
    }
}
```

## Key Takeaways

1. **Always use non-blocking attach** for vfio-user devices
   - Prevents race conditions
   - Better error handling
   - Proper signal handling

2. **Study working implementations** when stuck
   - nic-emu provided the reference architecture
   - Rust code is often clearer for understanding patterns

3. **Socket cleanup should be automatic**
   - Check for stale sockets before creating context
   - Use `stat()` + `unlink()` to verify and remove
   - Add delays after cleanup to let system release resources

4. **Separate concerns in the main loop**
   - Attach phase: wait for client
   - Run phase: process requests
   - Each phase has different error handling needs

## Files Modified

### `vfu_pvrdma.c`
- Added `LIBVFIO_USER_FLAG_ATTACH_NB` to `vfu_create_ctx()`
- Improved attach loop error handling
- Better separation of EAGAIN vs EINTR handling

## Next Steps

With reliable startup now working:
1. ✅ Test driver loading in VM
2. ✅ Test MSI-X interrupt allocation
3. ✅ Test device activation
4. 🔄 Test RDMA command processing (when backend available)

## References
- [nic-emu GitHub](https://github.com/vmuxIO/nic-emu) - Reference implementation
- [libvfio-user](https://github.com/nutanix/libvfio-user) - Library documentation
- `LIBVFIO_USER_FLAG_ATTACH_NB` - Non-blocking attach flag definition

