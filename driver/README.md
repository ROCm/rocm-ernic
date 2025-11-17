# AMD Emulated RDMA Driver (amd_emrdma)

## Overview

This is a Linux kernel driver for AMD Emulated RDMA devices, designed for use
with vfio-user based userspace device emulation. It is based on VMware's PVRDMA
driver but includes critical modifications to handle asynchronous BAR writes in
vfio-user environments.

## Key Differences from VMware PVRDMA

### 1. PCI Device IDs
- **Vendor ID:** 0x1022 (AMD) instead of 0x15ad (VMware)
- **Device ID:** 0x1484 (AMD Emulated RDMA) instead of 0x0820 (VMware PVRDMA)

### 2. Polling for DSR Initialization
The most critical change is the addition of **polling logic** to handle
asynchronous MMIO BAR writes in QEMU's vfio-user implementation.

**Problem:** QEMU's vfio-user client uses asynchronous writes for MMIO BARs by
default. When the driver writes to the DSRHIGH register, QEMU sends the message
to the userspace server but returns immediately to the guest. The guest then
executes `mb()` and reads DSR, seeing stale data before the server finishes
processing.

**Solution:** After writing DSRHIGH, the driver polls the DSR for up to 1 second
(100 iterations × 10-20ms), checking for valid `gid_types` to confirm the server
has initialized the capabilities.

```c
for (poll_count = 0; poll_count < 100; poll_count++) {
    mb();
    if (AMD_EMRDMA_SUPPORTED(dev)) {
        dsr_ready = true;
        break;
    }
    usleep_range(10000, 20000);  /* 10-20ms */
}
```

### 3. Optional VMXNET3 Pairing
The original PVRDMA driver **requires** a paired VMXNET3 network device. This
driver makes the pairing **optional**, allowing standalone operation without
any network device.

- If a VMXNET3 device is found, it will be used (VMware compatibility)
- If no VMXNET3 device is found, the driver continues in standalone mode
- Network operations will only be available if a paired device exists

### 4. Dependency Simplification
**Kconfig changes:**
- Removed hard dependency on `VMXNET3`
- Removed dependency on `NETDEVICES` and `ETHERNET` (optional)
- Minimal dependencies: `PCI` and `INET` only

## Building the Driver

### Out-of-Tree Build (Recommended for Testing)

```bash
cd /home/stebates/Projects/vfu-rdma/driver

# Set kernel source directory
export KDIR=/home/stebates/Projects/kernel-tools/src

# Build the module
make -C $KDIR M=$(pwd) modules

# Install the module (optional)
sudo make -C $KDIR M=$(pwd) modules_install
sudo depmod -a
```

### In-Tree Build (For Upstreaming)

1. Copy driver files to kernel source:
   ```bash
   cp -r driver/* $KDIR/drivers/infiniband/hw/amd_emrdma/
   ```

2. Add to `drivers/infiniband/hw/Kconfig`:
   ```
   source "drivers/infiniband/hw/amd_emrdma/Kconfig"
   ```

3. Add to `drivers/infiniband/hw/Makefile`:
   ```
   obj-$(CONFIG_INFINIBAND_AMD_EMRDMA)    += amd_emrdma/
   ```

4. Configure and build:
   ```bash
   cd $KDIR
   make menuconfig  # Enable CONFIG_INFINIBAND_AMD_EMRDMA
   make -j$(nproc)
   ```

## Loading the Driver

```bash
# Load the module
sudo modprobe amd_emrdma

# Check dmesg for driver messages
dmesg | grep amd_emrdma

# Expected output when device is found:
# [  xxx] amd_emrdma 0000:00:04.0: device version 17, driver version 20
# [  xxx] amd_emrdma 0000:00:04.0: DSR initialized after N polls
# [  xxx] amd_emrdma 0000:00:04.0: running in standalone mode (no netdev)
# [  xxx] amd_emrdma 0000:00:04.0: registered ibdev amd_emrdmaX
```

## Testing with rocm_ernic Server

1. Start the vfio-user server:
   ```bash
   cd /home/stebates/Projects/vfu-rdma
   sudo ./build/rocm_ernic --socket /tmp/vfio-user-rocm-ernic.sock
   ```

2. Launch QEMU with memory-backend-memfd:
   ```bash
   qemu-system-x86_64 \
     -machine q35,accel=kvm \
     -cpu EPYC \
     -smp cpus=2 \
     -object memory-backend-memfd,id=mem0,share=on,size=2048M \
     -machine memory-backend=mem0 \
     -nographic \
     -drive if=virtio,format=qcow2,file=vm-image.qcow2 \
     -netdev user,id=net0,hostfwd=tcp::2222-:22 \
     -device virtio-net-pci,netdev=net0 \
     -device '{"driver":"vfio-user-pci","socket":\
{"path":"/tmp/vfio-user-rocm-ernic.sock","type":"unix"}}'
   ```

3. Inside the guest, load the driver:
   ```bash
   sudo modprobe amd_emrdma
   ```

4. Verify the device is recognized:
   ```bash
   lspci | grep 1022:1484
   ibv_devices
   ```

## Troubleshooting

### Device Not Found
- Ensure the rocm_ernic server is running
- Check socket permissions: `sudo chmod 666 /tmp/vfio-user-rocm-ernic.sock`
- Verify QEMU command includes `memory-backend-memfd`

### DSR Initialization Timeout
If you see "DSR initialization timeout" in dmesg:
- Check server logs for DMA mapping errors
- Ensure `memory-backend-memfd` is configured in QEMU
- Increase polling timeout in driver if needed

### Network Operations Fail
This is expected in standalone mode:
- The driver loads successfully without VMXNET3
- Network-dependent operations will fail
- For full functionality, ensure a paired network device exists

## Comparison with VMware PVRDMA

| Feature | VMware PVRDMA | AMD EMRDMA |
|---------|---------------|------------|
| Vendor ID | 0x15ad (VMware) | 0x1022 (AMD) |
| Device ID | 0x0820 | 0x1484 |
| DSR Init | Synchronous (assumes immediate) | Polls for completion |
| VMXNET3 | Required | Optional |
| Use Case | VMware ESXi VMs | vfio-user userspace emulation |
| QEMU Support | Native | vfio-user protocol |

## References

- VMware PVRDMA: `drivers/infiniband/hw/vmw_pvrdma/`
- vfio-user protocol: `qemu/docs/interop/vfio-user.rst`
- libvfio-user: https://github.com/nutanix/libvfio-user
- rocm_ernic server: `../src/rocm_ernic.c`

## License

Dual licensed under GPLv2 and BSD 2-Clause (inherited from VMware PVRDMA).

## Authors

- Original PVRDMA: VMware, Inc. (Copyright 2012-2016)
- AMD EMRDMA modifications: Stephen Bates <stephen@elmail.org> (2025)

