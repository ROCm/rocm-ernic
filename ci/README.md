# Self-hosted CI

This directory holds the harness that turns a lab node
into a GitHub Actions runner for rocm-ernic, capable of
booting real VMs over the emulated RDMA NIC and
producing functional and performance reports.

The GitHub-hosted workflows in `.github/workflows/` can
only build and unit-test. Everything that needs KVM, a
golden VM image or two guests talking RDMA to each other
runs here instead.

## Design

Everything runs **unprivileged**. There is no `sudo`, no
`systemctl`, and nothing is written to `/run`,
`/var/log` or `/usr/local`. This works because the
rocm-ernic launcher and `ernicctl` are entirely
environment-driven, so the whole control plane can be
redirected under a workspace the CI user owns:

| Normal path | CI path |
|---|---|
| `/run/rocm-ernic` | `$CI_WORK/run` |
| `/var/log/rocm-ernic` | `$CI_WORK/log` |
| `/usr/local/bin/rocm-ernic` | `$CI_WORK/build/rocm-ernic` |
| `systemctl start rocm-ernic` | `service/rocm-ernic-launcher` |

`$CI_WORK` defaults to `/var/tmp/ernic-ci-work`, which
is on local disk. Home directories on the lab nodes are
NFS, where qcow2 and build traffic is markedly slower.

The CI runs the checked-out copies of
`service/rocm-ernic-launcher` and `service/ernicctl`
rather than whatever is installed system-wide, so a run
always tests the tree it was handed.

### Reusing the Ansible harness

The test logic is not duplicated here. `ansible/` already
implements guest provisioning, the sanity suite and the
performance sweeps, and the CI drives those plays
directly through `ansible/ci-site.yml`.

`site.yml` is the developer entry point and needs root:
it installs to `/usr/local`, drives systemd, and builds
the golden image over `qemu-nbd`. `ci-site.yml` skips
all of that. It assumes `ci/jobs/vm-up.sh` has already
brought VMs up unprivileged, and supplies only the
inventory registration those plays need, via
`ansible/playbooks/ci-vm-register.yml`.

The golden backing image is treated as a **prebuilt
input**, not something CI regenerates. Creating one
needs `qemu-nbd` and root; see
`ansible/playbooks/vm-create.yml`.

## Tiers

| Tier | Job | Needs KVM |
|---|---|---|
| 1 | build, ctest, loopback backend | no |
| 2 | two-VM RDMA functional | yes |
| 3 | performance sweeps | yes |

Tier 1 is fully self-contained and is the gate for
everything else. Tiers 2 and 3 are scheduled onto
runners carrying the `kvm` label, so a node that loses
KVM access stops attracting those jobs rather than
failing them.

Performance jobs refuse to run under emulation.
Software-emulated numbers are one to two orders of
magnitude off and would poison the regression baseline,
so `ci/jobs/perf.sh` exits rather than record them.

## Setting up a node

Check what the node can do:

```bash
ci/doctor.sh
```

It reports per-tier readiness and exits `0` for the full
matrix, `1` if only tier 1 can run, and `2` if the node
cannot run CI at all.

Stage the runner (downloads and unpacks it, writes a
systemd user unit, enables lingering so it survives
reboot):

```bash
ci/runner/install-runner.sh
```

The runner is installed to `/local/$USER/`, not `$HOME`.
Home is NFS on the lab node, and a long-lived runner
daemon does not belong on a shared filer: every job would
pay filer latency, an outage would take the runner down,
and its `.credentials` would sit on shared storage.
`/local` is the node's per-user local-disk area, on the
same ext4 volume as the work directory. On a fresh node
that directory has to be created once:

```bash
sudo install -d -o "$USER" -g "$(id -gn)" -m 0755 /local/"$USER"
```

Override with `--root` or `CI_RUNNER_ROOT` if your node
lays out local storage differently.

This deliberately stops short of registering. Mint a
registration token at
`https://github.com/ROCm/rocm-ernic/settings/actions/runners/new`
and then:

```bash
ci/runner/register-runner.sh --token <TOKEN>
```

Registration tokens expire after one hour. The script
applies the `kvm` label only when `/dev/kvm` is actually
usable; re-run it with `--replace` after fixing KVM to
refresh the labels.

Manage the runner with:

```bash
systemctl --user status rocm-ernic-runner
journalctl --user -u rocm-ernic-runner -f
```

## Security

`ROCm/rocm-ernic` is public, and this workflow runs on a
lab machine rather than a throwaway GitHub runner. Nothing
reaches the node unless someone asks for it:
`.github/workflows/self-hosted-ci.yml` has **no
`pull_request` trigger and no `push` trigger**. The only
ways in are `workflow_dispatch` and the nightly schedule.

That is a deliberate trade. Pull requests get no automatic
lab-node coverage; a maintainer runs `/run-ci` when they
want it. The GitHub-hosted workflows still run on every
pull request as usual, so contributors are not left
without feedback.

### Testing fork pull requests

Fork pull requests **are** runnable, via the `pr` input:

```bash
gh workflow run self-hosted-ci.yml --ref main -f pr=123
```

or just `/run-ci` on the pull request.

What makes that acceptable is the split between the
workflow and the code it tests. The dispatch targets the
workflow file on the **default branch**, and passes only a
pull request number. Each job then checks out
`refs/pull/<pr>/head`, which resolves for forks. So a fork
supplies code to compile and run, but cannot change what
the workflow does with it, and cannot start a run at all:
`workflow_dispatch` requires write access.

### What this does not protect

