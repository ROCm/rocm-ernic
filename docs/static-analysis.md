# rocm-ernic static-analysis report

> **Snapshot.** This report reflects the tree at commit `f7dda29` (see
> **Provenance**), before the fixes for these findings were merged. Every
> issue below marked *Live* except **S3** has since been fixed on `main` —
> D1 by #62, D2 by #63, D3 by #64, D4 by #63/#64, S1 by #65, S2 by #66 and
> S4 by #67. S3 remains open as #68. Re-run `nix build .#analysis-deep` for
> the current state rather than relying on the statuses below.

## What this is

A triaged report of the security- and stability-relevant findings from a full **static-analysis** sweep of the rocm-ernic userspace target. It leads with the issues worth acting on — each validated by reading the source at the commit below — and keeps the long tail of style/convention findings in an appendix.

Two memory-safety bugs found by the project's **dynamic** (fuzz / sanitizer) pass are restated here for a complete picture and clearly labelled as such; the static tools do not independently catch them (fixed-count reads and lifetime/teardown leaks defeat static inference), a useful reminder that the static and dynamic passes cover different ground.

## How to reproduce

Everything here comes from the Nix analysis toolkit in this repo. From the repo root:

```
nix build .#analysis-deep            # all 7 static tools + triage
cat result/summary.txt               # per-tool counts
cat result/triage/high-confidence.txt
cat result/triage/cross-ref.txt      # multi-tool correlations
cat result/clang-analyzer/report.txt
cat result/gcc-analyzer/report.txt
```

Run a single tool instead of the whole sweep:

```
nix build .#analysis-clang-tidy      # also: -cppcheck, -flawfinder, -semgrep,
nix build .#analysis-clang-analyzer  #       -gcc-warnings, -gcc-analyzer
```

Each per-tool target writes `result/<tool>/report.*` and `result/<tool>/count.txt`; `report.xml` (cppcheck) and `report.json` (semgrep) are the machine-readable forms, and `result/clang-analyzer/html-report/` is the browsable scan-build tree. The `quick` / `standard` / `deep` aggregates layer more tools on and re-run the triage pass (score + deduplicate + cross-reference). The dynamic passes (`.#analysis-sanitizers`, `.#analysis-tsan`, `.#analysis-valgrind`) and the fuzzers (`.#fuzz`, `.#fuzz-run`) are documented in `nix/README.md`.

## Tools & totals

`nix build .#analysis-deep` ran seven tools over the CMake compilation database and the raw sources, then triaged the results:

| Tool | Findings | What it looks for |
|---|---|---|
| clang-tidy | 3788 | broad C lint (bugprone-, cert-, readability-, misc-) |
| cppcheck | 203 | correctness, undefined behaviour, dead code |
| flawfinder | 140 | dangerous API usage patterns |
| clang-analyzer (scan-build) | 3 | path-sensitive null-deref / uninit |
| gcc-warnings | 4 | `-Wall -Wextra -Wpedantic -Wformat=2 …` |
| gcc-analyzer (`-fanalyzer`) | 1 | path-sensitive null-argument |
| semgrep | 124 | custom C hygiene rules |

Triage flagged **184 high-confidence** findings out of **2310** in-scope (across 79 categories); this report is the security/stability subset of those, after manual validation.

## Scope

The **userspace** build only (`src/`). The kernel driver and the rdma-core provider are out of scope. Both our own code (`src/rocm_ernic_*`) and the `src/from-qemu/**` tree are in scope and treated as security-sensitive, because that tree holds the wire-protocol parsers (DHCP, rdma_cm, TCP, Ethernet RX) that handle untrusted input.

**A note on `src/from-qemu/`:** despite the directory name, these files are rocm-ernic's own code (Copyright 2025 AMD) — the minimal DHCP server, the loopback CM stub, the TCP mesh backend, and the PVRDMA device model. They are not code carried from upstream QEMU, so findings here are ours to fix, not to forward upstream.

## Executive summary

Eight distinct security/stability issues survived validation; one further issue that earlier sweeps flagged is **already fixed in this tree** and is listed for continuity. Status is judged purely against the current source — no external issue/PR tracking.

