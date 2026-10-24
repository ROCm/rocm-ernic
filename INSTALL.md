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

That presents the emulated ionic device (`1dd8:1002`, subsystem
`1dd8:5400`) to any client that
connects to the socket.

## Nightly packages

Every night that `develop` changes, DEB packages of it for Ubuntu 24.04 and
26.04 are published as the
[`nightly` prerelease](https://github.com/ROCm/rocm-ernic/releases/tag/nightly).
They're development builds for testing, not supported releases. Download the
packages for your Ubuntu release (the file names end with it) and install
them:

```
sudo apt install ./rocm-ernic_<version>_amd64.deb ./rocm-ernic-exporter_<version>_all.deb
sudo systemctl daemon-reload
```

`rocm-ernic` has the server, with its own copy of libvfio-user, the systemd
service, `ernicctl`, and the guest driver pack, installed under `/usr`. Its
configuration is in `/etc/rocm-ernic/rocm-ernic.env` (see
[docs/service.rst](docs/service.rst)). `rocm-ernic-exporter` has the
Prometheus exporter, which also needs `prometheus_client` (see
[docs/monitoring.rst](docs/monitoring.rst)).

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
| CMAKE\_BUILD\_TYPE | Debug | Build type (Debug, Release, etc.); a parent project's when rocm-ernic is a subproject |
| BUILD\_TESTING | ON | Build the tests and register them with CTest |
| ERNIC\_WERROR | OFF | Treat compiler warnings as errors for project code (CI sets this ON) |
| ERNIC\_USE\_SANITIZERS | OFF | Build with ASan, LSan, UBSan, and the compiler's other checks that can be combined with them |
| ERNIC\_USE\_THREAD\_SANITIZER | OFF | Build with TSan (incompatible with above) |
| ERNIC\_SANITIZER\_SHARED\_RUNTIME | ON (GCC), OFF (Clang) | Link the shared sanitizer runtime instead of the static one |
| ERNIC\_BUILD\_DOCS | OFF | Build Sphinx + Breathe + Doxygen documentation |
| ERNIC\_DOCS\_ONLY | OFF | Docs-only build (no library dependencies required) |
| ERNIC\_BUILD\_KMOD | OFF | DKMS targets for the patched upstream ionic guest modules |
| IONIC\_KERNEL\_REF | v7.2.4 | Kernel tag/SHA the ionic sources are fetched from (>= v6.18) |
| IONIC\_KERNEL\_REPO | kernel.org stable | Linux kernel git repository the ionic sources are fetched from |
| ERNIC\_INSTALL\_SERVICE | OFF | Also install the systemd units, `ernicctl`, launcher, and Prometheus exporter |
| ERNIC\_BUNDLE\_VFIO\_USER | OFF | Install a private copy of libvfio-user that only the installed server uses, for packages |
| ERNIC\_VFIO\_USER\_LICENSE | (none) | libvfio-user's `LICENSE` file, installed with the private copy; required by `ERNIC_BUNDLE_VFIO_USER` |
| ERNIC\_PACKAGE\_RELEASE | local | The release part of the DEB packages' version |
| CMAKE\_INSTALL\_PREFIX | /usr/local | Installation prefix |

### Guest ionic modules

The guest runs the upstream Linux `ionic` and `ionic_rdma` drivers
with the patches in `ansible/files/patches/` applied, against a kernel >= 6.18.
Configure
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
`rocm-ernic` server in `<prefix>/bin` and its licenses in
`<prefix>/share/doc/rocm-ernic`.

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
full list of installed files. These files name each other's installed paths,
taken from `CMAKE_INSTALL_PREFIX` when the project is configured, so configure
with the prefix they'll be installed to rather than using `--prefix` here.

To make DEB packages, as the nightly build does, see
[Packages](docs/building.rst#packages) in docs/building.rst.

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
