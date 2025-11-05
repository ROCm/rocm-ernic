#!/bin/bash
#
# Helper script to commit wrapper API implementation with GPG signature
#

set -e

echo "Committing wrapper API implementation (90% complete)..."
echo ""
echo "Files to be committed:"
git status -s
echo ""
echo "This commit includes:"
echo "  - Clean wrapper API with opaque handles"
echo "  - Complete wrapper implementation (~350 lines)"
echo "  - Server refactored to use wrapper API"
echo "  - QEMU source files cleaned (removed qemu/osdep.h)"
echo "  - 90% toward working build"
echo ""
read -p "Proceed with GPG-signed commit? (y/N) " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    git commit -S -F COMMIT_MSG_WRAPPER.txt
    echo ""
    echo "✅ Commit successful!"
    echo ""
    echo "To push:"
    echo "  git push origin stephen"
    echo ""
    echo "Next steps (2-4 hours):"
    echo "  1. Remove unused includes (cpu.h) from QEMU files"
    echo "  2. Create minimal CPU stub header"
    echo "  3. Complete PCI stub header"
    echo "  See WRAPPER_API_STATUS.md for details"
else
    echo "Commit cancelled"
    exit 1
fi

