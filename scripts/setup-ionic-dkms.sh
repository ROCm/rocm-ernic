#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup-ionic-dkms.sh
#
# Stages the patched upstream ionic + ionic_rdma driver sources into a
# DKMS tree and registers/builds/installs them.
#
# Called by the CMake ionic DKMS targets; see cmake/ErnicKernelModule.cmake.
#
# Usage:
#   setup-ionic-dkms.sh --source-dir DIR --kernel-ref REF [OPTIONS]
#
# Options:
#   --source-dir DIR   Directory containing patched ionic kernel sources
#                      (the checkout produced by fetch-ionic-sources)
#   --kernel-ref REF   Kernel tag/SHA (used in DKMS package version)
#   --build-only       Build but do not install
#   --uninstall        Remove the DKMS modules

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PKG_NAME="ionic-ernic"
SOURCE_DIR=""
KERNEL_REF=""
BUILD_ONLY=false
UNINSTALL=false

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir)  SOURCE_DIR="$2";  shift 2 ;;
        --kernel-ref)  KERNEL_REF="$2";  shift 2 ;;
        --build-only)  BUILD_ONLY=true;  shift ;;
        --uninstall)   UNINSTALL=true;   shift ;;
        -h|--help)
            echo "Usage: $0 --source-dir DIR --kernel-ref REF" \
                 "[--build-only] [--uninstall]"
            exit 0 ;;
        *)
            echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "${SOURCE_DIR}" || -z "${KERNEL_REF}" ]]; then
    echo "ERROR: --source-dir and --kernel-ref are required"
    exit 1
fi

# Sanitize kernel ref for use in package version (strip leading 'v', replace
# non-alphanumeric with '-').
PKG_VERSION="${KERNEL_REF#v}"
PKG_VERSION="${PKG_VERSION//[^a-zA-Z0-9.]/-}"

DKMS_TREE_DIR="/usr/src/${PKG_NAME}-${PKG_VERSION}"
ETH_SRC="${SOURCE_DIR}/drivers/net/ethernet/pensando/ionic"
RDMA_SRC="${SOURCE_DIR}/drivers/infiniband/hw/ionic"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

remove_dkms_registration_if_present() {
    if sudo dkms status "${PKG_NAME}/${PKG_VERSION}" 2>/dev/null \
            | grep -q "${PKG_NAME}"; then
        sudo dkms remove "${PKG_NAME}/${PKG_VERSION}" --all || true
    fi
}

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------

if $UNINSTALL; then
    echo "Removing ${PKG_NAME}/${PKG_VERSION} from DKMS..."
    remove_dkms_registration_if_present
    sudo rm -rf "${DKMS_TREE_DIR}"
    echo "Done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Stage sources into DKMS tree
# ---------------------------------------------------------------------------

echo "Staging ionic sources into ${DKMS_TREE_DIR}..."
sudo rm -rf "${DKMS_TREE_DIR}"
sudo mkdir -p "${DKMS_TREE_DIR}/eth" "${DKMS_TREE_DIR}/rdma"

sudo cp -r "${ETH_SRC}/."  "${DKMS_TREE_DIR}/eth/"
sudo cp -r "${RDMA_SRC}/." "${DKMS_TREE_DIR}/rdma/"

# Write Kbuild file that builds both modules from one DKMS package.
sudo tee "${DKMS_TREE_DIR}/Kbuild" > /dev/null <<'EOF'
# Kbuild for ionic-ernic DKMS package
# Builds ionic.ko (Ethernet) and ionic_rdma.ko (RDMA).

ccflags-y += -I$(src)/eth

obj-m += ionic.o
ionic-y := $(patsubst $(src)/eth/%.c,eth/%.o,\
             $(wildcard $(src)/eth/*.c))

ccflags-y += -I$(src)/rdma -I$(src)/eth
obj-m += ionic_rdma.o
ionic_rdma-y := $(patsubst $(src)/rdma/%.c,rdma/%.o,\
                  $(wildcard $(src)/rdma/*.c))
EOF

sudo tee "${DKMS_TREE_DIR}/dkms.conf" > /dev/null <<EOF
# DKMS configuration for ionic-ernic
# Upstream ionic driver + rocm-ernic AMD device ID patch.
#
# SPDX-License-Identifier: GPL-2.0

PACKAGE_NAME="${PKG_NAME}"
PACKAGE_VERSION="${PKG_VERSION}"

BUILT_MODULE_NAME[0]="ionic"
DEST_MODULE_LOCATION[0]="/updates/dkms"

BUILT_MODULE_NAME[1]="ionic_rdma"
DEST_MODULE_LOCATION[1]="/updates/dkms"

AUTOINSTALL="yes"

MAKE="make -C \${kernel_source_dir} M=\${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build modules"
CLEAN="make -C \${kernel_source_dir} M=\${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build clean"
EOF

# ---------------------------------------------------------------------------
# Register and build with DKMS
# ---------------------------------------------------------------------------

remove_dkms_registration_if_present

echo "Registering ${PKG_NAME}/${PKG_VERSION} with DKMS..."
sudo dkms add "${PKG_NAME}/${PKG_VERSION}"

KVER="$(uname -r)"
echo "Building ionic + ionic_rdma for ${KVER}..."
if ! sudo dkms build "${PKG_NAME}/${PKG_VERSION}" -k "${KVER}"; then
    echo ""
    echo "ERROR: DKMS build failed."
    echo "Check: /var/lib/dkms/${PKG_NAME}/${PKG_VERSION}/build/make.log"
    exit 1
fi

echo ""
echo "=== ionic + ionic_rdma built via DKMS ==="
echo "  Package : ${PKG_NAME}/${PKG_VERSION}"
echo "  Kernel  : ${KVER}"
echo ""

if $BUILD_ONLY; then
    echo "Built (not installed). Re-run without --build-only to install."
    exit 0
fi

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

echo "Installing ionic + ionic_rdma..."
sudo dkms install "${PKG_NAME}/${PKG_VERSION}" -k "${KVER}"

echo ""
echo "=== ionic + ionic_rdma installed via DKMS ==="
echo ""
echo "DKMS will rebuild on kernel upgrades."
echo ""
echo "To load now:"
echo "  sudo modprobe ionic"
echo "  sudo modprobe ionic_rdma"
echo ""
echo "To unload (reverse order):"
echo "  sudo modprobe -r ionic_rdma"
echo "  sudo modprobe -r ionic"
echo ""
echo "To revert:"
echo "  $0 --source-dir '${SOURCE_DIR}' --kernel-ref '${KERNEL_REF}' --uninstall"
