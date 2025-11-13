#!/bin/bash
#
# Setup GID for standalone RDMA device (no netdev)
# 
# For RoCE devices without a network interface, the kernel RDMA subsystem
# doesn't automatically populate GIDs. This script manually adds a default
# GID using the rdma netlink tool.
#

set -e

DEVICE="amd_emrdma0"
PORT="1"

# Check if device exists
if [ ! -d "/sys/class/infiniband/$DEVICE" ]; then
    echo "Error: RDMA device $DEVICE not found"
    echo "Available devices:"
    ls /sys/class/infiniband/ 2>/dev/null || echo "  (none)"
    exit 1
fi

# Get the node GUID
NODE_GUID=$(cat /sys/class/infiniband/$DEVICE/node_guid)
echo "Device: $DEVICE"
echo "Node GUID: $NODE_GUID"

# Create default GID from node_guid
# Format: fe80:0000:0000:0000:GGGG:GGGG:GGGG:GGGG
# where G = node_guid bytes

# Extract GUID bytes and construct GID
GUID_HEX=$(echo $NODE_GUID | tr -d ':')
GID="fe80:0000:0000:0000:${GUID_HEX:0:4}:${GUID_HEX:4:4}:${GUID_HEX:8:4}:${GUID_HEX:12:4}"

echo "Adding GID: $GID"

# Add the GID using rdma tool
if command -v rdma &> /dev/null; then
    sudo rdma link add $DEVICE/$PORT type ipv6 address $GID
    echo "✓ GID added successfully"
    echo ""
    echo "Verify with:"
    echo "  cat /sys/class/infiniband/$DEVICE/ports/$PORT/gids/0"
else
    echo "Error: 'rdma' tool not found"
    echo "Install with: sudo apt-get install iproute2"
    exit 1
fi

