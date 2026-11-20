# devmem TCP TX on emulated ionic: test matrix on 7.2.15-devmem (ERNIC configuration)

Run 2026-11-18 (host clock; evidence files are stamped in UTC, 2026-11-19 03:00-04:00) on
stack 1 (`ionic-devmem`, guests `devmem-1` = sender 192.168.100.11, `devmem-2` = receiver
192.168.100.12). `T` = `/var/tmp/ionic-devmem/test`, `E` = `T/evidence/tx-7.2.15`.

**Result: every matrix row passed for the driver and the emulator. No product bug was found in
the driver series, the backport or the emulator; nothing in them was changed.** One row ends in
a kernel oops that is in the networking core, not the driver (module unload with a live TX
binding, F8). Nine findings are listed in section 5; none is a defect of the series.

## 1. Configuration under test

| Item | Value |
| --- | --- |
| Guest kernel | `7.2.15-devmem` (v7.2.15 + the bundle's dmabuf backport; DMA_API_DEBUG=y, DEBUG_NET=y; **no** KASAN, lockdep, kmemleak, KFENCE) |
| ionic | `T/ionic-mod/ernic-dmtx1`: v7.2.6 DKMS sources + srq + p4 + the seven patches of `/var/tmp/ionic-devmem/patches/ernic-v7.2.6/` (sha256 in `ernic-dmtx1/SOURCE`; `diff -rq` against `rocm-ernic/ansible/files/patches/devmem-tx/` shows only the README extra there). `ionic.ko` + `ionic_rdma.ko`, built in each guest, loaded in both |
| Emulator | `T/ernic/tx-offloads-1/build/rocm-ernic`, sha256 `29413ea9…7ef118`, in both `ernic-N` containers; not rebuilt |
| Offloads seen | `tx-checksumming on`, `scatter-gather on`, `tcp-segmentation-offload on`, GSO on; 4 queues; rings 1024/1024 |
| Attach flavour | **static**: `dma_buf_attach()` + `dma_buf_map_attachment_unlocked()`, no pin, no dynamic attach (`T/kbuild/7.2.15/src/net/core/devmem.c:229,236`; at run time the bind's call chain is `net_devmem_bind_dmabuf -> dma_buf_map_attachment_unlocked -> dma_buf_map_attachment -> get_sg_table (udmabuf) -> dma_map_sgtable`, `E/M2-dmesg-guest1-first-bind.txt`; `/sys/kernel/debug/dma_buf/bufinfo` shows the udmabuf with "Attached Devices: 0000:01:00.0") |
| IOMMU in the guest | **none**: `/sys/kernel/iommu_groups` and `/sys/class/iommu` are empty, no DMAR/AMD-Vi line in the boot log (only "iommu: Default domain type: Translated"). DMA is direct: a frag's DMA address is the page's physical address, which the PFN check below relies on and confirms |
| Exporter | udmabuf (memfd), 4 or 16 MiB |

## 2. Test program and harness

- **M6 program source: `T/src/devmem_tx.c`; static binary: `T/bin/devmem_tx`** (sha256
  `1579ebae…e9aa1e`; built on the host with `gcc -O2 -Wall -Wextra -static`, no warnings;
  installed in both guests as `/usr/local/bin/devmem_tx` by `T/scripts/devmem-tx/install.sh`).
  - Sender: memfd + udmabuf filled with a position-dependent pattern (64-bit mix of the dmabuf
    offset and a seed), bind-tx by raw generic netlink (`NETDEV_CMD_BIND_TX`, ifindex + fd, extack
    printed), `SO_ZEROCOPY`, `sendmsg(MSG_ZEROCOPY)` + `SCM_DEVMEM_DMABUF` with arbitrary
    `iov_base` offsets / lengths / iovec counts / gaps, reaps every completion from the error queue
    and checks range continuity, `ee_errno` and `SO_EE_CODE_ZEROCOPY_COPIED`.
  - Receiver (`-r`): plain TCP sink that regenerates the expected stream from the same plan
    options and compares every byte (reports first mismatch, mismatch count, how many mismatching
    bytes are zero). It has no knowledge of what the sender believes it sent.
  - Also: `-m copy|zc` controls, `-X close-sock|unbind|free-dmabuf|exit`, `-R` (retry a send
    refused while the route is gone), `-F` (dump the dmabuf's PFNs), `-D` (DMA-API accounting
    around the bind). Options are documented at the top of the source.
  - Self-test on the host loopback: a deliberately wrong plan on one side is reported as
    mismatches; `-m copy` and `-m zc` pass.
- Capture tool: `T/src/pcapcap.c` -> `T/bin/pcapcap` (static AF_PACKET -> pcap), run in a
  one-off container `ionic-devmem-cap-1` in emulator 1's network namespace on `tap0` (what the
  emulator put on the wire, after its TSO and checksum work). Removed after each use.
- Harness: `T/scripts/devmem-tx/` (`lib.sh`, `probe.bt`, `m3-m5.sh`, `m6.sh`, `m7.sh`, `m7x.sh`,
  `m8.sh`, `m9.sh`, `m9-rmmod.sh`, `m10.sh`, `summarize.py`, `pcapsum.py`, `install.sh`;
  `ALT=1` switches the guest addresses to 10.213.100.1N, see F9).
  `devmem_case` runs one case and prints: feature state, sender and receiver results, the
  bpftrace maps, the PFN check, changed counters of both guests and both emulators, the DMA-API
  check, and the kernel log of both guests since a marker written to `/dev/kmsg`.
  `summarize.py FILE...` prints one line per case.

## 3. Observability: how a silent copy fallback is excluded

For every devmem case (bpftrace on the sender, `probe.bt`; the patched module has no BTF, so
kprobes with vmlinux BTF types):

1. `kprobe:ionic_start_xmit`: counts skbs with `skb->unreadable` set, sums their `data_len`,
   histograms `nr_frags`, and checks bit 0 of `frags[0].netmem` (net_iov). In every positive case
   the unreadable payload bytes are >= the bytes the receiver verified (equal without
   retransmissions) and the only readable skbs are the handshake/FIN (4-6 skbs, < 320 bytes).
2. `kretprobe:ionic_tx_map_frag` while `ionic_tx_map_skb` runs on an unreadable skb: the return
   value is the DMA address the driver stores for the descriptor. Its page number, keyed by the
   socket's destination port, is compared with the PFNs of the dmabuf read from
   `/proc/self/pagemap` by the sender: **every frag address of the test's connection lies in the
   test's dmabuf** (`PFN-CHECK ... outside-the-dmabuf=0`).
3. `kprobe:dma_map_page_attrs` / `dma_map_phys` in the same window: exactly one call per
   unreadable skb (the linear header), none for the frags (e.g. 2201 skbs, 2201 map calls,
   35k frags in `E/M4-tso-on.txt`).
4. Completions: every send completed, `SO_EE_CODE_ZEROCOPY_COPIED` never set (the one exception
   is core behaviour after a route failure, finding F4).
5. `DMA_API_DEBUG`: `error_count` is read around each case. The only reports are the known
   "sg segment longer than device claims" ones raised inside the bind (finding F1); the harness
   subtracts those (`DMA-API-CHECK ... other=0`). A wrong unmap of a net_iov frag would show here.
6. Leak checks: `net_devmem_bind_dmabuf` / `net_devmem_unbind_dmabuf` /
   `__net_devmem_dmabuf_binding_free` call counts (1/1/1 per case), and
   `/sys/kernel/debug/dma_buf/bufinfo` back to 0 objects (after the orphaned connection of a
   stalled case has timed out).
7. Counters after every case: `tx_unreadable_drop`, `tx_N_linearize`, netdev `tx_dropped`,
   emulator `tx_dma_error` / `tx_desc_data_error` / `tx_desc_fetch_error` (always 0), kernel log
   splat lines in both guests.

Negative control: the series without patch 0006 (section 4, M7) delivers 2 MB of zeroes that the
receiver reports as 1,990,757 mismatches, so the receiver check does catch corruption.

## 4. Matrix

"clean" = no new kernel log line of any kind in either guest except those quoted, DMA-API "other"
= 0, emulator error counters 0, dma-buf objects back to 0.

| Row | What was run | Result | Evidence |
| --- | --- | --- | --- |
| M1 | `ncdevmem -f rocm-ernic0 -s .12 -p 5302 -t 0 -q 1` on the unpatched in-tree ionic and on the unpatched DKMS-source ionic (`ernic-base`), new emulator (SG, csum, TSO on): `netdev_bind_tx: Operation not supported`, extack "Driver does not support netmem TX", nothing received | PASS | `E/M1-baseline-intree.txt`, `E/M1-baseline-ernic-base.txt` |
| M2 | Same command on the patched module: `got tx dmabuf id=1`, two sends completed, receiver sha256 equal | PASS | `E/M2-ncdevmem-bind-patched.txt` (first bind logs F1: `E/M2-dmesg-guest1-first-bind.txt`) |
| M3 | TSO off (GSO on) and TSO+GSO off, csum on, MTU 1500: 128 MiB page-aligned (128 x 1 MiB) and 57 MiB unaligned (600 x 100003 bytes from offset 123, advance 7919). 92,800 / 41,677 unreadable non-GSO skbs, `ip_summed` CHECKSUM_PARTIAL | PASS, 0 mismatches, clean | `E/M3-tso-off.txt` |
| M4 | Same two streams, TSO on: 2201 / 993 unreadable GSO skbs, up to 17 frags each; `tx_N_linearize` +0, `tx_unreadable_drop` +0, netdev `tx_dropped` +0, 0 retransmissions | PASS, clean | `E/M4-tso-on.txt` |
| M5 | MTU 9000 with TSO on and off, MTU 9198 (the ionic maximum) with TSO on; both streams each; ping -M do at full size first | PASS (6 cases), clean. Jumbo works end to end up to 9198, which the offload validation found to be the ionic maximum (tap/bridge are at 9216, set by `stack.sh`) | `E/M5-mtu.txt` |
| M6 positive | (a) 300 x 1 byte at odd offsets; (b) 300 x 2 bytes across a page boundary; (c) 500 x 100003 bytes, odd offset, one iovec over 25 pages; (d) 300 x 250007 bytes in 50 iovecs of 5003 with 1237-byte gaps; (e) the last 4095 bytes of the dmabuf; (f) 480 iovecs of 128 bytes per send, TSO on; (g) **15 iovecs of 64 bytes, one per page, one send per skb: non-GSO skbs with exactly 15 frags** (300 of them, all sent); (h) 1024 iovecs of 1400 bytes; (i) as (f) with TSO off | PASS (9 cases), 0 mismatches, 0 drops, clean | `E/M6-positive.txt` |
| M6 negative | (n4) 16 iovecs of 64 bytes (one frag more than (g)); (n1, n2) 1024 iovecs of 16 bytes; (n0) 300 one-byte sends back to back. Every skb with 16 or 17 frags is dropped by the driver and counted: `tx_unreadable_drop` rises with each transmission attempt of such an skb (8-10 per case; equal to the number seen at `ionic_start_xmit`, or one more when the orphan retransmitted after the probe stopped), `tx_N_linearize` +0, netdev `tx_dropped` +0. Receiver gets 0 bytes (n0: the 43 bytes sent before a 16-frag skb formed, verified). No crash, no log line, no DMA-API report; binding and dmabuf freed once the connection dies; a positive run straight after passes (n3). What userspace sees: section 4.1 | PASS (clean counted drop) | `E/M6-negative.txt`, `E/M6-first-attempt-1byte-sends-stall.txt` |
| M6 threshold | 512 iovecs of 80, 90, 100 and 110 bytes per send, MTU 1500 (MSS 1448), `tcp_retries2=5`: 80 and 90 deliver nothing, 100 stalls after 3400 verified bytes, **110 passes** (3.6 MB, 0 drops). So at MTU 1500 the limit bites for pieces of about 100 bytes and less | as designed | `E/M6-edge-frag-sizes.txt` |
| M7 | Sender `ethtool -K rocm-ernic0` with `tx off`, `sg off`, `tx off sg off`, each also with `gso off`; set before the connection (100 KB sends and 1 KB sends) and 3 s into a stream. 15 cases, each with a capture on emulator 1's tap0 and in the receiver guest. **No payload frame left the emulator and the receiver got 0 bytes in every before-connect case; in the during-stream cases the receiver verified a prefix (14.7 MB, 0 mismatches) and then nothing; no all-zero payload, no bad checksum on tap0.** Mechanism per case: section 4.2 | PASS | `E/M7-tx.txt`, `E/M7-sg.txt`, `E/M7-both.txt`, `E/M7-gso.txt`, `E/pcap/M7-*.pcap` |
| M7 control | Sender module built **without patch 0006**, `tx off sg off`: 1,998,705 bytes arrive, **all zero**, valid TCP checksums, completions returned with `copied=0` | shows 0006 is needed on this core | `E/M7-nobackstop-variant.txt` |
| M8a | Bound on rocm-ernic0, no SO_BINDTODEVICE, destination 10.0.2.2:2222 (routed via enp0s5): connect works, first `sendmsg` returns **ENODEV**, nothing queued | PASS | `E/M8-other-netdev.txt`, `E/pcap/M8a.sender-enp0s5.pcap` |
| M8b | Stream to .12 via rocm-ernic0; 3 s in, `ip route add 192.168.100.12/32 via 10.0.2.2 dev enp0s5`: next `sendmsg` returns ENODEV; the 9 already-queued unreadable skbs (retransmissions) are freed in `validate_xmit_skb` on enp0s5 (`kfree_skb` stacks; enp0s5 `tx_dropped` +9); the enp0s5 capture has 6 frames for the port, all without payload (RSTs); receiver verified the 14.7 MB sent before and nothing after | PASS | same file, `E/pcap/M8b*.pcap` |
| M9 close socket | 64 x 1 MiB queued, TCP socket closed with 3 sends uncompleted, netlink socket 3 s later: receiver verified all 64 MiB | PASS, clean | `E/M9-close-unbind-free-exit.txt` |
| M9 unbind | Same, netlink socket closed first: a send with the stale id returns EINVAL; all 64 completions arrive; receiver verified all | PASS, clean | same |
| M9 free dmabuf / exit | Same, dmabuf + memfd + udmabuf fds closed and unmapped first; and `_exit()` with everything in flight: receiver verified all 64 MiB both times; binding freed | PASS, clean | same |
| M9 link down/up | Sender `ip link set down`, 2 s, `up`, 3 s into a 15 s stream: sends fail with EHOSTUNREACH while down, then EINVAL until TCP re-routes (finding F4); with the sender retrying, **the whole 252 MB stream verified**, 115 retransmissions, no log line besides link down/up | PASS (driver); core quirk F4 | `E/M9-link-down-up.txt`, first run without the EINVAL retry: `E/M9-link-down-up.first-run-EINVAL-after-link-up.txt` |
| M9 ring / channel / MTU | During a 25 s stream on the sender: `-G tx 512`, `-G rx 2048`, `-G tx 8192`, `-L combined 2`, `1`, `4`, MTU 1400, 1500, `-G tx 1024 rx 1024`: **460 MB verified**, all 4602 completions, 829 retransmissions, only the driver's "Changing ..." lines | PASS, clean | `E/M9-reconfig.txt` |
| M9 module unload, no live binding | Receiver stopped, 672 KB queued, netlink socket closed (unbound), sender exited, then `rmmod ionic_rdma ionic` with the orphaned connection still holding 581 KB of unacked devmem skbs: unload completes, no crash; one DMA-API warning from the core's mapping (F5); details in 4.3 | PASS for the driver; core F5 | `E/M9-rmmod-a-unbound-with-unacked-data.txt` (rerun with a capture: `E/M9-rmmod-a2-*.txt`) |
| M9 module unload, live binding (run last) | `rmmod` 4 s into a stream while the sender holds its binding: unload completes, the receiver has verified everything sent before (83.4 MB); **the kernel then oopses in `netdev_nl_sock_priv_destroy()` when the sender's netlink socket is closed** (core use-after-free, F8). The driver is not loaded at that point | **FAIL in the core** (not the driver); see 4.3 | `E/M9-rmmod-b-live-binding.txt`, `E/M9-rmmod-b-dmesg-guest1-oops.txt` |
| M10 | 360 s sustained send, 1000003 bytes per `sendmsg` in 4 iovecs of 250001 with 4099-byte gaps, start offset 17 advancing by 65521 (covers all 4096 pages of a 16 MiB dmabuf), TSO on, MTU 1500, receiver verifying as it reads: **69,148,207,444 bytes (64.4 GiB) verified, 0 mismatches; 69,148 sends, 69,148 completions, 0 COPIED, 0 gaps**; details in 4.4 | PASS, clean | `E/M10-sustained.txt` |

### 4.1 What userspace observes when the frag limit is exceeded (M6 negative)

The device takes the header plus 15 frags; the core fills skbs to `MAX_SKB_FRAGS` = 17 and makes
one frag per iovec piece per page. Measured boundary: 15 frags sent, 16 dropped.

- `sendmsg` succeeds while the send buffer has room (each devmem frag is charged a page of
  truesize, so that is few bytes: 544 of 16384 with 16-byte iovecs, 66 one-byte sends), then
  blocks. With `SO_SNDTIMEO` it returns a short count or EAGAIN after the timeout. No error is
  reported for the dropped data and `SO_ERROR` stays 0.
- Nothing reaches the receiver; the sender's TCP retransmits the same skb with backoff and the
  driver drops it each time (`tx_unreadable_drop` +8..10 in 10 s).
- With `net.ipv4.tcp_retries2=5` the blocked `sendmsg` returned a short count after 13.2 s and
  `SO_ERROR` = ETIMEDOUT. With the default 15 the wait is about 15 minutes (not waited for).
- **A zerocopy completion is still delivered for the dropped send, with `ee_errno` 0 and no
  COPIED flag** (n2: `completed=1`), when the queue is purged. A sender cannot learn from
  completions that data was not transmitted (core behaviour, as kernel-core-devmem-tx.md says).
- After `close()` the connection lingers as an orphan and keeps the binding and the dmabuf
  alive until TCP gives up: 75-80 s here with default sysctls, 0 s with `tcp_retries2=5`.
- The drop is visible only in `ethtool -S` `tx_unreadable_drop` (and per queue); netdev
  `tx_dropped`, `hw_tx_dropped` and the emulator's counters do not move.
- Small sends count like small iovecs: TCP appends each send to the unsent tail skb, so 300
  back-to-back one-byte sends form a 16-frag skb after 15 bytes (cwnd 1, 2, 4, 8, then 16) and
  the connection stalls there (n0 and the first attempt). Sent one at a time (each completion
  awaited), the same 300 sends pass (M6a).

### 4.2 M7: which mechanism stopped each case

The driver gates `netmem_tx` at probe only, so the bind and `sendmsg` succeed in all these cases
(TCP always adds `NETIF_F_SG | NETIF_F_HW_CSUM` to the socket's route caps, `gso off` included).
Stacks are from `tracepoint:skb:kfree_skb` on unreadable skbs; counts from the probe.

| Features off | skb | Stopped by | Seen |
| --- | --- | --- | --- |
| tx | GSO (100 KB sends) | core: `skb_segment()` fails (`kfree_skb_list_reason <- skb_segment <- ... <- validate_xmit_skb`) then `validate_xmit_skb` frees the skb | netdev `tx_dropped` +9; 0 unreadable skbs at `ionic_start_xmit` |
| tx | sub-MSS (1 KB sends) | core: `validate_xmit_skb` (`skb_checksum_help` cannot checksum it) | `tx_dropped` +9 |
| sg | GSO | core: `skb_segment()` (`skb_copy_bits` fails) | `tx_dropped` +10 |
| sg | sub-MSS | core: `validate_xmit_skb` (linearize fails) | `tx_dropped` +10 |
| tx + sg | GSO | **driver backstop (patch 0006)**: `skb_segment()` succeeds, `skb_copy_and_csum_bits()` ran on the unreadable skb 10 times and produced 10 linear skbs still marked unreadable with no frags; all 10 reached `ionic_start_xmit` and were dropped: `tx_unreadable_drop` +10 | 10 = 10 = 10; during-stream case 115 = 115 = 115. Netdev `tx_dropped` also +9: single-MSS retransmissions, dropped in the core as in the next row |
| tx + sg | sub-MSS | core: `validate_xmit_skb` (linearize fails) | `tx_dropped` +10 |
| any of the above + gso | both | same as without `gso off` | `E/M7-gso.txt` |

In the during-stream cases, skbs already built keep flowing until the features change; after
that the same mechanisms apply and the connection stalls. After `ethtool -K ... on` a stalled
connection's retransmissions are sent again (seen with an orphan, F6).

### 4.3 M9: module unload

**a) unbound, unacked data still queued.** The unload itself is clean in the driver: `rmmod`
returns 0, no crash, no ionic message besides "ionic removed". At driver unbind DMA-API debug
warns "device driver has pending DMA allocations while released from device [count=413] ...
mapped as scatter-gather", mapped at `dma_buf_map_attachment`: the binding's mapping, kept by the
skbs of the orphaned connection (F5). After `insmod` the receiver, resumed, verified the bytes
it had already buffered (90,544; 85,424 in the rerun) and then got ECONNRESET. A control run
without `rmmod` delivers and verifies all 671,872 queued bytes
(`E/M9-rmmod-a-control-no-rmmod.txt`), so the reset comes from the unload. Probable cause, not a
devmem matter: the sender's socket is bound to the netdev with `SO_BINDTODEVICE`, the re-probed
netdev has a new ifindex (5 -> 6), so the orphan no longer matches incoming segments and the
sender's stack answers the receiver with a RST (sender `TcpOutRsts` +1 in both runs). The
receiver-guest capture of the rerun (`E/pcap/M9-rmmod-a2.receiver-guest.pcap`) does not contain
that RST, which I could not explain. The orphan can neither send nor be reached afterwards and
kept its 16 MiB dmabuf attached for more than 200 s (gone within 8 minutes) until TCP gave up;
that is the core's lifetime rule and the price of unloading with unacked devmem data.

**b) live binding, stream running.** `rmmod` returned 0 with the binding live; DMA-API printed
the same pending-allocations warning. The sender's next `sendmsg` returned short and `SO_ERROR`
was ENETUNREACH; the receiver verified all 834 sends that had completed. When the sender then
closed its netlink socket:

    BUG: unable to handle page fault for address: ffff9e81e2382000
    #PF: supervisor write access in kernel mode
    Oops: Oops: 0002 [#1] SMP NOPTI
    RIP: 0010:netdev_nl_sock_priv_destroy+0x40/0xd0
    Call Trace: genl_release <- netlink_release <- __sock_release <- sock_close <- __fput <- close
    Modules linked in: rpcrdma rdma_ucm ... [last unloaded: ionic(O)]

The faulting instruction is `incl %gs:(%rax)`, i.e. `netdev_hold(binding->dev)` on the freed
net_device (`net/core/netdev-genl.c:1432-1442` in this kernel: `dev = binding->dev; ...
netdev_hold(dev, ...)`). This is the stale `binding->dev` the core notes predicted: nothing in
netdev unregistration clears a TX binding at v7.2.15 or v7.4-rc3, and net-next has since added
that. The process was killed, its binding and dmabuf leaked, and the guest was tainted; stack 1
was restarted afterwards (`E/99-stack-restart-and-module-reload.txt`).

### 4.4 M10: sustained send

- 183 MiB/s for 360 s. 1,106,487 unreadable skbs at `ionic_start_xmit` (1,106,482 GSO, up to
  17 frags), 17,994,803 frag mappings, all in the dmabuf's 4096 pages; 1,106,449 header maps
  and no other DMA map call; 23 readable skbs (2970 bytes) in the whole run.
- Counters consistent: unreadable payload at the driver = bytes verified + 2,621,058
  retransmitted bytes; sender `TcpRetransSegs` +202 (all fast retransmits) = receiver
  `hw_rx_dropped` +202 = emulator 2 `rx_queue_empty` +202. That is the receiver Rx-ring overrun
  the offload validation reported (its F2), not a TX fault; TCP repaired it and the data verified.
- `tx_unreadable_drop` +0, `tx_N_linearize` +0, netdev `tx_dropped` +0, emulator
  `tx_dma_error` / `tx_desc_data_error` / `tx_desc_fetch_error` 0, DMA-API "other" 0, no kernel
  log line in either guest, binding bound/unbound/freed 1/1/1, 0 dma-buf objects afterwards.

## 5. Findings

## Bugs found by testing

**No product bug was found, so nothing was fixed and nothing was regenerated.** The series,
the backport and the emulator are byte-for-byte what this run started with. Findings that are not
defects of the series:

- **F1 (driver, pre-existing, not from the series; same as test-rx.md P2).** ionic sets no DMA
  max segment size, so with `DMA_API_DEBUG` a bind of a udmabuf whose pages happen to be
  physically contiguous beyond 64 KiB logs "DMA-API: ionic 0000:01:00.0: mapping sg segment longer
  than device claims to support [len=131072] [max=65536]" with a WARN splat (once per boot by
  default, one count per long segment: 0 to 118 per bind here). The mapping is made by the
  exporter for the core; the driver only ever uses page-sized pieces, so nothing is wrong on the
  wire. mlx5 calls `dma_set_max_seg_size()`; bnxt, fbnic and gve do not. Not changed: the RX
  track already put the decision to the owner. It is the only splat a debug kernel prints for
  devmem TX on this driver, so a one-line `dma_set_max_seg_size(dev, UINT_MAX)` patch ahead of
  the series would make debug runs silent.
