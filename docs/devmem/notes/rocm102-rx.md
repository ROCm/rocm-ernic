# Stack 3 (`ionic-devmem-r102`): devmem TCP RX bring-up on the ROCm 10.2 guest (v7.2.6)

2026-11-20, 05:20Z to 06:55Z. `B` = `/var/tmp/ionic-devmem/test-rocm102`, `R` =
`/var/tmp/ionic-devmem/overnight2/rx` (`R/scripts`, `R/guest`, `R/evidence` = `E/` below),
`O` = `/var/tmp/ionic-devmem/overnight2/rx-backport`. Everything below was RUN by me unless
marked **NOT RUN**. Nothing was committed or posted; rocm-ernic, qemu-minimal and stacks 1
and 2 were not touched. Reads on from [impl-rx-backport-7.2.6.md](impl-rx-backport-7.2.6.md)
(the series, compile-tested) and [rocm102-p2p-kernel.md](rocm102-p2p-kernel.md) (the kernel).

## 1. Result

**Devmem TCP receive works on the fleet's kernel line.** The 8-patch backport
(`O/patches`, unchanged: sha256 equal to `O/evidence/patches.sha256`) built in both guests
for `7.2.6-devmemp2p`, loaded with `ionic_rdma` and `ernic_p4` rebuilt against it, and ran
every case below without a single driver change. No splat, no hang, no leaked page pool in
any boot; a lockdep build of the same kernel reported nothing.

- ionic `7C12352B6F24344C68BF423`, staged by `ionic-mod.sh stage rxbp srq p4 devmem-tx
  O/patches`; the staged tree is `diff -r`-identical to `O/tree`.
- Patch 1 alone (`rxbp-p1`, srcversion `708201974F179DDB116C2C9`) was run first on the
  compat server, as its engineer asked.
- **No backport bug was found, so nothing in `O/` was regenerated.** Three bugs were in my
  own harness (section 6).
- **Nothing had to be attributed to a v7.2.6 core difference**: the two differences the
  backport note lists (rx_mode work under RTNL, `netdev_WARN` instead of a refusal) were not
  visible in any run.

## 2. State left

**Stack 3 is up on the patched kernel, RX-capable modules loaded, servers in the
header-split profile.**

| Item | State |
| --- | --- |
| NIC servers | 2d3a3d02 Release (`B/ernic/poc-rel`) through `verify/hs-wrapper.sh`: `nic_profile : compat + header-split`, log line `NIC Rx header split: on`; emulator error counters 0; no P4 program (cold-cycled after `--p4-test`) |
| Guests | both `7.2.6-devmemp2p`; `ionic` `7C12352B...` (build-id `32079c74...`), `ionic_rdma`, `ernic_p4` from `~/ionic-mod/rxbp` by `insmod` (not persistent), amdgpu and `sdxi_char` loaded, 10.102.100.11 / .12, IPv4 GID at index 1 |
| Driver settings | defaults: `tcp-data-split off`, 4 combined channels, rings 1024, MTU 1500, `rx-checksumming on` (the profile offers it) |
| Kernel logs | 0 BUG / WARNING / Oops / Call Trace lines; 0 dma-buf objects; 0 dmabuf or orphan page pools (`E/90-final-*`) |
| Last checks in this state | base `quick-start test` rc 0, SRQ pass, hashed TCP both ways, devmem TX smoke, devmem RX 64 MiB validated (`E/90-final-state.txt`) |
| Added to guest disks | `~/rx/` (my scripts), `~/ionic-mod/{rxbp,rxbp-p1}` (+ `.old-*` copies of earlier builds in r102-2); **r102-2 only**: kernel `7.2.6-devmemp2p-lockdep` 7.2.6-4 with its amdgpu DKMS build, installed and not selected |
| Host | `R/kbuild-lockdep` (36 GB), exited container `ionic-devmem-r102-kbuild-lockdep`; the override file differs from how I found it only in the two `ernic-N` lines (`E/docker-compose.override.yml.as-found`) |

To receive: `sudo ethtool -L rocm-ernic0 combined 1; sudo ethtool -G rocm-ernic0
tcp-data-split on; sudo ~/devmem/ncdevmem -n -l -s 10.102.100.11 -p 5201 -f rocm-ernic0 -t 0
-q 1 -v 7 -L`. After a cold cycle: `B/scripts/ionic-mod.sh load rxbp`, then a
`qs-test.sh base` for amdgpu and addresses.

