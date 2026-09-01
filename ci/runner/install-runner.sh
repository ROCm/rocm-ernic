#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# install-runner.sh -- stage a GitHub Actions self-hosted
# runner on this node.
#
# This only downloads and unpacks the runner and writes
# the systemd user unit.  It deliberately does NOT
# register with GitHub: registration needs a short-lived
# token that only a repo admin can mint, and pointing a
# runner at a public repository is a decision that
# should be made explicitly.  Run register-runner.sh
# once you have a token.
#
# Nothing here needs root.  Persistence across reboots
# comes from a systemd *user* service plus lingering,
# which loginctl grants to your own account.
#
# Usage:
#   ci/runner/install-runner.sh [--version 2.337.0]

set -euo pipefail

RUNNER_VERSION="${RUNNER_VERSION:-2.337.0}"
# $HOME is NFS on this node (the shared /home_mkm filer).
# A long-lived runner daemon does not belong there: every
# job would pay filer latency, an outage would take the
# runner down, and .credentials would sit on shared
# storage.  /local is this node's per-user local-disk
# area, on the same ext4 volume as CI_WORK.
RUNNER_ROOT="${RUNNER_ROOT:-${CI_RUNNER_ROOT:-/local/${USER}/actions-runner-rocm-ernic}}"
# Work dir goes on local disk: $HOME is NFS on this node
# and checkouts/builds there are markedly slower.
RUNNER_WORK="${RUNNER_WORK:-/var/tmp/ernic-ci-work/runner-work}"

while [ $# -gt 0 ]; do
    case "$1" in
        --version) RUNNER_VERSION="$2"; shift 2 ;;
        --root)    RUNNER_ROOT="$2";    shift 2 ;;
        --work)    RUNNER_WORK="$2";    shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

TARBALL="actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
URL="https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${TARBALL}"

log() { echo "install-runner: $*"; }

# ── Unpack ────────────────────────────────────────

# /local is root-owned, so the per-user directory has to
# exist before an unprivileged install can proceed.  Say
# so plainly rather than failing inside mkdir.
parent="$(dirname "${RUNNER_ROOT}")"
if [ ! -d "${parent}" ] && [ ! -w "$(dirname "${parent}")" ]; then
    echo "ERROR: ${parent} does not exist and cannot be created." >&2
    echo "Create it once with:" >&2
    echo "  sudo install -d -o ${USER} -g \"$(id -gn)\" -m 0755 ${parent}" >&2
    exit 1
fi

mkdir -p "${RUNNER_ROOT}" "${RUNNER_WORK}"

if [ -x "${RUNNER_ROOT}/config.sh" ]; then
    log "runner already unpacked at ${RUNNER_ROOT}"
else
    log "downloading runner v${RUNNER_VERSION}"
    curl -fSL -o "/tmp/${TARBALL}" "${URL}"
    log "unpacking into ${RUNNER_ROOT}"
    tar -xzf "/tmp/${TARBALL}" -C "${RUNNER_ROOT}"
    rm -f "/tmp/${TARBALL}"
fi

# ── Runner environment ────────────────────────────
#
# .env is read by the runner process itself, so jobs
# inherit these without the workflow having to set them.

cat >"${RUNNER_ROOT}/.env" <<EOF
CI_WORK=/var/tmp/ernic-ci-work
LANG=C.UTF-8
EOF

# ── systemd user unit ─────────────────────────────

UNIT_DIR="${HOME}/.config/systemd/user"
mkdir -p "${UNIT_DIR}"

cat >"${UNIT_DIR}/rocm-ernic-runner.service" <<EOF
[Unit]
Description=GitHub Actions runner (rocm-ernic)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=${RUNNER_ROOT}
ExecStart=${RUNNER_ROOT}/run.sh
Restart=always
RestartSec=10
KillMode=process
KillSignal=SIGTERM
TimeoutStopSec=5min
Environment=RUNNER_ALLOW_RUNASROOT=0

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload

# Lingering keeps the user manager (and therefore the
# runner) alive across logout and reboot.  This is
# grantable without root via polkit.
if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" != "yes" ]; then
    log "enabling lingering so the runner survives logout/reboot"
    loginctl enable-linger "$(id -un)" || \
        log "WARNING: could not enable lingering; runner will not auto-start at boot"
fi

log "staged runner v${RUNNER_VERSION}"
log "  root: ${RUNNER_ROOT}"
log "  work: ${RUNNER_WORK}"
log ""
log "Next: mint a registration token and register."
log "  1. Open:"
log "     https://github.com/ROCm/rocm-ernic/settings/actions/runners/new"
log "  2. Copy the token from the ./config.sh line, then:"
log "     ci/runner/register-runner.sh --token <TOKEN>"
