<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# devmem TCP proof of concept: test aids

The programs, probes, scripts and kernel patches that the devmem TCP
proof of concept on the emulated ionic NIC was tested with. What the
proof of concept is, and what it showed, is in
[`docs/devmem.rst`](../../docs/devmem.rst); the reports of each run are
the working notes in [`docs/devmem/notes`](../../docs/devmem/notes).

**These are test aids, not part of the product, and not tests of this
repository:**

- CMake builds none of it, and none of it is a `ctest` test. The
  registered tests need no VM; everything here needs two guests.
- The C programs are built by hand, for a guest (below).
- The scripts were written for one development host and two test stacks
  on it. They are here so the runs can be repeated and read, not as a
  supported harness: see "The harness" for what they assume.

## What is here

| Directory | What |
| --- | --- |
| [`src/`](src) | The guest-side test programs: C, one XDP program, Python |
| [`probes/`](probes) | bpftrace scripts, run in a guest during a case |
| [`scripts/`](scripts) | The harness: host-side scripts that drive two guests |
| [`tools/`](tools) | Patch-series generators, golden-frame generators, mutation testers |
| [`kernel-patches/`](kernel-patches) | Kernel and amdgpu patches the tests needed or produced, each with a README |

The ionic driver series themselves are not here but in
[`ansible/files/patches`](../../ansible/files/patches): `devmem-tx`,
`devmem-rx-7.2.6`, `devmem-tx-mainline` and `devmem-rx`.

## The test programs

| File | What it is |
| --- | --- |
| `src/devmem_tx.c` | The devmem Tx exerciser. Sender: makes a udmabuf with a position-dependent pattern, binds it (`NETDEV_CMD_BIND_TX`, raw generic netlink) and sends from chosen offsets, lengths and iovec counts with `MSG_ZEROCOPY` and `SCM_DEVMEM_DMABUF`, reaping every completion. Receiver (`-r`): recomputes the stream from the same plan and compares every byte. Also sends the same framing by copy or by ordinary zerocopy, as controls. |
| `src/devmem_tx_gpu.c` | `devmem_tx.c` plus `-G NODE`: the buffer is an amdgpu GEM object in VRAM (`-J`: in GTT), exported as a dma-buf. |
| `src/devmem_rx_gpu.c` | The devmem Rx exerciser, and the receiving half of the GPU-to-GPU runs. Binds a dma-buf to Rx queues (`NETDEV_CMD_BIND_RX`, raw generic netlink), accepts one connection and receives with `recvmsg(MSG_SOCK_DEVMEM)`. **The payload is never read from the socket**: for each `SCM_DEVMEM_DMABUF` cmsg it reads the bytes from the dma-buf's own memory at the reported offset — through a GEM mapping (`-G`), through the parent's HIP device-to-host copy (`-E -Y -M`), or through the memfd (udmabuf, the control) — checks them against the sender's plan, and only then returns the tokens with `SO_DEVMEM_DONTNEED`. The buffer is pre-filled with `0xa5`, so a byte the NIC did not write cannot pass, and `mismatch_a5` and `mismatch_zero` are counted apart. |
| `src/hip_devmem_rx.cpp` | The GPU compute buffer behind `devmem_rx_gpu -E`: `hipMalloc()`, a `0xa5` pre-fill checked by reading it back, `hsa_amd_portable_export_dmabuf()`, then fork and exec of the receiver with the dma-buf fd. Each time the receiver asks on its gate, this copies the whole buffer device-to-host into a shared memfd (`-c`: through a GPU kernel instead) so that the receiver verifies what the GPU holds. |
| `src/vtx.c`, `src/vrx.py` | The verification pass's own sender and receiver, written from the uapi and not from `devmem_tx.c`. The buffer is random, the sender writes what the kernel accepted to a file, and the receiver hashes what arrives. |
| `src/pcapcap.c` | A static packet capture to a pcap file, for a container's network namespace that has no tcpdump. |
| `src/xdp_echo.c`, `src/udp_echo_check.py` | An XDP program that echoes UDP port 7777 with `XDP_TX` or `XDP_REDIRECT`, and the sender that checks the echoes. |
| `src/nettool.py`, `src/hwtstamp.py`, `src/xdp_pin.py` | Guest helpers of the Rx tests: hashed TCP and self-checking UDP traffic; the hardware timestamp ioctls; loading and pinning a two-instruction XDP program without clang. |

