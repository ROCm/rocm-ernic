# syntax=docker/dockerfile:1
#
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# The CI builder image: what the VM lanes run in. It has QEMU with the
# vfio-user client, libvfio-user to build the server against, the tools
# that boot and provision a guest (cloud-localds, ssh, ansible-core), and
# oras and zstd to fetch one. It holds no guest.
#
# Build it with ci/images/ensure-image.sh, which passes every pin in
# pins.env as a build argument; none has a default here, so a bare
# `docker build` fails rather than build something unpinned.
#
# QEMU and libvfio-user each build in their own stage and are copied into
# the final one, so neither's source tree ships, and changing one pin
# doesn't rebuild the other. QEMU's vfio-user client is its own; QEMU
# doesn't link libvfio-user.

ARG UBUNTU

# ── base: what every stage and the final image share ─────────────────

FROM ubuntu:${UBUNTU} AS base

ENV DEBIAN_FRONTEND=noninteractive

# Changing it in pins.env reruns every step from here, so a rebuild picks
# up Ubuntu's updates rather than reusing cached layers
ARG REBUILD
RUN : "${REBUILD:?pass the pins in pins.env as build arguments}"

RUN printf '%s\n' 'Acquire::Retries "10";' \
        > /etc/apt/apt.conf.d/99retries

