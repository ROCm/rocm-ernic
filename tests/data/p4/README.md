# P4 bmv2 JSON test fixtures

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

Upstream artifacts used as fixtures for the bmv2 JSON loader in
`src/p4/p4_json.{c,h}`. Both are **verbatim** copies — do not edit them. If a
fixture needs to differ from upstream, add a derived file next to it and say
here what it changes and why.

## Provenance

Both files come from [p4lang/behavioral-model][bmv2], which is licensed
**Apache-2.0** (`LICENSE` at the repository root; GitHub reports the license as
`NOASSERTION` only because that file omits the boilerplate appendix it matches
on). The files carry no copyright or license header of their own, being
compiler output, so none was added.

Fetched at repository commit `c8d8a3f054ba108d668d2eb986188dbf38b9e938`.

| File | Upstream path | Blob SHA | Last upstream commit | Bytes |
| --- | --- | --- | --- | --- |
| `loopback.json` | `targets/simple_switch_grpc/tests/testdata/loopback.json` | `ab589388afa97c5b14d5b56a3ca58cab4fdad054` | `de8f08b02464c6008f5aeb4d1e9e7671d6b0feca` (2018-04-25) | 3964 |
| `simple_router.json` | `targets/simple_router/simple_router.json` | `c7c92c32443d9e8325f4860b45a21f07ac5601fb` | `2106979fb700792dca1a3aed7351899acae7a696` (2017-01-16) | 19328 |

`loopback.json` is the only `loopback.json` in the tree; it is under
`targets/`, not at `simple_switch_grpc/` as some references give it.

## Why each is here

### `loopback.json` — intended positive fixture

