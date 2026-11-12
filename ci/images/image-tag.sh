#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# image-tag.sh
#
# Prints the tag of a CI image, which names its contents: a prefix for
# people, from its pins, then the first 12 hex digits of a SHA-256 over
# every file that defines it. Those are the files in ci/images/NAME/ (its
# pins.env included), this script, and ensure-image.sh, which builds it.
# Any change to them makes a new tag, and a tag, once published, is never
# built again, so a tag always names one build.
#
# Paths are hashed relative to ci/images/, so the tag doesn't change if
# the directory moves. The files are those git tracks or would track
# (untracked ones not ignored), read from the working tree, so the tag is
# the same before and after a commit.
#
# Usage: image-tag.sh [--ref] NAME
#   NAME    an image's directory under ci/images/ (builder)
#   --ref   print the full reference, REGISTRY/IMAGE:TAG, not the tag
#
# CI_IMAGES_REGISTRY names the registry and namespace (default:
# ghcr.io/rocm/rocm-ernic).

set -euo pipefail

PROG=$(basename -- "$0")
IMAGES_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--ref] NAME" >&2
    exit 2
}

REF=false
NAME=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --ref)
            REF=true
            shift
            ;;
        -h | --help) usage ;;
        -*) die "unknown option: $1" ;;
        *)
            [[ -z $NAME ]] || usage
            NAME=$1
            shift
            ;;
    esac
done
[[ -n $NAME ]] || usage

# The published image's name for each definition
case $NAME in
    builder) IMAGE=ci-builder ;;
    *) die "no such image: '$NAME'" ;;
esac

PINS="$IMAGES_DIR/$NAME/pins.env"
[[ -f $PINS ]] || die "$PINS is missing"

# Read pins.env into PIN[], without running it. Only comments, blank lines
# and KEY=VALUE are allowed.
declare -A PIN=()
while IFS= read -r line || [[ -n $line ]]; do
    if [[ -z $line || $line == \#* ]]; then
        continue
    fi
    if [[ ! $line =~ ^([A-Z][A-Z0-9_]*)=([^[:space:]\"\'\$\`]*)$ ]]; then
        die "$PINS: not KEY=VALUE: $line"
    fi
    PIN[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
done <"$PINS"

# Print a pin, which must be set.
#   $1  its name
pin() {
    [[ -n ${PIN[$1]:-} ]] || die "$PINS doesn't set $1"
    printf '%s\n' "${PIN[$1]}"
}

# The tag's readable prefix, from the pins people look for in it
case $NAME in
    builder)
        qemu=$(pin QEMU_COMMIT)
        vfu=$(pin LIBVFIO_USER_COMMIT)
        if [[ $qemu =~ ^v([0-9][0-9.]*)$ ]]; then
            qemu=${BASH_REMATCH[1]}
        elif [[ $qemu =~ ^[0-9a-f]{40}$ ]]; then
            qemu=.${qemu:0:7}
        else
            die "QEMU_COMMIT must be a vX.Y.Z tag or a full SHA: '$qemu'"
        fi
        [[ $vfu =~ ^[0-9a-f]{40}$ ]] ||
            die "LIBVFIO_USER_COMMIT must be a full SHA: '$vfu'"
        PREFIX="qemu$qemu-vfu.${vfu:0:7}"
        ;;
esac

# The files that define the image, relative to ci/images/, in a fixed
# order. A file deleted from the working tree but not from git's index is
# gone, so it's skipped.
cd -- "$IMAGES_DIR"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "$IMAGES_DIR isn't in a git working tree"
mapfile -t FILES < <(git ls-files --cached --others --exclude-standard \
    --deduplicate -- "$NAME" image-tag.sh ensure-image.sh | LC_ALL=C sort -u)
[[ ${#FILES[@]} -gt 0 ]] || die "no files define $NAME"

# One line per file: its path, whether it's executable, and its content's
# SHA-256
manifest() {
    local f mode sum
    for f in "${FILES[@]}"; do
        [[ -f $f ]] || continue
        mode=644
        if [[ -x $f ]]; then
            mode=755
        fi
        sum=$(sha256sum -- "$f")
        printf '%s %s %s\n' "${sum%% *}" "$mode" "$f"
    done
}
HASH=$(manifest | sha256sum)
TAG="$PREFIX-${HASH:0:12}"

# What a registry accepts as a tag
[[ $TAG =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]] || die "bad tag: '$TAG'"

if $REF; then
    REGISTRY=${CI_IMAGES_REGISTRY:-ghcr.io/rocm/rocm-ernic}
    [[ $REGISTRY =~ ^[a-z0-9][a-z0-9.:-]*(/[a-z0-9][a-z0-9._-]*)*$ ]] ||
        die "CI_IMAGES_REGISTRY isn't a lowercase registry path: '$REGISTRY'"
    printf '%s/%s:%s\n' "$REGISTRY" "$IMAGE" "$TAG"
else
    printf '%s\n' "$TAG"
fi