The Rx tests receive with the kernel's own `ncdevmem` selftest, built
from the kernel tree by `scripts/stack1/build-ncdevmem.sh`; it is not
copied here.

### Building them for a guest

From the top of the repository, with the flags every guest-side program
of this project is compiled with. `devmem_tx`, `devmem_tx_gpu`,
`devmem_rx_gpu` and `vtx` reach `memfd_create()` through
`src/ernic_gnu.c`, and all five parse numbers with `src/parse_int.h`:

```bash
OUT=$(mktemp -d)    # not the source tree: git does not ignore the binaries
FLAGS="-std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -O2 -Wall -Wextra -static -pthread -Isrc"
gcc $FLAGS -o "$OUT/devmem_tx" tests/devmem/src/devmem_tx.c src/ernic_gnu.c
gcc $FLAGS -o "$OUT/vtx"       tests/devmem/src/vtx.c       src/ernic_gnu.c
gcc $FLAGS -o "$OUT/pcapcap"   tests/devmem/src/pcapcap.c
# needs <drm/amdgpu_drm.h>: point UAPI at a kernel's "make headers" output
gcc $FLAGS -isystem "$UAPI" -o "$OUT/devmem_tx_gpu" \
  tests/devmem/src/devmem_tx_gpu.c src/ernic_gnu.c
gcc $FLAGS -isystem "$UAPI" -o "$OUT/devmem_rx_gpu" \
  tests/devmem/src/devmem_rx_gpu.c src/ernic_gnu.c
```

`hip_devmem_rx.cpp` is the exception: it is C++ and needs ROCm, so it is
built with `hipcc` for the GPU of the guest it runs in, not with the
flags above, and not statically:

```bash
R=/opt/rocm                 # the guests use /opt/rocm/core-10.2
"$R/bin/hipcc" -O2 --offload-arch=gfx1250 -I"$R/include" \
  -o "$OUT/hip_devmem_rx" tests/devmem/src/hip_devmem_rx.cpp \
  -L"$R/lib" -lhsa-runtime64 -Wl,-rpath,"$R/lib"
```

A host without ROCm cannot build it at all;
[`scripts/stack3/install.sh`](scripts/stack3/install.sh) builds it in
each guest, which is where it has to run anyway.

The binaries are static, so they can be built on the host and copied in.
Copy one into the guest's own filesystem before running it: a guest that
runs a rebuilt binary straight from the 9p share can execute stale
pages. The XDP program needs clang and no libbpf headers; `-DACT_TX` or
`-DACT_REDIRECT` chooses the action and `-DFRAGS` the multi-buffer
section:

```bash
clang -O2 -g -target bpf -I/usr/include/x86_64-linux-gnu \
  -DACT_TX -DFRAGS -c tests/devmem/src/xdp_echo.c -o "$OUT/xdp_tx_frags.o"
```

`devmem_tx` takes its numbers in decimal, with `k`, `m` or `g`; `vtx`
also takes hex with `0x`.

## The probes

