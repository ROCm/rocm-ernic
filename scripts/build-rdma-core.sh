#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build rdma-core with the rocm_ernic provider.
# Downloads the upstream tarball, injects the
# provider via apply-rocm-ernic-dv.sh, and builds.
#
# Usage:
#   build-rdma-core.sh \
#     <build_dir> <install_dir> \
#     [nproc] [--build-only]
#
# Environment:
#   RDMA_CORE_VERSION - upstream version
#                       (default 62.0)

set -euo pipefail

BUILD_DIR="$(realpath -m "$1")"
INSTALL_DIR="$(realpath -m "$2")"
NPROC="${3:-$(nproc)}"
BUILD_ONLY=false
if [ "${4:-}" = "--build-only" ]; then
  BUILD_ONLY=true
fi

VERSION="${RDMA_CORE_VERSION:-62.0}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APPLY_SCRIPT="${PROJECT_DIR}/rdma-core"
APPLY_SCRIPT="${APPLY_SCRIPT}/rocm-ernic-dv"
APPLY_SCRIPT="${APPLY_SCRIPT}/apply-rocm-ernic-dv.sh"

TARBALL_URL="https://github.com/linux-rdma"
TARBALL_URL="${TARBALL_URL}/rdma-core"
TARBALL_URL="${TARBALL_URL}/releases/download"
TARBALL_URL="${TARBALL_URL}/v${VERSION}"
TARBALL_URL="${TARBALL_URL}/rdma-core-${VERSION}.tar.gz"

SRC_DIR="${BUILD_DIR}/rdma-core-${VERSION}"
TARBALL="${BUILD_DIR}/rdma-core-${VERSION}.tar.gz"
CMAKE_BUILD="${BUILD_DIR}/cmake-build"

echo "=== build-rdma-core ==="
echo "  version  : ${VERSION}"
echo "  build    : ${BUILD_DIR}"
echo "  install  : ${INSTALL_DIR}"
echo ""

if [ -f "${INSTALL_DIR}/lib/libibverbs.so" ]; then
  echo "rdma-core already installed, skipping."
  exit 0
fi

mkdir -p "${BUILD_DIR}"

# --- Download tarball ---
if [ ! -f "${TARBALL}" ]; then
  echo "Downloading rdma-core v${VERSION}..."
  curl -fsSL -o "${TARBALL}" "${TARBALL_URL}"
fi

# --- Extract ---
if [ ! -d "${SRC_DIR}" ]; then
  echo "Extracting..."
  tar xf "${TARBALL}" -C "${BUILD_DIR}"
fi

# --- Inject rocm_ernic provider ---
echo ""
echo "--- Injecting rocm_ernic provider ---"
if [ -x "${APPLY_SCRIPT}" ]; then
  "${APPLY_SCRIPT}" "${SRC_DIR}"
else
  echo "ERROR: apply script not found:" \
    "${APPLY_SCRIPT}"
  exit 1
fi

# Record git SHA for provenance
if [ -d "${PROJECT_DIR}/.git" ]; then
  ERNIC_SHA="$(git -C "${PROJECT_DIR}" \
    rev-parse --short HEAD 2>/dev/null \
    || echo "unknown")"
  echo "${ERNIC_SHA}" > \
    "${SRC_DIR}/providers/rocm_ernic/SOURCE_SHA"
  echo "  rocm-ernic SHA: ${ERNIC_SHA}"
fi

# --- Build with cmake ---
echo ""
echo "Configuring rdma-core..."
mkdir -p "${CMAKE_BUILD}"
cmake -S "${SRC_DIR}" -B "${CMAKE_BUILD}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DIOCTL_MODE=write \
  -DNO_PYVERBS=1 \
  -DNO_MAN_PAGES=1 \
  -DENABLE_STATIC=0

echo "Building rdma-core (${NPROC} jobs)..."
cmake --build "${CMAKE_BUILD}" -j "${NPROC}"

if $BUILD_ONLY; then
  echo ""
  echo "=== rdma-core built (no install) ==="
  echo "  build dir : ${CMAKE_BUILD}"
  exit 0
fi

echo "Installing rdma-core to ${INSTALL_DIR}..."
cmake --install "${CMAKE_BUILD}"

echo ""
echo "=== rdma-core installed ==="
echo "  libibverbs     : ${INSTALL_DIR}/lib/"
echo "  librocm_ernic  : ${INSTALL_DIR}/lib/"
echo "  headers        : ${INSTALL_DIR}/include/"
