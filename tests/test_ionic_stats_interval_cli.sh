#!/usr/bin/env bash
#
# --stats-interval-ms: range checks, and the rule that the flag beats every
# --nic-profile layer, whichever order they are given in.  Each row runs
# `rocm-ernic --print-profile` and checks the merged stats.interval_ms, or
# that the server refused to start.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Exit 0 = pass, exit 1 = fail, exit 77 = skip.

set -uo pipefail

SERVER="${1:-}"
if [ -z "$SERVER" ]; then
    echo "Usage: $0 <server_path>"
    exit 1
fi
if [ ! -x "$SERVER" ]; then
    echo "SKIP: server not found: $SERVER"
    exit 77
fi
# Run the server under ERNIC_TEST_SERVER_WRAPPER (e.g. valgrind), split on
# whitespace, if it's set.
read -r -a SERVER_WRAPPER <<<"${ERNIC_TEST_SERVER_WRAPPER:-}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

PROFILE="$WORK/slow-stats.ini"
printf '[stats]\ninterval_ms = 500\n' > "$PROFILE"

# category|description|expected|arguments (split on spaces; @P is the
# profile above).  expected is "interval N" or "rejected".
ROWS=(
    "positive|default is 1000 ms|interval 1000|"
    "positive|flag sets the interval|interval 250|--stats-interval-ms 250"
    "positive|a profile layer sets the interval|interval 500|--nic-profile @P"
    "positive|flag beats a profile given after it|interval 250|--stats-interval-ms 250 --nic-profile @P"
    "positive|flag beats a profile given before it|interval 250|--nic-profile @P --stats-interval-ms 250"
    "positive|the last flag wins|interval 30|--stats-interval-ms 20 --stats-interval-ms 30"
    "negative|not a number|rejected|--stats-interval-ms abc"
    "negative|trailing junk|rejected|--stats-interval-ms 100ms"
    "negative|negative number|rejected|--stats-interval-ms -5"
    "negative|empty value|rejected|--stats-interval-ms="
    "boundary|minimum 10 accepted|interval 10|--stats-interval-ms 10"
    "boundary|9 is below the minimum|rejected|--stats-interval-ms 9"
    "boundary|maximum 60000 accepted|interval 60000|--stats-interval-ms 60000"
    "boundary|60001 is above the maximum|rejected|--stats-interval-ms 60001"
    "corner|value past UINT32_MAX does not wrap into range|rejected|--stats-interval-ms 4294967306"
    "corner|zero|rejected|--stats-interval-ms 0"
)

failures=0
for row in "${ROWS[@]}"; do
    IFS='|' read -r category description expected argstr <<< "$row"
    args=()
    for a in $argstr; do
        if [ "$a" = "@P" ]; then
            args+=("$PROFILE")
        else
            args+=("$a")
        fi
    done

    out=$("${SERVER_WRAPPER[@]}" "$SERVER" "${args[@]}" --print-profile 2> "$WORK/err")
    rc=$?
    if [ "$rc" -ne 0 ]; then
        got="rejected"
        if ! grep -q -- "--stats-interval-ms" "$WORK/err"; then
            got="rejected without naming the flag"
        fi
    else
        got="interval $(sed -n 's/^interval_ms = //p' <<< "$out")"
    fi

    if [ "$got" = "$expected" ]; then
        echo "ok $category: $description"
    else
        echo "FAIL $category: $description; expected: $expected; got: $got"
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures row(s) failed"
    exit 1
fi
echo "all ${#ROWS[@]} rows ok"
