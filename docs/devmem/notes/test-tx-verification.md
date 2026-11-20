# devmem TCP TX on emulated ionic: independent verification of the two test reports

Run 2026-11-19 05:28-06:31 UTC on stack 1 (`ionic-devmem`; `devmem-1` sender, `devmem-2` receiver).
Verifies `notes/test-tx-7.2.15.md` and `notes/test-tx-7.4-rc3.md`. `T` = `/var/tmp/ionic-devmem/test`,
`V` = `T/evidence/tx-verify` (everything of this pass), `E72` / `E74` = the testers' evidence directories.

**Verdict: no PASS claim was refuted.** Every row I re-ran with my own sender, receiver and probe
gave the result the testers reported, on both kernels. Three things need correcting or adding:

1. **The frag limit is much wider at jumbo MTU than either report says (new, V1).** At MTU 9000
   a devmem stream of 128-byte or 512-byte iovecs delivers nothing and stalls; the same 128-byte
   stream passes at MTU 1500 (it is the testers' M6 positive case (f)). The reports give only the
   MTU 1500 threshold ("110-byte pieces pass, 100-byte stall").
2. **"Exactly 15 frags sends" holds only if no iovec crosses a page (new, V2).** A 15-iovec send
   in which one 64-byte iovec straddles a page boundary is 16 frags and is dropped.
3. **Weak evidence in three places on 7.2.15** (section 2): M2 moved 46 bytes; M3-M5 have no usable
   DMA-API check (the harness line prints FAIL or is absent); the module-identity file records only
   the taint flag. I re-established all three directly; none hides a failure.

The stack is left up on `7.4.0-rc3-devmem-kasan` with `dmtx74-1` loaded (section 7).

## 1. What I used (independent of the testers' tools)

- `V/src/vtx.c` -> `V/bin/vtx` (static): my own devmem TX sender, written from the uapi (raw
  generic netlink `BIND_TX`, udmabuf, `SCM_DEVMEM_DMABUF` + `MSG_ZEROCOPY`). **The dmabuf is filled
  from `getrandom()`**, so no wrong offset can reproduce the stream. Every byte the kernel accepts
  is also written, in order, to an "expected" file. It can mix devmem sends, copying sends and
  user-memory `MSG_ZEROCOPY` sends on one socket, and can pass iovecs outside the dmabuf.
- `V/src/vrx.py`: receiver; SHA-256 of everything received, byte count, and a count of reads
  containing 32 consecutive zero bytes (impossible by chance with random data). Can stop reading
  (zero window), close or reset mid-transfer.
- Verdict per case (`V/src/v.sh`, `vcase`): `sha256(expected[0:N])` on the sender guest must equal
  the receiver's hash of its N bytes, zero runs must be 0. This catches zero-fill, wrong offset,
  reordering and truncation inside the stream; `COMPLETE` additionally requires N = bytes accepted.
- `V/src/vprobe.bt`: my own bpftrace probe on `ionic_start_xmit`: per destination port and Tx
  queue, `skb->unreadable`, bit 0 of **every** frag's `netmem` (the testers checked frag 0), frag
  count, GSO, `ip_summed`; `skb_copy_and_csum_bits` / `skb_checksum_help` / `skb_segment` /
  `skb_copy_bits` called on an unreadable skb; unreadable skbs freed as drops; bind/unbind/free.
- Per case: counters of both guests and both emulators before/after, DMA-API `error_count` with
  the bind-time part separated (as the testers do), kernel log of both guests since a marker.
- Not independent: the PFN-in-dmabuf check and the "one DMA map per skb" count were not redone
  with my own code (section 6).

**Check that my detector is not vacuous:** with the testers' control module (series without patch
0005) on the sender and `tx off sg off` 3 s into a stream, my receiver got all 80,102,403 bytes,
all 801 completions reported success, and the verdict was **DATA-CORRUPT with 1904 zero runs**
(`V/C-control-nobackstop.txt`). So the trap is real, my check sees it, and patch 0005 is what
closes it on this core. (The testers' sender stops early; mine does not, and shows that without
0005 the rest of the stream is zeroes indefinitely.)

