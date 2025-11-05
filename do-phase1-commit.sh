#!/bin/bash
#
# Helper script to commit Phase 1 integration work with GPG signature
#

set -e

echo "Committing Phase 1 QEMU PVRDMA integration architecture..."
echo ""
echo "Files to be committed:"
git status -s
echo ""
echo "Commit message:"
echo "==============================================================================="
cat COMMIT_MSG_PHASE1.txt
echo "==============================================================================="
echo ""
read -p "Proceed with GPG-signed commit? (y/N) " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    git commit -S -F COMMIT_MSG_PHASE1.txt
    echo ""
    echo "✅ Commit successful!"
    echo ""
    echo "To push:"
    echo "  git push origin stephen"
else
    echo "Commit cancelled"
    exit 1
fi

