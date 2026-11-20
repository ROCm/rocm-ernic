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