## 2. Audit of the testers' evidence

Read: every `M*.txt`, `EXTRA-ipv6.txt`, the XDP files' result lines, `devmem_tx.c` (pattern,
receiver), `lib.sh`, `probe.bt`.

Sound in general: the receiver compares every byte against a 64-bit-mixed position-dependent
pattern (`devmem_tx.c:161-177`, `:281-300`), so zero-fill, offsets and reordering are caught; for
time-based runs (`-n 0`) the receiver cannot see a truncation at a send boundary, but in every such
file the receiver's byte count equals the sender's (`M10`: 69,148,207,444 = 69,148,207,444 and
66,522,199,566 = 66,522,199,566). Unreadable-skb proof, PFN check, counters and marker-delimited
kernel logs are present per case.

| Claim | Evidence | Audit |
| --- | --- | --- |
| M1 both kernels (EOPNOTSUPP + extack on unpatched) | `E72/M1-baseline-*.txt`, `E74/M1-baseline-intree-unpatched.txt` | CONFIRMED (module path, empty taint, extack text, nothing received) |
| M2 on 7.2.15 "ncdevmem data matches" | `E72/M2-ncdevmem-bind-patched.txt` | **WEAK EVIDENCE**: the file sent is 46 bytes (two 23-byte lines); hashes equal. True, but it proves little. 7.4-rc3's M2 sent 405,264 bytes: CONFIRMED |
| M3-M5 on 7.2.15 "clean" incl. DMA-API | `E72/M3-tso-off.txt`, `M4-tso-on.txt`, `M5-mtu.txt` | **WEAK EVIDENCE for the DMA-API part only**: M3 has no `DMA-API-CHECK` line; M4 and M5 print `other=107/34/118 => FAIL` in all 8 cases because the bind-time count was not yet separated (`at bind=0`). Data, PFN and counter evidence is fine. From M6 on the check works and passes; my re-runs on 7.2.15 show `other=0` for the same traffic |
| M3-M5 on 7.4-rc3 | `E74/M3..M5` | CONFIRMED (12 cases, `other=0`) |
| M6 positive / negative / threshold | `E72/M6-*.txt`, `E74/M6-*.txt` | CONFIRMED. Three `PFN-CHECK ... => FAIL` lines in `E72/M6-negative.txt` (and 15 in `E72/M7-*`) are "driver mapped no devmem frag" cases that the older harness labelled FAIL; the later harness prints N/A. Not failures |
| M7 (15 cases) and control | `E72/M7-*.txt`, `E74/M7-*.txt`, pcaps | CONFIRMED: tap0 capture counts payload frames, all-zero payloads and bad checksums per case |
| M8 | `E72/E74 M8-other-netdev.txt` | CONFIRMED |
| M9 close / unbind / free / exit | `E74/M9-xact.txt`, `E72/M9-close-unbind-free-exit.txt` | CONFIRMED, with a caveat: 64 MiB at ~165 MiB/s with the action after the last send means at most one send buffer of data was in flight at teardown |
| M9 link flap, reconfig | `E72/M9-link-down-up.txt`, `M9-reconfig.txt`, `E74/M9-linkdown.txt`, `M9-reconfig.txt` | CONFIRMED |
| M10 | `E72/E74 M10-sustained.txt` | CONFIRMED (bytes equal both sides, 4096 of 4096 dmabuf pages seen, completions = sends, 0 COPIED) |
| M9 rmmod a / b, F5, F8 | `E72/M9-rmmod-*`, `E74/M9-rmmod-*` | CONFIRMED, and reproduced (section 3) |
| F8 "core, not driver" | `T/kbuild/7.4-rc3/src/net/core/netdev-genl.c:1478-1492` | CONFIRMED by source: the socket's binding list keeps `binding->dev` with no reference; nothing clears it for a TX binding at unregister |
| XDP row and B1 | `E74/XDP-*.txt`; `ionic_txrx.c:525` (`skb_frag_fill_page_desc(frag, bi->page, 0, frag_len)`) | CONFIRMED by evidence and source (5000 of 10,000 bad on the unpatched in-tree driver, 0 with the fix). **Not re-run** |
| IPv6 extra | `E74/EXTRA-ipv6.txt` | CONFIRMED by evidence. Not re-run |
| "No KASAN/lockdep report before the last case" | `E74/dmesg-*.txt` | CONFIRMED by evidence, and no report in any of my 60+ cases on that kernel before the unload case |

