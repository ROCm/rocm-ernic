#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
# Exercise the actual harness initializer, including filesystem failures.
set -euo pipefail
binary=$(realpath "$1")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
umask 000
while IFS='|' read -r mode category description expected; do
    work="$scratch/$mode"
    mkdir "$work"
    printf 'preserve me\n' > "$work/victim"
    cp "$work/victim" "$work/expected"
    want=0
    case "$mode" in
        regular) cp "$work/victim" "$work/seed-00"; want=1 ;;
        symlink) ln -s victim "$work/seed-00"; want=1 ;;
        fifo) mkfifo "$work/seed-00"; want=1 ;;
        directory) mkdir "$work/seed-00"; want=1 ;;
    esac
    rc=0
    (
        cd "$work"
        case "$mode" in
            unset) unset FUZZ_EMIT_SEEDS ;;
            invalid) export FUZZ_EMIT_SEEDS="$work/untrusted-path" ;;
            *) export FUZZ_EMIT_SEEDS=1 ;;
        esac
        exec "$binary"
    ) > "$work/log" 2>&1 || rc=$?
    ok=true
    if (( rc != want )); then ok=false; fi
    cmp -s "$work/victim" "$work/expected" || ok=false
    case "$mode" in
        private)
            count=0
            for seed in "$work"/seed-*; do
                if [[ ! -f "$seed" || -L "$seed" ]] ||
                    [[ $(stat -c %a "$seed") != 600 ]]; then ok=false; fi
                count=$((count + 1))
            done
            if (( count == 0 )); then ok=false; fi
            ;;
        unset|invalid)
            if compgen -G "$work/seed-*" > /dev/null ||
                [[ -e "$work/untrusted-path" ]]; then ok=false; fi ;;
        regular) cmp -s "$work/seed-00" "$work/expected" || ok=false ;;
        symlink) [[ -L "$work/seed-00" ]] || ok=false ;;
        fifo) [[ -p "$work/seed-00" ]] || ok=false ;;
        directory) [[ -d "$work/seed-00" ]] || ok=false ;;
    esac
    if ! "$ok"; then
        printf 'FAIL %s: %s; expected: %s (exit %s, wanted %s)\n' \
            "$category" "$description" "$expected" "$rc" "$want"
        cat "$work/log"
        exit 1
    fi
    printf 'ok %s: %s; expected: %s\n' "$category" "$description" "$expected"
done <<'CASES'
private|positive/boundary|emit under umask 000|success; every seed is a regular file with mode 0600
unset|negative|emission flag absent|successful replay; no seed files
invalid|negative|path supplied instead of flag 1|successful replay; environment value never used as a path
regular|negative|first seed already exists|exit 1; existing content preserved
symlink|corner|first seed is a symlink|exit 1; link and target preserved
fifo|corner|first seed is a FIFO with no reader|exit 1 promptly; FIFO preserved
directory|corner|first seed is a directory|exit 1; directory preserved
CASES
