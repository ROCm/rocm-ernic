#!/bin/bash
# Helper script to commit with GPG signing

cd /home/stebates/Projects/vfu-rdma
git commit -S -F COMMIT_MSG_STUBS.txt

echo ""
echo "Commit completed!"
echo ""
echo "To continue fixing the remaining 2 build errors, just say 'continue'."