- **F2 (by design, wider than it reads).** The 15-frag limit is reached by small sends as well
  as by small iovecs (section 4.1), and the stall is silent apart from `tx_unreadable_drop`. The
  commit message's "pieces under about MSS/16" is accurate; a reader may not realise that
  separate `sendmsg` calls are such pieces. Worth one sentence in the cover letter and in
  `ionic.rst`.
- **F3 (core).** Completion notifications report success for data the driver or the core
  dropped (4.1), so devmem TX gives the sender no error path for frag-limit or feature-off drops.
- **F4 (core).** After the route is lost (link down), `inet_sk_rebuild_header()` zeroes
  `sk_route_caps` (`net/ipv4/af_inet.c:1339`); until TCP routes a packet again,
  `tcp_sendmsg_locked()` sees no `NETIF_F_SG`, clears `uarg->zerocopy` and returns **EINVAL** for
  a devmem send (`net/ipv4/tcp.c:1153-1175`). Because the uarg is shared with earlier sends,
  their completions then carry `SO_EE_CODE_ZEROCOPY_COPIED` although nothing was copied (2-3
  sends per run). Retrying the send works within the same second. Not driver-specific.
- **F5 (core).** In-flight devmem skbs keep the binding's DMA mapping alive past driver unbind:
  unloading ionic while an orphaned connection still holds unacked devmem data logs "DMA-API:
  pci 0000:01:00.0: device driver has pending DMA allocations while released from device
  [count=1389] ... mapped as scatter-gather", mapped at `dma_buf_map_attachment`
  (`E/M9-dmesg-guest1-module-swap-with-orphan-bindings.txt`). The entries are the core's dma-buf
  attachment, released when the skbs are freed. Same as test-rx.md C1, here without any live
  binding or process.