The node itself. Fork code runs as the runner's user, with
whatever that user can do. **Treat the ability to dispatch
a fork pull request as equivalent to shell access on the
lab node.**

On the current node the runner user also has passwordless
`sudo`, so that is equivalent to root. Outstanding
hardening, in rough priority order:

1. Run the runner as a dedicated account with no `sudo`.
2. Give that account its own `/local/<user>` workspace and
   `/opt/qemu-images` group access, rather than reusing a
   developer's.
3. Consider an ephemeral runner, so each job starts from a
   clean machine state.

Until those land, only dispatch fork pull requests whose
diff you have actually read.

The workflow also takes a `concurrency` lock. Two
simultaneous runs would collide on VM ssh ports, the
instance manifest and the qcow2 overlays.

## Triggering from a comment

`.github/workflows/ci-command.yml` adds a slash command
usable from any pull request or issue:

| Command | Runs |
|---|---|
| `/run-ci` | build, loopback and two-VM RDMA functional |
| `/run-ci build` | build, ctest and loopback only |
| `/run-ci functional` | same as bare `/run-ci` |
| `/run-ci full` | functional plus performance sweeps |
| `/run-ci full tcg` | VMs under emulation, no valid perf |
| `/run-ci help` | print usage |

Arguments are order-insensitive and case-insensitive, so
`/run-ci tcg full` and `/run-ci FULL TCG` both work.

The dispatcher reacts to the comment, dispatches the
workflow against the right ref, and replies with a link
to the run.

Three properties keep this honest on a public repository:

1. The dispatcher runs on `ubuntu-latest`, never on the
   self-hosted runner, so untrusted comment text is
   never processed on the lab node.
2. The commenter must have write access or above,
   checked against the API at trigger time.
3. The dispatch always targets the workflow file on the
   default branch and passes the pull request number as an
   input, so a fork supplies the code under test but not
   the workflow that runs it.

Fork pull requests are eligible. See *What this does not
protect* above before running one.

`issue_comment` always runs the workflow file from the
default branch, so this gate cannot be weakened by a
pull request.

Note that `workflow_dispatch` only resolves workflows
present on the default branch. Both
`self-hosted-ci.yml` and `ci-command.yml` must be merged
to `main` before the command works, even when targeting
a topic branch.

## Running jobs by hand

Each job is a standalone script and can be run outside
Actions:

```bash
bash ci/jobs/build.sh          # configure, build, ctest
bash ci/jobs/loopback.sh       # loopback backend suite
bash ci/jobs/vm-up.sh          # boot the CI VMs
bash ci/jobs/vm-functional.sh  # RDMA functional tests
bash ci/jobs/perf.sh           # performance sweeps
bash ci/jobs/vm-down.sh        # tear down
```

Useful overrides:

| Variable | Default | Meaning |
|---|---|---|
| `CI_WORK` | `/var/tmp/ernic-ci-work` | workspace root |
| `CI_BUILD_TYPE` | `Release` | CMake build type |
| `CI_VM_ACCEL` | `kvm` | set `tcg` to emulate |
| `ERNIC_INSTANCES` | `2` | number of VMs |
| `CI_VM_SSH_BASE_PORT` | `2350` | first guest ssh port |
| `CI_KEEP_OVERLAYS` | `false` | keep qcow2 after teardown |

VM names and ports are deliberately distinct from the
interactive defaults in `/etc/rocm-ernic/rocm-ernic.env`
so a CI run can never disturb a developer's VMs on the
same box. Teardown only ever matches VMs it launched.

## Reports

`ci/report/gen-report.py` merges three sources into one
report:

- `$CI_WORK/results/*.jsonl` — shell-level check results
- `$CI_WORK/results/junit/*.xml` — Ansible task results,
  captured via the `junit` callback plugin
- `$CI_WORK/results/perf-csv/*.csv` — perftest and
  iperf3 sweeps

It writes `report.md` and `summary.json`, and appends
the report to the GitHub step summary. Failed
measurements are shown as an incomplete sample count
rather than dropped, so a run that silently stopped
measuring is visible.

Regression checking compares medians against a stored
baseline:

```bash
python3 ci/report/gen-report.py \
    --results "$CI_WORK/results" \
    --out "$CI_WORK/report" \
    --baseline "$CI_WORK/baseline/summary.json" \
    --threshold 15
```

Latency metrics are inverted so that "worse" is always
negative. The script exits non-zero on any functional
failure or regression, which is what gates the workflow.

Only a clean tier-3 run on `main` updates the baseline,
so a failing or partial run can never quietly lower the
bar.

### What is gated, and why not everything

Only latency is gated by default. Two back-to-back clean
sweeps on the same host and build differ far more by
metric than you might expect:

| Metric | median | worst |
|---|---:|---:|
| `lat_max_us` | 1.0% | 11.9% |
| `lat_typical_us` | 1.0% | 28.6% |
| `bw_peak_GBs` | 7.4% | 69.0% |
| `bw_avg_GBs` | 7.0% | 153.6% |
| `msg_rate_mpps` | 7.1% | 152.8% |

Averaged bandwidth and message rate swing too wide on an
emulated two-VM setup to gate on. At any threshold tight
enough to catch a real regression they fire on noise, and
a gate that cries wolf nightly gets ignored. They are
still measured and reported, just not failed on. Add
others with `--gate-metric` if a workload proves stable
enough to warrant it.

Build the baseline from the median of at least two clean
sweeps rather than a single run; one run bakes in
whichever way the noise happened to fall. The baseline
lives under `$CI_WORK`, not in git, because it describes
one host: numbers from this node are not meaningful on
another. Rebuilding a node means recapturing it.
