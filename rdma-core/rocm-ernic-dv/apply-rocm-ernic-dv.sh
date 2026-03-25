#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Copy the rocm_ernic rdma-core provider into an
# existing rdma-core source tree and register it
# with the build system.
#
# Usage:
#   apply-rocm-ernic-dv.sh <rdma-core-dir>
#
# The provider is copied (not patched) because the
# rocm_ernic provider is entirely new -- there are
# no upstream files to modify.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROVIDER_SRC="${SCRIPT_DIR}/../providers/rocm_ernic"
RDMA_CORE="${1:-}"

if [ -z "${RDMA_CORE}" ]; then
    echo "Usage: $0 <rdma-core-source-dir>"
    exit 1
fi

if [ ! -f "${RDMA_CORE}/CMakeLists.txt" ]; then
    echo "ERROR: ${RDMA_CORE} does not look like" \
         "an rdma-core tree"
    exit 1
fi

DEST="${RDMA_CORE}/providers/rocm_ernic"

echo "=== apply-rocm-ernic-dv ==="
echo "  source:  ${PROVIDER_SRC}"
echo "  dest:    ${DEST}"
echo ""

# Copy provider files
mkdir -p "${DEST}"
cp -v "${PROVIDER_SRC}/CMakeLists.txt" "${DEST}/"
cp -v "${PROVIDER_SRC}/main.c"         "${DEST}/"
cp -v "${PROVIDER_SRC}/verbs.c"        "${DEST}/"
cp -v "${PROVIDER_SRC}/dv.c"           "${DEST}/"
cp -v "${PROVIDER_SRC}/rocm_ernic.h"   "${DEST}/"
cp -v "${PROVIDER_SRC}/rocm_ernic_dv.h" \
    "${DEST}/"
cp -v "${PROVIDER_SRC}/rocm_ernic_driver_id.h" \
    "${DEST}/"
cp -v "${PROVIDER_SRC}/rocm_ernic.map" "${DEST}/"

# Register the provider with the top-level
# CMakeLists.txt if not already present
if ! grep -q "rocm_ernic" \
    "${RDMA_CORE}/CMakeLists.txt"; then
    echo ""
    echo "Registering provider in CMakeLists.txt..."

    # Append after the last add_subdirectory(providers/...)
    last_line="$(grep -n "add_subdirectory(providers/" \
        "${RDMA_CORE}/CMakeLists.txt" \
        | tail -1 | cut -d: -f1)"
    if [ -n "${last_line}" ]; then
        sed -i "${last_line}a\\add_subdirectory(providers/rocm_ernic)" \
            "${RDMA_CORE}/CMakeLists.txt"
    else
        echo "add_subdirectory(providers/rocm_ernic)" \
            >> "${RDMA_CORE}/CMakeLists.txt"
    fi
fi

echo ""
echo "=== rocm_ernic provider installed ==="
echo "Build rdma-core normally:"
echo "  cd ${RDMA_CORE}"
echo "  mkdir -p build && cd build"
echo "  cmake .."
echo "  make -j\$(nproc)"
echo ""
echo "The provider builds librocm_ernic.so which"
echo "exports the DV symbols loaded by rocm-xio."
