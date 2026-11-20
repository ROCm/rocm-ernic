# Runtime test of the two carried ionic fixes on the v7.2.6 base (2026-11-19, night)

The two fixes in rocm-ernic's base patch series, run on the configuration the fleet ships:
v7.2.6 ionic sources built as an external module on the `7.2.15-devmem` kernel.

- `0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch` (= `srq/0010`)
- `0004-ionic-free-every-temporary-queue-after-a-queue-count.patch` (= `srq/0011`)

**Result: both bugs reproduce on this base without the fixes, and both are gone with them. The
regression runs with the shipped module pass.** Two of my own harness mistakes and one emulator
behaviour got in the way of the regression runs; they are listed in section 4, with the evidence
of the failed attempts kept.

Everything is under `/var/tmp/ionic-devmem/overnight/a/` (`A` below): `A/scripts`, `A/evidence`
(`E` below), `A/ionic-mod`, `A/series`, `A/patches-head`. Stack 1 only (compose project
`ionic-devmem`). Nothing in any repository was changed.

## 1. Configuration under test

| Item | Value |
| --- | --- |
| Guests | devmem-1, devmem-2, both on `7.2.15-devmem` (no KASAN, no KFENCE; `DMA_API_DEBUG=y`, `DEBUG_NET=y`), gcc 15.2.0. Guest 1 was on `7.4.0-rc3-devmem-kasan-p2p`: its GRUB default was rewritten in place and the stack cold-cycled (`stack.sh down`/`up`), no warm reboot (`E/01-guest1-select-7.2.15.txt`) |
| Patches | `git archive HEAD ansible/files/patches` of rocm-ernic at `2d3a3d02` into `A/patches-head`. `0003`/`0004` are byte-identical to `srq/0010`/`srq/0011` (`cmp`) |
| Sources | `git archive v7.2.6` of the ionic eth and rdma drivers and `include/uapi/rdma`, patches applied with `git apply` in a plain directory, laid out with the Kbuild `setup-ionic-dkms.sh` writes (`A/scripts/stage.sh`) |
| Module (i) `fix726-nofix` | `srq/0001`..`0009` + `p4/0001`: the bugs present. `ionic.ko` build-id `214bb942…` |
| Module (ii) `fix726-shipped` | `srq/0001`..`0011` + `p4/0001`: as shipped. `ionic.ko` build-id `538bf05b…` |
| Difference (i) vs (ii) | exactly the two hunks, in `ionic_txrx.c` and `ionic_lif.c` (`E/00-module-source-diff.txt`) |
| Build | in each guest, `make -C /lib/modules/7.2.15-devmem/build M=… W=1 modules`: `ionic.ko` + `ionic_rdma.ko`, no compiler warning (`E/02-module-build.txt`). The same `.ko` hashes in both guests |
| Which module ran | every run prints the loaded module's build-id read from `/sys/module/ionic/notes/.note.gnu.build-id` |
| Emulator | `T/ernic/tx-offloads-1/build/rocm-ernic`, the binary stack 1 already ran (not rebuilt, not HEAD's emulator) |
| Addresses | 10.213.100.1N on `rocm-ernic0` for Ethernet tests: 192.168.100.x between the guests is still broken by the host (`ionic-devmem-rx_l2` is still 192.168.96.0/20) |

Not done the fleet's own way: `fetch-ionic-sources.sh` (`git am`) and `setup-ionic-dkms.sh` were
not run; the patches were applied with `git apply` and the module built with a plain `make M=`,
not through DKMS. The resulting tree for (i) is `diff -rq`-identical to the earlier
`T/ionic-mod/ernic-base`, which was checked against the image's `/usr/src/ionic-ernic-7.2.6`.

## 2. Results

| Case | Module | Result | Evidence |
| --- | --- | --- | --- |
| XDP_TX, `xdp.frags`, MTU 9000, 10,000 distinct 8000-byte datagrams echoed on one queue | (i) | **BUG REPRODUCED.** 5000 of 10,000 echoes captured on each emulator's tap0 have their last 426 bytes wrong (payload offsets 7574-7999); all 5000 carry another datagram's bytes at the same offsets; all 10,000 requests intact. Receiver: 5000 verified, 5000 lost, `UdpInCsumErrors` +5000 | `E/10-xdp-module-i-nofix.txt`, `E/pcap/XDP-nofix-tx_frags.*` |
| XDP_REDIRECT, same | (i) | **BUG REPRODUCED.** 5000 of 10,000 bad on both taps; 4999 are another datagram's bytes, **1 (seq 0) is stale kernel memory, including a kernel pointer (bytes `0070bb017490ffff` = `0xffff907401bb7000`)**: the information leak. Receiver: 5000 verified, `UdpInCsumErrors` +5000 | same file, `E/pcap/XDP-nofix-redirect_frags.*` |
| XDP_TX, same | (ii) | **PASS.** 10,000 of 10,000 echoes intact on both taps; receiver verified 10,000, 0 bad, 0 lost; `UdpInCsumErrors` unchanged | `E/11-xdp-module-ii-shipped.txt`, `E/pcap/XDP-shipped-tx_frags.*` |
| XDP_REDIRECT, same | (ii) | **PASS.** 10,000 of 10,000 on both taps; receiver verified 10,000, 0 lost | same file, `E/pcap/XDP-shipped-redirect_frags.*` |
| Queue count: `ifdown; ethtool -L combined 2; ifup; ethtool -L combined 4; rmmod`, once, clean boot state | (i) | **BUG REPRODUCED.** DMA-API coherent entries 50 -> 62 after the sequence; page pools on the netdev 4 -> 6 (two with no NAPI id and 0 in flight). At `rmmod`: `DMA-API: pci 0000:01:00.0: device driver has pending DMA allocations while released from device [count=12]` and its WARNING (`error_count` 0 -> 1), 12 coherent entries left for a device with no driver, and 2 orphan page pools (no ifindex) left listed | `E/21-leak-module-i-nofix-1x1.txt` |
| Page-pool dump after that unload | (i) | **Freed memory read, shown without KASAN.** A bpftrace kprobe on `page_pool_nl_fill` during a `page-pool-get` dump: orphan pools 72 and 70 still have `p.napi` = `0xffff90740568ba78` / `0xffff907404569278` with ionic unloaded and its netdev gone, and the function reads `napi_id` through them (0 was read both times, so the dump shows no `napi-id` and nothing crashes). **A KASAN report is not obtainable on this kernel (KASAN not configured): NOT RUN.** The guest was not crashed; two dumps were made in that state | `E/22-leak-module-i-orphan-pool-napi-pointer.txt` |
| Same sequence, once | (ii) | **PASS.** Coherent 50 before and after; pools 4 and 4, none without a NAPI id; after `rmmod`: no entry for the device, 0 orphan pools, no DMA-API line, `error_count` 0 | `E/20-leak-module-ii-shipped-1x1.txt` |
| Same sequence 9 times: 3 loads x 3 sequences, after a cold-cycle | (ii) | **PASS.** After each of the 9 sequences: coherent 50, phy 2560, 4 pools, 0 orphans. After each of the 3 unloads: no entry for the device, 1 pool total (the virtio NIC's), no DMA-API or WARNING line, `error_count` 0 | `E/23-leak-module-ii-shipped-3x3.txt` |
| TCP 256 MiB each way, sha256 at the receiver | (ii) | **PASS** both ways (1.47 s, 1.62 s) | `E/31-regress-tcp-hash-256M.txt` |
| iperf3 TCP, 10 s each way (byte counts, not payload) | (ii) | **PASS.** 1 -> 2: 1533 Mbit/s, 2 -> 1: 1670 Mbit/s, 0 retransmits | `E/32-regress-iperf-and-rdma-over-alt-gid-FAILED.txt` (top) |
| rping, 50 pings of 4096 bytes validated by rping, each way | (ii) | **PASS** each way, over the 192.168.100.1N GIDs (see 4.1) | `E/33-regress-rdma.txt`, again in `E/38-…` |
| `ib_send_bw`, 20,000 x 4096 bytes, each way (no payload check) | (ii) | **PASS**: 259 and 195 MiB/s | `E/33-regress-rdma.txt` |
| Reconfiguration under traffic: sha256-verified TCP streams both ways at once for 150 s, while 14 steps run on guest 1 and then the same 14 on guest 2: `-L` 2, 1, 4; `-G rx 4096`; `-G tx 2048`; `-G rx 1024 tx 1024`; MTU 9000; MTU 1500; `-L` 3, 4; then the leak sequence itself (down, `-L 2`, up, `-L 4`) | (ii) | **PASS.** 1 -> 2: 20,544,749,568 bytes, 2 -> 1: 20,643,315,712 bytes, sha256 equal at the receivers. Every command rc=0; after each of the 24 steps that leave the link up, both streams moved a further 1 MB within 0.25-0.92 s. `TcpInCsumErrors` 0 | `E/37-regress-churn-under-traffic.txt` |
| Kernel log and counters after all of the above | (ii) | **Clean for the driver.** Both guests: DMA-API `error_count` 0, coherent 50, 4 pools, 0 orphans, no WARNING/BUG/DMA-API line. Lines that are not the driver's routine messages: `ionic … opcode 9 error 7` twice per guest (the refused MODIFY_QP of 4.1) and one `clocksource: Watchdog remote CPU 4 read timed out` on guest 2 (4.4) | `E/37-…` (second half) |
| Unload after the regression, reload | (ii) | **PASS.** No DMA-API or WARNING line since boot on either guest, `error_count` 0, 4 pools after the reload; 64 MiB sha256 transfer and rping pass each way afterwards | `E/38-final-reload-and-state.txt` |

## 3. What the runs add to what was known

- The XDP numbers on this base equal finding B1 on v7.4-rc3 exactly: 5000 of 10,000, 426 bytes,
  offsets 7574-7999. The redirect run again shows the first bad frame carrying stale kernel
  memory rather than a neighbour's payload.
- The queue-count numbers equal the v7.4-rc3 ones: 12 leaked allocations and 2 orphan pools for
  one `2 -> 4` growth (3 allocations per qcq, a Tx and an Rx qcq for each of the 2 new queue
  indices).
- On a kernel without KASAN the dangling NAPI is silent: the dump succeeds and the pool merely
  shows no `napi-id`. That is what a fleet guest would see. The only loud symptom without the
  fix is the DMA-API warning at unload, and only with `DMA_API_DEBUG`.
- `ansible/files/patches/README.md` says "Neither has been run on `v7.2.6` itself". That
  sentence is now out of date; I did not edit the repository.

## 4. Things that went wrong, and why

1. **RDMA over the hand-added 10.213.100.1N address fails, by design of the emulator.** rping:
   `RDMA_CM_EVENT_UNREACHABLE, error -110`; `ib_send_bw`: `Failed to modify QP … to RTR`. The
   emulators log `no mesh node owns destination gid ::ffff:0ad5:640c` and refuse the MODIFY_QP
   (guest: `opcode 9 error 7`) and drop the QP1 datagrams. The GID of a second IPv4 address
   added in the guest is in the guest's GID table (index 2) but the mesh does not resolve it.
   Over the 192.168.100.1N GIDs (index 1) RDMA works, although IP to those addresses does not
   (ARP still resolves, and RDMA rides the mesh). Not investigated further: whether the mesh is
   meant to learn a GID added after start is a question for the emulator, not for these fixes
   (`E/32-…-FAILED.txt`).
2. **Harness bug, first churn run:** the listener was started with `sh -c` and used bash
   process substitution, so no listener existed and no traffic flowed. The reconfiguration
   steps of that run executed against an idle link; it proves nothing about traffic
   (`E/34-regress-churn-HARNESS-BUG-no-traffic.txt`).
3. **Second churn run failed on both hashes; cause is the test bed, not the driver.** Both
   streams died at guest 1's `ip link set down` with `Connection reset by peer`. With the netdev
   down its connected route is gone, the socket was bound to an address but not to the device,
   so the next segment left through the default route on the SLIRP NIC, and QEMU's user network
   answered with a RST. Captured on `enp0s5`: the segment `10.213.100.11 > 10.213.100.12` and
   the `[R]` coming back (`E/36-churn-failure-diagnosis-slirp-reset.txt`; failed run in
   `E/35-…-FAILED-sockets-not-bound-to-device.txt`). Fixed in the harness with
   `so-bindtodevice=rocm-ernic0`; the passing run is `E/37`. Anyone taking an ionic netdev down
   under a TCP test in these guests will hit this.
4. `clocksource: Watchdog remote CPU N read timed out` appeared once on guest 1 (module (i),
   during the XDP redirect run) and once on guest 2 (0.5 s after loading module (ii)). It is the
   clocksource watchdog failing to get a timely answer from another vCPU; the host is shared
   and ran three stacks. Seen with both modules; not attributed to the driver, not investigated.

## 5. Not run

- KASAN confirmation of the page-pool use-after-free on this base (no KASAN in `7.2.15-devmem`).
- The fleet's own build route (`fetch-ionic-sources.sh` + DKMS); the stock `7.2.6-070206-generic`
  kernel; module (i) under the regression runs; HEAD's emulator binary.
- The base series without `srq/` (`0002`-`0004` alone) and the `devmem-tx` series on top.
- XDP with a devmem stream on the same queue (the 7.4-rc3 run had one; here the echo ran alone),
  XDP at MTU 1500, more than one Rx queue under XDP.
- iperf3 UDP, IPv6, VLAN.
- Whether module (i) leaks again on a second sequence within one load (run once only).

## 6. Scripts (`A/scripts`)

- `stage.sh NAME SERIES_DIR...`: stage a module tree from v7.2.6 and series directories.
- `mod.sh build|load NAME [GUESTS]`, `mod.sh unload [GUESTS]`: like `ionic-mod.sh`, reading
  `A/ionic-mod`; `load` writes a kernel-log marker instead of clearing the log, adds
  10.213.100.1N and prints the running build-id.
- `xdp-diag.sh NAME OBJ SEC [SIZE] [MTU] [SECS] [PPS]`: `m-xdp-diag.sh` without the devmem stream.
- `leak.sh NAME LOADS SEQS`: the queue-count sequence with DMA-API and page-pool counts.
- `regress.sh iperf|rdma|churn|log`.

The originals in `T/scripts` were used unchanged (`stack.sh`, `gssh`, `tcp-test-alt.sh`,
`devmem-tx/lib.sh`, `devmem-tx/udpsum.py`).

## 7. State left

Stack 1 up (cold-cycled twice in this run, last at 02:17Z), both guests on `7.2.15-devmem`
(GRUB default on both now), module (ii) `fix726-shipped` loaded on both with `ionic_rdma`
(`~/ionic-mod/fix726-shipped`; `fix726-nofix` is built there too). 4 queues, rings 1024, MTU
1500, offloads on, no XDP program, 10.213.100.1N/24 added. Not persistent: a reboot returns to
the kernel's in-tree ionic; `A/scripts/mod.sh load fix726-shipped` reloads it. Server unchanged
(`tx-offloads-1`). No capture container left. `E/pcap` holds 1.3 GB (eight captures of 160 MB).
The patched amdgpu and the `-p2p` kernel are still installed in guest 1, just not booted.
