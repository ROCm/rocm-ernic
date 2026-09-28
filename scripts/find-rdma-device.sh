#!/bin/sh
# Print the name of the emulated ERNIC RDMA device in this guest, or
# print nothing and exit 1 if there is none.
#
# Matches on the emulated PCI ids rather than on device name.  The name
# is whatever the guest's udev policy last set it to, and on our guests
# it is rewritten twice: the kernel registers ionic_%d,
# 60-rdma-persistent-naming.rules rewrites that to rocep<bus>s<slot> via
# rdma_rename, and udev/99-rocm-ernic.rules rewrites it again to
# rocm-rdma-ernic<n>.  Any list of name patterns is a snapshot of one
# moment in that sequence and goes stale silently.
#
# Intended to be run in the guest:
#   ssh guest 'sh -s' < scripts/find-rdma-device.sh
# or via ansible's script module.  Deliberately POSIX sh and free of
# external commands so it works on a minimal guest image.

ERNIC_PCI_VENDOR_ID=0x1dd8
ERNIC_PCI_DEVICE_ID=0x1002
ERNIC_PCI_SUBSYSTEM_VENDOR_ID=0x1dd8
ERNIC_PCI_SUBSYSTEM_DEVICE_ID=0x5400

for vendor_attr in /sys/class/infiniband/*/device/vendor; do
    [ -r "$vendor_attr" ] || continue
    device_dir=${vendor_attr%/vendor}
    read -r vendor_id < "$vendor_attr" || continue
    [ "$vendor_id" = "$ERNIC_PCI_VENDOR_ID" ] || continue
    read -r device_id < "$device_dir/device" || continue
    [ "$device_id" = "$ERNIC_PCI_DEVICE_ID" ] || continue
    read -r subsystem_vendor_id < "$device_dir/subsystem_vendor" || continue
    [ "$subsystem_vendor_id" = "$ERNIC_PCI_SUBSYSTEM_VENDOR_ID" ] || continue
    read -r subsystem_device_id < "$device_dir/subsystem_device" || continue
    [ "$subsystem_device_id" = "$ERNIC_PCI_SUBSYSTEM_DEVICE_ID" ] || continue

    ibdev_dir=${vendor_attr%/device/vendor}
    echo "${ibdev_dir##*/}"
    exit 0
done

exit 1
