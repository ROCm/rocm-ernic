#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# fetch-guest-image.sh
#
# Pulls the prebuilt guest qcow2 and its metadata from the ORAS artifact
# registry.  Used by ci/lib/common.sh and by hand, so both land the
# same bytes in the same layout as .github/actions/fetch-guest-vm,
# which does this for the hosted jobs.
#
# The image is the "ionic" flavour published by
# https://github.com/sbates130272/batesste-ci-images.
#
# On success DEST holds the decompressed qcow2, vm-info.json, id_rsa and
# id_rsa.pub, and k=v lines naming them are written to stdout.
#
# Usage:
#   fetch-guest-image.sh --tag TAG --dest DIR
#                        [--repo REF] [--info-type TYPE] [--force]

set -euo pipefail

REPO="docker.io/sbates130272/batesste-ci-images-ubuntu-qcow2-gen-ionic"
TAG=""
DEST=""
INFO_TYPE="application/vnd.batesste.vm-info.v1"
FORCE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)      REPO="$2";      shift 2 ;;
        --tag)       TAG="$2";       shift 2 ;;
        --dest)      DEST="$2";      shift 2 ;;
        --info-type) INFO_TYPE="$2"; shift 2 ;;
        --force)     FORCE=true;     shift ;;
        -h|--help)
            echo "Usage: $0 --tag TAG --dest DIR" \
                 "[--repo REF] [--info-type TYPE] [--force]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

[ -n "$TAG" ]  || { echo "✗ --tag is required" >&2; exit 1; }
[ -n "$DEST" ] || { echo "✗ --dest is required" >&2; exit 1; }

for tool in oras jq zstd qemu-img; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "✗ $tool is not on PATH" >&2; exit 1; }
done

REF="${REPO}:${TAG}"
STAMP="${DEST}/.artifact-tag"

retry() {
    local rc=0
    for i in 1 2 3; do
        if "$@"; then
            return 0
        else
            rc=$?
        fi
        echo "Command failed (attempt $i/3): $*" >&2
        sleep 5
    done
    return "$rc"
}

# Emit the k=v lines a caller parses.  Done from whatever is on disk, so
# the cached path and the freshly-pulled path report identically.
report() {
    local disk
    disk=$(jq -r '.image_path // ""' "${DEST}/vm-info.json")
    disk=$(basename -- "$disk")
    echo "dir=${DEST}"
    echo "disk=${DEST}/${disk}"
    echo "user=$(jq -r '.username // ""' "${DEST}/vm-info.json")"
    echo "key=${DEST}/id_rsa"
    echo "pubkey=${DEST}/id_rsa.pub"
    echo "kernel=$(jq -r '.kernel_release // ""' "${DEST}/vm-info.json")"
    echo "flavour=$(jq -r '.flavour // ""' "${DEST}/vm-info.json")"
}

# A 3.7 GB pull per run is not acceptable for a per-commit CI job, so the
# stamp records which tag the directory holds.  Tags are pinned and
# date-prefixed, and DEST is tag-scoped by every caller, so a matching
# stamp with the disk present means there is nothing to do.
if [ "$FORCE" = false ] && [ -f "$STAMP" ] \
   && [ "$(cat "$STAMP")" = "$TAG" ] && [ -f "${DEST}/vm-info.json" ]; then
    CACHED=$(jq -r '.image_path // ""' "${DEST}/vm-info.json")
    if [ -f "${DEST}/$(basename -- "$CACHED")" ]; then
        echo "=== ${TAG} already present in ${DEST} ===" >&2
        report
        exit 0
    fi
fi

mkdir -p "$DEST"
cd "$DEST"

echo "=== Fetching metadata for ${REF} ===" >&2
# Referrer first: it is kilobytes, so a bad reference fails in seconds
# rather than after a 3.7 GB download.
DIGEST=$(retry oras discover --format json "$REF" \
    | jq -r --arg t "$INFO_TYPE" \
        '[.referrers[] | select(.artifactType==$t)]
         | sort_by(.annotations["org.opencontainers.image.created"])
         | last | .digest')
if [ -z "$DIGEST" ] || [ "$DIGEST" = "null" ]; then
    echo "✗ ${REF} has no ${INFO_TYPE} referrer" >&2
    exit 1
fi
retry oras pull "${REPO}@${DIGEST}"

# oras does not preserve modes; OpenSSH refuses a group-readable key.
# Take the key name from the metadata for the same reason the disk name
# is globbed below: a rename upstream should not abort under set -e on a
# hardcoded filename.
KEY=$(jq -r '.ssh_keys.private_key_path // ""' vm-info.json)
KEY=$(basename -- "${KEY:-id_rsa}")
if [ ! -f "$KEY" ]; then
    echo "✗ referrer carries no private key named ${KEY}" >&2
    ls -la >&2
    exit 1
fi
chmod 600 "$KEY"

echo "=== Fetching disk for ${REF} ===" >&2
retry oras pull "$REF"

# Take the disk name from what actually landed rather than pinning it,
# so a renamed guest fails here instead of at first boot.
shopt -s nullglob
ZST=(*.qcow2.zst)
if [ ${#ZST[@]} -ne 1 ]; then
    echo "✗ expected exactly one *.qcow2.zst, got: ${ZST[*]:-none}" >&2
    ls -la >&2
    exit 1
fi

# --long=27 matches the compressor's window, which zstd otherwise refuses
# as needing too much memory to decode.  Unlike the hosted action this
# keeps the .zst: on a persistent host the 3.7 GB is cheaper than
# re-pulling it, and --force is the way to redo the decompression.
zstd -d --long=27 -q -f "${ZST[0]}" -o "${ZST[0]%.zst}"

# Fail here rather than at boot if the download truncated.
qemu-img check -q "${ZST[0]%.zst}" >/dev/null

echo "$TAG" > "$STAMP"
report
