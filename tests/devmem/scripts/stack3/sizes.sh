#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# sizes.sh TAG TXSRC RXSRC "SIZE..."  -- repeat one case over buffer sizes.
#
#   env: REPS (default 2), SENDS (default 32), PORT0 (default 5500)
#
# The probe runs on the first repetition of each size only.
set -uo pipefail
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=tests/devmem/scripts/stack3/env.sh
. "$HERE/env.sh"

TAG=${1:?tag}
TXSRC=${2:?txsrc}
RXSRC=${3:?rxsrc}
SIZES=${4:?sizes}
REPS=${REPS:-2}
SENDS=${SENDS:-32}
P=${PORT0:-5500}

for sz in $SIZES; do
    for r in $(seq "$REPS"); do
        P=$((P + 1))
        echo "### $TAG $sz rep$r port $P $(date -u +%FT%TZ)"
        if [ "$r" = 1 ]; then probe=1; else probe=0; fi
        PROBE=$probe "$HERE/case.sh" "$TAG-$sz-r$r" "$P" "$TXSRC" "$RXSRC" "$sz" "$SENDS" \
            >/dev/null 2>&1
        grep -hE '^RESULT (rx|tx) |rx_niov_alloc_failed|FAIL' "$EV/$TAG-$sz-r$r.txt" |
            sed "s/^/[$TAG $sz r$r] /"
    done
done
