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
# If the repository never answers, the script tries each mirror in turn.
# A repository on git.kernel.org, as the default is, is mirrored at the
# same path on kernel.googlesource.com, which comes first; --mirror adds
# others.  git.kernel.org doesn't always answer GitHub's runners.
#
# Usage:
#   fetch-ionic-sources.sh --source-dir DIR --kernel-ref REF
#                          [--patches-dir DIR]... [--repo URL]
#                          [--mirror URL]... [--force]
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
MIRRORS=()
FORCE=false

# How many times git_fetch_retry() tries a fetch, and how long it waits
# before the first retry; the wait doubles after each one.
FETCH_ATTEMPTS=4
FETCH_RETRY_DELAY=10
# How long each attempt waits for the server to answer at all, in seconds.
# git has no connect timeout of its own, and a server it can't reach costs
# about 77 s an attempt, and 13 minutes in all.
FETCH_CONNECT_TIMEOUT=30
# The repository git_fetch_retry() fetches from, and whether one of its
# attempts has reached it
FETCH_URL=""
REACHED_SERVER=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir)  SOURCE_DIR="$2";  shift 2 ;;
        --kernel-ref)  KERNEL_REF="$2";  shift 2 ;;
        --patches-dir) PATCHES_DIRS+=("$2"); shift 2 ;;
        --repo)        KERNEL_REPO="$2"; shift 2 ;;
        --mirror)      MIRRORS+=("$2");  shift 2 ;;
        --force)       FORCE=true;       shift ;;
        -h|--help)
            echo "Usage: $0 --source-dir DIR --kernel-ref REF" \
                 "[--patches-dir DIR]... [--repo URL] [--mirror URL]..." \
                 "[--force]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${SOURCE_DIR}" || -z "${KERNEL_REF}" ]]; then
    echo "ERROR: --source-dir and --kernel-ref are required" >&2
    exit 1
fi

# Google mirrors every repository on git.kernel.org at the same path,
# without the .git
if [[ ${KERNEL_REPO} =~ ^https://git\.kernel\.org/(pub/scm/.+)$ ]]; then
    MIRRORS=("https://kernel.googlesource.com/${BASH_REMATCH[1]%.git}"
             "${MIRRORS[@]}")
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
# The repository goes in as well, because the same ref in another fork can
# be another tree.
tree_digest() {
    local d f
    {
        printf '%s\n' "${KERNEL_REPO}"
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

SENTINEL="${SOURCE_DIR}/.patches-applied-${KERNEL_REF}-$(tree_digest)"

# Whether the server answers within FETCH_CONNECT_TIMEOUT: asked for one
# ref, its answer is small, so a fetch that's slow but working isn't cut
# short.
server_answers() {
    timeout "${FETCH_CONNECT_TIMEOUT}" \
        git ls-remote "${FETCH_URL}" HEAD >/dev/null 2>&1
}

# Run "git fetch" with the given arguments, trying again if it fails.
# git.kernel.org answers with an HTTP 502 now and then, and one such
# answer would otherwise fail the whole run, and with it a CI lane or a
# guest's provisioning.  git exits 128 for a network error and for a ref
# the server refuses alike, so every failure is retried; a refusal only
# costs the waits.  An attempt whose server doesn't answer fails without
# fetching.  Returns 1 once every attempt has failed.
git_fetch_retry() {
    local attempt delay="${FETCH_RETRY_DELAY}"
    for ((attempt = 1; ; attempt++)); do
        if ! server_answers; then
            echo "   ${FETCH_URL} didn't answer within" \
                 "${FETCH_CONNECT_TIMEOUT} s" >&2
        else
            REACHED_SERVER=true
            if git fetch "$@"; then
                return 0
            fi
        fi
        if ((attempt >= FETCH_ATTEMPTS)); then
            echo "   git fetch from ${FETCH_URL} failed" \
                 "${FETCH_ATTEMPTS} times; giving up" >&2
            return 1
        fi
        echo "   git fetch from ${FETCH_URL} failed (attempt ${attempt}" \
             "of ${FETCH_ATTEMPTS}); trying again in ${delay} s" >&2
        sleep "${delay}"
        delay=$((delay * 2))
    done
}

# The Ansible roles' fetch tasks report a change unless they see "nothing
# to do" in this message.
if [ -f "${SENTINEL}" ] && ! ${FORCE}; then
    echo "-- ionic sources already at ${KERNEL_REF} from ${KERNEL_REPO}" \
         "with this series; nothing to do"
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
# repository instead: that also resolves a bare SHA.  If that still fails
# after its retries -- the server may refuse a SHA it doesn't advertise,
# or still be failing -- fall back to fetching every branch and tag.  That
# is far larger: git.kernel.org ignores --filter, so it is the history of
# the whole repository, file contents included.  A repository that never
# answered isn't asked for that, which would fail the same way: the next
# mirror is tried instead.  The mirrors are of the same repository, so the
# same ref names the same commit.
rev=""
for url in "${KERNEL_REPO}" "${MIRRORS[@]}"; do
    FETCH_URL="${url}"
    REACHED_SERVER=false
    git remote set-url origin "${url}"
    if [ "${url}" != "${KERNEL_REPO}" ]; then
        echo "   trying the mirror ${url}"
    fi
    if git_fetch_retry -q --depth 1 --filter=blob:none origin "${KERNEL_REF}"
    then
        rev=FETCH_HEAD
        break
    fi
    if ${REACHED_SERVER}; then
        echo "   could not fetch ${KERNEL_REF} on its own (see git's error);" \
             "fetching every branch and tag instead"
        git_fetch_retry -q --filter=blob:none --tags origin
        rev="${KERNEL_REF}"
        break
    fi
    echo "   ${url} never answered" >&2
done
if [ -z "${rev}" ]; then
    echo "   no repository answered; giving up" >&2
    exit 1
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
