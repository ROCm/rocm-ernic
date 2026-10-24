#!/bin/bash -eu
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT
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
#
# The "-eu" on the shebang is redundant with the "set" below and is there
# for ansible-test's shebang check, which runs over everything in the
# collection and accepts "#!/bin/bash -eu" but not a bare "#!/bin/bash".
# This script is installed onto hosts and guests and may run as root, so
# it keeps an absolute /bin/bash rather than /usr/bin/env.

set -euo pipefail

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
# The driver does #include <rdma/ionic-abi.h>, which kbuild resolves out of the
# RUNNING kernel's headers. A series that changes that UAPI header therefore
# builds against the old one and fails on the fields it just added -- or, worse,
# succeeds while silently disagreeing with userspace about the struct. Stage the
# tree's own copy when there is one and put it ahead of the kernel's.
UAPI_SRC="${SOURCE_DIR}/include/uapi/rdma"

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

if [[ -f "${UAPI_SRC}/ionic-abi.h" ]]; then
    echo "Staging patched UAPI header from ${UAPI_SRC}"
    sudo mkdir -p "${DKMS_TREE_DIR}/uapi/rdma"
    sudo cp "${UAPI_SRC}/ionic-abi.h" "${DKMS_TREE_DIR}/uapi/rdma/"
fi

# Write Kbuild file that builds both modules from one DKMS package.
sudo tee "${DKMS_TREE_DIR}/Kbuild" > /dev/null <<'EOF'
# Kbuild for ionic-ernic DKMS package
# Builds ionic.ko (Ethernet) and ionic_rdma.ko (RDMA).

# Prepended to LINUXINCLUDE, not added via ccflags-y: kbuild puts
# LINUXINCLUDE ahead of ccflags-y on the command line, so a -I there would
# lose to the kernel's own <rdma/ionic-abi.h> every time.
ifneq ($(wildcard $(src)/uapi/rdma/ionic-abi.h),)
LINUXINCLUDE := -I$(src)/uapi $(LINUXINCLUDE)
endif

ccflags-y += -I$(src)/eth

obj-m += ionic.o
ionic-y := $(patsubst $(src)/eth/%.c,eth/%.o,\
             $(wildcard $(src)/eth/*.c))

ccflags-y += -I$(src)/rdma -I$(src)/eth
obj-m += ionic_rdma.o
ionic_rdma-y := $(patsubst $(src)/rdma/%.c,rdma/%.o,\
                  $(wildcard $(src)/rdma/*.c))
EOF

# The SPDX tag below is the generated dkms.conf's, which is GPL-2.0 like the
# drivers it builds. This script itself is MIT.
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

# No autoinstall.  These sources are one kernel's ionic, and they only
# build against a kernel close enough to the ref they came from: the
# v7.2.4 driver includes <rdma/iter.h>, which no 7.0 kernel has, so a
# build for one fails with "fatal error: rdma/iter.h: No such file or
# directory".  Autoinstall does not know that and builds for every kernel
# with headers installed, which on a CI guest carrying both its running
# kernel and the distribution's own is a build that cannot succeed.
#
# It also cannot fail quietly.  DKMS gives a module ONE build directory,
# the "build" directory named in MAKE below, shared by every kernel it
# builds for, so an autoinstall running beside the explicit build has them
# both in the same tree -- and the failing one leaves its make.log there
# under the other's name.  That is what made this look like the wrong
# kernel being built rather than a second one being built at all.
#
# So this script builds for exactly the kernel it was asked for, and a
# kernel upgrade needs it run again.
AUTOINSTALL="no"

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
    DKMS_DIR="/var/lib/dkms/${PKG_NAME}/${PKG_VERSION}"
    # DKMS moves the log to a per-kernel directory once a build SUCCEEDS;
    # a failed one leaves it in the shared build directory, which is the
    # case we are in here.  Prefer the per-kernel copy anyway: if both
    # exist, the shared one is the more likely to have been overwritten.
    MAKE_LOG="${DKMS_DIR}/${KVER}/$(uname -m)/log/make.log"
    sudo test -r "${MAKE_LOG}" || MAKE_LOG="${DKMS_DIR}/build/make.log"
    echo ""
    echo "ERROR: DKMS build failed for ${KVER}."
    echo "Check: ${MAKE_LOG}"
    # In CI the guest is torn down straight after this, so the log has to be
    # emitted here or the actual compile error is lost.
    if sudo test -r "${MAKE_LOG}"; then
        # That directory is shared by every kernel this module builds for,
        # so say which kernel the log is actually for.  A log naming a
        # kernel other than the one that just failed is a different build's,
        # and the error in it is not the one being chased.
        LOG_KVER="$(sudo sed -n \
            's/^DKMS .*make\.log for .* for kernel \([^ ]*\) .*/\1/p' \
            "${MAKE_LOG}" | head -1)"
        if [ -n "${LOG_KVER}" ] && [ "${LOG_KVER}" != "${KVER}" ]; then
            echo ""
            echo "WARNING: this log is for kernel ${LOG_KVER}, not ${KVER}."
            echo "  Another build shared the directory and overwrote it;"
            echo "  the error below is that build's, not this one's."
        fi
        echo ""
        echo "=== ${MAKE_LOG} (last 200 lines) ==="
        sudo tail -200 "${MAKE_LOG}"
        echo "=== end of make.log ==="
    fi
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
echo "Built for ${KVER} only. These sources track the kernel they came"
echo "from, so run this again after a kernel upgrade; DKMS will not"
echo "rebuild them by itself."
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