| File | Attached to | Shows |
| --- | --- | --- |
| `probes/tx-probe.bt` | `ionic_start_xmit`, the driver's Tx map path, `skb_segment()`, `skb_copy_and_csum_bits()`, skb drops, the bind calls | That the data skbs are unreadable with `net_iov` frags (none fell back to a copy), their frag counts, the addresses mapped |
| `probes/vprobe.bt`, `probes/dmamap.bt` | the same, written independently for the verification pass | Every frag's netmem bit per destination port and queue; calls that would read an unreadable skb; DMA maps per skb |
| `probes/p2p-probe.bt` | `pci_p2pdma_distance_many()`, amdgpu's dma-buf attach, pin and map, `ionic_tx_map_frag()` | Whether a GPU buffer was attached peer-to-peer and which address range the NIC was given |
| `probes/skbshape.bt` | the GRO receive entry points and `tcp_v4_rcv()` | Linear and frag bytes of received skbs: whether headers were split from payload |
| `probes/rx-probe.bt` | the dma-buf attach, pin and map, `pci_p2pdma_distance_many()`, `ionic_tx_map_frag()`, `mp_dmabuf_devmem_alloc_netmems()`, `ionic_rx_fill()` and `dma_sync_single_*` | Whether the binding was peer-to-peer, and whether each net_iov DMA address, Rx payload descriptor and Tx frag is in the GPU's BAR0 or in system RAM. Run in **both** guests during a GPU-to-GPU case |

## The harness

The harness was written in a work area on the development host,
`/var/tmp/ionic-devmem`, and still runs from one. Host scripts find each
other, the test binaries and the probes by their paths under it, the
kernel trees, guest images, built modules and evidence live there, and
the guests mount it over 9p as `/mnt/hostfs` and read some scripts and
binaries from there.

- `DEVMEM_ROOT` names the work area, and **must be set: there is no
  default**. The scripts used to fall back to `/var/tmp/ionic-devmem`,
  and on the development host a script started without the variable
  then acted on a stack that other work was using. Now every script
  that reaches a guest or the work area stops at
  `${DEVMEM_ROOT:?set it to the work area}`. The two fleet files,
  `scripts/stack1/devmem-fleet.xml` and `scripts/stack2/rx-fleet.xml`,
  still name `/var/tmp/ionic-devmem` literally: `qemu-tool` reads them
  as they are.
- [`scripts/deploy.sh`](scripts/deploy.sh) copies every file to its
  place in the work area; [`scripts/work-area.map`](scripts/work-area.map)
  lists the places. Run a script from the work area, not from here.
- `ERNIC_REPO` names this repository where a script reads it
  (`build-ernic.sh` snapshots it to build a server); the default is
  `$HOME/Projects/rocm-ernic`.

| Here | In the work area | What |
| --- | --- | --- |
| `scripts/stack1/` | `test/scripts/` | Stack 1 (compose project `ionic-devmem`, guests `devmem-1` and `-2`): `stack.sh` (up, down, which server), `gssh` (ssh to a guest), kernel and module builds and installs, plain-TCP sanity |
| `scripts/stack1/devmem-tx/` | `test/scripts/devmem-tx/` | The devmem Tx matrix: `lib.sh`, one script per row (`m3-m5.sh` to `m10.sh`, `m-xdp.sh`), `summarize.py` |
| `scripts/stack1/emu-offloads/` | `test/scripts/emu-offloads/` | The emulator Tx offload validation against unpatched drivers: `run.py` on the host, `gt.py` in the guests |
| `scripts/stack1/gpu/` | `test/scripts/gpu/` | The GPU-memory runs: the `CONFIG_PCI_P2PDMA` kernel, the patched in-tree amdgpu, `run-vram-test.sh` |
| `scripts/tx-verify/` | `test/evidence/tx-verify/src/` | The independent verification pass: `v.sh` and one script per group of cases |
| `scripts/stack2/` | `test-rx/scripts/` | Stack 2 (`ionic-devmem-rx`, guests `rx-1` and `-2`, server in `header-split` mode): the devmem Rx cases |
| `scripts/stack2/guest/` | `test-rx/guest/` | Scripts run as root in a stack 2 guest |
| `scripts/stack3/` | `test-rocm102/e2e/scripts/` | Stack 3 (`ionic-devmem-r102`, guests `r102-1` and `-2`, the ROCm 10.2 image): the GPU-to-GPU end-to-end cases. `install.sh` builds the two receive programs in the guests, `case.sh` runs one case, `sizes.sh` and `bench.sh` sweep it, `plan_bytes.py` computes on the host what a fragment should hold |
| `scripts/host/` | `test/scripts/drun.sh`, `test-rx/scripts/drun.sh`, `test/builder/Dockerfile` | Glue for the development host only: `drun.sh` works around its Docker daemon (a container needs an explicit cgroup parent, and attached output is lost); `Dockerfile.kbuild` is the kernel build container |
| `src/`, `probes/`, `tools/`, `kernel-patches/` | where `work-area.map` says | |

