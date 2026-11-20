# devmem-TCP TX on real Pensando hardware — runbook (ENVIRONMENT 2)

> # NOT RUN. NO HARDWARE ON THIS HOST.
>
> **Every command in this document is UNEXECUTED.** There is no Pensando
> (ionic) NIC and no AMD Instinct GPU on the machine where this was written;
> the only ionic in reach is the `rocm-ernic` **emulator** behind vfio-user in
> two QEMU guests (ENVIRONMENT 1). Nothing below has been run against
> firmware, so no output, no counter value and no pass/fail in this file is an
> observation — the "expected" columns are predictions carried over from the
> emulated runs ([test-tx-7.2.15.md](test-tx-7.2.15.md),
> [test-tx-7.4-rc3.md](test-tx-7.4-rc3.md),
> [test-tx-verification.md](test-tx-verification.md)) and from source reading
> ([ionic-tx-audit.md](ionic-tx-audit.md),
> [ionic-tx-device-contract.md](ionic-tx-device-contract.md)).
>
> Treat a disagreement between this file and the hardware as the hardware
> being right, and record it in §10.
>
> Mark each case NOT RUN / PASS / FAIL as you go. Nothing here has an
> "already passed" status to inherit.

Scope: transmit from a dma-buf (`NETDEV_CMD_BIND_TX` +
`sendmsg(MSG_ZEROCOPY)` + `SCM_DEVMEM_DMABUF`) on a real ionic NIC, with the
patch series that was developed and exercised only against the emulator. RX
(header split) is **out of scope** — ionic has no RX devmem support at all;
see [rx-design.md](rx-design.md) and §9 for what firmware would have to
provide.

Two hosts are assumed, `TXHOST` (the sender, with the NIC and optionally the
GPU) and `RXHOST` (an ordinary peer; its kernel and NIC do not matter — the
receiver is plain TCP). A single host with two cards in different namespaces
is **not** a valid substitute for M8 or for the performance row.

---

## 0. Before anything: what to copy to the hardware host

Everything the matrix needs is on this host under `T=/var/tmp/ionic-devmem`.
Nothing is installed from a package; copy sources and build on the target, or
copy the static binaries.

