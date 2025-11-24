# Dockerfile for ROCm ERNIC CI Testing
# Extends the base VM image with project-specific dependencies
#
# Base image contains:
# - QEMU with vfio-user-pci support
# - libvfio-user
# - VM disk image
# - Build tools (meson, ninja, gcc, etc.)
# - RDMA libraries (libibverbs, librdmacm)
# - SSH tools (sshpass, etc.)
#
# Build and publish:
#   docker build -t ghcr.io/rocm/rocm-ernic-ci:latest .
#   docker push ghcr.io/rocm/rocm-ernic-ci:latest

FROM docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest

LABEL org.opencontainers.image.title="ROCm ERNIC CI Image"
LABEL org.opencontainers.image.description="CI image extending base VM image with project-specific dependencies"
LABEL org.opencontainers.image.vendor="ROCm"
LABEL org.opencontainers.image.version="latest"

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install project-specific dependencies
RUN apt-get update && apt-get install -y \
    # Project build dependencies
    libjson-c-dev \
    libcmocka-dev \
    # Linting tools
    clang-format \
    cppcheck \
    shellcheck \
    # Other utilities
    libguestfs-tools \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Verify base image tools are available
RUN echo "=== Verifying Base Image ===" && \
    qemu-system-x86_64 --version && \
    echo "" && \
    echo "=== QEMU vfio-user-pci support ===" && \
    qemu-system-x86_64 -device help 2>&1 | grep vfio-user-pci && \
    echo "" && \
    echo "=== Build tools ===" && \
    meson --version && \
    ninja --version && \
    echo "" && \
    echo "=== RDMA libraries ===" && \
    pkg-config --exists libibverbs && pkg-config --modversion libibverbs && \
    echo "" && \
    echo "=== VM Disk Image ===" && \
    (test -f /vm-disk.img && echo "Found: /vm-disk.img" || \
     test -f /vm/vm-disk.img && echo "Found: /vm/vm-disk.img" || \
     test -f /root/vm-disk.img && echo "Found: /root/vm-disk.img" || \
     test -f /opt/vm/vm-disk.img && echo "Found: /opt/vm/vm-disk.img" || \
     echo "⚠️  VM disk image not found in standard locations") && \
    echo "" && \
    echo "=== Project Dependencies ===" && \
    pkg-config --exists json-c && pkg-config --modversion json-c && \
    pkg-config --exists cmocka && pkg-config --modversion cmocka && \
    echo "" && \
    echo "All dependencies installed successfully!"

# Default command
CMD ["/bin/bash"]
