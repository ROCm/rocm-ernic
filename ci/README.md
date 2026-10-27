# CI support files

This directory holds what the GitHub-hosted workflows in
`.github/workflows/` share:

- `guest-image.env` -- the pin for the guest image every VM
  lane boots.
- `report/` -- `gen-report.py`, which turns a lane's results
  into a report, and `publish-perf.py`, which records the
  performance numbers and renders the trend page.
- `iwyu/` -- the scripts that the IWYU workflow
  (`.github/workflows/iwyu.yml`) runs.

The VM lanes themselves are `system-tests.yml` and the
`nvmeof-lane.yml`, `s3-lane.yml` and `p4-lane.yml` workflows
it calls. They provision their guests and run their tests
through `ansible/ci-site.yml`, so the test logic lives in
the same Ansible plays a developer runs.

## The guest image

The guest disk is **pulled from the registry**, not built
here. The image is built elsewhere, by the `ionic` flavour of
[batesste-ci-images][ref-ci-images].

The tag, disk name and login account are pinned only in
`ci/guest-image.env`, which the GitHub lanes and
`ansible/group_vars/all.yml` both read; a bump is one edit
there. `tools/check-guest-image-tag.sh` fails the build if a
literal tag appears anywhere else.

The hosted lanes pull it through
`.github/actions/fetch-guest-vm`. By hand,
`scripts/fetch-guest-image.sh` lands the same bytes in the
same layout, and checks the image before it pulls the disk:
it rejects an image whose kernel is below 6.18, skewed from
`IONIC_KERNEL_REF`, or different from the badge on
`README.md`, and, given `--expect-user`, `--expect-disk`,
`--expect-release` or `--expect-flavour`, one whose
`vm-info.json` disagrees. Those checks run after the
kilobyte metadata pull and before the 1.6 GB disk pull, so a
wrong image fails in seconds.

## Publishing performance trends

The scheduled run publishes its medians to the docs site at
<https://rocm.github.io/rocm-ernic/>, under *Performance
trends*.

All three badges are fed on identical terms. `system-tests.yml`
is the single entry point, and both of its publishing lanes --
the two-VM one for `perftest`/`iperf3`, the NVMe-oF one for the
fio sweep -- hand off to the same composite action,
`.github/actions/publish-perf`. There is one copy of the commit,
push and Pages-dispatch logic, so a bug in it is fixed once rather
than per lane.

### Nothing CI writes lands on `develop`

`develop` is source only. The published site and the data behind it
live on the orphan `gh-pages` branch, which is also what GitHub
Pages serves (Settings > Pages > "Deploy from a branch",
`gh-pages` / root):

```
/                    built Sphinx HTML
/.nojekyll
/perf/history.jsonl  the durable record, one JSON object per run
/perf/badge-*.json   the shields the README points at
```

`perf/` has no leading underscore deliberately, so it is served
even if `.nojekyll` is ever lost.

A publishing lane clones `gh-pages`, copies `perf/history.jsonl`
into `docs/perf-history/`, runs `ci/report/publish-perf.py` to
append its record and refresh the badges, copies both back into
`perf/` and pushes. It never touches the built HTML, so a perf
run cannot disturb the site.

That push is made with the job's own `GITHUB_TOKEN`, which
GitHub's loop-prevention rule exempts from starting other workflow
runs, so the lane then dispatches `docs-deploy.yml` explicitly
(`workflow_dispatch`, needs `actions: write`) to rebuild the site.
`docs-deploy` fetches `perf/history.jsonl` back off `gh-pages`,
renders the trend page from it (`publish-perf.py --render-only`),
builds Sphinx and `rsync -a --delete --exclude perf/`s the result
onto `gh-pages`. The exclusion is what keeps the two halves of the
branch from clobbering each other.

The badges are therefore correct the moment a lane pushes: they no
longer wait on a Sphinx build, which is what left run `35270045274`
publishing a real number behind a stale shield.

`docs/perf-history/history.jsonl` is committed to `develop` **empty**,
with `"no data"` badges and a stub `docs/perf-trends.rst`, so a
local `make docs` or a pull request build still renders a complete
site with no toctree warnings. To see the real page locally, fetch
the history first:

```sh
git show origin/gh-pages:perf/history.jsonl \
    > docs/perf-history/history.jsonl
python3 ci/report/publish-perf.py --render-only --docs-dir docs
```

### Runner classes