Back to the TX-only set and compat profile (the module half was run, `E/06`; the server half
is the same command that brought it here):

    B/scripts/stack.sh down; B/scripts/verify/vserver.sh poc-rel; B/scripts/stack.sh up
    B/scripts/ionic-mod.sh load devmem

## 3. Sequence

| Boot | Server | Kernel r102-1 / r102-2 | Modules | What ran |
| --- | --- | --- | --- | --- |
| as found | compat | devmemp2p / devmemp2p | `rxbp-p1`, then `rxbp` | patch 1 alone; the whole series with the feature absent |
| cycle 1 | header-split | devmemp2p / **lockdep** | `rxbp` | ops locking under lockdep, header split, first light, sizes, teardown, stress |
| cycle 2 | header-split | devmemp2p / devmemp2p | `rxbp` | upstream tests, then all of it again on the fleet kernel, `--p4-test` last |
| cycle 3 | header-split | devmemp2p / devmemp2p | `devmem`, then `rxbp` | way-back check, final checks, state left |

## 4. Cases

"splat" = BUG, WARNING, Oops, Call Trace, hung task, "blocked for more", csum failure, page
pool stall, refcount, sleeping-in-atomic, RTNL assertion, and on the lockdep boot any
lockdep report. Every case below ended with none.

### 4.1 Build and load (step 1)

| Case | Result | Evidence |
| --- | --- | --- |
| `ionic`, `ionic_rdma`, `ernic_p4` built in both guests for `7.2.6-devmemp2p`, patch 1 only and the whole series, no warning | PASS | `E/00-build.txt` |
| Patch 1 only probes and loads (the v7.4 first-load failure does not recur: the async rx_mode conversion is in) | PASS | `E/01-load-rxbp-p1.txt` |
| Whole series loads on compat and on header-split; loaded srcversion and build-id equal the built files, 6 of 6, every load | PASS | `E/02`, `E/04`, `E/05`, `E/06` |
| Servers switched with `vserver.sh header-split` and a whole-stack cycle | PASS | `E/04-hs-boot-load-rxbp.txt` |

### 4.2 Ops locking (step 2)

