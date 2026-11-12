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
# The lockfile, ci/guest-image.env, pins each image defined here, and
# --check fails unless it names what the tree defines:
#   - the builder as BUILDER_IMAGE (the image in the default registry) and
#     BUILDER_TAG, and so does every workflow line that is only
#     `image: IMAGE:TAG`, IMAGE being the builder in the default registry:
#     the lanes' container images, written out because the security scan
#     can't tell an image named by an expression is pinned;
#   - the guest as GUEST_ARTIFACT_REPO and GUEST_ARTIFACT_TAG, with
#     GUEST_ARTIFACT_INFO_TYPE, the type guest/push.sh attaches its
#     vm-info.json as, and GUEST_ARTIFACT_DISK and GUEST_ARTIFACT_USER,
#     its disk's name and login account, from its pins.
# --check also fails unless the guest boots the kernel the lanes build the
# ionic sources for: its KERNEL_REF must be IONIC_KERNEL_REF, in
# cmake/ErnicKernelModule.cmake.
#
# Usage: image-tag.sh [--ref] NAME
#        image-tag.sh (--check | --write) [NAME]
#   NAME     an image's directory under ci/images/ (builder, guest);
#            --check and --write without one cover every directory
#            holding a pins.env, so one this script doesn't know fails
#            rather than going unchecked
#   --ref    print the full reference, REGISTRY/IMAGE:TAG, not the tag
#   --check  fail unless every pin names the image as the tree defines it
#   --write  make every pin name the image as the tree defines it
#
# CI_IMAGES_REGISTRY names the registry and namespace for --ref (default:
# ghcr.io/rocm/rocm-ernic). The pins always name the default.

set -euo pipefail

PROG=$(basename -- "$0")
IMAGES_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$IMAGES_DIR/../.." && pwd)
LOCKFILE=$REPO_ROOT/ci/guest-image.env
WORKFLOWS=$REPO_ROOT/.github/workflows
DEFAULT_REGISTRY=ghcr.io/rocm/rocm-ernic

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--ref] NAME" >&2
    echo "       $PROG (--check | --write) [NAME]" >&2
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

# Every image, one at a time: each directory holding a pins.env
if [[ -z $NAME ]]; then
    [[ $MODE == check || $MODE == write ]] || usage
    shopt -s nullglob
    all_pins=("$IMAGES_DIR"/*/pins.env)
    shopt -u nullglob
    [[ ${#all_pins[@]} -gt 0 ]] || die "no image is defined in $IMAGES_DIR"
    status=0
    for pins in "${all_pins[@]}"; do
        dir=${pins%/pins.env}
        "$IMAGES_DIR/$PROG" "--$MODE" "${dir##*/}" || status=1
    done
    exit "$status"
fi

