#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# push.sh
#
# Publishes a guest image build.sh made as an OCI artifact, which the
# lanes and scripts/fetch-guest-image.sh pull with oras: the disk,
# compressed with zstd, with vm-info.json attached to it as a referrer.
#
# The tag comes last. The disk is pushed by digest, vm-info.json is
# attached to that digest, and only then is the digest tagged, so a tag
# never names a disk without its vm-info.json, and a push that fails part
# way leaves no tag, so ensure-image.sh builds the image again next time.
#
# Usage: push.sh OUT REF
#   OUT  the directory build.sh wrote the image to
#   REF  REGISTRY/REPOSITORY:TAG to publish it as
#
# The pins come from pins.env through the environment, as ensure-image.sh
# passes them. Needs oras, logged in to the registry, zstd and jq.

set -euo pipefail

PROG=$(basename -- "$0")

# Ours, so nothing else's tooling takes the artifact for its own.
# image-tag.sh --write copies INFO_TYPE into ci/guest-image.env, as
# GUEST_ARTIFACT_INFO_TYPE, which the lanes look for.
ARTIFACT_TYPE=application/vnd.rocm-ernic.vm-image.v1
LAYER_TYPE=application/vnd.rocm-ernic.vm-image.layer.v1+zstd
INFO_TYPE=application/vnd.rocm-ernic.vm-info.v1

die() {
    echo "$PROG: $*" >&2
    exit 1
}

[[ $# -eq 2 ]] || {
    echo "Usage: $PROG OUT REF" >&2
    exit 2
}
OUT=$(cd -- "$1" && pwd)
REF=$2
ref_re='^([a-z0-9][a-z0-9.:-]*(/[a-z0-9][a-z0-9._-]*)+)'
ref_re+=':([A-Za-z0-9_][A-Za-z0-9_.-]{0,127})$'
[[ $REF =~ $ref_re ]] || die "not REGISTRY/REPOSITORY:TAG: '$REF'"
REPO=${BASH_REMATCH[1]}
TAG=${BASH_REMATCH[3]}

[[ -n ${VM_NAME:-} ]] ||
    die "VM_NAME isn't set: pass the pins in pins.env, as ensure-image.sh does"
DISK=$VM_NAME.qcow2
INFO=$OUT/vm-info.json
[[ -f $OUT/$DISK ]] || die "no image at $OUT/$DISK"
[[ -f $INFO ]] || die "no vm-info.json in $OUT"
[[ $(jq -r '.image_path' "$INFO") == "$DISK" ]] ||
    die "$INFO doesn't describe $DISK"
for tool in jq oras zstd; do
    command -v "$tool" >/dev/null || die "$tool isn't on PATH"
done

WORK=$(mktemp -d "$OUT/push-XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT

# --long=27, a 128 MiB window, finds the repeats far apart in a disk
# image; level 12, as above it costs several times as long for a few
# percent. Readers decompress with --long=27 too.
echo "$PROG: compressing $DISK" >&2
zstd -T0 -12 --long=27 -q -o "$WORK/$DISK.zst" -- "$OUT/$DISK"

# What a registry listing shows without fetching the referrer
info() {
    jq -r "$1 // \"\"" "$INFO"
}
OCI=org.opencontainers.image
CREATED=$(info .build_info.build_timestamp)
DESCRIPTION="rocm-ernic CI guest: Ubuntu $(info .release)"
DESCRIPTION+=", Linux $(info .kernel_release)"
ANNOTATIONS=(
    --annotation "$OCI.created=$CREATED"
    --annotation "$OCI.description=$DESCRIPTION"
    --annotation "$OCI.revision=$(info .build_info.source_revision)"
    --annotation "$OCI.source=https://github.com/ROCm/rocm-ernic"
    --annotation "$OCI.version=$TAG"
    --annotation "vm.flavour=$(info .flavour)"
    --annotation "vm.kernel_release=$(info .kernel_release)"
    --annotation "vm.release=$(info .release)"
    --annotation "vm.username=$(info .username)"
)

# From the directory, so the layer's title is the file's name
echo "$PROG: pushing $DISK.zst to $REPO" >&2
DIGEST=$(cd "$WORK" && oras push --artifact-type "$ARTIFACT_TYPE" \
    "${ANNOTATIONS[@]}" --format go-template='{{.digest}}' \
    "$REPO" "$DISK.zst:$LAYER_TYPE")
[[ $DIGEST =~ ^sha256:[0-9a-f]{64}$ ]] ||
    die "oras push didn't give the digest: '$DIGEST'"

echo "$PROG: attaching vm-info.json to $REPO@$DIGEST" >&2
(cd "$OUT" && oras attach --artifact-type "$INFO_TYPE" \
    --annotation "$OCI.created=$CREATED" \
    "$REPO@$DIGEST" vm-info.json:application/json) >&2

oras tag "$REPO@$DIGEST" "$TAG" >&2
echo "$PROG: published $REF ($DIGEST)" >&2
