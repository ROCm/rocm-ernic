#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# gen-report.py -- turn a CI run's raw results into the
# functional and performance reports.
#
# Inputs (all optional; whatever is present is used):
#   <results>/*.jsonl        shell-level check results
#   <results>/junit/*.xml    Ansible junit callback output
#   <results>/perf-csv/*.csv perftest / iperf3 sweeps
#
# Outputs:
#   <out>/report.md          human-readable report
#   <out>/summary.json       machine-readable rollup
#
# Usage:
#   gen-report.py --results DIR --out DIR [--baseline FILE]
#
# Exit status is 1 if any functional check failed, or if
# a perf regression beyond --threshold is detected
# against --baseline.  That makes the script usable as
# the gating step of a workflow.

import argparse
import csv
import glob
import json
import os
import statistics
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from datetime import datetime, timezone

# ── CSV schema handling ───────────────────────────
#
# The perf plays emit several shapes.  Each entry maps
# a header tuple to (kind, metric columns).  "lower is
# better" metrics are listed in LOWER_IS_BETTER so
# regression comparisons get the sign right.

SCHEMAS = {
    ("verb", "size", "bw_peak_GBs", "bw_avg_GBs",
     "msg_rate_mpps"): "bandwidth",
    ("verb", "run", "size", "bw_peak_GBs", "bw_avg_GBs",
     "msg_rate_mpps"): "reliability",
    ("verb", "size", "lat_min_us", "lat_typical_us",
     "lat_max_us"): "latency",
    ("verb", "size", "lat_avg_us"): "latency",
    ("verb", "size", "elapsed_us", "iters",
     "bw_GBs"): "bandwidth",
    ("section", "test", "detail", "result",
     "value"): "stress",
}

LOWER_IS_BETTER = {
    "lat_min_us", "lat_typical_us", "lat_max_us",
    "lat_avg_us", "elapsed_us",
}

# Values the plays write into numeric columns when a
# measurement did not complete.
SENTINELS = {"FAIL", "ERROR", "N/A", "", "-"}


def to_float(value):
    """Parse a CSV cell, treating failure sentinels as None."""
    if value is None:
        return None
    v = value.strip()
    if v.upper() in SENTINELS:
        return None
    try:
        return float(v)
    except ValueError:
        return None


# ── Functional results ────────────────────────────

def load_jsonl(results_dir):
    """Load every shell-level check result."""
    checks = []
    for path in sorted(glob.glob(os.path.join(results_dir,
                                              "*.jsonl"))):
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    checks.append(json.loads(line))
                except json.JSONDecodeError:
                    # A job killed mid-write can leave a
                    # torn final line; the rest is still
                    # worth reporting.
                    continue
    return checks


def load_junit(results_dir):
    """Load Ansible junit callback results as checks."""
    checks = []
    pattern = os.path.join(results_dir, "junit", "*.xml")
    for path in sorted(glob.glob(pattern)):
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        suites = ([root] if root.tag == "testsuite"
                  else root.findall("testsuite"))
        for suite in suites:
            suite_name = suite.get("name") or "ansible"
            for case in suite.findall("testcase"):
                if case.find("skipped") is not None:
                    status = "skip"
                elif (case.find("failure") is not None
                      or case.find("error") is not None):
                    status = "fail"
                else:
                    status = "pass"
                detail = ""
                for tag in ("failure", "error"):
                    node = case.find(tag)
                    if node is not None:
                        detail = (node.get("message")
                                  or (node.text or ""))[:400]
                        break
                checks.append({
                    "suite": suite_name,
                    "name": case.get("name") or "?",
                    "status": status,
                    "duration_s": float(case.get("time")
                                        or 0.0),
                    "detail": detail,
                })
    return checks


# ── Performance results ───────────────────────────