Every published number comes off a GitHub-hosted runner. The
history also holds a few older records from a self-hosted lab
node, `hpe-rack-15`, which ran CI until it was retired.

The two are never joined into one series. A GitHub runner has
noisy neighbours and no fixed CPU; the lab node had neither.
Interleaving the two in one series produces a chart whose every
other point is a step change in the machine rather than in the
code, which is exactly the signal the trend exists to show.

`publish-perf.py --runner` records the class on each history entry
(`github-hosted`, the default), and the page charts each class on
its own axes. Records written before that flag existed carry no
`runner` key and are read as `self-hosted`, which is where they
came from. The shields read the `github-hosted` series only: a
fallback across classes would put a lab-node number behind a label
the README presents as a GitHub-hosted one.

### The shields

Each publishing run writes one shields.io [endpoint badge][shields-endpoint]
per transport into `docs/perf-history/`, each falling back to the
latest prior run *of the same runner class* that has a value, so one
bad sweep does not grey a badge out:

| File | Metric | Written by |
| --- | --- | --- |
| `badge-rdma.json` | `ib_send_bw`, 1 MiB, peak GB/s | `system-tests.yml` |
| `badge-tcp.json` | `iperf3`, sustained GB/s | `system-tests.yml` |
| `badge-nvmeof.json` | fio randread, 4 KiB, IOPS | `system-tests.yml` |

The lane copies them to `perf/` on `gh-pages`, so each badge is
reachable at
`https://rocm.github.io/rocm-ernic/perf/badge-<name>.json` as soon
as that push lands -- no doc build in between.

README.md links all three. Each is committed with a `"no data"`
placeholder. The placeholder matters -- without a file at that URL
shields.io renders an *error*, which looks far worse than a grey
"no data".

[shields-endpoint]: https://shields.io/badges/endpoint-badge

### Publishing on demand

Waiting for 04:23 UTC is optional. `system-tests.yml` takes a
`workflow_dispatch` with a `publish` boolean (default true), so a
manual run against `develop` fills all three shields:

```bash
gh workflow run system-tests.yml --ref develop
```

Pass `-f publish=false` to exercise the sweeps without touching
the history -- useful when the sweep itself is what you are
testing. Dispatching anything other than `--ref develop` sweeps but
never publishes, by the same rule below.

Budget for it: the two-VM lane boots two guests and runs for the
best part of an hour, so this is not a quick way to refresh a
badge. The NVMe-oF lane finishes much sooner and pushes as soon
as it does, rather than waiting for its slower sibling.

The workflow does not cancel in-progress runs on `develop`, so a
dispatch will not kill a scheduled run part-way through its push;
the two queue instead.

Only a green run on `develop` publishes. Pull request runs never
do -- a pull request's numbers describe the pull request, not
`develop` -- and on `system-tests.yml` they do not even sweep:
the two perf plays are minutes of `perftest` and `iperf3` on top
of a job that already runs the best part of an hour, for numbers
that would be discarded.

Charts are inline SVG with no JavaScript and no extra build
dependency. Each message size gets its own panel and its own
scale: 4 KiB bandwidth is around 0.15 GB/s against roughly 7
at 1 MiB, and on a shared axis the small sizes flatten onto
the baseline, hiding exactly the movement a regression would
show. Every chart is also rendered as a table, which is both
the accessible reading and the relief required for the one
palette colour that sits below 3:1 on the light surface.

Publishing never fails a run. Every git step in the publish
block warns and exits 0 rather than propagating: if `develop`
moved underneath the job, if the push is rejected, or if git
cannot read the repository at all, the run keeps the result it
actually earned. That last case is not hypothetical -- these
lanes run in a container, where the workspace arrives owned by
another uid and `actions/checkout`'s `safe.directory` entry is
out of scope, so the publish block marks the workspace safe
itself before touching git.

## Reports

`ci/report/gen-report.py` reduces a lane's sweeps,
`<results>/perf-csv/*.csv` (perftest, iperf3, fio and the
other plays' CSVs), to per-metric medians, means and ranges.
It writes `report.md` and `summary.json`, and appends the
report to the GitHub step summary. Failed measurements are
shown as an incomplete sample count rather than dropped, so
a run that silently stopped measuring is visible.

The hosted lanes run it from `.github/actions/publish-perf`,
and the `summary.json` it writes is what `publish-perf.py`
records.

[ref-ci-images]: https://github.com/sbates130272/batesste-ci-images
