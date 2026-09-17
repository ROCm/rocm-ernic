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
# The image's own metadata is then checked against this checkout's pins
# -- see verify_image() below.  Those assertions used to live in the
# Ansible play ansible/playbooks/vm-fetch.yml, which was removed in
# 0.2.0; they belong here, where every caller gets them.
#
# Usage:
#   fetch-guest-image.sh --tag TAG --dest DIR
#                        [--repo REF] [--info-type TYPE] [--force]
#                        [--project-root DIR] [--min-kernel VER]
#                        [--expect-user U] [--expect-disk NAME]
#                        [--expect-release R] [--expect-flavour F]
#                        [--no-verify]

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

REPO="docker.io/sbates130272/batesste-ci-images-ubuntu-qcow2-gen-ionic"
TAG=""
DEST=""
INFO_TYPE="application/vnd.batesste.vm-info.v1"
FORCE=false

# Where README.md and cmake/ErnicKernelModule.cmake are read from.
PROJECT_ROOT="$(dirname -- "$SCRIPT_DIR")"
# drivers/infiniband/hw/ionic was merged for 6.18.  Kept in step with
# ernic_ionic_min_kernel in ansible/group_vars/all.yml and
# ansible/roles/ernic_guest_setup/defaults/main.yml, which assert the
# same floor against the running guest.
MIN_KERNEL="6.18"
# Optional site expectations.  Empty means "do not check"; the values
# the self-hosted CI cares about are passed by ci/lib/common.sh.
EXPECT_USER=""
EXPECT_DISK=""
EXPECT_RELEASE=""
EXPECT_FLAVOUR=""
VERIFY=true

usage() {
    cat <<EOF
Usage: $0 --tag TAG --dest DIR [options]

  --repo REF           artifact repository (default: ${REPO})
  --info-type TYPE     vm-info referrer artifactType
  --force              re-pull and re-decompress even on a cache hit

Verification (on by default, on the cached path too):
  --project-root DIR   where README.md and cmake/ live
  --min-kernel VER     guest kernel floor (default: ${MIN_KERNEL})
  --expect-user U      assert vm-info username
  --expect-disk NAME   assert vm-info image_path basename
  --expect-release R   assert vm-info release
  --expect-flavour F   assert vm-info flavour
  --no-verify          skip all of the above
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)           REPO="$2";           shift 2 ;;
        --tag)            TAG="$2";            shift 2 ;;
        --dest)           DEST="$2";           shift 2 ;;
        --info-type)      INFO_TYPE="$2";      shift 2 ;;
        --project-root)   PROJECT_ROOT="$2";   shift 2 ;;
        --min-kernel)     MIN_KERNEL="$2";     shift 2 ;;
        --expect-user)    EXPECT_USER="$2";    shift 2 ;;
        --expect-disk)    EXPECT_DISK="$2";    shift 2 ;;
        --expect-release) EXPECT_RELEASE="$2"; shift 2 ;;
        --expect-flavour) EXPECT_FLAVOUR="$2"; shift 2 ;;
        --force)          FORCE=true;          shift ;;
        --no-verify)      VERIFY=false;        shift ;;
        -h|--help)        usage; exit 0 ;;
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
# Absolute, because the pull path cds into DEST and report() and
# verify_image() both address it by path afterwards.
mkdir -p "$DEST"
DEST=$(cd -- "$DEST" && pwd)
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

fail() {
    echo "✗ $1" >&2
    shift
    local line
    for line in "$@"; do echo "  ${line}" >&2; done
    exit 1
}