`R/guest/opslock.sh` (stack 2's script plus RDMA and P4 checks): per round ifdown/ifup x6,
rings 512/4096/1024, channels 1/2/4, split interrupts, MTU 9000/3000/1500, XDP attach, channel
and MTU change and ifdown/ifup with XDP, detach, 30 `ethtool` get/set operations, promiscuous,
`tcpdump`, allmulti, 41 multicast addresses, MAC change, IPv6 neighbour discovery,
`rdma link`, netdev netlink dumps; after every group a ping, `ibv_devinfo` PORT_ACTIVE and
`/dev/ernic-p4`. Each step runs under `timeout`, with the hung-task detector at 30 s.
"load" = `iperf3 --bidir` plus an `ib_send_bw` loop between the guests throughout.

| Case | Result | Evidence |
| --- | --- | --- |
| Patch 1 only, compat: 1 round idle, both guests | PASS | `E/10-*` |
| Patch 1 only, compat: 2 rounds under load, both guests | PASS | `E/11-*` |
| Patch 1 only: SRQ (`ib_send_bw --use-srq`, `ibv_srq_pingpong`) | PASS | `E/12-srq-p1-compat.txt` |
| Whole series, compat: 1 round idle | PASS | `E/20-*` |
| Header-split server, split off: 2 rounds idle **with PCI function resets** (up and down); r102-2 under lockdep | PASS | `E/30-*` |
| Same, 2 rounds under load, split off | PASS | `E/31-*` |
| Same, 2 rounds under load, **split on** (XDP attach is then refused, as designed) | PASS | `E/32-*` |
| `rmmod ernic_p4 ionic_rdma ionic` and reload x3 while the peer floods it, each guest | PASS | `E/33-*` |
| Both guests on the fleet kernel: 2 rounds load split on, 1 round idle split off, with PCI resets | PASS | `E/60-*`, `E/61-*` |
| RDMA and P4 queries racing lock holders: about 1000 rounds of `ibv_devinfo -v` + `rdma link` + open of `/dev/ernic-p4` against about 2300 ring/channel/MTU/ifdown operations in 40 s, each guest | PASS: 0 failed, worst round 59 ms and 56 ms | `E/67-*` |
| Bind/unbind against ring, channel, MTU, ifdown/ifup, PCI resets and netlink dumps, 90 s: 157 binds vs 323 operations (lockdep), 160 vs 332 (fleet kernel) | PASS, netdev and RDMA work afterwards, 0 dmabuf pools | `E/54-*`, `E/65-*` |
| hwtstamp config | RUN: `-EOPNOTSUPP` (the emulator offers no timestamps) | `E/10-*` |
| Link flap, firmware heartbeat stop | NOT RUN: the emulator can do neither | |
| Patch 1 alone on the header-split server, or under lockdep | NOT RUN | |

In the load runs the `ib_send_bw` loop loses runs (for example 19 of 34 pass): the perftest
control connection is TCP over the interface being taken down, and a PCI reset removes the
RDMA device. Every loop resumed; the iperf3 runs and all pings passed. The first r102-2 run
(6 of 40) was my loop's 90 s server timeout, not the driver
(`E/11-*-harness-artifact.txt`); with a 12 s timeout the same run recovers at once.

**Lockdep.** Cheap enough, so done: v7.2.6 + the P2P backport + the guest's full config +
`PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`, `DEBUG_NET`, `DEBUG_MUTEXES`, `DEBUG_SPINLOCK`
(`R/scripts/kbuild-lockdep.sh`, 38 config lines differ, `R/kbuild-lockdep/config-diff.txt`),
release `7.2.6-devmemp2p-lockdep`. Build 9 min 50 s; install in r102-2 4 min (amdgpu DKMS
built for it); booted on r102-2 for cycle 1 only. **Lockdep reported nothing**: after the
`E/30`..`E/33` runs, header split, devmem RX as receiver and as sender, the teardown cases
and the stress run, `debug_locks` is still 1 (1936 lock classes, 27894 chains) and the whole
boot's log has 0 lines matching the splat pattern
(`E/55-lockdep-final-state.txt`, `E/55-lockdep-boot-dmesg-r102-2.txt`). KASAN was not built.

### 4.3 Upstream tests on the RX modules (step 2), header-split server, fleet kernel

| Test | Result | Numbers | Evidence |
| --- | --- | --- | --- |
| base (GPU, `ib_send_bw`, NVMe), split off | PASS, rc 0 | `vector-add: PASS` x2; NVMe read 1593 / 1456 MiB/s | `E/quickstart/rx-base/` |
| SRQ, split off | PASS | 1662 and 1697 MiB/s (client, server) | `E/srq/rx-hs-splitoff.txt` |
| `--rocm-rdma-test --rocm-rdma-mode dmabuf`, **split on** | PASS | 1454 MiB/s, `VALIDATION: PASSED - 7 chunks, 58720256 bytes` | `E/quickstart/rx-rdma-dmabuf-spliton/` |
| SRQ, split on | PASS | 1538 and 1566 MiB/s | `E/srq/rx-hs-spliton.txt` |
| `--p4-test`, split on | PASS | fp8 172.7 MB/s | `E/quickstart/rx-p4/` |
| base after the last cycle | PASS, rc 0 | | `B/evidence/quickstart/rx-final-base/` |
| SRQ on the series, compat server | PASS | | `E/22-*` |
| `--sdxi-test`, `--hipfile-test`, host-alloc RDMA, `vram` mode | NOT RUN | | |

Rates are single samples inside the band the earlier notes measured; no rate claim is made.

### 4.4 Header split with ordinary pages (step 3)

`R/scripts/hds.sh`, run on cycle 1 (`E/40-hds-lockdep-boot.txt`) and on the fleet kernel
(`E/62-hds-final.txt`); shapes by bpftrace at `gro_receive_skb` / `napi_gro_frags`.

| Case | Result |
| --- | --- |
| Split off on the header-split server: 64 MB both ways, sha256; frag path, `ip_summed` 2 (`CHECKSUM_COMPLETE`) on all 44200 data frames | PASS |
| `tcp-data-split on`: 64 MB both ways, sha256; **44199 of 44199 data skbs have 52 linear bytes after the Ethernet header and one frag, `ip_summed` 1**; GRO skbs up to 64K at `tcp_v4_rcv`; `rx_hds_split` and `rx_csum_unnecessary` rise by the frame count | PASS |
| 1..5 byte segments, timestamps on (linear 52) and off (linear 40) | PASS |
| ARP, ping 56 / 1472 / 2000 (IP fragments) both ways | PASS |
| UDP self-checking datagrams 1..8000 bytes both ways: 1200 good, 0 bad, each way | PASS |
| IPv6: ping, TCP 16 MB both ways (linear 72), UDP 400 good | PASS |
| `ethtool -K rx off` (`ip_summed` 0), GRO off (11050 segments of 1K-2K reach TCP) | PASS |
| XDP attach with split on, and split on with XDP attached: both refused | PASS |
| ifdown/ifup, ring 512/4096, channels 2/4 with split on, then 16 MB | PASS |
| Receiver at MTU 9000, sender at 1500: 64 MB each way | PASS |
| Split off again: frag path, `CHECKSUM_COMPLETE` | PASS |
| "hw csum failure" lines, `TcpInCsumErrors` | 0, 0 on both guests |
| Feature off: series driver on the **compat** server shows `TCP data split: n/a`, refuses `on` and `off` ("Device does not support tcp-data-split"), accepts `auto`, `rx-checksumming off [fixed]`, frag path with `ip_summed` 0, 64 MB both ways, devmem TX smoke and PFN check pass, RX bind refused ("tcp-data-split is disabled") | PASS, `E/21-*`, `E/22-*` |
| **Jumbo frames across the wire** (multi-SG payload) | NOT RUN: stack 3's tap and docker bridge are MTU 1500 (`rocm102-devmem.md`). My first attempt set both guests to 9000 and failed there, 100 % loss on an 8972-byte ping (`E/40`, "MTU 9000 on both") |
| VLAN; padded frames, IP options, IPv6 extension headers on the wire | NOT RUN |

### 4.5 Devmem RX with udmabuf (step 4)

Receiver: `ethtool -L combined 1`, split on, `ncdevmem -n -l -s IP -p 5201 -f rocm-ernic0
-t 0 -q 1 -v 7 -L` (62.5 MiB udmabuf; `-L` fails on any `SCM_DEVMEM_LINEAR`; `-v 7`
validates every byte). The binary is the one already in the guests (`1cdf3e86...`).

| Case | Result | Evidence |
| --- | --- | --- |
| **First light**, plain TCP 1 MiB: 728 `SCM_DEVMEM_DMABUF`, 0 linear, 728 buffers validated, exit 0; 728 of 728 data skbs `unreadable`, linear 52, `ip_summed` 1; `page-pool-get` shows the dmabuf on the pool, `queue-get` on RX queue 0 | PASS | `E/50-first-light.txt` |
| Plain TCP 1 MiB, 16 MiB, 128 MiB, **1 GiB** (742429 cmsgs, 0 linear, 0 validation failures) | PASS x4 | `E/63-*` (fleet kernel), `E/51-*` |
| **Devmem TX -> devmem RX**, `devmem_tx` udmabuf sender, 1 MiB, 16 MiB, 128 MiB, **1 GiB**: sha256 of the receiver's dma-buf contents equals the same plan sent with `-m copy` to an ordinary receiver; 0 linear; sender completions = sends, `tx_unreadable_drop` 0 | PASS x4 | `E/63-*`, `E/51-*` |
| 2.1 MB in 5-byte `TCP_NODELAY` writes | PASS | `E/63-*` |
| `ncdevmem` TX client, 70000 bytes in 10000 sends -> devmem RX | PASS | `E/63-*` |
| r102-2 as receiver: 1 MiB, 128 MiB plain; 1 MiB, 128 MiB TX -> RX; small writes; 10000 sends | PASS | `E/66-*` |
| Kill -9 mid-stream x3 (50-60 MB in), then hashed TCP both ways, then bind again | PASS | `E/64-*` |
| ifdown/ifup with a binding, then 4 MiB (pool id changes, same dmabuf) | PASS | `E/64-*` |
| With a binding: split off and XDP refused by the core; rings 512/4096/1024, MTU 9000/1500, channels 1 -> 2; then 4 MiB to queue 0 **steered by source port** (40001, emulator flow hash) | PASS | `E/64-*` |
| With a binding on queue 0: channels 2 -> 1 | refused by the core ("requested channel counts are too low for existing memory provider setting"), as on v7.4 | `E/64-*` |
| Receiver MTU 9000 with a binding (pool inflight 1023 -> 3069), 16 MiB | PASS | `E/64-*` |
| Bind while down | `Network is down` | `E/64-*` |
| ifdown/ifup mid-stream | fleet kernel: the stream **continued**, 400000000 bytes validated. Lockdep boot: the connection was reset, 103549680 bytes validated, exit 0. Stack 2 saw the reset with the stock driver too | `E/64-*`, `E/53-*` |
| `ethtool -K rx off` with a binding: `rx_unreadable_drop` 2 -> 14, nothing delivered | PASS (as designed) | `E/64-*` |
| `ib_send_bw -D 8` across a bind, 64 MiB and an unbind: both pass | PASS | `E/64-*` |
| 20 bind / 64 KiB / clean-exit cycles | PASS, 20 of 20 exit 0 | `E/64-*` |
| Devmem RX with the fp8 P4 program in the receiver's NIC | PASS, 16 MiB | `E/69-*` |
| Same teardown list with r102-2 (lockdep) as receiver | PASS | `E/53-rx-teardown-rcv2-lockdep.txt` |
| **Leaked page pools**: after every case 0 dmabuf pools, 0 orphans, 0 detached, 0 dma-buf objects, 0 "stalled pool" lines; the same after about 320 binds in the stress runs | PASS | every file above, `E/54`, `E/65`, `E/90` |
| GPU buffers (amdgpu GEM, `hipMalloc`) as the RX dma-buf | NOT RUN |  |
| IPv6 or VLAN devmem RX, two queues with queue 1 bound, rmmod with a live binding, injected queue-op failures, kmemleak, slab accounting | NOT RUN | |
| 35-case devmem TX matrix on the RX modules | NOT RUN (smoke, and the four TX -> RX sizes only) | |

## 5. Findings

- **F1. An idle bound queue counts drops.** `rx_unreadable_drop` rose by 6 in 70 s with no
  sender (`E/68-*`). The only frames on the segment over 256 bytes that are not TCP were the
  guests' 336-byte DHCP broadcasts on `rocm-ernic0`, so this is consistent with the
  interface's rule that such frames on a bound queue are dropped (`rx-interface.md` D5). I
  saw r102-1's own broadcast in the capture, not the dropped frames themselves.
- **F2. ifdown/ifup mid-stream had two outcomes** (table). Not explained; not a driver
  regression on the evidence (stack 2, stock driver).
- **F3. `ethtool -C rx-usecs 50` returns `ERANGE`** with patch 1 alone and with the whole
  series. I did not compare it with the pre-series driver.
- **F4. The `ncdevmem` TX client refuses a sender with one queue** ("number of device queues
  is too low"); one of my runs failed there before I restored 4 channels (`E/66-*`).

## 6. Bugs fixed

Backport: **none found, none fixed**; `O/tree` and `O/patches` are as their engineer left
them. In my harness, each found because a case "failed":

1. `load.sh`: `ib_send_bw` server loop waited 90 s on a dead control connection (`E/11-*-harness-artifact.txt`).
2. `rxlib.sh`: `pkill -f` of the sender matched its own shell, so the mid-stream cases had
   no sender (`E/52-*-harness-bug.txt`, `E/53-*-harness-bug.txt`); T1 and T5 were rerun.
3. `hds.sh`: jumbo frames on a 1500-byte L2 (section 4.4).

## 7. What remains

- GPU buffers into devmem RX, on the stock and on the P2P attach path (the emulator must
  DMA-write into BAR0): the backport note's risk 4, untouched.
- Jumbo frames need the stack's L2 raised (stack 1's `stack.sh jumbo` does it); VLAN.
- The modules are loaded by `insmod`; nothing is in DKMS or the image. For the repository:
  `impl-rx-backport-7.2.6.md` section 9.
- The lockdep kernel covers one guest for one boot, without KASAN; firmware-down, link-flap
  and queue-op failure paths are as untested here as on v7.4.
- The 15-frag TX limit and its `ETIMEDOUT` case are unchanged by this work.

## 8. Files

    R/scripts/  lib.sh rxlib.sh opslock-run.sh hds.sh rx-sizes.sh rx-teardown.sh final-boot.sh
                kernel-select3.sh kbuild-lockdep.sh kbuild-lockdep-in-container.sh
    R/guest/    opslock.sh load.sh rdmaq.sh stress.sh pools.sh nettool.py skbshape.bt
                xdp_pin.py hwtstamp.py ynl/   (copied to ~/rx in both guests)
    R/kbuild-lockdep/   source, build tree, debs, config-diff.txt, SOURCE
    R/evidence/ 00-06 build and loads; 10-12 patch 1; 20-22 series on compat; 30-33 ops
                locking; 40 header split and 50-55 devmem RX on the lockdep boot; 60-69 fleet
                kernel; 90 final state; quickstart/, srq/
    B/ionic-mod/{rxbp,rxbp-p1}/   the staged trees and their SOURCE files