def load_perf(results_dir):
    """Load perf CSVs grouped by kind and metric."""
    # samples[kind][(verb, size, metric)] = [values]
    samples = defaultdict(lambda: defaultdict(list))
    stress = []
    files = 0

    pattern = os.path.join(results_dir, "perf-csv", "*.csv")
    for path in sorted(glob.glob(pattern)):
        with open(path, newline="") as fh:
            reader = csv.reader(fh)
            try:
                header = next(reader)
            except StopIteration:
                continue
            kind = SCHEMAS.get(tuple(h.strip()
                                     for h in header))
            if kind is None:
                continue
            files += 1
            cols = [h.strip() for h in header]

            for row in reader:
                if len(row) != len(cols):
                    continue
                rec = dict(zip(cols, row))

                if kind == "stress":
                    stress.append(rec)
                    continue

                verb = rec.get("verb", "?")
                size = rec.get("size", "?")
                for col in cols:
                    if col in ("verb", "size", "run"):
                        continue
                    val = to_float(rec.get(col))
                    if val is None:
                        # Record the failed measurement so
                        # the report can show completeness
                        # rather than silently dropping it.
                        samples[kind][(verb, size,
                                       col)].append(None)
                    else:
                        samples[kind][(verb, size,
                                       col)].append(val)
    return samples, stress, files


def summarize(samples):
    """Reduce raw samples to per-metric statistics."""
    out = {}
    for kind, metrics in samples.items():
        rows = []
        for (verb, size, metric), values in sorted(
                metrics.items(),
                key=lambda kv: (kv[0][0],
                                _size_key(kv[0][1]),
                                kv[0][2])):
            good = [v for v in values if v is not None]
            rows.append({
                "verb": verb,
                "size": size,
                "metric": metric,
                "n": len(values),
                "n_ok": len(good),
                "mean": statistics.fmean(good) if good else None,
                "median": statistics.median(good) if good else None,
                "min": min(good) if good else None,
                "max": max(good) if good else None,
            })
        out[kind] = rows
    return out


def _size_key(size):
    try:
        return int(size)
    except (TypeError, ValueError):
        return 0


# ── Regression comparison ─────────────────────────

def compare(current, baseline, threshold):
    """Flag metrics that moved beyond threshold percent."""
    regressions = []
    base_index = {}
    for kind, rows in baseline.get("perf", {}).items():
        for row in rows:
            key = (kind, row["verb"], row["size"],
                   row["metric"])
            base_index[key] = row.get("median")

    for kind, rows in current.items():
        for row in rows:
            key = (kind, row["verb"], row["size"],
                   row["metric"])
            base = base_index.get(key)
            cur = row.get("median")
            if base is None or cur is None or base == 0:
                continue
            delta_pct = (cur - base) / abs(base) * 100.0
            metric = row["metric"]
            # Normalise so negative always means "worse".
            signed = (-delta_pct if metric in LOWER_IS_BETTER
                      else delta_pct)
            if signed < -threshold:
                regressions.append({
                    "kind": kind,
                    "verb": row["verb"],
                    "size": row["size"],
                    "metric": metric,
                    "baseline": base,
                    "current": cur,
                    "change_pct": delta_pct,
                })
    return regressions


# ── Rendering ─────────────────────────────────────

def fmt(value, places=3):
    if value is None:
        return "—"
    return f"{value:.{places}f}"


