#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Put the devmem PoC harness where it runs from.
#
# The harness was written in a work area, not in this repository: the host
# scripts find each other, the test programs and the probes by their paths
# under it, and the guests read some of them through a 9p share of it
# (/mnt/hostfs).  This copies every file work-area.map lists to its place
# under $DEVMEM_ROOT, keeping its mode.  DEVMEM_ROOT has no default, here or
# in any script: one that fell back to the original work area would act on
# whatever stack is running there.
#
#   deploy.sh            copy what is missing; stop at a file that is there
#                        and differs, changing nothing
#   deploy.sh --check    only list the files that are missing or differ
#   deploy.sh --force    copy over files that differ
#
# It does not build the test programs or the kernels, fetch a guest image,
# or start a stack: see ../README.md.
set -euo pipefail

mode=${1:-copy}
case "$mode" in
copy | --check | --force) ;;
*)
    echo "usage: deploy.sh [--check|--force]" >&2
    exit 2
    ;;
esac

here=$(dirname "$(readlink -f "$0")")
repo=$(cd "$here/../../.." && pwd)
root=${DEVMEM_ROOT:?set it to the work area}
map=$here/work-area.map

missing=() differ=()
while IFS=$'\t' read -r src dst; do
    case "$src" in '' | '#'*) continue ;; esac
    if [ ! -f "$repo/$src" ]; then
        echo "deploy.sh: work-area.map names $src, which is not in the repository" >&2
        exit 1
    fi
    if [ ! -e "$root/$dst" ]; then
        missing+=("$src"$'\t'"$dst")
    elif ! cmp -s "$repo/$src" "$root/$dst"; then
        differ+=("$src"$'\t'"$dst")
    fi
done <"$map"

if [ "$mode" = --check ]; then
    for e in "${missing[@]}"; do echo "missing: $root/${e#*$'\t'}"; done
    for e in "${differ[@]}"; do echo "differs: $root/${e#*$'\t'}"; done
    [ ${#missing[@]} -eq 0 ] && [ ${#differ[@]} -eq 0 ]
    exit
fi
if [ ${#differ[@]} -gt 0 ] && [ "$mode" != --force ]; then
    for e in "${differ[@]}"; do echo "differs: $root/${e#*$'\t'}" >&2; done
    echo "deploy.sh: ${#differ[@]} file(s) under $root differ; nothing copied (--force overwrites)" >&2
    exit 1
fi
todo=("${missing[@]}")
if [ "$mode" = --force ]; then
    todo+=("${differ[@]}")
fi
for e in "${todo[@]}"; do
    src=${e%%$'\t'*} dst=${e#*$'\t'}
    install -D -m "$(stat -c %a "$repo/$src")" "$repo/$src" "$root/$dst"
done
echo "deploy.sh: copied ${#todo[@]} file(s) to $root"