- **F8 (core, serious; reproduced).** Unregistering the netdev (module unload here; PCI unbind
  and hot-unplug reach the same code but were not run) while a process holds a TX binding makes the kernel oops when
  that process closes its netlink socket or exits (4.3 b). Core code, both v7.2.15 and
  v7.4-rc3 by source; no driver can avoid it. It applies to every netmem TX driver on these
  kernels. For the ERNIC bundle kernel it means: do not unload or unbind ionic while a devmem TX
  binder is alive, or backport net-next's `net_devmem_dev_unregister()` handling of TX bindings
  into the bundle's dmabuf patch set.
- **F9 (host/testbed, affects whoever uses stack 1 next).** Since stack 2's l2 docker network was
  recreated at 22:52:21 EST on 192.168.96.0/20, the host masquerades stack 1's guest-to-guest
  192.168.100.x frames (bridge netfilter is on; Docker's MASQUERADE rule for that /20 matches
  them): guest 2 sees source 192.168.128.1 and nothing gets an answer
  (`E/99-host-masquerade-of-guest-frames-after-stack2-restart.txt`). Everything in the matrix
  ran before that, except the rerun of M9 unload (a), its control and the final sanity run, which
  use extra addresses 10.213.100.11/.12 on `rocm-ernic0` (`ALT=1` in the harness; not
  persistent). Fixing it needs stack 2's l2 network moved off 192.168.96.0/20 (pin a subnet
  outside 192.168.96.0/20 in both stacks' overrides) or root on the host; I touched neither.
  Whichever stack's l2 network holds 192.168.96.0/20 breaks the other one the same way.
