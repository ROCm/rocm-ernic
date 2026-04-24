#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Quick VM1 -> VM2 iperf3 check over the rocm-ernic data IP.  Uses ssh -n and
# per-invocation timeout(1) so a stuck remote shell or TCP connect cannot hang
# this script indefinitely.

set -u

IP=192.168.200.10
DUR=300
# -t is transfer time after connect; if TCP stalls, iperf3 can hang without a
# wall-clock cap. timeout(1) exits 124 when it fires.
CLIENT_TIMEOUT=$((DUR + 45))
# Upper bound for each ssh(1) session (remote pkill, server start, cleanup).
SSH_SESSION_TIMEOUT=90
# After starting the server, poll until tcp/5201 listens or give up.
SERVER_LISTEN_TIMEOUT=20

SSH_BASE=(
  ssh -n
  -o BatchMode=yes
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o ConnectTimeout=15
  -o ServerAliveInterval=10
  -o ServerAliveCountMax=3
)
SSHP1=("${SSH_BASE[@]}" -p 2250 ubuntu@localhost)
SSHP2=("${SSH_BASE[@]}" -p 2251 ubuntu@localhost)

ss1() { timeout "${SSH_SESSION_TIMEOUT}" "${SSHP1[@]}" "$@"; }
ss2() { timeout "${SSH_SESSION_TIMEOUT}" "${SSHP2[@]}" "$@"; }

cleanup() {
  ss1 'sudo pkill -9 iperf3 2>/dev/null' || true
}
trap cleanup EXIT INT TERM

ss1 'sudo pkill -9 iperf3 2>/dev/null' || true
ss2 'sudo pkill -9 iperf3 2>/dev/null' || true
sleep 1

# Prefer daemon mode so the SSH session does not wait on a background job's
# stdio (a common hang with nohup ... & over ssh without -n).
if ! ss1 "sudo sh -c 'iperf3 -s -1 -B ${IP} -D --logfile /tmp/iperf3-s.log'"; then
  echo "run-iperf3.sh: iperf3 -D start failed; trying nohup fallback" >&2
  if ! ss1 "sudo bash -c 'nohup iperf3 -s -1 -B ${IP} >/tmp/iperf3-s.log 2>&1 </dev/null &'"; then
    echo "run-iperf3.sh: iperf3 server start failed" >&2
    ss1 'sudo tail -50 /tmp/iperf3-s.log 2>/dev/null || true' >&2 || true
    exit 1
  fi
fi

LISTEN_OK=0
if ss1 'command -v ss >/dev/null 2>&1'; then
  end=$((SECONDS + SERVER_LISTEN_TIMEOUT))
  while (( SECONDS < end )); do
    if ss1 "ss -ltn 2>/dev/null | grep -q ':5201'"; then
      LISTEN_OK=1
      break
    fi
    sleep 0.25
  done
else
  sleep 2
  LISTEN_OK=1
fi

if (( LISTEN_OK == 0 )); then
  echo "run-iperf3.sh: iperf3 did not listen on tcp/5201 within ${SERVER_LISTEN_TIMEOUT}s" >&2
  ss1 'sudo tail -80 /tmp/iperf3-s.log 2>/dev/null || true' >&2 || true
  exit 1
fi

ss2 "timeout ${CLIENT_TIMEOUT} iperf3 -c ${IP} -t ${DUR} -f m"
RC=$?

trap - EXIT INT TERM
cleanup

exit "${RC:-0}"
