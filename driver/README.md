# AMD ROCm ERNIC Driver (rocm_ernic) — DEPRECATED

> **This driver is deprecated.**  New deployments should use the upstream
> **ionic** driver with the AMD emulated-device patch instead.  See
> `patches/0001-ionic-add-AMD-emulated-ionic-device-id.patch` and
> `scripts/setup-ionic-dkms.sh`.  Start the server with `--ionic` (`-I`).
>
> This directory is retained for reference and backwards compatibility.
> It will be removed once the ionic migration path is validated end-to-end.

## Overview (historical)

This was the Linux kernel driver for the AMD ROCm ERNIC (Emulated RDMA NIC),
designed for use with the libvfio-user based userspace device server
(`rocm-ernic`). The driver enabled RDMA functionality in virtual machines
using a PVRDMA-derived protocol (VID:DID 0x1022:0x8000).

## Building the Driver

```bash
# Build the module (note KDIR is optional)
make -C $KDIR M=$(pwd) modules

# Install the module (optional)
sudo make -C $KDIR M=$(pwd) modules_install
sudo depmod -a
```

## Loading the Driver

```bash
# Load the module
sudo modprobe rocm_ernic

# Check dmesg for driver messages
dmesg | grep rocm_ernic

# Expected output when device is found:
# [  xxx] rocm_ernic 0000:00:04.0: device version 17, driver version 20
# [  xxx] rocm_ernic 0000:00:04.0: DSR initialized after N polls
# [  xxx] rocm_ernic 0000:00:04.0: running in standalone mode (no netdev)
# [  xxx] rocm_ernic 0000:00:04.0: registered ibdev rocm_ernicX
```

## Testing with rocm-ernic server

1. Start the vfio-user server:
   ```bash
   cd /home/stebates/Projects/rocm-ernic
   sudo ./build/rocm-ernic --socket /tmp/vfio-user-rocm-ernic.sock
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
   sudo modprobe rocm_ernic
   ```

4. Verify the device is recognized:
   ```bash
   lspci | grep 1022:8000
   ibv_devices
   ```

## PCI device name

To have `lspci` show "ROCm Emulated RDMA NIC" for device
`0x1022:0x8000`, merge the fragment from the project into your
system pci.ids:

- Fragment: [../scripts/pci.ids.rocm-ernic](../scripts/pci.ids.rocm-ernic)
- Add the device line `8000  ROCm Emulated RDMA NIC` under vendor
  `1022` (AMD) in your system pci.ids (e.g.
  `/usr/share/hwdata/pci.ids` or `/usr/share/misc/pci.ids`). You
  can merge the fragment or add the line manually.
- For inclusion in the official PCI ID database, submit
  `0x1022`/`0x8000` with name "ROCm Emulated RDMA NIC" at
  <https://admin.pci-ids.ucw.cz/> (see <https://pci-ids.ucw.cz/>
  for instructions).

## Unprivileged dmesg (optional)

For rocm-ernic testing, you may want any user to run `dmesg`
without root.

- **Option A (e.g. Ubuntu 24.04):** Allow all users to read the
  kernel log:
  - Temporarily: `sudo sysctl -w kernel.dmesg_restrict=0`
  - Persistently: copy
    [../scripts/99-rocm-ernic-dmesg.conf](../scripts/99-rocm-ernic-dmesg.conf)
    to `/etc/sysctl.d/` and reboot or run
    `sudo sysctl -p /etc/sysctl.d/99-rocm-ernic-dmesg.conf`.
- **Option B:** On distros where the `adm` group can read kernel
  logs, add the test user to group `adm` and re-login.