- **F6 (harness, explained).** One case reported a frag page outside its dmabuf. Cause: the
  previous case's orphaned connection, stalled by `tx off`, retransmitted from its own dmabuf
  once the features were restored. Reproduced and proved by keying the probe by destination port
  (`E/M7-orphan-retransmit-explains-foreign-pfn.txt`: the one foreign page belongs to port 5456
  and is in that case's dmabuf). The PFN check is per port since then.
- **F7 (testbed).** A binary rebuilt on the host and run in a guest straight from the 9p share
  crashed there once (general protection fault in the receiver,
  `E/M9-link-down-up.receiver-crashed-stale-9p-binary.txt`): stale page cache. The harness now
  installs the program into the guests. Also: my first M6 "exact limit" case let iovecs cross
  pages and so exceeded the limit; corrected and rerun (note in `E/M6-positive.txt`).

Surprising, not a defect:

- Tiny frags throttle themselves: each devmem frag is charged `PAGE_SIZE` of truesize, so
  128-byte iovecs with TSO off ran at 7 MiB/s against 180-200 MiB/s for page-sized ones (M6i).
- `tx_tso_bytes` rose by 3008 for 2200 TSO skbs carrying 128 MiB (`E/M4-tso-on.txt`): it does
  not count payload. Not looked into; the series does not touch that counter.
- In the receiver guest's capture most data frames show a "bad" TCP checksum: they are GRO
  super-frames (up to 65 KB). On emulator 1's tap0 every frame's checksum is good.
- With `gso off` and `sg off` the kernel still accepts `sendmsg` with a dmabuf (TCP sockets always
  get `NETIF_F_GSO`, hence SG, in their route caps); only the transmit path stops it.

About RX, in passing:

- The receiver Rx-ring overrun reported by the offload validation (F2 there) also happens with
  devmem TX as the source, at the default ring of 1024: 202 frames in the 64 GiB sustained run
  (`hw_rx_dropped` = emulator `rx_queue_empty` = sender fast retransmits), none in the short
  cases (<= 460 MB). Retransmitted devmem skbs verified like the rest.
- The receiver took every stream through the ordinary (page) RX path with GRO; nothing on RX
  needed the patched module, and no receive-side counter or log line moved unexpectedly.

## 6. State left

Stack 1 is up (restarted after the oops of 4.3 b): both guests on `7.2.15-devmem` with the
patched `ernic-dmtx1` module loaded (`ionic.ko` + `ionic_rdma.ko`; a reboot returns to the
in-tree ionic, `T/scripts/ionic-mod.sh load ernic-dmtx1` reloads it), server `tx-offloads-1`,
MTU 1500, rings 1024, offloads on, `tcp_retries2` 15. Both guests have the extra address
10.213.100.1N/24 on `rocm-ernic0` (lost on reboot or module reload) because of F9; over it a
final devmem run passed (`E/99-final-sanity.txt`: 60 MB verified, PFN check, no drops), while
192.168.100.x between the guests does not work until F9 is dealt with. Guest 1 still had one
orphaned connection from the M9 (a) rerun holding a 16 MiB dmabuf when I left; it goes away when
TCP gives up. The comparison module `ernic-dmtx1-nobackstop` is built in guest 1's
`~/ionic-mod/` but not loaded. No capture container is left.

## 7. Not run / limits of this run

- No KASAN, lockdep or kmemleak on this kernel: "no use-after-free, no leak" here means no
  crash, no log line, DMA-API debug quiet, binding/dmabuf object counts back to zero.
- `devmem.py` (cannot run on ionic, see selftest-ncdevmem.md); IPv6 devmem sends (the program
  supports them; all cases used IPv4-mapped addresses); VLAN (kernel has no 8021q); XDP_REDIRECT
  into a ring also carrying devmem skbs; firmware reset; hwstamp queue.
- The bridge between the two emulators was not captured separately; tap0 in emulator 1's
  namespace is its only port besides the docker interface.
- `tcp_retries2=15` expiry of a stalled connection (about 15 minutes) was not waited for.
- The docs in rocm-ernic that say the series is "built, not run" (CHANGELOG, the ansible
  fragment, `ansible/files/patches/devmem-tx/README.md`) were not edited.
