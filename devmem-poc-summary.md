# devmem TCP on the emulated ionic NIC

A proof of concept on the `feat/devmem-poc` branch: seven commits that let a
guest send and receive TCP payload directly out of, and into, memory the
kernel may not read — including GPU VRAM, end to end between two guests.

## What devmem TCP is, and why bother

Ordinary zero-copy networking still assumes the kernel can touch the payload:
it checksums it, it linearizes it when a driver cannot handle the layout, it
copies it on a retransmit. Devmem TCP drops that assumption. A dma-buf is
bound to a netdev, and `sendmsg()` refers to offsets inside it rather than to
process memory. The NIC reads the payload by DMA; the kernel sees only an
opaque fragment it must never read. The same in reverse for receive: the NIC
writes the payload into the dma-buf, and the application gets back offsets.

If the dma-buf is GPU memory, data can go from one GPU's VRAM onto the wire
and into another GPU's VRAM without a bounce through host memory — which is
the point for a machine whose job is moving tensors between accelerators.

The question this work set out to answer was narrow: **is devmem TCP on ionic
a driver-only change, or does it need new firmware or hardware?** The answer,
established by code and then by running it, is that the kernel side is
driver-only — but the device must grant transmit scatter-gather and checksum
offload, and for receive it must split headers from payload, which no ionic
firmware does.

## The three parts

**The emulator** (`src/ionic/`, about 1,500 lines). The emulated NIC granted
no offloads at all, so a Linux guest came up with no scatter-gather, no
checksum offload and no TSO — and the stack will not hand a device unreadable
payload without the first two. The series implements all three, plus the v1
scatter-gather descriptor layout the driver needs, and fixes a descriptor
length convention that disagreed with the real driver. It also adds an opt-in
**receive header-split mode** (`--nic-profile header-split`): a device
interface invented for the purpose, clearly marked as not a firmware
commitment, which puts each segment's headers in one buffer and its payload in
another so the payload can land in memory the kernel may not read.

**The driver series** (`ansible/files/patches/`). Four directories, two
changes in two forms each: a transmit series and a receive series, in a
mainline form against `v7.4-rc3` for eventual upstream submission, and a
backported form against the `v7.2.6` sources the fleet's guests build. The
transmit series sets `netdev->netmem_tx`, unmaps fragments through the netmem
helpers, counts what it cannot send, and sends an over-limit unreadable skb as
a TSO chain rather than dropping it. The receive series makes ionic an
ops-locked driver, converts receive buffers to netmem, implements the
header-split datapath and the queue-management ops the core requires, and
negotiates the emulator's interface.

**The test material** (`tests/devmem/`, 163 files; `docs/devmem/notes/`, 41
notes). The programs, probes, harness scripts and kernel patches the results
were produced with, and the working notes that record them — including the
independent verification passes, whose corrections take precedence over the
reports they audit.

## What was actually demonstrated

Transmit was proven on three kernels — `v7.2.15`, `v7.4-rc3` under KASAN and
lockdep, and the fleet's own `v7.2.6` — with a matrix covering TSO on and off,
unaligned offsets, many-fragment sends, MTU 1500 to 9198, teardown with sends
in flight, and sustained runs: 74.1 GiB in 360 seconds with every zerocopy
completion returned and every byte verified at the receiver. A probe confirmed
each data skb was genuinely unreadable with `net_iov` fragments, so no silent
fallback to a copy could pass as success.

Receive reached first light on `v7.4-rc3` and then on the pinned `v7.2.6`
line, where the backport needed no code change at all.

The headline result is **GPU VRAM to GPU VRAM**: a buffer filled by one guest's
GPU, sent by the NIC reading its BAR directly, received by the other NIC
writing into the second GPU's BAR, and read back through the GPU. The
verifier reproduced it independently with its own data and probes, and
confirmed all four links in a single run — sender fragment addresses in the
GPU BAR, receiver `net_iov` addresses in the BAR, both buffers reported VRAM
while bound, and the bytes found in the receiving GPU's memory — plus a
negative control and a poison pre-fill whose untouched tail survived.

On the kernel the fleet uses, that needs exactly **one** upstream commit
backported (`69ab87acc614`, in `v7.4-rc1`), which allows the peer-to-peer
attach. A full tree comparison showed the test kernel was stock `v7.2.6` plus
that one file and nothing else.

## What this gives rocm-ernic today

Within the emulator this is a working capability, both directions, and all of
it is on the branch. What has to be true for it:

* **The fork QEMU**, which passes a device BAR's file descriptor in its DMA
  map so the NIC server can reach the GPU's memory. The stock image never
  advertises the window, and fails silently.
* **`CONFIG_PCI_P2PDMA` and `ZONE_DEVICE`** in the guest kernel — absent from
  the bundle kernel's configuration today, which is worth fixing given that
  the whole point of the fleet is a GPU beside an RDMA NIC.
* **The backported peer-to-peer attach commit** on any `v7.2.x` kernel, in
  `tests/devmem/kernel-patches/net-core-devmem-p2p/`.
