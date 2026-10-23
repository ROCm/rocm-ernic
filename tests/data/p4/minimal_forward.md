# `minimal_forward.json`

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

Hand-written, not `p4c` output. It is the **positive fixture** for the bmv2
JSON loader in `src/p4/p4_json.{c,h}`: the smallest program that touches every
section of the phase 1 subset and nothing outside it. Unlike `loopback.json`
and `simple_router.json` in this directory, this file is ours and may be
edited — but see [Invariants](#invariants-a-test-should-pin) before you do.

The top-level key set is **identical** to `loopback.json`'s, in the same order,
so a loader that walks the root object the same way for both files sees the
same shape.

## What the program does

One Ethernet header, parsed, matched on the destination MAC, and sent to a
port chosen by the control plane.

### Behaviour of a frame, precisely

A frame arrives on `standard_metadata.ingress_port`, which for rocm-ernic is
`P4_PORT_HOST` (0) when the guest transmitted it and `P4_PORT_UPLINK` (1) when
it came off the TAP (see `docs/p4.rst`).

1. The parser runs its only state, `start`. It extracts 14 bytes into the
   `ethernet` instance, then takes its `default` transition to `next_state:
   null`, which is bmv2's spelling of `accept`. A frame shorter than 14 bytes
   fails the extract and is a parser error; everything after the 14th byte is
   payload and is untouched.
2. The ingress pipeline starts at `init_table: "forward"` and applies it,
   looking up the 48-bit `ethernet.dstAddr` as an exact key.
3. **On a hit**, the entry's action runs: `set_egress` writes its 9-bit
   runtime parameter into `standard_metadata.egress_spec`, or `drop_pkt`
   drops.
4. **On a miss**, the table's `default_entry` runs, which is `action_id: 1` —
   `drop_pkt` — with no action data. The frame is dropped.
5. Either way `next_tables` maps both actions to `null` and
   `base_default_next` is `null`, so the ingress pipeline ends after this one
   table.
6. The egress pipeline is present with `init_table: null` and no tables, so it
   is a no-op.
7. The deparser emits `ethernet` (the only entry in `order`) and then the
   payload. No field was modified, so the frame leaves byte-for-byte as it
   arrived, on whichever port `egress_spec` names.

### The table is empty when loaded, so everything is dropped

**As loaded, this program drops every frame.** `forward` has no `entries` key,
so it starts empty and every lookup misses and takes the `drop_pkt` default.
That is what the task specification asked for (`default action drop_pkt`), and
it is the right default for a fixture — but it means the file alone does not
demonstrate forwarding. Forwarding needs one control-plane entry, which is the
point: this fixture is also the first thing the table-insert path will be
tested against.

The entry that makes a frame to `02:00:00:00:00:01` leave on the uplink port:

| Field | Value |
| --- | --- |
| table | `forward` |
| match | `exact`, key `0x020000000001` (6 bytes, the full 48 bits) |
| action | `set_egress` (`action_id` 0) |
| action data | `["0x001"]` — 9 bits, byte-padded to 2 bytes |

With that entry installed, a frame entering on `P4_PORT_HOST` (0) addressed to
`02:00:00:00:00:01` leaves on `P4_PORT_UPLINK` (1), unmodified. Any other
destination MAC misses and is dropped.

Note that a 9-bit value is byte-padded to 2 bytes in a bmv2 hexstring, per the
padding rule in `docs/JSON_format.md`. `egress_spec` is 9 bits because that is
v1model's width, not because the engine has 512 ports; rocm-ernic uses only 0
and 1 and the loader should range-check anything it is given.

## Section by section, and what each exercises

### `program` (ignored)

`"minimal_forward.p4"`. There is no such `.p4` file. The key is here because
`p4c` emits it and `docs/JSON_format.md` does not document it. Together with
`force_arith` and the table's `action_ids` it is this file's coverage of
**"unknown and cosmetic keys are ignored silently"** — the rule without which
no real artifact loads.

### `__meta__`

`version: [2, 24]`, the version `docs/JSON_format.md` currently describes.
Exercises the version check's accept path at the documented major 2 and a
minor well inside `0..255`. `compiler` is optional and is set to a string that
does not claim to be `p4c`, so nothing downstream mistakes this for compiler
output.

### `header_types`

Two entries, ids 0 and 1, both using the 3-tuple field form
`[name, bitwidth, signed]` that modern `p4c` emits (the 2-tuple form is also
legal; this file does not cover it).

- `standard_metadata` (id 0) — copied field-for-field from `loopback.json` and
  `packet_redirect.json`, so it is a real v1model metadata layout rather than
  an invention, plus `parser_error`, which `packet_redirect.json` carries and
  `loopback.json` does not. 176 bits, which is byte-aligned, with `_padding`
  doing that job. Note the **header type is named `standard_metadata`, same as
  the instance** — that is what `p4c` does, and a loader that assumes type and
  instance names live in one namespace will collide here. That collision is
  deliberate coverage.
- `ethernet_t` (id 1) — `dstAddr(48)`, `srcAddr(48)`, `etherType(16)`. 112
  bits, 14 bytes, the real Ethernet II header.

No variable-length field, so no `length_exp` or `max_length`: the loader never
has to deal with `"*"` widths here.

### `headers`

Two instances, ids 0 and 1.

- `standard_metadata` (id 0), `metadata: true`. This is the **required**
  coverage of "metadata is a header instance": `egress_spec` is reached as a
  normal field reference `["standard_metadata", "egress_spec"]`, not through
  any special case. It also carries `pi_omit: true`, another undocumented
  `p4c` key, ignored.
- `ethernet` (id 1), `metadata: false`.

### Known-unsupported sections, all present and empty

`header_stacks`, `header_union_types`, `header_unions`,
`header_union_stacks`, `field_lists`, `errors`, `enums`, `parse_vsets`,
`meter_arrays`, `counter_arrays`, `register_arrays`, `calculations`,
`checksums`, `learn_lists`, `extern_instances` — every one is `[]`.

This is the whole point of the file for the reject table: it exercises the
**"present but empty is fine, ignore it"** path for all fifteen at once. The
negative cases (non-empty `calculations` and `checksums`) are
`simple_router.json`'s job, and the absent-key path is also
`simple_router.json`'s.

`errors` is `[]` here even though `loopback.json` carries six entries and
`README.md` argues `errors` should be treated as cosmetic rather than
known-unsupported. Empty satisfies both readings, so this file does not decide
that question.

### `parsers`

Exactly one parser (the subset requires exactly one), `init_state: "start"`,
one state.

- `parser_ops`: one `extract` with a single parameter of type `regular`, value
  `"ethernet"` — the only extract form in the subset. Covers the
  `regular`-only restriction positively; `stack` and `union_stack` are
  unreachable here because there are no stacks.
- `transition_key`: `[]`, so there is nothing to match on and the only legal
  transition is the default. This also means the loader's byte-padding
  arithmetic for transition values is not exercised by this file.
- `transitions`: one entry, `{"type": "default", "value": null, "mask": null,
  "next_state": null}`. `next_state: null` is `accept`.

**This uses the documented transition encoding, which `loopback.json` does
not.** See [Format uncertainties](#format-uncertainties) below.

The subset also supports the `set` parser op from `field` and from `hexstr`.
**This file does not exercise `set`.** If you want coverage of it, add a
second fixture rather than complicating this one.

### `deparsers`

Exactly one, `order: ["ethernet"]`. Contrast `loopback.json`, whose deparser
`order` is `[]`; this file is the one that proves a non-empty order is read
and that the name in it resolves to a header instance.

### `actions`

Ids 0 and 1, dense and unique. Both ids appear in the table's `action_ids` and
one appears in `default_entry.action_id`, so the loader's action-id resolution
has something to resolve.

- `set_egress` (id 0) — one `runtime_data` entry, `{"name": "port",
  "bitwidth": 9}`, and one `assign` primitive whose parameters are a `field`
  and a `runtime_data` with `value: 0`. That `0` is an **index into the
  action's own `runtime_data` array**, not a value; a loader that reads it as
  a constant will silently set `egress_spec` to zero and forward everything to
  the host port. This is the only `runtime_data` reference in the file and the
  only index the loader has to bounds-check (valid range here is `[0, 1)`).
- `drop_pkt` (id 1) — empty `runtime_data`, one `drop` primitive with
  `parameters: []`.

Between them these cover two of the subset's primitives (`assign`, `drop`) and
two of its three parameter types (`field`, `runtime_data`). **`hexstr`
parameters are not exercised here**; `simple_router.json` has them, including
the negative `"-0x1"`.

`assign` is the spelling modern `p4c` emits; `modify_field` is the legacy
alias for the same thing and is what `simple_router.json` uses. `drop` takes
no parameters in bmv2 (`ActionPrimitive<>`); `mark_to_drop` is a *different*
primitive taking a header reference, and is not used here — see
[Format uncertainties](#format-uncertainties).

### `pipelines`

Two, ids 0 and 1, named `ingress` and `egress`. `ingress` is required by the
subset, `egress` is optional and is present-but-empty, which is the shape
`loopback.json`, `truncate.json` and `parser_error.json` all have for a
control with nothing in it: `init_table: null`, `tables: []`,
`action_profiles: []`, `conditionals: []`.

The `forward` table:

| Key | Value | Why |
| --- | --- | --- |
| `id` | 0 | unique across *all* tables in the file, not per pipeline |
| `key` | one `exact` element on `["ethernet", "dstAddr"]`, `mask: null` | the subset's `exact` path; `name` is the optional display name |
| `match_type` | `"exact"` | must agree with the key elements |
| `type` | `"simple"` | the only type in the subset |
| `max_size` | 1024 | same as `loopback.json`; the loader's per-table cap applies here |
| `with_counters` | `false` | must be false in the subset |
| `support_timeout` | `false` | must be false in the subset |
| `direct_meters` | `null` | must be null in the subset |
| `action_ids` | `[0, 1]` | undocumented `p4c` key, ignored by us |
| `actions` | `["set_egress", "drop_pkt"]` | order does not matter |
| `base_default_next` | `null` | the **miss target**: null means end of pipeline |
| `next_tables` | keyed by action name, both `null` | the action-name form, never mixed with `__HIT__`/`__MISS__` |
| `default_entry` | `action_id: 1`, no data, neither const | the miss action, `drop_pkt` |

`base_default_next` is the load-bearing undocumented key. Here it is `null`,
which is the end of the pipeline, so dropping it would happen to be harmless
*in this file* — which is exactly why this file is a weak test of it. A test
that pins `base_default_next` needs a two-table program.

`next_tables` uses the **action-name** form. A fixture for the
`__HIT__`/`__MISS__` form, and a negative fixture mixing the two, are not in
this file.

**There is no `entries` key, deliberately.** See
[Format uncertainties](#format-uncertainties).

### `field_aliases`

`[]`. The subset parses this section; this file covers only the empty case.
`parser_error.json` upstream has eleven real aliases if a non-empty fixture is
wanted.

## Invariants a test should pin

If you edit this file, these are the things a loader test is likely to be
asserting on:

- top-level key set and order identical to `loopback.json`
- `__meta__.version == [2, 24]`
- every one of the fifteen known-unsupported sections present and `[]`
- `forward.base_default_next == null` and `default_entry.action_id == 1`
- `set_egress.runtime_data` has exactly one element, so index `0` is the only
  valid `runtime_data` reference
- no `entries` key on `forward`

## Format uncertainties

Places where the bmv2 format is ambiguous or where two real artifacts
disagree, resolved here by reading `P4Objects.cpp` rather than guessed.

1. **`entries: []` is not harmless, so it is omitted.** The subset says a
   table's `entries` must be empty. In bmv2, the *presence* of the key — even
   as an empty array — sets `set_immutable_entries()` on the table, and
   `get_table_size()` prefers `entries.size()` over `max_size` when both are
   present, giving a table of size 0 that the control plane can never write
   to. `p4c` only emits `entries` for tables with `const entries`. So a table
   with `"entries": []` is a permanently empty table, not an empty-but-usable
   one, and this fixture omits the key. **The subset rule should probably be
   "`entries` must be absent", not "must be empty"** — worth deciding before
   the loader is written, because "empty is fine" here means something
   different from what it means for the fifteen unsupported sections.
2. **Two incompatible encodings of a parser transition exist in the wild.**
   `docs/JSON_format.md` and `simple_router.json` use
   `{"type": "default", "value": null, ...}`. `loopback.json` — the file next
   to this one — uses `{"value": "default", ...}` with **no `type` key at
   all**. bmv2 handles both: it defaults a missing `type` to `"hexstr"` and
   then re-reads `value == "default"` as the default transition. This file
   uses the documented form, so **the loader still needs the `loopback.json`
   form to load `loopback.json`**, and this fixture will not catch its
   absence.
3. **`drop` vs `mark_to_drop`.** The subset allow-lists both and restricts
   parameter types to `field`/`hexstr`/`runtime_data`. But in bmv2
   `mark_to_drop` is `ActionPrimitive<Header &>` — its one parameter is of
   type `header`, which is *outside* the allowed parameter types. `drop` is
   `ActionPrimitive<>` and takes none. This file uses `drop` with
   `"parameters": []`, so it sidesteps the problem. **As the subset is
   written, no `mark_to_drop` call can load**, since any real one has a
   `header` parameter. Either `header` needs adding to the allowed parameter
   types for that one primitive, or `mark_to_drop` should come off the
   allow-list.
4. **The header type and the header instance share the name
   `standard_metadata`.** This is what `p4c` emits and what `loopback.json`
   contains, so it is not a mistake in this file. If the loader keys types and
   instances in one table it will break on every real artifact.
5. **`parser_error` in `standard_metadata`.** `loopback.json` omits it,
   `packet_redirect.json` has it. Both are real; the field set of
   `standard_metadata` is not fixed by the format and the loader must not
   assume a particular one. This file includes it, which keeps the header
   byte-aligned at 176 bits.
6. **Not covered by this file**, and needing their own fixtures: the `set`
   parser op, `hexstr` action parameters, `lpm` and `ternary` match types, the
   conditional expression grammar, `__HIT__`/`__MISS__` `next_tables`, a
   non-null `base_default_next`, non-empty `field_aliases`, and the 2-tuple
   header-field form.
