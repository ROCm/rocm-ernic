#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# fetch-p4c.sh
#
# Put a working p4c into a private directory, so the P4 programs under
# tests/data/p4/ can be compiled from their .p4 sources rather than
# hand-written as JSON.
#
# Nothing is installed and nothing on the system changes.  The compiler
# ends up behind a wrapper that sets its own library path, and the whole
# tree can be deleted.
#
# Why a private copy rather than apt: p4lang publishes p4c for Ubuntu up to
# 23.04 and nothing newer, and the binary wants libboost_iostreams 1.74,
# which a current Ubuntu does not carry.  That one library is fetched
# beside it.
#
# Integrity comes from the pinned SHA-256 of each artifact, not from the
# transport.  That is deliberate: one of the two hosts redirects to a
# mirror whose certificate a TLS-inspecting corporate proxy will not
# validate, so HTTPS is tried first and plain HTTP accepted as a fallback.
# A changed or tampered artifact fails the checksum and stops the script
# either way, which is the control that actually matters.
#
# Usage:
#   scripts/fetch-p4c.sh [--prefix DIR] [--force]
#
# Then:
#   <prefix>/bin/p4c-bm2-ss --std p4-16 -I <prefix>/p4include \
#       -o program.json program.p4
#
# Add --emit-externs for a program that calls a target-specific extern;
# without it p4c refuses one it does not know.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/../build-p4c"
FORCE=false

# Pinned artifacts.  The p4c package is the newest p4lang publishes; the
# boost one is the single library it needs that a current Ubuntu lacks.
P4C_URL="https://download.opensuse.org/repositories/home:/p4lang/xUbuntu_23.04/amd64/p4lang-p4c_1.2.4.2-3_amd64.deb"
P4C_SHA="e78eda050cb7de510735ab5c8dde2cc6f946d801191b6a65a02072acc122b71a"
BOOST_URL="https://archive.ubuntu.com/ubuntu/pool/main/b/boost1.74/libboost-iostreams1.74.0_1.74.0-14ubuntu3_amd64.deb"
BOOST_SHA="144d169144544d103beba7da13b6d21d6bf4335d58e753aa91bf654b4b8f39ca"

usage() {
    cat <<EOF
Usage: $0 [--prefix DIR] [--force]

  --prefix DIR  Where to put the compiler (default: ${PREFIX})
  --force       Re-fetch even if a compiler is already there
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --force)  FORCE=true;  shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "$0: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Everything below goes into a file that is read from somewhere else: the
# bin/ wrappers name the real binary and boost's directory, and the
# p4include symlink names a target resolved relative to its own
# directory, not to the caller's. A relative --prefix put a cwd-relative
# path in all three, so the symlink pointed at <prefix>/<prefix>/... and
# the wrappers ran only from the directory the fetch happened in. The
# --version check at the end runs from that same directory, so it would
# have reported success either way.
#
# The prefix need not exist yet -- it is removed and recreated below --
# so it is made absolute textually rather than by cd'ing into it.
case "${PREFIX}" in
    /*) ;;
    *)  PREFIX="${PWD}/${PREFIX}" ;;
esac

die() { echo "$0: $*" >&2; exit 1; }

for tool in curl sha256sum dpkg-deb; do
    command -v "$tool" >/dev/null 2>&1 || die "need $tool"
done

if [[ -x "${PREFIX}/bin/p4c-bm2-ss" ]] && [[ "${FORCE}" != true ]]; then
    echo "p4c is already at ${PREFIX}; pass --force to re-fetch"
    "${PREFIX}/bin/p4c-bm2-ss" --version
    exit 0
fi

# Each archive is fetched into its own empty directory and only ever read
# from there, so nothing it contains can be picked up as a script or a
# module by something run later.
rm -rf "${PREFIX}"
mkdir -p "${PREFIX}/download" "${PREFIX}/root" "${PREFIX}/lib"

fetch() {
    local url="$1" want="$2" out="$3" got

    echo "fetching $(basename "${out}")"
    if ! curl -fsSL --max-time 300 -o "${out}" "${url}"; then
        # See the note at the top of this file: the checksum below is what
        # establishes the artifact is the right one, so falling back to
        # http costs nothing that a verified download would have given.
        local plain="${url/#https:/http:}"

        [[ "${plain}" != "${url}" ]] || die "download failed: ${url}"
        echo "  https failed; retrying over http, checksum still checked"
        curl -fsSL --max-time 300 -o "${out}" "${plain}" ||
            die "download failed: ${url}"
    fi
    got="$(sha256sum "${out}" | cut -d' ' -f1)"
    if [[ "${got}" != "${want}" ]]; then
        die "checksum mismatch for ${url}
  expected ${want}
  got      ${got}
The published artifact has changed. Do not use it until the new one has
been checked; update the pin in this script deliberately."
    fi
}

fetch "${P4C_URL}"   "${P4C_SHA}"   "${PREFIX}/download/p4c.deb"
fetch "${BOOST_URL}" "${BOOST_SHA}" "${PREFIX}/download/boost.deb"

dpkg-deb -x "${PREFIX}/download/p4c.deb"   "${PREFIX}/root"
dpkg-deb -x "${PREFIX}/download/boost.deb" "${PREFIX}/lib"

BOOST_DIR="$(dirname "$(find "${PREFIX}/lib" -name 'libboost_iostreams.so.*' \
    -print -quit)")"
[[ -n "${BOOST_DIR}" ]] || die "the boost package did not contain the library"

INCLUDE_DIR="$(find "${PREFIX}/root" -type d -name p4include -print -quit)"
[[ -n "${INCLUDE_DIR}" ]] || die "the p4c package did not contain p4include"
ln -sfn "${INCLUDE_DIR}" "${PREFIX}/p4include"

mkdir -p "${PREFIX}/bin"
for prog in p4c p4c-bm2-ss; do
    target="${PREFIX}/root/usr/bin/${prog}"
    [[ -x "${target}" ]] || die "the p4c package did not contain ${prog}"
    # A wrapper rather than a symlink: the binary needs a library path
    # that is not on the system, and nothing outside this tree should
    # have to know that.
    cat > "${PREFIX}/bin/${prog}" <<EOF
#!/usr/bin/env bash
# Generated by scripts/fetch-p4c.sh.  Do not edit.
export LD_LIBRARY_PATH="${BOOST_DIR}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
exec "${target}" "\$@"
EOF
    chmod 0755 "${PREFIX}/bin/${prog}"
done

echo
"${PREFIX}/bin/p4c-bm2-ss" --version ||
    die "the compiler does not run; the package may need a library this
system does not have"

cat <<EOF

p4c is at ${PREFIX}/bin.  Compile a program with:

  ${PREFIX}/bin/p4c-bm2-ss --std p4-16 \\
      -I ${PREFIX}/p4include \\
      -o out.json in.p4

Add --emit-externs for a program that calls a target-specific extern.
EOF
