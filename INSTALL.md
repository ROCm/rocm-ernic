# Install guide for rocm-ernic

## Quick Install Guide

rocm-ernic requires libvfio-user and several RDMA/networking development
libraries. On Ubuntu 24.04, install the required packages:

```
sudo apt install cmake meson ninja-build pkg-config \
  libibverbs-dev librdmacm-dev libglib2.0-dev
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

## Building rocm-ernic

> [!NOTE]
> rocm-ernic is early-access software that has undergone testing on limited
> hardware. It may not work on your system at this time.

Supported compilers: gcc, clang

Supported platforms: Linux (tested on Ubuntu 24.04)

### Requirements

* CMake 3.16 or later
* Meson and Ninja (for libvfio-user)
* pkg-config
* libvfio-user (built from source; see above)
* libibverbs and librdmacm development packages
* GLib 2.0 development package

### Configure

| Option | Default | Purpose |
|--------|---------|---------|
| CMAKE\_BUILD\_TYPE | Debug | Build type (Debug, Release, etc.) |
| ERNIC\_USE\_SANITIZERS | OFF | Enable ASAN / LSAN / UBSAN |
| ERNIC\_USE\_THREAD\_SANITIZER | OFF | Enable TSAN (incompatible with above) |
| ERNIC\_BUILD\_DOCS | OFF | Build Sphinx + Breathe + Doxygen documentation |
| ERNIC\_DOCS\_ONLY | OFF | Docs-only build (no library dependencies required) |
| CMAKE\_INSTALL\_PREFIX | /usr/local | Installation prefix |

### Build

```
cmake --build build
```

### Run tests

```
ctest --test-dir build
```

### Install

The default install prefix is `/usr/local`.

```
sudo cmake --install build
```

Custom install prefix:

```
cmake --install build --prefix /tmp/rocm-ernic-test
```

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
