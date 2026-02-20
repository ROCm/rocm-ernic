# AMD ROCm ERNIC Driver (rocm_ernic)

## Overview

This is the Linux kernel driver for the AMD ROCm ERNIC (Emulated RDMA NIC),
designed for use with the libvfio-user based userspace device server
(`rocm-ernic`). The driver enables RDMA functionality in virtual machines.

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

## VM setup script

To set up a new VM in one go (mount hostfs, install packages, build/load
driver, pci.ids, dmesg, udev, netplan), run inside the VM (with the repo
available, e.g. via hostfs at ~/Projects/rocm-ernic):

```bash
sudo python3 scripts/setup-vm-rocm-ernic.py
```

Use `--no-mount`, `--no-packages`, `--no-driver`, etc. to skip steps. See
script help: `python3 scripts/setup-vm-rocm-ernic.py --help`.

### Run from host via SSH

From the host, you can run the setup script inside the VM over SSH. Use the
same SSH port as when starting the VM (e.g. `SSH_PORT=2222` in
`run-vm-vfio-user.sh`). Replace `USER` with the VM login user (e.g. `ubuntu`
or `stebates`) and `PORT` with the forwarded port (e.g. `2222`).

**Option A – VM started with hostfs (recommended)**  
Start the VM with `FILESYSTEM` set to the host directory that contains
rocm-ernic (e.g. `FILESYSTEM=$HOME/Projects ./scripts/run-vm-vfio-user.sh`).
Then from the host, mount hostfs in the guest and run the script:

```bash
# From host (one line). Creates ~/Projects in the VM and mounts hostfs there.
ssh -o StrictHostKeyChecking=no -p PORT USER@localhost \
  'sudo mkdir -p ~/Projects && sudo mount -t 9p -o trans=virtio,version=9p2000.L,uid=$(id -u),gid=$(id -g) hostfs ~/Projects 2>/dev/null || true; cd ~/Projects/rocm-ernic && sudo python3 scripts/setup-vm-rocm-ernic.py --no-mount'
```

If the VM user’s home differs when using `sudo`, use the full path, e.g.
`/home/ubuntu/Projects` and `/home/ubuntu/Projects/rocm-ernic`, and set
`uid`/`gid` to that user’s IDs.

**Option B – Copy repo into VM, then run**  
If the VM was started without hostfs, copy the repo into the VM and run the
script there (mount step will be skipped or will fail; use `--no-mount`):

```bash
# From host
scp -o StrictHostKeyChecking=no -P PORT -r /path/to/rocm-ernic USER@localhost:/tmp/
ssh -o StrictHostKeyChecking=no -p PORT USER@localhost \
  'sudo python3 /tmp/rocm-ernic/scripts/setup-vm-rocm-ernic.py --no-mount'
```

Example with port 2222 and user `stebates`:

```bash
ssh -o StrictHostKeyChecking=no -p 2222 stebates@localhost \
  'sudo mkdir -p ~/Projects && sudo mount -t 9p -o trans=virtio,version=9p2000.L,uid=$(id -u),gid=$(id -g) hostfs ~/Projects 2>/dev/null || true; cd ~/Projects/rocm-ernic && sudo python3 scripts/setup-vm-rocm-ernic.py --no-mount'
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

To have `lspci` show "ROCm Emulated RDMA NIC" for device 0x1022:0x8000, merge
the fragment from the project into your system pci.ids:

- Fragment: [../scripts/pci.ids.rocm-ernic](../scripts/pci.ids.rocm-ernic)
- Add the device line `8000  ROCm Emulated RDMA NIC` under vendor `1022` (AMD)
  in your system pci.ids (e.g. `/usr/share/hwdata/pci.ids` or
  `/usr/share/misc/pci.ids`). You can merge the fragment or add the line
  manually.
- For inclusion in the official PCI ID database, submit 0x1022/0x8000 with
  name "ROCm Emulated RDMA NIC" at <https://admin.pci-ids.ucw.cz/> (see
  <https://pci-ids.ucw.cz/> for instructions).

## Unprivileged dmesg (optional)

For rocm-ernic testing, you may want any user to run `dmesg` without root.

- **Option A (e.g. Ubuntu 24.04):** Allow all users to read the kernel log:
  - Temporarily: `sudo sysctl -w kernel.dmesg_restrict=0`
  - Persistently: copy [../scripts/99-rocm-ernic-dmesg.conf](../scripts/99-rocm-ernic-dmesg.conf)
    to `/etc/sysctl.d/` and reboot or run
    `sudo sysctl -p /etc/sysctl.d/99-rocm-ernic-dmesg.conf`.
- **Option B:** On distros where the `adm` group can read kernel logs, add
  the test user to group `adm` and re-login.

## Interface naming (udev)

To rename the ROCm ERNIC ethernet interface to `rocXsY` (e.g. `roc0s0`):

1. Copy the udev rule into place (from the project root):
   ```bash
   sudo cp scripts/85-rocm-ernic-net.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   ```
2. After loading the driver (or on next boot), the interface will appear as
   `roc0s0`. If it was already up as enp0s4, reload the driver or run
   `sudo ip link set enp0s4 name roc0s0` once.

Rule file: [../scripts/85-rocm-ernic-net.rules](../scripts/85-rocm-ernic-net.rules).

## Network (netplan)

To get an IPv4 address via DHCP on the ROCm ERNIC interface (e.g. `roc0s0`),
add a netplan stanza and apply it:

1. Use the example snippet [../scripts/netplan-rocm-ernic.yaml.example](../scripts/netplan-rocm-ernic.yaml.example)
   and merge it into your `/etc/netplan/*.yaml` or add it as a new file under
   `/etc/netplan/`.
2. Run: `sudo netplan apply`

The interface will then receive an IPv4 address from DHCP.
