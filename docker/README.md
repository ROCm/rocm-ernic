# ROCm ERNIC CI Docker Image

This directory contains the Dockerfile for building a CI testing image that extends
the base VM image (`docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest`).

## Base Image Contains

The base image (`docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest`)
provides:
- **QEMU** with vfio-user-pci support
- **libvfio-user** library
- **VM disk image** (qcow2 format) for testing
- All build dependencies (meson, ninja, gcc, etc.)
- RDMA/InfiniBand libraries (libibverbs, librdmacm)
- Testing tools (sshpass, cloud-image-utils, etc.)

## This Image Adds

- Project-specific build dependencies (`libjson-c-dev`, `libcmocka-dev`)
- Linting tools (clang-format, cppcheck, shellcheck)
- VM disk manipulation tools (`libguestfs-tools`)

## Building the Image

The Dockerfile extends the base VM image, so building is fast:

```bash
cd docker
docker build -t rocm-ernic-ci:latest .
```

Or with a specific tag:

```bash
docker build -t rocm-ernic-ci:v1.0.0 .
```

**Note**: The base image (`docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest`)
will be automatically pulled from Docker Hub during the build.

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

# The VM disk image is available in the container at:
# `/vm-disk.img` (or `/vm/vm-disk.img`, `/root/vm-disk.img`, `/opt/vm/vm-disk.img`)
```

### Testing with VM

You can use the image to run loopback backend tests locally:

```bash
# Build the image
docker build -t rocm-ernic-ci:latest ./docker

# Run tests (mount project directory)
docker run -it --rm \
    --privileged \
    -v $(pwd):/workspace \
    rocm-ernic-ci:latest \
    bash -c "cd /workspace && ./tests/test_loopback_with_vm.sh"
```

### CI Usage

The image should be published to a container registry (e.g., Docker Hub, GitHub Container Registry) and
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

All base image contents are inherited from `docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest`:

### QEMU Installation
- **Binary**: `qemu-system-x86_64` (in PATH)
- **Version**: From base image (with vfio-user-pci support)

### libvfio-user Installation
- **Location**: `/usr` (system-wide)
- **Libraries**: `/usr/lib/libvfio-user.so` or `/usr/lib/x86_64-linux-gnu/libvfio-user.so`

### VM Disk Image
- **Location**: One of:
  - `/vm-disk.img` (filename: `vm-disk.img`)
  - `/vm/vm-disk.img`
  - `/root/vm-disk.img`
  - `/opt/vm/vm-disk.img`

### Build Tools (from base image)
- meson, ninja-build
- gcc, g++, make, cmake
- pkg-config

### RDMA Libraries (from base image)
- libibverbs-dev
- librdmacm-dev
- rdma-core
- ibverbs-utils

### Testing Tools (from base image)
- cloud-image-utils
- openssh-client
- sshpass
- qemu-utils

### Project-Specific Additions
- **Build dependencies**: `libjson-c-dev`, `libcmocka-dev`
- **Linting tools**: clang-format, cppcheck, shellcheck
- **VM tools**: `libguestfs-tools` (for mounting VM disks)

## Publishing to Registry

The Docker image is automatically built and published via GitHub Actions when:
- Changes are pushed to `main` branch
- Tags matching `v*` are pushed
- Manual workflow dispatch

### Manual Publishing

#### GitHub Container Registry (GHCR) - Recommended

```bash
# Login to GitHub Container Registry
# aspell: ignore next
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

### Updating Project Dependencies

To add or update project-specific dependencies:

1. Modify the Dockerfile (add packages to the `apt-get install` command)
2. Rebuild the image
3. Push new version to registry
4. Update CI workflows to use new tag

### Updating Base Image

To use a newer version of the base VM image:

1. Update the `FROM` line in Dockerfile with new tag/version
2. Rebuild the image
3. Push new version to registry

**Note**: QEMU, libvfio-user, and VM disk image updates come from the base image.
To update those, you'll need to rebuild the base image at:
`docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user`

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

