#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ensure-image.sh
#
# Makes sure a CI image is published under the tag image-tag.sh gives it.
# If the registry already has that tag, it does nothing: a tag names one
# build, and is never built or pushed again. If the registry says the tag
# doesn't exist, it builds the image from ci/images/NAME/, with every pin
# in its pins.env as a build argument, runs NAME/check.sh inside it, and
# only then pushes it, unless by then the registry has it, which another
# run building the same image at the same time can cause. An image
# without a check.sh isn't built at all. Any other answer from the
# registry, such as a refused login or a network error, is a failure
# rather than a reason to build or push, so a published tag can't be
# overwritten by mistake.
#
# The build caches its layers in the registry, as IMAGE:buildcache, so an
# image whose pins haven't changed rebuilds in minutes.
#
# On success it prints the image's reference, REGISTRY/IMAGE:TAG.
#
# Usage: ensure-image.sh [--build-only] NAME
#   NAME          an image's directory under ci/images/ (builder)
#   --build-only  build and check the image locally, under its tag, and
#                 neither look in the registry nor push; with podman if
#                 there's no docker with buildx
#
# Environment:
#   CI_IMAGES_REGISTRY  registry and namespace (default:
#                       ghcr.io/rocm/rocm-ernic)
#   CI_IMAGES_CAN_PUSH  false where a push can't work, as in a pull
#                       request from a fork, so a missing image fails
#                       with a message saying so (default: true)
#   CI_IMAGES_ROOT_CA   with --build-only, a root certificate for the
#                       build to trust, for a build behind a proxy that
#                       re-signs TLS, such as the AMD network's
#
# Publishing needs docker with buildx and a docker-container builder (the
# docker driver can't export a registry cache), logged in to the
# registry.

set -euo pipefail

PROG=$(basename -- "$0")
IMAGES_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--build-only] NAME" >&2
    exit 2
}

BUILD_ONLY=false
NAME=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --build-only)
            BUILD_ONLY=true
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

case ${CI_IMAGES_CAN_PUSH:-true} in
    true | false) ;;
    *) die "CI_IMAGES_CAN_PUSH must be true or false" ;;
esac

# Checks NAME, and pins.env's format, before anything runs
REF=$("$IMAGES_DIR/image-tag.sh" --ref "$NAME")
REPO=${REF%:*}
DIR="$IMAGES_DIR/$NAME"
PINS="$DIR/pins.env"

# Every image is checked before it's published, so one without a check
# isn't built at all, rather than published unchecked
[[ -f $DIR/check.sh ]] || die "$DIR/check.sh is missing"

BUILD_ARGS=()
while IFS= read -r line || [[ -n $line ]]; do
    if [[ -n $line && $line != \#* ]]; then
        BUILD_ARGS+=(--build-arg "$line")
    fi
done <"$PINS"

# The image trusts the certificate from then on, so a published image
# mustn't carry one. Its SHA-256 goes in as a build argument too, because
# a secret isn't part of the build cache's key, and without it a build
# could reuse a cached layer made with a different certificate, or none.
ROOT_CA_SHA256=none
if [[ -n ${CI_IMAGES_ROOT_CA:-} ]]; then
    $BUILD_ONLY || die "CI_IMAGES_ROOT_CA is only for --build-only"
    [[ -f $CI_IMAGES_ROOT_CA && -r $CI_IMAGES_ROOT_CA ]] ||
        die "CI_IMAGES_ROOT_CA isn't a readable file: $CI_IMAGES_ROOT_CA"
    ROOT_CA_SHA256=$(sha256sum <"$CI_IMAGES_ROOT_CA")
    ROOT_CA_SHA256=${ROOT_CA_SHA256%% *}
    BUILD_ARGS+=(--secret "id=root-ca,src=$CI_IMAGES_ROOT_CA")
fi
BUILD_ARGS+=(--build-arg "ROOT_CA_SHA256=$ROOT_CA_SHA256")

# docker with buildx, or for --build-only, podman. podman-docker's
# `docker` is podman, whose `buildx` is only an alias of its build, so
# it's told apart by its version.
ENGINE=docker
docker_version=$(docker --version 2>/dev/null) || docker_version=""
if [[ $docker_version != Docker* ]] ||
    ! docker buildx version >/dev/null 2>&1; then
    if $BUILD_ONLY && command -v podman >/dev/null; then
        ENGINE=podman
    else
        die "docker with buildx not found"
    fi
fi

# Run the image's own checks inside it
check_image() {
    local mount="$DIR/check.sh:/check.sh:ro"
    if [[ $ENGINE == podman ]]; then
        # Relabel the mount for SELinux, where podman runs confined
        mount+=",z"
    fi
    echo "$PROG: checking $REF" >&2
    "$ENGINE" run --rm --env-file "$PINS" -v "$mount" "$REF" \
        bash /check.sh >&2
}

if $BUILD_ONLY; then
    echo "$PROG: building $REF locally with $ENGINE" >&2
    if [[ $ENGINE == podman ]]; then
        podman build --file "$DIR/Dockerfile" "${BUILD_ARGS[@]}" \
            --tag "$REF" "$DIR" >&2
    else
        docker buildx build --file "$DIR/Dockerfile" "${BUILD_ARGS[@]}" \
            --load --tag "$REF" "$DIR" >&2
    fi
    check_image
    printf '%s\n' "$REF"
    exit 0
fi

# Whether the registry has the image: true if it has, false if it says
# there's no such tag or no such repository. Any other answer is fatal.
#   $1  what not to do if the answer is unclear, for the message
published() {
    local answer status=0
    answer=$(docker buildx imagetools inspect "$REF" 2>&1) || status=$?
    if [[ $status -eq 0 ]]; then
        return 0
    fi
    if [[ $answer != *": not found"* && $answer != *MANIFEST_UNKNOWN* &&
        $answer != *NAME_UNKNOWN* ]]; then
        printf '%s\n' "$answer" >&2
        die "couldn't tell whether $REF is published, so it isn't $1"
    fi
    return 1
}

if published built; then
    echo "$PROG: $REF is already published" >&2
    printf '%s\n' "$REF"
    exit 0
fi

if [[ ${CI_IMAGES_CAN_PUSH:-true} == false ]]; then
    die "$REF isn't published, and this run can't publish it. A pull" \
        "request from a fork can't push images: ask a maintainer to push" \
        "the branch to ROCm/rocm-ernic, which builds it."
fi

echo "$PROG: $REF isn't published; building it" >&2
docker buildx build --file "$DIR/Dockerfile" "${BUILD_ARGS[@]}" \
    --cache-from "type=registry,ref=$REPO:buildcache" \
    --cache-to "type=registry,ref=$REPO:buildcache,mode=max" \
    --load --tag "$REF" "$DIR" >&2
check_image
# Another run may have published it while this one built. Then the tag
# already names a checked build, and stays that one. Nothing makes this
# look and the push one step, but a race needs two runs to finish
# building the same new image within a few seconds, and the worst it does
# is publish two checked builds of one definition under the tag in turn.
if published pushed; then
    echo "$PROG: $REF was published while this run built it;" \
        "not pushing" >&2
    printf '%s\n' "$REF"
    exit 0
fi
docker push "$REF" >&2
echo "$PROG: published $REF" >&2
printf '%s\n' "$REF"
