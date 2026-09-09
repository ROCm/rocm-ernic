#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fetch-ionic-sources.sh
#
# Sparse-clones the two upstream ionic driver subtrees at a pinned kernel
# ref and applies patches/*.patch on top.  Used by the CMake
# fetch-ionic-sources target, by the Ansible guest role, and by the
# in-VM driver installer, so all three produce an identical tree.
#
# Usage:
#   fetch-ionic-sources.sh --source-dir DIR --kernel-ref REF
#                          [--patches-dir DIR] [--repo URL] [--force]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

SOURCE_DIR=""
KERNEL_REF=""
PATCHES_DIR="${SCRIPT_DIR}/../patches"
KERNEL_REPO="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
FORCE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir)  SOURCE_DIR="$2";  shift 2 ;;
        --kernel-ref)  KERNEL_REF="$2";  shift 2 ;;
        --patches-dir) PATCHES_DIR="$2"; shift 2 ;;
        --repo)        KERNEL_REPO="$2"; shift 2 ;;
        --force)       FORCE=true;       shift ;;
        -h|--help)
            echo "Usage: $0 --source-dir DIR --kernel-ref REF" \
                 "[--patches-dir DIR] [--repo URL] [--force]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${SOURCE_DIR}" || -z "${KERNEL_REF}" ]]; then
    echo "ERROR: --source-dir and --kernel-ref are required" >&2
    exit 1
fi

SENTINEL="${SOURCE_DIR}/.patches-applied-${KERNEL_REF}"

if [ -f "${SENTINEL}" ] && ! ${FORCE}; then
    echo "-- ionic sources already at ${KERNEL_REF}; nothing to do"
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
git sparse-checkout set \
    'drivers/net/ethernet/pensando/ionic' \
    'drivers/infiniband/hw/ionic'

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

echo "-- Applying rocm-ernic patches from ${PATCHES_DIR}..."
# This is a scratch build tree, and a user with commit.gpgsign=true globally
# cannot sign non-interactively, so signing is disabled for these commits.
shopt -s nullglob
for p in "${PATCHES_DIR}"/*.patch; do
    echo "  applying: ${p}"
    git -c commit.gpgsign=false am --whitespace=fix "${p}"
done
shopt -u nullglob

touch "${SENTINEL}"
echo "-- ionic sources ready in ${SOURCE_DIR}"
