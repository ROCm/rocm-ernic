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
`runtime_data` parameters, and a conditional using `and`, `valid` and `>`. All
of that is inside the phase 1 subset.

It is a negative fixture for exactly one reason: `calculations` and `checksums`
are non-empty — the `csum16` IPv4 header checksum (`ipv4_checksum`, targeting
`ipv4.hdrChecksum`) — which phase 1 rejects. **It flips to a positive fixture
when checksum support lands**, so the test that asserts rejection should be
rewritten then rather than the file replaced.

It is also the fixture for signed bmv2 hexstrs: it contains `"-0x1"`, which
neither hexstr helper in `src/parse_int.h` accepts.

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
