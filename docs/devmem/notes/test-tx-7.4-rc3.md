# devmem TCP TX on emulated ionic: test matrix on 7.4.0-rc3 (mainline series, KASAN + lockdep)

Run 2026-11-18/19 (evidence files are stamped in UTC, 2026-11-19 04:07-05:25) on stack 1
(`ionic-devmem`; `devmem-1` = sender, `devmem-2` = receiver). `T` = `/var/tmp/ionic-devmem/test`,
`E` = `T/evidence/tx-7.4-rc3`.

**Result: every matrix row passed for the devmem TX series and for the emulator; neither was
changed.** Two things fail, and neither is in the series:

- **B1, a pre-existing mainline ionic bug found by the XDP row:** multi-buffer XDP (MTU above one
  buffer, `xdp.frags` program) gives the program, and XDP_TX / XDP_REDIRECT send, the wrong bytes
  for every second frame. Root-caused, reproduced on the unpatched in-tree driver, fixed in a
  stand-alone patch and re-tested. **The fix is not folded into the series or the backport**
  (see "Bugs found by testing"): that is a decision for Stephen.
- **Module unload with a live TX binding** ends in a KASAN use-after-free and an oops in the
  networking core, as predicted (section 4.3). Core, not driver.

No KASAN, lockdep, sleeping-in-atomic, refcount or DMA-API unmap report came from any devmem row
before that last case.

## 1. Configuration under test