| # | Issue | File | Severity | Found by | Status |
|---|---|---|---|---|---|
| D1 | OOB read in rdma_cm debug hex-dump | `rdma_cm_proto.c:114` | High | fuzz | Live |
| D2 | OOB read on short DHCP request | `dhcp_server.c:197` | High | fuzz | Live |
| S1 | NULL-deref in TCP/UDP checksum on empty payload | `net_headers.h:308` | Medium | clang-analyzer + cppcheck | Live |
| — | NULL-deref in verbs `query_port` fallback | `rdma_backend.c:345` | Medium | clang-analyzer | **Fixed in tree** |
| S2 | Signed-shift-by-31 undefined behaviour | `rocm_ernic_eth.h:38` + uses | Low | cppcheck | Live |
| S3 | Dead-code guards in loopback SGE copy | `rdma_backend_loopback.c:380` | Low | cppcheck | Live |
| D3 | DHCP server leaked at shutdown | `rocm_ernic_compat.c:251` | Low | sanitizer | Live |
| D4 | Leased-IP leak (NULL value destructor) | `dhcp_server.c:46` | Low | sanitizer | Live |
| S4 | Unchecked I/O & unvalidated parsing (theme) | many | Low | clang-tidy / semgrep | Live |

Four notable tool reports were validated as **false positives** and are documented, with reasons, in Appendix A — they need no code change.

## Static-analysis findings

### S1. NULL-deref in TCP/UDP checksum on an empty payload (Medium)

`src/from-qemu/utils/net_headers.h:308` (`tcp_checksum`) and the identical pattern in `udp_checksum` (line 255). Cross-tool: **clang-analyzer** `core.NullDereference` flags `net_headers.h:308` and `:313`, and **cppcheck** `nullPointerRedundantCheck` flags `net_headers.h:313`.

The checksum helpers dereference `payload` without a null guard:

```c
const uint16_t *payload_words = (const uint16_t *)payload;
for (i = 0; i < payload_len / 2; i++)
    sum += ntohs(payload_words[i]);
if (payload_len % 2)
    sum += ((uint8_t *)payload)[payload_len - 1] << 8;
```

The caller in `tcp_conn.c` guards the payload **copy** with `if (payload && payload_len > 0)` (line 269) but calls `tcp_checksum(ip_hdr, tcp_hdr, payload, payload_len)` **unconditionally** (line 276). A caller passing `payload == NULL` with `payload_len > 0` therefore dereferences NULL inside the loop. Today's in-tree callers keep `payload` and `payload_len` consistent, so this is a latent defensive-hardening bug rather than a live crash — but two independent tools agree on the site, and the fix is trivial.

**Fix:** guard the payload accumulation with `if (payload)` (or treat a NULL payload as `payload_len = 0`) in both `tcp_checksum` and `udp_checksum`.

### S2. Signed-shift-by-31 undefined behaviour (Low)

`src/rocm_ernic_eth.h:38` (root) and the use sites flagged by **cppcheck** `shiftTooManyBitsSigned` (error severity, 8 sites): `src/from-qemu/hw/rdma/vmw/pvrdma_eth.c:125,132`, `pvrdma_main.c:917,929,949,970,989`, and the imported uAPI header `include/qemu-extra/standard-headers/drivers/infiniband/hw/vmw_pvrdma/pvrdma_dev_api.h:360`.

Our own reset-bit macro is defined as a signed shift:

```c
#define ROCM_ERNIC_ETH_CTL_RESET     (1 << 31) /* Software Reset */
```

`1 << 31` shifts into the sign bit of a signed `int`, which is undefined behaviour in C. It works on today's compilers/targets but is a real portability/correctness defect. The `pvrdma_*` use sites are the same class, rooted in mask macros from the imported PVRDMA uAPI headers.

**Fix:** make the shift unsigned — `(1U << 31)` — in `ROCM_ERNIC_ETH_CTL_RESET` (ours to fix directly) and, where practical, in the uAPI mask definitions.

### S3. Dead-code guards in the loopback SGE copy (Low)

`src/from-qemu/hw/rdma/rdma_backend_loopback.c:380` and the mirror sites `:520`, `:617` — **cppcheck** `oppositeInnerCondition` ("opposite inner condition leads to a dead code block", 3 sites).

The SGE-copy loop is bounded by `while (src_idx < num_src_sge && …)`, then re-checks `if (src_idx >= num_src_sge) break;` without `src_idx` having changed since the loop condition — so the inner guard can never fire. Harmless at runtime, but it signals confused control flow and should be removed or corrected (e.g. if the intent was to re-check after an increment).

