# Install guide for rocm-ernic

## Quick Install Guide

rocm-ernic requires libvfio-user and several RDMA/networking development
libraries. On Ubuntu 24.04, install the required packages:

```
sudo apt install cmake meson ninja-build pkg-config \
  libibverbs-dev librdmacm-dev libglib2.0-dev libjson-c-dev
```

Build and install libvfio-user if it is not already available on your system:

```
git clone https://github.com/nutanix/libvfio-user.git
cd libvfio-user
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Then build rocm-ernic:

```
git clone https://github.com/ROCm/rocm-ernic.git
cd rocm-ernic
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Verify the build by starting the server with the loopback backend:

```
./build/rocm-ernic \
  --socket /tmp/vfio-user-rocm-ernic.sock \
  --backend loopback --verbose
```

That presents the emulated ionic device (`1dd8:100a`) to any client that
connects to the socket.

## Building rocm-ernic

> [!NOTE]
> rocm-ernic is early-access software that has undergone testing on limited
> hardware. It may not work on your system at this time.

Supported compilers: gcc, clang

Supported platforms: Linux (tested on Ubuntu 24.04)

### Requirements

* CMake 3.21 or later
* Meson and Ninja (for libvfio-user)
* pkg-config
* libvfio-user (built from source; see above)
* libibverbs and librdmacm development packages
* GLib 2.0 development package
* json-c development package (required by `libvfio-user.pc`)

If libvfio-user is installed somewhere other than `/usr` or `/usr/local`, add
its `pkgconfig` directory to `PKG_CONFIG_PATH` or point CMake at the prefix:

```
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$HOME/.local"
```

### Configure

| Option | Default | Purpose |
|--------|---------|---------|
| CMAKE\_BUILD\_TYPE | Debug | Build type (Debug, Release, etc.) |
| ERNIC\_WERROR | OFF | Treat compiler warnings as errors for project code (CI sets this ON) |
| ERNIC\_USE\_SANITIZERS | OFF | Enable ASAN / LSAN / UBSAN |
| ERNIC\_USE\_THREAD\_SANITIZER | OFF | Enable TSAN (incompatible with above) |
| ERNIC\_BUILD\_DOCS | OFF | Build Sphinx + Breathe + Doxygen documentation |
| ERNIC\_DOCS\_ONLY | OFF | Docs-only build (no library dependencies required) |
| ERNIC\_BUILD\_KMOD | OFF | DKMS targets for the patched upstream ionic guest modules |
| IONIC\_KERNEL\_REF | v7.2.4 | Kernel tag/SHA the ionic sources are fetched from (>= v6.18) |
| IONIC\_KERNEL\_REPO | kernel.org stable | Linux kernel git repository the ionic sources are fetched from |
| ERNIC\_INSTALL\_SERVICE | OFF | Also install the systemd units, `ernicctl`, launcher, and Prometheus exporter |
| CMAKE\_INSTALL\_PREFIX | /usr/local | Installation prefix |

### Guest ionic modules

The guest runs the upstream Linux `ionic` and `ionic_rdma` drivers
with the patches in `patches/` applied, against a kernel >= 6.18. Configure
with `-DERNIC_BUILD_KMOD=ON` and run these targets inside the guest:

```
cmake --build build --target fetch-ionic-sources
cmake --build build --target build-ionic-dkms
sudo cmake --build build --target install-ionic-dkms
```

The `sbates130272.rocm_ernic` Ansible collection does all of this for you;
see [ansible/README.md](ansible/README.md).

See [docs/ionic.rst](docs/ionic.rst) for details.

### Build

```
cmake --build build
```

### Run tests

```
ctest --test-dir build
```

### Install

The default install prefix is `/usr/local`. A default install places only the
`rocm-ernic` server in `<prefix>/bin`.

```
sudo cmake --install build
```

Custom install prefix:

```
cmake --install build --prefix /tmp/rocm-ernic-test
```

To also install the systemd units, `ernicctl`, the launcher, the Prometheus
exporter, and the ionic driver patches, configure with
`-DERNIC_INSTALL_SERVICE=ON`. See [docs/service.rst](docs/service.rst) for the
full list of installed files.

### Documentation

Build the documentation with Sphinx, Breathe, and Doxygen. A Python virtual
environment is created automatically in the build tree.

```
cmake -B build -G Ninja -DERNIC_BUILD_DOCS=ON
cmake --build build --target sphinx-html
```

Output appears in `build/docs/html/index.html`.

To build documentation without needing the project's library dependencies
(libvfio-user, GLib, libibverbs):

```
cmake -B build -DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON
cmake --build build --target sphinx-html
```

For full build details see [docs/building.rst](docs/building.rst).
