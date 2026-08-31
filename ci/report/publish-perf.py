#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# publish-perf.py
#
# Append one run's performance medians to the tracked
# history, then regenerate the Sphinx page that charts it.
#
#   publish-perf.py --summary  $CI_WORK/report/summary.json \
#                   --docs-dir docs
#
# History lives in docs/perf-history/history.jsonl, one JSON
# object per run.  The page is docs/perf-trends.rst, which
# embeds the charts as inline SVG: no JavaScript, no build
# dependency, and it renders wherever the docs render.
#
# Only trusted runs should call this.  A pull request's
# numbers must never enter the history, or the published
# trend stops describing main.  The workflow enforces that;
# this script does not check it.

import argparse
import json
import pathlib
import sys
import textwrap

# Series to track over time.  Kept to three per chart: the
# reference palette validates its first three slots against
# every pair in both light and dark modes, and a fourth puts
# yellow next to orange, which does not clear the floors.
TRACKED_SIZES = ("4096", "65536", "1048576")
SIZE_LABEL = {"4096": "4 KiB", "65536": "64 KiB", "1048576": "1 MiB"}

CHARTS = (
    {
        "key": "bandwidth",
        "metric": "bw_peak_GBs",
        "verb": "send",
        "title": "Send bandwidth over time",
        "unit": "GB/s",
        "higher_is_better": True,
    },
    {
        "key": "latency",
        "metric": "lat_typical_us",
        "verb": "send",
        "title": "Send latency over time",
        "unit": "µs",
        "higher_is_better": False,
    },
)

# Reference palette, categorical slots 1-3.  Validated with
# the skill's checker at --pairs all in both modes: worst CVD
# dE 9.2 light / 9.4 dark, worst normal-vision dE 24.0 / 20.9.
LIGHT = ("#2a78d6", "#eb6834", "#1baf7a")
DARK = ("#3987e5", "#d95926", "#199e70")


def load_history(path):
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


def extract(summary):
    """Pull the tracked medians out of a run summary."""
    rec = {
        "generated": summary["meta"].get("generated"),
        "sha": (summary["meta"].get("sha") or "")[:8],
        "run_id": summary["meta"].get("run_id"),
        "node": summary["meta"].get("node"),
        "series": {},
    }
    for chart in CHARTS:
        got = {}
        for row in summary.get("perf", {}).get(chart["key"], []):
            if (row.get("verb") == chart["verb"]
                    and row.get("metric") == chart["metric"]
                    and str(row.get("size")) in TRACKED_SIZES
                    and row.get("median") is not None):
                got[str(row["size"])] = float(row["median"])
        rec["series"][chart["key"]] = got
    return rec


def nice_ceiling(v):
    """A round upper bound at or above v."""
    if v <= 0:
        return 1.0
    import math
    mag = 10 ** math.floor(math.log10(v))
    for step in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if step * mag >= v:
            return step * mag
    return 10 * mag


def fmt_tick(v):
    """Short tick text: no repeating decimals, no trailing zeros."""
    if v == 0:
        return "0"
    d = 0 if abs(v) >= 10 else (1 if abs(v) >= 1 else 3)
    out = f"{v:.{d}f}"
    # Only trim inside a fraction: "10" must not become "1".
    return out.rstrip("0").rstrip(".") if "." in out else out


