#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Setup DKMS tree for rocm_ernic_eth + rocm_ernic_rdma.
#
# 1. Copies driver source from driver/
# 2. Copies ABI and DV UAPI headers to include/rdma/
# 3. Populates /usr/src/rocm-ernic-eth-rdma-<ver>/
# 4. Registers and builds with DKMS.
#
# Usage:
#   setup-rocm-ernic-dkms.sh [OPTIONS]
#
# Options:
#   --version VER   DKMS package version
#                   (default: 0.2.0-g<shortrev>)
#   --build-only    Build but do not install
#   --uninstall     Remove the DKMS module

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DRIVER_DIR="${SCRIPT_DIR}"

PKG_NAME="rocm-ernic-eth-rdma"
BASE_VERSION="0.2.0"
PKG_VERSION=""
BUILD_ONLY=false
UNINSTALL=false

# ---------------------------------------------------
# Argument parsing
# ---------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      PKG_VERSION="$2"; shift 2 ;;
    --build-only)
      BUILD_ONLY=true; shift ;;
    --uninstall)
      UNINSTALL=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--version VER]" \
        "[--build-only] [--uninstall]"
      exit 0 ;;
    *)
      echo "Unknown option: $1"; exit 1 ;;
  esac
done

GIT_REV="$(git -C "${PROJECT_ROOT}" \
  rev-parse --short HEAD 2>/dev/null \
  || echo "unknown")"

if [ -z "${PKG_VERSION}" ]; then
  PKG_VERSION="${BASE_VERSION}-g${GIT_REV}"
fi

DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

# ---------------------------------------------------
# Uninstall
# ---------------------------------------------------

if $UNINSTALL; then
  echo "Removing DKMS module" \
    "${PKG_NAME}/${PKG_VERSION}..."
  sudo dkms remove \
    "${PKG_NAME}/${PKG_VERSION}" \
    --all 2>/dev/null || true
  sudo rm -rf "${DKMS_SRC}"
  echo "Done."
  exit 0
fi

# ---------------------------------------------------
# Prerequisites
# ---------------------------------------------------