# The published image's name for each definition
case $NAME in
    builder) IMAGE=ci-builder ;;
    guest) IMAGE=guest-ionic ;;
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
    guest)
        release=$(pin RELEASE)
        kernel=$(pin KERNEL_REF)
        [[ $release =~ ^[a-z]+$ ]] ||
            die "RELEASE must be an Ubuntu codename: '$release'"
        [[ $kernel =~ ^v([0-9]+(\.[0-9]+)+)$ ]] ||
            die "KERNEL_REF must be a mainline version tag, vX.Y.Z:" \
                "'$kernel'"
        PREFIX="$release-k${BASH_REMATCH[1]}"
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
        want_image="$DEFAULT_REGISTRY/$IMAGE"
        # What the lockfile must say, KEY=VALUE, and whether the
        # workflows' image: lines name the image too
        case $NAME in
            builder)
                WANT=("BUILDER_IMAGE=$want_image" "BUILDER_TAG=$TAG")
                WORKFLOW_PINS=true
                ;;
            guest)
                # The type push.sh attaches vm-info.json as, which the
                # lanes look for
                info_type=$(sed -n 's/^INFO_TYPE=//p' \
                    "$IMAGES_DIR/guest/push.sh")
                [[ $info_type =~ ^application/[a-z0-9.+-]+$ ]] ||
                    die "guest/push.sh sets no INFO_TYPE"
                WANT=(
                    "GUEST_ARTIFACT_REPO=$want_image"
                    "GUEST_ARTIFACT_TAG=$TAG"
                    "GUEST_ARTIFACT_INFO_TYPE=$info_type"
                    "GUEST_ARTIFACT_DISK=$(pin VM_NAME).qcow2"
                    "GUEST_ARTIFACT_USER=$(pin USERNAME)"
                )
                WORKFLOW_PINS=false
                ;;
        esac
        # Each pin naming anything else, as FILE or FILE:LINE
        stale=()

        # The lockfile's lines. Each key must be there once; --write
        # changes it in place and adds nothing.
        mapfile -t LINES <"$LOCKFILE"
        declare -A SEEN=()
        lock_stale=false
        for want in "${WANT[@]}"; do
            key=${want%%=*}
            SEEN[$key]=0
            for i in "${!LINES[@]}"; do
                if [[ ${LINES[i]} == "$key="* ]]; then
                    SEEN[$key]=$((SEEN[$key] + 1))
                    if [[ ${LINES[i]} != "$want" ]]; then
                        lock_stale=true
                        LINES[i]=$want
                    fi
                fi
            done
        done
        keys=()
        once=true
        for want in "${WANT[@]}"; do
            keys+=("${want%%=*}")
            [[ ${SEEN[${want%%=*}]} -eq 1 ]] || once=false
        done
        if ! $once; then
            last=${keys[-1]}
            unset 'keys[-1]'
            list=$(printf '%s, ' "${keys[@]}")
            list=${list%, }
            [[ ${#keys[@]} -lt 2 ]] || list+=","
            die "$LOCKFILE must set $list and $last once each"
        fi
        if $lock_stale; then
            stale+=("${LOCKFILE#"$REPO_ROOT/"}")
            if [[ $MODE == write ]]; then
                # Rewritten in place, so it keeps its mode
                printf '%s\n' "${LINES[@]}" >"$LOCKFILE"
            fi
        fi

        # The workflows' image: lines naming the image, whatever the tag.
        # Only a line that is nothing else is a pin, so --write changes
        # nothing but the tag.
        pin_re='^([[:space:]]*image:[[:space:]]+)([^[:space:]]+)$'
        shopt -s nullglob
        workflows=()
        if $WORKFLOW_PINS; then
            workflows=("$WORKFLOWS"/*.yml "$WORKFLOWS"/*.yaml)
        fi
        for wf in "${workflows[@]}"; do
            mapfile -t LINES <"$wf"
            changed=false
            for i in "${!LINES[@]}"; do
                [[ ${LINES[i]} =~ $pin_re ]] || continue
                indent=${BASH_REMATCH[1]}
                ref=${BASH_REMATCH[2]}
                if [[ $ref == "$want_image:"* &&
                    $ref != "$want_image:$TAG" ]]; then
                    stale+=("${wf#"$REPO_ROOT/"}:$((i + 1))")
                    LINES[i]="$indent$want_image:$TAG"
                    changed=true
                fi
            done
            if [[ $MODE == write ]] && $changed; then
                printf '%s\n' "${LINES[@]}" >"$wf"
            fi
        done

        if [[ $MODE == check ]]; then
            if [[ ${#stale[@]} -gt 0 ]]; then
                die "the tree defines $want_image:$TAG, but these pin" \
                    "another: ${stale[*]}. Run ci/images/image-tag.sh" \
                    "--write $NAME and commit the result."
            fi
            # The lanes build the ionic sources at IONIC_KERNEL_REF into
            # the guest, against its kernel's headers
            if [[ $NAME == guest ]]; then
                cmake=$REPO_ROOT/cmake/ErnicKernelModule.cmake
                [[ -f $cmake ]] || die "$cmake is missing"
                ionic_ref=$(sed -n \
                    's/^set(IONIC_KERNEL_REF "\([^"]*\)".*/\1/p' "$cmake")
                [[ -n $ionic_ref ]] ||
                    die "$cmake doesn't set IONIC_KERNEL_REF"
                [[ $ionic_ref == "$(pin KERNEL_REF)" ]] ||
                    die "the guest boots $(pin KERNEL_REF), its" \
                        "KERNEL_REF in $PINS, but the lanes build the" \
                        "ionic sources at $ionic_ref, IONIC_KERNEL_REF in" \
                        "$cmake. Change one to match the other."
            fi
            echo "$PROG: every pin names $want_image:$TAG" >&2
        else
            echo "$PROG: $want_image:$TAG now pinned;" \
                "changed ${stale[*]:-nothing}" >&2
        fi
        ;;
esac
