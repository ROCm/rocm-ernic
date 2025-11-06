#!/bin/bash
#
# run-vm-vfio-user.sh
#
# Modified version of qemu-minimal's run-vm to support vfio-user devices
# 
# This script launches a VM that connects to a vfio-user device server
# (like vfu_pvrdma) via a Unix socket.
#
# Usage:
#   VFIO_USER_SOCKET=/var/run/vfu-pvrdma.sock ./run-vm-vfio-user.sh
#

set -e

QEMU_PATH=${QEMU_PATH:-/opt/qemu-v10.1.2/bin/}
VM_NAME=${VM_NAME:-stebates-test-vm}
ARCH=${ARCH:-amd64}
VCPUS=${VCPUS:-4}
VMEM=${VMEM:-8192}
FILESYSTEM=${FILESYSTEM:-none}
IMAGES=${IMAGES:-/home/stebates/Projects/qemu-minimal/images}
SSH_PORT=${SSH_PORT:-2222}
KVM=${KVM:-enable}
VFIO_USER_SOCKET=${VFIO_USER_SOCKET:-/tmp/vfio-user-pvrdma.sock}

# Check if vfio-user socket exists
if [ ! -S "${VFIO_USER_SOCKET}" ]; then
    echo "ERROR: vfio-user socket not found: ${VFIO_USER_SOCKET}"
    echo "Please start vfu_pvrdma server first:"
    echo "  sudo ./build/vfu_pvrdma --socket ${VFIO_USER_SOCKET}"
    exit 1
fi

if [ ${KVM} == "enable" ]; then
    KVM_ARGS=",accel=kvm"
else
    KVM_ARGS=""
fi

if [ ${FILESYSTEM} == "none" ]; then
    FILESYSTEM_ARGS=""
else
    FILESYSTEM_ARGS="-virtfs local,path=${FILESYSTEM},security_model=passthrough,mount_tag=hostfs"
fi

if [ ${ARCH} == "amd64" ]; then
    QARCH="x86_64"
    QARCH_ARGS="-machine q35${KVM_ARGS} -cpu EPYC"
else
    echo "Error: Only amd64/x86_64 supported for vfio-user testing"
    exit 1
fi

# Check if VM image exists
if [ ! -f "${IMAGES}/${VM_NAME}.qcow2" ]; then
    echo "ERROR: VM image not found: ${IMAGES}/${VM_NAME}.qcow2"
    echo "Available images:"
    ls -lh ${IMAGES}/*.qcow2
    echo ""
    echo "To create a new VM, use qemu-minimal's gen-vm script:"
    echo "  cd /home/stebates/Projects/qemu-minimal/qemu"
    echo "  VM_NAME=${VM_NAME} ./gen-vm"
    exit 1
fi

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Starting VM with vfio-user PVRDMA Device                   ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Configuration:"
echo "  VM Name:        ${VM_NAME}"
echo "  VM Image:       ${IMAGES}/${VM_NAME}.qcow2"
echo "  VCPUs:          ${VCPUS}"
echo "  Memory:         ${VMEM} MB"
echo "  SSH Port:       ${SSH_PORT}"
echo "  vfio-user:      ${VFIO_USER_SOCKET}"
echo ""
echo "To connect:"
echo "  ssh -p ${SSH_PORT} ubuntu@localhost"
echo ""
echo "Press Ctrl-A then X to exit QEMU"
echo ""

# NOTE: vfio-user-pci device support requires QEMU with vfio-user client
# support. This should be available in QEMU 7.0+
#
# The device specification tries different formats depending on QEMU version:
# - Modern: -device vfio-user-pci,socket=${VFIO_USER_SOCKET}
# - Some versions: -chardev socket,id=vfio0,path=${VFIO_USER_SOCKET}
#                  -device vfio-user-pci,chardev=vfio0

exec ${QEMU_PATH}qemu-system-${QARCH} \
   ${QARCH_ARGS} \
   -smp cpus=${VCPUS} \
   -object memory-backend-memfd,id=mem0,share=on,size=${VMEM}M \
   -machine memory-backend=mem0 \
   ${FILESYSTEM_ARGS} \
   -nographic \
   -drive if=virtio,format=qcow2,file=${IMAGES}/${VM_NAME}.qcow2 \
   -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
   -device virtio-net-pci,netdev=net0 \
   -device '{"driver":"vfio-user-pci","socket":{"path":"'"${VFIO_USER_SOCKET}"'","type":"unix"}}'