| Item | Value |
| --- | --- |
| Guest kernel | `7.4.0-rc3-devmem-kasan` (v7.4-rc3 unmodified): KASAN generic inline, PROVE_LOCKING, PROVE_RCU, DEBUG_ATOMIC_SLEEP, DEBUG_LIST, DMA_API_DEBUG, DEBUG_NET. Not configured: kmemleak, KFENCE, DEBUG_OBJECTS, NET_DEV_REFCNT_TRACKER, UBSAN, VLAN_8021Q, XDP_SOCKETS (`E/00-environment.txt`) |
| ionic | `T/ionic-mod/dmtx74-1`: the kernel's own in-tree ionic + `/var/tmp/ionic-devmem/patches/mainline/0001..0006` (sha256 in `E/00-environment.txt`; staged tree `diff -rq`-identical to `/var/tmp/ionic-devmem/linux`). `ionic.ko` only (no ionic_rdma on this kernel), built in each guest, loaded in both (`E/01-module-build-load.txt`) |
| Emulator | `T/ernic/tx-offloads-1/build/rocm-ernic`, sha256 `29413ea9…7ef118`, the binary the 7.2.15 run used; not rebuilt |
| Offloads seen | `tx-checksumming on`, `scatter-gather on`, `tcp-segmentation-offload on`, GSO on; 4 queues; rings 1024 |
| Attach flavour | **dynamic attach + pin**: at bind, `net_devmem_bind_dmabuf` calls `dma_buf_dynamic_attach`, `dma_buf_pin`, `dma_buf_map_attachment`, `dma_resv_wait_timeout` (one call each, traced with bpftrace); at free, a kworker runs `__net_devmem_dmabuf_binding_free` -> `dma_buf_unmap_attachment`, `dma_buf_unpin`, `dma_buf_detach` (`E/M2-ncdevmem-bind-patched.txt`; source `T/kbuild/7.4-rc3/src/net/core/devmem.c:55,238,247,253,261`). `dma_buf_attach` (static) is not called. udmabuf has no `.pin`, so the pin is a no-op that succeeds |
| IOMMU in the guest | **none**: `/sys/kernel/iommu_groups` and `/sys/class/iommu` empty, no DMAR/AMD-Vi line (only "iommu: Default domain type: Translated" and "PCI-DMA: Using software bounce buffering for IO (SWIOTLB)", not forced). DMA is direct, which the PFN check relies on and confirms |
| Exporter | udmabuf (memfd), 4 or 16 MiB |
| Addresses | 10.213.100.11/.12 on `rocm-ernic0` (`ALT=1`): 192.168.100.x between the guests is still broken by the host (the 7.2.15 run's F9; still so at the end of this run) |

## 2. What was reused, changed and added

Reused unchanged: `T/bin/devmem_tx` (sha256 `1579ebae…e9aa1e`), `T/bin/pcapcap`, the server binary,
and the harness `T/scripts/devmem-tx/` with the same case parameters as on 7.2.15.

Changed (all backwards compatible):

- `T/scripts/ionic-mod.sh`: an `intree-*` stage now also stages `ionic.rst`. The mainline series
  patches the driver documentation (patch 4), and the first attempt failed to apply for that reason
  (`T/ionic-mod/dmtx74-1.failed-stage-no-doc-file` is that attempt, kept).
- `lib.sh`: the splat count also matches lockdep's and `might_sleep`'s messages and ignores two
  boot lines; the counter diff shows XDP counters.
- `m9-rmmod.sh`: works without `ionic_rdma.ko`.

Added:

- `T/src/xdp/xdp_echo.c` -> `T/bin/xdp/xdp_{pass,tx,redirect,pass_frags,tx_frags,redirect_frags}.o`
  (clang 18, `-target bpf`, no libbpf headers): echoes UDP port 7777 with XDP_TX or
  `bpf_redirect()` to the ingress device (ndo_xdp_xmit), everything else XDP_PASS.
- `T/src/xdp/udp_echo_check.py`: guest 2 sends datagrams and checks every echo.
- `T/scripts/devmem-tx/m-xdp.sh` (`basic`, `frags`, `toggle`), `m-xdp-diag.sh`, `udpsum.py`
  (checks request and echo payloads in a capture), `dmesg-lines.py` (every kernel log line recorded
  after a case's marker).
- `T/series/mainline-no-backstop/` (the series without 0005) for the M7 control module.

How a silent copy fallback is excluded is unchanged from the 7.2.15 notes (section 3 there): per
case, bpftrace on the sender counts unreadable skbs and bytes at `ionic_start_xmit`, checks bit 0
of `frags[0].netmem`, records every frag DMA address the driver takes and compares its page with
the dmabuf's own PFNs, counts DMA map calls (one per skb, the header), and the harness reads
completions, `tx_unreadable_drop`, linearize and drop counters, the emulator's error counters, the
DMA-API error count and the kernel log of both guests since a marker.

## 3. Matrix

"clean" = no kernel log line in either guest since the case's marker other than those quoted
(checked with `dmesg-lines.py`), DMA-API reports other than F1 = 0, emulator `tx_dma_error` /
`tx_desc_data_error` / `tx_desc_fetch_error` = 0, dma-buf objects back to 0.

| Row | What was run | Result | Evidence |
| --- | --- | --- | --- |
| M1 | `ncdevmem -f rocm-ernic0 -s .12 -p 5302 -t 0 -q 1` on the unpatched in-tree ionic with the new emulator (SG, csum, TSO on): EOPNOTSUPP, extack "Driver does not support netmem TX", nothing received | PASS | `E/M1-baseline-intree-unpatched.txt` |
| M2 | Same on the patched module, 405 KB file: bind succeeds, every send completed, receiver sha256 equal; attach flavour traced | PASS | `E/M2-ncdevmem-bind-patched.txt`; first bind logs F1: `E/M2-dmesg-guest1-first-bind.txt` |
| M3 | TSO off and TSO+GSO off, csum on, MTU 1500: 128 MiB aligned and 57 MiB unaligned; 92,808 / 41,859 unreadable non-GSO skbs, CHECKSUM_PARTIAL | PASS, 0 mismatches, clean | `E/M3-tso-off.txt` |
| M4 | Same streams, TSO on: 2193 / 976 unreadable GSO skbs, up to 17 frags; linearize +0, `tx_unreadable_drop` +0, netdev `tx_dropped` +0, 0 retransmissions | PASS, clean | `E/M4-tso-on.txt` |
| M5 | MTU 9000 (TSO on and off) and 9198 (TSO on), both streams | PASS (6 cases), clean. Jumbo works to 9198, the ionic maximum | `E/M5-mtu.txt` |
| M6 positive | The nine cases of the 7.2.15 run (1-byte sends at odd offsets; 2 bytes across a page boundary; odd length and offset; 50 iovecs with gaps; last 4095 bytes of the dmabuf; 480 x 128 B iovecs, TSO on and off; **15 frags of 64 B, non-GSO, exactly the limit**; 1024 iovecs of 1400 B) | PASS (9), 0 mismatches, 0 drops, clean | `E/M6-positive.txt` |
| M6 negative | 16 frags of 64 B (one over); 1024 iovecs of 16 B (17 frags), with SO_SNDTIMEO and blocking with `tcp_retries2=5`; 300 one-byte sends with Nagle. Each over-limit skb is dropped and counted (`tx_unreadable_drop` +7..+10 = transmission attempts), linearize +0, netdev `tx_dropped` +0, receiver gets 0 bytes (1 byte in the Nagle case, verified), no log line, binding and dmabuf freed when the connection dies (72 s default, 1 s with `tcp_retries2=5`); a positive run straight after passes | PASS (clean counted drop) | `E/M6-negative.txt`, `E/M6-negative-small-sends-nagle.txt` |
| M6 threshold | 512 iovecs of 80 / 90 / 100 / 110 B per send, MTU 1500: 80 and 90 deliver nothing, 100 stalls after 3400 verified bytes, 110 passes | as designed, same as 7.2.15 | `E/M6-edge-frag-sizes.txt` |
| M7 | `tx off`, `sg off`, `tx off sg off`, each also with `gso off`; before connect (100 KB and 1 KB sends) and 3 s into a stream; 15 cases with captures on emulator 1's tap0 and in the receiver guest. **No payload frame left the emulator in any before-connect case; during-stream cases: a verified prefix (14.2-14.4 MB, 0 mismatches) then nothing; no all-zero payload and no bad checksum on tap0** | PASS | `E/M7-tx.txt`, `E/M7-sg.txt`, `E/M7-both.txt`, `E/M7-gso.txt`, `E/pcap/M7-*.pcap` |
| M7 control | Sender module without patch 0005, `tx off sg off`: **397,066 zero bytes reach the receiver**; 528 all-zero payload frames with valid TCP checksums on tap0 | 0005 is needed on the v7.4-rc3 core | `E/M7-nobackstop-variant.txt` |
| M8a | Bound on rocm-ernic0, destination routed via enp0s5: first `sendmsg` returns ENODEV, nothing queued, no payload frame on enp0s5 | PASS | `E/M8-other-netdev.txt`, `E/pcap/M8a.sender-enp0s5.pcap` |
| M8b | Route moved to enp0s5 3 s into a stream: next `sendmsg` ENODEV; 9 queued unreadable skbs freed in `validate_xmit_skb` (enp0s5 `tx_dropped` +9); enp0s5 capture has 7 frames for the port, none with payload; receiver verified the 12.5 MB sent before | PASS | same, `E/pcap/M8b*.pcap` |
| M9 close socket / unbind / free dmabuf / exit | 64 x 1 MiB with sends uncompleted at teardown: receiver verified all 64 MiB each time; binding bound/unbound/freed 1/1/1 | PASS, clean | `E/M9-xact.txt` |
| M9 link down/up | 2 s down, 3 s into a 15 s stream, sender retrying refused sends: 226 MB verified, all 2262 completions, none COPIED, 116 retransmissions | PASS, clean | `E/M9-linkdown.txt` |
| M9 ring / channel / MTU | Nine changes during a 25 s stream: 446 MB verified, all 4456 completions, 736 retransmissions; only the driver's "Changing ..." lines | PASS, clean | `E/M9-reconfig.txt` |
| XDP (extra row) | Sender on ONE queue so XDP frames and devmem skbs share queue 0's descriptor-info slots. XDP_PASS, XDP_TX, XDP_REDIRECT at MTU 1500 with a 15 s devmem stream: 285-287 MB verified each, 10,000 of 10,000 echoes verified for TX and REDIRECT. Attach/detach of the three programs during a 25 s stream: 456 MB verified. Multi-buffer (MTU 9000, 8000-byte datagrams): **devmem data verified, but half the echoes are corrupt: bug B1, not from the series**; with B1's fix, 10,000 of 10,000 for XDP_TX and XDP_REDIRECT | PASS for the series; B1 for mainline ionic | `E/XDP-basic-per-datagram-pattern.txt`, `E/XDP-attach-detach-mid-stream.txt`, `E/XDP-multibuf-mtu9000.txt`, `E/XDP-multibuf-mtu9000-series-plus-xdp-fix.txt`, `E/XDP-diag-*.txt` |
| IPv6 (extra row) | 128 MiB and 57 MiB over `fd00:100::/64`, TSO on; 57 MiB TSO off | PASS (3), clean | `E/EXTRA-ipv6.txt` |
| M10 | 360 s, 1000003 bytes per `sendmsg` in 4 iovecs with gaps, all 4096 pages of a 16 MiB dmabuf, TSO on: **66,522,199,566 bytes (62.0 GiB) verified, 0 mismatches; 66,522 sends, 66,522 completions, 0 COPIED, 0 gaps**, 176 MiB/s | PASS, clean | `E/M10-sustained.txt` |
| M9 module unload, unbound with unacked data | Receiver stopped, 625 KB queued, netlink socket closed, sender exited, `rmmod ionic` with the orphan holding 540 KB of devmem skbs: unload completes, no KASAN or lockdep report; one DMA-API warning from the core's mapping (F5) | PASS for the driver; core F5 | `E/M9-rmmod-a-unbound-with-unacked-data.txt` |
| M9 module unload, live binding (run last) | `rmmod` 4 s into a stream: unload completes; receiver has verified all 908 completed sends (90.8 MB); when the sender closes its netlink socket: **KASAN use-after-free, then an oops, in `netdev_nl_sock_priv_destroy()`** | **FAIL in the core** (not the driver), section 4.3 | `E/M9-rmmod-b-live-binding.txt`, `E/M9-rmmod-b-dmesg-guest1-full.txt` |

### 3.1 What userspace observes at the frag limit (M6 negative)

Same as on 7.2.15: `sendmsg` succeeds until the send buffer is full (544 of 16384 bytes with
16-byte iovecs), then blocks; with `SO_SNDTIMEO` it returns a short count or EAGAIN / ETIMEDOUT;
`SO_ERROR` stays 0 until TCP gives up (ETIMEDOUT after 13 s with `tcp_retries2=5`); a success
completion is still delivered for dropped data (`completed=1` with nothing received; Nagle case:
35 sends completed, 1 byte delivered). The drop shows only in `tx_unreadable_drop`.

One difference, which is timing and not behaviour: 300 back-to-back one-byte sends with
`TCP_NODELAY` stalled on 7.2.15 but **passed here** (77 skbs, at most 13 frags: the slower kernel
gets each small skb out before 16 pieces pile up). Without `TCP_NODELAY` the same sends stall after
1 delivered byte (`E/M6-negative-small-sends-nagle.txt`). Whether small sends hit the limit depends
on how fast the queue drains, so a sender cannot rely on either outcome.

### 3.2 M7: which mechanism stopped each case

Identical to 7.2.15 (stacks from `tracepoint:skb:kfree_skb` on unreadable skbs, counts from the probe):

| Features off | skb | Stopped by | Seen |
| --- | --- | --- | --- |
| tx | GSO or sub-MSS | core `validate_xmit_skb` (`skb_segment` / `skb_checksum_help` fail) | netdev `tx_dropped` +9..+10; 0 unreadable skbs at `ionic_start_xmit` |
| sg | GSO or sub-MSS | core `validate_xmit_skb` (`skb_segment` copy / linearize fail) | `tx_dropped` +9..+11; 0 at the driver |
| tx + sg | GSO | **driver backstop (patch 0005)**: `skb_copy_and_csum_bits()` ran on the unreadable skb 10 times, 10 unreadable skbs with no frags reached `ionic_start_xmit`, `tx_unreadable_drop` +10 (during-stream: 115 = 115 = 115) | plus `tx_dropped` +7..+8 for single-MSS retransmissions dropped in the core |
| tx + sg | sub-MSS | core `validate_xmit_skb` | `tx_dropped` +9 |
| any + gso | both | same as without `gso off` | `E/M7-gso.txt` |

The driver's probe-time gate is not what stops any of these (bind and `sendmsg` succeed in all).

### 3.3 M10 counters

- 1,064,689 unreadable skbs at `ionic_start_xmit` (all but 2 GSO, up to 17 frags), 17,311,287 frag
  addresses, all in the dmabuf's 4096 pages; 1,064,372 header maps; 25 readable skbs (2700 bytes).
- `tx_tso` +1,064,370 and the map count are 317-319 below the skb count at `ionic_start_xmit`, and
  the unreadable bytes seen there exceed the verified bytes by 20,013,238 (319 x 62.7 KB). Sender
  `TcpRetransSegs` did not change, so these are not retransmissions; they match `ionic_start_xmit`
  returning `NETDEV_TX_BUSY` before mapping (ring full) and the qdisc offering the same skb again.
  I did not count the BUSY returns directly, so this is an inference from the three numbers.
- No Rx-ring overrun on the receiver this time (`hw_rx_dropped` +0; the sender is slower than on 7.2.15).
- `tx_unreadable_drop` +0, linearize +0, `tx_dropped` +0, emulator error counters 0, DMA-API
  delta 0, no kernel log line in either guest, binding 1/1/1, 0 dma-buf objects afterwards.

## 4. Kernel reports

Whole-run scan of every saved kernel log (`E/dmesg-*.txt`, the per-case sections, and the logs
taken before each module reload, which clears the log): **no KASAN, lockdep ("possible circular
locking", "inconsistent lock state"), "BUG: sleeping function", suspicious-RCU, refcount or list
debug report before the last case.** Receiver guest: none at all. What was printed:

### 4.1 F1: DMA-API "sg segment longer than device claims" (driver, pre-existing, not the series)

Once per boot, at the first bind (later binds only raise `error_count`; the harness subtracts
them). Full text: `E/M2-dmesg-guest1-first-bind.txt`.

    DMA-API: ionic 0000:01:00.0: mapping sg segment longer than device claims to support [len=131072] [max=65536]
    WARNING: kernel/dma/debug.c:1234 at debug_dma_map_sg+0x7a6/0xcb0, CPU#0: ncdevmem/2201
     get_sg_table.isra.0 <- dma_buf_map_attachment <- net_devmem_bind_dmabuf <- netdev_nl_bind_tx_doit

ionic sets no DMA max segment size; the mapping is the exporter's, for the core. Same as on 7.2.15.

### 4.2 F5: DMA-API "pending DMA allocations while released from device" (core)

Three times, each at an `rmmod ionic` while a connection still held unacked devmem skbs or a
binding (module swap after the no-backstop control with its orphan alive, count=772; unload case a,
count=1738; unload case b, count=2367). Full text of one: `E/M9-rmmod-a-unbound-with-unacked-data.txt`.

    DMA-API: pci 0000:01:00.0: device driver has pending DMA allocations while released from device [count=1738]
    One of leaked entries details: [device address=0x0000000187f92000] [size=4096 bytes] [mapped with DMA_TO_DEVICE] [mapped as scatter-gather]
    WARNING: kernel/dma/debug.c:892 at dma_debug_device_change+0x25f/0x4b0, CPU#1: rmmod/64262
    DMA-API: Mapped at: debug_dma_map_sg <- __dma_map_sg_attrs <- dma_map_sgtable <- get_sg_table <- dma_buf_map_attachment

The entries are the binding's dma-buf mapping, made by the core and released when the last skb
goes (dma-buf objects reached 0 after 315 s in case a). Not a driver mapping.

In case a the receiver got ECONNRESET after the reload, as on 7.2.15; this time the receiver-guest
capture shows the RST from the sender (`10.213.100.11.53710 > 10.213.100.12.5480: Flags [R]`),
which the 7.2.15 run could not find.

### 4.3 Module unload with a live TX binding: KASAN use-after-free + oops (core)

`rmmod ionic` returned 0 with the sender still holding its binding and sending. The sender's next
`sendmsg` returned short, `SO_ERROR` = ENETUNREACH; the receiver verified all 908 completed sends.
When the sender closed its netlink socket (13 s later):

    BUG: KASAN: use-after-free in netdev_nl_sock_priv_destroy+0x1b1/0x1e0
    Read of size 8 at addr ffff8881362386c0 by task devmem_tx/69808
    CPU: 5 UID: 0 PID: 69808 Comm: devmem_tx Tainted: G        W  O        7.4.0-rc3-devmem-kasan #1 PREEMPT(lazy)
    Call Trace:
     kasan_report+0xe4/0x1a0
     netdev_nl_sock_priv_destroy+0x1b1/0x1e0
     genl_release+0xf5/0x190
     netlink_release+0xcd1/0x18e0
     __sock_release+0xb0/0x270
     sock_close+0x18/0x20
     __fput+0x365/0xa90
     fput_close_sync+0xe5/0x1a0
     __x64_sys_close+0x7d/0xd0
    The buggy address belongs to the physical page:
    page: refcount:0 mapcount:0 mapping:0000000000000000 index:0xffff88813623a200 pfn:0x136238
    Memory state around the buggy address:
    >ffff888136238680: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
    BUG: unable to handle page fault for address: ffff88829fd34000
    #PF: supervisor write access in kernel mode
    Oops: Oops: 0002 [#1] SMP KASAN NOPTI
    RIP: 0010:netdev_nl_sock_priv_destroy+0xbd/0x1e0
    Modules linked in: ib_core efivarfs [last unloaded: ionic(O)]
    note: devmem_tx[69808] exited with irqs disabled

Complete text: `E/M9-rmmod-b-dmesg-guest1-full.txt` (from `[ 4246.377818]`).

**Classification: core.** The read is `dev = binding->dev` followed by `netdev_hold(dev)` at
`net/core/netdev-genl.c:1480-1489` on the freed `net_device` (the page is back in the page
allocator, so KASAN has no alloc/free stacks to print). ionic is not loaded at that point. Nothing
in netdev unregistration clears a TX binding at v7.4-rc3; net-next has since added that. The
process was killed, its binding and 16 MiB dmabuf leaked, and the guest was tainted; the stack was
restarted afterwards.

## Bugs found by testing

### B1. ionic XDP multi-buffer: frags ignore the Rx buffer's page offset (mainline, pre-existing)

**Not a bug of the devmem TX series, the backport or the emulator.** It is in mainline ionic's Rx
XDP path at v7.4-rc3, and the same line is in v7.2.6.

- **Symptom.** With an `xdp.frags` program at MTU 9000, guest 1 echoing 8000-byte UDP datagrams
  with XDP_TX or XDP_REDIRECT: of 10,000 echoes captured on the emulator's tap0, **5000 have their
  last 426 bytes wrong** (payload offsets 7574-7999, i.e. the frame from byte 7616 on); the
  requests arriving are all intact; the receiver drops the bad echoes on the UDP checksum
  (`UdpInCsumErrors`). In 4999 of the 5000 the wrong bytes are another datagram's bytes at the same
  offsets. My first runs used the same payload in every datagram, which hid all but 0.3-5 % of
  them; in those that showed, the bytes were stale kernel memory (kernel pointers, slab strings):
  **an information leak onto the wire**, not only corruption
  (`E/XDP-multibuf-mtu9000.txt`, `E/XDP-multibuf-echo-loss-analysis.txt`, `E/XDP-diag-1.txt`,
  `E/XDP-diag-2.txt`).
- **Not from the series.** Identical on the kernel's unpatched in-tree ionic: 5000 of 10,000 for
  XDP_TX and for XDP_REDIRECT (`E/XDP-diag-3-unpatched-intree.txt`).
- **Root cause.** `ionic_run_xdp()` builds each frag with
  `skb_frag_fill_page_desc(frag, bi->page, 0, frag_len)`: offset 0, where the buffer starts at
  `bi->page_offset`. Rx buffers come from `page_pool_alloc()`, which hands out page fragments for
  sizes up to half a page. At MTU 9000 a frame's buffers are 3520 + 4096 + 1402 bytes, so two
  1402-byte buffers share a page and every second one starts at offset 1402. The device descriptor
  has the right address (`ionic_rx_buf_pa()` adds `page_offset`); the XDP frag points at the start
  of the page, i.e. at the neighbouring buffer (another frame's tail, or whatever the page held
  before if that buffer is not filled yet). The CPU sync beside it also uses the first buffer's
  offset. The skb path uses `page_offset` and is not affected (M5's jumbo streams verified).
  The offset-0 line dates from 5377805dc1c0 ("ionic: implement xdp frags support"); it became wrong
  with ac8813c0ab7d ("ionic: convert Rx queue buffers to use page_pool", v6.12).
- **Fix.** Use `bi->page_offset` for the frag and for the sync (3 lines). Stand-alone patch:
  `/var/tmp/ionic-devmem/patches/xdp-frag-offset/net/0001-ionic-use-the-rx-buffers-page-offset-for-xdp-frags.patch`
  (`[PATCH net]`, Fixes: ac8813c0ab7d, no Signed-off-by; generated by `mkpatch.sh` there from
  `stages/s0` -> `s1`). `git apply --check` passes on pristine v7.4-rc3, on v7.4-rc3 + the series,
  on v7.2.6 and on v7.2.6 + srq + p4 + the backport. checkpatch: only the missing Signed-off-by
  (and "unknown commit id", because it ran outside the git tree).
- **Re-test.** Module `T/ionic-mod/dmtx74-1-xdpfix` (series + fix) on the sender: 10,000 of 10,000
  echoes intact on both taps for XDP_TX and XDP_REDIRECT (`E/XDP-diag-5-series-plus-xdp-fix.txt`),
  and with a devmem stream on the same queue: 10,000 of 10,000 each, 285-288 MB of devmem data
  verified, no log line (`E/XDP-multibuf-mtu9000-series-plus-xdp-fix.txt`).
- **What I did not do, on purpose.** The fix is **not** in `patches/mainline/`, not in the linux
  tree, not in `patches/ernic-v7.2.6/` and not in rocm-ernic's `ansible/files/patches/devmem-tx/`:
  it is unrelated to netmem TX, belongs to `net` rather than net-next, and adding it to the DKMS
  patch set changes what ERNIC guests run. Stephen decides whether to send it and whether the
  bundle carries it. Everything else in this matrix ran on the series without it.
- Not checked: whether the bug shows on real hardware (it should: nothing in it depends on the
  emulator), and the first buffer (always a full page here, offset 0).

No other product bug was found. The series, the backport and the emulator are byte-for-byte what
this run started with.

## 5. Findings other than bugs

- **F1, F5, and the unload use-after-free** (7.2.15's F8): reproduced on this kernel, sections 4.1-4.3.
- **F2 / F3 (frag limit is silent; completions report success for dropped data):** reproduced
  (3.1). New: whether back-to-back small sends reach the limit is timing-dependent.
- **F4 (EINVAL and COPIED flags after a link flap):** not seen here: the link-flap run had no
  COPIED completion and the retry option was enough. I did not look for the cause of the difference.
- **F9 (host masquerades 192.168.100.x):** still present; `ionic-devmem-rx_l2` is on
  192.168.96.0/20. All devmem traffic used 10.213.100.1N (IPv6 for the extra row).
- **Throughput under KASAN** (not a stall): TSO on 212-221 MiB/s at MTU 1500 and 315-514 at
  9000; TSO off 38-45 MiB/s at 1500 (per-skb cost under KASAN); sustained 176 MiB/s.
- **Test design note:** identical payloads in every datagram masked B1 almost completely. The echo
  checker now varies the payload per datagram.

## 6. State left

Stack 1 is up, restarted after the oops: both guests on `7.4.0-rc3-devmem-kasan` (GRUB default)
with `dmtx74-1` (in-tree ionic + the mainline series, without the B1 fix) loaded, server
`tx-offloads-1`, MTU 1500, 4 queues, rings 1024, offloads on, no XDP program, `tcp_retries2` 15,
10.213.100.1N/24 added to `rocm-ernic0` (lost on reboot or module reload). A final devmem run
passed (`E/99-final-sanity.txt`: 60 MB verified, PFN check, no drops, no report). A reboot returns
to the unpatched in-tree ionic; `T/scripts/ionic-mod.sh load dmtx74-1` reloads the series. Also
built in guest 1's `~/ionic-mod/`: `dmtx74-1-xdpfix`, `dmtx74-1-nobackstop`. No capture container
is left. `E/pcap/` holds 2.0 GB of captures (the XDP diagnosis ones are 160 MB each).

## 7. Not run / limits

- kmemleak is not configured in this kernel, so "no leak" means: binding bind/unbind/free counts
  equal, dma-buf objects back to 0, DMA-API debug entries returned.
- VLAN (no 8021q in this kernel), firmware reset, PCI unbind / hot-unplug, hwstamp queue,
  `devmem.py`.
- XDP_REDIRECT from another netdev into ionic (the redirect here is ionic to itself, which uses
  the same `ndo_xdp_xmit`); AF_XDP (not configured).
- The XDP map-failure path that patch 0001 fixes was not provoked (needs a DMA mapping error).
- The B1 fix was built and run on 7.4-rc3 only; on v7.2.6 it was only checked to apply.
- A `tcp_retries2=15` stall (about 15 minutes) was not waited out.
- The rocm-ernic docs that say the series is "built, not run" were not edited.