The smallest realistic p4c artifact in the repository: one parser with a single
`start` state and no parser ops, an empty deparser, one `assign` action over two
`field` parameters, and one `exact` table in `ingress` with an empty `egress`.
Every known-unsupported section is present and empty **except `errors`** — see
[Caveat](#caveat-errors-is-not-empty) below, which the loader has to resolve
before this file loads.

It also exercises the keys `docs/JSON_format.md` omits but p4c emits
(`program`, `force_arith`, per-primitive `source_info`, `pi_omit` on headers),
so it is the regression test for "unknown keys are ignored silently".

`__meta__.version` is `[2, 18]`.

### `simple_router.json` — negative fixture

A full IPv4 router: `extract` parser ops, `lpm` and `exact` tables,
`modify_field` / `add_to_field` / `drop` primitives with `field`, `hexstr` and
`runtime_data` parameters, and a conditional using `and`, `valid` and `>`.

It is a negative fixture for two reasons. `calculations` and `checksums` are
non-empty — the `csum16` IPv4 header checksum (`ipv4_checksum`, targeting
`ipv4.hdrChecksum`) — and are refused first. Without them, `set_nhop`'s TTL
decrement, `add_to_field` with the hexstr `"-0x1"`, is refused, because the
loader refuses negative literals. p4c emits one only for a signed `int<W>`
value, which is refused as a signed field anyway, and compiles an unsigned
`ttl - 1` to `(ttl + 0xff) & 0xff`. The test that asserts rejection checks for
the checksum.

`__meta__.version` is `[2, 0]`. It has no `field_aliases`, `errors`, `enums`,
`extern_instances` or `header_union*` keys at all, so it also covers the
"absent section" path.

## Caveat: `errors` is not empty

`loopback.json` carries six entries in `errors`:

```
[["NoError",1],["PacketTooShort",2],["NoMatch",3],
 ["StackOutOfBounds",4],["HeaderTooShort",5],["ParserTimeout",6]]
```

These are the standard v1model parser error codes that p4c emits into every
artifact from JSON version 2.18 onward; they are a name-to-ordinal table, not a
feature the pipeline uses. A loader that rejects a non-empty `errors` rejects
this file, and with it every modern p4c output. Treat `errors` as cosmetic and
ignore it, rather than listing it with the known-unsupported sections.

## Section inventory

Measured from the files, not from upstream documentation. "absent" means the
key is not present at all.

| Section | `loopback.json` | `simple_router.json` |
| --- | --- | --- |
| `header_stacks` | empty | empty |
| `header_union_types` | empty | absent |
| `header_unions` | empty | absent |
| `header_union_stacks` | empty | absent |
| `parse_vsets` | empty | empty |
| `meter_arrays` | empty | empty |
| `counter_arrays` | empty | empty |
| `register_arrays` | empty | empty |
| `learn_lists` | empty | empty |
| `extern_instances` | empty | absent |
| `checksums` | empty | **1 entry** |
| `calculations` | empty | **1 entry** |
| `enums` | empty | absent |
| `errors` | **6 entries** | absent |
| `field_lists` | empty | empty |

[bmv2]: https://github.com/p4lang/behavioral-model

## wide_field.json

Hand written, to pin down arithmetic and comparison on fields wider than a
machine word.

The program sets a 128-bit metadata field to 2^64 -- a value that is zero
in every bit below 64 -- and then tests it against zero. A true answer
sends the frame to a table that drops it; a false answer to one that
forwards it to port 1. An executor that read only the low 64 bits of a
field would see zero, answer true, and drop.

It is the discriminating case for that bug: both branches are reachable,
both are observable from outside, and the two differ only above bit 64.

## minimal_forward.json

**Compiled, not hand written.** The source is `src/minimal_forward.p4`
and the compiler is the one `scripts/fetch-p4c.sh` fetches:

```
scripts/fetch-p4c.sh
build-p4c/bin/p4c-bm2-ss --std p4-16 -I build-p4c/p4include \
    -o tests/data/p4/minimal_forward.json \
    tests/data/p4/src/minimal_forward.p4
```

Regenerate and diff after touching the loader, so the subset is held to
what p4c emits rather than to what the format documentation says.

One thing it pins that a hand-written fixture did not: p4c qualifies every
table and action name with the control it was declared in, so this program
has `IngressImpl.forward` and not `forward`. A control plane written
against unqualified names works on no real artifact at all.

## fp8_policy.json

**Compiled, not hand written**, from `src/fp8_policy.p4` with
`--emit-externs`:

```
build-p4c/bin/p4c-bm2-ss --std p4-16 --emit-externs \
    -I build-p4c/p4include \
    -o tests/data/p4/fp8_policy.json \
    tests/data/p4/src/fp8_policy.p4
```

`--emit-externs` is required: without it p4c refuses an extern it does not
already know, which is what makes a target-specific one compile at all.

This is the fixture that pins the extern JSON shape to what the compiler
emits rather than to what the format documentation describes. Two things
in it were not what the documentation led us to expect, and both would
have made a hand-written fixture wrong:

* p4c names a field of the user metadata struct after the struct, so the
  program carries `userMetadata.rdma_metadata`, not `rdma_metadata`.
* It marks that header `metadata: false`, because a `header`-typed field
  has a validity bit. The loader therefore checks that the deparser does
  not emit architecture state, which is the property actually wanted,
  rather than checking the metadata flag.

## fp8_convert.json

**Compiled, not hand written**, from `src/fp8_convert.p4` with the same
command. It is the other shape the `Fp8` extern has:
`cvt(src_fmt, dst_fmt)` rather than `map(op)`.

Where `fp8_policy.p4` reduces two equal-length operands into a result of
the same length, this one **converts**: it reads a single region and
writes the same number of elements in another format. The table entry
names both formats, so one program narrows or widens depending only on
what the control plane put in it -- the destination may be longer than
the source, shorter, or the same length.

The format numbers are the `enum p4_fmt` values in `src/p4/p4_fmt.h`:
`0` fp32, `1` fp16, `2` bf16, `3` fp8 E4M3, `4` fp8 E5M2, `5` fp4 E2M1.
The last is the OCP microscaling 4-bit format, packed two elements to a
byte, low nibble first; an odd element count is half a byte at one end
and is refused.

Which operation a work request asks for is its gather list, not anything
the program names: two equal entries is a reduction, one entry is a
conversion. A request that is neither has not asked for arithmetic at
all and is dispatched as the ordinary transfer it is.

It is the only operation whose destination is not the same length as its
source, which is why `ionic_datapath_fmt_convert()` is a separate entry
point rather than a selector on the map: checking one length against all
three regions, as the map does, would read the wrong amount at one end.

## fp8_nodrop.json

**Compiled, not hand written**, from `src/fp8_nodrop.p4` with the same
command. It is `fp8_policy.p4` with the `mark_to_drop()` left out of the
action that calls the extern, which is how one would plausibly write the
program on the first attempt.

It exists to be rejected. The extern writes the destination region itself,
so an operation that then also transfers would overwrite the result with
the raw payload; `ionic_datapath.c` refuses such an operation rather than
letting that happen quietly, and this is the program that makes it do so.

## fp8_drop_then_map.json

**Compiled, not hand written**, from `src/fp8_drop_then_map.p4` with the
same command. Its action calls `mark_to_drop()` and then `fp8.map()` --
the reverse of `fp8_policy.p4`'s order.

It exists to show that a drop made before the extern declines still
stands. A one-SGE RDMA write is not a reduction's shape, so `map()`
declines it and the pipeline stops at the call; the drop was already
made, so `ionic_datapath.c` completes the write without transferring
rather than dispatching what the program denied.

## skeleton.json

Hand written: the smallest complete program the loader accepts, with one
exact table, a `_drop` and a `set_etype` action, and p4c's
`standard_metadata`. Every case in `reject_cases.json` is this file with
one defect, and `p4-engine-unit` checks that it loads, so the defect is the
only thing that can refuse a case.

## reject_cases.json

Hand written: documents the loader must refuse, one defect each. A case is
an object with:

* `name`, `group`, and `why`, the defect and what it would do if loaded;
* `errno`, `EINVAL` or `ENOSYS`, and `error`, a piece of the reason the
  loader must give. The reason is what makes a case worth having: a case
  refused for some earlier defect never reaches the check it was written
  for. 39 once passed that way, refused only because the skeleton predated
  the loader's requirement of `standard_metadata`;
* `json`, the document. A case whose document must hold bytes no JSON
  string can (invalid UTF-8, a raw NUL) has `"encoding": "latin1"`: each
  code point of `json` is then one byte of the document.

Two kinds of document are deliberately not here. A parse state that loops
is accepted, as P4 allows, and bounded at run time instead; and of two
members with one name, json-c keeps the last. `p4-engine-unit` tests both
directly.

## features.json

Hand written, for what no compiled fixture uses: a parser that selects on
a field under a mask, on two fields at once (a 12-bit VLAN ID and an
EtherType, so each field's whole-byte slot in the key matters), with and
without a default arm, and through a state that loops; an LPM table and a
ternary table with priorities; `__HIT__`/`__MISS__` next tables; a default
entry with an argument; every arithmetic primitive in both of its forms;
and conditions using `valid`, `not`, `and`, `or`, a `bool`, and every
comparison.

The ingress pipeline:

* `c_ip`: an IPv4 frame goes to `route`; anything else to `c_l2`, which
  drops a tagged frame and lets an untagged one through unchanged.
* `route` (LPM on `ipv4.dstAddr`): `fwd(port)`, or `drop` on a miss.
* `acl` (ternary on `ipv4.srcAddr` and `ipv4.protocol`, 4 entries):
  `set_class(c)`, or `nop` on a miss, which goes straight to `c_ttl`.
* `c_class`: classes 5 to 7, and 11 to 31 but not 12, are dropped.
* `c_ttl`: a TTL of 0 or 1 is dropped; otherwise `t_dec` decrements it and
  changes `identification`, `diffserv` and `hdrChecksum` in ways a test can
  see.