| What | Path here | Use |
| --- | --- | --- |
| devmem TX sender/receiver | `T/test/src/devmem_tx.c` → `T/test/bin/devmem_tx` (static, sha256 `1579ebae…e9aa1e`) | every M-case |
| amdgpu dma-buf variant of the sender | `T/test/src/gpu/devmem_tx_gpu.c` | §8 (GPU source) |
| packet capture | `T/test/src/pcapcap.c` → `T/test/bin/pcapcap` | M7, M8 (on hardware, prefer plain `tcpdump` — `pcapcap` exists because the emulator's tap had no tcpdump) |
| upstream selftest | `T/test/bin/ncdevmem` (static, built from v7.4-rc3 selftests by `T/test/scripts/build-ncdevmem.sh`, sha256 `1cdf3e86…ef4db`) | M1, M2 |
| independent verifier | `T/test/evidence/tx-verify/src/{vtx.c,vrx.py,vprobe.bt,dmamap.bt,v.sh}` | the negative control and the "is it really devmem" proof |
| harness | `T/test/scripts/devmem-tx/{lib.sh,probe.bt,m3-m5.sh,m6.sh,m7.sh,m7x.sh,m8.sh,m9.sh,m9-rmmod.sh,m10.sh,summarize.py,pcapsum.py,install.sh}` | the drivers for each case |
| mainline series | `T/patches/mainline/0000-cover-letter.patch`, `0001..0006` | v7.4-rc target |
| DKMS backport | `T/patches/ernic-v7.2.6/0001..0007`, also in rocm-ernic at `ansible/files/patches/devmem-tx/` (+ its `README.md`) | v7.2.x bundle target |
| XDP objects and checker | `T/test/src/xdp/`, `T/test/bin/xdp/*.o` | the XDP interaction row |

Copy, e.g.:

```bash
TXH=user@txhost
tar -C /var/tmp/ionic-devmem -czf - \
    test/src test/bin test/scripts/devmem-tx test/evidence/tx-verify/src \
    patches/mainline patches/ernic-v7.2.6 \
  | ssh "$TXH" 'mkdir -p ~/devmem && tar -C ~/devmem -xzf -'
scp /var/tmp/ionic-devmem/test/src/devmem_tx.c "$RXH":  # receiver side
```

**The harness scripts are written for the two-guest testbed**: they call
`gssh N`, read emulator stats out of a docker container and drive two VMs by
name. On hardware either (a) run the `devmem_tx` command lines by hand — they
are quoted in full in §5 — or (b) adapt `lib.sh`: replace `G1`/`G2` with
`ssh $TXH`/`ssh $RXH`, delete `emu_stats`/`cap_start`/`cap_stop`, and keep the
marker, counter-diff, bpftrace and DMA-API logic unchanged. Option (b) is
worth the hour: the counter arithmetic is where the result actually lives.

Build on the target:

```bash
cd ~/devmem/test/src && gcc -O2 -Wall -Wextra -static -o devmem_tx devmem_tx.c
cd ~/devmem/test/evidence/tx-verify/src && gcc -O2 -Wall -Wextra -static -o vtx vtx.c
```

The target needs: `gcc`, `make`, kernel headers for the running kernel,
`ethtool`, `bpftrace` (with BTF — `/sys/kernel/btf/vmlinux` must exist),
`tcpdump`, `nstat`, `python3`, and `mpstat` (sysstat) and `perf` for §8.

---

## 1. Record the device before touching it

Everything in this section is read-only. Save it; the matrix is
uninterpretable without it, and §9's questions are answered out of it.

```bash
IF=enp...            # the ionic netdev
PCI=$(basename "$(readlink -f /sys/class/net/$IF/device)")
OUT=~/devmem/evidence/H0-device-identity.txt
{
  echo "== uname"; uname -a
  echo "== ethtool -i"; ethtool -i "$IF"
  echo "== ethtool -k (offloads as found)"; ethtool -k "$IF"
  echo "== ethtool -l (channels)"; ethtool -l "$IF"
  echo "== ethtool -g (rings)"; ethtool -g "$IF"
  echo "== ethtool -S (counters at rest)"; ethtool -S "$IF"
  echo "== devlink dev info"; devlink dev info "pci/$PCI"
  echo "== devlink dev param / resource"; devlink dev param show "pci/$PCI"; devlink resource show "pci/$PCI"
  echo "== lspci"; lspci -nn -s "$PCI"; lspci -vvv -s "$PCI"
  echo "== PCI link"; lspci -vv -s "$PCI" | grep -E 'LnkCap|LnkSta|MaxPayload|MaxReadReq|DevCtl'
  echo "== numa/irq"; cat /sys/class/net/$IF/device/numa_node; grep "$IF" /proc/interrupts | head -40
  echo "== driver module"; modinfo ionic | head -20
  echo "== netmem_tx"; cat /sys/class/net/$IF/... 2>/dev/null   # not exposed in sysfs; see below
  echo "== dmesg for the probe"; dmesg | grep -i ionic
} > "$OUT" 2>&1
```

`netmem_tx` is **not** in sysfs. Read it from netlink instead — it is what
decides whether `bind-tx` can work at all:

```bash
# ynl from the kernel tree (tools/net/ynl), or the selftest's own probe:
python3 ./tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/netdev.yaml \
        --dump dev-get | grep -A3 -B6 "ifindex: $(cat /sys/class/net/$IF/ifindex)"
```

### 1.1 The feature bits that decide the outcome

The series sets `netdev->netmem_tx = NETMEM_TX_DMA` **only** when the LIF was
granted both scatter-gather and hardware checksum. On firmware that grants
neither, `bind-tx` fails and the whole matrix is unreachable; on firmware that
grants them and then revokes them (reset), see finding **V5** in §7.

Record, from `dmesg`, from `ethtool -k` and (if you can) from a
`LIF_IDENTIFY`/`LIF_SETATTR` trace, which of these the card granted:

| Bit | `IONIC_ETH_HW_*` | Why it matters here |
| --- | --- | --- |
| 5 | `TX_SG` | **required.** Without `NETIF_F_SG` the core refuses a devmem `sendmsg` with `-EINVAL` before any skb exists, and the series will not set `netmem_tx` |
| 7 | `TX_CSUM` | **required.** The data is unreadable, so the stack can never checksum in software; `CHECKSUM_PARTIAL` must be offloaded |
| 8 / 9 | `TSO` / `TSO_IPV6` | M4, M5, M10 and the whole performance row; not required for M3 |
| 10 | `TSO_ECN` | ECN under TSO; not exercised in the emulated runs |
| 11–16 | tunnel TSO / ENCAP bits | not exercised; see §9 question B |
| 4 | `RX_CSUM` | not needed for TX, but record it: it is a hard prerequisite for any future RX work |
| 3 | `RX_HASH` | RSS; only relevant to RX and to the steering questions in §9 |
| 0–2 | VLAN bits | the VLAN-TSO checksum question, §9 A10 — **the highest-value firmware test in these notes** |
| 20 | `TIMESTAMP` | the hwstamp queue, never exercised with devmem |

Also record the TXQ `Q_IDENTIFY` answer (version, `max_sg_elems`,
`sg_desc_sz`, `sg_desc_stride`, `features`). On the emulator these are a
guess; on hardware they are the real contract, and `max_sg_elems` below 15 or
a `version`/`sg_desc_sz` mismatch is a correctness problem in the **existing**
driver, not in this series (§9 C21–C24).

### 1.2 IOMMU state — record it for every single run

```bash
cat /proc/cmdline                       # intel_iommu=/amd_iommu=, iommu.strict=, iommu.passthrough=
ls -d /sys/kernel/iommu_groups/* | wc -l
cat /sys/class/iommu/*/device/.. 2>/dev/null
dmesg | grep -iE 'iommu|dmar|ivrs' | head -20
cat /sys/bus/pci/devices/$PCI/iommu_group/type   # DMA / DMA-FQ / identity
```

Report it as `IOMMU=<off|identity|DMA(strict)|DMA-FQ(lazy)>` in every result
line. Two reasons: the throughput of §8 moves by tens of percent between
strict and lazy; and **the PFN check of §6.2 is only valid with no IOMMU** —
with one, the addresses the driver puts in descriptors are IOVAs and cannot be
compared with the dma-buf's PFNs.

---

## 2. Target A — mainline series on a v7.4-rc kernel

```bash
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux && git checkout v7.4-rc3
git am ~/devmem/patches/mainline/000[1-6]-*.patch      # 0000 is the cover letter
```

Config requirements (the emulated runs used exactly these):

```
CONFIG_IONIC=m  CONFIG_NET_DEVMEM=y  CONFIG_PAGE_POOL=y  CONFIG_UDMABUF=y
CONFIG_MEMFD_CREATE=y  CONFIG_DMA_SHARED_BUFFER=y
CONFIG_DEBUG_NET=y  CONFIG_DMA_API_DEBUG=y
CONFIG_FTRACE=y CONFIG_FUNCTION_TRACER=y CONFIG_KPROBES=y CONFIG_KPROBE_EVENTS=y
CONFIG_FPROBE=y CONFIG_BPF_SYSCALL=y CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y CONFIG_DEBUG_INFO_BTF_MODULES=y
# the debug leg (run the matrix twice, once with these, once without):
CONFIG_KASAN=y (generic, inline)  CONFIG_PROVE_LOCKING=y  CONFIG_DEBUG_ATOMIC_SLEEP=y
# for the GPU row (§8) only:
CONFIG_ZONE_DEVICE=y  CONFIG_PCI_P2PDMA=y  CONFIG_MEMORY_HOTPLUG=y  CONFIG_MEMORY_HOTREMOVE=y
```

`CONFIG_DMA_API_DEBUG_SG` is dropped by `olddefconfig` in this config; if you
want it, set it after `olddefconfig` and rebuild.

Rebuilding only the module, against an installed v7.4-rc kernel, is enough for
every case except the ones that need a patched core:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD/drivers/net/ethernet/pensando/ionic modules
sudo rmmod ionic && sudo insmod ./ionic.ko
```

Build the **negative control** too — the same series with **0005**
(`dont-send-an-unreadable-skb-that-has-no-frags`) removed. Without it the M7
control is not runnable, and without that control the matrix proves nothing
(§7, F-control).

## 3. Target B — DKMS backport on the v7.2.15 bundle kernel

Seven patches, `T/patches/ernic-v7.2.6/` (identical set staged in rocm-ernic
at `ansible/files/patches/devmem-tx/`; read its `README.md`). They apply to
**v7.2.6** of the stable tree — the ref `IONIC_KERNEL_REF` pins in
`cmake/ErnicKernelModule.cmake` — with or without the `srq` and `p4` series
below them. Give it as the **last** `--patches-dir`.

```bash
# on a build host:
rocm-ernic/ansible/files/scripts/fetch-ionic-sources.sh \
    --source-dir /tmp/ionic-src --kernel-ref v7.2.6 \
    --patches-dir <pin>/srq --patches-dir <pin>/p4 \
    --patches-dir ~/devmem/patches/ernic-v7.2.6
# on the hardware host:
scp -r /tmp/ionic-src rocm-ernic/ansible/files/scripts/setup-ionic-dkms.sh "$TXH":
ssh "$TXH" 'sudo ./setup-ionic-dkms.sh --source-dir ~/ionic-src --kernel-ref v7.2.6'
```

The running kernel must have netmem TX (`netdev->netmem_tx`,
`netmem_dma_unmap_addr_set()`, `netmem_dma_unmap_page_attrs()` in
`<net/netmem.h>`). v7.2.6 and v7.2.15 have them.

Note 0001 (`use netif_txq_maybe_stop in ionic_tx`) is upstream as
`c91b4d6e5cc3` / `e247d033dd32`; it changes the Tx path of **every** guest
built with this series, devmem or not. The backport's backstop patch is
**0006**, not 0005 — mind the numbering when you build the no-backstop
control.

Confirm which module is actually loaded by its GNU build-id, not by
`srcversion` (these modules are built without one):

```bash
cat /sys/module/ionic/notes/.note.gnu.build-id | xxd | tail -2
readelf -n ./ionic.ko | grep -A1 'Build ID'
```

---

## 4. Standing setup for every case

```bash
IF=enp...; PEER=<RXHOST ip on the same L2>; MYIP=<TXHOST ip>
sudo ethtool -K $IF tx on sg on tso on gso on gro on
sudo ethtool -G $IF rx 4096                 # see F-ring in §7
sudo ip link set $IF mtu 1500
sudo sysctl -w net.ipv4.tcp_retries2=15     # 5 for the negative cases only
```

Per case, in **both** hosts:

```bash
MARK="DEVMEM-TX-CASE $name $(date +%s.%N)"; echo "$MARK" | sudo tee /dev/kmsg
# ... run the case ...
sudo dmesg | sed -n "/$MARK/,\$p" | tail -n +2 | cut -c1-240
```

and snapshot before/after: `ethtool -S $IF`,
`/sys/class/net/$IF/statistics/*`, `nstat -az`,
`/sys/kernel/debug/dma-api/error_count`,
`awk '/^Total/{print $2}' /sys/kernel/debug/dma_buf/bufinfo`.

**Between cases, wait for `bufinfo` Total to return to 0.** A stalled devmem
connection lingers as an orphan (75–80 s at default sysctls, ~15 min at
`tcp_retries2=15`) holding its dma-buf, and retransmits into the next case
(finding F6).

---

## 5. The matrix, M1–M10

Plan options are identical on both sides: `-S` dma-buf bytes, `-P` seed,
`-o` first offset, `-l` bytes per `sendmsg`, `-z` iovec length, `-g` gap
between iovecs, `-a` offset advance per send, `-n` sends (receiver `0` =
until EOF). Sender form used everywhere:

```bash
sudo devmem_tx -f $IF -s $PEER -p $PORT <PLAN> -D -F /tmp/dtx-pfns <EXTRA>
```
receiver, on RXHOST:
```bash
devmem_tx -r -p $PORT <PLAN> <RX EXTRA>
```

"clean" below means: no new kernel-log line past the marker on either host;
DMA-API `error_count` delta minus the bind-time sg-segment count = 0;
`bufinfo` back to 0; bind/unbind/free counts 1/1/1; and the NIC's own error
counters (`ethtool -S`: `hw_tx_dropped`, `tx_desc_data_error`,
`tx_desc_fetch_error`, `tx_dma_error` or whatever this firmware names them —
record the mapping in §1) unchanged. **On hardware these firmware counters
replace the emulator's `/run/ernic-stats/ernic-N.stats` file**; there is no
other channel, because the driver never reads TX `comp->status` (§9 A4).

### M1 — negative baseline, unpatched driver must refuse

```bash
sudo modprobe -r ionic; sudo modprobe ionic        # stock driver
printf 'hello devmem tx line 1\nhello devmem tx line 2\n' > /tmp/devmem-tx.txt
sudo ncdevmem -f $IF -s $PEER -p 5302 -t 0 -q 1 < /tmp/devmem-tx.txt
sudo strace -f -s 256 -e trace=sendto,sendmsg,recvmsg,recvfrom \
     ncdevmem -f $IF -s $PEER -p 5302 -t 0 -q 1 < /tmp/devmem-tx.txt 2>&1 | tail -12
```
**Expected:** `netdev_bind_tx: Operation not supported`, extack
`Driver does not support netmem TX`, netlink `error=-EOPNOTSUPP`; receiver
gets nothing.  Status: **NOT RUN**

### M2 — positive baseline with the stock selftest

Load the patched module, then the same command. **Expected:**
`got tx dmabuf id=…`, every send completed, receiver sha256 equal. Use a file
of at least a few hundred KB — a 46-byte run was judged weak evidence in the
emulated matrix.

The **first** bind on a given boot is expected to emit the DMA-API
sg-segment warning of finding **F1**; see §7.

`devmem.py` from the selftests **cannot** be used: `require_devmem()` runs the
RX probe, which needs header split, and every test SKIPs.  Status: **NOT RUN**

### M3 — TSO off / TSO+GSO off, csum on, MTU 1500

```bash
BULK="-S 16m -l 1048576 -n 128 -a 65536"        # 128 MiB, page-aligned
ODD="-S 16m -o 123 -l 100003 -a 7919 -n 600"    # 57 MiB, nothing aligned
sudo ethtool -K $IF tx on sg on; sudo ethtool -K $IF tso off gso on   # then tso off gso off
# per stream: start the receiver, then
sudo devmem_tx -f $IF -s $PEER -p 5410 $BULK -D -F /tmp/dtx-pfns
sudo devmem_tx -f $IF -s $PEER -p 5411 $ODD  -D -F /tmp/dtx-pfns
```
**Expected:** 0 mismatches; all skbs non-GSO with `ip_summed ==
CHECKSUM_PARTIAL`; clean.  Status: **NOT RUN**

### M4 — the same two streams, TSO on

**Expected:** GSO skbs reach the driver with up to 17 frags; `tx_N_linearize`
+0; `tx_unreadable_drop` +0; netdev `tx_dropped` +0; 0 retransmissions.
Do **not** read `tx_tso_bytes` as a byte count (finding V4).  Status: **NOT RUN**

### M5 — jumbo MTU

MTU 9000 TSO on / TSO off and MTU 9198 TSO on (9198 is the ionic maximum),
each with both streams, each preceded by

```bash
ping -c 2 -q -M do -s $((MTU - 28)) $PEER
```
On hardware also raise the MTU on every switch port and on RXHOST. **Expected:**
PASS ×6 — but see finding **V1**: at MTU 9000 the frag limit bites at a much
larger piece size (~600 B), so small-iovec shapes that pass at 1500 fail here
**by design**.  Status: **NOT RUN**

### M6 positive — 9 unaligned / many-frag shapes

```bash
devmem_tx ... -p 5420 -S 4m -o 1    -l 1       -a 4097   -n 300  -N -w 0   # 300 × 1 byte, odd offsets
devmem_tx ... -p 5421 -S 4m -o 4095 -l 2       -a 4096   -n 300  -N -w 0   # 2 bytes across a page
devmem_tx ... -p 5422 -S 16m -o 123 -l 100003  -a 7919   -n 500            # one iovec over 25 pages
devmem_tx ... -p 5423 -S 16m -o 3001 -l 250007 -z 5003 -g 1237 -a 104729 -n 300
devmem_tx ... -p 5424 -S 4m -o 4190209 -l 4095 -n 50     -N -w 0           # last bytes of the dma-buf
devmem_tx ... -p 5425 -S 16m -o 7 -l 61440 -z 128 -g 64 -a 8191 -n 500     # 480 iovecs, TSO on
devmem_tx ... -p 5426 -S 4m -o 9 -l 960 -z 64 -g 4032 -a 4096 -n 300 -N -w 0  # exactly 15 frags
devmem_tx ... -p 5427 -S 16m -o 77 -l 1433600 -z 1400 -g 2696 -a 4099 -n 40   # 1024 iovecs
devmem_tx ... -p 5428 <as 5425, TSO off>
```
**Expected:** 0 mismatches, 0 drops, clean. The "exactly 15 frags" case only
holds if no iovec crosses a page (finding **V2**).  Status: **NOT RUN**

### M6 negative — above the 15-frag device limit (a PASS case)

```bash
sudo sysctl -w net.ipv4.tcp_retries2=5
devmem_tx ... -p 5433 -S 4m -o 9 -l 1024 -z 64 -g 4032 -a 4096 -n 300 -N -w 0 -t 10 -C 3   # 16 frags
devmem_tx ... -p 5430 -S 4m -o 5 -l 16384 -z 16 -a 4099 -n 64 -t 10 -C 3                   # 17 frags
devmem_tx ... -p 5431 <same, blocking>  -C 20
devmem_tx ... -p 5432 -S 16m -o 123 -l 100003 -a 7919 -n 500        # positive straight after
sudo sysctl -w net.ipv4.tcp_retries2=15
```
**Expected:** every over-limit skb is dropped **by the driver and counted** —
`tx_unreadable_drop` rises by exactly the number of transmission attempts,
`tx_N_linearize` +0, netdev `tx_dropped` +0, receiver gets 0 bytes, no crash,
no log line, no DMA-API report, and the positive run straight after passes.
**This is where real firmware may differ from the emulator**: if this card's
`max_sg_elems` is not 15 (§9 C21), recompute the limit before judging.
Status: **NOT RUN**

### M6 threshold — where the limit bites

512 iovecs of 80 / 90 / 100 / 110 B per send at MTU 1500, `tcp_retries2=5`.
**Expected** (finding V1): pieces below **MSS/15** stall — ~97 B at MTU 1500,
~600 B at MTU 9000. Re-derive the threshold from this card's real MSS and
real `max_sg_elems` rather than copying the numbers.  Status: **NOT RUN**

### M7 — TX checksum and/or SG offload turned off (the silent-corruption trap)

15 cases: `tx off`, `sg off`, `tx off sg off`, each also with `gso off`, each
set **before connect** (100 KB sends and 1 KB sends) and again **3 s into a
stream**; capture on the sender's netdev and on the receiver.

```bash
BIG="-S 4m -o 123 -l 100003 -a 7919 -n 20"; SMALL="-S 4m -o 123 -l 1000 -a 7919 -n 5"
sudo ethtool -K $IF <COMBO>
sudo tcpdump -i $IF -U -w /tmp/m7.pcap tcp port 5450 &
sudo devmem_tx -f $IF -s $PEER -p 5450 $BIG   -D -t 6 -C 3
sudo devmem_tx -f $IF -s $PEER -p 5451 $SMALL -D -N -w 0 -t 6 -C 3
# mid-stream: the same with the ethtool -K run 3 s after the sender starts
```

**Expected:** nothing corrupt reaches the receiver in any of the 15 cases —
no all-zero payload, no bad TCP checksum on the wire, `TcpInCsumErrors` +0 on
the receiver. The split of *who* stops it is the whole point:

| Features off | skb | Stopped by | Signature |
| --- | --- | --- | --- |
| `tx` | GSO | core, `skb_segment()` | netdev `tx_dropped` rises; **0** unreadable skbs at `ionic_start_xmit` |
| `tx` | sub-MSS | core, `validate_xmit_skb` | netdev `tx_dropped` |
| `sg` | GSO | core, `skb_segment()` | netdev `tx_dropped` |
| `sg` | sub-MSS | core, linearize fails | netdev `tx_dropped` |
| **`tx`+`sg`** | **GSO** | **the driver backstop (0005 mainline / 0006 backport)** | `skb_copy_and_csum_bits` count = skbs at the driver = `tx_unreadable_drop` |
| `tx`+`sg` | sub-MSS | core | netdev `tx_dropped` |

Invariant to check on every row: a **driver** drop moves
`tx_unreadable_drop` and **not** netdev `tx_dropped`; a **core** drop moves
netdev `tx_dropped` and **not** `tx_unreadable_drop`.  Status: **NOT RUN**

### M7 control — run this FIRST, it is what makes the matrix non-vacuous

Load the module built **without** the backstop patch (0005 mainline / 0006
backport), `ethtool -K $IF tx off sg off`, and send.

```bash
sudo ethtool -K $IF tx off sg off
sudo devmem_tx -f $IF -s $PEER -p 5455 -S 4m -o 123 -l 100003 -a 7919 -n 20 -D -t 6 -C 3
# receiver: vrx.py is the better oracle here
python3 vrx.py 5455 --idle 9
```
**Expected (this case must FAIL loudly):** megabytes of **all-zero** payload
arrive with **valid TCP checksums**, every completion reports success, and the
receiver reports a hash mismatch and non-zero `zero_runs_of_32`. If this case
*passes* (no zeros), your detector is broken or the backstop is still in the
module — stop and fix that before reading any other result.  Status: **NOT RUN**

### M8 — the route leaves through a different netdev

```bash
# a) bound on $IF, destination routed via the management NIC, no SO_BINDTODEVICE
sudo tcpdump -i $MGMT -U -w /tmp/m8a.pcap tcp port 2222 &
sudo devmem_tx -f $IF -s <mgmt-reachable-ip> -p 2222 -S 4m -o 123 -l 100003 -a 7919 -n 20 -B -t 6 -C 2
# b) move the route 3 s into a stream
sudo devmem_tx -f $IF -s $PEER -p 5461 -S 16m -o 123 -l 100003 -a 7919 -n 0 -B -T 12 -u 20000 -t 6 -C 3 &
sleep 3; sudo ip route add $PEER/32 via <gw> dev $MGMT
```
**Expected:** (a) connect succeeds, the **first `sendmsg` returns ENODEV**,
nothing queued, no payload frame on the other NIC. (b) the next `sendmsg`
returns ENODEV, already-queued unreadable skbs are freed in
`validate_xmit_skb` on the other netdev (its `tx_dropped` rises), its capture
contains **only control frames, none with payload**, and the receiver
verified the prefix.  Status: **NOT RUN**

### M9 — teardown, reconfiguration, unload

```bash
BULK="-S 16m -l 1048576 -a 65536 -n 64"; STREAM="-S 16m -o 123 -l 100003 -a 7919 -n 0"
for x in close-sock unbind free-dmabuf exit; do
  sudo devmem_tx -f $IF -s $PEER -p 5470 $BULK -D -w 1000 -X $x
done
# link down/up 3 s into a stream  (-R 20 retries refused sends: finding F4)
sudo devmem_tx -f $IF -s $PEER -p 5471 $STREAM -D -T 15 -u 5000 -R 20 -C 30 &
sleep 3; sudo ip link set $IF down; sleep 2; sudo ip link set $IF up
# 9 reconfigurations during a 25 s stream
for c in "ethtool -G $IF tx 512" "ethtool -G $IF rx 2048" "ethtool -G $IF tx 8192" \
         "ethtool -L $IF combined 2" "ethtool -L $IF combined 1" "ethtool -L $IF combined 4" \
         "ip link set $IF mtu 1400" "ip link set $IF mtu 1500" "ethtool -G $IF tx 1024 rx 1024"; do
  sudo $c; sleep 1.5
done
```
**Expected:** everything verified in every sub-case; `unbind` makes a send
with the stale id return **EINVAL**; only the driver's own "Changing …" lines
in the log. On hardware the ring sizes available differ — read `ethtool -g`
first and use real values.  Status: **NOT RUN**

### M9 rmmod — two sub-cases, (b) LAST

```bash
# (a) unbound, with unacked data queued: freeze the receiver, queue ~600 KB,
#     close the netlink socket, exit the sender, then unload.
sudo rmmod ionic; sudo insmod ./ionic.ko
# (b) live binding -- EXPECTED TO CRASH THE KERNEL (finding F8)
sudo devmem_tx -f $IF -s $PEER -p 5481 $STREAM -D -T 20 -u 5000 -t 8 -C 5 &
sleep 4; sudo rmmod ionic
```
**(b) is expected to oops** in `netdev_nl_sock_priv_destroy()` when the sender
closes its netlink socket — with KASAN, a use-after-free report first. This is
a **core** bug present in v7.2.15 and v7.4-rc3, not in this series, and it
applies to every netmem-TX driver on those kernels; net-next has since added
the teardown. On hardware the same path is reached by **PCI unbind / hot
unplug / an FLR-style firmware reset** — none of which was ever exercised in
the emulator (§7, "not run"). Either backport net-next's
`net_devmem_dev_unregister()` handling before running (a) and (b), or accept a
crashed host and reboot afterwards.  Status: **NOT RUN**

### M10 — sustained send

```bash
sudo devmem_tx -f $IF -s $PEER -p 5490 \
    -S 16m -o 17 -l 1000003 -z 250001 -g 4099 -a 65521 -n 0 -D -F /tmp/dtx-pfns \
    -T 360 -v -C 60
```
**Expected:** tens of GiB verified with 0 mismatches, sends = completions,
0 `SO_EE_CODE_ZEROCOPY_COPIED`, clean. Run it for at least an hour on
hardware, and run it once more at MTU 9000 with TSO on — that is the
configuration a deployment would use and the one the emulator could least
faithfully represent.  Status: **NOT RUN**

### XDP interaction row

Force one queue (`ethtool -L $IF combined 1`) so XDP frames and devmem skbs
share queue 0's descriptor-info slots, then run XDP_PASS / XDP_TX /
XDP_REDIRECT under a devmem stream, and attach/detach all three mid-stream.

```bash
sudo ip link set dev $IF xdpdrv obj ~/devmem/test/bin/xdp/xdp_tx.o sec xdp
sudo ip link set dev $IF xdp off
```
At MTU 9000 use the `*_frags` objects with `sec xdp.frags`. **Use distinct
per-datagram payloads**: identical ones almost entirely hide bug **B1** (§7).
Status: **NOT RUN**

---

## 6. Proving the skbs really are unreadable devmem skbs

A passing byte comparison proves nothing on its own: a silent copy fallback
would also deliver correct bytes. Six independent checks; run at least 1, 3
and 4 on hardware, and 2 only if there is no IOMMU.

### 6.1 bpftrace at the driver entry

`~/devmem/test/scripts/devmem-tx/probe.bt`, or the independent
`~/devmem/test/evidence/tx-verify/src/vprobe.bt` (preferred — it checks bit 0
of **every** frag's `netmem`, not only frag 0):

```bash
sudo bpftrace vprobe.bt            # keyed by TCP destination port and Tx queue
```
It reports, for `ionic_start_xmit`: the count and bytes of skbs with
`skb->unreadable` set, the frag-count histogram, GSO and `ip_summed`, whether
every frag's `netmem` has bit 0 set (net_iov), whether
`skb_copy_and_csum_bits` / `skb_checksum_help` / `skb_segment` /
`skb_copy_bits` were ever called on an unreadable skb, unreadable skbs freed
as drops (with reason and stack), and
`net_devmem_bind_dmabuf`/`_unbind_`/`__net_devmem_dmabuf_binding_free` counts
(must be 1/1/1 per case).

**Expected:** unreadable payload bytes ≥ bytes the receiver verified (equal
without retransmissions); the only readable skbs are handshake/FIN, a handful
of skbs under ~320 bytes total.

### 6.2 The descriptor-address check (no-IOMMU only)

`kretprobe:ionic_tx_map_frag.constprop.0` returns **the DMA address the driver
writes into the descriptor**. Compare its page number, keyed by the socket's
destination port, with the dma-buf's own PFNs that the sender dumped with
`-F /tmp/dtx-pfns`:

```
PFN-CHECK port P: dmabuf pages=N driver-frag pages=M outside-the-dmabuf=0 => PASS
```
`comm -23 probe.pfns dmabuf.pfns | wc -l` must be 0. **With an IOMMU this
check is meaningless** — the values are IOVAs. In that case either run one leg
with `iommu.passthrough=1`, or fall back to the net_iov bit check of 6.1.

### 6.3 One DMA map per skb, none per frag

```bash
sudo bpftrace ~/devmem/test/evidence/tx-verify/src/dmamap.bt
```
counts `dma_map_page_attrs` / `dma_map_phys` / `dma_map_sg_attrs` made
**inside** `ionic_start_xmit`. **Expected:** exactly one map per unreadable
skb (its linear header) and **none for the frags**, with unmaps matching.

### 6.4 Completions

Every send completes; `SO_EE_CODE_ZEROCOPY_COPIED` is never set. A devmem send
can never legitimately report COPIED (`skb_copy_ubufs()` returns `-EFAULT` for
unreadable skbs). The exception is finding **F4** after a route loss.

### 6.5 DMA-API error_count arithmetic, not the log

```bash
cat /sys/kernel/debug/dma-api/error_count     # before and after; -D brackets the bind
```
**DMA-API debug prints only while its `num_errors` budget lasts** — after the
first report a later error raises `error_count` and prints nothing. A clean
`dmesg` therefore says nothing; only the arithmetic does.

### 6.6 Counters and object counts

`tx_unreadable_drop` (the one counter that shows a devmem drop),
`tx_N_linearize` (must stay +0), netdev `tx_dropped`, `hw_tx_dropped`, and
`bufinfo` Total returning to 0.

---

## 7. Known issues a hardware tester must expect

| | What to expect |
| --- | --- |
| **15-frag limit (F2, V1, V2)** | The device takes at most 16 buffers per descriptor, so the driver drops any unreadable skb with more than 15 frags, silently except for `tx_unreadable_drop`. Pieces smaller than **MSS/15** therefore stall a stream: ~97 B at MTU 1500, ~600 B at MTU 9000. Separate small `sendmsg` calls count as pieces too. "Exactly 15 frags" only holds when no iovec crosses a page. **Re-derive the limit from this card's `max_sg_elems`** — the emulator's 15 is a guess (§9 C21). |
| **No error path (F3)** | Zerocopy completions report *success* for data the driver or the core dropped. The sender gets no indication. Expect to diagnose stalls from counters, never from the application. |
| **Route loss (F4)** | After a link down, `inet_sk_rebuild_header()` zeroes `sk_route_caps` and a devmem send returns **EINVAL** until TCP re-routes; shared `uarg`s then report `ZEROCOPY_COPIED` although nothing was copied. Use `devmem_tx -R <secs>`. Seen on v7.2.15, not on v7.4-rc3. |
| **Pending DMA on unload (F5)** | In-flight devmem skbs keep the binding's mapping alive past driver unbind: `device driver has pending DMA allocations while released from device`. It is the binding's mapping, not the driver's. |
| **Module unload with a live binding (F8)** | **Kernel oops / KASAN use-after-free** in `netdev_nl_sock_priv_destroy()`. Core bug, both kernels, every netmem-TX driver; fixed in net-next. Run M9(b) last, or backport the fix first. On hardware the same path is reachable by PCI hot-unplug and by a firmware reset — **untested anywhere**. |
| **DMA max segment size (F1)** | ionic never calls `dma_set_max_seg_size()`, so with `DMA_API_DEBUG` the first bind of a physically contiguous dma-buf logs `mapping sg segment longer than device claims to support [len=…] [max=65536]` with a WARN. The mapping is the exporter's; the driver only uses page-sized pieces. mlx5 sets it; ionic does not. A one-line `dma_set_max_seg_size(dev, UINT_MAX)` would silence debug runs. |
| **Patches 0005/0006 are load-bearing** | With **both** SG and checksum offload off, the core's `skb_segment()` succeeds and `skb_copy_and_csum_bits()` produces linear skbs still marked unreadable with no frags — which the device would transmit as **zero-filled payload with valid TCP checksums**. Mainline patch **0005** (backport **0006**) is the only thing that stops it. Prove it with the M7 control before anything else. |
| **XDP multi-buffer (B1)** | Pre-existing mainline ionic bug, not this series: `ionic_run_xdp()` fills the frag at offset 0 instead of `bi->page_offset`, so every second multi-buffer frame carries wrong bytes — **stale kernel memory on the wire**. Fix in `T/patches/xdp-frag-offset/`. Nothing in it depends on the emulator; it should reproduce on hardware. Deliberately not folded into the series. |
| **Rx ring (F-ring)** | Bulk TCP overran a 1024-entry Rx ring in the emulated receiver; `ethtool -G $IF rx 4096` avoided it. On hardware check `hw_rx_dropped` before blaming TX. |
| **`tx_tso_bytes` (V4)** | Not a byte count — `stats->tso_bytes = len;` is an assignment. Pre-existing mainline. |
| **Blocking sender (V3)** | After an unload with a live binding, a sender without `SO_SNDTIMEO` blocks in `sendmsg` until TCP gives up. |
| **Never run a rebuilt binary off a shared mount (F7)** | Install it locally first. |

---

## 8. The amdgpu VRAM source test — does the NIC really read VRAM?

Goal: replace the udmabuf exporter with an **amdgpu** dma-buf and determine
whether the NIC's descriptors point at the GPU's BAR (true peer-to-peer) or at
system pages (the buffer migrated to GTT).

### 8.1 What decides it

- **Kernel version.** On **v7.2.x** `net_devmem_bind_dmabuf()` uses the
  **static** `dma_buf_attach()` + `dma_buf_map_attachment_unlocked()`, with no
  `dma_buf_attach_ops`; `peer2peer` is set only from `importer_ops`, so it is
  **false**. amdgpu's pin then strips VRAM from the allowed domains whenever
  any attachment has `peer2peer == false`, and the map returns **page DMA
  addresses for GTT**. On v7.2.x the answer is therefore *system pages*, and no
  amount of `pci_p2pdma_distance()` goodness changes it.
- On **v7.4-rc** the importer is dynamic (`dma_buf_dynamic_attach` +
  `dma_buf_pin` + `dma_resv_wait_timeout`); this is the version that **can**
  keep the buffer in VRAM, provided `peer2peer` is actually granted.
- **`pci_p2pdma_distance()`** must be ≥ 0 between GPU and NIC or amdgpu
  downgrades the attachment at attach time. Needs `CONFIG_PCI_P2PDMA=y` (a
  `n` config makes the stub return −1 and kills P2P for every importer).
  Across a host bridge it needs AMD family ≥ 0x17 or a whitelisted bridge.
- **Runtime PM.** In mainline v7.2.15 `amdgpu_dma_buf_attach()` takes a
  runtime-PM reference with `pm_runtime_get_if_active()` and **downgrades to
  `peer2peer = false` if the GPU is runtime-suspended**. Pin the GPU awake
  before binding.
- GFX12+ BOs created with `AMDGPU_GEM_CREATE_GFX12_DCC` are never peer2peer.

### 8.2 Procedure

```bash
# 0. state
cat /sys/bus/pci/devices/$GPU/power/runtime_status      # must be "active"
echo on | sudo tee /sys/bus/pci/devices/$GPU/power/control
lspci -vv -s $GPU | grep -E 'Region|Memory at'          # the VRAM BAR range
cat /proc/iomem | grep -iE 'System RAM|$GPU'            # system-RAM ranges to exclude
# 1. p2p distance, from a tiny module or from the amdgpu log line
dmesg | grep -i 'peer-to-peer DMA memory'               # amdgpu registers its p2pdma range here
# 2. allocate a VRAM buffer and export it
~/devmem/test/src/gpu/devmem_tx_gpu -G /dev/dri/renderD128 -f $IF -s $PEER -p 5600 \
     -S 16m -o 123 -l 100003 -a 7919 -n 200 -D -F /tmp/dtx-pfns
#    (-J asks for GTT instead of VRAM, as the control)
# 3. where did it end up?
sudo cat /sys/kernel/debug/dri/*/amdgpu_gem_info | head -40     # per-BO domain
sudo cat /sys/kernel/debug/dri/*/amdgpu_vram_mm | head
awk '/Attached Devices/,0' /sys/kernel/debug/dma_buf/bufinfo    # the NIC must appear here
# 4. what address did the driver put in the descriptor?
sudo bpftrace ~/devmem/test/evidence/tx-verify/src/vprobe.bt    # + the ionic_tx_map_frag kretprobe
```

**The decision rule.** Take the frag DMA addresses from the
`ionic_tx_map_frag` kretprobe (with the IOMMU off, or translated) and compare:

| Addresses fall in | Verdict |
| --- | --- |
| the GPU's VRAM BAR / the `added peer-to-peer DMA memory` range | **real P2P**: the NIC is reading VRAM |
| System RAM | the buffer migrated to **GTT / system pages**; no P2P happened |

Cross-check with the BO's domain in `amdgpu_gem_info` and with the NIC's
`tx_dma_error`-equivalent counter: a NIC that cannot reach the GPU BAR will
report DMA errors rather than deliver wrong data.

### 8.3 What the emulator could never answer, and why

On the emulator this test is **structurally impossible**, not merely
unperformed: `rocm-ernic` resolves a guest address only through
`vfu_addr_to_sgl()`/`vfu_sgl_get()`, which need a mapped region with a host
`vaddr`. QEMU sends an fd for a RAMBlock only when
`memory_region_get_fd() != -1`, and a BAR mapped as a `ram_device_ptr` has
`fd = -1`, so the emulator sees a region it cannot read. rocm-ernic states it:
"GPU VRAM buffers would need a vfio-user DMA proxy for GPU BAR regions, which
does not exist" (`docs/performance.rst`). Hardware is the only place this
question has an answer.  Status: **NOT RUN**

---

## 9. Performance: devmem TX vs MSG_ZEROCOPY vs copy

Same MTU, same TSO/GSO settings, same ring and channel counts, same CPU
pinning, same IOMMU configuration, in one session, alternating the three
modes. `devmem_tx -m devmem|zc|copy` gives all three with identical framing,
which is what makes the comparison fair.

```bash
# fixed for the whole sweep
sudo ethtool -K $IF tx on sg on tso on gso on; sudo ip link set $IF mtu $MTU
sudo ethtool -L $IF combined 1; sudo ethtool -G $IF tx 1024 rx 4096
# receiver
devmem_tx -r -p 5700 -S 16m -o 17 -l 1000003 -z 250001 -g 4099 -a 65521 -n 0 -W 30
# sender, per mode
for m in devmem zc copy; do
  mpstat -P ALL 1 70 > cpu-$m.txt &
  perf stat -a -e cycles,instructions,cache-misses -- sleep 60 2> perf-$m.txt &
  taskset -c 4 sudo devmem_tx -m $m -f $IF -s $PEER -p 5700 \
      -S 16m -o 17 -l 1000003 -z 250001 -g 4099 -a 65521 -n 0 -T 60 -v -C 30
  wait
done
# the kernel's own zerocopy selftest, as a cross-check of the zc leg
make -C tools/testing/selftests/net msg_zerocopy
./tools/testing/selftests/net/msg_zerocopy -4 -t 30 -z -p 5701 $PEER     # sender
./tools/testing/selftests/net/msg_zerocopy -4 -r -p 5701                 # receiver
```

Report, per row: MTU, TSO on/off, IOMMU state, queue count, CPU pinning,
kernel, driver build-id.

| Mode | MTU | TSO | IOMMU | Gbit/s | sender CPU % (all cores) | sender cycles/byte | receiver CPU % | retransmits | `tx_unreadable_drop` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| copy | 1500 | on | | | | | | | |
| MSG_ZEROCOPY | 1500 | on | | | | | | | |
| devmem | 1500 | on | | | | | | | |
| copy | 9000 | on | | | | | | | |
| MSG_ZEROCOPY | 9000 | on | | | | | | | |
| devmem | 9000 | on | | | | | | | |
| devmem | 9000 | off | | | | | | | |
| devmem (GPU dma-buf, §8) | 9000 | on | | | | | | | |
| `msg_zerocopy` selftest | 1500 | on | | | | | | | |

Status: **NOT RUN** — no number in this table is an observation.

Caveats to carry into the reading: each devmem frag is charged `PAGE_SIZE` of
truesize, so small-iovec shapes collapse in throughput (7 MiB/s vs
180–200 MiB/s in the emulated runs) for reasons that have nothing to do with
the NIC; and the emulated numbers (~180 MiB/s) are an artefact of the
emulator and must not be used as a baseline.

---

## 10. Questions only real firmware can answer

Collected from [ionic-tx-audit.md](ionic-tx-audit.md),
[ionic-tx-device-contract.md](ionic-tx-device-contract.md) and
[rx-design.md](rx-design.md). Each is phrased so a hardware tester can answer
it; record the answer next to the question.

### A. TSO completion granularity and chain semantics

1. Does firmware write **one completion per TSO chain, or one per
   descriptor**? The driver tolerates either; the emulator chose
   one-at-end-of-transmit. (contract §5, §10.1)
2. Does firmware guarantee it has read **every buffer of a chain** before
   writing any completion covering the SOT descriptor? The skb and all its
   mappings hang off descriptor 0, so an early completion tells a zerocopy or
   devmem application its buffer is reusable mid-gather. (contract §5)
3. Is `comp_index` always exactly the last descriptor the completion covers,
   and always inside `[tail, head)`? The driver's cleanup loop is unbounded.
4. Does firmware ever use TX `comp->status` to signal a drop, or are LIF
   counters the only channel? The driver never reads it.
5. What does firmware do with a TSO chain that reaches the producer index
   **without an EOT** descriptor — hold it, or transmit it partially?
6. Does firmware validate that `hdr_len`/`mss`/VLAN/ENCAP repeat identically
   in every descriptor of a chain, that only the first has SOT, and that
   `mss != 0` — and how does it fail?
7. Is the `hdr_len` header template taken from descriptor 0's **first buffer**
   or from its **gathered** bytes? A devmem skb has a ~66-byte linear head, so
   the distinction is reachable.
8. Does firmware fetch descriptors strictly below the doorbell's `p_index`,
   never prefetching past it?
9. Is there a firmware limit on TSO chain length, total TSO size, or segments
   per chain? The driver sets no `gso_max_segs`/`tso_max_size`.
10. **Highest-value test in these notes:** does firmware recompute the TCP and
    IPv4 checksums per segment from scratch, or add the per-segment length to
    the driver's preloaded pseudo-header seed? The driver does **not** prepare
    the template when the frame carries an in-packet VLAN tag, so a
    seed-adding device emits a wrong checksum on every segment of VLAN TSO
    traffic. (contract Verification C1)
11. Under TSO6, does firmware walk IPv6 extension headers or reject the chain?
    Such skbs do reach the device, because ionic advertises `NETIF_F_HW_CSUM`.
12. Does firmware increment the IPv4 ID per segment, and does it honour
    `tx-tcp-mangleid-segmentation`?
13. For a computed-zero L4 checksum, does firmware store `0xffff` (as UDP
    requires) on all protocols, or leave 0?

### B. ENCAP and checksum-offload semantics

14. What does firmware actually do with **ENCAP on a CSUM_PARTIAL
    descriptor**? The Linux stack has already completed the outer IPv4 and
    outer UDP (LCO) checksums; if firmware rewrites them, every tunnelled
    `CHECKSUM_PARTIAL` packet is double-offloaded and corrupt.
15. On a TSO descriptor, what exactly does ENCAP make firmware rewrite per
    segment — outer IP length/ID/checksum, outer UDP length/checksum?
16. Does firmware validate `csum_start`/`csum_offset` against the gathered
    total, and what happens if they point past the frame? Security-relevant:
    this is a guest/host boundary in the emulated deployment.
17. Are `csum_start`/`csum_offset` relative to the frame **as posted**, i.e.
    before any VLAN tag the device inserts?
18. For CSUM_HW (opcode 2, which the Linux driver never emits): does `CSUM_L4`
    include the pseudo-header, does `CSUM_L3` mean only the IPv4 header
    checksum, and does ENCAP extend both to the outer headers?

### C. Q_IDENTIFY / LIF_IDENTIFY / LIF_SETATTR

19. For a TXQ `Q_IDENTIFY` with `cmd.ver = 3`, what does production firmware
    return for `version`, `supported`, `desc_sz`, `comp_sz`, `sg_desc_sz`,
    `max_sg_elems`, `sg_desc_stride` and `features`?
20. Does firmware return `min(requested, supported)` in `version` — will it
    answer v0 to a driver that asks `ver = 0`?
21. Does any firmware report `max_sg_elems` **below 15**, and if so is that
    safe? The driver **ignores** the reported value
    (`min(IONIC_TX_MAX_FRAGS - 1, MAX_SKB_FRAGS)` = 15) and always writes up
    to 15 SG elements; on a 128-byte v0 SG slot that overruns into the next
    descriptor's slot.
22. Does firmware reporting `version >= 1` always also report
    `sg_desc_sz == 256`, and does it really read the SG ring at a 256-byte
    stride? Both halves of the driver's test must agree.
23. Which `IONIC_QIDENT_F_*` bits (CMB, EXPDB, 2X_CQ_DESC) does firmware set?
24. **Can TXQ `Q_IDENTIFY` ever fail?** On `-EINVAL` the driver leaves
    `max_sg_elems == 0`, after which every skb with a frag takes the linearize
    path and **every devmem skb is dropped**.
25. What mask does firmware report in LIF_IDENTIFY `eth.config.features`
    (offset 0x0a8), and does it match what `LIF_SETATTR(FEATURES)` then
    grants?
26. For a `LIF_SETATTR(FEATURES)` request containing unsupported bits, does
    firmware return `status = 0` with `features = requested & supported`,
    return extra bits, or fail the command? A non-zero status fails
    `ionic_lif_init()`.
27. Which `IONIC_ETH_HW_*` bits does current production firmware actually
    grant (table in §1.1)? The whole gating design depends on it.
28. Does firmware grant the **same mask after a firmware reset**? After a
    reset the driver calls `ionic_set_nic_features()` without recomputing
    `netdev->features` or `netdev->netmem_tx`, so a device granting less after
    reset leaves `netmem_tx` set — and the next devmem send is undefined.
    **Nothing anywhere has tested a firmware reset with devmem TX.**
29. Does firmware require LIF-level `IONIC_ETH_HW_RX_SG` (bit 6) for RX queues
    initialised with `IONIC_QINIT_F_SG`? The driver never requests bit 6.

### D. RX header split (prerequisite for any devmem RX work)

30. **Does any Pensando firmware have, or plan, RX header/data split — how is
    it negotiated, and where is the header length reported?** The design in
    rx-design.md invents a per-queue `IONIC_RXQ_F_HDS` bit and a 2X completion
    carrying `hds_hdr_len`, and marks them **emulator-only, not a firmware
    interface**.
31. Would the firmware team assign real values for `IONIC_RXQ_F_HDS`,
    `IONIC_HDS_CQ_NEGOFFSET`, `IONIC_RXQ_COMP_HDS_F_SPLIT/_HBO`, and do the
    proposed numbers collide with anything out-of-tree?
32. Is a **buffer 0 smaller than the frame's headers** accepted with an SG
    descriptor, and what is the guaranteed fill order across buffer 0 and the
    SG elements? The Linux driver has never posted a small buffer 0.
33. Is there a firmware-imposed **minimum length** for an RX descriptor's
    buffer 0?
34. How would firmware split **encapsulated** traffic — inner or outer
    headers — and does split work on CMB RX rings?
35. May per-queue feature sets differ within one LIF (HDS on one RX queue
    only), and what is the cost of 32-byte completions?
36. What are the real RX gather limits (8 SG elements of ≤ 64 KiB each), and
    can larger payload buffers be supported?

### E. RX checksum (as hard a prerequisite as header split)

37. What are `comp.csum` semantics with split placement, with VLAN stripping,
    and for odd frame lengths — and is `IONIC_RXQ_COMP_CSUM_F_CALC` set for
    every TCP segment? An unreadable RX skb delivered `CHECKSUM_NONE` cannot
    be received at all (`skb_checksum()` hits
    `WARN_ON_ONCE(!skb_frags_readable())`).
38. **In which byte order** does firmware store `comp.csum`? A wrong value
    surfaces as `netdev_rx_csum_fault()` "hw csum failure". Flagged as a trap
    and never tested.
39. Does firmware set `CSUM_F_TCP_OK`/`UDP_OK` reliably enough to report
    `CHECKSUM_UNNECESSARY` rather than `CHECKSUM_COMPLETE`?
40. Does `comp.csum` include Ethernet padding on a runt, and would a padded
    runt be header-split?

### F. Queue lifecycle

41. Is `Q_CONTROL(DISABLE)` **synchronous** — is it guaranteed that no DMA
    lands in the old buffers after it completes? For devmem a late write lands
    in memory already handed back to userspace.
42. May a **single RX queue** be re-`Q_INIT`ed while the LIF is up and not
    quiesced? No Linux path does this today.

### G. Flow steering and RSS

43. Does a flow-steering filter exist, or can one exist, behind
    `RX_FILTER_ADD` — what capacity, and what priority relative to RSS?
    `Documentation/networking/devmem.rst` calls header split, flow steering
    and RSS *required* features; ionic has no `get_rxnfc`/`set_rxnfc` at all.
44. Does firmware honour the `LIF_SETATTR(RSS)` key and 128-entry indirection
    table exactly, so `ethtool -X $IF equal N` reliably keeps other flows off
    the bound queue — and what are the hash inputs?
45. Does the existing `STEER_PKTCLASS` filter (which carries `qtype`/`qid`)
    steer to an arbitrary queue, and does `RX_FILTER_ADD` return unique
    non-zero `filter_id`s with a working `RX_FILTER_DEL`?

### H. DMA limits, alignment, addressing

46. Does firmware **pad runt frames** on TX? The driver has no `skb_pad` of
    any kind; a 1-byte TCP payload without timestamps is a 55-byte frame.
47. Does firmware tolerate **many tiny SG elements** (down to 1 byte) at
    arbitrary unaligned addresses? `ionic_tx_tso()` freely emits 1-byte chunks
    at arbitrary offsets.
48. Is there any alignment or minimum-length requirement for TX data buffers
    or SG elements not stated in `ionic_if.h`?
49. What happens with a TX buffer address **above 2^52**?
    `encode_txq_desc_cmd()` masks silently (`addr & (BIT_ULL(52)-1)`) and the
    driver has no range check. A dma-buf exporter (especially a GPU, or an
    IOMMU handing out high IOVAs) can produce such addresses — does firmware
    detect it, or DMA to the truncated address? **This is the one that can
    corrupt unrelated memory.**
50. Can the device DMA to the addresses a **GPU dma-buf** exports — PCIe P2P,
    through the IOMMU? (§8 answers this empirically; this asks what firmware
    guarantees.)
51. What is the real maximum per-buffer length and maximum gathered frame
    size? `desc->len` and `elem->len` are `__le16`; the driver never checks a
    frag length against 65535 on TX.

### I. Counters as the only drop signal

52. Which firmware LIF counters move when a descriptor is rejected —
    `tx_desc_data_error`, `tx_desc_fetch_error`, `tx_dma_error`,
    `tx_queue_disabled` — and are they per-frame or per-descriptor under TSO?
    Since TX `comp->status` is never read, these (as `hw_tx_dropped` /
    `hw_tx_aborted_errors`) are the only drop signal there is.

---

## 11. Gaps the emulated runs leave open, that hardware should close

Never run anywhere, in any environment: **firmware reset**, **PCI unbind /
hot-unplug** (same code path as F8), the **hwstamp queue**, **AF_XDP**,
**XDP_REDIRECT from another netdev into ionic**, **VLAN** of any kind (the
test kernels had no `CONFIG_VLAN_8021Q`), the XDP map-failure path that
mainline patch 0001 fixes, and kmemleak / KFENCE / DEBUG_OBJECTS /
NET_DEV_REFCNT_TRACKER / UBSAN legs. IPv6 was exercised on v7.4-rc3 only. The
B1 XDP fix was built and run on v7.4-rc3 only.

Record every result as: case, status (NOT RUN / PASS / FAIL), kernel, driver
build-id, firmware version, MTU, offloads, IOMMU state, and the counter deltas
— in that order. A result without the IOMMU state and the firmware version is
not reproducible.

---

## CORRECTION (2026-11-19, evening) — the emulator is no longer the blocker

The passage above (§ around "`fd = -1`, so the emulator sees a region it cannot
read ... Hardware is the only place this ...") is **WRONG** and is retracted.
A GPU-VRAM-backed dma-buf has now been used as a devmem TCP TX source **in the
emulated stack**, with the NIC reading VRAM over vfio-user P2P: see
[test-gpu-vram.md](test-gpu-vram.md) and `test/evidence/gpu-vram/`.

`fd = -1` is the behaviour of **stock** QEMU. The fork
(`sbates130272/qemu`, branch `dev/stephen/pci-mmio-bridge-submit`, commit
**a2ee87e**, "hw/vfio: assign region fd to RAMBlock for vfio-user P2P DMA")
assigns the region fd to the BAR's RAMBlock, so `memory_region_get_fd()`
succeeds, QEMU sends the fd in `DMA_MAP`, and a peer vfio-user server maps the
window — stack 1's `rocm-ernic` servers each map a 262144 MiB
`/memfd:rocjitsu-vram` region. The other missing piece was
`CONFIG_PCI_P2PDMA=y` in the guest kernel.

Real hardware is still needed for the things this test bed cannot show (a real
Instinct GPU's VRAM, an IOMMU in the path, real PCIe P2P routing and ACS), but
it is no longer needed to answer "can the NIC source TX from VRAM at all".
