# ROCm ERNIC RDMA Test Report

**Date:** 2026-03-25
**Host:** hpe-rack-15.adc.amd.com
**Branch:** `ping-pong` (tracking `dev/stebates/ping-pong`)

## Environment

| Component          | Value                                      |
|--------------------|--------------------------------------------|
| Host kernel        | 6.8.0-31-generic (Ubuntu 24.04)            |
| VM kernel          | 6.17.0-19-generic                          |
| QEMU               | 10.2.2-pci-mmio-bridge-submit              |
| rocm-ernic server  | commit 56b5cf8 (rebase: fix kernel CI)     |
| rdma-core          | v62.0 + rocm\_ernic, IOCTL\_MODE=write     |
| Kernel driver      | rocm\_ernic\_eth + rocm\_ernic\_rdma (OOE)  |
| Instances          | 4 (1 manager + 3 workers)                  |
| VMs                | 2 (instance 1 on port 2250,                |
|                    | instance 2 on port 2251)                   |

### VM Network Configuration

| VM   | SSH Port | RDMA NIC IP        | Overlay IP          |
|------|----------|--------------------|---------------------|
| VM1  | 2250     | 192.168.200.10/24  | 192.168.100.10/24   |
| VM2  | 2251     | 192.168.200.20/24  | 192.168.100.20/24   |

### RDMA Device Info

```
hca_id:    rocep1s0
transport: InfiniBand (0)
vendor_id: 0x1022  device_id: 0x8000
hw_ver:    0x1
port 1:    PORT_ACTIVE, link_layer Ethernet
GID[0]:    fe80::200:ff:fe00:9c20 (VM1)
           fe80::200:ff:fe00:207c (VM2)
```

## Test Results

### ibv_rc_pingpong (RC Send/Recv)

All tests **PASS**. 20 iterations each, both
directions verified.

| Msg Size | Bytes Xfer | Throughput   | Latency/iter |
|----------|------------|--------------|--------------|
| 64 B     | 2,560      | 0.01 Mbit/s  | 78.0 ms      |
| 256 B    | 10,240     | 0.05 Mbit/s  | 80.0 ms      |
| 1024 B   | 40,960     | 0.21 Mbit/s  | 78.0 ms      |
| 4096 B   | 163,840    | 0.84 Mbit/s  | 78.1 ms      |
| 8192 B   | 327,680    | 1.68 Mbit/s  | 78.0 ms      |

**Observations:**
- Latency is consistent at ~78 ms/iter regardless of
  message size. This is dominated by the vfio-user +
  TCP backend round-trip, not the payload size.
- Throughput scales linearly with message size as
  expected for a latency-bound path.
- Matches the commit-verified performance
  (74 ms/iter for 4096 B in commit d0a9518).

### Ethernet Ping (ICMP via RDMA NIC)

| Path         | Avg RTT   | Loss |
|--------------|-----------|------|
| Overlay      | 0.29 ms   | 0%   |
| RDMA NIC     | 62.2 ms   | 0%   |

### Other Tests

| Test                  | Result       | Notes                     |
|-----------------------|--------------|---------------------------|
| `ibv_devices`         | **PASS**     | Device `rocep1s0` visible |
| `ibv_devinfo`         | **PASS**     | PORT\_ACTIVE, Ethernet    |
| `ibv_ud_pingpong`     | **HANG**     | UD QP type not supported  |
| `ibv_srq_pingpong`    | Not tested   | SRQ path untested         |
| `ib_send_lat`         | **HANG**     | perftest conn. setup fail |
| `ib_send_bw`          | **HANG**     | perftest conn. setup fail |
| `ib_write_lat`        | **HANG**     | perftest conn. setup fail |
| `ib_read_lat`         | **HANG**     | perftest conn. setup fail |
| `rping`               | Not tested   | RDMA CM path untested     |

