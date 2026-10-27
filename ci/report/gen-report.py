#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# The hyphenated script name is not a valid module name. Enabling it again
# at once still covers the module's name, but no name inside it.
# pylint: disable=invalid-name
# pylint: enable=invalid-name

"""Turn a CI run's performance sweeps into a report.

Input:
  <results>/perf-csv/*.csv perftest / iperf3 / fio sweeps

Outputs:
  <out>/report.md          human-readable report
  <out>/summary.json       machine-readable rollup, which
                           publish-perf.py records

Usage:
  gen-report.py --results DIR --out DIR
"""

import argparse
import csv
import glob
import json
import os
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timezone

# ── CSV schema handling ───────────────────────────
#
# The perf plays emit several shapes.  Each entry maps
# a header tuple to the kind of measurement it holds.

SCHEMAS = {
    ("verb", "size", "bw_peak_GBs", "bw_avg_GBs", "msg_rate_mpps"): "bandwidth",
    (
        "verb",
        "run",
        "size",
        "bw_peak_GBs",
        "bw_avg_GBs",
        "msg_rate_mpps",
    ): "reliability",
    ("verb", "size", "lat_min_us", "lat_typical_us", "lat_max_us"): "latency",
    ("verb", "size", "lat_avg_us"): "latency",
    ("verb", "size", "elapsed_us", "iters", "bw_GBs"): "bandwidth",
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


# ── Performance results ───────────────────────────


# pylint: disable-next=too-many-locals
def load_perf(results_dir):
    """Load perf CSVs grouped by kind and metric."""
    # samples[kind][(verb, size, metric)] = [values]
    samples = defaultdict(lambda: defaultdict(list))
    files = 0

    pattern = os.path.join(results_dir, "perf-csv", "*.csv")
    for path in sorted(glob.glob(pattern)):
        with open(path, newline="", encoding="utf-8") as fh:
            reader = csv.reader(fh)
            try:
                header = next(reader)
            except StopIteration:
                continue
            kind = SCHEMAS.get(tuple(h.strip() for h in header))
            if kind is None:
                continue
            files += 1
            cols = [h.strip() for h in header]

            for row in reader:
                if len(row) != len(cols):
                    continue
                rec = dict(zip(cols, row))
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
                        samples[kind][(verb, size, col)].append(None)
                    else:
                        samples[kind][(verb, size, col)].append(val)
    return samples, files


def summarize(samples):
    """Reduce raw samples to per-metric statistics."""
    out = {}
    for kind, metrics in samples.items():
        rows = []
        for (verb, size, metric), values in sorted(
            metrics.items(), key=lambda kv: (kv[0][0], _size_key(kv[0][1]), kv[0][2])
        ):
            good = [v for v in values if v is not None]
            rows.append(
                {
                    "verb": verb,
                    "size": size,
                    "metric": metric,
                    "n": len(values),
                    "n_ok": len(good),
                    "mean": statistics.fmean(good) if good else None,
                    "median": statistics.median(good) if good else None,
                    "min": min(good) if good else None,
                    "max": max(good) if good else None,
                }
            )
        out[kind] = rows
    return out


def _size_key(size):
    try:
        return int(size)
    except (TypeError, ValueError):
        return 0


# ── Rendering ─────────────────────────────────────


def fmt(value, places=3):
    """Format a statistic for the report, or a dash if it is missing."""
    if value is None:
        return "—"
    return f"{value:.{places}f}"


def render(perf, meta):
    """Render the Markdown report."""
    lines = []
    add = lines.append

    add("# rocm-ernic CI report")
    add("")
    add(f"- **Run**: {meta['run_id']}")
    add(f"- **Commit**: `{meta['sha'][:12]}` ({meta['ref']})")
    add(f"- **Node**: {meta['node']}")
    add(f"- **Generated**: {meta['generated']}")
    add("")

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
        add("| Verb | Size | Metric | Median | Mean | Min | Max | Samples |")
        add("|---|---:|---|---:|---:|---:|---:|---:|")
        for r in rows:
            ok = f"{r['n_ok']}/{r['n']}" if r["n_ok"] != r["n"] else str(r["n"])
            add(
                f"| {r['verb']} | {r['size']} | "
                f"{r['metric']} | {fmt(r['median'])} | "
                f"{fmt(r['mean'])} | {fmt(r['min'])} | "
                f"{fmt(r['max'])} | {ok} |"
            )
        add("")

    return "\n".join(lines) + "\n"


def main():
    """Generate the reports; return the exit status."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    samples, csv_files = load_perf(args.results)
    perf = summarize(samples)

    meta = {
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "sha": os.environ.get("GITHUB_SHA", "unknown"),
        "ref": os.environ.get("GITHUB_REF_NAME", "unknown"),
        "node": os.environ.get("RUNNER_NAME", os.uname().nodename),
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC"),
    }

    report = render(perf, meta)
    report_path = os.path.join(args.out, "report.md")
    with open(report_path, "w", encoding="utf-8") as fh:
        fh.write(report)

    summary = {
        "meta": meta,
        "perf": perf,
        "perf_csv_files": csv_files,
    }
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)

    # Mirror into the GitHub step summary when present.
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as fh:
            fh.write(report)

    print(f"wrote {report_path}")
    print(f"perf: {csv_files} CSV file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
