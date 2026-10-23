#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run a command on a guest over ssh, and try again if ssh fails before
# the command starts, without ever starting it twice.
#
# Usage: run-on-guest.sh SSH_COMMAND... REMOTE_COMMAND
#
# SSH_COMMAND is the ssh command line up to and including the
# destination, and REMOTE_COMMAND, the last argument, is the command
# to run there.  Exits with the status of the last attempt: the
# command's, or 255 if ssh failed.
#
# ssh exits 255 both when it fails and when the command does, and a
# 255 alone doesn't say whether the command started: the connection
# can be lost after the guest started it but before its output
# arrived.  So before it starts the command, the guest creates a
# directory named for a token unique to this call, which only one
# attempt can do.  An attempt that finds the directory there doesn't
# run the command, and this gives up: the command started on an
# earlier attempt, and its output and status are lost.  The guest also
# prints a marker just before it starts the command, so that a 255
# from the command isn't taken for one from ssh and retried.
#
# The directories are left in GUEST_DIR, relative to the guest user's
# home directory; whatever runs this removes them when it's done.
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
GUEST_DIR=.cache/run-on-guest
MARKER=run-on-guest-started
STARTED_BEFORE=run-on-guest-started-before

cmd=${*: -1}
ssh_cmd=("${@:1:$#-1}")

token=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
if [ ${#token} -ne 32 ]; then
    echo "${0##*/}: can't make a token" >&2
    exit 1
fi
claim=$GUEST_DIR/$token

# Run by the guest user's shell, which might be sh, so POSIX.  The
# command goes last, on lines of its own, so that it runs as it would
# alone and its status is the shell's.
remote="if ! { mkdir -p $GUEST_DIR && mkdir $claim; }; then
    if [ -d $claim ]; then
        echo $STARTED_BEFORE
        exit 255
    fi
    echo \"run-on-guest: can't create $claim on the guest\" >&2
    exit 1
fi
echo $MARKER
$cmd"

attempt=1
while :; do
    out=$("${ssh_cmd[@]}" "$remote" </dev/null)
    rc=$?
    first=${out%%$'\n'*}
    if [ "$first" = "$MARKER" ]; then
        out=${out#"$MARKER"}
        out=${out#$'\n'}
        break
    fi
    if [ "$first" = "$STARTED_BEFORE" ]; then
        echo "${0##*/}: the command started on an earlier attempt," \
            "and its output and status were lost; not running it again" >&2
        out=
        rc=255
        break
    fi
    if [ "$rc" -ne 255 ]; then
        break
    fi
    if [ "$attempt" -ge "$ATTEMPTS" ]; then
        echo "${0##*/}: ssh failed; giving up after $ATTEMPTS attempts" >&2
        break
    fi
    echo "${0##*/}: ssh failed (attempt $attempt/$ATTEMPTS);" \
        "retrying in ${RETRY_DELAY}s" >&2
    sleep "$RETRY_DELAY"
    attempt=$((attempt + 1))
done

if [ -n "$out" ]; then
    printf '%s\n' "$out"
fi
exit "$rc"