* **The patch series**: `devmem-tx` for transmit, `devmem-rx-7.2.6` for
  receive, with `ionic_rdma.ko` and `ernic_p4.ko` rebuilt against the result.
* **`--nic-profile header-split`** on the server, for the receive side.
* An **AMD Zen host**: the peer-to-peer distance check passes there, and on an
  Intel host the path falls back to system memory without saying so.

Two qualifications on "between two nodes". The demonstration was two guests on
**one physical host**, their Ethernet crossing a TAP per server on a shared
bridge. Guests on different hosts should work by the same mechanism — each
server reaches only its own local guest's GPU BAR, and what passes between
them is ordinary TCP over the host network — but that has never been run. And
the receive side is real only because the emulator implements an interface we
invented: a guest running on rocm-ernic exercises our interface, not a
firmware one, so if header split is later specified differently, the receive
series changes with it.

## Sizes, and the three places they bite

Buffer and payload sizes were swept rather than assumed, and three limits
turned up — two at the bottom of the range, one at the top.

**The smallest receive buffer that works is about 4 MiB.** A 1 MiB dma-buf
bound for receive starves the page pool against a 1024-entry ring: only 256
`net_iov`s exist, allocation failed 9,995 times in 100 seconds, the NIC has
nothing to post, and TCP collapses into retransmission backoff (`cwnd:1
backoff:7`). Nothing is corrupted — it simply stops making progress. 4 MiB and
above passed in every repetition.

**Small *pieces* used to kill the connection, and no longer do.** ionic takes
a header plus 15 fragments per descriptor, and an unreadable skb cannot be
linearized, so a send built from pieces below roughly MSS/15 was dropped and —
because TCP retransmits the same skb — never recovered. Measured: at MTU 1500,
110-byte pieces passed and 100-byte pieces stalled to `ETIMEDOUT`; at MTU
9000, streams of 128-byte and 512-byte pieces delivered nothing at all. The
frag-limit patch re-sends such an skb as a TSO chain, and those same cases now
complete with every byte verified. What is still dropped: anything where the
device cannot segment — TSO turned off for that IP version, IPv4 without DF,
non-TCP, or a layout needing more than half the transmit ring.

**At the top, 1020 MiB works and 1024 MiB silently does not.** With 1021 MiB
of the emulated GPU's 1024 MiB free, a buffer requested at exactly 1024 MiB is
created, bound and sent — from **system memory**, pinned in GTT, with every
fragment outside the GPU BAR. The payload is still correct, so only the
addresses reveal it. 1020 MiB stayed in VRAM. Separately, `hipMalloc` fails at
1016 MiB with an out-of-memory error, before any bind, where a GEM buffer of
that size succeeds — HIP's own limit, not the NIC's.

Verified ranges, each repeated: transmit and receive from 4 MiB to 1020 MiB
for GEM buffers, 16 MiB to 960 MiB for `hipMalloc`. Sustained runs: 655 MiB
VRAM to VRAM over 200 seconds with 484,668 tokens recycled, 67.3 GiB from a
GPU buffer over 330 seconds, and 74.1 GiB over 360 seconds from a udmabuf —
all with zero mismatching bytes and every completion returned.

## The trap worth knowing

Without that commit the attach is static, amdgpu quietly moves the buffer to
system memory, and **the transfer still succeeds and still verifies**. Every
byte arrives correct; only the addresses betray that the NIC never touched
VRAM. Any claim of a VRAM transfer that rests on payload verification alone is
worthless. Check the placement, not the bytes.

## Limits

Receive is emulator-only: the device interface it negotiates does not exist in
firmware. Nothing has run on ionic hardware, under an IOMMU, or on an Intel
host — the peer-to-peer path here depends on a CPU check that passes on AMD
Zen. A receiver whose only queue holds a binding gets nothing on ordinary TCP,
which bounds "devmem and RDMA on the same NIC". A receive dma-buf below about
4 MiB starves the page pool. Performance figures in the docs are emulation on
a loaded shared host where identical runs varied by more than 2x; they measure
the emulator, not a NIC.

## Three bugs found on the way

Testing turned up three pre-existing defects, none of them ours. An **XDP
multi-buffer information leak** in mainline ionic put another frame's bytes —
sometimes stale kernel memory — on the wire; half of 10,000 echoed datagrams
were corrupt. A **queue-count change leaked** DMA allocations and left a page
pool pointing at a freed NAPI. Both are fixed in the shipping patch series and
reported to the Pensando team. The third is a **core use-after-free** on module
unload with a live transmit binding, already fixed in net-next; the backport is
carried as a file for whoever builds the guest kernel.

## Where next

Real hardware is the gap that matters: it alone can confirm the series against
real firmware, and whether a NIC reads real VRAM through an IOMMU. The runbook
is written and nothing has run. Beyond that: Pensando's response on the three
bugs, and rebasing the transmit series onto net-next for submission.
