#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Run tests locally against built binaries
#
# This script runs the test suite against locally built binaries.
# Use this for quick testing during development.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Check if build directory exists
if [ ! -d "build" ]; then
    echo "Error: build directory not found"
    echo "Run 'cmake -B build -G Ninja && cmake --build build' first"
    exit 1
fi

# Check if binaries exist
if [ ! -x "build/rocm-ernic" ]; then
    echo "Error: rocm-ernic not found in build directory"
    echo "Run 'ninja -C build' to build the project"
    exit 1
fi

if [ ! -x "build/tests/test_pci_client" ]; then
    echo "Error: test_pci_client not found"
    echo "Run 'ninja -C build' to build the tests"
    exit 1
fi

echo "================================================="
echo "  Running rocm-ernic Local Tests"
echo "================================================="
echo ""

# Run the test harness
exec "$PROJECT_ROOT/tests/run-test.sh" \
    "$PROJECT_ROOT/build/tests/test_pci_client" \
    "$PROJECT_ROOT/build/rocm-ernic"