def svg_chart(chart, history, chart_id):
    """Render one metric as small multiples: one panel per size.

    A single linear axis cannot carry these together.  Send
    bandwidth at 4 KiB is around 0.15 GB/s while 1 MiB is near
    7, so on a shared scale the small sizes flatten onto the
    baseline and any movement in them is invisible - which is
    the movement a regression would show up in.  One panel per
    size, each on its own scale, keeps every series readable.

    One series per panel also means no legend is needed (the
    panel title names it) and no colour pair is ever compared,
    so the palette carries no CVD burden here.  Marks follow
    the skill's specs: 2px lines, a recessive grid, and text in
    ink tokens rather than the series colour.
    """
    pts = [h for h in history if h["series"].get(chart["key"])]
    if not pts:
        return ""
    sizes = [s for s in TRACKED_SIZES
             if any(p["series"][chart["key"]].get(s) is not None
                    for p in pts)]
    if not sizes:
        return ""

    PW, PH = 236, 190           # panel plot box
    ml, mr, mt, mb = 54, 14, 30, 42
    gap = 18
    panel_w = ml + PW + mr
    W = panel_w * len(sizes) + gap * (len(sizes) - 1)
    H = mt + PH + mb

    o = []
    a = o.append
    a(f'<svg class="viz" viewBox="0 0 {W} {H}" width="100%" '
      f'role="img" aria-labelledby="{chart_id}-t" '
      f'xmlns="http://www.w3.org/2000/svg">')
    a(f'<title id="{chart_id}-t">{chart["title"]}, '
      f'one panel per message size</title>')

    n = len(pts)
    for pi, size in enumerate(sizes):
        ox = pi * (panel_w + gap)
        colour = f"var(--s{pi + 1})"
        seq = [(i, p["series"][chart["key"]].get(size))
               for i, p in enumerate(pts)]
        seq = [(i, v) for i, v in seq if v is not None]
        if not seq:
            continue
        ymax = nice_ceiling(max(v for _, v in seq) * 1.12)

        def x(i, ox=ox):
            return ox + ml + (PW / 2 if n == 1 else PW * i / (n - 1))

        def y(v, ymax=ymax):
            return mt + PH - (v / ymax) * PH

        a(f'<text class="ptitle" x="{ox + ml}" y="{mt - 12}">'
          f'{SIZE_LABEL[size]}</text>')
        for k in range(5):
            gv = ymax * k / 4
            gy = y(gv)
            a(f'<line class="grid" x1="{ox + ml}" y1="{gy:.1f}" '
              f'x2="{ox + ml + PW}" y2="{gy:.1f}"/>')
            a(f'<text class="tick" x="{ox + ml - 7}" y="{gy + 4:.1f}" '
              f'text-anchor="end">{fmt_tick(gv)}</text>')

        d = " ".join(f'{"M" if k == 0 else "L"}{x(i):.1f},{y(v):.1f}'
                     for k, (i, v) in enumerate(seq))
        a(f'<path class="ln" d="{d}" stroke="{colour}"/>')
        for i, v in seq:
            a(f'<circle class="pt" cx="{x(i):.1f}" cy="{y(v):.1f}" '
              f'r="4" fill="{colour}">'
              f'<title>{SIZE_LABEL[size]} \u2014 {v:g} {chart["unit"]} '
              f'\u2014 {pts[i]["sha"]}</title></circle>')

        # Direct-label the newest value: the number a reader
        # actually wants, and identity never rests on colour.
        li, lv = seq[-1]
        a(f'<text class="lbl" x="{ox + ml + PW}" y="{y(lv) - 10:.1f}" '
          f'text-anchor="end">{lv:g}</text>')
        a(f'<text class="tick" x="{ox + ml}" y="{mt + PH + 18}">'
          f'{pts[0]["sha"]}</text>')
        a(f'<text class="tick" x="{ox + ml + PW}" y="{mt + PH + 18}" '
          f'text-anchor="end">{pts[-1]["sha"]}</text>')

    a(f'<text class="axis" x="{ml}" y="{H - 8}">'
      f'{chart["unit"]}, oldest run left to newest right</text>')
    a('</svg>')
    return "\n".join(o)


CSS = """
<style>
.vizwrap{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;
 --muted:#898781;--rule:#c3c2b7;
 --s1:%s;--s2:%s;--s3:%s;
 background:var(--surface);padding:8px 4px;border-radius:6px;margin:6px 0 2px}
.vizwrap .ln{fill:none;stroke-width:2;stroke-linejoin:round;
 stroke-linecap:round}
.vizwrap .pt{stroke:var(--surface);stroke-width:2}
.vizwrap .grid{stroke:var(--rule);stroke-width:1;opacity:.55}
.vizwrap .tick{fill:var(--muted);font:11px system-ui,sans-serif}
.vizwrap .axis{fill:var(--ink2);font:11px system-ui,sans-serif}
.vizwrap .lbl{fill:var(--ink);font:12px system-ui,sans-serif;font-weight:600}
.vizwrap .ptitle{fill:var(--ink2);font:12px system-ui,sans-serif}
@media (prefers-color-scheme:dark){.vizwrap{
 --surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;--rule:#383835;
 --s1:%s;--s2:%s;--s3:%s}}
</style>
""" % (LIGHT + DARK)


PLACEHOLDER = """.. This page is generated by ci/report/publish-perf.py.
.. Edits will be overwritten by the next nightly run.

Performance trends
==================

No published measurements yet.

The nightly full-tier run on the lab node appends its medians
here and regenerates this page, so charts appear after the
first successful nightly. Pull request runs never publish:
their numbers describe the pull request, not ``main``.
"""