# Only for a local build behind a proxy that re-signs TLS, such as the
# AMD network's: ensure-image.sh passes CI_IMAGES_ROOT_CA as a secret,
#   --secret id=root-ca,src=/path/to/root-ca.crt
# and it's trusted from here on. CI passes none, and ensure-image.sh
# won't publish an image built with one.
#
# A secret isn't part of the build cache's key, so ROOT_CA_SHA256, the
# certificate's SHA-256 or "none", is: a build with a certificate and one
# without never share this layer, or any after it. Otherwise a build
# could reuse a layer that trusts a certificate it wasn't given.
ARG ROOT_CA_SHA256
RUN --mount=type=secret,id=root-ca,target=/run/secrets/root-ca \
    if [ "${ROOT_CA_SHA256:?pass none, or the root CA SHA-256}" = none ]; then \
        if [ -s /run/secrets/root-ca ]; then \
            echo "a root CA was passed with ROOT_CA_SHA256=none" >&2; \
            exit 1; \
        fi; \
    else \
        printf '%s  %s\n' "${ROOT_CA_SHA256}" /run/secrets/root-ca | \
            sha256sum -c - && \
        apt-get update && \
        apt-get install -y --no-install-recommends ca-certificates && \
        cp /run/secrets/root-ca \
            /usr/local/share/ca-certificates/build-root-ca.crt && \
        update-ca-certificates && \
        rm -rf /var/lib/apt/lists/*; \
    fi

# The toolchain the server builds with, and libvfio-user's build
# dependencies, which are also the server's
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        libcmocka-dev \
        libglib2.0-dev \
        libjson-c-dev \
        meson \
        ninja-build \
        pkg-config \
        python3 \
        python3-venv && \
    rm -rf /var/lib/apt/lists/*

# QEMU's build dependencies beyond the base's. The final image installs
# them too, so every library QEMU linked against is there at run time.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libpixman-1-dev \
        libslirp-dev && \
    rm -rf /var/lib/apt/lists/*

# ── qemu: QEMU, installed under /opt/qemu ────────────────────────────

FROM base AS qemu

ARG QEMU_REPO
ARG QEMU_COMMIT
RUN : "${QEMU_REPO:?}" "${QEMU_COMMIT:?}"

# A tag is cloned shallow; a commit SHA needs the history
RUN if printf '%s\n' "${QEMU_COMMIT}" | grep -qE '^[0-9a-f]{40}$'; then \
        git clone --filter=blob:none "${QEMU_REPO}" /build/qemu && \
        git -C /build/qemu checkout "${QEMU_COMMIT}"; \
    else \
        git clone --depth 1 --branch "${QEMU_COMMIT}" "${QEMU_REPO}" \
            /build/qemu; \
    fi

WORKDIR /build/qemu
RUN ./configure --target-list=x86_64-softmmu --enable-slirp \
        --prefix=/opt/qemu && \
    make -j"$(nproc)" && \
    make install && \
    git rev-parse HEAD > /opt/qemu/share/qemu-commit.txt

# ── libvfio-user: the library, staged under /stage/usr/local ─────────

FROM base AS libvfio-user

ARG LIBVFIO_USER_REPO
ARG LIBVFIO_USER_COMMIT
RUN : "${LIBVFIO_USER_REPO:?}" "${LIBVFIO_USER_COMMIT:?}"

# Only the library, as the other CI builds do: its tests aren't needed,
# and don't build with Ubuntu 26.04's cmocka 2. /usr/local, because the
# VM lanes look for it there.
WORKDIR /build/libvfio-user
RUN git clone --filter=blob:none "${LIBVFIO_USER_REPO}" . && \
    git checkout "${LIBVFIO_USER_COMMIT}" && \
    meson setup build --prefix=/usr/local -Dwarning_level=0 && \
    meson compile -C build vfio-user && \
    DESTDIR=/stage meson install -C build --no-rebuild \
        --tags runtime,devel && \
    mkdir -p /stage/usr/local/share && \
    git rev-parse HEAD > /stage/usr/local/share/libvfio-user-commit.txt

# ── final ────────────────────────────────────────────────────────────

FROM base

# The tools the lanes use around the guest:
#   cloud-image-utils   cloud-localds, for ci/boot-guest.sh's seed
#   file                the TCP job's check of the built server
#   openssh-client      ssh, scp and ssh-keygen
#   iproute2            ip and ss
#   jq, zstd            fetching the guest
#   qemu-utils          Ubuntu's qemu-img, as the Docker Hub builder had;
#                       /opt/qemu/bin's comes first on PATH
#   pipx                ansible-core, below
#   procps              ps, pgrep and pkill, which the lanes use to watch
#                       and stop the server and QEMU
#   sudo                Ansible's become, for the TCP job's host setup,
#                       which runs here as root
# and the RDMA headers the server builds against
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        cloud-image-utils \
        file \
        iproute2 \
        jq \
        libibverbs-dev \
        librdmacm-dev \
        openssh-client \
        pipx \
        procps \
        qemu-utils \
        sudo \
        zstd && \
    rm -rf /var/lib/apt/lists/*

ARG ORAS_VERSION
ARG ORAS_SHA256
RUN : "${ORAS_VERSION:?}" "${ORAS_SHA256:?}" && \
    curl -fsSL -o /tmp/oras.tar.gz \
        "https://github.com/oras-project/oras/releases/download/v${ORAS_VERSION}/oras_${ORAS_VERSION}_linux_amd64.tar.gz" && \
    printf '%s  %s\n' "${ORAS_SHA256}" /tmp/oras.tar.gz | sha256sum -c - && \
    tar -xzf /tmp/oras.tar.gz -C /usr/local/bin oras && \
    rm -f /tmp/oras.tar.gz

# pipx puts the tools it installs on PATH, in /usr/local/bin
ENV PIPX_HOME=/opt/pipx \
    PIPX_BIN_DIR=/usr/local/bin \
    PIPX_MAN_DIR=/usr/local/share/man
ARG ANSIBLE_CORE_VERSION
RUN : "${ANSIBLE_CORE_VERSION:?}" && \
    pipx install "ansible-core==${ANSIBLE_CORE_VERSION}"

COPY --from=qemu /opt/qemu /opt/qemu
COPY --from=libvfio-user /stage/usr/local /usr/local
RUN ldconfig

ENV PATH=/opt/qemu/bin:${PATH}

LABEL org.opencontainers.image.source="https://github.com/ROCm/rocm-ernic" \
      org.opencontainers.image.description="rocm-ernic CI builder: QEMU with vfio-user, libvfio-user, and the tools to boot and provision a guest" \
      org.opencontainers.image.licenses="MIT"

# Run unprivileged unless asked otherwise. The VM lanes do need root, for
# /dev/kvm, the TAP devices and apt, so their container options ask for it
# with --user root.
RUN useradd --create-home --uid 1001 --shell /bin/bash ci
USER ci
WORKDIR /home/ci

# Nothing here runs as a service, so there's nothing to check
HEALTHCHECK NONE

CMD ["/bin/bash"]