def render(checks, perf, stress, regressions, meta):
    total = len(checks)
    passed = sum(1 for c in checks if c["status"] == "pass")
    failed = sum(1 for c in checks if c["status"] == "fail")
    skipped = sum(1 for c in checks if c["status"] == "skip")

    lines = []
    add = lines.append

    add("# rocm-ernic CI report")
    add("")
    add(f"- **Run**: {meta['run_id']}")
    add(f"- **Commit**: `{meta['sha'][:12]}` "
        f"({meta['ref']})")
    add(f"- **Node**: {meta['node']}")
    add(f"- **Accelerator**: {meta['accel']}")
    add(f"- **Generated**: {meta['generated']}")
    add("")

    # ── Functional ────────────────────────────────
    add("## Functional results")
    add("")
    if total == 0:
        add("_No functional results recorded._")
    else:
        verdict = "PASS" if failed == 0 else "FAIL"
        add(f"**{verdict}** — {passed}/{total} passed, "
            f"{failed} failed, {skipped} skipped")
        add("")
        by_suite = defaultdict(list)
        for c in checks:
            by_suite[c["suite"]].append(c)
        for suite in sorted(by_suite):
            items = by_suite[suite]
            s_fail = sum(1 for c in items
                         if c["status"] == "fail")
            mark = "❌" if s_fail else "✅"
            add(f"### {mark} {suite} "
                f"({len(items) - s_fail}/{len(items)})")
            add("")
            add("| Check | Status | Time (s) |")
            add("|---|---|---|")
            for c in items:
                icon = {"pass": "✅", "fail": "❌",
                        "skip": "⏭️"}.get(c["status"], "?")
                add(f"| {c['name']} | {icon} "
                    f"{c['status']} | "
                    f"{c['duration_s']:.2f} |")
            add("")
            for c in items:
                if c["status"] == "fail" and c.get("detail"):
                    add(f"<details><summary>{c['name']} "
                        f"detail</summary>")
                    add("")
                    add("```")
                    add(str(c["detail"])[:1500])
                    add("```")
                    add("</details>")
                    add("")

    # ── Performance ───────────────────────────────
    add("## Performance results")
    add("")
    if not perf:
        add("_No performance data in this run._")
        add("")
    for kind in sorted(perf):
        rows = perf[kind]
        if not rows:
            continue
        add(f"### {kind.capitalize()}")
        add("")
        add("| Verb | Size | Metric | Median | Mean | "
            "Min | Max | Samples |")
        add("|---|---:|---|---:|---:|---:|---:|---:|")
        for r in rows:
            ok = (f"{r['n_ok']}/{r['n']}"
                  if r["n_ok"] != r["n"]
                  else str(r["n"]))
            add(f"| {r['verb']} | {r['size']} | "
                f"{r['metric']} | {fmt(r['median'])} | "
                f"{fmt(r['mean'])} | {fmt(r['min'])} | "
                f"{fmt(r['max'])} | {ok} |")
        add("")

    if stress:
        fails = [s for s in stress
                 if s.get("result", "").upper() == "FAIL"]
        add(f"### Stress ({len(stress)} records, "
            f"{len(fails)} failing)")
        add("")

    # ── Regressions ───────────────────────────────
    if regressions:
        add(f"## ⚠️ Performance regressions "
            f"({len(regressions)})")
        add("")
        add("| Kind | Verb | Size | Metric | Baseline | "
            "Current | Change |")
        add("|---|---|---:|---|---:|---:|---:|")
        for r in regressions:
            add(f"| {r['kind']} | {r['verb']} | "
                f"{r['size']} | {r['metric']} | "
                f"{fmt(r['baseline'])} | "
                f"{fmt(r['current'])} | "
                f"{r['change_pct']:+.1f}% |")
        add("")

    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baseline",
                    help="summary.json from a previous run")
    ap.add_argument("--threshold", type=float, default=15.0,
                    help="regression threshold in percent")
    ap.add_argument("--no-fail", action="store_true",
                    help="always exit 0")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    checks = load_jsonl(args.results)
    checks += load_junit(args.results)
    samples, stress, csv_files = load_perf(args.results)
    perf = summarize(samples)

    regressions = []
    if args.baseline and os.path.isfile(args.baseline):
        with open(args.baseline) as fh:
            baseline = json.load(fh)
        regressions = compare(perf, baseline, args.threshold)

    meta = {
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "sha": os.environ.get("GITHUB_SHA", "unknown"),
        "ref": os.environ.get("GITHUB_REF_NAME", "unknown"),
        "node": os.environ.get("RUNNER_NAME",
                               os.uname().nodename),
        "accel": os.environ.get("CI_VM_ACCEL", "n/a"),
        "generated": datetime.now(timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S UTC"),
    }

    report = render(checks, perf, stress, regressions, meta)
    report_path = os.path.join(args.out, "report.md")
    with open(report_path, "w") as fh:
        fh.write(report)

    failed = sum(1 for c in checks if c["status"] == "fail")
    summary = {
        "meta": meta,
        "functional": {
            "total": len(checks),
            "passed": sum(1 for c in checks
                          if c["status"] == "pass"),
            "failed": failed,
            "skipped": sum(1 for c in checks
                           if c["status"] == "skip"),
        },
        "perf": perf,
        "perf_csv_files": csv_files,
        "regressions": regressions,
    }
    with open(os.path.join(args.out, "summary.json"),
              "w") as fh:
        json.dump(summary, fh, indent=2)

    # Mirror into the GitHub step summary when present.
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a") as fh:
            fh.write(report)

    print(f"wrote {report_path}")
    print(f"functional: {summary['functional']['passed']}"
          f"/{summary['functional']['total']} passed, "
          f"{failed} failed")
    print(f"perf: {csv_files} CSV file(s), "
          f"{len(regressions)} regression(s)")

    if args.no_fail:
        return 0
    return 1 if (failed or regressions) else 0


if __name__ == "__main__":
    sys.exit(main())