What the harness needs that is not in this repository:

- qemu-minimal's `qemu-tool` (2.0.0 for stacks 1 and 2) and its stack
  definition
  `qemu/stacks/rocm-golden.xml`, cloned into the work area, and the
  container and guest images that names. `stack.sh` runs it.
- Docker, and KVM.
- A kernel tree in the work area (`linux`, at `v7.4-rc3`; `linux-7.2.6`
  and `linux-7.2.15`) for the guest kernels, `ncdevmem`, and the in-tree
  ionic the mainline series is applied to.
- The guests' ssh key, which `qemu-tool` fetches with the image. Never
  put it, or anything under `images/keys/`, in the repository.

The runbooks are the notes `testbed.md` (stack 1) and `test-rx.md`
(stack 2); they say what state each stack was left in and how each step
was done.

### Running the Tx matrix

With stack 1 up, a kernel with `CONFIG_NET_DEVMEM` and `CONFIG_UDMABUF`
in both guests, the ionic module with a devmem Tx series loaded
(`stack1/ionic-mod.sh`), and `devmem_tx` built into `test/bin/`:

```bash
S=$DEVMEM_ROOT/test/scripts/devmem-tx
$S/install.sh                       # copy devmem_tx into both guests
E=$DEVMEM_ROOT/test/evidence/mine $S/smoke.sh
E=... $S/m3-m5.sh > .../M3-M5.txt   # then m6, m7, m7x, m8, m9, m10, m-xdp
$S/m9-rmmod.sh                      # last: on a kernel without the core fix it oopses
$S/summarize.py .../*.txt           # one line per case
```

Each case prints the sender's and the receiver's `RESULT` line, the
probe's maps, the counters of both guests and both emulators that
changed, and the kernel log since a marker. **The receiver's line is the
verdict**; a sender that reports success proves nothing about the bytes.
`ALT=1` uses the `10.213.100.x` addresses (`testbed.md` section 11 says
why the development host needed them).

The verification pass is `scripts/tx-verify/`: source `v.sh`, `vinstall`,
then the case scripts. The Rx cases are `scripts/stack2/`: `td-setup.sh`,
`final.sh` for a first transfer, then `td1-kill.sh` to `td3-more.sh` and
the `rv-*.sh` review cases. The emulator offload matrix is
`scripts/stack1/emu-offloads/run.py LABEL`.

### Running a GPU-to-GPU case

With stack 3 up on a kernel carrying
[`kernel-patches/net-core-devmem-p2p`](kernel-patches/net-core-devmem-p2p),
the ionic module built with `devmem-tx`, the Rx series and the
frag-limit patch, the server in `header-split` mode, and the receiving
guest at one combined channel with `tcp-data-split on`:

```bash
S=$DEVMEM_ROOT/test-rocm102/e2e/scripts
$S/install.sh                                      # both guests
$S/case.sh B1 5401 gem-vram gem-vram 64m 64        # VRAM to VRAM
HOLD=1 $S/case.sh B1h 5402 gem-vram gem-vram 64m 64  # + the host-side check
$S/sizes.sh D-vram2vram gem-vram gem-vram "4m 16m 64m 256m"
$S/bench.sh 3
```

**The receiver's `RESULT rx` line is the verdict.** `HOLD=1` makes the
receiver keep its last batch's tokens while the host hashes the
*receiving* NIC server's own mapping of the GPU's BAR at the address the
driver gave the device, and compares that with what `plan_bytes.py` says
was sent — the check that the bytes really reached VRAM, independent of
anything in the guest. A dma-buf of 4 MiB is the smallest that kept an
`rx 1024` ring fed in these runs; 1 MiB starves the page pool and the
connection collapses (with nothing corrupted).