**Fix:** delete the redundant guards, or move the index increment so the re-check is meaningful.

### S4. Unchecked I/O and unvalidated parsing — a hardening theme (Low)

Not a single bug but a recurring pattern worth a pass:

- `cert-err33-c` — **146** findings (clang-tidy): return values of functions like `snprintf`, `write`, and `read` used without checking.
- `nix.store.getenv-unchecked` (3) and `nix.store.atoi-atol-usage` (5) / `integer.atoi` (5, flawfinder): `getenv()` results used without a NULL check and `atoi()` used without error handling, notably in the `rdma_backend_tcp.c` environment parsing.
- `cert-err34-c` (6) and `nix.store.unsafe-sprintf` (4): unchecked string-to-number conversions and unbounded `sprintf` targets.

Individually low-risk, but tightening the network- and config-parsing paths (checking short writes, validating parsed integers) would remove a class of silent-failure and misconfiguration bugs. Best handled as a focused cleanup rather than one-off fixes.

## Memory-safety findings from the dynamic / fuzz pass

These were found by the fuzz harnesses in `nix/analysis/fuzz/` and the sanitizer builds (`.#analysis-sanitizers`), **not** by the static tools. They are restated here because they are live in the code as it stands and are the highest-severity issues in the target.

### D1. Out-of-bounds read in the rdma_cm debug hex-dump (High)

`src/from-qemu/utils/rdma_cm_proto.c:114` — reachable from untrusted wire data (loopback / TCP-mesh CM path).

`rdma_cm_process_message()` unconditionally reads the first 32 bytes of the payload for a debug log once `payload_len >= 4` (payloads below 4 bytes take an early `return` above):

```c
rdma_info_report("rdma_cm: Message bytes (first 32): …",
    ((const uint8_t *)tcp_payload)[0], … ((const uint8_t *)tcp_payload)[31]);
```

For a payload of 4..31 bytes this reads past the end of the buffer, crashing the server or leaking adjacent memory into the log. Found by `fuzz/fuzz_rdma_cm_proto.c`.

**Fix:** bound the dump to `min(payload_len, 32)` and format with `snprintf`.

### D2. Out-of-bounds read on short DHCP requests (High)

`src/from-qemu/utils/dhcp_server.c` (`dhcp_server_process`, line 197) — reachable from untrusted DHCP packets on the loopback / TCP-manager path.

`dhcp_server_process()` and `dhcp_find_option()` treat the request as a full, fixed-size `struct dhcp_packet` (including the 312-byte `options` array) but never check `request_len` — the up-front guard (line 206) only validates the **response** buffer size, then the code reads `request->options[0..7]` unconditionally (line 217). A request shorter than the struct, down to zero bytes, is read past its end — first in the options debug dump, then in the option scan. Found by `fuzz/fuzz_dhcp_server.c`.

**Fix:** reject `request_len < sizeof(struct dhcp_packet)` up front.

### D3. DHCP server never freed at shutdown (Low)

`src/rocm_ernic_compat.c:251` (`pvrdma_device_destroy`).

`pvrdma_device_realize()` creates a `DhcpServer` (loopback and TCP-manager modes), but `pvrdma_device_destroy()` frees every other member and then `free(pvrdma)` (line 299) without ever freeing `pvrdma->dhcp_server`. The whole server leaks at teardown. Found by the LSan lifecycle exercise.

**Fix:** call `dhcp_server_destroy()` in the teardown path.

### D4. Leased IPs never freed (Low)

`src/from-qemu/utils/dhcp_server.c:46`.

`dhcp_server_create()` builds the `allocations` table with a NULL value destructor:

```c
server->allocations = g_hash_table_new_full(mac_hash, mac_equal, g_free, NULL);
server->leases      = g_hash_table_new_full(mac_hash, mac_equal, g_free, g_free);
```

so the heap-allocated IP copy stored for each allocation (inserted at line 173) leaks even when the table is destroyed — note the `leases` table right below already uses `g_free` for its value.

**Fix:** use `g_free` for the `allocations` value too (one line, pairs naturally with D3 in a single lifecycle test).

## Already fixed in this tree

### NULL-deref in the verbs `query_port` fallback (Medium) — fixed