# Numeric dotted-version compare: ver_ge A B is true when A >= B.
# A string compare puts 6.9 above 6.18, which is exactly the pair that
# matters here.
ver_ge() {
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

major_minor() { cut -d. -f1,2 <<<"$1"; }

# ── Metadata verification ────────────────────────────────────────────
#
# The image is the source of truth for what is inside it, but not for
# whether that is the image this checkout wants.  Comparing the two
# turns a wrong --tag into a five-second failure here, instead of a
# DKMS build failure inside a guest that has already booted, or an SSH
# timeout three Ansible plays later.
#
# This runs on the cache hit as well, and deliberately: bumping
# IONIC_KERNEL_REF or editing the README badge invalidates a directory
# that was perfectly correct when it was pulled.  The checks are all
# local file reads, so the cheap path stays cheap -- and on the pull path
# they run between the referrer and the disk, which is early enough that
# a rejection costs kilobytes.
#
# Missing repo files are fatal rather than skipped.  A check that
# silently does nothing is worse than no check, because it still reads
# as coverage -- pass --no-verify to opt out explicitly.
verify_image() {
    local info="${DEST}/vm-info.json"
    local kernel kver release username flavour disk
    local cmake ref refver readme badge key

    [ -f "$info" ] || fail "no vm-info.json in ${DEST}"

    kernel=$(jq -r '.kernel_release // ""'        "$info")
    release=$(jq -r '.release // ""'              "$info")
    username=$(jq -r '.username // ""'            "$info")
    flavour=$(jq -r '.flavour // ""'              "$info")
    key=$(jq -r '.ssh_keys.private_key_path // ""' "$info")
    disk=$(basename -- "$(jq -r '.image_path // ""' "$info")")

    [ -n "$kernel" ] || fail "vm-info.json for ${TAG} carries no kernel_release"
    # 7.2.3-061203-generic -> 7.2.3
    kver="${kernel%%-*}"

    # 1. The floor.  Below 6.18 there is no drivers/infiniband/hw/ionic,
    #    so the guest cannot build the RDMA half of the stack at all.
    ver_ge "$kver" "$MIN_KERNEL" || fail \
        "image kernel ${kernel} is older than ${MIN_KERNEL}" \
        "It carries no drivers/infiniband/hw/ionic and cannot build ionic_rdma."

    # 2. The floor is necessary but not sufficient.  The ionic sources
    #    track IB-core helpers that move between minor releases, so the
    #    guest kernel's major.minor must equal the pinned ref's:
    #    v7.2.4 sources on a 7.0 kernel fail on ib_umem_get_va.
    cmake="${PROJECT_ROOT}/cmake/ErnicKernelModule.cmake"
    [ -f "$cmake" ] || fail \
        "no cmake/ErnicKernelModule.cmake under ${PROJECT_ROOT}" \
        "Pass --project-root, or --no-verify to skip these checks."
    ref=$(sed -n 's/.*set(IONIC_KERNEL_REF[[:space:]]*"\([^"]*\)".*/\1/p' \
          "$cmake" | head -n1)
    [ -n "$ref" ] || fail "IONIC_KERNEL_REF is not set in ${cmake}"
    # A vX.Y[.Z] tag also names a kernel version; a bare SHA does not,
    # and then there is nothing to compare.  Same test that
    # ansible/roles/ernic_source/tasks/ionic_ref.yml makes.
    if [[ "$ref" =~ ^v[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
        refver="${ref#v}"
        [ "$(major_minor "$kver")" = "$(major_minor "$refver")" ] || fail \
            "image kernel ${kernel} does not match the ionic sources pinned at ${ref}" \
            "ionic_rdma needs a $(major_minor "$refver").x kernel." \
            "Repin IONIC_KERNEL_REF in cmake/ErnicKernelModule.cmake," \
            "or pin a --tag whose image matches it."
    else
        echo "  (IONIC_KERNEL_REF is ${ref}, which names no kernel" \
             "version; skipping the skew check)" >&2
    fi

    # 3. README.md line 9 hardcodes the guest kernel in a shields.io
    #    badge, because shields.io has no way to read the image.  That
    #    makes it the one copy with nothing tying it to the tag: bump
    #    the tag and the front page advertises the old kernel forever.
    readme="${PROJECT_ROOT}/README.md"
    [ -f "$readme" ] || fail \
        "no README.md under ${PROJECT_ROOT}" \
        "Pass --project-root, or --no-verify to skip these checks."
    badge=$(sed -n 's/.*CI%20guest%20kernel-\([0-9][0-9.]*\)-.*/\1/p' \
            "$readme" | head -n1)
    [ -n "$badge" ] || fail \
        "README.md carries no 'CI guest kernel' badge to check ${kernel} against"
    [ "$badge" = "$kver" ] || fail \
        "README.md advertises CI guest kernel ${badge}, but ${TAG} ships ${kernel}" \
        "Update the badge on README.md line 9." \
        "Leave docs/performance.rst alone: its Reference Measurements are a" \
        "historical record of a run on the old guest, not a claim about this one."

    # 4. Site expectations, when the caller supplied them.  These are
    #    what ci/lib/common.sh and ansible/group_vars/all.yml hardcode
    #    alongside the tag, and so are what can drift away from it.
    [ -z "$EXPECT_USER" ] || [ "$username" = "$EXPECT_USER" ] || fail \
        "image login account is '${username}', expected '${EXPECT_USER}'"
    [ -z "$EXPECT_DISK" ] || [ "$disk" = "$EXPECT_DISK" ] || fail \
        "image disk is '${disk}', expected '${EXPECT_DISK}'"
    [ -z "$EXPECT_RELEASE" ] || [ "$release" = "$EXPECT_RELEASE" ] || fail \
        "image release is '${release}', expected '${EXPECT_RELEASE}'"
    [ -z "$EXPECT_FLAVOUR" ] || [ "$flavour" = "$EXPECT_FLAVOUR" ] || fail \
        "image flavour is '${flavour}', expected '${EXPECT_FLAVOUR}'" \
        "An 'ionic' image ships no ROCm, so the GPU roles fail deep in rocm-smi."

    # 5. The key has to be called id_rsa, because report(), this script's
    #    header and CI_VM_SSH_IDENTITY in ci/lib/common.sh all say it is.
    #    A rename upstream would otherwise be silent: vm_ssh's -i guard
    #    drops the missing identity and every login fails on publickey
    #    after the VM has already booted.
    key=$(basename -- "${key:-id_rsa}")
    [ "$key" = id_rsa ] || fail \
        "image declares its private key as '${key}', not id_rsa" \
        "report(), the script header and CI_VM_SSH_IDENTITY all name id_rsa."
    # oras does not preserve modes and OpenSSH refuses a group-readable
    # key.  The pull path chmods what it pulled; this also covers a
    # directory somebody populated by hand.
    [ -f "${DEST}/${key}" ] || fail "no private key at ${DEST}/${key}"
    chmod 600 "${DEST}/${key}"

    echo "✓ ${TAG}: ${release}, kernel ${kernel}, user ${username}," \
         "flavour ${flavour}" >&2
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
        [ "$VERIFY" = false ] || verify_image
        report
        exit 0
    fi
fi

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

# Here rather than after the disk pull.  Every assertion reads vm-info.json
# or a file in this checkout, all of which are on disk now, and a rejection
# is deterministic for the checkout -- re-pulling identical bytes cannot
# change the verdict.  Checking first means a stale badge or an unbumped
# IONIC_KERNEL_REF costs seconds instead of 3.7 GB, twice: once to fail,
# once more on the retry, since the failure exits before the stamp and
# oras re-downloads blobs that are already in the directory.
[ "$VERIFY" = false ] || verify_image

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
