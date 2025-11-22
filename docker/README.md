# ROCm ERNIC CI Docker Image

This directory contains the Dockerfile for building a CI testing image that includes:

- **QEMU** (latest from git) with vfio-user-pci support
- **libvfio-user** (latest from git)
- All build dependencies (meson, ninja, gcc, etc.)
- RDMA/InfiniBand libraries (libibverbs, librdmacm)
- Testing tools (sshpass, cloud-image-utils, etc.)
- Linting tools (clang-format, cppcheck, shellcheck)

## Building the Image

```bash
cd docker
docker build -t rocm-ernic-ci:latest .
```

Or with a specific tag:

```bash
docker build -t rocm-ernic-ci:v1.0.0 .
```

## Using the Image

### Local Testing

```bash
# Run interactive shell
docker run -it --rm \
    -v $(pwd):/workspace \
    --privileged \
    rocm-ernic-ci:latest

# Inside container:
cd /workspace
meson setup build
ninja -C build
```

### CI Usage

The image should be published to a container registry (e.g., Docker Hub, GHCR) and
referenced in GitHub Actions workflows:

```yaml
jobs:
  test:
    runs-on: ubuntu-latest
    container:
      image: your-registry/rocm-ernic-ci:latest
    steps:
      - uses: actions/checkout@v4
      - run: meson setup build
      - run: ninja -C build
```

## Image Contents

### QEMU Installation
- **Location**: `/opt/qemu`
- **Binary**: `/opt/qemu/bin/qemu-system-x86_64`
- **Version**: Latest from git (with vfio-user-pci support)
- **PATH**: Automatically added to PATH

### libvfio-user Installation
- **Location**: `/usr` (system-wide)
- **pkg-config**: Available via `pkg-config vfio-user`
- **Libraries**: `/usr/lib/libvfio-user.so`

### Build Tools
- meson, ninja-build
- gcc, g++, make, cmake
- pkg-config

### RDMA Libraries
- libibverbs-dev
- librdmacm-dev
- rdma-core
- ibverbs-utils

### Testing Tools
- cloud-image-utils
- openssh-client
- sshpass
- qemu-utils

### Linting Tools
- clang-format
- cppcheck
- shellcheck

## Publishing to Registry

The Docker image is automatically built and published via GitHub Actions when:
- Changes are pushed to `main` branch
- Tags matching `v*` are pushed
- Manual workflow dispatch

### Manual Publishing

#### GitHub Container Registry (GHCR) - Recommended

```bash
# Login to GHCR
echo $GITHUB_TOKEN | docker login ghcr.io -u USERNAME --password-stdin

# Build and tag
docker build -t ghcr.io/rocm/rocm-ernic-ci:latest ./docker

# Push
docker push ghcr.io/rocm/rocm-ernic-ci:latest
```

#### Docker Hub

```bash
docker tag rocm-ernic-ci:latest docker.io/yourusername/rocm-ernic-ci:latest
docker push docker.io/yourusername/rocm-ernic-ci:latest
```

### Version Tags

It's recommended to tag images with version numbers:

```bash
docker tag rocm-ernic-ci:latest ghcr.io/rocm/rocm-ernic-ci:v1.0.0
docker tag rocm-ernic-ci:latest ghcr.io/rocm/rocm-ernic-ci:latest
docker push ghcr.io/rocm/rocm-ernic-ci:v1.0.0
docker push ghcr.io/rocm/rocm-ernic-ci:latest
```

### Public Registry Usage

The image is available at:
- `ghcr.io/rocm/rocm-ernic-ci:latest` (default)
- `ghcr.io/rocm/rocm-ernic-ci:v1.0.0` (versioned tags)

CI workflows automatically use the public registry image. No authentication needed for public images.

## Image Size

The image is relatively large (~2-3GB) due to:
- QEMU build artifacts
- Build dependencies
- Multiple toolchains

Consider using multi-stage builds or image optimization if size is a concern.

## Updating the Image

To update QEMU or libvfio-user to newer versions:

1. Modify the Dockerfile (git clone commands will get latest)
2. Rebuild the image
3. Push new version to registry
4. Update CI workflows to use new tag

## Troubleshooting

### QEMU vfio-user-pci not found

If QEMU doesn't have vfio-user-pci support, check:
- QEMU version (needs 7.0+)
- Configure flags (--enable-vfio-user)
- Build logs for errors

### libvfio-user not found

If pkg-config can't find libvfio-user:
- Check installation path
- Run `ldconfig` after installation
- Verify PKG_CONFIG_PATH

### Permission Issues

The image creates a `ci-user` user. For privileged operations (KVM, etc.),
use `--privileged` flag or add specific capabilities.