**Note on perftest failures:** The perftest tools
(`ib_send_bw`, `ib_send_lat`, etc.) use a different
TCP socket exchange protocol than `ibv_rc_pingpong`.
When the server process is backgrounded inside an SSH
session, it gets killed on session close. Running the
server via a locally-backgrounded SSH (`ssh host 'cmd'
&`) works; using `ssh host 'cmd &'` or `nohup` does
not. Perftest may also require QP features (e.g.
inline data, specific QP caps) not yet implemented.

## Server Statistics (Post-Test)

### Instance 1 (Manager) -- VM1

```
connection  : connected (device reset)
commands    : 296
uar_writes  : 6651
interrupts  : 323
```

User QPs created by ibv\_rc\_pingpong (6 tests):

| QP   | send DB | recv DB | WQEs  | CQEs | Opcode |
|------|---------|---------|-------|------|--------|
| 105  | 5       | 500     | 501   | 10   | SEND   |
| 108  | 20      | 500     | 516   | 40   | SEND   |
| 109  | 20      | 500     | 516   | 40   | SEND   |
| 110  | 20      | 500     | 516   | 40   | SEND   |
| 111  | 20      | 500     | 516   | 40   | SEND   |
| 112  | 20      | 500     | 516   | 40   | SEND   |

### Instance 2 (Worker) -- VM2

```
connection  : connected (device reset)
commands    : 233
uar_writes  : 6149
interrupts  : 254
```

User QPs (matching):

| QP   | send DB | recv DB | WQEs  | CQEs | Opcode |
|------|---------|---------|-------|------|--------|
| 106  | 5       | 500     | 501   | 10   | SEND   |
| 107  | 20      | 500     | 516   | 40   | SEND   |
| 108  | 20      | 500     | 516   | 40   | SEND   |
| 109  | 20      | 500     | 516   | 40   | SEND   |
| 110  | 20      | 500     | 516   | 40   | SEND   |
| 111  | 20      | 500     | 516   | 40   | SEND   |

## Key Findings

### 1. rdma-core must be built with IOCTL_MODE=write

The emulated PVRDMA device uses the legacy write-based
uverbs interface. Building rdma-core with the default
ioctl mode causes struct size mismatches because the
kernel UAPI headers in rdma-core v62 contain flexible
array members (`__aligned_u64 driver_data[]`) that
collapse embedded response structs to their base size.
The project's `build-rdma-core.sh` correctly sets
`-DIOCTL_MODE=write`.

### 2. Stale QEMU processes cause cascading failures

When `ernicctl` restarts the service, any QEMU
processes from the previous run continue holding disk
image locks and SSH port bindings. New VM launches
fail silently. The `vm-stop` command only sends QMP
quit to known QMP sockets; stale processes connected
to old (deleted) sockets are orphaned.

**Recommendation:** `ernicctl vm-launch` should check
for existing QEMU processes using the same disk image
or SSH port before starting a new one.

### 3. VM driver auto-load needed

The kernel modules are loaded via `insmod` (not DKMS)
so they don't persist across VM reboots. Each reboot
requires manual driver installation.

**Recommendation:** Run `vm-driver-install.sh --dkms`
to install via DKMS for persistence.

### 4. env file uses $HOME (breaks under sudo)

`ERNIC_QEMU_MINIMAL`, `ERNIC_VM_IMAGE_DIR`, and
`ERNIC_QEMU_PATH` used `$HOME` which resolves to
`/root` under sudo. Fixed during this session to use
absolute paths.

### 5. ERNIC_VM_MCAST_GROUP must be enabled

The multicast overlay network
(`ERNIC_VM_MCAST_GROUP=230.0.0.1:1234`) is required
for inter-VM L2 connectivity. It was commented out in
the default env file.

### 6. Server backgrounding via SSH

Backgrounding the ibv\_rc\_pingpong server inside an
SSH session (`ssh host 'cmd &'`) causes the server to
be killed when SSH closes. The working pattern is to
background the entire SSH command locally:
`ssh host 'cmd' &`.