### State after the move into the repository

The files were changed on the way in, to pass this repository's checks,
and **no harness script has completed a run against a stack from this
tree**. One was started by accident: `stack1/devmem-tx/smoke.sh`,
deployed from here to a temporary root but run without `DEVMEM_ROOT`,
fell back to the real work area and stack 1. It wrote its case marker
to both guests' kernel logs and started the receiver and the bpftrace
probe, and was killed about 30 s later, before its sender ran. Why it
had not reached the sender by then was not looked into (`lib.sh` waits
up to 30 s for the probe to attach, which may be all it was), so the
run shows nothing either way, and **whether the moved harness still
drives a case to its sender is open** until a deliberate run from this
tree passes at the receiver. The accident is why `DEVMEM_ROOT` now has
no default.

- The C programs: `_GNU_SOURCE` removed, `memfd_create()` and the seals
  through `src/ernic_gnu.c` (which seals more than the original did:
  grow and seal as well as shrink), numbers through `parse_int.h`
  (`devmem_tx` no longer takes hex), `vtx`'s spec split by hand rather
  than `sscanf()`, 64-bit constants as `UINT64_C()`, and clang-format.
  `xdp_echo.c` is the exception to this repository's C rules, and was
  left as it was but for its header and formatting: it is a BPF object
  built with `clang -target bpf`, not C11 userspace, CMake does not
  build it, and it needs `__attribute__((section))` for its `SEC()`
  macro and `__builtin_bswap16()` and `__builtin_memcpy()`, since a BPF
  program has no libc. Nobody has given permission for those
  extensions; say so if it should not be here. The four userspace
  programs compile with no warning
  against `v7.4-rc3` uapi headers, a udmabuf was created from a memfd
  sealed that way on the host, and `devmem_tx` was run sender to
  receiver over loopback in its copy mode. After the last change (`UINT64_C()`),
  the programs were rebuilt against the host's headers only, and the
  original `devmem_tx` binary sent to the rebuilt receiver over
  loopback, in copy mode, with no mismatch. Later `devmem_tx`, `vtx`,
  `pcapcap` and the XDP object were built from this tree with the
  commands above (GCC 13.3, the host's headers), and **that `devmem_tx`
  made one devmem send on stack 1**, guest to guest on `v7.2.15` with
  the `v7.2.6` ionic sources plus `srq`, `p4` and `devmem-tx`: the
  smoke case, 64 sends of 64 KiB from a 4 MiB udmabuf. The receiver,
  the same binary, reported `RESULT rx PASS`, 4,194,304 bytes and no
  mismatch; `probes/tx-probe.bt`, also from this tree, showed all 85
  data skbs unreadable with `net_iov` frags and every page the driver
  mapped inside the dma-buf; no `tx_unreadable_drop`, DMA-API error or
  kernel log line. That is one case with one binary. Every other
  result in the notes is from the original binaries, and `vtx`,
  `pcapcap`, `devmem_tx_gpu` and the XDP object from this tree have
  not been run in a guest. `devmem_tx_gpu` was last built against
  `v7.4-rc3` uapi headers before the `UINT64_C()` change.
- `devmem_rx_gpu.c` and `hip_devmem_rx.cpp` came in later, with the
  GPU-to-GPU runs. `devmem_rx_gpu.c` was changed the same way as the
  others — `_GNU_SOURCE` removed, `memfd_create()` and the seals through
  `src/ernic_gnu.c`, `atoi()`, `strtoull()` and the `sscanf()` of `-Y`
  replaced by `parse_int.h`, `die()` marked `_Noreturn`, clang-format —
  and from this tree it compiles with **no warning** under GCC 13 and
  clang 18 with the flags above, against the host's headers.
  `hip_devmem_rx.cpp` kept its code and gained its copyright header; it
  builds with `hipcc` on a host with ROCm. **Neither has been run from
  this tree**: every result in the notes is from the originals, which
  differ from these only in the changes just listed.
  `scripts/stack3/plan_bytes.py` was rewritten for black and pylint and
  gives the same hash as the original on three plans.
- The Python: reformatted and restructured for black and pylint. Each
  was compared with its original: the evidence summarizers and pcap
  checkers on the recorded evidence and captures, `gt.py`,
  `nettool.py` and `udp_echo_check.py` over loopback, `run.py`'s
  capture analysis and guest scripts on recorded captures, the golden
  frame generators and verifiers on their output. `hwtstamp.py`,
  `xdp_pin.py`, `vrx.py`, `rxq.py`, `patch_golden.py` and `edit.py`
  were changed by a line or two and only compiled or run trivially.
- The shell scripts: quoting and unused variables for ShellCheck, the
  work area as `DEVMEM_ROOT`, libraries sourced from beside the script.
  They parse (`bash -n`) and pass ShellCheck, and that is all, but
  for the accident above.
  `deploy.sh` is new, and was run against an empty directory.

### Left out

- `mutate_asan.py`: merged into `tools/emu-mutate/mutate.py`, which
  takes the build directory and the mutants to run.
- The second `edit.py` and `drun.sh`, which were copies.
- The review-time driver simulator and descriptor fuzzer for the Tx
  offload code (`drvsim.c`, `fuzz.c` and their checkers), one-offs that
  `tests/test_ionic_eth_tx.c` and the fuzz harnesses replace.
- The control series "devmem-tx without its backstop": it is
  [`ansible/files/patches/devmem-tx`](../../ansible/files/patches/devmem-tx)
  without `0006`.
- Most of the third stack's scripts (the ROCm 10.2 guest image), which
  were still changing; `rocm102-baseline.md` and `rocm102-devmem.md`
  describe them. Only the end-to-end set is here, in
  `scripts/stack3/`, and **it does not stand alone**: stack 3's own
  `env.sh`, `gssh`, `gscp`, `stack.sh` and `ionic-mod.sh`, and the
  sender `devmem_tx_ext2` and its HIP exporter `hip_devmem_src2` (which
  are `devmem_tx_gpu.c` and a HIP source with extra options, built in
  the guests' `~/gputx`), are not. So `case.sh` and the two sweeps
  **cannot be run from this tree as they are**; they are here to be
  read, repeated and adapted.
- All evidence, kernel trees, images and keys.

## The tools

| Directory | What |
| --- | --- |
| `tools/patch-series/tx-mainline/`, `rx-mainline/`, `tx-backport-v7.2.6/`, `xdp-frag-offset/` | How each patch series was made and checked. A series was developed as one copy of the ionic files per patch (a "stage"); `mkpatches.sh` turns the stages and the messages into `git am` files, and `verify.sh` checks that the files reproduce the stages, parse with `git mailinfo`, pass `checkpatch.pl` and build at every patch. The stages were not moved; applying a series patch by patch recreates them. |
| `tools/emu-golden/`, `tools/emu-golden-rx/` | Where the frames in `tests/golden/ionic_eth_tx_frames.h` and `ionic_eth_rx_frames.h` came from, and their independent check. `gen_rx.py` writes the whole Rx header from fields; `gen_review.py` added the three IPv6 TSO cases to the Tx header; `verify.py` and `verify_rx.py` have scapy recompute every checksum. Run from here, the generators' frames are byte for byte those in the headers, and both verifiers pass. |
| `tools/emu-mutate/`, `tools/emu-rx-mutate/` | Mutation checks of the emulator's Tx offload and Rx header-split code: each removes or weakens one check in a copy of the source and expects `test_ionic_eth_tx` or `test_ionic_eth_rx` to fail. They write only to a scratch directory. At the commit they were added in, 18 of the 19 Tx mutants fail the test and the 19th no longer finds its line, and all 60 Rx mutants fail it. |

The verifiers need scapy (`uvx --with scapy python
tests/devmem/tools/emu-golden/verify.py tests/golden/ionic_eth_tx_frames.h`).
