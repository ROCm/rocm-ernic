#!/bin/bash
#
# run-vm-vfio-user.sh
#
# Modified version of qemu-minimal's run-vm to support vfio-user devices
# 
# This script launches a VM that connects to a vfio-user device server
# (like rocm-ernic) via a Unix socket.
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
VFIO_USER_SOCKET=${VFIO_USER_SOCKET:-/tmp/vfio-user-rocm-ernic.sock}
QMP_SOCKET=${QMP_SOCKET:-/tmp/qemu-qmp.sock}

# Remove stale QMP socket from a previous QEMU crash
if [ -e "${QMP_SOCKET}" ]; then
    if [ -S "${QMP_SOCKET}" ]; then
        rm -f "${QMP_SOCKET}" || {
            echo "ERROR: Failed to remove stale QMP socket: ${QMP_SOCKET}"
            exit 1
        }
    else
        echo "ERROR: QMP socket path exists and is not a socket: ${QMP_SOCKET}"
        exit 1
    fi
fi

# Check if vfio-user socket exists
if [ ! -S "${VFIO_USER_SOCKET}" ]; then
    echo "ERROR: vfio-user socket not found: ${VFIO_USER_SOCKET}"
    echo "Please start rocm-ernic server first:"
    echo "  sudo ./build/rocm-ernic --socket ${VFIO_USER_SOCKET}"
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
echo "  QMP socket:     ${QMP_SOCKET}"
echo ""
echo "Hot-reload:"
echo "  scripts/hot-reload.sh"
echo ""
echo "To connect:"
echo "  ssh -p ${SSH_PORT} ubuntu@localhost"
echo ""
echo "Press Ctrl-A then X to exit QEMU"
echo ""

# vfio-user-pci requires QEMU 10.1+ with vfio-user client support.
#
# A PCIe root port is used so the device can be hot-unplugged and
# re-added at runtime via QMP (see scripts/hot-reload.sh).

exec ${QEMU_PATH}qemu-system-${QARCH} \
   ${QARCH_ARGS} \
   -smp cpus=${VCPUS} \
   -object memory-backend-memfd,id=mem0,share=on,size=${VMEM}M \
   -machine memory-backend=mem0 \
   ${FILESYSTEM_ARGS} \
   -nographic \
   -qmp unix:${QMP_SOCKET},server,nowait \
   -drive if=virtio,format=qcow2,file=${IMAGES}/${VM_NAME}.qcow2 \
   -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
   -device virtio-net-pci,netdev=net0 \
   -device pcie-root-port,id=rp0,slot=0,chassis=0 \
   -device '{"driver":"vfio-user-pci","id":"ernic0","bus":"rp0","socket":{"path":"'"${VFIO_USER_SOCKET}"'","type":"unix"}}'

