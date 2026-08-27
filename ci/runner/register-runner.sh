#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# register-runner.sh -- register the staged runner with
# GitHub and start it as a systemd user service.
#
# Run install-runner.sh first.
#
# Mint a token at:
#   https://github.com/ROCm/rocm-ernic/settings/actions/runners/new
# Registration tokens expire after one hour.
#
# Usage:
#   ci/runner/register-runner.sh --token <TOKEN>
#   ci/runner/register-runner.sh --token <TOKEN> \
#       --url https://github.com/ROCm/rocm-ernic
#
# SECURITY NOTE
# -------------
# ROCm/rocm-ernic is a public repository.  A self-hosted
# runner attached to a public repo will execute whatever
# a pull request contains unless workflows are scoped to
# trusted triggers.  The workflow this harness ships
# (.github/workflows/self-hosted-ci.yml) never runs on
# pull_request for exactly this reason.  If you add
# fork-PR triggers later, gate them behind a GitHub
# Environment with required reviewers, and confirm that
#   Settings -> Actions -> General
#     -> "Require approval for all external contributors"
# is enabled.

set -euo pipefail

# $HOME is NFS on this node (the shared /home_mkm filer).
# A long-lived runner daemon does not belong there: every
# job would pay filer latency, an outage would take the
# runner down, and .credentials would sit on shared
# storage.  /local is this node's per-user local-disk
# area, on the same ext4 volume as CI_WORK.
RUNNER_ROOT="${RUNNER_ROOT:-${CI_RUNNER_ROOT:-/local/${USER}/actions-runner-rocm-ernic}}"
RUNNER_WORK="${RUNNER_WORK:-/var/tmp/ernic-ci-work/runner-work}"
RUNNER_URL="${RUNNER_URL:-https://github.com/ROCm/rocm-ernic}"
RUNNER_NAME="${RUNNER_NAME:-$(hostname -s)}"
TOKEN=""
REPLACE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --token)   TOKEN="$2";       shift 2 ;;
        --url)     RUNNER_URL="$2";  shift 2 ;;
        --name)    RUNNER_NAME="$2"; shift 2 ;;
        --root)    RUNNER_ROOT="$2"; shift 2 ;;
        --replace) REPLACE="--replace"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

log() { echo "register-runner: $*"; }

[ -n "${TOKEN}" ] || {
    echo "ERROR: --token is required." >&2
    echo "Mint one at ${RUNNER_URL}/settings/actions/runners/new" >&2
    exit 1
}
[ -x "${RUNNER_ROOT}/config.sh" ] || {
    echo "ERROR: no runner at ${RUNNER_ROOT}." >&2
    echo "Run ci/runner/install-runner.sh first." >&2
    exit 1
}

# ── Labels ────────────────────────────────────────
#
# Workflows select on these.  The kvm label is applied
# only when /dev/kvm is actually usable, so a node that
# loses KVM stops attracting VM jobs rather than
# failing them.

LABELS="self-hosted,linux,x64,rocm-ernic"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    LABELS="${LABELS},kvm"
    log "/dev/kvm is accessible -- adding 'kvm' label"
else
    log "WARNING: /dev/kvm not accessible; omitting 'kvm' label."
    log "  VM and performance jobs require it and will stay queued."
    log "  Fix: sudo chgrp kvm /dev/kvm && sudo chmod 660 /dev/kvm"
    log "  Then re-run with --replace to refresh labels."
fi

cd "${RUNNER_ROOT}"

log "registering '${RUNNER_NAME}' with ${RUNNER_URL}"
log "  labels: ${LABELS}"

./config.sh \
    --unattended \
    --url "${RUNNER_URL}" \
    --token "${TOKEN}" \
    --name "${RUNNER_NAME}" \
    --labels "${LABELS}" \
    --work "${RUNNER_WORK}" \
    ${REPLACE}

systemctl --user daemon-reload
systemctl --user enable --now rocm-ernic-runner.service

sleep 3
if systemctl --user is-active --quiet rocm-ernic-runner.service; then
    log "runner is active"
else
    log "ERROR: runner failed to start. Logs:"
    systemctl --user status rocm-ernic-runner.service --no-pager | tail -20
    exit 1
fi

log ""
log "Runner registered and running."
log "  status:  systemctl --user status rocm-ernic-runner"
log "  logs:    journalctl --user -u rocm-ernic-runner -f"
log "  stop:    systemctl --user stop rocm-ernic-runner"
log "  remove:  cd ${RUNNER_ROOT} && ./config.sh remove --token <TOKEN>"