`src/from-qemu/hw/rdma/rdma_backend.c:345` (`rdma_backend_query_port`). Earlier sweeps reported a `core.NullDereference`: the verbs fallback dereferenced `backend_dev->context` even though the function's own top-of-body check establishes `backend_dev` may be NULL. The current tree adds an early guard:

```c
if (!backend_dev) {
    return -EINVAL;
}
```

so the fallback can no longer be reached with a NULL `backend_dev`, and clang-analyzer no longer flags the site in this sweep. Recorded here so the delta from earlier reports is explicit.

## Appendix A — validated false positives

These tool reports were checked against the source and need **no** code change:

- **`rocm_ernic_server.c:668` — `parse_verbs_options` NULL `backend_str`** (gcc-analyzer `-Wanalyzer-null-argument`, CWE-476). Not reachable: the only caller (`validate_backend_options`, line 755) first calls `get_backend_type_base()`, which maps a NULL string to `"none"`, so `is_verbs` is false and `parse_verbs_options()` is never entered with NULL. (Adding a defensive null-check to match its sibling would be cheap but is not required.)
- **`rdma_backend_tcp.c:2167` — uninitialized `client_addr.sin_addr`** (clang-analyzer `core.CallAndMessage`, "passed-by-value struct contains uninitialized data"). False positive: `client_addr` is filled by a successful `accept()` (line 2136), and the code `continue`s on `sockfd < 0` before reaching `inet_ntoa()`. The analyzer cannot model the kernel writing through the `accept()` pointer.
- **`rdma_backend.c:501,1385,1407` — `memcpy` with `sizeof(pointer)`** (semgrep `nix.store.memcpy-sizeof-pointer`). False positive: the `sizeof` operand is a struct member (`msg.hdr.sgid`, a 16-byte `union ibv_gid`), not a pointer, and equals the size of the `.raw` array being copied.
- **`rdma_backend_tcp.c:3237` — duplicate `if (dgid)`** (cppcheck `duplicateCondition`). Two consecutive `if (dgid)` blocks (lines 3221, 3237) guarding separate logical steps; benign, though they could be merged.

The clang-analyzer run also emits a handful of `-Wgnu-zero-variadic-macro-arguments` / `-Wformat-nonliteral` / `-Wzero-length-array` extension warnings from the imported QEMU-compat headers and rejects some `qatomic` operations on `_Atomic` members (`pvrdma_qp_ops.c`) that its front-end models too strictly; none are counted as bug reports (`count.txt` = 3) and none require a change.

## Appendix B — lower-priority findings (style / convention)

The bulk of the 2310 in-scope findings are style and convention lint, not security or stability defects. Grouped by volume:

| Category | Count | Tool | Nature |
|---|---|---|---|
| misc-include-cleaner | 449 | clang-tidy | unused/missing includes |
| readability-magic-numbers | 429 | clang-tidy | literals should be named constants |
| readability-identifier-length | 352 | clang-tidy | short identifier names |
| cert-err33-c | 146 | clang-tidy | unchecked return values (see S4) |
| misc-unused-parameters | 90 | clang-tidy | unused function parameters |
| bugprone-easily-swappable-parameters | 84 | clang-tidy | adjacent same-type params |
| nix.store.fprintf-stderr | 74 | semgrep | direct `fprintf(stderr, …)` logging |
| buffer.memcpy | 62 | flawfinder | `memcpy` usage (informational) |
| constParameterCallback | 54 | cppcheck | missing `const` on callback params |
| readability-braces-around-statements | 53 | clang-tidy | brace style |
| readability-isolate-declaration | 47 | clang-tidy | one declaration per statement |
| constVariablePointer | 36 | cppcheck | missing `const` |
| nix.store.strerror-thread-unsafe | 22 | semgrep | `strerror()` vs `strerror_r()` |

These are best treated as a formatting/cleanup backlog (many are auto-fixable via clang-tidy `--fix`), not as part of the security work.

## Provenance

- Tree: branch `docs/static-analysis` (stacked on `feat/nix`), commit `f7dda29`.
- nixpkgs: `nixos-unstable`, rev `56c02bc00adcf003215cc4bd996d6efaf4cff188`.
- Analysis derivation: `/nix/store/il732msfmm9rkcc2imbm85n74xarbrwn-rocm-ernic-analysis-deep`.

Regenerate with `nix build .#analysis-deep` (see **How to reproduce** above); the counts here are read straight from `result/summary.txt` and `result/triage/`.
