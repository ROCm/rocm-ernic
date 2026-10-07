#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
LOOKUP_SCRIPT="$SCRIPT_DIR/../scripts/find-rdma-device.sh"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/test-find-rdma-device-XXXXXX")
ERNIC_SYSFS_ROOT="$WORK_DIR/sys"
export ERNIC_SYSFS_ROOT
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM

clear_devices() {
    rm -rf "$ERNIC_SYSFS_ROOT/class/infiniband"
    mkdir -p "$ERNIC_SYSFS_ROOT/class/infiniband"
}

add_device() {
    name=$1
    vendor_id=$2
    device_id=$3
    subsystem_vendor_id=$4
    subsystem_device_id=$5
    device_dir="$ERNIC_SYSFS_ROOT/class/infiniband/$name/device"
    mkdir -p "$device_dir"
    printf '%s\n' "$vendor_id" > "$device_dir/vendor"
    printf '%s\n' "$device_id" > "$device_dir/device"
    printf '%s\n' "$subsystem_vendor_id" > "$device_dir/subsystem_vendor"
    printf '%s\n' "$subsystem_device_id" > "$device_dir/subsystem_device"
}

expect_match() {
    expected=$1
    if output=$(sh "$LOOKUP_SCRIPT"); then
        [ "$output" = "$expected" ] || {
            echo "Expected RDMA device '$expected', got '$output'" >&2
            exit 1
        }
    else
        echo "Expected RDMA device '$expected', but lookup failed" >&2
        exit 1
    fi
}

expect_no_match() {
    if output=$(sh "$LOOKUP_SCRIPT"); then
        echo "Expected no RDMA device, got '$output'" >&2
        exit 1
    fi
    [ -z "$output" ] || {
        echo "Expected no output for a non-matching device, got '$output'" >&2
        exit 1
    }
}

clear_devices
add_device rdma0 0x1dd8 0x1002 0x1dd8 0x1234
add_device rdma1 0x1dd8 0x1002 0x1dd8 0x5400
expect_match rdma1

clear_devices
add_device rdma0 0x1dd8 0x1002 0x1234 0x5400
expect_no_match

clear_devices
add_device rdma0 0x1dd8 0x1002 0x1dd8 0x1234
expect_no_match

clear_devices
add_device rdma0 0x1dd8 0x1003 0x1dd8 0x5400
expect_no_match

clear_devices
add_device rdma0 0x1dd9 0x1002 0x1dd8 0x5400
expect_no_match

echo "RDMA device lookup matches only the emulated PCI tuple"