check_prereqs() {
  local missing=()
  command -v dkms &>/dev/null \
    || missing+=("dkms")
  command -v make &>/dev/null \
    || missing+=("make")

  if [ ${#missing[@]} -gt 0 ]; then
    echo "ERROR: Missing required tools:" \
      "${missing[*]}"
    echo "Install with:"
    echo "  sudo apt install ${missing[*]}"
    exit 1
  fi

  if [ ! -f "${DRIVER_DIR}/rocm_ernic_main.c" ]; then
    echo "ERROR: rocm_ernic_main.c not found" \
      "in ${DRIVER_DIR}"
    exit 1
  fi

  local kver
  kver="$(uname -r)"
  if [ ! -f \
    "/lib/modules/${kver}/build/Makefile" ]; then
    echo "ERROR: Kernel headers not found" \
      "for ${kver}."
    echo "Install with:"
    echo "  sudo apt install" \
      "linux-headers-${kver}"
    exit 1
  fi
}

check_prereqs

# ---------------------------------------------------
# Print status
# ---------------------------------------------------

echo ""
echo "=== setup-rocm-ernic-dkms ==="
echo "  project   : ${PROJECT_ROOT}"
echo "  driver    : ${DRIVER_DIR}"
echo "  DKMS src  : ${DKMS_SRC}"
echo "  package   :" \
  "${PKG_NAME}/${PKG_VERSION}"
echo ""

# ---------------------------------------------------
# Skip if already installed
# ---------------------------------------------------

if ! $BUILD_ONLY && \
    dkms status \
    "${PKG_NAME}/${PKG_VERSION}" \
    2>/dev/null | grep -q "installed"; then
  echo "${PKG_NAME}/${PKG_VERSION}" \
    "already installed."
  echo "Use --uninstall to remove first."
  exit 0
fi

# ---------------------------------------------------
# Populate DKMS source tree
# ---------------------------------------------------

populate_dkms() {
  echo "Installing source to ${DKMS_SRC}..."
  sudo rm -rf "${DKMS_SRC}"
  sudo mkdir -p "${DKMS_SRC}/include/rdma"

  sudo cp -a "${DRIVER_DIR}/"*.c \
    "${DKMS_SRC}/" 2>/dev/null || true
  sudo cp -a "${DRIVER_DIR}/"*.h \
    "${DKMS_SRC}/" 2>/dev/null || true

  if [ -f \
    "${DRIVER_DIR}/rocm_ernic-abi.h" ]; then
    sudo cp \
      "${DRIVER_DIR}/rocm_ernic-abi.h" \
      "${DKMS_SRC}/include/rdma/"
  fi

  if [ -f \
    "${DRIVER_DIR}/rocm_ernic_dv_uapi.h" ]; then
    sudo cp \
      "${DRIVER_DIR}/rocm_ernic_dv_uapi.h" \
      "${DKMS_SRC}/include/rdma/"
  fi

  echo "${GIT_REV}" | sudo tee \
    "${DKMS_SRC}/SOURCE_SHA" >/dev/null

  sudo cp "${DRIVER_DIR}/Makefile" \
    "${DKMS_SRC}/"
  sudo cp "${DRIVER_DIR}/dkms.conf" \
    "${DKMS_SRC}/"
  sudo sed -i \
    "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${PKG_VERSION}\"/" \
    "${DKMS_SRC}/dkms.conf"

  echo "Source tree populated."
}

populate_dkms

# ---------------------------------------------------
# Register and build with DKMS
# ---------------------------------------------------

build_dkms() {
  echo ""

  if dkms status \
      "${PKG_NAME}/${PKG_VERSION}" \
      2>/dev/null | grep -q .; then
    echo "Removing stale DKMS registration..."
    sudo dkms remove \
      "${PKG_NAME}/${PKG_VERSION}" \
      --all 2>/dev/null || true
  fi

  echo "Registering with DKMS..."
  sudo dkms add "${PKG_NAME}/${PKG_VERSION}"

  local kver
  kver="$(uname -r)"

  echo "Building rocm_ernic_eth +" \
    "rocm_ernic_rdma for ${kver}..."
  if ! sudo dkms build \
      "${PKG_NAME}/${PKG_VERSION}" \
      -k "${kver}"; then
    echo ""
    echo "ERROR: DKMS build failed."
    echo "Check: /var/lib/dkms/${PKG_NAME}/" \
      "${PKG_VERSION}/build/make.log"
    exit 1
  fi

  echo ""
  echo "=== rocm_ernic eth+RDMA built" \
    "via DKMS ==="
  echo "  Module: /var/lib/dkms/${PKG_NAME}/" \
    "${PKG_VERSION}/${kver}/$(uname -m)/module/"
  echo ""
  echo "To install:"
  echo "  $0  (re-run without --build-only)"
}

install_dkms() {
  local kver
  kver="$(uname -r)"

  echo "Installing rocm_ernic_eth +" \
    "rocm_ernic_rdma..."
  sudo dkms install \
    "${PKG_NAME}/${PKG_VERSION}" -k "${kver}"

  echo ""
  echo "=== rocm_ernic eth+RDMA installed" \
    "via DKMS ==="
  echo ""
  echo "DKMS will rebuild on kernel upgrades."
  echo ""
  echo "To load now:"
  echo "  sudo modprobe rocm_ernic_eth"
  echo "  sudo modprobe rocm_ernic_rdma"
  echo ""
  echo "To unload (reverse order):"
  echo "  sudo modprobe -r rocm_ernic_rdma"
  echo "  sudo modprobe -r rocm_ernic_eth"
  echo ""
  echo "To revert:"
  echo "  $0 --uninstall"
  echo "  sudo depmod -a"
}

build_dkms
if ! $BUILD_ONLY; then
  install_dkms
fi
