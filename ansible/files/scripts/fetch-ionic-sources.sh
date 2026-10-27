#!/bin/bash -eu
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# fetch-ionic-sources.sh
#
# Sparse-clones the two upstream ionic driver subtrees at a pinned kernel
# ref and applies one or more patch series on top.  Used by the CMake
# fetch-ionic-sources target, by the Ansible guest role, and by the
# in-VM driver installer, so all three produce an identical tree.
#
# --patches-dir may be given more than once, and the series are applied in
# the order given.  That is how an optional series -- patches/p4, which
# only a guest that will talk to the P4 engine needs -- is added without
# every caller that wants the base series having to name it.  Giving it at
# all replaces the default rather than adding to it, so a caller that
# names a directory gets exactly what it named.
#
# Usage:
#   fetch-ionic-sources.sh --source-dir DIR --kernel-ref REF
#                          [--patches-dir DIR]... [--repo URL] [--force]
#
# The "-eu" on the shebang is redundant with the "set" below and is there
# for ansible-test's shebang check, which runs over everything in the
# collection and accepts "#!/bin/bash -eu" but not a bare "#!/bin/bash".
# This script is installed onto hosts and guests and may run as root, so
# it keeps an absolute /bin/bash rather than /usr/bin/env.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

SOURCE_DIR=""
KERNEL_REF=""
PATCHES_DIRS=()
KERNEL_REPO="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
FORCE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir)  SOURCE_DIR="$2";  shift 2 ;;
        --kernel-ref)  KERNEL_REF="$2";  shift 2 ;;
        --patches-dir) PATCHES_DIRS+=("$2"); shift 2 ;;
        --repo)        KERNEL_REPO="$2"; shift 2 ;;
        --force)       FORCE=true;       shift ;;
        -h|--help)
            echo "Usage: $0 --source-dir DIR --kernel-ref REF" \
                 "[--patches-dir DIR]... [--repo URL] [--force]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${SOURCE_DIR}" || -z "${KERNEL_REF}" ]]; then
    echo "ERROR: --source-dir and --kernel-ref are required" >&2
    exit 1
fi

if [ ${#PATCHES_DIRS[@]} -eq 0 ]; then
    PATCHES_DIRS=("${SCRIPT_DIR}/../patches")
fi

# Every path is made absolute before anything uses it, because this
# script changes into SOURCE_DIR partway through and every relative path
# then means somewhere else.
#
# That is not hypothetical: a relative --patches-dir passed the check
# below, which runs in the caller's directory, and then matched nothing
# in the glob that applies the series, which runs after the cd. With
# nullglob that loop simply does not execute, so the script applied no
# patches and reported success -- the exact silent no-op the check was
# added to prevent, and the form both READMEs document.
#
# SOURCE_DIR need not exist yet (it is recreated below), so it is
# resolved textually; the patch directories must, so they are resolved
# and validated at once.
case "${SOURCE_DIR}" in
    /*) ;;
    *)  SOURCE_DIR="${PWD}/${SOURCE_DIR}" ;;
esac

for i in "${!PATCHES_DIRS[@]}"; do
    d="${PATCHES_DIRS[i]}"
    if [ ! -d "${d}" ]; then
        echo "ERROR: --patches-dir ${d} is not a directory" >&2
        exit 1
    fi
    PATCHES_DIRS[i]="$(cd "${d}" && pwd)"
done

# The sentinel names the ref AND the series, because the ref alone does not
# identify the tree this produces. SOURCE_DIR outlives a run on a
# persistent host, so with only the ref in the name, editing a patch and
# rerunning against the same kernel reported "nothing to do" and tested the
# previous series -- a change that silently did not take, which is the worst
# shape a CI result can have. Names as well as contents, so deleting or
# renaming a patch counts as a change too.
# Every series contributes, and each directory's name goes in too: naming
# one series and not another is exactly the change this has to notice.
patch_digest() {
    local d f
    {
        for d in "${PATCHES_DIRS[@]}"; do
            printf '%s\n' "${d}"
            for f in "${d}"/*.patch; do
                [ -e "${f}" ] || continue
                printf '%s\n' "${f##*/}"
                cat "${f}"
            done
        done
    } | sha256sum | cut -c1-16
}

SENTINEL="${SOURCE_DIR}/.patches-applied-${KERNEL_REF}-$(patch_digest)"

if [ -f "${SENTINEL}" ] && ! ${FORCE}; then
    echo "-- ionic sources already at ${KERNEL_REF} with this series;" \
         "nothing to do"
    exit 0
fi

echo "-- Fetching ionic sources at ${KERNEL_REF}..."

# Start from a clean checkout so the ref is never a partial mix.
rm -rf "${SOURCE_DIR}"
mkdir -p "${SOURCE_DIR}"
cd "${SOURCE_DIR}"

git init -q .
git remote add origin "${KERNEL_REPO}"
git sparse-checkout init --cone
# include/uapi/rdma carries ionic-abi.h, the userspace ABI the driver and
# rdma-core agree on. It is not optional for the SRQ series, which adds the
# `srqid` field there: without this path a `git am` of those patches fails on
# a file that is not in the checkout, and a DKMS build would otherwise
# compile against the RUNNING kernel's older ionic-abi.h and silently lose
# the new field.
git sparse-checkout set \
    'drivers/net/ethernet/pensando/ionic' \
    'drivers/infiniband/hw/ionic' \
    'include/uapi/rdma'

# "git clone --branch" only accepts a tag or branch, so fetch into an empty
# repository instead: that also resolves a bare SHA.  Fall back to a full
# blobless fetch when the server refuses to serve the object directly.
if git fetch -q --depth 1 --filter=blob:none origin "${KERNEL_REF}"; then
    rev=FETCH_HEAD
else
    echo "   ${KERNEL_REF} is not directly fetchable;" \
         "falling back to a full blobless fetch"
    git fetch -q --filter=blob:none --tags origin
    rev="${KERNEL_REF}"
fi
git checkout -q --detach "${rev}"

echo "-- Applying rocm-ernic patches from ${PATCHES_DIRS[*]}..."
# This is a scratch build tree, and a user with commit.gpgsign=true globally
# cannot sign non-interactively, so signing is disabled for these commits.
# The committer identity is pinned for the same reason: git am refuses to
# commit without one, and the guests this runs in (as root, in a VM with no
# git config) have none.
shopt -s nullglob
for d in "${PATCHES_DIRS[@]}"; do
    for p in "${d}"/*.patch; do
        echo "  applying: ${p}"
        git -c commit.gpgsign=false \
            -c user.name="rocm-ernic build" \
            -c user.email="rocm-ernic@localhost" \
            am --whitespace=fix "${p}"
    done
done
shopt -u nullglob

touch "${SENTINEL}"
echo "-- ionic sources ready in ${SOURCE_DIR}"
