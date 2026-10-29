# AGENTS.md for the P4 engine

This adds to the repository's [AGENTS.md](../../AGENTS.md), which still
applies here.

`src/p4/` emulates real P4, so **do what the real P4 tools do**, not what
seems reasonable: p4c for what a program may contain, P4Runtime for what
a table entry may be (the update record is P4Runtime-shaped), and bmv2
for run-time behaviour. Check their source or spec before deciding: p4c's
`backends/bmv2/common/control.h` (at the version `scripts/fetch-p4c.sh`
pins), the P4Runtime spec's "Match Format" section, and bmv2's
`docs/JSON_format.md`. Where they disagree, p4c and P4Runtime win over
bmv2's looser spec (e.g. bmv2 allows a `ternary` table with no ternary
key; p4c never emits one, and the loader refuses it).

- **Refuse what they refuse, with `-EINVAL`**, rather than accepting it
  leniently: a value with bits past its LPM prefix or outside its mask, a
  priority where P4Runtime forbids one, a second LPM key. Check a delete
  like an insert.
- **Depart only where our representation differs**, and document it in
  `docs/p4.rst`. The record always carries every key field, so a "don't
  care" field is a zero prefix or mask, where P4Runtime omits it.
- **Compute a program's facts at load**, not at lookup (e.g.
  `p4_table.lpm_key`, not key 0), and add a case to
  `tests/data/p4/reject_cases.json` for each new load refusal.
- **Don't copy `p4_devcmd.c`'s wire handling.** It predates AGENTS.md's
  wire-format rule: its commands are naturally aligned structs,
  `memcpy()`'d in and out and converted with `<endian.h>`.
- Building `src/p4/` and its tests needs `-DERNIC_BUILD_P4=ON` (and
  json-c). `scripts/fetch-p4c.sh` puts the pinned p4c in a private
  directory, for compiling the programs under `tests/data/p4/` from
  their `.p4` sources.
