#!/bin/bash
#
# --eth-queues: range checks, and the rule that the flag beats every
# --nic-profile layer, whichever order they are given in.  Each row runs
# `rocm-ernic --print-profile` and checks the merged queues.eth, or that the
# server refused to start.
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

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

PROFILE="$WORK/eight-queues.ini"
printf '[queues]\neth = 8\n' > "$PROFILE"

# category|description|expected|arguments (split on spaces; @P is the
# profile above).  expected is "queues N" or "rejected".
ROWS=(
    "positive|default is compat's 4|queues 4|"
    "positive|flag sets the count|queues 16|--eth-queues 16"
    "positive|a profile file sets the count|queues 8|--nic-profile @P"
    "positive|built-in generic-100g sets 16|queues 16|--nic-profile generic-100g"
    "positive|flag beats a profile given after it|queues 2|--eth-queues 2 --nic-profile generic-400g"
    "positive|flag beats a profile given before it|queues 2|--nic-profile @P --eth-queues 2"
    "positive|the last flag wins|queues 3|--eth-queues 5 --eth-queues 3"
    "negative|not a number|rejected|--eth-queues many"
    "negative|trailing junk|rejected|--eth-queues 16q"
    "negative|negative number|rejected|--eth-queues -1"
    "negative|empty value|rejected|--eth-queues="
    "boundary|minimum 1 accepted|queues 1|--eth-queues 1"
    "boundary|0 is below the minimum|rejected|--eth-queues 0"
    "boundary|maximum 64 accepted|queues 64|--eth-queues 64"
    "boundary|65 is above the maximum|rejected|--eth-queues 65"
    "corner|value past UINT32_MAX does not wrap into range|rejected|--eth-queues 4294967300"
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

    out=$("$SERVER" "${args[@]}" --print-profile 2> "$WORK/err")
    rc=$?
    if [ "$rc" -ne 0 ]; then
        got="rejected"
        if ! grep -q -- "--eth-queues" "$WORK/err"; then
            got="rejected without naming the flag"
        fi
    else
        got="queues $(sed -n 's/^eth = //p' <<< "$out")"
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
