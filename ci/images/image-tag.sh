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
# ci/guest-image.env is the lockfile: it pins each image defined here, as
# PREFIX_IMAGE (the image in the default registry) and PREFIX_TAG, and
# --check fails if it doesn't pin what the tree defines.
#
# Usage: image-tag.sh [--ref | --check | --write] NAME
#   NAME     an image's directory under ci/images/ (builder)
#   --ref    print the full reference, REGISTRY/IMAGE:TAG, not the tag
#   --check  fail unless ci/guest-image.env pins the image as the tree
#            defines it
#   --write  pin the image in ci/guest-image.env as the tree defines it
#
# CI_IMAGES_REGISTRY names the registry and namespace for --ref (default:
# ghcr.io/rocm/rocm-ernic). The lockfile always names the default.

set -euo pipefail

PROG=$(basename -- "$0")
IMAGES_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
LOCKFILE=$(cd -- "$IMAGES_DIR/.." && pwd)/guest-image.env
DEFAULT_REGISTRY=ghcr.io/rocm/rocm-ernic

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--ref | --check | --write] NAME" >&2
    exit 2
}

MODE=tag
NAME=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --ref | --check | --write)
            [[ $MODE == tag ]] || usage
            MODE=${1#--}
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

# The published image's name for each definition, and its keys' prefix in
# the lockfile
case $NAME in
    builder)
        IMAGE=ci-builder
        LOCK_KEY=BUILDER
        ;;
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

case $MODE in
    tag) printf '%s\n' "$TAG" ;;
    ref)
        REGISTRY=${CI_IMAGES_REGISTRY:-$DEFAULT_REGISTRY}
        [[ $REGISTRY =~ ^[a-z0-9][a-z0-9.:-]*(/[a-z0-9][a-z0-9._-]*)*$ ]] ||
            die "CI_IMAGES_REGISTRY isn't a lowercase registry path:" \
                "'$REGISTRY'"
        printf '%s/%s:%s\n' "$REGISTRY" "$IMAGE" "$TAG"
        ;;
    check | write)
        [[ -f $LOCKFILE ]] || die "$LOCKFILE is missing"
        IMAGE_KEY=${LOCK_KEY}_IMAGE
        TAG_KEY=${LOCK_KEY}_TAG
        want_image="$DEFAULT_REGISTRY/$IMAGE"

        # The lockfile's lines, and what it pins now. Each key must be
        # there once; --write changes it in place and adds nothing.
        mapfile -t LINES <"$LOCKFILE"
        pinned_image="" pinned_tag="" n_image=0 n_tag=0
        for line in "${LINES[@]}"; do
            case $line in
                "$IMAGE_KEY="*)
                    pinned_image=${line#*=}
                    n_image=$((n_image + 1))
                    ;;
                "$TAG_KEY="*)
                    pinned_tag=${line#*=}
                    n_tag=$((n_tag + 1))
                    ;;
            esac
        done
        [[ $n_image -eq 1 && $n_tag -eq 1 ]] ||
            die "$LOCKFILE must set $IMAGE_KEY and $TAG_KEY once each"

        if [[ $MODE == check ]]; then
            if [[ $pinned_image != "$want_image" ||
                $pinned_tag != "$TAG" ]]; then
                die "$LOCKFILE pins $pinned_image:$pinned_tag, but the" \
                    "tree defines $want_image:$TAG. Run" \
                    "ci/images/image-tag.sh --write $NAME and commit it."
            fi
            echo "$PROG: $LOCKFILE pins $want_image:$TAG" >&2
        else
            for i in "${!LINES[@]}"; do
                case ${LINES[i]} in
                    "$IMAGE_KEY="*) LINES[i]="$IMAGE_KEY=$want_image" ;;
                    "$TAG_KEY="*) LINES[i]="$TAG_KEY=$TAG" ;;
                esac
            done
            # Rewritten in place, so it keeps its mode
            printf '%s\n' "${LINES[@]}" >"$LOCKFILE"
            echo "$PROG: $LOCKFILE now pins $want_image:$TAG" >&2
        fi
        ;;
esac