Note for anyone reading kernel logs here: DMA-API debug prints only while its `num_errors` budget
lasts. After the first report a later DMA-API error raises `error_count` and prints nothing, so
"clean dmesg" says nothing about DMA-API; only the `error_count` arithmetic does. Both harnesses
re-arm the budget around the bind, which is why the count is trustworthy from E72/M6 onward.

## 3. Re-runs with my own tools

Per-case one-line summaries: `V/SUMMARY-7.4-rc3.txt`, `V/SUMMARY-7.2.15.txt` (`V/src/vsum.py`).
"OK" = receiver hash equals the hash of the expected prefix, 0 zero runs, every frag of every
unreadable skb has the net_iov bit, DMA-API other = 0, no kernel log line of the splat class in
either guest, emulator `tx_dma_error` / `tx_desc_data_error` / `tx_desc_fetch_error` = 0.

### 3.1 On 7.4.0-rc3-devmem-kasan, module `dmtx74-1`

| Row | What I ran | Result | Verdict on the claim | Evidence |
| --- | --- | --- | --- | --- |
| M1 | `vtx` bind on the kernel's unpatched in-tree ionic straight after a boot (build-id = installed `ionic.ko`, taint empty, offloads on, no `tx_unreadable_drop` stat) | `BIND_TX failed: Operation not supported (-95) extack='Driver does not support netmem TX'`, nothing received | PASS, CONFIRMED | `V/M1-unpatched-7.4-rc3.txt` |
| M2 | bind + 64 x 1 MiB from odd offsets, TSO on | 67,108,864 bytes, hashes equal, 64 of 64 completions, none COPIED | PASS, CONFIRMED | `V/A1-bind-tso-on.txt` (probe failed to load in this one case; unreadable proof is in every later case) |
| M3 | 64 x 999,983 bytes, offset 13, `tso off`; and `tso off gso off` | 63,998,912 bytes each, OK; 44,225 / 44,233 unreadable skbs at the driver, all non-GSO, `CHECKSUM_PARTIAL`; software GSO (`skb_segment`) ran on 1154 / 1152 unreadable skbs. `gso off` changes nothing for TCP | PASS, CONFIRMED | `V/A2-A3-tso-off.txt` |
| M4 | 480 x 128 B iovecs with 61 B gaps, odd offsets, TSO on; 1024 x 1400 B iovecs | 36.9 MB and 57.3 MB OK; 17,397 and 3246 GSO skbs, up to 17 frags, `tx_unreadable_drop` +0 | PASS, CONFIRMED | `V/B-frags-positive.txt` (B1, B4) |
| M5 | MTU 9000, 64 x 999,983 bytes | 63,998,912 bytes OK. 9198 not re-run | PASS, CONFIRMED for 9000 | `V/D-jumbo.txt` (D10a) |
| M6 positive | the same 128 B stream with TSO off (up to 13 frags); 1-byte, 2-bytes-across-a-page and last-page sends; 300 sends of exactly 15 x 64 B | all OK; 300 of 300 non-GSO skbs with 15 frags sent | PASS, CONFIRMED | `V/B-frags-positive.txt` (B2, B5), `V/B-frags-limit.txt` (B3b, B7) |
| M6 negative | 16 x 64 B; 1024 x 16 B | receiver 0 bytes; 7 and 8 skbs with 16 / 17 frags at the driver, `tx_unreadable_drop` +7 / +9, netdev `tx_dropped` +0, no log line; `sendmsg` reports success (16 frags) or blocks then EAGAIN after 544 bytes (17 frags); a 32 MiB run straight after passes | PASS (clean counted drop), CONFIRMED | `V/B-frags-limit.txt` (B6, B8, B9) |
| M6 threshold (100 / 110 B) | not re-run at MTU 1500 | see V1 for MTU 9000 | NOT RUN | |
| M7 | `tx off`, `sg off`, `tx off sg off`, each also with `gso off`; before connect and 3 s into a stream: 12 cases | **no zero run and no wrong byte reached the receiver in any case**: before connect 0 bytes; mid-stream a verified prefix of 26.0-28.0 MB then nothing. Receiver `TcpInCsumErrors` +0. Mechanisms as reported: `tx_dropped` +9/+10 in the core for `tx off` or `sg off` alone; with both off `skb_copy_and_csum_bits` ran on 10 / 115 unreadable skbs and `tx_unreadable_drop` rose by exactly 10 / 115 | PASS, CONFIRMED | `V/C-offloads-off.txt` |
| M7 control | no-backstop module, `tx off sg off` mid-stream | 80.1 MB "delivered", 1904 zero runs, hash mismatch, all completions success | CONFIRMED (0005 is needed) | `V/C-control-nobackstop.txt` |
| M8a | bound on rocm-ernic0, destination 10.0.2.2 (via enp0s5) | first `sendmsg`: ENODEV; enp0s5 capture: SYN, ACK, FIN, RST from us, no payload (the one payload frame is the peer's SSH banner) | PASS, CONFIRMED | `V/E-wrong-netdev.txt`, `V/E1b-wrong-netdev-frames.txt` |
| M8b | /32 route to the receiver moved to enp0s5 3 s into a stream | next `sendmsg` ENODEV; receiver verified 27,300,819 bytes (all but the last accepted send); enp0s5 `tx_dropped` +3; enp0s5 capture: 16 frames, 0 with payload | PASS, CONFIRMED | `V/E-wrong-netdev.txt` |
| M9 teardown in flight (KASAN) | SIGKILL of the sender mid-stream; SIGKILL while the receiver is not reading (orphan holds the devmem skbs); receiver RST and receiver FIN with 7 MB queued | verified prefixes (657 MB; 12.4 MB; 30.0 MB twice); bind/unbind/free 1/1/1; dma-buf objects back to 0 within 1 s; no KASAN / lockdep / DMA-API report | PASS, CONFIRMED | `V/D-kill.txt`, `V/D-rxclose.txt` |
| M9 rmmod, unbound with unacked data | sender killed with 7.3 MB of devmem skbs queued to a stalled receiver, then `rmmod ionic`, `insmod` | unload and reload return 0; the only report is DMA-API "pending DMA allocations while released from device [count=2428]" (F5); no KASAN; a 32 MiB run after the reload passes | PASS for the driver, CONFIRMED (F5 too) | `V/G-rmmod-a.txt` |
| M9 rmmod, live binding | `rmmod ionic` 4 s into a stream | unload returns 0 (F5 warning, count=2277); receiver verified its 555,979,065 bytes; the sender then **blocks in `sendmsg`** (`sk_stream_wait_memory`; no send timeout set); on its exit: `BUG: KASAN: slab-use-after-free in netdev_nl_sock_priv_destroy+0x1b1`, then `Oops ... RIP: netdev_nl_sock_priv_destroy+0xbd` | **FAIL in the core, CONFIRMED** (F8) | `V/G-rmmod-b.txt`, `V/G-rmmod-b-state-after-timeout.txt` |
| M10 (360 s) | not re-run | | NOT RUN | |
| XDP row, IPv6 row | not re-run | | NOT RUN | |

### 3.2 On 7.2.15-devmem, modules `ernic-dmtx1` (ionic + ionic_rdma)

| Row | What I ran | Result | Verdict on the claim | Evidence |
| --- | --- | --- | --- | --- |
| M1 | `vtx` bind on the kernel's unpatched in-tree ionic straight after boot (build-id = installed module, taint empty, offloads on) | EOPNOTSUPP, extack "Driver does not support netmem TX", nothing received. The DKMS-source variant was not re-run | PASS, CONFIRMED (in-tree) | `V/M1-unpatched-7.2.15.txt` |
| M2 | bind + 64 x 1 MiB, TSO on | 67,108,864 bytes OK; 1107 unreadable skbs, up to 17 frags, 17,494 frags all net_iov | PASS, CONFIRMED (and no longer resting on 46 bytes) | `V/F-subset-7.2.15.txt` (F-A1) |
| M3 | 64 x 999,983 bytes, offset 13, `tso off` | 63,998,912 bytes OK; 44,225 non-GSO unreadable skbs; DMA-API other = 0 | PASS, CONFIRMED | F-A2 |
| M4 | 480 x 128 B iovecs with gaps, TSO on | 36.9 MB OK; 17 frags, no drop; DMA-API other = 0 | PASS, CONFIRMED | F-B1 |
| M6 | 300 sends of exactly 15 x 64 B; 16 x 64 B; 1024 x 16 B | 300 of 300 sent and verified; 16 and 17 frags: receiver 0 bytes, `tx_unreadable_drop` +7 / +9, `tx_dropped` +0, no log line | PASS, CONFIRMED | F-B3b, F-B6, F-B8 |
| M7 | `tx off`, `sg off`, `tx off sg off`; before connect and mid-stream: 6 cases | no zero run, no wrong byte: 0 bytes before connect, verified prefixes of 31.3 / 26.8 / 29.1 MB mid-stream; `tx_dropped` +9/+10 (core) for one feature off; both off: `skb_copy_and_csum_bits` on 10 / 115 unreadable skbs = `tx_unreadable_drop` +10 / +115 | PASS, CONFIRMED | F-C-* |
| M8 | as on 7.4-rc3 | ENODEV in both cases; mid-stream: 29,000,870 bytes verified, enp0s5 `tx_dropped` +3, enp0s5 capture 14 frames, 0 with payload | PASS, CONFIRMED | `V/F-wrong-netdev-7.2.15.txt` |
| M9 exit in flight | SIGKILL of the sender mid-stream | 635 MB prefix verified, binding freed, no report | PASS, CONFIRMED | F-D8 |
| M5, M9 link/reconfig/rmmod, M10 | not re-run on this kernel | | NOT RUN | |

No kernel log line of the splat class in any of these cases (17 in `F-subset`, 3 wrong-netdev, 1 concurrent) other than the bind-time DMA-API
sg-segment warning (F1) in the concurrent-sender case, where four binds race my re-arming of the
print budget. This kernel has no KASAN.

## 4. Things the testers did not do

| # | Case | Result | Evidence |
| --- | --- | --- | --- |
| N1 | **Four concurrent devmem senders**, each with its own dmabuf and binding, pinned to CPUs 0-3 with XPS set so all four Tx queues carry devmem at once; 15 s | PASS: 1.30 GB, 295 MB, 1.41 GB, 321 MB, all four hashes equal and complete; the flows used two to four queues each and all four queues carried devmem skbs; no drop, no report. (First attempt invalid: my expected files filled the guest's tmpfs; kept as `*.first-attempt-*`.) Also PASS on 7.2.15 (978 MB, 251 MB, 904 MB, 286 MB) | `V/D1-concurrent.txt`, `V/F-concurrent-7.2.15.txt` |
| N2 | **devmem, copying and user-memory zerocopy sends interleaved on one socket** (sizes 1 byte to 200 KB, with and without `TCP_NODELAY`) | PASS: 140.6 MB and 2.5 MB verified; 3874 unreadable and 1776 readable skbs on the one flow; no skb mixed the two (every frag of every unreadable skb is a net_iov). PASS on 7.2.15 | `V/D-interleave.txt`, `V/F-subset-7.2.15.txt` |
| N3 | **iovecs outside the dmabuf**: crossing its end, starting at its end, far beyond, second iovec out of range | PASS: each `sendmsg` fails with EFAULT, nothing of it is sent (receiver got exactly the 8192 bytes of the valid send before it), no report | `V/D-oob.txt` |
| N4 | **More data than the dmabuf, many wraps**: 786 MB from a 1 MiB dmabuf incl. sends of the whole buffer and of its last byte | PASS | `V/D-oob.txt` (D3e) |
| N5 | **Zero window**: receiver stops reading for 12 s, twice (64 KB receive buffer), then resumes | PASS: 60.0 MB verified, `TCPToZeroWindowAdv` +2 / `FromZeroWindowAdv` +2 on the receiver. PASS on 7.2.15 | `V/D-zwin.txt` |
| N6 | **Retransmission of devmem skbs under loss**: receiver Rx ring 64 so it overruns | PASS: 150 MB verified with 411 frames dropped at the receiver and 411 retransmitted segments | `V/D-loss.txt` |
| N7 | **Receiver link down 2 s mid-stream** (RTO retransmits) | PASS once the test bed is taken out of it: 1.355 GB verified, 4 RTOs, 363 retransmitted segments. First two runs ended in ECONNRESET with a verified prefix: with rocm-ernic0 down, guest 2's ACKs follow the default route out of enp0s5 and SLIRP answers with a RST (seen in guest 2's enp0s5 capture). A blackhole fallback route for 10.213.100.0/24 removes it. Test-bed effect, not devmem | `V/D-rxflap.txt`, `V/D-rxflap-rerun.txt` |
| N8 | **Receiver closes mid-transfer** (RST, and FIN) with 7 MB queued at the sender | PASS: sender gets ECONNRESET, receiver's 30.0 MB verified, binding and dmabuf freed at once, no report | `V/D-rxclose.txt` |
| N9 | **Packet capture on the sender's netdev during a devmem stream** | PASS: 64 MB verified; the capture holds 1019 frames, none with payload (the core does not hand unreadable skbs to taps), no report | `V/D-tap.txt` |
| N10 | **Small iovecs at MTU 9000** | see V1 | `V/D-jumbo.txt` |

## 5. Findings of this pass

- **V1. At MTU 9000 the frag limit stalls streams that pass at MTU 1500 (design limit, scope
  understated).** 480 x 128 B iovecs and 120 x 512 B iovecs per send at MTU 9000: receiver gets 0
  bytes, 12 non-GSO skbs with 17 frags reach the driver, `tx_unreadable_drop` +12, the sender
  blocks (EAGAIN after 10,624 / 38,400 bytes accepted). 60 x 1024 B and unaligned 4096 B iovecs
  pass. Cause: an skb of less than one MSS is not GSO, the stack fills it to 17 frags, and the
  driver sends at most 15 frags without segmentation; the MSS is 8948 at MTU 9000, so any stream
  whose pieces are smaller than about 600 bytes builds such skbs (about 97 bytes at MTU 1500, which
  is the testers' 100/110 result). Same on 7.2.15 (512 B iovecs, `tx_unreadable_drop` +12, 0 bytes received). Nothing is corrupted and the drop is
  counted, so the M6 "clean counted drop" claim stands; but the reports' F2 ("worth a sentence in
  the docs") should give the rule as "pieces below MSS/15", not as a fixed ~100 bytes.
  `V/D-jumbo.txt` (D10b-D10e), `V/F-subset-7.2.15.txt`.
- **V2. An iovec that crosses a page boundary is two frags.** 15 x 64 B iovecs passed 20 times
  and then stalled when the 21st send had an iovec straddling a page (16 frags, dropped; my first
  B3 run, kept in `V/B-frags-positive.txt`). With page-straddle-free offsets 300 of 300 pass. So
  "exactly 15 frags sends" is right and "15 iovecs send" is not.
- **V3. After `rmmod` with a live binding, a sender without a send timeout blocks in `sendmsg`**
  until TCP gives up (the testers' sender returned short because it had `SO_SNDTIMEO`). The oops
  then comes whenever the process exits, including by SIGKILL. Core, same root cause as F8.
- **V4. `tx_tso_bytes` is not a byte count** (`stats->tso_bytes = len;` in `ionic_tx_tso()`, an
  assignment, in pristine v7.4-rc3 at `ionic_txrx.c:1553`): 67 MB of TSO moved it by 6082.
  Pre-existing mainline, not the series; noted because the counter diffs show it.
- **Not findings, my own mistakes, kept for the record:** `V/D1-concurrent.first-attempt-*` (tmpfs
  full), `V/D-oob.first-attempt-*` (no expected file written), the "KASAN/BUG lines: 9" count in
  `V/D1-concurrent.txt` (pattern matched `lockdep_*` frame names in the bind-time DMA-API traces;
  the log has no such report), B7 (the offset I chose did not straddle a page).

## 6. Identity of what was tested (task item 4)

| Item | Check | Result |
| --- | --- | --- |
| Emulator | sha256 of `/usr/local/bin/rocm-ernic` copied out of both `ernic-N` containers | `29413ea9e943196b...3d7ef118` = `T/ernic/tx-offloads-1/build/rocm-ernic`; bind mount shown by `docker inspect`. `diff -rq` of the snapshot's `src/` against the rocm-ernic working tree: identical. The netdev shows SG, HW csum and TSO, which the image's server does not offer. CONFIRMED |
| ionic on 7.4-rc3 | `/sys/module/ionic/notes/.note.gnu.build-id` in both guests vs `readelf -n ~/ionic-mod/dmtx74-1/ionic.ko` | `176291d54f6083c46d20ec253a91e0111a36ce8f` both; taint `O`; differs from the installed in-tree module (`f65bded2...`), from `-nobackstop` (`1eb9bf28...`) and `-xdpfix` (`83c07a4d...`). Staged tree `diff -rq`-identical to `/var/tmp/ionic-devmem/linux` (= v7.4-rc3 + the six patches, uncommitted). `tx_unreadable_drop` present. CONFIRMED |
| ionic on 7.2.15 | same, after `ionic-mod.sh load ernic-dmtx1` | ionic `000a7bb3100be7bd3aa4e1bd356776364874f9af`, ionic_rdma `3817350a341a92c3ef77f9bb8b721675d60381af`, both equal to the built files, both guests; in-tree module is `27652d56...`; RDMA link ACTIVE. `ernic-dmtx1/SOURCE` lists the seven `patches/ernic-v7.2.6` files with the hashes they have now; `diff -rq` against rocm-ernic's `ansible/files/patches/devmem-tx/` shows only the README. CONFIRMED (the testers' own file records only the taint flag) |
| Behavioural marker | unpatched in-tree module refuses the bind on both kernels; patched one binds | `V/M1-unpatched-*.txt` |

Modules here have no `srcversion` (built without it), hence the build-id. `V/00-identity.txt`,
`V/F-identity-7.2.15.txt`.

Not verified independently: that the frag DMA addresses lie in the dmabuf (testers' PFN check)
and "one DMA map call per skb". The second I recounted at the end (`V/Z2b-dma-map-recount.txt`): 1870 unreadable skbs with 31,310 frags entered `ionic_start_xmit`, 1870 `dma_map_page_attrs` calls were made inside it, and 1873 unmaps in total (1870 + 3 readable skbs): one map per skb, none per frag, CONFIRMED. The first I did not redo. The hash at the receiver plus the net_iov bit on every
frag is what my PASS verdicts rest on.

## 7. State left, and what I did not run

- Stack 1 up, restarted once by me after the unload oops (`V/H-stack-restart-after-oops.txt`).
  Both guests on `7.4.0-rc3-devmem-kasan` with `dmtx74-1` loaded (build-id checked again), server `tx-offloads-1`, 10.213.100.1N/24 added, taint 4096 (`O` only), 0 dma-buf objects. Final run: 63,998,912 bytes verified (`V/Z-final-state.txt`). In between, the guests were on 7.2.15-devmem with `ernic-dmtx1` for section 3.2.
- Guest additions: `/usr/local/bin/vtx`, `vrx.py`, `vprobe.bt` in both guests. Nothing else kept:
  XPS masks back to 0, MTU 1500, offloads on, rings 1024, `tcp_retries2` 15, no extra routes.
- No product code, patch, testers' script or testers' evidence was changed. No commit anywhere.
- 192.168.100.x between the guests is still dead (F9: `ionic-devmem-rx_l2` holds 192.168.96.0/20);
  everything ran over 10.213.100.1N.
- NOT RUN: M10 (360 s), the XDP row and the B1 fix, IPv6, MTU 9198, the 100/110-byte threshold at
  MTU 1500, sender-side link flap (F4), ring/channel/MTU reconfiguration mid-stream, completions
  for dropped data (F3; my waits were 2 s), VLAN, firmware reset, PCI unbind, and the unload cases
  on 7.2.15.
