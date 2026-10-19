#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run a command on a guest over ssh, and run it again if ssh fails
# before the command starts.
#
# Usage: run-on-guest.sh SSH_COMMAND... REMOTE_COMMAND
#
# SSH_COMMAND is the ssh command line up to and including the
# destination, and REMOTE_COMMAND, the last argument, is the command
# to run there.  Exits with the status of the last attempt: the
# command's, or 255 if ssh failed.
#
# ssh exits 255 both when it fails and when the command does, so the
# guest prints a marker before it runs the command, and only a 255
# without the marker is retried.  The command never started then, so
# it can't run twice.  A connection lost after the command started
# isn't retried; that failure is the caller's to see.
#
# The command's output is printed, without the marker, when it ends.
# ssh's own errors, and a line for each retry, go to stderr.  The
# command gets no stdin: a retry couldn't give it the same stdin
# again.

set -uo pipefail

if [ $# -lt 2 ]; then
    echo "usage: ${0##*/} SSH_COMMAND... REMOTE_COMMAND" >&2
    exit 2
fi

ATTEMPTS=3
RETRY_DELAY=5
MARKER=run-on-guest-started

cmd=${*: -1}
ssh_cmd=("${@:1:$#-1}")

attempt=1
while :; do
    out=$("${ssh_cmd[@]}" "echo $MARKER; $cmd" </dev/null)
    rc=$?
    if [ "$rc" -ne 255 ] || [ "${out%%$'\n'*}" = "$MARKER" ]; then
        break
    fi
    if [ "$attempt" -ge "$ATTEMPTS" ]; then
        echo "${0##*/}: ssh failed before the command started;" \
            "giving up after $ATTEMPTS attempts" >&2
        break
    fi
    echo "${0##*/}: ssh failed before the command started" \
        "(attempt $attempt/$ATTEMPTS); retrying in ${RETRY_DELAY}s" >&2
    sleep "$RETRY_DELAY"
    attempt=$((attempt + 1))
done

if [ "${out%%$'\n'*}" = "$MARKER" ]; then
    out=${out#"$MARKER"}
    out=${out#$'\n'}
fi
if [ -n "$out" ]; then
    printf '%s\n' "$out"
fi
exit "$rc"