def build_page(history, docs_dir):
    """Write perf-trends.rst plus the HTML fragment it includes.

    The charts live in a separate .html file rather than inline
    ``.. raw:: html`` blocks.  docs-check runs
    ``doc8 docs/ --max-line-length 80``, and SVG path data is
    nowhere near 80 columns; doc8 only scans .rst, so keeping
    the markup out of the .rst is what lets both coexist.
    """
    out = docs_dir / "perf-trends.rst"
    frag = docs_dir / "perf-history" / "charts.html"
    frag.parent.mkdir(parents=True, exist_ok=True)
    if not history:
        out.write_text(PLACEHOLDER)
        return out

    latest = history[-1]
    lines = [
        ".. This page is generated by ci/report/publish-perf.py.",
        ".. Edits will be overwritten by the next nightly run.",
        "",
        "Performance trends",
        "==================",
        "",
    ]
    lines += textwrap.wrap(
        f"Medians from the nightly full-tier run on "
        f"``{latest.get('node', 'the lab node')}``, oldest run at the "
        f"left. Latest: ``{latest['sha']}`` at "
        f"{latest.get('generated', 'an unknown time')}.", 72)
    lines.append("")
    lines += textwrap.wrap(
        "Each panel carries one message size on its own scale. "
        "Bandwidth at 4 KiB and at 1 MiB differ by more than an "
        "order of magnitude, so a shared axis would flatten the "
        "small sizes onto the baseline and hide exactly the "
        "movement a regression shows up in. Bandwidth and latency "
        "are likewise never combined: they share no scale.", 72)
    lines.append("")

    for ci_, chart in enumerate(CHARTS):
        svg = svg_chart(chart, history, f"c{ci_}")
        if not svg:
            continue
        better = ("higher is better" if chart["higher_is_better"]
                  else "lower is better")
        lines += [chart["title"], "-" * len(chart["title"]), ""]
        lines += textwrap.wrap(
            f"Median ``{chart['metric']}`` for ``{chart['verb']}`` "
            f"({better}).", 72)
        # One fragment per chart, so each sits with its own
        # table rather than all the charts stacking up top.
        name = f"chart-{chart['key']}.html"
        (frag.parent / name).write_text(
            f'{CSS.strip()}\n<div class="vizwrap">\n{svg}\n</div>\n')
        lines += ["", ".. raw:: html",
                  f"   :file: perf-history/{name}", ""]

        # A table of the same numbers: the accessible reading,
        # and the relief the palette check asks for, since
        # light-mode aqua sits below 3:1 on the surface.
        sizes = [z for z in TRACKED_SIZES
                 if any(h["series"][chart["key"]].get(z) is not None
                        for h in history)]
        head = ["Run"] + [SIZE_LABEL[z] for z in sizes]
        rows = []
        for h in history[-10:]:
            got = h["series"].get(chart["key"], {})
            rows.append([h["sha"] or "?"] + [
                (f"{got[z]:g}" if got.get(z) is not None else "--")
                for z in sizes])
        w = [max(len(r[i]) for r in [head] + rows)
             for i in range(len(head))]
        sep = " ".join("=" * x for x in w)
        lines += [f"Last {len(rows)} runs, in {chart['unit']}:", "", sep,
                  " ".join(c.ljust(x) for c, x in zip(head, w)).rstrip(),
                  sep]
        for r in rows:
            lines.append(" ".join(c.ljust(x)
                                  for c, x in zip(r, w)).rstrip())
        lines += [sep, ""]

    out.write_text("\n".join(lines) + "\n")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", required=True)
    ap.add_argument("--docs-dir", default="docs")
    ap.add_argument("--max-runs", type=int, default=60,
                    help="how many runs of history to keep")
    args = ap.parse_args()

    docs = pathlib.Path(args.docs_dir)
    hist_path = docs / "perf-history" / "history.jsonl"
    hist_path.parent.mkdir(parents=True, exist_ok=True)

    summary = json.load(open(args.summary))
    rec = extract(summary)
    if not any(rec["series"].values()):
        print("no tracked medians in this run; nothing published")
        return 0

    history = load_history(hist_path)
    if history and history[-1].get("run_id") == rec["run_id"]:
        print(f"run {rec['run_id']} already recorded; nothing to do")
        return 0

    history.append(rec)
    history = history[-args.max_runs:]
    hist_path.write_text(
        "".join(json.dumps(h, sort_keys=True) + "\n" for h in history))

    page = build_page(history, docs)
    print(f"recorded run {rec['run_id']} ({rec['sha']}); "
          f"{len(history)} runs in history")
    print(f"wrote {page}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
