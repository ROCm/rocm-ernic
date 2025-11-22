# Dockerfile for ROCm ERNIC CI Testing
# Builds QEMU (latest) and libvfio-user, plus all dependencies needed for testing
#
# Build and publish:
#   docker build -t ghcr.io/rocm/rocm-ernic-ci:latest .
#   docker push ghcr.io/rocm/rocm-ernic-ci:latest

FROM ubuntu:24.04

LABEL org.opencontainers.image.title="ROCm ERNIC CI Image"
LABEL org.opencontainers.image.description="CI image with QEMU, libvfio-user, and RDMA testing tools"
LABEL org.opencontainers.image.vendor="ROCm"
LABEL org.opencontainers.image.version="latest"

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install all build dependencies and runtime tools
RUN apt-get update && apt-get install -y \
    # Build tools
    build-essential \
    gcc \
    g++ \
    make \
    cmake \
    meson \
    ninja-build \
    pkg-config \
    git \
    wget \
    curl \
    tar \
    xz-utils \
    # QEMU build dependencies
    libglib2.0-dev \
    libpixman-1-dev \
    libslirp-dev \
    libcap-ng-dev \
    libattr1-dev \
    libfdt-dev \
    libaio-dev \
    libnuma-dev \
    # RDMA/InfiniBand libraries
    libibverbs-dev \
    librdmacm-dev \
    rdma-core \
    ibverbs-utils \
    # Project build dependencies
    libjson-c-dev \
    libcmocka-dev \
    # Testing and CI tools
    cloud-image-utils \
    openssh-client \
    sshpass \
    qemu-utils \
    # Kernel headers (for driver builds)
    linux-headers-generic \
    # Linting tools
    clang-format \
    cppcheck \
    shellcheck \
    # Python (for some build scripts)
    python3 \
    python3-pip \
    # Other utilities
    sudo \
    && rm -rf /var/lib/apt/lists/*

# Set up QEMU installation directory
ENV QEMU_PREFIX=/opt/qemu
ENV PATH="${QEMU_PREFIX}/bin:${PATH}"
ENV LD_LIBRARY_PATH="${QEMU_PREFIX}/lib:${LD_LIBRARY_PATH}"
# Ensure pkg-config can find libvfio-user
ENV PKG_CONFIG_PATH="/usr/lib/pkgconfig:/usr/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

# Build QEMU from latest git source (x86_64 only)
WORKDIR /tmp
RUN git clone --depth 1 https://gitlab.com/qemu-project/qemu.git qemu-build && \
    cd qemu-build && \
    ./configure \
        --prefix=${QEMU_PREFIX} \
        --target-list=x86_64-softmmu \
        --enable-slirp \
        --enable-kvm \
        --disable-docs \
        --disable-gtk \
        --disable-sdl \
        --disable-vnc \
        --disable-curses \
        --disable-brlapi \
        --disable-linux-user \
        --disable-bsd-user \
        --disable-guest-agent \
        --disable-guest-agent-msi \
        --disable-tools \
        --extra-cflags="-w -O2" && \
    make -j$(nproc) && \
    make install && \
    cd / && \
    rm -rf /tmp/qemu-build && \
    # Verify QEMU installation
    ${QEMU_PREFIX}/bin/qemu-system-x86_64 --version && \
    # Verify vfio-user-pci support
    ${QEMU_PREFIX}/bin/qemu-system-x86_64 -device help 2>&1 | grep -q vfio-user-pci || \
        (echo "ERROR: QEMU missing vfio-user-pci support" && exit 1)

# Build and install libvfio-user
WORKDIR /tmp
RUN git clone --depth 1 https://github.com/nutanix/libvfio-user.git libvfio-user && \
    cd libvfio-user && \
    meson setup build --prefix=/usr -Dwarning_level=0 && \
    ninja -C build && \
    sudo ninja -C build install && \
    sudo ldconfig && \
    cd / && \
    rm -rf /tmp/libvfio-user && \
    # Verify libvfio-user installation (library exists, pkg-config is optional)
    if [ -f /usr/lib/libvfio-user.so ] || [ -f /usr/lib/x86_64-linux-gnu/libvfio-user.so ]; then \
        echo "✓ libvfio-user library installed"; \
    else \
        echo "ERROR: libvfio-user library not found"; \
        find /usr -name "libvfio-user.so*" 2>/dev/null || echo "No libvfio-user.so found"; \
        exit 1; \
    fi && \
    # Check for pkg-config file (optional - meson might not generate it)
    if pkg-config --exists vfio-user 2>/dev/null; then \
        echo "✓ libvfio-user pkg-config found: $(pkg-config --modversion vfio-user)"; \
    else \
        echo "⚠️  libvfio-user pkg-config not found (library installed, will use manual linking)"; \
        echo "PKG_CONFIG_PATH: ${PKG_CONFIG_PATH}"; \
        find /usr -name "vfio-user.pc" 2>/dev/null || echo "No vfio-user.pc found"; \
    fi

# Set working directory
WORKDIR /workspace

# Create a non-root user for testing (some tests may need this)
RUN useradd -m -s /bin/bash ci-user && \
    echo "ci-user ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

# Verify installations (as root before switching to ci-user)
RUN echo "=== QEMU Version ===" && \
    ${QEMU_PREFIX}/bin/qemu-system-x86_64 --version && \
    echo "" && \
    echo "=== QEMU vfio-user-pci support ===" && \
    ${QEMU_PREFIX}/bin/qemu-system-x86_64 -device help 2>&1 | grep vfio-user-pci && \
    echo "" && \
    echo "=== libvfio-user ===" && \
    (pkg-config --exists vfio-user 2>/dev/null && echo "pkg-config: $(pkg-config --modversion vfio-user)" || echo "pkg-config: not found (library exists, using manual linking)") && \
    (ls -lh /usr/lib/libvfio-user.so* /usr/lib/x86_64-linux-gnu/libvfio-user.so* 2>/dev/null | head -1 || echo "Library check: OK") && \
    echo "" && \
    echo "=== Build tools ===" && \
    meson --version && \
    ninja --version && \
    echo "" && \
    echo "=== RDMA libraries ===" && \
    pkg-config --exists libibverbs && pkg-config --modversion libibverbs && \
    echo "" && \
    echo "All dependencies installed successfully!"

# Set default user (can be overridden)
USER ci-user

# Default command
CMD ["/bin/bash"]

